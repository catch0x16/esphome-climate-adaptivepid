"""Base entity for the supervisor."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import Entity

from .const import DOMAIN
from .coordinator import SupervisorCoordinator


class SupervisorEntity(Entity):
    """Updates whenever the coordinator recomputes."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, coordinator: SupervisorCoordinator, key: str) -> None:
        self.coordinator = coordinator
        entry = coordinator.entry
        self._attr_unique_id = f"{entry.entry_id}_{key}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.entry_id)},
            name=entry.title,
            manufacturer="climate_adaptivepid",
            model="Outdoor unit supervisor",
        )

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(self.coordinator.async_add_listener(self.async_write_ha_state))


def room_key(climate_entity_id: str) -> str:
    return climate_entity_id.split(".", 1)[-1]
