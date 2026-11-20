"""What the supervisor asks each hinted room's head to do, and how every head's starts went."""

from __future__ import annotations

from homeassistant.components.sensor import RestoreSensor, SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import PERCENTAGE, EntityCategory, UnitOfTime
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SupervisorConfigEntry
from .entity import SupervisorEntity, room_key
from .logic import HIT_WITHIN_S, SCORE_HISTORY, SCORE_LEAD_S, STACK_WINDOW_S, HeadStats

ACTIONS = ["none", "shared_heat", "shared_heat_expected", "disabled"]


async def async_setup_entry(
    hass: HomeAssistant, entry: SupervisorConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    entities: list[SensorEntity] = [SupervisorAction(coordinator, climate) for climate in coordinator.hinted]
    for climate in coordinator.heads:
        entities += [
            HeadStarts(coordinator, climate),
            HeadStackedStarts(coordinator, climate),
            StartForecastError(coordinator, climate),
            StartForecastHitRate(coordinator, climate),
        ]
    async_add_entities(entities)


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


class HeadStatsEntity(SupervisorEntity, SensorEntity):
    """One head's start statistics (diagnostic)."""

    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator, climate: str, key: str) -> None:
        super().__init__(coordinator, f"{room_key(climate)}_{key}")
        self._climate = climate
        self._attr_translation_placeholders = {"room": room_key(climate)}

    @property
    def stats(self) -> HeadStats:
        return self.coordinator.stats.head(self._climate)


class _RestoredCount(HeadStatsEntity, RestoreSensor):
    """A count that only goes up, carried across restarts."""

    _attr_state_class = SensorStateClass.TOTAL_INCREASING
    _field: str

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        last = await self.async_get_last_sensor_data()
        if last is None or last.native_value is None:
            return
        try:
            restored = int(float(last.native_value))
        except (TypeError, ValueError):
            return
        setattr(self.stats, self._field, getattr(self.stats, self._field) + restored)

    @property
    def native_value(self) -> int:
        return getattr(self.stats, self._field)


class HeadStarts(_RestoredCount):
    """Every start of the head (an off to on edge of Operating)."""

    _attr_translation_key = "starts"
    _field = "starts"

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, climate, "starts")


class HeadStackedStarts(_RestoredCount):
    """Starts within STACK_WINDOW_S of another head's start, before or after."""

    _attr_translation_key = "stacked_starts"
    _field = "stacked"

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, climate, "stacked_starts")

    @property
    def extra_state_attributes(self) -> dict[str, float]:
        return {"window_s": STACK_WINDOW_S}


class StartForecastError(HeadStatsEntity):
    """Actual minus forecast start of the last start; unknown if it was not forecast."""

    _attr_translation_key = "start_forecast_error"
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = UnitOfTime.SECONDS

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, climate, "start_forecast_error")

    @property
    def native_value(self) -> int | None:
        error = self.stats.last_error_s
        return None if error is None else round(error)

    @property
    def extra_state_attributes(self) -> dict[str, float]:
        return {"lead_s": SCORE_LEAD_S}


class StartForecastHitRate(HeadStatsEntity):
    """Recent forecast starts that came within HIT_WITHIN_S of the forecast."""

    _attr_translation_key = "start_forecast_hit_rate"
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = PERCENTAGE

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, climate, "start_forecast_hit_rate")

    @property
    def native_value(self) -> float | None:
        return self.stats.hit_rate

    @property
    def extra_state_attributes(self) -> dict[str, float | int]:
        return {
            "forecast_starts": len(self.stats.errors),
            "unforecast_starts": self.stats.unforecast,
            "within_s": HIT_WITHIN_S,
            "lead_s": SCORE_LEAD_S,
            "history": SCORE_HISTORY,
        }
