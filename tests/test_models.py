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
