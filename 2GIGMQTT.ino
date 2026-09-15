#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

#include "AppConfig.h"
#include "CredentialStore.h"
#include "DeviceIdentity.h"
#include "Gc2ConsoleParser.h"
#include "Gc2State.h"
#include "MqttService.h"
#include "OtaService.h"
#include "PanelBridge.h"
#include "ProvisioningPortal.h"
#include "StatusLed.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select the Waveshare ESP32-S3-Zero board in Arduino IDE."
#endif

namespace {
enum class NetworkState { Connecting, Provisioning, Connected, Reconnecting };

CredentialStore credentialStore;
ProvisioningPortal provisioningPortal;
Gc2State gc2State;
Gc2ConsoleParser consoleParser(gc2State);
PanelBridge panelBridge(credentialStore, consoleParser);
MqttService mqttService(credentialStore, gc2State, panelBridge);
OtaService otaService(credentialStore);
StatusLed statusLed;

NetworkState networkState = NetworkState::Connecting;
uint32_t networkStateStartedAt = 0;
uint32_t resetButtonPressedAt = 0;
bool resetWarningShown = false;

void startSavedWifiConnection() {
    WiFi.persistent(true);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(DeviceIdentity::hostname().c_str());
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin();
    networkState = NetworkState::Connecting;
    networkStateStartedAt = millis();
    Serial.println(F("[wifi] Connecting with saved credentials..."));
}

void announceNetworkReady() {
    provisioningPortal.stop();
    networkState = NetworkState::Connected;
    networkStateStartedAt = millis();
    panelBridge.setNetworkReady(true);
    mqttService.setNetworkReady(true);

    // UTC is used only for downstream timestamps. The panel's own RTC remains
    // untouched and is still parsed independently for reboot detection.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    MDNS.end();
    if (MDNS.begin(DeviceIdentity::hostname().c_str())) {
        MDNS.addService("telnet", "tcp", AppConfig::kTelnetPort);
    }
    otaService.setNetworkReady(true);

    Serial.print(F("[wifi] Connected. Address: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("[wifi] Tera Term host: "));
    Serial.print(DeviceIdentity::hostname());
    Serial.print(F(".local, port "));
    Serial.println(AppConfig::kTelnetPort);
}

void startProvisioning() {
    panelBridge.setNetworkReady(false);
    mqttService.setNetworkReady(false);
    otaService.stopNetwork();
    provisioningPortal.begin();
    networkState = NetworkState::Provisioning;
    networkStateStartedAt = millis();
}

void processNetwork() {
    switch (networkState) {
        case NetworkState::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                announceNetworkReady();
            } else if (millis() - networkStateStartedAt >=
                       AppConfig::kWifiConnectTimeoutMs) {
                Serial.println(
                    F("[wifi] Saved network unavailable; remaining provisioned and retrying."));
                WiFi.reconnect();
                networkState = NetworkState::Reconnecting;
                networkStateStartedAt = millis();
            }
            break;

        case NetworkState::Provisioning: {
            provisioningPortal.loop();
            ProvisionedSettings settings;
            if (provisioningPortal.takeConnectedSettings(settings)) {
                const bool passwordSaved =
                    credentialStore.setTelnetPassword(settings.telnetPassword);
                const bool mqttSaved =
                    credentialStore.setMqttSettings(settings.mqtt);
                settings.wifiPassword = String();
                settings.telnetPassword = String();
                settings.mqtt.password = String();
                if (passwordSaved && mqttSaved) {
                    statusLed.setProvisioned(true);
                    mqttService.reloadSettings();
                    announceNetworkReady();
                } else {
                    Serial.println(F("[security] Could not save bridge settings."));
                    credentialStore.clear();
                    WiFi.disconnect(false, true);
                }
            }
            break;
        }

        case NetworkState::Connected:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println(F("[wifi] Connection lost; reconnecting."));
                panelBridge.setNetworkReady(false);
                mqttService.setNetworkReady(false);
                otaService.stopNetwork();
                MDNS.end();
                WiFi.reconnect();
                networkState = NetworkState::Reconnecting;
                networkStateStartedAt = millis();
            }
            break;

        case NetworkState::Reconnecting:
            if (WiFi.status() == WL_CONNECTED) {
                announceNetworkReady();
            } else if (millis() - networkStateStartedAt >=
                       AppConfig::kWifiReconnectIntervalMs) {
                Serial.println(F("[wifi] Still offline; retrying saved network."));
                WiFi.reconnect();
                networkStateStartedAt = millis();
            }
            break;
    }
}

void processFactoryResetButton() {
    if (digitalRead(AppConfig::kFactoryResetPin) != LOW) {
        resetButtonPressedAt = 0;
        resetWarningShown = false;
        return;
    }

    if (resetButtonPressedAt == 0) resetButtonPressedAt = millis();
    const uint32_t heldFor = millis() - resetButtonPressedAt;
    if (!resetWarningShown && heldFor >= AppConfig::kFactoryResetHoldMs / 2) {
        resetWarningShown = true;
        Serial.println(F("[reset] Keep holding BOOT to erase saved settings."));
    }
    if (heldFor < AppConfig::kFactoryResetHoldMs) return;

    Serial.println(F("[reset] Erasing Wi-Fi and bridge credentials."));
    provisioningPortal.stop();
    panelBridge.stopNetwork();
    mqttService.stopNetwork();
    otaService.stopNetwork();
    MDNS.end();
    credentialStore.clear();
    statusLed.setProvisioned(false);
    WiFi.disconnect(true, true);
    delay(300);
    ESP.restart();
}
}  // namespace

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    // The USB CDC console blocks each write for up to 100 ms when a host is
    // attached but not reading. That stall is long enough to overflow the
    // panel UART buffer, so never wait on the debug console.
    Serial.setTxTimeoutMs(0);
#endif
    delay(250);
    Serial.println();
    Serial.println(F("GC2 UART Bridge starting"));

    DeviceIdentity::begin();
    Serial.print(F("[identity] Station MAC: "));
    Serial.println(DeviceIdentity::macHex());
    Serial.print(F("[identity] Network hostname: "));
    Serial.println(DeviceIdentity::hostname());
    Serial.print(F("[identity] MQTT device: "));
    Serial.println(DeviceIdentity::mqttDeviceId());

    pinMode(AppConfig::kFactoryResetPin, INPUT_PULLUP);
    panelBridge.begin();
    mqttService.begin();
    otaService.begin();

    const bool provisioned = credentialStore.hasTelnetPassword();
    statusLed.begin(provisioned);
    if (provisioned) {
        startSavedWifiConnection();
    } else {
        startProvisioning();
    }
}

void loop() {
    statusLed.loop();
    processFactoryResetButton();
    processNetwork();
    otaService.loop();
    panelBridge.loop();
    mqttService.loop();
    delay(1);
}
