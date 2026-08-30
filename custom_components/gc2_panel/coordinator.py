"""MQTT transport coordinator for GC2 Panel."""

from __future__ import annotations

import logging
from typing import Any

from homeassistant.components import mqtt
from homeassistant.components.mqtt.models import ReceiveMessage
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator

from .models import Gc2Snapshot

_LOGGER = logging.getLogger(__name__)


class Gc2Coordinator(DataUpdateCoordinator[Gc2Snapshot]):
    """Condition retained GC2 MQTT topics into one panel snapshot."""

    def __init__(
        self,
        hass: HomeAssistant,
        root_topic: str,
        device_id: str,
        device_name: str,
    ) -> None:
        super().__init__(hass, _LOGGER, name=f"GC2 {root_topic}")
        self.root_topic = root_topic.rstrip("/")
        # Device registry identifiers must never depend on retained MQTT
        # delivery order. The manifest may arrive before or after individual
        # platforms are set up, so use config-entry data captured at discovery.
        self.device_id = device_id
        self.device_name = device_name
        self.data = Gc2Snapshot()
        self._unsubscribe = None

    async def async_start(self) -> None:
        """Wait for MQTT and subscribe to the complete bridge inventory."""
        await mqtt.async_wait_for_mqtt_client(self.hass)
        self._unsubscribe = await mqtt.async_subscribe(
            self.hass, f"{self.root_topic}/#", self._message_received, qos=1
        )

    @callback
    def _message_received(self, message: ReceiveMessage) -> None:
        payload = message.payload
        if isinstance(payload, bytes):
            payload = payload.decode("utf-8", errors="replace")
        if self.data.apply_message(self.root_topic, message.topic, str(payload)):
            self.async_set_updated_data(self.data)

    async def async_publish(
        self, suffix: str, payload: str, *, retain: bool = False
    ) -> None:
        """Publish an allowlisted panel request below the selected root."""
        await mqtt.async_publish(
            self.hass,
            f"{self.root_topic}/{suffix}",
            payload,
            qos=1,
            retain=retain,
        )

    async def async_stop(self) -> None:
        """Release the MQTT subscription."""
        if self._unsubscribe is not None:
            self._unsubscribe()
            self._unsubscribe = None
