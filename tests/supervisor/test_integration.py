"""The supervisor running in Home Assistant against three fake heads."""

from __future__ import annotations

from datetime import timedelta

from pytest_homeassistant_custom_component.common import MockConfigEntry, async_fire_time_changed

from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.util import dt as dt_util

from custom_components.climate_adaptivepid_supervisor.const import (
    CONF_HEADS,
    CONF_HINTED,
    CONF_PARK_MARGIN,
    DOMAIN,
    PARK_VALID_SECONDS,
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
            CONF_PARK_MARGIN: 0.2,
        },
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


async def test_hint_lifecycle(hass: HomeAssistant, house: FakeHouse) -> None:
    await setup_supervisor(hass, house)
    # Nobody running: shared heat cleared, no park request.
    assert house.calls("hailee") == [("shared_heat", {"active": False, "valid_for_s": SHARED_HEAT_VALID_SECONDS})]
    assert house.calls("primary") == [] and house.calls("main") == []

    # The primary bedroom starts; Hailee's room is 0.28 below target (outside the 0.2 margin): take the leak.
    house.heads["hailee"].calls.clear()
    house.set_operating("primary", True)
    await hass.async_block_till_done()
    assert house.calls("hailee") == [
        ("shared_heat", {"active": True, "valid_for_s": SHARED_HEAT_VALID_SECONDS}),
        ("request_park", {"park": False, "valid_for_s": PARK_VALID_SECONDS}),
    ]
    assert hass.states.get("binary_sensor.outdoor_unit_outdoor_unit_running").state == "on"

    # The room warms to within the margin: park.
    house.heads["hailee"].calls.clear()
    house.set_climate("hailee", room=20.15)
    await hass.async_block_till_done()
    assert ("request_park", {"park": True, "valid_for_s": PARK_VALID_SECONDS}) in house.calls("hailee")

    # Renewed every minute while it still applies.
    house.heads["hailee"].calls.clear()
    async_fire_time_changed(hass, dt_util.utcnow() + timedelta(seconds=61))
    await hass.async_block_till_done()
    assert ("request_park", {"park": True, "valid_for_s": PARK_VALID_SECONDS}) in house.calls("hailee")

    # The primary stops: shared heat cleared, the park request left to expire.
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
        {"name": "Outdoor unit", CONF_HEADS: heads[:1], CONF_HINTED: [], CONF_PARK_MARGIN: 0.2},
    )
    assert result["errors"] == {CONF_HEADS: "need_two_heads"}
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"],
        {"name": "Outdoor unit", CONF_HEADS: heads, CONF_HINTED: [heads[0]], CONF_PARK_MARGIN: 0.25},
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"] == {CONF_HEADS: heads, CONF_HINTED: [heads[0]], CONF_PARK_MARGIN: 0.25}
