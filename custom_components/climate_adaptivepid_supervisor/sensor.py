"""What the supervisor asks each hinted room's head to do, and how every head's starts went."""

from __future__ import annotations

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import PERCENTAGE, EntityCategory, UnitOfTime
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SupervisorConfigEntry
from .entity import SupervisorEntity, room_key
from .logic import (
    HIT_WITHIN_S,
    RECOVERY_MIN_GAP,
    RECOVERY_REACHED,
    SCORE_HISTORY,
    SCORE_LEAD_S,
    STACK_WINDOW_S,
    HeadStats,
)

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
            RecoveryTime(coordinator, climate),
            RecoveryForecastError(coordinator, climate),
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


class _Count(HeadStatsEntity):
    """A count that only goes up; the coordinator keeps it across restarts."""

    _attr_state_class = SensorStateClass.TOTAL_INCREASING
    _field: str

    @property
    def native_value(self) -> int:
        return getattr(self.stats, self._field)


class HeadStarts(_Count):
    """Every start of the head (an off to on edge of Operating)."""

    _attr_translation_key = "starts"
    _field = "starts"

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, climate, "starts")


class HeadStackedStarts(_Count):
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


class RecoveryTime(HeadStatsEntity):
    """Minutes until the room is at target at full output, from the head's room model."""

    _attr_translation_key = "recovery_time"
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = UnitOfTime.MINUTES
    _attr_suggested_display_precision = 0

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, climate, "recovery_time")

    @property
    def native_value(self) -> float | None:
        minutes = self.coordinator.recovery_estimates.get(self._climate)
        return None if minutes is None else round(minutes, 1)

    @property
    def extra_state_attributes(self) -> dict[str, float | str | None]:
        model = self.coordinator.models.get(self._climate)
        return {
            "outdoor": self.coordinator.outdoor,
            "heating_rate": model.heat_rate if model else None,
            "cooling_rate": model.cool_rate if model else None,
            "heat_loss": model.loss if model else None,
            "dead_time_heating": model.heat_dead_min if model else None,
            "dead_time_cooling": model.cool_dead_min if model else None,
            "within": RECOVERY_REACHED,
        }


class RecoveryForecastError(HeadStatsEntity):
    """The last timed recovery's actual minus predicted minutes."""

    _attr_translation_key = "recovery_forecast_error"
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = UnitOfTime.MINUTES
    _attr_suggested_display_precision = 0

    def __init__(self, coordinator, climate: str) -> None:
        super().__init__(coordinator, climate, "recovery_forecast_error")

    @property
    def tracker(self):
        return self.coordinator.recoveries.get(self._climate)

    @property
    def native_value(self) -> float | None:
        last = self.tracker.last if self.tracker else None
        return None if last is None else round(last.error_min, 1)

    @property
    def extra_state_attributes(self) -> dict[str, float | int | None]:
        tracker = self.tracker
        last = tracker.last if tracker else None
        active = tracker.active if tracker else None
        return {
            "predicted_min": None if last is None else round(last.predicted_min, 1),
            "actual_min": None if last is None else round(last.actual_min, 1),
            "recoveries": len(tracker.results) if tracker else 0,
            "mean_abs_error_min": tracker.mean_abs_error_min if tracker else None,
            "timing_predicted_min": None if active is None else round(active[2], 1),
            "min_gap": RECOVERY_MIN_GAP,
        }
