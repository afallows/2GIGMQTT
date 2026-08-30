"""Chime and announcement volume for the physical GC2 panel."""

from __future__ import annotations

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.const import PERCENTAGE
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import Gc2ConfigEntry
from .entity import Gc2Entity


async def async_setup_entry(
    hass: HomeAssistant,
    entry: Gc2ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Create the GC2 chime/announcement-volume slider."""
    async_add_entities([Gc2SounderVolume(entry.runtime_data)])


class Gc2SounderVolume(Gc2Entity, NumberEntity):
    """Control the panel's normalized chime and announcement volume."""

    _attr_name = "Chime and announcement volume"
    _attr_icon = "mdi:volume-high"
    _attr_native_min_value = 0
    _attr_native_max_value = 100
    _attr_native_step = 1
    _attr_native_unit_of_measurement = PERCENTAGE
    _attr_mode = NumberMode.SLIDER

    def __init__(self, coordinator) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_sounder_volume"
        )

    @property
    def native_value(self) -> float | None:
        """Return the volume confirmed by the panel console."""
        return self.coordinator.data.sounder_volume

    async def async_set_native_value(self, value: float) -> None:
        """Request a volume change without assuming it succeeded."""
        await self.coordinator.async_publish(
            "panel/sounder_volume/set", str(round(value))
        )
