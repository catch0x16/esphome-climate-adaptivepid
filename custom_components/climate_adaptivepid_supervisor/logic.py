"""Cross-head decisions for climate_adaptivepid heads on one outdoor unit.

Pure Python with no Home Assistant imports, so it can be tested on its own
(tests/supervisor/test_logic.py). See docs/supervisor-design.md and
docs/start-control-design.md.

Heads on a shared outdoor unit leak heat (cooling) into each other: while any
head runs, an idle head that is still powered keeps receiving refrigerant and
its fan carries that into its room. A head can only infer that from its own
room; the supervisor sees every head's `operating` and knows for certain.

For each room that receives hints, shared heat is on while any head that ranks
above it is operating; the room's own run never counts. The head decides what
to do with it: it starts no run of its own while shared heat is on, and parks
(switches off) at once once its room is past its hysteresis off_threshold.

Shared heat can also be forecast. A head that is not under start control
starts at a fairly steady deficit below its target (the primary bedroom:
0.22-0.37 C, median 0.32), so its next start is predicted from its room
temperature and how fast it is falling. A hinted room is told when another
head is expected to start, so it can wait for that heat instead of starting a
run that stacks on top of it. Runs also delay each other: a bedroom's run
warms the other bedroom and pushes its start back by several minutes.

Heads have a precedence: the order they are configured in, highest first (in
this house the main floor, which owns the heating, then the primary bedroom,
then Hailee's room). A room hears only from the heads above it, so it never waits for,
defers to or parks for a head below it; that head yields to it instead.
Without this, two hinted rooms would hold each other back. The leak runs
mostly downhill anyway: the main floor's runs warm the primary bedroom by
+0.04 C/min, Hailee's runs warm it by +0.01.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
import math
import statistics
from typing import Any


@dataclass(frozen=True)
class Head:
    """What the supervisor knows about one head."""

    name: str
    # None: not known (the head's Operating sensor is unavailable).
    operating: bool | None = None
    # "heat", "cool" or anything else (no hints outside heat and cool).
    mode: str | None = None
    # The room temperature the head regulates, and its target.
    room: float | None = None
    target: float | None = None
    # False while the head's device is switched off in heat or cool (parked by
    # hysteresis or start control: hvac_action "off"). A parked head starts only
    # once it is switched back on, so no start is forecast for it. None: not known.
    powered: bool | None = None


@dataclass(frozen=True)
class RoomPlan:
    """What to tell one hinted room's head."""

    shared_heat: bool
    reason: str
    # Seconds until another head is predicted to start; None when none is due
    # within the forecast window (or one is already running).
    expected_in_s: int | None = None


# How many recent starts a head's start deficit is learned from, and how many
# it needs before it is predicted at all.
START_HISTORY = 8
MIN_STARTS = 4
# The fall rate is measured over this, from samples at least this far apart.
SLOPE_WINDOW_S = 300.0
# No forecast in the first minutes after a stop (the room is still coasting).
AFTER_STOP_S = 180.0
# A fall slower than this (C/s, ~0.006 C/min) gives no forecast.
MIN_FALL_PER_S = 0.0001
# Past the start level by more than this, the forecast is dropped as wrong.
OVERDUE_MARGIN = 0.15
# A forecast that has been due (0 s) this long without a start is dropped as
# wrong: a room holding flat at its start level would otherwise be "starting
# now" for as long as it stays there, and hold the rooms below it back.
OVERDUE_HOLD_S = 300.0
# Forecasts further out than this are not sent.
EXPECT_WINDOW_S = 300
# A start this soon after a target or mode change was commanded, not reached
# by drifting, so its deficit is not learned.
AFTER_CHANGE_S = 600.0
# A head whose Operating state goes unknown (an ESPHome reconnect) keeps its
# last known state for this long before it counts as not running.
UNKNOWN_HOLD_S = 300.0


@dataclass
class OperatingHold:
    """Bridges short gaps in one head's Operating state for the plan."""

    known: bool | None = None
    unknown_since: float | None = None

    def update(self, now: float, operating: bool | None) -> bool | None:
        """The state the plan uses: the reported one, or the last known within the hold."""
        if operating is not None:
            self.known = operating
            self.unknown_since = None
            return operating
        if self.unknown_since is None:
            self.unknown_since = now
        if now - self.unknown_since > UNKNOWN_HOLD_S:
            return None
        return self.known


