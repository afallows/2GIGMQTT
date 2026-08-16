"""Config flow for GC2 Panel."""

from __future__ import annotations

import json
from typing import Any

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.helpers.service_info.mqtt import MqttServiceInfo
import voluptuous as vol

from .const import CONF_DEVICE_ID, CONF_ROOT_TOPIC, DOMAIN, TRANSPORT_SCHEMA


def _discovery_payload(payload: str) -> dict[str, Any] | None:
    try:
        value = json.loads(payload)
    except (TypeError, ValueError):
        return None
    if not isinstance(value, dict) or value.get("schema") != TRANSPORT_SCHEMA:
        return None
    if not value.get("root_topic") or not value.get("device_id"):
        return None
    return value


class Gc2PanelConfigFlow(ConfigFlow, domain=DOMAIN):
    """Configure one physical GC2 panel by its MQTT root."""

    VERSION = 1
    _discovered: dict[str, Any] | None = None

    async def async_step_mqtt(
        self, discovery_info: MqttServiceInfo
    ) -> ConfigFlowResult:
        """Offer a bridge advertised through the fixed discovery topic."""
        info = _discovery_payload(discovery_info.payload)
        if info is None:
            return self.async_abort(reason="invalid_discovery")

        device_id = str(info["device_id"])
        await self.async_set_unique_id(device_id)
        self._abort_if_unique_id_configured(
            updates={CONF_ROOT_TOPIC: str(info["root_topic"]).rstrip("/")}
        )
        self._discovered = info
        self.context["title_placeholders"] = {"name": str(info.get("name", device_id))}
        return await self.async_step_discovery_confirm()

    async def async_step_discovery_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Let the user select this discovered GC2 bridge."""
        assert self._discovered is not None
        if user_input is not None:
            info = self._discovered
            return self.async_create_entry(
                title=str(info.get("name", info["device_id"])),
                data={
                    CONF_ROOT_TOPIC: str(info["root_topic"]).rstrip("/"),
                    CONF_DEVICE_ID: str(info["device_id"]),
                },
            )
        return self.async_show_form(
            step_id="discovery_confirm",
            description_placeholders={
                "name": str(self._discovered.get("name", "GC2 Panel")),
                "topic": str(self._discovered["root_topic"]),
            },
        )

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Allow manual topic selection when MQTT discovery is disabled."""
        errors: dict[str, str] = {}
        if user_input is not None:
            root = str(user_input[CONF_ROOT_TOPIC]).strip().rstrip("/")
            if not root or "+" in root or "#" in root:
                errors[CONF_ROOT_TOPIC] = "invalid_topic"
            else:
                device_id = root.rsplit("/", 1)[-1]
                await self.async_set_unique_id(device_id)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title=f"GC2 ({device_id})",
                    data={CONF_ROOT_TOPIC: root, CONF_DEVICE_ID: device_id},
                )

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({vol.Required(CONF_ROOT_TOPIC): str}),
            errors=errors,
        )
