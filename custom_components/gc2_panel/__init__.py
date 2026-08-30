"""Home Assistant front end for a physical 2GIG GC2 panel."""

from __future__ import annotations

import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr, entity_registry as er

from .const import CONF_DEVICE_ID, CONF_ROOT_TOPIC, DOMAIN, PLATFORMS
from .coordinator import Gc2Coordinator

type Gc2ConfigEntry = ConfigEntry[Gc2Coordinator]

_LOGGER = logging.getLogger(__name__)


def _remove_empty_legacy_devices(
    hass: HomeAssistant, entry: Gc2ConfigEntry
) -> None:
    """Remove obsolete pre-MAC GC2 devices after their entities migrate."""
    device_registry = dr.async_get(hass)
    entity_registry = er.async_get(hass)
    current_identifier = (DOMAIN, str(entry.data[CONF_DEVICE_ID]))

    for device in dr.async_entries_for_config_entry(device_registry, entry.entry_id):
        if current_identifier in device.identifiers:
            continue
        if not any(identifier[0] == DOMAIN for identifier in device.identifiers):
            continue
        if er.async_entries_for_device(
            entity_registry, device.id, include_disabled_entities=True
        ):
            continue
        _LOGGER.info(
            "Removing empty legacy GC2 device %s with identifiers %s",
            device.name,
            device.identifiers,
        )
        device_registry.async_remove_device(device.id)


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
    _remove_empty_legacy_devices(hass, entry)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: Gc2ConfigEntry) -> bool:
    """Unload the panel and its dynamic entities."""
    if not await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        return False
    await entry.runtime_data.async_stop()
    return True