@dataclass
class StartPredictor:
    """Learns where one head starts, and forecasts its next start."""

    # Per mode ("heat", "cool"): how far past its target the room was at the
    # last starts, in the direction it drifts (below while heating).
    deficits: dict[str, deque[float]] = field(default_factory=dict)
    samples: deque[tuple[float, float]] = field(default_factory=deque)
    # None: not known yet, or after a gap (an edge across a gap is not a start
    # or a stop: the head may have started or stopped at any time in it).
    operating: bool | None = None
    last_stop: float | None = None
    target: float | None = None
    mode: str | None = None
    last_change: float | None = None
    # When the forecast last became due (0 s); cleared by a start, a stop or a
    # target or mode change, not by the room wobbling back above the level.
    overdue_since: float | None = None

    def observe(self, now: float, head: Head) -> bool:
        """Called on every state change and renewal with the head's state.

        True when the head started (an off to on edge, not one across a gap).
        """
        if head.target is not None:
            if self.target is not None and head.target != self.target:
                self.last_change = now
                self.overdue_since = None
            self.target = head.target
        if head.mode is not None:
            if self.mode is not None and head.mode != self.mode:
                self.last_change = now
                self.overdue_since = None
            self.mode = head.mode
        commanded = self.last_change is not None and now - self.last_change < AFTER_CHANGE_S
        started = bool(head.operating) and self.operating is False
        if (
            started
            and not commanded
            and head.mode in ("heat", "cool")
            and head.room is not None
            and head.target is not None
        ):
            sign = 1.0 if head.mode == "heat" else -1.0
            self.history(head.mode).append(sign * (head.target - head.room))
        if head.operating is False and self.operating is not False:
            # A stop, or a gap that ended stopped (it may have just stopped).
            self.last_stop = now
        if head.operating is not False:
            self.overdue_since = None
        self.operating = head.operating
        if head.room is not None:
            if not self.samples or now - self.samples[-1][0] >= 1.0:
                self.samples.append((now, head.room))
            while self.samples and now - self.samples[0][0] > 2 * SLOPE_WINDOW_S:
                self.samples.popleft()
        return started

    def history(self, mode: str) -> deque[float]:
        """The learned deficits of one mode."""
        return self.deficits.setdefault(mode, deque(maxlen=START_HISTORY))

    def to_dict(self) -> dict[str, Any]:
        """What is kept across restarts: the learned deficits."""
        return {"deficits": {mode: list(values) for mode, values in self.deficits.items() if values}}

    def load(self, data: Any) -> None:
        """Restores what to_dict saved; anything malformed is skipped."""
        deficits = data.get("deficits") if isinstance(data, dict) else None
        if not isinstance(deficits, dict):
            return
        for mode in ("heat", "cool"):
            values = _floats(deficits.get(mode), START_HISTORY, limit=MAX_DEFICIT)
            if values:
                self.history(mode).extend(values)

    def expected_in_s(self, now: float, head: Head) -> float | None:
        """Seconds until the head is predicted to start, or None."""
        if head.operating is not False or head.mode not in ("heat", "cool") or head.room is None or head.target is None:
            return None
        if head.powered is False:
            return None
        deficits = self.deficits.get(head.mode, ())
        if len(deficits) < MIN_STARTS:
            return None
        if self.last_stop is not None and now - self.last_stop < AFTER_STOP_S:
            return None
        sign = 1.0 if head.mode == "heat" else -1.0
        level = head.target - sign * statistics.median(deficits)
        # How far the room still has to go, in the direction it drifts.
        to_go = sign * (head.room - level)
        if to_go <= 0.0:
            if -to_go > OVERDUE_MARGIN:
                return None
            if self.overdue_since is None:
                self.overdue_since = now
            return 0.0 if now - self.overdue_since < OVERDUE_HOLD_S else None
        old = [(t, room) for t, room in self.samples if now - t >= SLOPE_WINDOW_S]
        if not old:
            return None
        then_t, then_room = old[-1]  # the newest sample at least SLOPE_WINDOW_S old
        fall = sign * (then_room - head.room) / (now - then_t)
        if fall < MIN_FALL_PER_S:
            return None
        return to_go / fall


