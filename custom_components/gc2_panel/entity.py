"""Shared GC2 entity behavior."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import Gc2Coordinator


class Gc2Entity(CoordinatorEntity[Gc2Coordinator]):
    """Base entity belonging to the selected physical GC2 panel."""

    _attr_has_entity_name = True

    @property
    def available(self) -> bool:
        """Report the bridge's MQTT last-will state."""
        return self.coordinator.data.available

    @property
    def device_info(self) -> DeviceInfo:
        """Group all GC2 data below one panel device."""
        manifest = self.coordinator.data.manifest
        device_id = str(manifest.get("device_id", self.coordinator.root_topic))
        return DeviceInfo(
            identifiers={(DOMAIN, device_id)},
            name=str(manifest.get("name", "2GIG GC2 Panel")),
            manufacturer="2GIG / Waveshare",
            model="GC2 via ESP32-S3 UART bridge",
            sw_version=manifest.get("bridge_firmware"),
        )
