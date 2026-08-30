"""Meaningful GC2 activity events for Home Assistant's activity card."""

from __future__ import annotations

from typing import Any

from homeassistant.components.event import EventEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import Gc2ConfigEntry
from .entity import Gc2Entity
from .models import (
    PANEL_ACTIVITY_EVENT_TYPES,
    ZONE_ACTIVITY_EVENT_TYPES,
    panel_activity_transition,
    zone_activity_transition,
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: Gc2ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Create the panel event feed and one event entity per programmed zone."""
    coordinator = entry.runtime_data
    async_add_entities([Gc2PanelActivity(coordinator)])
    known: set[int] = set()

    @callback
    def add_new_zones() -> None:
        new = set(coordinator.data.zones) - known
        if not new:
            return
        known.update(new)
        async_add_entities(
            [Gc2ZoneActivity(coordinator, number) for number in sorted(new)]
        )

    add_new_zones()
    entry.async_on_unload(coordinator.async_add_listener(add_new_zones))


class Gc2ActivityEvent(Gc2Entity, EventEntity):
    """Base event which remains available while retaining its last event."""

    @property
    def available(self) -> bool:
        """Avoid logging bridge availability recovery as security activity."""
        return True


class Gc2PanelActivity(Gc2ActivityEvent):
    """Meaningful arming-state changes from the GC2 panel."""

    _attr_event_types = list(PANEL_ACTIVITY_EVENT_TYPES)
    _attr_icon = "mdi:shield-home-outline"
    _attr_name = "Security system"

    def __init__(self, coordinator) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_panel_activity"
        )
        self._previous_state = coordinator.data.panel_state

    @callback
    def _handle_coordinator_update(self) -> None:
        current = self.coordinator.data.panel_state
        event_type = panel_activity_transition(self._previous_state, current)
        self._previous_state = current
        if event_type is None:
            return
        self._trigger_event(event_type, {"state": current})
        self.async_write_ha_state()


class Gc2ZoneActivity(Gc2ActivityEvent):
    """Meaningful open/close, alarm, and restore changes for one GC2 zone."""

    _attr_event_types = list(ZONE_ACTIVITY_EVENT_TYPES)
    _attr_icon = "mdi:shield-sensor-outline"

    def __init__(self, coordinator, number: int) -> None:
        super().__init__(coordinator)
        self.number = number
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_zone_{number:02d}_activity"
        )
        self._previous_state = self.zone.get("state")

    @property
    def zone(self) -> dict[str, Any]:
        return self.coordinator.data.zones.get(self.number, {})

    @property
    def name(self) -> str:
        return self.zone.get("name") or f"Zone {self.number}"

    @callback
    def _handle_coordinator_update(self) -> None:
        zone = self.zone
        current = zone.get("state")
        event_type = zone_activity_transition(zone, self._previous_state, current)
        self._previous_state = current
        if event_type is None:
            return
        self._trigger_event(
            event_type,
            {
                "zone": self.number,
                "zone_name": self.name,
                "state": current,
            },
        )
        self.async_write_ha_state()
