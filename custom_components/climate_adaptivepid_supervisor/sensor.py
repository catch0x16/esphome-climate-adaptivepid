"""What the supervisor is asking each hinted room's head to do."""

from __future__ import annotations

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SupervisorConfigEntry
from .entity import SupervisorEntity, room_key

ACTIONS = ["none", "shared_heat", "shared_heat_expected", "disabled"]


async def async_setup_entry(
    hass: HomeAssistant, entry: SupervisorConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities([SupervisorAction(coordinator, climate) for climate in coordinator.hinted])


class SupervisorAction(SupervisorEntity, SensorEntity):
    """none, shared_heat or disabled; the reason as an attribute."""

    _attr_translation_key = "supervisor_action"
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_options = ACTIONS

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, f"{room_key(climate)}_supervisor_action")
        self._climate = climate
        self._attr_translation_placeholders = {"room": room_key(climate)}

    @property
    def native_value(self) -> str:
        if not self.coordinator.enabled:
            return "disabled"
        room_plan = self.coordinator.plans.get(self._climate)
        if room_plan is None:
            return "none"
        if room_plan.shared_heat:
            return "shared_heat"
        if room_plan.expected_in_s is not None:
            return "shared_heat_expected"
        return "none"

    @property
    def extra_state_attributes(self) -> dict[str, str | int | None]:
        room_plan = self.coordinator.plans.get(self._climate)
        return {
            "reason": room_plan.reason if room_plan else "room not in heat or cool",
            "climate": self._climate,
            "expected_in_s": room_plan.expected_in_s if room_plan else None,
        }
