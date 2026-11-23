"""The supervisor's decisions, without Home Assistant."""

import math

import pytest

from custom_components.climate_adaptivepid_supervisor.logic import (
    OVERDUE_HOLD_S,
    UNKNOWN_HOLD_S,
    Head,
    OperatingHold,
    CONFLICT_SUSTAIN_S,
    RECOVERY_MAX_S,
    RecoveryTracker,
    RoomModel,
    recovery_minutes,
    HeadStats,
    ModeConflict,
    Sustained,
    mode_conflict,
    StartPredictor,
    StartStats,
    outdoor_unit_running,
    plan,
    plan_room,
)


def heads(**operating: bool) -> dict[str, Head]:
    return {
        name: Head(name=name, operating=operating.get(name, False), mode="heat")
        for name in ("hailee", "primary", "main")
    }


def test_outdoor_unit_running():
    assert not outdoor_unit_running(heads())
    assert outdoor_unit_running(heads(main=True))


def test_no_other_head_running_clears_shared_heat():
    p = plan_room("hailee", heads())
    assert p.shared_heat is False


def test_own_run_is_not_shared_heat():
    p = plan_room("hailee", heads(hailee=True))
    assert p.shared_heat is False


def test_other_head_running_is_shared_heat():
    p = plan_room("hailee", heads(primary=True, main=True))
    assert p.shared_heat is True
    assert "main, primary" in p.reason


def test_cooling_gets_hints_too():
    h = heads(primary=True)
    h["hailee"] = Head(name="hailee", operating=False, mode="cool")
    assert plan_room("hailee", h).shared_heat is True


def test_off_or_other_modes_get_no_hints():
    h = heads(main=True)
    h["hailee"] = Head(name="hailee", operating=False, mode="off")
    assert plan_room("hailee", h) is None
    assert plan(h, ["hailee"]) == {}


def test_only_hinted_rooms_are_planned():
    assert set(plan(heads(main=True), ["hailee"])) == {"hailee"}


def test_running_head_is_shared_heat_not_a_forecast():
    p = plan_room("hailee", heads(primary=True), {"primary": 60.0})
    assert p.shared_heat is True and p.expected_in_s is None


def test_forecast_from_other_heads_only():
    p = plan_room("hailee", heads(), {"hailee": 30.0, "primary": 150.4, "main": 400.0})
    assert p.shared_heat is False
    assert p.expected_in_s == 150
    assert "primary expected to start in 150 s" == p.reason
    assert plan_room("hailee", heads(), {"hailee": 30.0, "main": 400.0}).expected_in_s is None


# Main floor (not hinted) -> primary -> Hailee's room.
ORDER = ["primary", "hailee"]


def test_precedence_main_floor_heats_both_bedrooms():
    plans = plan(heads(main=True), ORDER)
    assert plans["primary"].shared_heat is True and plans["hailee"].shared_heat is True


def test_precedence_primary_ignores_a_lower_room():
    plans = plan(heads(hailee=True), ORDER)
    assert plans["primary"].shared_heat is False
    assert plans["primary"].reason == "hailee running, ranks below"
    assert plans["hailee"].shared_heat is False  # its own run


def test_precedence_lower_room_yields_to_a_higher_one():
    plans = plan(heads(primary=True), ORDER)
    assert plans["hailee"].shared_heat is True
    assert plans["primary"].shared_heat is False and plans["primary"].reason == "no other head running"


def test_precedence_forecasts_only_from_above():
    plans = plan(heads(), ORDER, {"primary": 120.0, "hailee": 60.0, "main": None})
    assert plans["primary"].expected_in_s is None
    assert plans["hailee"].expected_in_s == 120
    # The main floor's forecast counts for both.
    plans = plan(heads(), ORDER, {"primary": None, "hailee": None, "main": 200.0})
    assert plans["primary"].expected_in_s == 200 and plans["hailee"].expected_in_s == 200


def test_precedence_follows_the_configured_order():
    plans = plan(heads(primary=True), ["hailee", "primary"])
    assert plans["hailee"].shared_heat is False
    plans = plan(heads(hailee=True), ["hailee", "primary"])
    assert plans["primary"].shared_heat is True


