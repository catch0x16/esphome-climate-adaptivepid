"""Cross-head decisions for climate_adaptivepid heads on one outdoor unit.

Pure Python with no Home Assistant imports, so it can be tested on its own
(tests/supervisor/test_logic.py). See docs/supervisor-design.md and
docs/start-control-design.md.

Heads on a shared outdoor unit leak heat (cooling) into each other: while any
head runs, an idle head that is still powered keeps receiving refrigerant and
its fan carries that into its room. A head can only infer that from its own
room; the supervisor sees every head's `operating` and knows for certain.

For each room that receives hints, shared heat is on while any *other* head in
the group is operating; the room's own run never counts. The head decides what
to do with it: it starts no run of its own while shared heat is on, and parks
(switches off) at once once its room is past its hysteresis off_threshold.

Shared heat can also be forecast. A head that is not under start control
starts at a fairly steady deficit below its target (the primary bedroom:
0.22-0.37 C, median 0.32), so its next start is predicted from its room
temperature and how fast it is falling. A hinted room is told when another
head is expected to start, so it can wait for that heat instead of starting a
run that stacks on top of it. Runs also delay each other: a bedroom's run
warms the other bedroom and pushes its start back by several minutes.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
import statistics


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

    deficits: deque[float] = field(default_factory=lambda: deque(maxlen=START_HISTORY))
    samples: deque[tuple[float, float]] = field(default_factory=deque)
    # None: not known yet, or after a gap (an edge across a gap is not a start
    # or a stop: the head may have started or stopped at any time in it).
    operating: bool | None = None
    last_stop: float | None = None
    target: float | None = None
    mode: str | None = None
    last_change: float | None = None

    def observe(self, now: float, head: Head) -> None:
        """Called on every state change and renewal with the head's state."""
        if head.target is not None:
            if self.target is not None and head.target != self.target:
                self.last_change = now
            self.target = head.target
        if head.mode is not None:
            if self.mode is not None and head.mode != self.mode:
                self.last_change = now
            self.mode = head.mode
        commanded = self.last_change is not None and now - self.last_change < AFTER_CHANGE_S
        if (
            head.operating
            and self.operating is False
            and not commanded
            and head.mode in ("heat", "cool")
            and head.room is not None
            and head.target is not None
        ):
            sign = 1.0 if head.mode == "heat" else -1.0
            self.deficits.append(sign * (head.target - head.room))
        if head.operating is False and self.operating is not False:
            # A stop, or a gap that ended stopped (it may have just stopped).
            self.last_stop = now
        self.operating = head.operating
        if head.room is not None:
            if not self.samples or now - self.samples[-1][0] >= 1.0:
                self.samples.append((now, head.room))
            while self.samples and now - self.samples[0][0] > 2 * SLOPE_WINDOW_S:
                self.samples.popleft()

    def expected_in_s(self, now: float, head: Head) -> float | None:
        """Seconds until the head is predicted to start, or None."""
        if head.operating is not False or head.mode not in ("heat", "cool") or head.room is None or head.target is None:
            return None
        if len(self.deficits) < MIN_STARTS:
            return None
        if self.last_stop is not None and now - self.last_stop < AFTER_STOP_S:
            return None
        sign = 1.0 if head.mode == "heat" else -1.0
        level = head.target - sign * statistics.median(self.deficits)
        # How far the room still has to go, in the direction it drifts.
        to_go = sign * (head.room - level)
        if to_go <= 0.0:
            return 0.0 if -to_go <= OVERDUE_MARGIN else None
        old = [(t, room) for t, room in self.samples if now - t >= SLOPE_WINDOW_S]
        if not old:
            return None
        then_t, then_room = old[-1]  # the newest sample at least SLOPE_WINDOW_S old
        fall = sign * (then_room - head.room) / (now - then_t)
        if fall < MIN_FALL_PER_S:
            return None
        return to_go / fall


def outdoor_unit_running(heads: dict[str, Head]) -> bool:
    """Whether any head in the group is operating."""
    return any(head.operating for head in heads.values())


def plan_room(
    name: str, heads: dict[str, Head], expected: dict[str, float | None] | None = None
) -> RoomPlan | None:
    """The hint for one room, or None when nothing should be sent.

    `expected` holds each head's forecast start (seconds), if any.
    """
    head = heads.get(name)
    if head is None or head.mode not in ("heat", "cool"):
        return None
    running = sorted(other.name for key, other in heads.items() if key != name and other.operating)
    if running:
        return RoomPlan(shared_heat=True, reason=f"{', '.join(running)} running")
    due = [
        (seconds, heads[key].name)
        for key, seconds in (expected or {}).items()
        if key != name and key in heads and seconds is not None and seconds <= EXPECT_WINDOW_S
    ]
    if due:
        seconds, who = min(due)
        return RoomPlan(
            shared_heat=False,
            reason=f"{who} expected to start in {round(seconds)} s",
            expected_in_s=int(round(seconds)),
        )
    return RoomPlan(shared_heat=False, reason="no other head running")


def plan(
    heads: dict[str, Head], hinted: list[str], expected: dict[str, float | None] | None = None
) -> dict[str, RoomPlan]:
    """The hints for every hinted room that should get one."""
    plans: dict[str, RoomPlan] = {}
    for name in hinted:
        room_plan = plan_room(name, heads, expected)
        if room_plan is not None:
            plans[name] = room_plan
    return plans
