#pragma once

#include <Arduino.h>

namespace DeviceIdentity {

void begin();
const String& macHex();
const String& shortSuffix();
const String& hostname();
const String& setupSsid();
const String& mqttDeviceId();
const String& displayName();
const String& legacyMqttDeviceId();

}  // namespace DeviceIdentity
