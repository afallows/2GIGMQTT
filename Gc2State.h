#pragma once

#include <Arduino.h>

#include <deque>

enum class Gc2ZoneKind : uint8_t { Unknown, Contact, Motion, Smoke };

struct Gc2TroubleSnapshot {
    bool valid = false;
    uint8_t slot = 0;
    uint8_t zone = 0;
    bool active = false;
    bool acknowledged = false;
    uint32_t raisedTick = 0;
    uint32_t restoredTick = 0;
    String device;
    String description;
};

struct Gc2PanelSecuritySnapshot {
    bool known = false;
    bool acLossInstantaneous = false;
    bool acLossFiltered = false;
    bool acLossLowPower = false;
    bool phoneLineFailure = false;
    bool panelTamper = false;
    bool sirenTamper = false;
    bool rfJam = false;
    bool centralStationFailure = false;
    bool cellularFailure = false;
    bool radioModemNetworkFailure = false;
    bool ethernetNetworkFailure = false;
    bool resetRequired = false;
    uint32_t sampledAtMs = 0;
};

struct Gc2AlarmMemorySnapshot {
    bool known = false;
    bool clear = false;
    bool latched = false;
    bool bellTimeout = false;
    int reportedAlarmType = 0;
    uint32_t sampledAtMs = 0;
};

struct Gc2ZoneSnapshot {
    bool discovered = false;
    bool metadataKnown = false;
    bool enabled = false;
    uint8_t input = 0;
    bool stateKnown = false;
    bool open = false;
    bool bypassKnown = false;
    bool bypassed = false;
    uint8_t bypassUser = 0;
    String bypassType;
    String bypassOrigin = "unknown";
    Gc2ZoneKind kind = Gc2ZoneKind::Unknown;
    // Programmed per-zone chime type from the zone_info "Ch" column
    // (0=none, 1=voice, 2=voice+dingdong, 3=loud dingdong,
    // 4=voice+loud dingdong, 5=dingdong). -1 until zone_info reports it.
    int8_t chimeMode = -1;
    uint32_t rfId = 0;
    uint8_t rawStatus = 0;
    uint8_t rawStatusChange = 0;
    bool batteryKnown = false;
    bool batteryLow = false;
    bool troubleKnown = false;
    bool troubleActive = false;
    bool tamper = false;
    bool supervisionLost = false;
    String troubleSummary;
    uint32_t lastSeenMs = 0;
    uint32_t revision = 0;
    uint32_t metadataRevision = 0;
    String name;
    String zoneType;
    String voiceDescriptor;
};

class Gc2State {
  public:
    static constexpr uint8_t kMaxZones = 75;
    static constexpr uint8_t kMaxTroubles = 32;
    static constexpr size_t kMaxQueuedEvents = 32;

    void notePanelLine(bool recognized);
    void recordZonePacket(uint8_t zone, uint32_t rfId, uint8_t status,
                          uint8_t statusChange, Gc2ZoneKind kind);
    void recordZoneState(uint8_t zone, bool open);
    void recordZoneBattery(uint8_t zone, bool low);
    void beginTroubleSnapshot();
    void recordTrouble(uint8_t slot, uint8_t zone, const String& device,
                       const String& description, bool active,
                       bool acknowledged, uint32_t raisedTick,
                       uint32_t restoredTick);
    void finishTroubleSnapshot();
    bool recordPanelSecurityField(const String& name, int value);
    void beginAlarmMemorySnapshot();
    void recordAlarmMemoryClear();
    void recordAlarmMemoryBellTimeout(bool active);
    void recordAlarmMemoryReportedType(int alarmType);
    void finishAlarmMemorySnapshot();
    void recordZoneBypass(uint8_t zone, bool bypassed, uint8_t user,
                          const String& bypassType, const String& origin);
    void recordZoneName(uint8_t zone, const String& name);
    void recordZoneConfiguration(uint8_t zone, const String& zoneType,
                                 const String& voiceDescriptor,
                                 uint32_t rfId, bool enabled, uint8_t input,
                                 const String& decodedName,
                                 Gc2ZoneKind kind, int chimeMode = -1);
    void recordAlarmState(const String& state,
                          const String& mode = String(),
                          const String& origin = String(), int user = -1,
                          uint32_t flags = 0);
    void recordBattery(const String& state, const String& summary,
                       int millivolts);
    void recordSounderVolume(uint8_t percent);
    void recordFirmware(uint32_t build, const String& version);
    void recordRfSupervision();
    void recordZWaveAck();
    void recordZWaveNoAck();
    void recordZWaveMaxRetry();

