"""Home Assistant front end for a physical 2GIG GC2 panel."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant

from .const import CONF_DEVICE_ID, CONF_ROOT_TOPIC, PLATFORMS
from .coordinator import Gc2Coordinator

type Gc2ConfigEntry = ConfigEntry[Gc2Coordinator]


async def async_setup_entry(hass: HomeAssistant, entry: Gc2ConfigEntry) -> bool:
    """Set up a selected GC2 MQTT root."""
    coordinator = Gc2Coordinator(
        hass,
        entry.data[CONF_ROOT_TOPIC],
        entry.data[CONF_DEVICE_ID],
        entry.title,
    )
    await coordinator.async_start()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: Gc2ConfigEntry) -> bool:
    """Unload the panel and its dynamic entities."""
    if not await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        return False
    await entry.runtime_data.async_stop()
    return True
