#pragma once

#include <Arduino.h>

#include "Gc2State.h"

class Gc2ConsoleParser {
  public:
    explicit Gc2ConsoleParser(Gc2State& state) : state_(state) {}

    void processLine(const String& line);
    void noteBridgeCommand(const String& action);

  private:
    Gc2State& state_;
    uint8_t recentZone_ = 0;
    uint32_t recentZoneAt_ = 0;
    String pendingAlarmMode_ = "unknown";
    String pendingAlarmOrigin_ = "unknown";
    int pendingAlarmUser_ = -1;
    uint32_t pendingAlarmFlags_ = 0;
    uint8_t pendingBypassUser_ = 0;
    String pendingBypassType_;
    String pendingBypassOrigin_ = "unknown";
    String bridgeCommandAction_;
    uint32_t bridgeCommandAt_ = 0;

    static String normalizeState(String value);
    static String collapseSpaces(String value);
    bool parseZonePacket(const String& line);
    bool parseZoneState(const String& line);
    bool parseAlarmState(const String& line);
    bool parseAlarmActivity(const String& line);
    bool parseZoneBypass(const String& line);
    bool consumeBridgeCommand(const String& prefix);
    bool parseBattery(const String& line);
    bool parseBuildInfo(const String& line);
    bool parseChimeName(const String& line);
    bool parseZoneInfo(const String& line);
    static Gc2ZoneKind classifyZone(const String& zoneType,
                                    const String& decodedName);
};
