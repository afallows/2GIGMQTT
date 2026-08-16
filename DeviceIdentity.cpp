#include "DeviceIdentity.h"

#include <esp_mac.h>

namespace {
bool initialized = false;
String macHexValue;
String shortSuffixValue;
String hostnameValue;
String setupSsidValue;
String mqttDeviceIdValue;
String displayNameValue;
String legacyMqttDeviceIdValue;

void initialize() {
    if (initialized) return;

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        // A factory-programmed station MAC is present on production ESP32-S3
        // devices. This fixed fallback keeps the failure visible instead of
        // silently manufacturing an identity that could collide.
        memset(mac, 0, sizeof(mac));
    }

    char full[13];
    snprintf(full, sizeof(full), "%02x%02x%02x%02x%02x%02x", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    macHexValue = full;

    char shortSuffix[7];
    snprintf(shortSuffix, sizeof(shortSuffix), "%02x%02x%02x", mac[3],
             mac[4], mac[5]);
    shortSuffixValue = shortSuffix;

    hostnameValue = String("gc2-bridge-") + macHexValue;
    setupSsidValue = String("GC2-Bridge-") + macHexValue;
    setupSsidValue.toUpperCase();
    mqttDeviceIdValue = String("gc2_bridge_") + macHexValue;
    String displaySuffix = shortSuffixValue;
    displaySuffix.toUpperCase();
    displayNameValue = String("2GIG GC2 Bridge ") + displaySuffix;

    const uint64_t legacyChipId = ESP.getEfuseMac();
    char legacySuffix[7];
    snprintf(legacySuffix, sizeof(legacySuffix), "%06llx",
             static_cast<unsigned long long>(legacyChipId & 0xFFFFFFULL));
    legacyMqttDeviceIdValue = String("gc2_bridge_") + legacySuffix;
    initialized = true;
}
}  // namespace

namespace DeviceIdentity {

void begin() { initialize(); }

const String& macHex() {
    initialize();
    return macHexValue;
}

const String& shortSuffix() {
    initialize();
    return shortSuffixValue;
}

const String& hostname() {
    initialize();
    return hostnameValue;
}

const String& setupSsid() {
    initialize();
    return setupSsidValue;
}

const String& mqttDeviceId() {
    initialize();
    return mqttDeviceIdValue;
}

const String& displayName() {
    initialize();
    return displayNameValue;
}

const String& legacyMqttDeviceId() {
    initialize();
    return legacyMqttDeviceIdValue;
}

}  // namespace DeviceIdentity
