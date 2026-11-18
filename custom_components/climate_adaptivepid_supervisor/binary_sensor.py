"""Outdoor unit running (diagnostic) and shared heat per hinted room."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SupervisorConfigEntry
from .entity import SupervisorEntity, room_key


async def async_setup_entry(
    hass: HomeAssistant, entry: SupervisorConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    entities: list[BinarySensorEntity] = [OutdoorUnitRunning(coordinator)]
    entities += [SharedHeat(coordinator, climate) for climate in coordinator.hinted]
    async_add_entities(entities)


class OutdoorUnitRunning(SupervisorEntity, BinarySensorEntity):
    """Any head in the group operating. Diagnostic only: hints use the other heads."""

    _attr_translation_key = "outdoor_unit_running"
    _attr_device_class = BinarySensorDeviceClass.RUNNING
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator) -> None:
        super().__init__(coordinator, "outdoor_unit_running")

    @property
    def is_on(self) -> bool:
        return self.coordinator.outdoor_running


class SharedHeat(SupervisorEntity, BinarySensorEntity):
    """Another head (not this room's) is operating."""

    _attr_translation_key = "shared_heat"

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, f"{room_key(climate)}_shared_heat")
        self._climate = climate
        self._attr_translation_placeholders = {"room": room_key(climate)}

    @property
    def is_on(self) -> bool:
        room_plan = self.coordinator.plans.get(self._climate)
        return room_plan is not None and room_plan.shared_heat
