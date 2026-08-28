"""Panel information, diagnostics, and automatically observed GC2 users."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import EntityCategory, UnitOfElectricPotential
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import Gc2ConfigEntry
from .entity import Gc2Entity
from .models import Gc2Snapshot


@dataclass(frozen=True, slots=True)
class Gc2SensorSpec:
    key: str
    name: str
    value: Callable[[Gc2Snapshot], Any]
    attributes: Callable[[Gc2Snapshot], dict[str, Any]] | None = None
    device_class: SensorDeviceClass | None = None
    unit: str | None = None
    state_class: SensorStateClass | None = None
    category: EntityCategory | None = EntityCategory.DIAGNOSTIC


_SENSORS = (
    Gc2SensorSpec("battery_state", "Backup battery", lambda data: data.battery.get("state"), lambda data: dict(data.battery)),
    Gc2SensorSpec(
        "battery_voltage",
        "Backup battery voltage",
        lambda data: data.battery.get("millivolts"),
        device_class=SensorDeviceClass.VOLTAGE,
        unit=UnitOfElectricPotential.MILLIVOLT,
        state_class=SensorStateClass.MEASUREMENT,
    ),
    Gc2SensorSpec("panel_firmware", "Panel firmware", lambda data: data.firmware.get("version"), lambda data: dict(data.firmware)),
    Gc2SensorSpec("uart_baud", "UART baud", lambda data: data.baud),
    Gc2SensorSpec("last_action_origin", "Last alarm action origin", lambda data: data.panel.get("origin"), lambda data: dict(data.panel)),
    Gc2SensorSpec("last_action_user", "Last alarm user ID", lambda data: data.panel.get("user")),
    Gc2SensorSpec("command_status", "Command status", lambda data: data.command_status.get("status"), lambda data: dict(data.command_status)),
    Gc2SensorSpec("debug_unlock", "Debug unlock", lambda data: data.diagnostics.get("debug_unlock")),
    Gc2SensorSpec("received_lines", "Console lines", lambda data: data.diagnostics.get("received_lines"), state_class=SensorStateClass.TOTAL_INCREASING),
    Gc2SensorSpec("unknown_lines", "Unparsed console lines", lambda data: data.diagnostics.get("unknown_lines"), state_class=SensorStateClass.TOTAL_INCREASING),
    Gc2SensorSpec("rf_supervision", "RF supervisory packets", lambda data: data.diagnostics.get("rf_supervision"), state_class=SensorStateClass.TOTAL_INCREASING),
    Gc2SensorSpec("zwave_ack", "Z-Wave acknowledgements", lambda data: data.diagnostics.get("zwave_ack"), state_class=SensorStateClass.TOTAL_INCREASING),
    Gc2SensorSpec("zwave_no_ack", "Z-Wave no acknowledgement", lambda data: data.diagnostics.get("zwave_no_ack"), state_class=SensorStateClass.TOTAL_INCREASING),
    Gc2SensorSpec("zwave_max_retry", "Z-Wave maximum retries", lambda data: data.diagnostics.get("zwave_max_retry"), state_class=SensorStateClass.TOTAL_INCREASING),
    Gc2SensorSpec("dropped_events", "Dropped bridge events", lambda data: data.diagnostics.get("dropped_events"), state_class=SensorStateClass.TOTAL_INCREASING),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: Gc2ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(Gc2ValueSensor(coordinator, spec) for spec in _SENSORS)

    known_users: set[int] = set()

    @callback
    def add_new_users() -> None:
        new = set(coordinator.data.users) - known_users
        if new:
            known_users.update(new)
            async_add_entities(
                Gc2UserSensor(coordinator, user_id) for user_id in sorted(new)
            )

    add_new_users()
    entry.async_on_unload(coordinator.async_add_listener(add_new_users))


class Gc2ValueSensor(Gc2Entity, SensorEntity):
    """A value projected from the complete panel snapshot."""

    def __init__(self, coordinator, spec: Gc2SensorSpec) -> None:
        super().__init__(coordinator)
        self.spec = spec
        prefix = coordinator.root_topic.replace("/", "_")
        self._attr_unique_id = f"{prefix}_{spec.key}"
        self._attr_name = spec.name
        self._attr_device_class = spec.device_class
        self._attr_native_unit_of_measurement = spec.unit
        self._attr_state_class = spec.state_class
        self._attr_entity_category = spec.category

    @property
    def native_value(self) -> Any:
        return self.spec.value(self.coordinator.data)

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        if self.spec.attributes is None:
            return None
        return self.spec.attributes(self.coordinator.data)


class Gc2UserSensor(Gc2Entity, SensorEntity):
    """An automatically observed, non-secret GC2 user ID."""

    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator, user_id: int) -> None:
        super().__init__(coordinator)
        self.user_id = user_id
        prefix = coordinator.root_topic.replace("/", "_")
        self._attr_unique_id = f"{prefix}_user_{user_id:03d}"

    @property
    def user(self) -> dict[str, Any]:
        return self.coordinator.data.users.get(self.user_id, {})

    @property
    def name(self) -> str:
        if self.user_id == 0:
            return "Remote / system user"
        return f"User {self.user_id}"

    @property
    def available(self) -> bool:
        return super().available and bool(self.user)

    @property
    def native_value(self) -> str | None:
        return self.user.get("last_action")

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return dict(self.user)
