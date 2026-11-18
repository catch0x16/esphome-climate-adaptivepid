"""The supervisor's decisions, without Home Assistant."""

from custom_components.climate_adaptivepid_supervisor.logic import Head, outdoor_unit_running, plan, plan_room


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
