"""Tests for MQTT inventory conditioning without requiring Home Assistant."""

import importlib.util
from pathlib import Path
import sys

MODULE = (
    Path(__file__).parents[1]
    / "custom_components"
    / "gc2_panel"
    / "models.py"
)
SPEC = importlib.util.spec_from_file_location("gc2_models", MODULE)
assert SPEC is not None and SPEC.loader is not None
MODELS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODELS
SPEC.loader.exec_module(MODELS)
Gc2Snapshot = MODELS.Gc2Snapshot
panel_activity_transition = MODELS.panel_activity_transition
zone_activity_transition = MODELS.zone_activity_transition


def test_retained_inventory_rebuilds_panel() -> None:
    root = "2gig/gc2/gc2_bridge_aabbccddeeff"
    data = Gc2Snapshot()

    assert data.apply_message(root, f"{root}/availability", "online")
    assert data.apply_message(
        root,
        f"{root}/manifest",
        '{"schema":"gc2-mqtt-v1","device_id":"gc2_bridge_aabbccddeeff"}',
    )
    assert data.apply_message(
        root,
        f"{root}/zone/02/state",
        '{"state":"OFF","name":"Patio Door","enabled":true}',
    )
    assert data.apply_message(
        root,
        f"{root}/user/001/state",
        '{"id":1,"last_action":"disarmed","origin":"local"}',
    )

    assert data.available is True
    assert data.zones[2]["name"] == "Patio Door"
    assert data.users[1]["last_action"] == "disarmed"


def test_zone_battery_fields_are_retained_with_zone_state() -> None:
    root = "2gig/gc2/bridge"
    data = Gc2Snapshot()

    assert data.apply_message(
        root,
        f"{root}/zone/07/state",
        '{"state":"OFF","name":"Shed Door","battery_known":true,'
        '"battery_low":true}',
    )

    assert data.zones[7]["state"] == "OFF"
    assert data.zones[7]["battery_known"] is True
    assert data.zones[7]["battery_low"] is True


def test_empty_retained_payload_removes_disabled_zone() -> None:
    root = "2gig/gc2/bridge"
    data = Gc2Snapshot()
    data.apply_message(root, f"{root}/zone/02/state", '{"state":"ON"}')

    assert data.apply_message(root, f"{root}/zone/02/state", "")
    assert 2 not in data.zones


def test_invalid_or_unrelated_messages_are_ignored() -> None:
    data = Gc2Snapshot()
    assert not data.apply_message("gc2/root", "another/root/manifest", "{}")
    assert not data.apply_message("gc2/root", "gc2/root/zone/01/state", "oops")


def test_sounder_volume_accepts_only_normalized_percent() -> None:
    root = "2gig/gc2/bridge"
    data = Gc2Snapshot()

    assert data.apply_message(root, f"{root}/panel/sounder_volume", "42")
    assert data.sounder_volume == 42
    assert not data.apply_message(root, f"{root}/panel/sounder_volume", "42")
    assert not data.apply_message(root, f"{root}/panel/sounder_volume", "101")
    assert not data.apply_message(root, f"{root}/panel/sounder_volume", "loud")
    assert data.sounder_volume == 42
    assert data.apply_message(root, f"{root}/panel/sounder_volume", "")
    assert data.sounder_volume is None
    assert not data.apply_message(root, f"{root}/panel/sounder_volume", "")


def test_security_trouble_and_alarm_memory_are_retained() -> None:
    root = "2gig/gc2/bridge"
    data = Gc2Snapshot()

    assert data.apply_message(
        root,
        f"{root}/panel/security",
        '{"known":true,"panel_tamper":true,"rf_jam":false}',
    )
    assert data.apply_message(
        root,
        f"{root}/panel/trouble",
        '{"known":true,"active":true,"active_count":1,'
        '"unacknowledged_count":1}',
    )
    assert data.apply_message(
        root,
        f"{root}/panel/trouble/02",
        '{"slot":2,"zone":0,"description":"Panel Tamper",'
        '"active":true,"acknowledged":false}',
    )
    assert data.apply_message(
        root,
        f"{root}/panel/alarm_memory",
        '{"known":true,"clear":false,"latched":true}',
    )

    assert data.security["panel_tamper"] is True
    assert data.trouble_summary["active_count"] == 1
    assert data.troubles[2]["description"] == "Panel Tamper"
    assert data.alarm_memory["latched"] is True

    assert data.apply_message(root, f"{root}/panel/trouble/02", "")
    assert 2 not in data.troubles


def test_activity_transitions_ignore_initialization_and_repeated_reads() -> None:
    moisture = {"name": "Kitchen Flood Sensor", "zone_type": "Water"}

    assert zone_activity_transition(moisture, None, "OFF") is None
    assert zone_activity_transition(moisture, "OFF", "OFF") is None
    assert zone_activity_transition(moisture, "OFF", "ON") == "wet"
    assert zone_activity_transition(moisture, "ON", "OFF") == "dry"

    assert panel_activity_transition("unknown", "disarmed") is None
    assert panel_activity_transition("disarmed", "disarmed") is None
    assert panel_activity_transition("disarmed", "armed_away") == "armed_away"


def test_activity_transitions_use_security_specific_labels() -> None:
    assert zone_activity_transition({"name": "Patio Door"}, "OFF", "ON") == "opened"
    assert zone_activity_transition({"name": "Patio Door"}, "ON", "OFF") == "closed"
    assert zone_activity_transition({"name": "Hall Motion"}, "OFF", "ON") == "detected"
    assert zone_activity_transition({"name": "Smoke Alarm"}, "OFF", "ON") == "smoke"
