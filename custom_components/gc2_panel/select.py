"""Preset sounder volume and per-zone chime type for the physical GC2 panel."""

from __future__ import annotations

import json
from typing import Any

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity import EntityCategory
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

# GC2 console `zone_chime <zone> <type>` values, in the panel's own order.
CHIME_MODES: dict[str, int] = {
    "Off": 0,
    "Voice": 1,
    "Voice and ding-dong": 2,
    "Loud ding-dong": 3,
    "Voice and loud ding-dong": 4,
    "Ding-dong": 5,
}
_CHIME_MODE_LABELS = {value: key for key, value in CHIME_MODES.items()}


async def async_setup_entry(
    hass: HomeAssistant,
    entry: Gc2ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Create the volume preset selector and one chime selector per zone."""
    coordinator = entry.runtime_data
    async_add_entities([Gc2SounderVolumePreset(coordinator)])

    known: set[int] = set()

    @callback
    def add_new_zones() -> None:
        new = set(coordinator.data.zones) - known
        if new:
            known.update(new)
            async_add_entities(
                Gc2ZoneChime(coordinator, number) for number in sorted(new)
            )

    add_new_zones()
    entry.async_on_unload(coordinator.async_add_listener(add_new_zones))


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


class Gc2ZoneChime(Gc2Entity, SelectEntity):
    """Programmed chime type for one zone, written through the real panel."""

    _attr_icon = "mdi:bell-ring-outline"
    _attr_options = list(CHIME_MODES)
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, coordinator, number: int) -> None:
        super().__init__(coordinator)
        self.number = number
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_zone_{number:02d}_chime"
        )

    @property
    def zone(self) -> dict[str, Any]:
        return self.coordinator.data.zones.get(self.number, {})

    @property
    def name(self) -> str:
        return f"{self.zone.get('name') or f'Zone {self.number}'} chime"

    @property
    def available(self) -> bool:
        return super().available and bool(self.zone)

    @property
    def current_option(self) -> str | None:
        """Return the chime type read back from the panel's zone table."""
        if not self.zone.get("chime_known", False):
            return None
        return _CHIME_MODE_LABELS.get(self.zone.get("chime_mode"))

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {
            "zone": self.number,
            "bridge_topic": self.coordinator.root_topic,
            "chime_mode": self.zone.get("chime_mode"),
        }

    async def async_select_option(self, option: str) -> None:
        """Request a chime type; the bridge skips values that already match."""
        if option == self.current_option:
            return
        await self.coordinator.async_publish(
            "panel/zone_chime/set",
            json.dumps(
                {"zone": self.number, "mode": CHIME_MODES[option]},
                separators=(",", ":"),
            ),
        )
