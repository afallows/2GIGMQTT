"""Physical GC2 alarm control panel entity."""

from __future__ import annotations

from typing import Any

from homeassistant.components.alarm_control_panel import (
    AlarmControlPanelEntity,
    AlarmControlPanelEntityFeature,
)
from homeassistant.components.alarm_control_panel.const import AlarmControlPanelState
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import Gc2ConfigEntry
from .entity import Gc2Entity

_STATE_MAP = {
    "disarmed": AlarmControlPanelState.DISARMED,
    "arming": AlarmControlPanelState.ARMING,
    "armed_home": AlarmControlPanelState.ARMED_HOME,
    "armed_away": AlarmControlPanelState.ARMED_AWAY,
    "pending": AlarmControlPanelState.PENDING,
    "triggered": AlarmControlPanelState.TRIGGERED,
}


async def async_setup_entry(
    hass: HomeAssistant,
    entry: Gc2ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Create the single alarm entity backed by the actual GC2."""
    async_add_entities([Gc2Alarm(entry.runtime_data)])


class Gc2Alarm(Gc2Entity, AlarmControlPanelEntity):
    """Full Home Assistant control surface for the physical GC2."""

    _attr_name = None
    _attr_code_arm_required = False
    _attr_supported_features = (
        AlarmControlPanelEntityFeature.ARM_HOME
        | AlarmControlPanelEntityFeature.ARM_AWAY
    )

    def __init__(self, coordinator) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_alarm"
        )

    @property
    def alarm_state(self) -> AlarmControlPanelState | None:
        return _STATE_MAP.get(self.coordinator.data.panel_state)

    @property
    def changed_by(self) -> str | None:
        user = self.coordinator.data.panel.get("user")
        if user is None:
            return self.coordinator.data.panel.get("origin")
        return f"User {user} ({self.coordinator.data.panel.get('origin', 'unknown')})"

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return dict(self.coordinator.data.panel)

    async def async_alarm_arm_home(self, code: str | None = None) -> None:
        await self.coordinator.async_publish("panel/set", "ARM_HOME")

    async def async_alarm_arm_away(self, code: str | None = None) -> None:
        await self.coordinator.async_publish("panel/set", "ARM_AWAY")

    async def async_alarm_disarm(self, code: str | None = None) -> None:
        await self.coordinator.async_publish("panel/set", "DISARM")
