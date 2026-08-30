"""Physical GC2 zone bypass controls."""

from __future__ import annotations

import json
from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import Gc2ConfigEntry
from .entity import Gc2Entity


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
            async_add_entities(
                Gc2ZoneBypass(coordinator, number) for number in sorted(new)
            )

    add_new_zones()
    entry.async_on_unload(coordinator.async_add_listener(add_new_zones))


class Gc2ZoneBypass(Gc2Entity, SwitchEntity):
    """Bypass a zone in the real panel, using its protected command path."""

    def __init__(self, coordinator, number: int) -> None:
        super().__init__(coordinator)
        self.number = number
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_zone_{number:02d}_bypass"
        )

    @property
    def zone(self) -> dict[str, Any]:
        return self.coordinator.data.zones.get(self.number, {})

    @property
    def name(self) -> str:
        return f"{self.zone.get('name') or f'Zone {self.number}'} bypass"

    @property
    def available(self) -> bool:
        return super().available and bool(self.zone)

    @property
    def is_on(self) -> bool | None:
        if not self.zone.get("bypass_known", False):
            return None
        return bool(self.zone.get("bypassed", False))

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {
            "zone": self.number,
            "bridge_topic": self.coordinator.root_topic,
            "origin": self.zone.get("bypass_origin"),
            "user": self.zone.get("bypass_user"),
            "type": self.zone.get("bypass_type"),
        }

    async def _set_bypass(self, bypassed: bool) -> None:
        await self.coordinator.async_publish(
            "panel/bypass/set",
            json.dumps({"zone": self.number, "bypassed": bypassed}, separators=(",", ":")),
        )

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self._set_bypass(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._set_bypass(False)
