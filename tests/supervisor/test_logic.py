"""The supervisor's decisions, without Home Assistant."""

from custom_components.climate_adaptivepid_supervisor.logic import Head, outdoor_unit_running, plan, plan_room


def heads(**operating: bool) -> dict[str, Head]:
    rooms = {"hailee": (20.1, 20.28), "primary": (19.6, 19.72), "main": (21.0, 21.11)}
    return {
        name: Head(name=name, operating=operating.get(name, False), mode="heat", room=room, target=target)
        for name, (room, target) in rooms.items()
    }


def test_outdoor_unit_running():
    assert not outdoor_unit_running(heads())
    assert outdoor_unit_running(heads(main=True))


def test_no_other_head_running_clears_shared_heat_without_park():
    p = plan_room("hailee", heads(), 0.2)
    assert p.shared_heat is False and p.park is None


def test_own_run_is_not_shared_heat():
    p = plan_room("hailee", heads(hailee=True), 0.2)
    assert p.shared_heat is False and p.park is None


def test_other_head_running_near_target_parks():
    p = plan_room("hailee", heads(primary=True), 0.2)  # -0.18 from target
    assert p.shared_heat is True and p.park is True
    assert "primary" in p.reason


def test_other_head_running_well_below_target_unparks_to_take_the_leak():
    h = heads(main=True)
    h["hailee"] = Head(name="hailee", operating=False, mode="heat", room=19.8, target=20.28)
    p = plan_room("hailee", h, 0.2)
    assert p.shared_heat is True and p.park is False


def test_cooling_mirrors():
    h = heads(primary=True)
    h["hailee"] = Head(name="hailee", operating=False, mode="cool", room=23.6, target=23.5)
    assert plan_room("hailee", h, 0.2).park is True
    h["hailee"] = Head(name="hailee", operating=False, mode="cool", room=24.2, target=23.5)
    assert plan_room("hailee", h, 0.2).park is False


def test_unknown_room_temperature_sends_shared_heat_only():
    h = heads(main=True)
    h["hailee"] = Head(name="hailee", operating=False, mode="heat", room=None, target=20.28)
    p = plan_room("hailee", h, 0.2)
    assert p.shared_heat is True and p.park is None


def test_off_or_other_modes_get_no_hints():
    h = heads(main=True)
    h["hailee"] = Head(name="hailee", operating=False, mode="off", room=20.0, target=20.28)
    assert plan_room("hailee", h, 0.2) is None
    assert plan(h, ["hailee"], 0.2) == {}


def test_only_hinted_rooms_are_planned():
    assert set(plan(heads(main=True), ["hailee"], 0.2)) == {"hailee"}
