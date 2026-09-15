#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "CredentialStore.h"
#include "Gc2State.h"

class PanelBridge;

class MqttService {
  public:
    MqttService(CredentialStore& credentials, Gc2State& state,
                PanelBridge& bridge);

    void begin();
    void reloadSettings();
    void setNetworkReady(bool ready);
    void stopNetwork();
    void loop();
    bool enabled() const { return settings_.enabled; }

  private:
    static constexpr uint8_t kLegacySensorDiscoveryCount = 10;
    static constexpr uint8_t kBaseDiscoveryCount = 15;
    static constexpr uint16_t kLegacyCleanupCount =
        kLegacySensorDiscoveryCount + (Gc2State::kMaxZones - 1) + 5 +
        (Gc2State::kMaxZones - 1);
    static constexpr uint16_t kNativeDiscoveryCleanupCount =
        kBaseDiscoveryCount + 2 * (Gc2State::kMaxZones - 1);

    CredentialStore& credentials_;
    Gc2State& state_;
    PanelBridge& bridge_;
    WiFiClient transport_;
    PubSubClient client_;
    MqttSettings settings_;

    bool networkReady_ = false;
    uint32_t nextConnectAt_ = 0;
    uint32_t nextPublishAt_ = 0;
    uint32_t nextDiagnosticAt_ = 0;
    bool everConnected_ = false;
    uint32_t reconnectCount_ = 0;
    String deviceId_;
    String rootTopic_;
    String availabilityTopic_;
    String baudCommandTopic_;
    String alarmCommandTopic_;
    String bypassCommandTopic_;
    String zoneChimeCommandTopic_;
    String sounderVolumeCommandTopic_;
    uint8_t discoveryStage_ = 0;
    uint16_t legacyCleanupStage_ = kLegacyCleanupCount;
    uint16_t nativeCleanupStage_ = kNativeDiscoveryCleanupCount;
    bool manifestPublished_ = false;
    bool zoneDiscoveryPublished_[Gc2State::kMaxZones]{};
    uint32_t publishedZoneMetadataRevision_[Gc2State::kMaxZones]{};
    uint32_t publishedZoneRevision_[Gc2State::kMaxZones]{};
    uint32_t publishedAlarmRevision_ = 0;
    uint32_t publishedBatteryRevision_ = 0;
    uint32_t publishedSounderVolumeRevision_ = 0;
    uint32_t publishedFirmwareRevision_ = 0;
    uint32_t publishedTroubleRevision_ = 0;
    uint32_t publishingTroubleRevision_ = 0;
    uint8_t troublePublishStage_ = 0;
    uint32_t publishedPanelSecurityRevision_ = 0;
    uint32_t publishedAlarmMemoryRevision_ = 0;
    uint32_t publishedDiagnosticRevision_ = 0;
    uint32_t publishedAlarmCommandRevision_ = 0;
    String publishedBaudState_;
    String publishedDebugUnlockState_;

    void connectIfNeeded();
    void onMessage(char* topic, uint8_t* payload, unsigned int length);
    void resetPublishTracking();
    void publishNext();
    bool publishLegacyCleanup(uint16_t stage);
    bool publishNativeDiscoveryCleanup(uint16_t stage);
    bool publishManifest();
    bool publishBaseDiscovery(uint8_t stage);
    bool publishZoneDiscovery(uint8_t zone);
    bool publishZoneBypassDiscovery(uint8_t zone);
    bool removeZoneDiscovery(uint8_t zone);
    bool publishDiscovery(const String& component, const String& objectId,
                          const String& name, const String& stateTopic,
                          const String& valueTemplate,
                          const String& deviceClass = String(),
                          const String& unit = String(),
                          const String& entityCategory = String(),
                          const String& stateClass = String(),
                          const String& commandTopic = String());
    bool publishAlarmDiscovery();
    bool publishAlarmState();
    bool publishAlarmCommandStatus();
    bool publishBatteryState();
    bool publishSounderVolumeState();
    bool publishFirmwareState();
    bool publishTroubleSummary();
    bool publishTroubleEntry(uint8_t slot);
    bool publishPanelSecurityState();
    bool publishAlarmMemoryState();
    bool publishZoneState(uint8_t zone, const bool* stateOverride = nullptr);
    bool publishDiagnostics();
    bool publishBaudState();
    bool publishRetained(const String& suffix, const String& payload);
    String makeTopic(const String& suffix) const;
    String deviceJson() const;
    static String jsonEscape(const String& value);
    static String hexValue(uint32_t value, uint8_t width);
};