def test_precedence_from_the_order_of_the_heads():
    # A head without hints ranked below a room is not heard by it.
    order = ["primary", "main", "hailee"]
    assert plan(heads(main=True), ORDER, order=order)["primary"].shared_heat is False
    assert plan(heads(main=True), ORDER, order=order)["hailee"].shared_heat is True
    assert plan(heads(primary=True), ORDER, order=order)["hailee"].shared_heat is True
    # A head missing from the order ranks above every room.
    assert plan(heads(main=True), ORDER, order=["primary", "hailee"])["primary"].shared_heat is True


def test_one_hinted_room_hears_every_other_head():
    assert plan(heads(primary=True), ["hailee"])["hailee"].shared_heat is True
    assert plan(heads(main=True), ["hailee"])["hailee"].shared_heat is True


def primary(room: float, operating: bool = False) -> Head:
    return Head(name="primary", operating=operating, mode="heat", room=room, target=19.72)


def learned_predictor() -> tuple[StartPredictor, float]:
    """The primary bedroom: four cycles, each starting 0.32 below its 19.72 target."""
    predictor = StartPredictor()
    now = 0.0
    for _ in range(4):
        predictor.observe(now, primary(19.70))
        now += 60
        predictor.observe(now, primary(19.40))
        now += 60
        predictor.observe(now, primary(19.40, operating=True))
        now += 480
        predictor.observe(now, primary(19.80, operating=True))
        now += 60
        predictor.observe(now, primary(19.80))  # stopped
    return predictor, now


def test_forecast_from_the_rooms_fall():
    predictor, now = learned_predictor()
    # The room falls 0.06 C/min after the coast: 19.70 now, start at 19.40.
    for minute in range(11):
        room = 20.30 - 0.06 * minute
        predictor.observe(now + 60 * minute, primary(room))
    now += 600
    seconds = predictor.expected_in_s(now, primary(19.70))
    assert seconds is not None and abs(seconds - 300.0) < 1.0


def test_no_forecast_without_enough_starts_or_fall():
    predictor = StartPredictor()
    predictor.observe(0, primary(19.7))
    predictor.observe(600, primary(19.5))
    assert predictor.expected_in_s(600, primary(19.5)) is None  # nothing learned
    learned, now = learned_predictor()
    for minute in range(11):
        learned.observe(now + 60 * minute, primary(19.70))  # flat
    assert learned.expected_in_s(now + 600, primary(19.70)) is None
    assert learned.expected_in_s(now + 600, primary(19.70, operating=True)) is None


def test_forecast_due_then_dropped_when_overdue():
    predictor, now = learned_predictor()
    for minute in range(11):
        predictor.observe(now + 60 * minute, primary(19.60 - 0.02 * minute))
    assert predictor.expected_in_s(now + 600, primary(19.38)) == 0.0
    assert predictor.expected_in_s(now + 600, primary(19.20)) is None  # 0.2 past: wrong forecast


def test_a_due_forecast_is_dropped_once_it_has_been_due_too_long():
    """A room holding flat at its start level is not "starting now" for an hour."""
    predictor, now = learned_predictor()
    for minute in range(11):
        predictor.observe(now + 60 * minute, primary(19.60 - 0.02 * minute))
    now += 600
    assert predictor.expected_in_s(now, primary(19.38)) == 0.0
    assert predictor.expected_in_s(now + OVERDUE_HOLD_S - 1, primary(19.38)) == 0.0
    assert predictor.expected_in_s(now + OVERDUE_HOLD_S, primary(19.38)) is None
    # Wobbling back above the level and down again does not renew it.
    predictor.observe(now + OVERDUE_HOLD_S + 60, primary(19.45))
    predictor.expected_in_s(now + OVERDUE_HOLD_S + 60, primary(19.45))
    assert predictor.expected_in_s(now + OVERDUE_HOLD_S + 120, primary(19.38)) is None
    # A start and stop clear it: the next cycle is forecast again.
    later = now + 2000
    predictor.observe(later, primary(19.38, operating=True))
    predictor.observe(later + 480, primary(19.80))
    for minute in range(1, 12):
        predictor.observe(later + 480 + 60 * minute, primary(19.80 - 0.04 * minute))
    assert predictor.expected_in_s(later + 480 + 660, primary(19.36)) == 0.0


def test_no_forecast_for_a_parked_head():
    """A head switched off by hysteresis starts only once it is switched back on."""
    predictor, now = learned_predictor()
    for minute in range(11):
        predictor.observe(now + 60 * minute, primary(19.60 - 0.02 * minute))
    parked = Head(name="primary", operating=False, mode="heat", room=19.38, target=19.72, powered=False)
    assert predictor.expected_in_s(now + 600, parked) is None
    falling = Head(name="primary", operating=False, mode="heat", room=19.50, target=19.72, powered=False)
    assert predictor.expected_in_s(now + 600, falling) is None
    assert predictor.expected_in_s(now + 600, primary(19.38)) == 0.0  # powered: forecast as before


