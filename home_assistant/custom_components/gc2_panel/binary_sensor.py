"""Automatically discovered GC2 zones."""

from __future__ import annotations

from typing import Any

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import Gc2ConfigEntry
from .entity import Gc2Entity


def _device_class(zone: dict[str, Any]) -> BinarySensorDeviceClass:
    text = f"{zone.get('name', '')} {zone.get('zone_type', '')}".lower()
    if "carbon monoxide" in text:
        return BinarySensorDeviceClass.CO
    if "smoke" in text or "fire" in text:
        return BinarySensorDeviceClass.SMOKE
    if "water" in text or "flood" in text:
        return BinarySensorDeviceClass.MOISTURE
    if "gas" in text:
        return BinarySensorDeviceClass.GAS
    if "motion" in text or "interior" in text:
        return BinarySensorDeviceClass.MOTION
    if "window" in text:
        return BinarySensorDeviceClass.WINDOW
    if "garage" in text:
        return BinarySensorDeviceClass.GARAGE_DOOR
    if any(value in text for value in ("door", "entry", "gate")):
        return BinarySensorDeviceClass.DOOR
    return BinarySensorDeviceClass.OPENING


async def async_setup_entry(
    hass: HomeAssistant,
    entry: Gc2ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    known: set[int] = set()

    @callback
    def add_new_zones() -> None:
        new = set(coordinator.data.zones) - known
        if new:
            known.update(new)
            async_add_entities(Gc2Zone(coordinator, number) for number in sorted(new))

    add_new_zones()
    entry.async_on_unload(coordinator.async_add_listener(add_new_zones))


class Gc2Zone(Gc2Entity, BinarySensorEntity):
    """One panel-programmed GC2 zone."""

    def __init__(self, coordinator, number: int) -> None:
        super().__init__(coordinator)
        self.number = number
        self._attr_unique_id = f"{coordinator.root_topic.replace('/', '_')}_zone_{number:02d}"

    @property
    def name(self) -> str:
        return self.zone.get("name") or f"Zone {self.number}"

    @property
    def zone(self) -> dict[str, Any]:
        return self.coordinator.data.zones.get(self.number, {})

    @property
    def available(self) -> bool:
        return super().available and bool(self.zone)

    @property
    def is_on(self) -> bool | None:
        state = self.zone.get("state")
        return state == "ON" if state in ("ON", "OFF") else None

    @property
    def device_class(self) -> BinarySensorDeviceClass:
        return _device_class(self.zone)

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {key: value for key, value in self.zone.items() if key != "state"}
