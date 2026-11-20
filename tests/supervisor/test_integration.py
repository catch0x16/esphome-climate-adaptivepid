"""The supervisor running in Home Assistant against three fake heads."""

from __future__ import annotations

from datetime import timedelta

from freezegun.api import FrozenDateTimeFactory
from pytest_homeassistant_custom_component.common import MockConfigEntry, async_fire_time_changed

from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.util import dt as dt_util

from custom_components.climate_adaptivepid_supervisor.const import (
    CONF_HEADS,
    CONF_HINTED,
    DOMAIN,
    SHARED_HEAT_VALID_SECONDS,
)

from .conftest import FakeHouse


async def setup_supervisor(hass: HomeAssistant, house: FakeHouse) -> MockConfigEntry:
    entry = MockConfigEntry(
        domain=DOMAIN,
        title="Outdoor unit",
        data={
            CONF_HEADS: [h.climate for h in house.heads.values()],
            CONF_HINTED: [house.heads["hailee"].climate],
        },
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


async def test_hint_lifecycle(hass: HomeAssistant, house: FakeHouse) -> None:
    await setup_supervisor(hass, house)
    # Nobody running: shared heat cleared.
    assert house.calls("hailee") == [("shared_heat", {"active": False, "valid_for_s": SHARED_HEAT_VALID_SECONDS})]
    assert house.calls("primary") == [] and house.calls("main") == []

    # The primary bedroom starts: shared heat on.
    house.heads["hailee"].calls.clear()
    house.set_operating("primary", True)
    await hass.async_block_till_done()
    assert house.calls("hailee") == [("shared_heat", {"active": True, "valid_for_s": SHARED_HEAT_VALID_SECONDS})]
    assert hass.states.get("binary_sensor.outdoor_unit_outdoor_unit_running").state == "on"

    # A room temperature change alone sends nothing new.
    house.heads["hailee"].calls.clear()
    house.set_climate("hailee", room=20.15)
    await hass.async_block_till_done()
    assert house.calls("hailee") == []

    # Renewed every minute while it still applies.
    async_fire_time_changed(hass, dt_util.utcnow() + timedelta(seconds=61))
    await hass.async_block_till_done()
    assert house.calls("hailee") == [("shared_heat", {"active": True, "valid_for_s": SHARED_HEAT_VALID_SECONDS})]

    # The primary stops: shared heat cleared.
    house.heads["hailee"].calls.clear()
    house.set_operating("primary", False)
    await hass.async_block_till_done()
    assert house.calls("hailee") == [("shared_heat", {"active": False, "valid_for_s": SHARED_HEAT_VALID_SECONDS})]


async def test_own_run_is_not_shared(hass: HomeAssistant, house: FakeHouse) -> None:
    await setup_supervisor(hass, house)
    house.heads["hailee"].calls.clear()
    house.set_operating("hailee", True)
    await hass.async_block_till_done()
    # Nothing changed for Hailee's room: no new call until the renewal.
    assert house.calls("hailee") == []
    assert hass.states.get("binary_sensor.outdoor_unit_outdoor_unit_running").state == "on"
    sensors = [s for s in hass.states.async_all("sensor") if s.entity_id.endswith("_supervisor_action")]
    assert len(sensors) == 1 and sensors[0].state == "none"


async def test_disable_switch_stops_commands(hass: HomeAssistant, house: FakeHouse) -> None:
    await setup_supervisor(hass, house)
    await hass.services.async_call(
        "switch", "turn_off", {"entity_id": "switch.outdoor_unit_supervisor_enabled"}, blocking=True
    )
    house.heads["hailee"].calls.clear()
    house.set_operating("main", True)
    await hass.async_block_till_done()
    async_fire_time_changed(hass, dt_util.utcnow() + timedelta(seconds=61))
    await hass.async_block_till_done()
    assert house.calls("hailee") == []
    sensors = [s for s in hass.states.async_all("sensor") if s.entity_id.endswith("_supervisor_action")]
    assert sensors[0].state == "disabled"
    # Back on: hints resume at once.
    await hass.services.async_call(
        "switch", "turn_on", {"entity_id": "switch.outdoor_unit_supervisor_enabled"}, blocking=True
    )
    await hass.async_block_till_done()
    assert ("shared_heat", {"active": True, "valid_for_s": SHARED_HEAT_VALID_SECONDS}) in house.calls("hailee")


async def test_operating_dropout_keeps_shared_heat(
    hass: HomeAssistant, house: FakeHouse, freezer: FrozenDateTimeFactory
) -> None:
    """An ESPHome reconnect of a running head does not clear the other rooms' shared heat."""
    entry = await setup_supervisor(hass, house)
    house.set_operating("primary", True)
    await hass.async_block_till_done()
    predictor = entry.runtime_data.predictors[house.heads["primary"].climate]
    learned = list(predictor.deficits)  # the real start
    house.heads["hailee"].calls.clear()
    hass.states.async_set(house.heads["primary"].operating, "unavailable")
    await hass.async_block_till_done()
    assert house.calls("hailee") == []  # unchanged: still shared heat
    assert hass.states.get("binary_sensor.outdoor_unit_outdoor_unit_running").state == "on"
    # Back, still running: not learned as a start.
    house.set_operating("primary", True)
    await hass.async_block_till_done()
    assert list(predictor.deficits) == learned
    # Gone for good: after the hold it no longer counts as running.
    hass.states.async_set(house.heads["primary"].operating, "unavailable")
    await hass.async_block_till_done()
    for _ in range(6):
        freezer.tick(timedelta(seconds=61))
        async_fire_time_changed(hass, dt_util.utcnow())
        await hass.async_block_till_done()
    assert house.calls("hailee")[-1] == ("shared_heat", {"active": False, "valid_for_s": SHARED_HEAT_VALID_SECONDS})


async def test_head_offline_does_not_stop_others(hass: HomeAssistant, house: FakeHouse) -> None:
    hass.services.async_remove("esphome", "haileesbedroomhvac_shared_heat")
    await setup_supervisor(hass, house)
    house.set_operating("main", True)
    await hass.async_block_till_done()
    # The failed call is logged, not raised; the coordinator keeps running.
    assert hass.states.get("binary_sensor.outdoor_unit_outdoor_unit_running").state == "on"


async def test_config_flow(hass: HomeAssistant, house: FakeHouse) -> None:
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert result["type"] is FlowResultType.FORM
    heads = [h.climate for h in house.heads.values()]
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"],
        {"name": "Outdoor unit", CONF_HEADS: heads[:1], CONF_HINTED: []},
    )
    assert result["errors"] == {CONF_HEADS: "need_two_heads"}
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"],
        {"name": "Outdoor unit", CONF_HEADS: heads, CONF_HINTED: [heads[0]]},
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"] == {CONF_HEADS: heads, CONF_HINTED: [heads[0]]}