def test_no_forecast_right_after_a_stop():
    predictor, now = learned_predictor()
    predictor.observe(now + 60, primary(19.30))
    assert predictor.expected_in_s(now + 60, primary(19.30)) is None


def test_a_gap_is_not_a_start():
    """An Operating sensor that drops out and comes back on is not a start."""
    predictor, now = learned_predictor()
    learned = list(predictor.deficits["heat"])
    predictor.observe(now + 60, Head(name="primary", operating=None, mode="heat", room=19.60, target=19.72))
    predictor.observe(now + 120, primary(19.65, operating=True))  # back, mid-run
    assert list(predictor.deficits["heat"]) == learned
    # The next real start is learned again.
    predictor.observe(now + 700, primary(19.80))
    predictor.observe(now + 1300, primary(19.40, operating=True))
    assert len(predictor.deficits["heat"]) == len(learned) + 1


def test_a_gap_that_ends_stopped_counts_as_a_stop():
    predictor, now = learned_predictor()
    predictor.observe(now + 600, primary(19.70, operating=True))
    predictor.observe(now + 660, Head(name="primary", operating=None, mode="heat", room=19.75, target=19.72))
    predictor.observe(now + 720, primary(19.75))
    assert predictor.last_stop == now + 720


def test_no_forecast_for_a_head_in_a_gap():
    predictor, now = learned_predictor()
    for minute in range(11):
        predictor.observe(now + 60 * minute, primary(19.60 - 0.02 * minute))
    unknown = Head(name="primary", operating=None, mode="heat", room=19.40, target=19.72)
    assert predictor.expected_in_s(now + 600, unknown) is None


def test_a_start_after_a_target_change_is_not_learned():
    predictor, now = learned_predictor()
    learned = list(predictor.deficits["heat"])
    raised = 19.72 + 0.56  # the sunrise automation: +1 F
    predictor.observe(now + 600, Head(name="primary", operating=False, mode="heat", room=19.70, target=raised))
    predictor.observe(now + 660, Head(name="primary", operating=True, mode="heat", room=19.70, target=raised))
    assert list(predictor.deficits["heat"]) == learned
    # Long after the change, starts are learned again.
    predictor.observe(now + 2000, Head(name="primary", operating=False, mode="heat", room=20.40, target=raised))
    predictor.observe(now + 3000, Head(name="primary", operating=True, mode="heat", room=19.96, target=raised))
    assert predictor.deficits["heat"][-1] == pytest.approx(0.32)


def test_a_start_after_a_mode_change_is_not_learned():
    predictor, now = learned_predictor()
    learned = list(predictor.deficits["heat"])
    predictor.observe(now + 600, Head(name="primary", operating=False, mode="cool", room=19.70, target=19.72))
    predictor.observe(now + 660, Head(name="primary", operating=True, mode="cool", room=20.20, target=19.72))
    assert list(predictor.deficits["heat"]) == learned


def test_operating_hold_bridges_short_gaps_only():
    hold = OperatingHold()
    assert hold.update(0, True) is True
    assert hold.update(10, None) is True  # reconnecting: still running
    assert hold.update(10 + UNKNOWN_HOLD_S, None) is True
    assert hold.update(11 + UNKNOWN_HOLD_S, None) is None  # gone too long: not known
    assert hold.update(400, False) is False
    assert OperatingHold().update(0, None) is None  # never known


def test_observe_reports_starts_but_not_gaps():
    predictor = StartPredictor()
    assert predictor.observe(0, primary(19.7)) is False  # first sight: not an edge
    assert predictor.observe(60, primary(19.4, operating=True)) is True
    assert predictor.observe(120, Head(name="primary", operating=None, mode="heat")) is False
    assert predictor.observe(180, primary(19.6, operating=True)) is False


def test_starts_within_the_window_are_stacked_both_ways():
    stats = StartStats()
    stats.start(0, "hailee")
    stats.start(200, "primary")  # 3.3 min after Hailee: both stacked
    stats.start(250, "main")  # stacks too; the others are counted once
    stats.start(2000, "hailee")  # alone
    assert (stats.head("hailee").starts, stats.head("hailee").stacked) == (2, 1)
    assert stats.head("primary").stacked == 1
    assert stats.head("main").stacked == 1


