"""Tracks the heads of one outdoor unit and sends hints to the hinted rooms."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from datetime import timedelta
import logging

from homeassistant.components.climate import ATTR_CURRENT_TEMPERATURE
from homeassistant.components.logbook import async_log_entry
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import ATTR_TEMPERATURE, STATE_ON
from homeassistant.core import Event, EventStateChangedData, HomeAssistant, callback
from homeassistant.helpers import device_registry as dr, entity_registry as er
from homeassistant.helpers.event import async_track_state_change_event, async_track_time_interval

from .const import (
    DOMAIN,
    ACTION_REQUEST_PARK,
    ACTION_SHARED_HEAT,
    CONF_HEADS,
    CONF_HINTED,
    CONF_PARK_MARGIN,
    DEFAULT_PARK_MARGIN,
    OPERATING_NAME,
    PARK_VALID_SECONDS,
    RENEW_SECONDS,
    SHARED_HEAT_VALID_SECONDS,
)
from .logic import Head, RoomPlan, outdoor_unit_running, plan

_LOGGER = logging.getLogger(__name__)


@dataclass
class HeadEntities:
    """The Home Assistant entities and ESPHome node of one head."""

    climate: str
    operating: str | None
    node: str | None


def _head_options(entry: ConfigEntry) -> tuple[list[str], list[str], float]:
    data = {**entry.data, **entry.options}
    heads = list(data.get(CONF_HEADS, []))
    hinted = [h for h in data.get(CONF_HINTED, []) if h in heads]
    return heads, hinted, float(data.get(CONF_PARK_MARGIN, DEFAULT_PARK_MARGIN))


def resolve_head(hass: HomeAssistant, climate_entity_id: str) -> HeadEntities:
    """Find a head's `Operating` sensor and ESPHome node name from its climate."""
    ent_reg = er.async_get(hass)
    entry = ent_reg.async_get(climate_entity_id)
    operating: str | None = None
    node: str | None = None
    if entry is not None and entry.device_id is not None:
        for other in er.async_entries_for_device(ent_reg, entry.device_id):
            if other.domain == "binary_sensor" and (other.original_name or "") == OPERATING_NAME:
                operating = other.entity_id
        device = dr.async_get(hass).async_get(entry.device_id)
        if device is not None:
            # A device belongs to one config entry (`config_entry_id`); older
            # Home Assistant only has the `config_entries` set.
            if hasattr(device, "config_entry_id"):
                config_entry_ids = [device.config_entry_id]
            else:
                config_entry_ids = list(device.config_entries)
            for config_entry_id in config_entry_ids:
                config_entry = hass.config_entries.async_get_entry(config_entry_id) if config_entry_id else None
                if config_entry is not None and config_entry.domain == "esphome":
                    node = config_entry.data.get("device_name")
    return HeadEntities(climate=climate_entity_id, operating=operating, node=node)


class SupervisorCoordinator:
    """Event-driven: recomputes on any head change, and renews hints every minute."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self.hass = hass
        self.entry = entry
        self.heads, self.hinted, self.park_margin = _head_options(entry)
        self.entities: dict[str, HeadEntities] = {}
        self.enabled = True
        self.outdoor_running = False
        self.plans: dict[str, RoomPlan] = {}
        self._sent: dict[str, tuple[bool, bool | None]] = {}
        self._listeners: list[Callable[[], None]] = []
        self._unsubs: list[Callable[[], None]] = []

    async def async_start(self) -> None:
        for climate in self.heads:
            head = resolve_head(self.hass, climate)
            self.entities[climate] = head
            if head.operating is None:
                _LOGGER.warning("%s: no '%s' sensor found; it counts as not running", climate, OPERATING_NAME)
            if climate in self.hinted and head.node is None:
                _LOGGER.warning("%s: no ESPHome device found; hints cannot be sent to it", climate)
        tracked = [e for h in self.entities.values() for e in (h.climate, h.operating) if e]
        self._unsubs.append(async_track_state_change_event(self.hass, tracked, self._on_state_change))
        self._unsubs.append(
            async_track_time_interval(self.hass, self._on_renew, timedelta(seconds=RENEW_SECONDS))
        )
        await self.async_update(renew=True)

    async def async_stop(self) -> None:
        for unsub in self._unsubs:
            unsub()
        self._unsubs.clear()

    @callback
    def async_add_listener(self, listener: Callable[[], None]) -> Callable[[], None]:
        self._listeners.append(listener)

        def remove() -> None:
            self._listeners.remove(listener)

        return remove

    async def async_set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        self._sent.clear()
        await self.async_update(renew=True)

    def snapshot(self) -> dict[str, Head]:
        heads: dict[str, Head] = {}
        for climate, entities in self.entities.items():
            state = self.hass.states.get(climate)
            operating_state = self.hass.states.get(entities.operating) if entities.operating else None

            def number(attribute: str, state=state) -> float | None:
                if state is None:
                    return None
                try:
                    return float(state.attributes.get(attribute))
                except (TypeError, ValueError):
                    return None

            heads[climate] = Head(
                name=state.name if state is not None else climate,
                operating=operating_state is not None and operating_state.state == STATE_ON,
                mode=state.state if state is not None else None,
                room=number(ATTR_CURRENT_TEMPERATURE),
                target=number(ATTR_TEMPERATURE),
            )
        return heads

    async def _on_state_change(self, event: Event[EventStateChangedData]) -> None:
        await self.async_update(renew=False)

    async def _on_renew(self, _now) -> None:
        await self.async_update(renew=True)

    async def async_update(self, renew: bool) -> None:
        heads = self.snapshot()
        self.outdoor_running = outdoor_unit_running(heads)
        self.plans = plan(heads, self.hinted, self.park_margin)
        if self.enabled:
            for climate, room_plan in self.plans.items():
                await self._send(climate, room_plan, renew)
        for listener in list(self._listeners):
            listener()

    async def _send(self, climate: str, room_plan: RoomPlan, renew: bool) -> None:
        node = self.entities[climate].node
        if node is None:
            return
        key = (room_plan.shared_heat, room_plan.park)
        changed = self._sent.get(climate) != key
        if not changed and not renew:
            return
        try:
            await self.hass.services.async_call(
                "esphome",
                f"{node}_{ACTION_SHARED_HEAT}",
                {"active": room_plan.shared_heat, "valid_for_s": SHARED_HEAT_VALID_SECONDS},
                blocking=True,
            )
            if room_plan.park is not None:
                await self.hass.services.async_call(
                    "esphome",
                    f"{node}_{ACTION_REQUEST_PARK}",
                    {"park": room_plan.park, "valid_for_s": PARK_VALID_SECONDS},
                    blocking=True,
                )
        except Exception as err:  # noqa: BLE001 - a head offline must not stop the others
            _LOGGER.warning("%s: could not send hints to %s: %s", climate, node, err)
            self._sent.pop(climate, None)
            return
        self._sent[climate] = key
        if changed:
            _LOGGER.info("%s: %s", climate, room_plan.reason)
            async_log_entry(self.hass, "Supervisor", room_plan.reason, domain=DOMAIN, entity_id=climate)
