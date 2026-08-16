#include "CredentialStore.h"

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

namespace {
constexpr char kNamespace[] = "gc2bridge";
constexpr char kSaltKey[] = "telnetSalt";
constexpr char kHashKey[] = "telnetHash";
constexpr char kOtaHashKey[] = "otaPassHash";
constexpr char kMqttEnabledKey[] = "mqEnabled";
constexpr char kMqttHostKey[] = "mqHost";
constexpr char kMqttPortKey[] = "mqPort";
constexpr char kMqttUsernameKey[] = "mqUser";
constexpr char kMqttPasswordKey[] = "mqPass";
constexpr char kMqttBaseTopicKey[] = "mqBase";
constexpr char kMqttDiscoveryKey[] = "mqDiscover";
}  // namespace

bool CredentialStore::otaPasswordHash(const String& password,
                                      String& output) {
    uint8_t digest[kHashSize] = {};
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    const bool succeeded =
        mbedtls_sha256_starts(&context, 0) == 0 &&
        mbedtls_sha256_update(
            &context,
            reinterpret_cast<const unsigned char*>(password.c_str()),
            password.length()) == 0 &&
        mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!succeeded) {
        memset(digest, 0, sizeof(digest));
        return false;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    char encoded[kHashSize * 2 + 1];
    for (size_t index = 0; index < kHashSize; ++index) {
        encoded[index * 2] = kHex[digest[index] >> 4];
        encoded[index * 2 + 1] = kHex[digest[index] & 0x0F];
    }
    encoded[sizeof(encoded) - 1] = '\0';
    output = encoded;
    memset(digest, 0, sizeof(digest));
    memset(encoded, 0, sizeof(encoded));
    return true;
}

bool CredentialStore::deriveHash(const String& password,
                                 const uint8_t salt[kSaltSize],
                                 uint8_t output[kHashSize]) {
    uint8_t digest[kHashSize] = {};
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);

    if (mbedtls_sha256_starts(&context, 0) != 0 ||
        mbedtls_sha256_update(&context, salt, kSaltSize) != 0 ||
        mbedtls_sha256_update(
            &context, reinterpret_cast<const unsigned char*>(password.c_str()),
            password.length()) != 0 ||
        mbedtls_sha256_finish(&context, digest) != 0) {
        mbedtls_sha256_free(&context);
        return false;
    }

    for (uint32_t round = 1; round < kHashRounds; ++round) {
        if (mbedtls_sha256_starts(&context, 0) != 0 ||
            mbedtls_sha256_update(&context, digest, sizeof(digest)) != 0 ||
            mbedtls_sha256_update(&context, salt, kSaltSize) != 0 ||
            mbedtls_sha256_update(
                &context,
                reinterpret_cast<const unsigned char*>(password.c_str()),
                password.length()) != 0 ||
            mbedtls_sha256_finish(&context, digest) != 0) {
            mbedtls_sha256_free(&context);
            return false;
        }
    }

    mbedtls_sha256_free(&context);
    memcpy(output, digest, sizeof(digest));
    memset(digest, 0, sizeof(digest));
    return true;
}

bool CredentialStore::hasTelnetPassword() const {
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) return false;
    const bool present = preferences.getBytesLength(kSaltKey) == kSaltSize &&
                         preferences.getBytesLength(kHashKey) == kHashSize;
    preferences.end();
    return present;
}

bool CredentialStore::setTelnetPassword(const String& password) {
    if (password.length() < 8 || password.length() > 64) return false;

    uint8_t salt[kSaltSize];
    for (size_t i = 0; i < kSaltSize; i += sizeof(uint32_t)) {
        const uint32_t randomValue = esp_random();
        memcpy(salt + i, &randomValue,
               min(sizeof(randomValue), kSaltSize - i));
    }

    uint8_t hash[kHashSize];
    if (!deriveHash(password, salt, hash)) return false;
    String otaHash;
    if (!otaPasswordHash(password, otaHash)) {
        memset(salt, 0, sizeof(salt));
        memset(hash, 0, sizeof(hash));
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        memset(salt, 0, sizeof(salt));
        memset(hash, 0, sizeof(hash));
        otaHash = String();
        return false;
    }
    const bool saved = preferences.putBytes(kSaltKey, salt, sizeof(salt)) ==
                           sizeof(salt) &&
                       preferences.putBytes(kHashKey, hash, sizeof(hash)) ==
                           sizeof(hash) &&
                       preferences.putString(kOtaHashKey, otaHash) ==
                           otaHash.length();
    preferences.end();

    memset(salt, 0, sizeof(salt));
    memset(hash, 0, sizeof(hash));
    otaHash = String();
    return saved;
}

