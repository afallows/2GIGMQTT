"""Automatically discovered GC2 zones."""

from __future__ import annotations

from typing import Any

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity import EntityCategory
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
    async_add_entities(
        [
            Gc2PanelSecuritySensor(
                coordinator, "panel_trouble", "Panel trouble",
                "trouble_summary", "active", BinarySensorDeviceClass.PROBLEM
            ),
            Gc2PanelSecuritySensor(
                coordinator, "panel_tamper", "Panel tamper",
                "security", "panel_tamper", BinarySensorDeviceClass.TAMPER
            ),
            Gc2PanelSecuritySensor(
                coordinator, "siren_tamper", "Siren tamper",
                "security", "siren_tamper", BinarySensorDeviceClass.TAMPER
            ),
            Gc2PanelSecuritySensor(
                coordinator, "rf_jam", "RF jamming",
                "security", "rf_jam", BinarySensorDeviceClass.PROBLEM
            ),
            Gc2PanelSecuritySensor(
                coordinator, "ac_loss", "AC power loss",
                "security", "ac_loss", BinarySensorDeviceClass.PROBLEM
            ),
            Gc2PanelSecuritySensor(
                coordinator, "communication_failure", "Communication failure",
                "security", "communication_failure", BinarySensorDeviceClass.PROBLEM
            ),
            Gc2PanelSecuritySensor(
                coordinator, "reset_required", "Panel reset required",
                "security", "reset_required", BinarySensorDeviceClass.PROBLEM
            ),
            Gc2PanelSecuritySensor(
                coordinator, "alarm_memory", "Latched alarm memory",
                "alarm_memory", "latched", BinarySensorDeviceClass.PROBLEM
            ),
        ]
    )
    known: set[int] = set()

    @callback
    def add_new_zones() -> None:
        new = set(coordinator.data.zones) - known
        if new:
            known.update(new)
            entities = []
            for number in sorted(new):
                entities.extend(
                    (
                        Gc2Zone(coordinator, number),
                        Gc2ZoneBattery(coordinator, number),
                        Gc2ZoneTrouble(coordinator, number, "trouble_active", "trouble"),
                        Gc2ZoneTrouble(coordinator, number, "tamper", "tamper"),
                        Gc2ZoneTrouble(
                            coordinator, number, "supervision_lost", "supervision loss"
                        ),
                    )
                )
            async_add_entities(entities)

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


class Gc2ZoneBattery(Gc2Entity, BinarySensorEntity):
    """Low-battery condition reported by one GC2 wireless zone."""

    _attr_device_class = BinarySensorDeviceClass.BATTERY
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator, number: int) -> None:
        super().__init__(coordinator)
        self.number = number
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_zone_{number:02d}_battery"
        )

    @property
    def zone(self) -> dict[str, Any]:
        return self.coordinator.data.zones.get(self.number, {})

    @property
    def name(self) -> str:
        return f"{self.zone.get('name') or f'Zone {self.number}'} battery"

    @property
    def available(self) -> bool:
        return super().available and bool(self.zone)

    @property
    def is_on(self) -> bool | None:
        if not self.zone.get("battery_known", False):
            return None
        return bool(self.zone.get("battery_low", False))

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {
            "zone": self.number,
            "rf_id": self.zone.get("rf_id"),
            "last_seen_ms": self.zone.get("last_seen_ms"),
        }


class Gc2ZoneTrouble(Gc2Entity, BinarySensorEntity):
    """One normalized security trouble for a panel zone."""

    _attr_device_class = BinarySensorDeviceClass.PROBLEM
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator, number: int, field: str, label: str) -> None:
        super().__init__(coordinator)
        self.number = number
        self.field = field
        self.label = label
        self._attr_unique_id = (
            f"{coordinator.root_topic.replace('/', '_')}_zone_{number:02d}_{field}"
        )

    @property
    def zone(self) -> dict[str, Any]:
        return self.coordinator.data.zones.get(self.number, {})

    @property
    def name(self) -> str:
        return f"{self.zone.get('name') or f'Zone {self.number}'} {self.label}"

    @property
    def available(self) -> bool:
        return super().available and bool(self.zone)

    @property
    def is_on(self) -> bool | None:
        if not self.zone.get("trouble_known", False):
            return None
        return bool(self.zone.get(self.field, False))

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {
            "zone": self.number,
            "summary": self.zone.get("trouble_summary"),
        }


class Gc2PanelSecuritySensor(Gc2Entity, BinarySensorEntity):
    """A retained panel-wide security condition."""

    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(
        self, coordinator, key: str, name: str, source: str, field: str,
        device_class: BinarySensorDeviceClass
    ) -> None:
        super().__init__(coordinator)
        self.source = source
        self.field = field
        self._attr_unique_id = f"{coordinator.root_topic.replace('/', '_')}_{key}"
        self._attr_name = name
        self._attr_device_class = device_class

    @property
    def snapshot(self) -> dict[str, Any]:
        return getattr(self.coordinator.data, self.source)

    @property
    def available(self) -> bool:
        return super().available and bool(self.snapshot.get("known", False))

    @property
    def is_on(self) -> bool | None:
        if not self.snapshot.get("known", False):
            return None
        return bool(self.snapshot.get(self.field, False))

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        if self.source == "trouble_summary":
            attributes = dict(self.snapshot)
            attributes["entries"] = [
                self.coordinator.data.troubles[slot]
                for slot in sorted(self.coordinator.data.troubles)
            ]
            return attributes
        return dict(self.snapshot)
