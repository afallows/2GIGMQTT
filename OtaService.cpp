#include "OtaService.h"

#include <ArduinoOTA.h>
#include <WiFi.h>

#include "AppConfig.h"
#include "DeviceIdentity.h"

OtaService::OtaService(CredentialStore& credentials)
    : credentials_(credentials) {}

bool OtaService::deadlineReached(uint32_t deadline) {
    return static_cast<int32_t>(millis() - deadline) >= 0;
}

void OtaService::begin() { configureCallbacks(); }

void OtaService::configureCallbacks() {
    if (callbacksConfigured_) return;

    ArduinoOTA.onStart([this]() {
        lastProgressPercent_ = 255;
        Serial.println(F("[ota] Authenticated firmware update started."));
    });
    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        if (total == 0) return;
        const uint8_t percent = static_cast<uint8_t>(
            (static_cast<uint64_t>(progress) * 100U) / total);
        if (percent == lastProgressPercent_ || percent % 10 != 0) return;
        lastProgressPercent_ = percent;
        Serial.print(F("[ota] Progress: "));
        Serial.print(percent);
        Serial.println('%');
    });
    ArduinoOTA.onEnd([]() {
        Serial.println(F("[ota] Update complete; rebooting."));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.print(F("[ota] Update failed, error "));
        Serial.println(static_cast<unsigned int>(error));
    });
    callbacksConfigured_ = true;
}

void OtaService::setNetworkReady(bool ready) {
    if (!ready) {
        stopNetwork();
        return;
    }
    networkReady_ = true;
    nextStartAttemptAt_ = 0;
    startIfPossible();
}

void OtaService::stopNetwork() {
    if (active_) ArduinoOTA.end();
    active_ = false;
    networkReady_ = false;
    nextStartAttemptAt_ = 0;
}

void OtaService::startIfPossible() {
    if (!networkReady_ || active_ || WiFi.status() != WL_CONNECTED) return;

    String passwordHash;
    if (!credentials_.loadOtaPasswordHash(passwordHash)) {
        if (!missingPasswordNoticeShown_) {
            Serial.println(F(
                "[ota] Sign in to Tera Term once to enable authenticated OTA on an existing installation."));
            missingPasswordNoticeShown_ = true;
        }
        nextStartAttemptAt_ = millis() + 2000;
        return;
    }

    ArduinoOTA.setPort(AppConfig::kOtaPort);
    ArduinoOTA.setHostname(DeviceIdentity::hostname().c_str());
    ArduinoOTA.setPasswordHash(passwordHash.c_str());
    ArduinoOTA.setRebootOnSuccess(true);
    ArduinoOTA.begin();
    passwordHash = String();
    active_ = true;
    missingPasswordNoticeShown_ = false;

    Serial.print(F("[ota] Arduino IDE network upload ready as "));
    Serial.print(DeviceIdentity::hostname());
    Serial.print(F(".local on port "));
    Serial.println(AppConfig::kOtaPort);
}

void OtaService::loop() {
    if (!networkReady_) return;
    if (!active_) {
        if (deadlineReached(nextStartAttemptAt_)) startIfPossible();
        return;
    }
    ArduinoOTA.handle();
}
