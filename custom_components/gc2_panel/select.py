"""Preset chime and announcement volume for the physical GC2 panel."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import Gc2ConfigEntry
from .entity import Gc2Entity

VOLUME_PRESETS: dict[str, int] = {
    "0%": 0,
    "10%": 10,
    "25%": 25,
    "50%": 50,
    "75%": 75,
    "100%": 100,
}


async def async_setup_entry(
    hass: HomeAssistant,
    entry: Gc2ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Create the GC2 chime/announcement-volume preset selector."""
    async_add_entities([Gc2SounderVolumePreset(entry.runtime_data)])


class Gc2SounderVolumePreset(Gc2Entity, SelectEntity):
    """Select a normalized panel sounder-volume preset."""

    _attr_name = "Chime and announcement volume preset"
    _attr_icon = "mdi:volume-medium"
    _attr_options = list(VOLUME_PRESETS)

    def __init__(self, coordinator) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_sounder_volume_preset"
        )

    @property
    def current_option(self) -> str | None:
        """Return the preset confirmed by the panel console."""
        volume = self.coordinator.data.sounder_volume
        if volume is None:
            return None
        option = f"{volume}%"
        return option if option in VOLUME_PRESETS else None

    async def async_select_option(self, option: str) -> None:
        """Request a preset without assuming the panel accepted it."""
        await self.coordinator.async_publish(
            "panel/sounder_volume/set", str(VOLUME_PRESETS[option])
        )
