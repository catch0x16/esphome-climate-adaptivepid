"""Supervisor for climate_adaptivepid heads that share one outdoor unit.

Coordination only: it sends an expiring shared-heat hint to the heads through
their ESPHome action. Every head keeps working on its own when
the supervisor is stopped; the hints simply expire. See
docs/supervisor-design.md in esphome-climate-adaptivepid.
"""

from __future__ import annotations

import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant

from .const import CONF_HEADS, CONF_HINTED, PLATFORMS
from .coordinator import SupervisorCoordinator, make_store

_LOGGER = logging.getLogger(__name__)

type SupervisorConfigEntry = ConfigEntry[SupervisorCoordinator]


def _precedence_v2(values: dict[str, Any]) -> dict[str, Any]:
    """Version 1 ranked heads without hints first, then the hinted rooms in their
    own order. From version 2 the order of the heads is the precedence, so they
    are put in that order."""
    if CONF_HEADS not in values:
        return values
    heads = list(values[CONF_HEADS])
    hinted = [h for h in values.get(CONF_HINTED, []) if h in heads]
    return {
        **values,
        CONF_HEADS: [h for h in heads if h not in hinted] + hinted,
        CONF_HINTED: hinted,
    }


async def async_migrate_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    if entry.version > 2:
        return False
    if entry.version == 1:
        hass.config_entries.async_update_entry(
            entry, data=_precedence_v2(dict(entry.data)), options=_precedence_v2(dict(entry.options)), version=2
        )
        _LOGGER.info("Migrated %s: the order of the heads is now their precedence", entry.title)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: SupervisorConfigEntry) -> bool:
    coordinator = SupervisorCoordinator(hass, entry)
    entry.runtime_data = coordinator
    # Before the entities exist, so the restored counts are their first state.
    await coordinator.async_load()
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    await coordinator.async_start()
    entry.async_on_unload(entry.add_update_listener(_async_reload))
    return True


async def async_unload_entry(hass: HomeAssistant, entry: SupervisorConfigEntry) -> bool:
    await entry.runtime_data.async_stop()
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)


async def _async_reload(hass: HomeAssistant, entry: SupervisorConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def async_remove_entry(hass: HomeAssistant, entry: SupervisorConfigEntry) -> None:
    await make_store(hass, entry.entry_id).async_remove()
