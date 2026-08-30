"""Constants for the GC2 Panel integration."""

from homeassistant.const import Platform

DOMAIN = "gc2_panel"
CONF_ROOT_TOPIC = "root_topic"
CONF_DEVICE_ID = "device_id"
TRANSPORT_SCHEMA = "gc2-mqtt-v1"

PLATFORMS = (
    Platform.ALARM_CONTROL_PANEL,
    Platform.BINARY_SENSOR,
    Platform.NUMBER,
    Platform.SENSOR,
    Platform.SWITCH,
)