async def test_forecast_lifecycle(hass: HomeAssistant, house: FakeHouse, freezer: FrozenDateTimeFactory) -> None:
    # The target is set before the supervisor starts: a start right after a
    # target change is not learned.
    house.set_climate("primary", room=19.70, target=19.72)
    await setup_supervisor(hass, house)
    assert house.calls("hailee", "shared_heat_expected") == [
        ("shared_heat_expected", {"expected_in_s": -1, "valid_for_s": SHARED_HEAT_VALID_SECONDS})
    ]

    async def primary(room: float, operating: bool | None = None, minutes: float = 1.0) -> None:
        freezer.tick(timedelta(minutes=minutes))
        house.set_climate("primary", room=room, target=19.72)
        if operating is not None:
            house.set_operating("primary", operating)
        await hass.async_block_till_done()

    # Four primary cycles, each starting 0.32 below its target.
    for _ in range(4):
        await primary(19.70)
        await primary(19.40)
        await primary(19.40, operating=True)
        await primary(19.80, minutes=8)
        await primary(19.80, operating=False)

    # Falling 0.06 C/min towards 19.40: no forecast while 19.70 is 5 min out
    # and the slope is still being measured; then one within 5 min.
    house.heads["hailee"].calls.clear()
    for minute in range(12):
        await primary(20.30 - 0.06 * minute)
    sent = house.calls("hailee", "shared_heat_expected")
    assert sent, "a forecast once the primary is due within 5 min"
    assert 0 < sent[-1][1]["expected_in_s"] <= 300
    sensor = next(s for s in hass.states.async_all("sensor") if s.entity_id.endswith("_supervisor_action"))
    assert sensor.state == "shared_heat_expected"

    # The primary starts: shared heat replaces the forecast, which is cleared.
    house.heads["hailee"].calls.clear()
    await primary(19.40, operating=True)
    assert house.calls("hailee") == [("shared_heat", {"active": True, "valid_for_s": SHARED_HEAT_VALID_SECONDS})]
    assert house.calls("hailee", "shared_heat_expected") == [
        ("shared_heat_expected", {"expected_in_s": -1, "valid_for_s": SHARED_HEAT_VALID_SECONDS})
    ]


async def test_head_without_forecast_action(hass: HomeAssistant, house: FakeHouse) -> None:
    hass.services.async_remove("esphome", "haileesbedroomhvac_shared_heat_expected")
    await setup_supervisor(hass, house)
    house.set_operating("main", True)
    await hass.async_block_till_done()
    # Shared heat still goes out; the forecast is skipped.
    assert ("shared_heat", {"active": True, "valid_for_s": SHARED_HEAT_VALID_SECONDS}) in house.calls("hailee")
    assert house.calls("hailee", "shared_heat_expected") == []