bool CredentialStore::verifyTelnetPassword(const String& password) const {
    uint8_t salt[kSaltSize];
    uint8_t expected[kHashSize];

    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) return false;
    const bool loaded = preferences.getBytes(kSaltKey, salt, sizeof(salt)) ==
                            sizeof(salt) &&
                        preferences.getBytes(kHashKey, expected,
                                             sizeof(expected)) ==
                            sizeof(expected);
    preferences.end();
    if (!loaded) return false;

    uint8_t actual[kHashSize];
    if (!deriveHash(password, salt, actual)) return false;

    uint8_t difference = 0;
    for (size_t i = 0; i < kHashSize; ++i) {
        difference |= actual[i] ^ expected[i];
    }

    memset(salt, 0, sizeof(salt));
    memset(expected, 0, sizeof(expected));
    memset(actual, 0, sizeof(actual));
    return difference == 0;
}

bool CredentialStore::loadOtaPasswordHash(String& hash) const {
    hash = String();
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) return false;
    hash = preferences.getString(kOtaHashKey, "");
    preferences.end();
    if (hash.length() != kHashSize * 2) {
        hash = String();
        return false;
    }
    for (const char character : hash) {
        if (!isHexadecimalDigit(character)) {
            hash = String();
            return false;
        }
    }
    return true;
}

bool CredentialStore::ensureOtaPasswordHashForVerifiedPassword(
    const String& password) {
    String existing;
    if (loadOtaPasswordHash(existing)) return true;
    if (password.length() < 8 || password.length() > 64) return false;

    String hash;
    if (!otaPasswordHash(password, hash)) return false;
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return false;
    const bool saved = preferences.putString(kOtaHashKey, hash) == hash.length();
    preferences.end();
    hash = String();
    return saved;
}

bool CredentialStore::mqttSettingsValid(const MqttSettings& settings) {
    if (!settings.enabled) return true;
    if (settings.host.isEmpty() || settings.host.length() > 253 ||
        settings.port == 0 || settings.username.length() > 128 ||
        settings.password.length() > 128 || settings.baseTopic.isEmpty() ||
        settings.baseTopic.length() > 96) {
        return false;
    }
    if (settings.host.indexOf(' ') >= 0 ||
        settings.baseTopic.indexOf('#') >= 0 ||
        settings.baseTopic.indexOf('+') >= 0 ||
        settings.baseTopic.startsWith("/") ||
        settings.baseTopic.endsWith("/")) {
        return false;
    }
    return true;
}

bool CredentialStore::loadMqttSettings(MqttSettings& settings) const {
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) return false;
    if (!preferences.isKey(kMqttEnabledKey)) {
        preferences.end();
        return false;
    }
    settings.enabled = preferences.getBool(kMqttEnabledKey, false);
    settings.host = preferences.getString(kMqttHostKey, "");
    settings.port = preferences.getUShort(kMqttPortKey, 1883);
    settings.username = preferences.getString(kMqttUsernameKey, "");
    settings.password = preferences.getString(kMqttPasswordKey, "");
    settings.baseTopic = preferences.getString(kMqttBaseTopicKey, "2gig/gc2");
    settings.discovery = preferences.getBool(kMqttDiscoveryKey, true);
    preferences.end();
    return mqttSettingsValid(settings);
}

bool CredentialStore::setMqttSettings(const MqttSettings& settings) {
    if (!mqttSettingsValid(settings)) return false;
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return false;
    bool saved = preferences.putBool(kMqttEnabledKey, settings.enabled) == 1;
    saved = preferences.putString(kMqttHostKey, settings.host) ==
                settings.host.length() &&
            saved;
    saved = preferences.putUShort(kMqttPortKey, settings.port) == 2 && saved;
    saved = preferences.putString(kMqttUsernameKey, settings.username) ==
                settings.username.length() &&
            saved;
    saved = preferences.putString(kMqttPasswordKey, settings.password) ==
                settings.password.length() &&
            saved;
    saved = preferences.putString(kMqttBaseTopicKey, settings.baseTopic) ==
                settings.baseTopic.length() &&
            saved;
    saved = preferences.putBool(kMqttDiscoveryKey, settings.discovery) == 1 &&
            saved;
    preferences.end();
    return saved;
}

void CredentialStore::clear() {
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return;
    preferences.clear();
    preferences.end();
}
