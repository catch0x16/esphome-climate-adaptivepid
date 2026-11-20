"""The supervisor's decisions, without Home Assistant."""

import pytest

from custom_components.climate_adaptivepid_supervisor.logic import (
    UNKNOWN_HOLD_S,
    Head,
    OperatingHold,
    HeadStats,
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
