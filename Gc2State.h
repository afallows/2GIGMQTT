#pragma once

#include <Arduino.h>

#include <deque>

enum class Gc2ZoneKind : uint8_t { Unknown, Contact, Motion, Smoke };

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
    uint32_t rfId = 0;
    uint8_t rawStatus = 0;
    uint8_t rawStatusChange = 0;
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
    static constexpr size_t kMaxQueuedEvents = 32;

    void notePanelLine(bool recognized);
    void recordZonePacket(uint8_t zone, uint32_t rfId, uint8_t status,
                          uint8_t statusChange, Gc2ZoneKind kind);
    void recordZoneState(uint8_t zone, bool open);
    void recordZoneBypass(uint8_t zone, bool bypassed, uint8_t user,
                          const String& bypassType, const String& origin);
    void recordZoneName(uint8_t zone, const String& name);
    void recordZoneConfiguration(uint8_t zone, const String& zoneType,
                                 const String& voiceDescriptor,
                                 uint32_t rfId, bool enabled, uint8_t input,
                                 const String& decodedName,
                                 Gc2ZoneKind kind);
    void recordAlarmState(const String& state,
                          const String& mode = String(),
                          const String& origin = String(), int user = -1,
                          uint32_t flags = 0);
    void recordBattery(const String& state, const String& summary,
                       int millivolts);
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
    uint32_t firmwareBuild_ = 0;
    String firmwareVersion_;
    uint32_t firmwareRevision_ = 0;

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
};