# Two heads starting within this of each other stack their runs.
STACK_WINDOW_S = 300.0
# A start is scored against the forecast made at least this far ahead of it
# (the latest such), and is a hit within HIT_WITHIN_S of it.
SCORE_LEAD_S = 180.0
HIT_WITHIN_S = 120.0
# A forecast start this far in the past without a start is dropped (a miss is
# then an unforecast start).
STALE_FORECAST_S = 600.0
# The hit rate is over this many recent forecast starts.
SCORE_HISTORY = 20
# Saved values beyond these are not plausible and are not restored.
MAX_DEFICIT = 5.0
MAX_ERROR_S = 3600.0


def _floats(values: Any, keep: int, limit: float) -> list[float]:
    """The last `keep` finite numbers within +-limit of a saved list."""
    if not isinstance(values, list):
        return []
    good = [
        float(v)
        for v in values
        if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) and abs(v) <= limit
    ]
    return good[-keep:]


def _count(value: Any) -> int:
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else 0


@dataclass
class HeadStats:
    """How one head's starts went: counts, stacking and forecast accuracy."""

    starts: int = 0
    stacked: int = 0
    unforecast: int = 0
    last_start: float | None = None
    last_start_stacked: bool = False
    # When the latest forecast at least SCORE_LEAD_S ahead expects the start.
    forecast_at: float | None = None
    # Actual minus forecast start (s) of the last start; None if it had no forecast.
    last_error_s: float | None = None
    errors: deque[float] = field(default_factory=lambda: deque(maxlen=SCORE_HISTORY))

    def to_dict(self) -> dict[str, Any]:
        """What is kept across restarts: the counts and the recent forecast errors."""
        return {
            "starts": self.starts,
            "stacked": self.stacked,
            "unforecast": self.unforecast,
            "errors": list(self.errors),
        }

    def load(self, data: Any) -> None:
        """Restores what to_dict saved; anything malformed is skipped."""
        if not isinstance(data, dict):
            return
        self.starts = _count(data.get("starts"))
        self.stacked = min(_count(data.get("stacked")), self.starts)
        self.unforecast = min(_count(data.get("unforecast")), self.starts)
        self.errors.extend(_floats(data.get("errors"), SCORE_HISTORY, limit=MAX_ERROR_S))

    @property
    def hit_rate(self) -> float | None:
        """Percent of recent forecast starts within HIT_WITHIN_S; None before any."""
        if not self.errors:
            return None
        hits = sum(1 for error in self.errors if abs(error) <= HIT_WITHIN_S)
        return round(100.0 * hits / len(self.errors), 1)


@dataclass
class StartStats:
    """Start counts, stacking and forecast scoring across the heads of one unit."""

    heads: dict[str, HeadStats] = field(default_factory=dict)

    def head(self, key: str) -> HeadStats:
        return self.heads.setdefault(key, HeadStats())

    def forecast(self, now: float, key: str, seconds: float | None) -> None:
        """Called with each head's forecast while it is not running."""
        stats = self.head(key)
        if stats.forecast_at is not None and now - stats.forecast_at > STALE_FORECAST_S:
            stats.forecast_at = None
        if seconds is not None and seconds >= SCORE_LEAD_S:
            stats.forecast_at = now + seconds

    def start(self, now: float, key: str) -> None:
        """Called when a head starts."""
        stats = self.head(key)
        stats.starts += 1
        stacked = False
        for other_key, other in self.heads.items():
            if other_key == key or other.last_start is None or now - other.last_start > STACK_WINDOW_S:
                continue
            stacked = True
            if not other.last_start_stacked:
                other.stacked += 1
                other.last_start_stacked = True
        if stacked:
            stats.stacked += 1
        stats.last_start = now
        stats.last_start_stacked = stacked
        if stats.forecast_at is not None and now - stats.forecast_at <= STALE_FORECAST_S:
            stats.last_error_s = now - stats.forecast_at
            stats.errors.append(stats.last_error_s)
        else:
            stats.last_error_s = None
            stats.unforecast += 1
        stats.forecast_at = None


# Heads in opposite modes this long are a mode conflict (a mode automation
# switching the heads one by one disagrees only briefly).
CONFLICT_SUSTAIN_S = 600.0


@dataclass(frozen=True)
class ModeConflict:
    """Heads heating while others cool or dry, on one outdoor unit."""

    heating: tuple[str, ...]
    cooling: tuple[str, ...]