def test_forecast_scored_from_at_least_three_minutes_out():
    stats = StartStats()
    stats.forecast(0, "primary", 400.0)  # expects 400
    stats.forecast(100, "primary", 290.0)  # expects 390, still >= 3 min out
    stats.forecast(250, "primary", 130.0)  # too close to score: ignored
    stats.start(420, "primary")
    head = stats.head("primary")
    assert head.last_error_s == 30.0 and head.hit_rate == 100.0 and head.unforecast == 0
    stats.forecast(1000, "primary", 200.0)
    stats.start(1500, "primary")  # 300 s late: a miss
    assert head.last_error_s == 300.0 and head.hit_rate == 50.0


def test_unforecast_and_stale_forecasts():
    stats = StartStats()
    stats.start(0, "primary")
    head = stats.head("primary")
    assert head.last_error_s is None and head.unforecast == 1 and head.hit_rate is None
    stats.forecast(100, "primary", 200.0)  # expects 300
    stats.forecast(1000, "primary", None)  # 700 s past it: dropped
    stats.start(1100, "primary")
    assert head.unforecast == 2 and head.last_error_s is None


def test_cooling_does_not_use_heating_starts():
    predictor, now = learned_predictor()
    for minute in range(11):
        predictor.observe(now + 60 * minute, primary(19.60 - 0.02 * minute))
    cooling = Head(name="primary", operating=False, mode="cool", room=20.0, target=19.72)
    assert predictor.expected_in_s(now + 600, cooling) is None


def test_predictor_saved_and_restored():
    predictor, now = learned_predictor()
    restored = StartPredictor()
    restored.load(predictor.to_dict())
    assert restored.deficits["heat"] == predictor.deficits["heat"]
    for minute in range(11):
        restored.observe(now + 60 * minute, primary(19.60 - 0.02 * minute))
    assert restored.expected_in_s(now + 600, primary(19.38)) == 0.0  # forecasts at once


def test_malformed_saves_are_skipped():
    predictor = StartPredictor()
    predictor.load({"deficits": {"heat": [0.3, "x", None, True, float("nan"), 99.0, 0.31], "dry": [0.1]}})
    assert list(predictor.deficits["heat"]) == [0.3, 0.31] and "dry" not in predictor.deficits
    for junk in (None, [], {"deficits": [1, 2]}):
        StartPredictor().load(junk)
    stats = HeadStats()
    stats.load({"starts": 10, "stacked": 50, "unforecast": -3, "errors": [30, 1e9, "a", -45.5]})
    assert (stats.starts, stats.stacked, stats.unforecast) == (10, 10, 0)
    assert list(stats.errors) == [30.0, -45.5]
    HeadStats().load("junk")


def test_stats_saved_and_restored():
    stats = StartStats()
    stats.forecast(0, "primary", 200.0)
    stats.start(230, "primary")
    stats.start(300, "main")
    restored = HeadStats()
    restored.load(stats.head("primary").to_dict())
    assert (restored.starts, restored.stacked, list(restored.errors)) == (1, 1, [30.0])


def test_mode_conflict_is_heat_against_cool_or_dry():
    h = heads()
    assert mode_conflict(h) is None
    h["main"] = Head(name="main", mode="cool")
    assert mode_conflict(h) == ModeConflict(heating=("hailee", "primary"), cooling=("main",))
    h["main"] = Head(name="main", mode="dry")
    assert mode_conflict(h) is not None
    for harmless in ("off", "fan_only", "heat_cool", "unavailable", None):
        h["main"] = Head(name="main", mode=harmless)
        assert mode_conflict(h) is None


def test_sustained_only_after_its_time_and_ends_at_once():
    watch = Sustained(CONFLICT_SUSTAIN_S)
    assert watch.update(0, True) is False
    assert watch.update(CONFLICT_SUSTAIN_S - 1, True) is False
    assert watch.update(CONFLICT_SUSTAIN_S, True) is True
    assert watch.update(CONFLICT_SUSTAIN_S + 1, False) is False
    assert watch.update(CONFLICT_SUSTAIN_S + 2, True) is False  # starts over


MODEL = RoomModel(heat_rate=2.0, cool_rate=1.5, loss=0.1, heat_dead_min=5.0, cool_dead_min=4.0)


