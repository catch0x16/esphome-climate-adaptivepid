"""Tracks the heads of one outdoor unit and sends hints to the hinted rooms."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any
from dataclasses import dataclass, replace
from datetime import timedelta
import logging

from homeassistant.components.logbook import async_log_entry
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import STATE_OFF, STATE_ON
from homeassistant.core import Event, EventStateChangedData, HomeAssistant, callback
from homeassistant.helpers import device_registry as dr, entity_registry as er, issue_registry as ir
from homeassistant.helpers.event import async_track_state_change_event, async_track_time_interval
from homeassistant.helpers.storage import Store
from homeassistant.util import dt as dt_util

from .const import (
    DOMAIN,
    ACTION_SHARED_HEAT,
    ACTION_SHARED_HEAT_EXPECTED,
    CONF_HEADS,
    CONF_HINTED,
    OPERATING_NAME,
    RENEW_SECONDS,
    SAVE_DELAY_SECONDS,
    SHARED_HEAT_VALID_SECONDS,
    STORAGE_VERSION,
)
from .logic import (
    CONFLICT_SUSTAIN_S,
    Head,
    ModeConflict,
    OperatingHold,
    RoomPlan,
    StartPredictor,
    StartStats,
    Sustained,
    mode_conflict,
    outdoor_unit_running,
    plan,
)

_LOGGER = logging.getLogger(__name__)

# Nothing sent to a room yet.
_UNSENT = object()


@dataclass
class HeadEntities:
    """The Home Assistant entities and ESPHome node of one head."""

    climate: str
    operating: str | None
    node: str | None


def _head_options(entry: ConfigEntry) -> tuple[list[str], list[str]]:
    data = {**entry.data, **entry.options}
    heads = list(data.get(CONF_HEADS, []))
    hinted = [h for h in data.get(CONF_HINTED, []) if h in heads]
    return heads, hinted


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


def make_store(hass: HomeAssistant, entry_id: str) -> Store[dict[str, Any]]:
    """Where one outdoor unit's learned starts and statistics are kept."""
    return Store(hass, STORAGE_VERSION, f"{DOMAIN}.{entry_id}")