def mode_conflict(heads: dict[str, Head]) -> ModeConflict | None:
    """The heads in opposite modes, or None. Off, fan and auto conflict with nothing."""
    heating = tuple(sorted(head.name for head in heads.values() if head.mode == "heat"))
    cooling = tuple(sorted(head.name for head in heads.values() if head.mode in ("cool", "dry")))
    return ModeConflict(heating, cooling) if heating and cooling else None


@dataclass
class Sustained:
    """True once a condition has held for `seconds`; false as soon as it ends."""

    seconds: float
    since: float | None = None

    def update(self, now: float, active: bool) -> bool:
        if not active:
            self.since = None
            return False
        if self.since is None:
            self.since = now
        return now - self.since >= self.seconds


def outdoor_unit_running(heads: dict[str, Head]) -> bool:
    """Whether any head in the group is operating."""
    return any(head.operating for head in heads.values())


def plan_room(
    name: str,
    heads: dict[str, Head],
    expected: dict[str, float | None] | None = None,
    above: set[str] | None = None,
) -> RoomPlan | None:
    """The hint for one room, or None when nothing should be sent.

    `expected` holds each head's forecast start (seconds), if any. `above` is
    the heads whose runs and forecasts count for this room (None: every other
    head).
    """
    head = heads.get(name)
    if head is None or head.mode not in ("heat", "cool"):
        return None

    def counts(key: str) -> bool:
        return key != name and (above is None or key in above)

    running = sorted(other.name for key, other in heads.items() if counts(key) and other.operating)
    if running:
        return RoomPlan(shared_heat=True, reason=f"{', '.join(running)} running")
    due = [
        (seconds, heads[key].name)
        for key, seconds in (expected or {}).items()
        if counts(key) and key in heads and seconds is not None and seconds <= EXPECT_WINDOW_S
    ]
    if due:
        seconds, who = min(due)
        return RoomPlan(
            shared_heat=False,
            reason=f"{who} expected to start in {round(seconds)} s",
            expected_in_s=int(round(seconds)),
        )
    below = sorted(other.name for key, other in heads.items() if key != name and not counts(key) and other.operating)
    if below:
        return RoomPlan(shared_heat=False, reason=f"{', '.join(below)} running, ranks below")
    return RoomPlan(shared_heat=False, reason="no other head running")


def plan(
    heads: dict[str, Head],
    hinted: list[str],
    expected: dict[str, float | None] | None = None,
    order: list[str] | None = None,
) -> dict[str, RoomPlan]:
    """The hints for every hinted room that should get one.

    `order` is every head in order of precedence, highest first; a head not in
    it ranks above all of them. Without it, the heads that are not hinted rank
    first, then the hinted rooms in their own order.
    """
    if order is None:
        order = [key for key in heads if key not in hinted] + list(hinted)
    rank = {key: index for index, key in enumerate(order)}
    plans: dict[str, RoomPlan] = {}
    for name in hinted:
        mine = rank.get(name, len(order))
        above = {key for key in heads if key != name and rank.get(key, -1) < mine}
        room_plan = plan_room(name, heads, expected, above=above)
        if room_plan is not None:
            plans[name] = room_plan
    return plans


# Recovery: how long a head takes to bring its room to target, from the room
# model the head learns and publishes, and how that compares with real
# recoveries after a target change.
#
#   dT/dt = s * rate - loss * (T - T_outdoor)     (s = +1 heating, -1 cooling)
#
# at full output, after the dead time. With the outdoor temperature known, the
# room heads for T_steady = T_outdoor + s * rate / loss and reaches the target
# after -ln((target - T_steady) / (T - T_steady)) / loss. Without it, loss is
# left out (target - T) / rate. It is a full-output estimate for one head: the
# PID eases off near the target, and heads recovering together share the
# outdoor unit, so real recoveries take longer.

# A room within this of its target has recovered.
RECOVERY_REACHED = 0.1
# A target change that leaves the room at least this far from the new target,
# in the direction the head works, starts a recovery to time.
RECOVERY_MIN_GAP = 0.3
# A recovery not done in this time is dropped (the target was not reachable).
RECOVERY_MAX_S = 6 * 3600.0
# Kept and scored over this many recent recoveries.
RECOVERY_HISTORY = 20
MAX_RECOVERY_MIN = RECOVERY_MAX_S / 60.0


