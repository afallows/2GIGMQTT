"""Pure MQTT state model for a GC2 panel."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import re
from typing import Any

_ZONE_TOPIC = re.compile(r"zone/(\d{2})/state$")
_USER_TOPIC = re.compile(r"user/(\d{3})/state$")


def _json_object(payload: str) -> dict[str, Any] | None:
    try:
        value = json.loads(payload)
    except (TypeError, ValueError):
        return None
    return value if isinstance(value, dict) else None


@dataclass(slots=True)
class Gc2Snapshot:
    """Latest retained and live data received below one bridge topic."""

    available: bool = False
    manifest: dict[str, Any] = field(default_factory=dict)
    panel_state: str = "unknown"
    panel: dict[str, Any] = field(default_factory=dict)
    battery: dict[str, Any] = field(default_factory=dict)
    firmware: dict[str, Any] = field(default_factory=dict)
    diagnostics: dict[str, Any] = field(default_factory=dict)
    command_status: dict[str, Any] = field(default_factory=dict)
    baud: str | None = None
    zones: dict[int, dict[str, Any]] = field(default_factory=dict)
    users: dict[int, dict[str, Any]] = field(default_factory=dict)

    def apply_message(self, root_topic: str, topic: str, payload: str) -> bool:
        """Apply one MQTT message; return whether modeled state changed."""
        prefix = root_topic.rstrip("/") + "/"
        if not topic.startswith(prefix):
            return False
        suffix = topic[len(prefix) :]

        if suffix == "availability":
            value = payload.strip().lower() == "online"
            if value == self.available:
                return False
            self.available = value
            return True

        if suffix == "panel/state":
            value = payload.strip().lower() or "unknown"
            if value == self.panel_state:
                return False
            self.panel_state = value
            return True

        if suffix == "uart/baud":
            value = payload.strip() or None
            if value == self.baud:
                return False
            self.baud = value
            return True

        object_targets = {
            "manifest": "manifest",
            "panel/status": "panel",
            "panel/battery": "battery",
            "panel/firmware": "firmware",
            "panel/command_status": "command_status",
            "diagnostic": "diagnostics",
        }
        if target := object_targets.get(suffix):
            value = _json_object(payload)
            if value is None or value == getattr(self, target):
                return False
            setattr(self, target, value)
            return True

        if match := _ZONE_TOPIC.fullmatch(suffix):
            number = int(match.group(1))
            if not payload:
                return self.zones.pop(number, None) is not None
            value = _json_object(payload)
            if value is None:
                return False
            value["number"] = number
            if value == self.zones.get(number):
                return False
            self.zones[number] = value
            return True

        if match := _USER_TOPIC.fullmatch(suffix):
            user_id = int(match.group(1))
            if not payload:
                return self.users.pop(user_id, None) is not None
            value = _json_object(payload)
            if value is None:
                return False
            value["id"] = user_id
            if value == self.users.get(user_id):
                return False
            self.users[user_id] = value
            return True

        return False