    const Gc2ZoneSnapshot& zone(uint8_t number) const;
    const String& alarmState() const { return alarmState_; }
    const String& alarmMode() const { return alarmMode_; }
    const String& alarmOrigin() const { return alarmOrigin_; }
    int alarmUser() const { return alarmUser_; }
    uint32_t alarmFlags() const { return alarmFlags_; }
    uint32_t alarmChangedAtMs() const { return alarmChangedAtMs_; }
    uint32_t alarmRevision() const { return alarmRevision_; }
    const String& batteryState() const { return batteryState_; }
    const String& batterySummary() const { return batterySummary_; }
    int batteryMillivolts() const { return batteryMillivolts_; }
    uint32_t batteryRevision() const { return batteryRevision_; }
    bool sounderVolumeKnown() const { return sounderVolumeKnown_; }
    uint8_t sounderVolumePercent() const { return sounderVolumePercent_; }
    uint32_t sounderVolumeRevision() const { return sounderVolumeRevision_; }
    uint32_t firmwareBuild() const { return firmwareBuild_; }
    const String& firmwareVersion() const { return firmwareVersion_; }
    uint32_t firmwareRevision() const { return firmwareRevision_; }
    uint32_t diagnosticRevision() const { return diagnosticRevision_; }
    uint32_t receivedLineCount() const { return receivedLineCount_; }
    uint32_t unknownLineCount() const { return unknownLineCount_; }
    uint32_t rfSupervisionCount() const { return rfSupervisionCount_; }
    uint32_t zwaveAckCount() const { return zwaveAckCount_; }
    uint32_t zwaveNoAckCount() const { return zwaveNoAckCount_; }
    uint32_t zwaveMaxRetryCount() const { return zwaveMaxRetryCount_; }
    uint32_t droppedEventCount() const { return droppedEventCount_; }
    uint32_t lastPanelLineMs() const { return lastPanelLineMs_; }
    bool troubleKnown() const { return troubleKnown_; }
    uint8_t troubleCount() const { return troubleCount_; }
    uint8_t activeTroubleCount() const { return activeTroubleCount_; }
    uint8_t unacknowledgedTroubleCount() const {
        return unacknowledgedTroubleCount_;
    }
    const Gc2TroubleSnapshot& trouble(uint8_t slot) const;
    uint32_t troubleRevision() const { return troubleRevision_; }
    const Gc2PanelSecuritySnapshot& panelSecurity() const {
        return panelSecurity_;
    }
    uint32_t panelSecurityRevision() const { return panelSecurityRevision_; }
    const Gc2AlarmMemorySnapshot& alarmMemory() const {
        return alarmMemory_;
    }
    uint32_t alarmMemoryRevision() const { return alarmMemoryRevision_; }

    bool peekEvent(String& event) const;
    void popEvent();
    void markAllForRepublish();

  private:
    Gc2ZoneSnapshot zones_[kMaxZones]{};
    String alarmState_ = "unknown";
    String alarmMode_ = "unknown";
    String alarmOrigin_ = "unknown";
    int alarmUser_ = -1;
    uint32_t alarmFlags_ = 0;
    uint32_t alarmChangedAtMs_ = 0;
    uint32_t alarmRevision_ = 0;
    String batteryState_ = "unknown";
    String batterySummary_;
    int batteryMillivolts_ = -1;
    uint32_t batteryRevision_ = 0;
    bool sounderVolumeKnown_ = false;
    uint8_t sounderVolumePercent_ = 0;
    uint32_t sounderVolumeRevision_ = 0;
    uint32_t firmwareBuild_ = 0;
    String firmwareVersion_;
    uint32_t firmwareRevision_ = 0;

    Gc2TroubleSnapshot troubles_[kMaxTroubles]{};
    Gc2TroubleSnapshot pendingTroubles_[kMaxTroubles]{};
    bool troubleSnapshotActive_ = false;
    bool troubleKnown_ = false;
    uint8_t troubleCount_ = 0;
    uint8_t activeTroubleCount_ = 0;
    uint8_t unacknowledgedTroubleCount_ = 0;
    uint32_t troubleRevision_ = 0;
    Gc2PanelSecuritySnapshot panelSecurity_{};
    uint32_t panelSecurityRevision_ = 0;
    Gc2AlarmMemorySnapshot alarmMemory_{};
    bool alarmMemorySnapshotActive_ = false;
    bool alarmMemoryEvidence_ = false;
    bool pendingAlarmMemoryClear_ = false;
    bool pendingAlarmMemoryBellTimeout_ = false;
    int pendingAlarmMemoryReportedType_ = 0;
    uint32_t alarmMemoryRevision_ = 0;

    uint32_t receivedLineCount_ = 0;
    uint32_t unknownLineCount_ = 0;
    uint32_t rfSupervisionCount_ = 0;
    uint32_t zwaveAckCount_ = 0;
    uint32_t zwaveNoAckCount_ = 0;
    uint32_t zwaveMaxRetryCount_ = 0;
    uint32_t droppedEventCount_ = 0;
    uint32_t diagnosticRevision_ = 0;
    uint32_t lastPanelLineMs_ = 0;
    uint32_t nextRevision_ = 1;
    std::deque<String> events_;

    uint32_t newRevision();
    void queueEvent(const String& event);
    void recordZoneTrouble(uint8_t zone, bool active, bool tamper,
                           bool supervisionLost, const String& summary);
};