@dataclass(frozen=True)
class RoomModel:
    """What a head has learned about its room, as it publishes it."""

    heat_rate: float | None = None  # per hour at full output, in the climate's unit
    cool_rate: float | None = None
    loss: float | None = None  # per hour
    heat_dead_min: float | None = None
    cool_dead_min: float | None = None


def recovery_minutes(head: Head, model: RoomModel, outdoor: float | None) -> float | None:
    """Minutes until the room is within RECOVERY_REACHED of its target; 0 if it is.

    None outside heat and cool, without the room model, or when the target is
    out of reach (the room settles short of it).
    """
    if head.mode not in ("heat", "cool") or head.room is None or head.target is None:
        return None
    heating = head.mode == "heat"
    sign = 1.0 if heating else -1.0
    rate = model.heat_rate if heating else model.cool_rate
    dead = model.heat_dead_min if heating else model.cool_dead_min
    if rate is None or not rate > 0.0 or dead is None or not dead >= 0.0:
        return None
    goal = head.target - sign * RECOVERY_REACHED
    if sign * (goal - head.room) <= 0.0:
        return 0.0
    loss = model.loss if outdoor is not None and model.loss is not None and model.loss > 0.0 else None
    if loss is None:
        hours = sign * (goal - head.room) / rate
    else:
        steady = outdoor + sign * rate / loss
        ratio = (goal - steady) / (head.room - steady)
        if not 0.0 < ratio < 1.0:
            return None
        hours = -math.log(ratio) / loss
    return dead + 60.0 * hours


@dataclass(frozen=True)
class RecoveryResult:
    predicted_min: float
    actual_min: float

    @property
    def error_min(self) -> float:
        """Actual minus predicted: positive when the room took longer."""
        return self.actual_min - self.predicted_min


@dataclass
class RecoveryTracker:
    """Times one head's recoveries after target changes against the estimate."""

    target: float | None = None
    mode: str | None = None
    # The recovery being timed: (started, mode, predicted minutes).
    active: tuple[float, str, float] | None = None
    results: deque[RecoveryResult] = field(default_factory=lambda: deque(maxlen=RECOVERY_HISTORY))

    def observe(self, now: float, head: Head, model: RoomModel, outdoor: float | None) -> RecoveryResult | None:
        """Called with each update; the result when a recovery just finished."""
        changed = (head.target is not None and self.target is not None and head.target != self.target) or (
            head.mode is not None and self.mode is not None and head.mode != self.mode
        )
        if head.target is not None:
            self.target = head.target
        if head.mode is not None:
            self.mode = head.mode
        finished = None
        if self.active is not None:
            started, mode, predicted = self.active
            if changed or head.mode != mode or now - started > RECOVERY_MAX_S:
                self.active = None
            elif head.room is not None and head.target is not None:
                sign = 1.0 if mode == "heat" else -1.0
                if sign * (head.target - head.room) <= RECOVERY_REACHED:
                    finished = RecoveryResult(predicted_min=predicted, actual_min=(now - started) / 60.0)
                    self.results.append(finished)
                    self.active = None
        if changed and head.mode in ("heat", "cool") and head.room is not None and head.target is not None:
            sign = 1.0 if head.mode == "heat" else -1.0
            predicted = recovery_minutes(head, model, outdoor)
            if sign * (head.target - head.room) >= RECOVERY_MIN_GAP and predicted is not None:
                self.active = (now, head.mode, predicted)
        return finished

    @property
    def last(self) -> RecoveryResult | None:
        return self.results[-1] if self.results else None

    @property
    def mean_abs_error_min(self) -> float | None:
        if not self.results:
            return None
        return round(statistics.fmean(abs(r.error_min) for r in self.results), 1)

    def to_dict(self) -> dict[str, Any]:
        return {"results": [[r.predicted_min, r.actual_min] for r in self.results]}

    def load(self, data: Any) -> None:
        """Restores what to_dict saved; anything malformed is skipped."""
        results = data.get("results") if isinstance(data, dict) else None
        if not isinstance(results, list):
            return
        for pair in results[-RECOVERY_HISTORY:]:
            values = _floats(pair, 2, limit=MAX_RECOVERY_MIN)
            if isinstance(pair, list) and len(pair) == 2 and len(values) == 2 and min(values) >= 0.0:
                self.results.append(RecoveryResult(predicted_min=values[0], actual_min=values[1]))