def room(temp: float, target: float, mode: str = "heat") -> Head:
    return Head(name="primary", operating=False, mode=mode, room=temp, target=target)


def test_recovery_without_outdoor_is_gap_over_rate():
    # 1.0 to go, less the 0.1 that counts as there, at 2.0/h: 27 min, plus 5 dead.
    assert recovery_minutes(room(19.0, 20.0), MODEL, None) == pytest.approx(5.0 + 27.0)
    assert recovery_minutes(room(21.0, 20.0, "cool"), MODEL, None) == pytest.approx(4.0 + 36.0)


def test_recovery_with_heat_loss_takes_longer_in_the_cold():
    # Steady state 0 + 2.0 / 0.1 = 20 C: the gap to it falls from 1.0 to 0.1
    # (19.9 reached) after ln(10) / 0.1 h.
    minutes = recovery_minutes(room(19.0, 20.0), MODEL, 0.0)
    assert minutes == pytest.approx(5.0 + 60.0 * math.log(10.0) / 0.1)
    assert minutes > recovery_minutes(room(19.0, 20.0), MODEL, 15.0)
    # The room settles at 19.5 short of 19.9: out of reach.
    assert recovery_minutes(room(19.0, 20.0), MODEL, -0.5) is None


def test_recovery_at_target_or_unknown():
    assert recovery_minutes(room(19.95, 20.0), MODEL, None) == 0.0
    assert recovery_minutes(room(19.0, 20.0, "off"), MODEL, None) is None
    assert recovery_minutes(room(19.0, 20.0), RoomModel(), None) is None
    assert recovery_minutes(room(19.0, 20.0), RoomModel(heat_rate=0.0, heat_dead_min=5.0), None) is None


def test_recovery_timed_after_a_target_raise():
    tracker = RecoveryTracker()
    assert tracker.observe(0, room(19.6, 19.72), MODEL, None) is None
    tracker.observe(60, room(19.6, 20.28), MODEL, None)  # raised: 0.68 to go
    assert tracker.active is not None
    predicted = tracker.active[2]
    assert predicted == pytest.approx(5.0 + 60.0 * 0.58 / 2.0)
    assert tracker.observe(1000, room(20.0, 20.28), MODEL, None) is None
    result = tracker.observe(60 + 30 * 60, room(20.2, 20.28), MODEL, None)
    assert result.actual_min == pytest.approx(30.0) and result.error_min == pytest.approx(30.0 - predicted)
    assert tracker.active is None and tracker.last == result


def test_small_or_lowering_changes_are_not_timed_and_changes_abort():
    tracker = RecoveryTracker()
    tracker.observe(0, room(19.6, 19.72), MODEL, None)
    tracker.observe(60, room(19.6, 19.80), MODEL, None)  # 0.2 to go: too small
    tracker.observe(120, room(19.6, 19.00), MODEL, None)  # lowered while heating
    assert tracker.active is None
    tracker.observe(180, room(19.0, 20.0), MODEL, None)
    assert tracker.active is not None
    tracker.observe(240, room(19.1, 19.5), MODEL, None)  # changed again: dropped, 0.4 to go: new one
    assert tracker.active is not None and tracker.active[0] == 240
    tracker.observe(300, room(19.1, 19.5, "cool"), MODEL, None)
    assert tracker.active is None and not tracker.results
    tracker.observe(400, room(19.1, 19.5), MODEL, None)  # back to heat, 0.4 below: timed
    assert tracker.observe(400 + RECOVERY_MAX_S + 1, room(19.2, 19.5), MODEL, None) is None
    assert tracker.active is None  # too long: dropped


def test_recoveries_saved_and_restored():
    tracker = RecoveryTracker()
    tracker.observe(0, room(19.0, 19.0), MODEL, None)
    tracker.observe(60, room(19.0, 20.0), MODEL, None)
    tracker.observe(3660, room(19.95, 20.0), MODEL, None)
    restored = RecoveryTracker()
    restored.load(tracker.to_dict())
    assert restored.last == tracker.last and restored.mean_abs_error_min == tracker.mean_abs_error_min
    junk = RecoveryTracker()
    junk.load({"results": [[1.0], [2.0, "x"], [-1.0, 3.0], [5.0, 1e9], "x", [10.0, 12.5]]})
    assert [(r.predicted_min, r.actual_min) for r in junk.results] == [(10.0, 12.5)]
    RecoveryTracker().load(None)
