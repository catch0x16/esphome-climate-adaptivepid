"""Turns the supervisor's commands on and off."""

from __future__ import annotations

from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.const import STATE_OFF
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.helpers.restore_state import RestoreEntity

from . import SupervisorConfigEntry
from .entity import SupervisorEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: SupervisorConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([SupervisorEnabledSwitch(entry.runtime_data)])


class SupervisorEnabledSwitch(SupervisorEntity, SwitchEntity, RestoreEntity):
    """While off, no hints are sent and the heads decide alone once theirs expire."""

    _attr_translation_key = "enabled"

    def __init__(self, coordinator) -> None:
        super().__init__(coordinator, "enabled")

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        last = await self.async_get_last_state()
        if last is not None and last.state == STATE_OFF:
            await self.coordinator.async_set_enabled(False)

    @property
    def is_on(self) -> bool:
        return self.coordinator.enabled

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self.coordinator.async_set_enabled(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.coordinator.async_set_enabled(False)