class SupervisorCoordinator:
    """Event-driven: recomputes on any head change, and renews hints every minute."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self.hass = hass
        self.entry = entry
        self.heads, self.hinted = _head_options(entry)
        self.entities: dict[str, HeadEntities] = {}
        self.enabled = True
        self.outdoor_running = False
        self.plans: dict[str, RoomPlan] = {}
        self._sent: dict[str, bool] = {}
        # The forecast last sent to each room (None: cleared).
        self._sent_expected: dict[str, object] = {}
        self._no_expected_action: set[str] = set()
        self.predictors: dict[str, StartPredictor] = {}
        self.holds: dict[str, OperatingHold] = {}
        self.stats = StartStats()
        # A sustained mode conflict, as reported (None: none).
        self.mode_conflict: ModeConflict | None = None
        self._conflict = Sustained(CONFLICT_SUSTAIN_S)
        self._issues: set[str] = set()
        self._store = make_store(hass, entry.entry_id)
        self._listeners: list[Callable[[], None]] = []
        self._unsubs: list[Callable[[], None]] = []

    async def async_load(self) -> None:
        """Restores the learned starts and statistics of the configured heads."""
        try:
            data = await self._store.async_load()
        except Exception as err:  # noqa: BLE001 - a bad file must not stop the supervisor
            _LOGGER.warning("Could not read the saved starts; learning again: %s", err)
            data = None
        heads = data.get("heads") if isinstance(data, dict) else None
        if not isinstance(heads, dict):
            return
        for climate in self.heads:
            saved = heads.get(climate)
            if not isinstance(saved, dict):
                continue
            self.predictors.setdefault(climate, StartPredictor()).load(saved.get("predictor"))
            self.stats.head(climate).load(saved.get("stats"))

    def _issue_id(self, key: str) -> str:
        return f"{self.entry.entry_id}_{key}"

    def _create_issue(self, key: str, translation_key: str, **placeholders: str) -> None:
        issue_id = self._issue_id(key)
        ir.async_create_issue(
            self.hass,
            DOMAIN,
            issue_id,
            is_fixable=False,
            severity=ir.IssueSeverity.WARNING,
            translation_key=translation_key,
            translation_placeholders={"name": self.entry.title, **placeholders},
        )
        self._issues.add(issue_id)

    def _report_conflict(self, conflict: ModeConflict | None) -> None:
        """Raises or clears the mode conflict issue when it changes."""
        if conflict == self.mode_conflict:
            return
        self.mode_conflict = conflict
        if conflict is None:
            _LOGGER.info("Mode conflict cleared")
            ir.async_delete_issue(self.hass, DOMAIN, self._issue_id("mode_conflict"))
            self._issues.discard(self._issue_id("mode_conflict"))
            return
        heating, cooling = ", ".join(conflict.heating), ", ".join(conflict.cooling)
        message = f"mode conflict: {heating} heating, {cooling} cooling"
        _LOGGER.warning("%s: %s", self.entry.title, message)
        async_log_entry(self.hass, "Supervisor", message, domain=DOMAIN)
        self._create_issue("mode_conflict", "mode_conflict", heating=heating, cooling=cooling)

    def _data(self) -> dict[str, Any]:
        return {
            "heads": {
                climate: {
                    "predictor": self.predictors[climate].to_dict() if climate in self.predictors else {},
                    "stats": self.stats.head(climate).to_dict(),
                }
                for climate in self.heads
            }
        }

    async def async_start(self) -> None:
        for climate in self.heads:
            head = resolve_head(self.hass, climate)
            self.entities[climate] = head
            if head.operating is None:
                _LOGGER.warning("%s: no '%s' sensor found; it counts as not running", climate, OPERATING_NAME)
                self._create_issue(f"no_operating_{climate}", "no_operating_sensor", climate=climate)
            if climate in self.hinted and head.node is None:
                _LOGGER.warning("%s: no ESPHome device found; hints cannot be sent to it", climate)
                self._create_issue(f"no_esphome_{climate}", "no_esphome_device", climate=climate)
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
        # Raised again on the next start if they still apply.
        for issue_id in list(self._issues):
            ir.async_delete_issue(self.hass, DOMAIN, issue_id)
        self._issues.clear()
        await self._store.async_save(self._data())

    @callback
    def async_add_listener(self, listener: Callable[[], None]) -> Callable[[], None]:
        self._listeners.append(listener)

        def remove() -> None:
            self._listeners.remove(listener)

        return remove

    async def async_set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        self._sent.clear()
        self._sent_expected.clear()
        await self.async_update(renew=True)

    def snapshot(self) -> dict[str, Head]:
        heads: dict[str, Head] = {}
        for climate, entities in self.entities.items():
            state = self.hass.states.get(climate)
            heads[climate] = Head(
                name=state.name if state is not None else climate,
                operating=self._operating(entities),
                mode=state.state if state is not None else None,
                room=_float_attr(state, "current_temperature"),
                target=_float_attr(state, "temperature"),
            )
        return heads

    def _operating(self, entities: HeadEntities) -> bool | None:
        """On or off; None while the sensor is unavailable. A head without one counts as off."""
        if entities.operating is None:
            return False
        state = self.hass.states.get(entities.operating)
        if state is None or state.state not in (STATE_ON, STATE_OFF):
            return None
        return state.state == STATE_ON

    async def _on_state_change(self, event: Event[EventStateChangedData]) -> None:
        await self.async_update(renew=False)

    async def _on_renew(self, _now) -> None:
        await self.async_update(renew=True)

    async def async_update(self, renew: bool) -> None:
        reported = self.snapshot()
        now = dt_util.utcnow().timestamp()
        expected: dict[str, float | None] = {}
        heads: dict[str, Head] = {}
        for climate, head in reported.items():
            # The predictor sees gaps as gaps; the plan bridges short ones.
            predictor = self.predictors.setdefault(climate, StartPredictor())
            if predictor.observe(now, head):
                self.stats.start(now, climate)
                self._store.async_delay_save(self._data, SAVE_DELAY_SECONDS)
            hold = self.holds.setdefault(climate, OperatingHold())
            heads[climate] = replace(head, operating=hold.update(now, head.operating))
            expected[climate] = predictor.expected_in_s(now, heads[climate])
            self.stats.forecast(now, climate, expected[climate])
        self.outdoor_running = outdoor_unit_running(heads)
        conflict = mode_conflict(heads)
        sustained = self._conflict.update(now, conflict is not None)
        if sustained:
            self._report_conflict(conflict)
        elif conflict is None:
            self._report_conflict(None)
        self.plans = plan(heads, self.hinted, expected)
        if self.enabled:
            for climate, room_plan in self.plans.items():
                await self._send(climate, room_plan, renew)
                await self._send_expected(climate, room_plan, renew)
        for listener in list(self._listeners):
            listener()

    async def _send(self, climate: str, room_plan: RoomPlan, renew: bool) -> None:
        node = self.entities[climate].node
        if node is None:
            return
        key = room_plan.shared_heat
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
        except Exception as err:  # noqa: BLE001 - a head offline must not stop the others
            _LOGGER.warning("%s: could not send hints to %s: %s", climate, node, err)
            self._sent.pop(climate, None)
            return
        self._sent[climate] = key
        if changed:
            _LOGGER.info("%s: %s", climate, room_plan.reason)
            async_log_entry(self.hass, "Supervisor", room_plan.reason, domain=DOMAIN, entity_id=climate)

    async def _send_expected(self, climate: str, room_plan: RoomPlan, renew: bool) -> None:
        """The forecast: sent when it appears or clears, and renewed every minute."""
        node = self.entities[climate].node
        if node is None:
            return
        service = f"{node}_{ACTION_SHARED_HEAT_EXPECTED}"
        if not self.hass.services.has_service("esphome", service):
            if climate not in self._no_expected_action:
                self._no_expected_action.add(climate)
                _LOGGER.warning("%s: no esphome.%s action; forecasts are not sent to it", climate, service)
            return
        self._no_expected_action.discard(climate)
        value = room_plan.expected_in_s
        previous = self._sent_expected.get(climate, _UNSENT)
        # A new forecast or a cleared one goes out at once; a changed countdown
        # waits for the renewal (the head counts down by itself).
        changed = previous is _UNSENT or (previous is None) != (value is None)
        if not changed and not renew:
            return
        try:
            await self.hass.services.async_call(
                "esphome",
                service,
                {"expected_in_s": -1 if value is None else value, "valid_for_s": SHARED_HEAT_VALID_SECONDS},
                blocking=True,
            )
        except Exception as err:  # noqa: BLE001 - a head offline must not stop the others
            _LOGGER.warning("%s: could not send the forecast to %s: %s", climate, node, err)
            self._sent_expected.pop(climate, None)
            return
        self._sent_expected[climate] = value
        if changed and value is not None:
            _LOGGER.info("%s: %s", climate, room_plan.reason)
            async_log_entry(self.hass, "Supervisor", room_plan.reason, domain=DOMAIN, entity_id=climate)


def _float_attr(state, name: str) -> float | None:
    if state is None:
        return None
    try:
        value = state.attributes.get(name)
        return None if value is None else float(value)
    except (TypeError, ValueError):
        return None
