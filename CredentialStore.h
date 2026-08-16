#pragma once

#include <Arduino.h>

struct MqttSettings {
    bool enabled = false;
    String host;
    uint16_t port = 1883;
    String username;
    String password;
    String baseTopic = "2gig/gc2";
    bool discovery = true;
};

class CredentialStore {
  public:
    bool hasTelnetPassword() const;
    bool setTelnetPassword(const String& password);
    bool verifyTelnetPassword(const String& password) const;
    bool loadOtaPasswordHash(String& hash) const;
    bool ensureOtaPasswordHashForVerifiedPassword(const String& password);
    bool loadMqttSettings(MqttSettings& settings) const;
    bool setMqttSettings(const MqttSettings& settings);
    void clear();

  private:
    static constexpr size_t kSaltSize = 16;
    static constexpr size_t kHashSize = 32;
    static constexpr uint32_t kHashRounds = 4096;

    static bool deriveHash(const String& password,
                           const uint8_t salt[kSaltSize],
                           uint8_t output[kHashSize]);
    static bool otaPasswordHash(const String& password, String& output);
    static bool mqttSettingsValid(const MqttSettings& settings);
};
