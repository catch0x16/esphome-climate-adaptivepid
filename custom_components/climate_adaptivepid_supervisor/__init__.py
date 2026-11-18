"""Supervisor for climate_adaptivepid heads that share one outdoor unit.

Coordination only: it sends an expiring shared-heat hint to the heads through
their ESPHome action. Every head keeps working on its own when
the supervisor is stopped; the hints simply expire. See
docs/supervisor-design.md in esphome-climate-adaptivepid.
"""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant

from .const import PLATFORMS
from .coordinator import SupervisorCoordinator

type SupervisorConfigEntry = ConfigEntry[SupervisorCoordinator]


async def async_setup_entry(hass: HomeAssistant, entry: SupervisorConfigEntry) -> bool:
    coordinator = SupervisorCoordinator(hass, entry)
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    await coordinator.async_start()
    entry.async_on_unload(entry.add_update_listener(_async_reload))
    return True


async def async_unload_entry(hass: HomeAssistant, entry: SupervisorConfigEntry) -> bool:
    await entry.runtime_data.async_stop()
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)


async def _async_reload(hass: HomeAssistant, entry: SupervisorConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)
