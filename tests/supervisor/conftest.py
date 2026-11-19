"""Fixtures for the supervisor integration tests.

Run in an isolated environment (pytest-homeassistant-custom-component pins its
own Home Assistant):

    uv run --isolated --no-project --python 3.13 \\
      --with pytest-homeassistant-custom-component \\
      pytest tests/supervisor -o pythonpath=. -o asyncio_mode=auto
"""

from __future__ import annotations

from dataclasses import dataclass, field

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.helpers import device_registry as dr, entity_registry as er

pytest_plugins = "pytest_homeassistant_custom_component"


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    yield


@dataclass
class FakeHead:
    """One climate_adaptivepid head as Home Assistant sees it."""

    node: str
    climate: str
    operating: str
    calls: list[tuple[str, dict]] = field(default_factory=list)


@dataclass
class FakeHouse:
    hass: HomeAssistant
    heads: dict[str, FakeHead]

    def set_climate(self, key: str, mode: str = "heat", room: float = 20.0, target: float = 20.28) -> None:
        self.hass.states.async_set(
            self.heads[key].climate, mode, {"current_temperature": room, "temperature": target}
        )

    def set_operating(self, key: str, on: bool) -> None:
        self.hass.states.async_set(self.heads[key].operating, "on" if on else "off")

    def calls(self, key: str, action: str = "shared_heat") -> list[tuple[str, dict]]:
        """One action's calls to a head (shared_heat by default)."""
        return [call for call in self.heads[key].calls if call[0] == action]


@pytest.fixture
async def house(hass: HomeAssistant) -> FakeHouse:
    """Three heads on one outdoor unit, each an ESPHome device with its actions."""
    ent_reg = er.async_get(hass)
    dev_reg = dr.async_get(hass)
    heads: dict[str, FakeHead] = {}
    for key, node in (("hailee", "haileesbedroomhvac"), ("primary", "primarybedroomhvac"), ("main", "mainfloorhvac")):
        esphome_entry = MockConfigEntry(domain="esphome", data={"device_name": node}, title=node)
        esphome_entry.add_to_hass(hass)
        device = dev_reg.async_get_or_create(config_entry_id=esphome_entry.entry_id, identifiers={("esphome", node)})
        climate = ent_reg.async_get_or_create(
            "climate", "esphome", f"{node}-climate", device_id=device.id, config_entry=esphome_entry,
            suggested_object_id=f"{node}_hvac", original_name="HVAC",
        )
        operating = ent_reg.async_get_or_create(
            "binary_sensor", "esphome", f"{node}-operating", device_id=device.id, config_entry=esphome_entry,
            suggested_object_id=f"{node}_operating", original_name="Operating",
        )
        head = FakeHead(node=node, climate=climate.entity_id, operating=operating.entity_id)
        heads[key] = head

        def make_handler(head=head):
            async def handler(call: ServiceCall) -> None:
                head.calls.append((call.service.removeprefix(f"{head.node}_"), dict(call.data)))

            return handler

        hass.services.async_register("esphome", f"{node}_shared_heat", make_handler())
        hass.services.async_register("esphome", f"{node}_shared_heat_expected", make_handler())
    house = FakeHouse(hass=hass, heads=heads)
    for key in heads:
        house.set_climate(key)
        house.set_operating(key, False)
    return house
