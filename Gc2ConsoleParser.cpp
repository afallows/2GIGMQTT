#include "Gc2ConsoleParser.h"

#include <ctype.h>
#include <stdio.h>

#include "ZoneVocabulary.h"

namespace {
constexpr uint32_t kSnapshotSettleMs = 2000;

bool parseTrailingInteger(const String& line, int& value) {
    int end = line.length() - 1;
    while (end >= 0 && isspace(static_cast<unsigned char>(line[end]))) --end;
    if (end < 0 || !isdigit(static_cast<unsigned char>(line[end]))) {
        return false;
    }
    int start = end;
    while (start >= 0 && isdigit(static_cast<unsigned char>(line[start]))) {
        --start;
    }
    if (start >= 0 && line[start] == '-') --start;
    value = line.substring(start + 1, end + 1).toInt();
    return true;
}
}  // namespace

String Gc2ConsoleParser::normalizeState(String value) {
    value.trim();
    value.toLowerCase();
    String normalized;
    normalized.reserve(value.length());
    bool previousSeparator = false;
    for (const char character : value) {
        if (isalnum(static_cast<unsigned char>(character))) {
            normalized += character;
            previousSeparator = false;
        } else if (!previousSeparator && !normalized.isEmpty()) {
            normalized += '_';
            previousSeparator = true;
        }
    }
    while (normalized.endsWith("_")) {
        normalized.remove(normalized.length() - 1);
    }
    return normalized;
}

String Gc2ConsoleParser::collapseSpaces(String value) {
    value.trim();
    String collapsed;
    collapsed.reserve(value.length());
    bool inSpace = false;
    for (const char character : value) {
        if (isspace(static_cast<unsigned char>(character))) {
            if (!inSpace && !collapsed.isEmpty()) collapsed += ' ';
            inSpace = true;
        } else {
            collapsed += character;
            inSpace = false;
        }
    }
    collapsed.trim();
    return collapsed;
}

void Gc2ConsoleParser::noteBridgeCommand(const String& action) {
    bridgeCommandAction_ = action;
    bridgeCommandAt_ = millis();
}

bool Gc2ConsoleParser::consumeBridgeCommand(const String& prefix) {
    if (bridgeCommandAction_.isEmpty() ||
        millis() - bridgeCommandAt_ > 15000 ||
        !bridgeCommandAction_.startsWith(prefix)) {
        return false;
    }
    bridgeCommandAction_ = String();
    return true;
}

bool Gc2ConsoleParser::parseZonePacket(const String& line) {
    int marker = line.indexOf("SENSOR --");
    Gc2ZoneKind kind = Gc2ZoneKind::Contact;
    const char* format = "SENSOR -- rf_id %lx zone %u status %x status_change %x";
    if (marker < 0) {
        marker = line.indexOf("PIR --");
        kind = Gc2ZoneKind::Motion;
        format = "PIR -- rf_id %lx zone %u status %x status_change %x";
    }
    if (marker < 0) return false;

    unsigned long rfId = 0;
    unsigned int zone = 0;
    unsigned int status = 0;
    unsigned int statusChange = 0;
    if (sscanf(line.c_str() + marker, format, &rfId, &zone, &status,
               &statusChange) != 4) {
        return false;
    }
    if (zone == 0 || zone >= Gc2State::kMaxZones) return false;
    state_.recordZonePacket(static_cast<uint8_t>(zone),
                            static_cast<uint32_t>(rfId),
                            static_cast<uint8_t>(status),
                            static_cast<uint8_t>(statusChange), kind);
    return true;
}

void Gc2ConsoleParser::beginTroubleSnapshot() {
    state_.beginTroubleSnapshot();
    troubleSnapshotStartedAt_ = millis();
}

void Gc2ConsoleParser::beginAlarmMemorySnapshot() {
    state_.beginAlarmMemorySnapshot();
    alarmMemorySnapshotStartedAt_ = millis();
}

void Gc2ConsoleParser::finishTimedSnapshots() {
    if (troubleSnapshotStartedAt_ != 0 &&
        millis() - troubleSnapshotStartedAt_ >= kSnapshotSettleMs) {
        state_.finishTroubleSnapshot();
        troubleSnapshotStartedAt_ = 0;
    }
    if (alarmMemorySnapshotStartedAt_ != 0 &&
        millis() - alarmMemorySnapshotStartedAt_ >= kSnapshotSettleMs) {
        state_.finishAlarmMemorySnapshot();
        alarmMemorySnapshotStartedAt_ = 0;
    }
}

bool Gc2ConsoleParser::parseZoneSnapshot(const String& line) {
    if (!line.startsWith("Zone ")) return false;

    unsigned int zone = 0;
    if (sscanf(line.c_str(), "Zone %u", &zone) != 1 || zone == 0 ||
        zone >= Gc2State::kMaxZones) {
        return false;
    }

    // Output from the read-only `zones` command has a variable-width type
    // column followed by one of these fixed state words.
    if (line.indexOf(" CLOSED ") >= 0) {
        state_.recordZoneState(static_cast<uint8_t>(zone), false, true);
        state_.recordZoneBypass(static_cast<uint8_t>(zone), false, 0,
                                "snapshot", "panel");
        return true;
    }
    if (line.indexOf(" OPEN ") >= 0) {
        state_.recordZoneState(static_cast<uint8_t>(zone), true, true);
        state_.recordZoneBypass(static_cast<uint8_t>(zone), false, 0,
                                "snapshot", "panel");
        return true;
    }
    if (line.indexOf(" BYPASSED ") >= 0) {
        state_.recordZoneBypass(static_cast<uint8_t>(zone), true, 0,
                                "snapshot", "panel");
        return true;
    }
    return false;
}

bool Gc2ConsoleParser::parseZoneState(const String& line) {
    const int marker = line.indexOf("Zone ");
    if (marker < 0) return false;

    unsigned int zone = 0;
    char state[16] = {};
    if (sscanf(line.c_str() + marker, "Zone %u %15s", &zone, state) != 2 ||
        zone == 0 || zone >= Gc2State::kMaxZones) {
        return false;
    }

    // Live sensor-layer lines:
    //   sensors_handle_345MHz_sensor:  Zone 2 OPENED / RESTORED
    //   sensors_handle_345MHz_pir:     Zone 9 OPENED / CLOSED
    //   sensors_handle_345MHz_sensor:  Zone 2 TAMPERED / TAMPER RESTORE
    //   alarm_handle_zone_tamper_or_lo: Zone 2 TAMPER DETECTED / restored
    if (strcmp(state, "TAMPERED") == 0) {
        state_.recordZoneTamper(static_cast<uint8_t>(zone), true);
        return true;
    }
    if (strcmp(state, "TAMPER") == 0) {
        String rest = line.substring(marker);
        rest.toLowerCase();
        if (rest.indexOf("restore") >= 0) {
            state_.recordZoneTamper(static_cast<uint8_t>(zone), false);
            return true;
        }
        if (rest.indexOf("detected") >= 0) {
            state_.recordZoneTamper(static_cast<uint8_t>(zone), true);
            return true;
        }
        return false;
    }

    bool open = false;
    if (strcmp(state, "OPENED") == 0) {
        open = true;
    } else if (strcmp(state, "RESTORED") != 0 &&
               strcmp(state, "CLOSED") != 0) {
        return false;
    }

    state_.recordZoneState(static_cast<uint8_t>(zone), open);
    recentZone_ = static_cast<uint8_t>(zone);
    recentZoneAt_ = millis();
    return true;
}

bool Gc2ConsoleParser::parseZoneEvent(const String& line) {
    // The alarm engine's own zone event, printed for every open and close
    // independently of the sensor-layer lines above:
    //   alarm_handle_zone_opened_or_cl:  EVENT_ZONE_OPENED  OPEN  on zone 2 while  READY
    int marker = line.indexOf("EVENT_ZONE_OPENED");
    if (marker >= 0) {
        const int zoneAt = line.indexOf("on zone ", marker);
        if (zoneAt < 0) return true;
        const int zone = line.substring(zoneAt + 8).toInt();
        if (zone <= 0 || zone >= Gc2State::kMaxZones) return true;
        const String verb = line.substring(marker + 17, zoneAt);
        if (verb.indexOf("OPEN") >= 0) {
            state_.recordZoneState(static_cast<uint8_t>(zone), true);
        } else if (verb.indexOf("CLOSE") >= 0) {
            state_.recordZoneState(static_cast<uint8_t>(zone), false);
        }
        return true;
    }

    // Live trouble-memory changes for any zone, same description text as the
    // polled table:
    //   trouble_memory_add:  adding Zone 2 Zone Tamper to trouble memory 0
    //   trouble_memory_restore:  restoring trouble Zone Tamper Zone 2
    marker = line.indexOf("trouble_memory_add:");
    if (marker >= 0) {
        const int zoneAt = line.indexOf("adding Zone ", marker);
        const int endAt = line.indexOf(" to trouble memory", marker);
        if (zoneAt < 0 || endAt < zoneAt) return false;
        const int zone = line.substring(zoneAt + 12).toInt();
        int descriptionAt = zoneAt + 12;
        while (descriptionAt < endAt && isDigit(line[descriptionAt])) {
            ++descriptionAt;
        }
        String description = line.substring(descriptionAt, endAt);
        description.trim();
        if (zone > 0 && zone < Gc2State::kMaxZones) {
            state_.recordZoneTroubleLive(static_cast<uint8_t>(zone),
                                         description, true);
        }
        return true;
    }
    marker = line.indexOf("trouble_memory_restore:");
    if (marker >= 0) {
        const int descriptionAt = line.indexOf("restoring trouble ", marker);
        const int zoneAt = line.lastIndexOf(" Zone ");
        if (descriptionAt < 0 || zoneAt < descriptionAt) return false;
        const int zone = line.substring(zoneAt + 6).toInt();
        String description = line.substring(descriptionAt + 18, zoneAt);
        description.trim();
        if (zone > 0 && zone < Gc2State::kMaxZones) {
            state_.recordZoneTroubleLive(static_cast<uint8_t>(zone),
                                         description, false);
        }
        return true;
    }

    // Live panel (enclosure) tamper, otherwise only seen by the 60 s poll:
    //   alarm_handle_panel_tamper:  PANEL TAMPER DETECTED / restored
    marker = line.indexOf("PANEL TAMPER ");
    if (marker >= 0 && line.indexOf("alarm_handle_panel_tamper:") >= 0) {
        String rest = line.substring(marker + 13);
        rest.toLowerCase();
        if (rest.startsWith("detected")) {
            state_.recordPanelSecurityField("panel_tamper", 1);
        } else if (rest.startsWith("restored")) {
            state_.recordPanelSecurityField("panel_tamper", 0);
        }
        return true;
    }
    return false;
}

bool Gc2ConsoleParser::parseZoneTrouble(const String& line) {
    String lowercase = line;
    lowercase.toLowerCase();
    if (!line.startsWith("#") || line.length() < 4 || !isdigit(line[1]) ||
        !isdigit(line[2])) {
        return false;
    }

    const int zoneMarker = lowercase.indexOf("zone ");
    if (zoneMarker < 0) return false;
    int zoneStart = zoneMarker + 5;
    while (zoneStart < static_cast<int>(line.length()) &&
           isspace(static_cast<unsigned char>(line[zoneStart]))) {
        ++zoneStart;
    }
    int zoneEnd = zoneStart;
    while (zoneEnd < static_cast<int>(line.length()) &&
           isdigit(static_cast<unsigned char>(line[zoneEnd]))) {
        ++zoneEnd;
    }
    if (zoneEnd == zoneStart) return false;

    int stateMarker = lowercase.indexOf(" not restored", zoneEnd);
    bool active = true;
    int stateLength = strlen(" not restored");
    if (stateMarker < 0) {
        stateMarker = lowercase.indexOf(" restored", zoneEnd);
        stateLength = strlen(" restored");
        active = false;
    }
    if (stateMarker < 0) return false;

    String identity = collapseSpaces(line.substring(zoneEnd, stateMarker));
    const int separator = identity.indexOf(' ');
    if (separator < 0) return false;
    const String device = identity.substring(0, separator);
    const String description = collapseSpaces(identity.substring(separator + 1));
    if (description.isEmpty()) return false;

    int acknowledgement = stateMarker + stateLength;
    while (acknowledgement < static_cast<int>(line.length()) &&
           isspace(static_cast<unsigned char>(line[acknowledgement]))) {
        ++acknowledgement;
    }
    const String tailLower = lowercase.substring(acknowledgement);
    bool acknowledged = false;
    int acknowledgementLength = 0;
    if (tailLower.startsWith("unack'd")) {
        acknowledgementLength = strlen("unack'd");
    } else if (tailLower.startsWith("ack'd")) {
        acknowledged = true;
        acknowledgementLength = strlen("ack'd");
    } else {
        return false;
    }

    unsigned long raisedTick = 0;
    unsigned long restoredTick = 0;
    if (sscanf(line.c_str() + acknowledgement + acknowledgementLength,
               "%lx %lx", &raisedTick, &restoredTick) != 2) {
        return false;
    }

    const int zone = line.substring(zoneStart, zoneEnd).toInt();
    state_.recordTrouble(
        static_cast<uint8_t>(line.substring(1, 3).toInt()),
        zone >= 0 && zone < Gc2State::kMaxZones
            ? static_cast<uint8_t>(zone)
            : 0,
        device, description, active, acknowledged,
        static_cast<uint32_t>(raisedTick),
        static_cast<uint32_t>(restoredTick));
    return true;
}

bool Gc2ConsoleParser::parsePanelSecurity(const String& line) {
    static constexpr char const* kFields[] = {
        "AC_loss_instantaneous",       "AC_loss_filtered",
        "AC_loss_lpm",                 "phone_line_failure",
        "panel_tamper",                "siren_tamper",
        "RF_jam_detect",               "CS_failure_to_communicate",
        "cell_failure_to_communicate", "radio_modem_network_failure",
        "eb_network_failure",          "reset_required",
    };
    for (const char* field : kFields) {
        const size_t length = strlen(field);
        if (!line.startsWith(field) || line.length() <= length ||
            !isspace(static_cast<unsigned char>(line[length]))) {
            continue;
        }
        String raw = line.substring(length);
        raw.trim();
        if (raw.isEmpty() ||
            (!isdigit(static_cast<unsigned char>(raw[0])) && raw[0] != '-')) {
            return false;
        }
        return state_.recordPanelSecurityField(field, raw.toInt());
    }
    return false;
}

bool Gc2ConsoleParser::parseAlarmMemory(const String& line) {
    String lowercase = line;
    lowercase.toLowerCase();
    if (lowercase.indexOf("alarm memory is clear") >= 0) {
        state_.recordAlarmMemoryClear();
        return true;
    }

    int value = 0;
    if (lowercase.indexOf(
            "alarm_memory_has_bell_timeout_without_clearing") >= 0 &&
        parseTrailingInteger(line, value)) {
        state_.recordAlarmMemoryBellTimeout(value != 0);
        return true;
    }
    if (lowercase.indexOf("alarm_memory_has_reported_alarm_type") >= 0 &&
        parseTrailingInteger(line, value)) {
        state_.recordAlarmMemoryReportedType(value);
        return true;
    }
    return false;
}

bool Gc2ConsoleParser::parseAlarmState(const String& line) {
    const int marker = line.indexOf("alarm_set_state:");
    if (marker < 0) return false;
    const int arrow = line.indexOf("-->", marker);
    if (arrow < 0) return false;
    const String next = normalizeState(line.substring(arrow + 3));
    if (next.isEmpty()) return false;
    if (next == "arming") {
        state_.recordAlarmState("arming", pendingAlarmMode_,
                                pendingAlarmOrigin_, pendingAlarmUser_,
                                pendingAlarmFlags_);
    } else if (next == "armed") {
        String armedState;
        if (pendingAlarmMode_ == "stay") {
            armedState = F("armed_home");
        } else if (pendingAlarmMode_ == "away") {
            armedState = F("armed_away");
        } else if (state_.alarmState() == "armed_home" ||
                   state_.alarmState() == "armed_away") {
            armedState = state_.alarmState();
        } else {
            // Keep Home Assistant in the transitional state until the
            // panel's system_armed_s/system_armed_a phrase identifies mode.
            armedState = F("arming");
        }
        state_.recordAlarmState(armedState, pendingAlarmMode_,
                                pendingAlarmOrigin_, pendingAlarmUser_,
                                pendingAlarmFlags_);
    } else if (next == "ready" || next == "not_ready") {
        state_.recordAlarmState("disarmed", "none", pendingAlarmOrigin_,
                                pendingAlarmUser_, 0);
    } else {
        // Unknown firmware-specific alarm states are recognized but are not
        // published as invalid Home Assistant alarm-control-panel states.
        return true;
    }
    return true;
}

bool Gc2ConsoleParser::parseAlarmActivity(const String& line) {
    int marker = line.indexOf("alarm_handle_arm:");
    if (marker >= 0) {
        unsigned int user = 0;
        unsigned long flags = 0;
        if (sscanf(line.c_str() + marker,
                   "alarm_handle_arm: arm attempt: user %u arming_flags 0x%lx",
                   &user, &flags) == 2) {
            pendingAlarmFlags_ = static_cast<uint32_t>(flags);
            pendingAlarmMode_ = (pendingAlarmFlags_ & 0x04U) != 0
                                    ? String("stay")
                                    : String("away");
            pendingAlarmOrigin_ = consumeBridgeCommand("arm_")
                                      ? String("bridge_mqtt")
                                      : (pendingAlarmFlags_ & 0x0800U) != 0
                                            ? String("alarm_dot_com")
                                            : String("local");
            pendingAlarmUser_ = static_cast<int>(user);
        }
        return true;
    }

    marker = line.indexOf("alarm_handle_disarm:");
    if (marker >= 0) {
        unsigned int user = 0;
        if (sscanf(line.c_str() + marker,
                   "alarm_handle_disarm: COMMAND_ALARM_DISARM by user %u",
                   &user) == 1) {
            pendingAlarmUser_ = static_cast<int>(user);
            pendingAlarmOrigin_ = consumeBridgeCommand("disarm")
                                      ? String("bridge_mqtt")
                                      : user == 0 ? String("alarm_dot_com")
                                                  : String("local");
            pendingAlarmMode_ = F("none");
            pendingAlarmFlags_ = 0;
        }
        return true;
    }

    if (line.indexOf("system_armed_s") >= 0) {
        pendingAlarmMode_ = F("stay");
        state_.recordAlarmState("armed_home", pendingAlarmMode_,
                                pendingAlarmOrigin_, pendingAlarmUser_,
                                pendingAlarmFlags_);
        return true;
    }
    if (line.indexOf("system_armed_a") >= 0) {
        pendingAlarmMode_ = F("away");
        state_.recordAlarmState("armed_away", pendingAlarmMode_,
                                pendingAlarmOrigin_, pendingAlarmUser_,
                                pendingAlarmFlags_);
        return true;
    }
    if (line.indexOf("system_disarmed-ready_to_arm") >= 0) {
        pendingAlarmMode_ = F("none");
        pendingAlarmFlags_ = 0;
        state_.recordAlarmState("disarmed", pendingAlarmMode_,
                                pendingAlarmOrigin_, pendingAlarmUser_, 0);
        return true;
    }
    return false;
}

bool Gc2ConsoleParser::parseZoneBypass(const String& line) {
    int marker = line.indexOf("alarm_handle_quick_bypass:");
    if (marker >= 0) {
        unsigned int user = 0;
        unsigned int type = 0;
        if (sscanf(line.c_str() + marker,
                   "alarm_handle_quick_bypass: ALARM_QUICK_BYPASS User %u Type %u",
                   &user, &type) == 2) {
            pendingBypassUser_ = static_cast<uint8_t>(user);
            pendingBypassType_ = String("quick_") + type;
            pendingBypassOrigin_ = F("local");
        }
        return true;
    }

    marker = line.indexOf("alarm_bypass_zone:");
    if (marker >= 0) {
        unsigned int zone = 0;
        char type[24] = {};
        if (sscanf(line.c_str() + marker,
                   "alarm_bypass_zone: bypassing zone %u %23s", &zone,
                   type) >= 1 &&
            zone > 0 && zone < Gc2State::kMaxZones) {
            String bypassType = type[0] == '\0' ? pendingBypassType_
                                                 : normalizeState(type);
            if (bypassType.isEmpty()) bypassType = F("unknown");
            if (consumeBridgeCommand("bypass_")) {
                pendingBypassOrigin_ = F("bridge_mqtt");
                pendingBypassUser_ = 0;
            }
            state_.recordZoneBypass(static_cast<uint8_t>(zone), true,
                                    pendingBypassUser_, bypassType,
                                    pendingBypassOrigin_);
        }
        return true;
    }

    // Different firmware builds use both "unbypass" and "unbypassing" in
    // their diagnostic names. Do not clear a bypass on an ordinary sensor
    // restore; wait for an explicit bypass-clearing line.
    String lowercase = line;
    lowercase.toLowerCase();
    if (lowercase.indexOf("unbypass") >= 0) {
        const int zoneMarker = lowercase.indexOf("zone ");
        if (zoneMarker >= 0) {
            const int zone = lowercase.substring(zoneMarker + 5).toInt();
            if (zone > 0 && zone < Gc2State::kMaxZones) {
                const String origin = consumeBridgeCommand("unbypass_")
                                          ? String("bridge_mqtt")
                                          : String("local");
                if (origin == "bridge_mqtt") pendingBypassUser_ = 0;
                state_.recordZoneBypass(static_cast<uint8_t>(zone), false,
                                        pendingBypassUser_, "cleared", origin);
            }
        }
        return true;
    }
    return false;
}

bool Gc2ConsoleParser::parseBattery(const String& line) {
    if (!line.startsWith("CH[")) return false;
    const int closeBracket = line.indexOf(']');
    const int unit = line.lastIndexOf(" mV");
    if (closeBracket < 0 || unit < 0 || unit <= closeBracket) return false;

    int numberStart = unit - 1;
    while (numberStart >= 0 && isdigit(line[numberStart])) --numberStart;
    ++numberStart;
    if (numberStart >= unit) return false;

    const int millivolts = line.substring(numberStart, unit).toInt();
    const String summary = collapseSpaces(
        line.substring(closeBracket + 1, numberStart));
    String condition = F("ok");
    String lowercase = summary;
    lowercase.toLowerCase();
    if (lowercase.indexOf("no battery") >= 0) {
        condition = F("none");
    } else if (lowercase.indexOf("low battery") >= 0) {
        condition = F("low");
    } else if (lowercase.indexOf("battery problem") >= 0) {
        condition = F("problem");
    } else if (lowercase.indexOf("wait for ac") >= 0) {
        condition = F("ac_wait");
    }
    state_.recordBattery(condition, summary, millivolts);
    return true;
}

bool Gc2ConsoleParser::parseBuildInfo(const String& line) {
    if (!line.startsWith("BUILDINFO:")) return false;
    unsigned long build = 0;
    char version[80] = {};
    if (sscanf(line.c_str(), "BUILDINFO: %lu %79s", &build, version) != 2) {
        return false;
    }
    state_.recordFirmware(static_cast<uint32_t>(build), String(version));
    return true;
}

bool Gc2ConsoleParser::parseChimeName(const String& line) {
    if (recentZone_ == 0 || millis() - recentZoneAt_ > 3000) return false;
    const int chime = line.indexOf("Chime:");
    if (chime < 0) return false;
    const int volume = line.indexOf("vol", chime);
    if (volume < 0) return false;
    int nameStart = volume + 3;
    while (nameStart < static_cast<int>(line.length()) &&
           isspace(static_cast<unsigned char>(line[nameStart]))) {
        ++nameStart;
    }
    const int volumeStart = nameStart;
    while (nameStart < static_cast<int>(line.length()) &&
           isdigit(static_cast<unsigned char>(line[nameStart]))) {
        ++nameStart;
    }
    if (nameStart > volumeStart) {
        const int appliedVolume = line.substring(volumeStart, nameStart).toInt();
        if (appliedVolume >= 0 && appliedVolume <= 100) {
            state_.recordSounderVolume(static_cast<uint8_t>(appliedVolume));
        }
    }
    while (nameStart < static_cast<int>(line.length()) &&
           (isspace(static_cast<unsigned char>(line[nameStart])) ||
            isdigit(static_cast<unsigned char>(line[nameStart])))) {
        ++nameStart;
    }
    String name = collapseSpaces(line.substring(nameStart));
    String lowercase = name;
    lowercase.toLowerCase();
    if (lowercase.startsWith("chime")) {
        int prefixEnd = strlen("chime");
        while (prefixEnd < static_cast<int>(name.length()) &&
               (name[prefixEnd] == '_' || name[prefixEnd] == '-' ||
                isspace(static_cast<unsigned char>(name[prefixEnd])))) {
            ++prefixEnd;
        }
        const int indexStart = prefixEnd;
        while (prefixEnd < static_cast<int>(name.length()) &&
               isdigit(static_cast<unsigned char>(name[prefixEnd]))) {
            ++prefixEnd;
        }
        const bool hasChimeIndex = prefixEnd > indexStart;
        while (prefixEnd < static_cast<int>(name.length()) &&
               (name[prefixEnd] == '_' || name[prefixEnd] == '-' ||
                isspace(static_cast<unsigned char>(name[prefixEnd])))) {
            ++prefixEnd;
        }
        if (hasChimeIndex && prefixEnd < static_cast<int>(name.length())) {
            name = name.substring(prefixEnd);
        }
    }
    name.replace('-', ' ');
    name.replace('_', ' ');
    name = collapseSpaces(name);
    if (name.isEmpty()) return false;
    state_.recordZoneName(recentZone_, name);
    return true;
}

Gc2ZoneKind Gc2ConsoleParser::classifyZone(const String& zoneType,
                                           const String& decodedName) {
    String description = zoneType + ' ' + decodedName;
    description.toLowerCase();
    if (description.indexOf("motion") >= 0 ||
        description.indexOf("interior") >= 0) {
        return Gc2ZoneKind::Motion;
    }
    if (description.indexOf("fire") >= 0 ||
        description.indexOf("smoke") >= 0 ||
        description.indexOf("carbon monoxide") >= 0) {
        return Gc2ZoneKind::Smoke;
    }
    return Gc2ZoneKind::Contact;
}

bool Gc2ConsoleParser::parseZoneInfo(const String& line) {
    if (line.length() < 2 || !isdigit(line[0]) || !isdigit(line[1]) ||
        (line.length() > 2 && !isspace(static_cast<unsigned char>(line[2])))) {
        return false;
    }

    String tokens[20];
    size_t tokenCount = 0;
    int position = 0;
    while (position < static_cast<int>(line.length()) && tokenCount < 20) {
        while (position < static_cast<int>(line.length()) &&
               isspace(static_cast<unsigned char>(line[position]))) {
            ++position;
        }
        if (position >= static_cast<int>(line.length())) break;
        const int start = position;
        while (position < static_cast<int>(line.length()) &&
               !isspace(static_cast<unsigned char>(line[position]))) {
            ++position;
        }
        tokens[tokenCount++] = line.substring(start, position);
    }

    // zone + one-or-more type tokens + the 12 fixed zone_info columns.
    if (tokenCount < 14) return false;
    const size_t physicalIndex = tokenCount - 12;
    if (physicalIndex <= 1 || tokens[physicalIndex + 2].length() < 15 ||
        tokens[physicalIndex + 2].length() > 16) {
        return false;
    }

    const uint8_t zone = static_cast<uint8_t>(tokens[0].toInt());
    if (zone >= Gc2State::kMaxZones) return false;
    String zoneType;
    for (size_t index = 1; index < physicalIndex; ++index) {
        if (!zoneType.isEmpty()) zoneType += ' ';
        zoneType += tokens[index];
    }

    const String voiceDescriptor = tokens[physicalIndex + 2];
    const uint32_t rfId =
        static_cast<uint32_t>(strtoul(tokens[physicalIndex + 3].c_str(),
                                      nullptr, 16));
    const bool enabled = tokens[physicalIndex + 4].toInt() != 0;
    const uint8_t input =
        static_cast<uint8_t>(tokens[physicalIndex + 5].toInt());
    // Column order after RFid: En In CZ Ch Em Op Tx Eq. "Ch" is the
    // programmed chime type written by the console's zone_chime command.
    const String& chimeToken = tokens[physicalIndex + 7];
    int chimeMode = -1;
    if (!chimeToken.isEmpty() &&
        isdigit(static_cast<unsigned char>(chimeToken[0]))) {
        chimeMode = chimeToken.toInt();
    }
    const String decodedName = ZoneVocabulary::decode(voiceDescriptor);
    state_.recordZoneConfiguration(
        zone, zoneType, voiceDescriptor, rfId, enabled, input, decodedName,
        classifyZone(zoneType, decodedName), chimeMode);
    return true;
}

bool Gc2ConsoleParser::parseSounderVolume(const String& line) {
    String lowercase = line;
    lowercase.toLowerCase();

    // A raw "sounder_volume N" line is the console echo of our request, not
    // proof that the panel applied it. The sounder-status command can also
    // report a transient DVT/output-channel volume (commonly zero), which is
    // not the chime/announcement master volume. Only master_volume traces and
    // live phrases are authoritative for this entity.
    int valueStart = -1;
    const int phraseAt = lowercase.indexOf("sounder_play_phrase:");
    const int phraseVolumeAt =
        phraseAt < 0 ? -1 : lowercase.indexOf(" vol ", phraseAt);
    if (phraseVolumeAt >= 0) {
        valueStart = phraseVolumeAt + strlen(" vol ");
    } else {
        for (const char* marker : {
                 "setting master_volume to", "setting master volume to",
                 "master_volume:", "master_volume=", "master volume:",
                 "master volume=", "master_volume ", "master volume "}) {
            const int markerAt = lowercase.indexOf(marker);
            if (markerAt >= 0) {
                valueStart = markerAt + strlen(marker);
                break;
            }
        }
    }
    if (valueStart < 0) return false;
    while (valueStart < static_cast<int>(line.length()) &&
           isspace(static_cast<unsigned char>(line[valueStart]))) {
        ++valueStart;
    }
    if (valueStart >= static_cast<int>(line.length()) ||
        !isdigit(static_cast<unsigned char>(line[valueStart]))) {
        return false;
    }
    unsigned int value = 0;
    while (valueStart < static_cast<int>(line.length()) &&
           isdigit(static_cast<unsigned char>(line[valueStart]))) {
        value = value * 10 + static_cast<unsigned int>(line[valueStart] - '0');
        ++valueStart;
    }
    if (value > 100) return false;
    state_.recordSounderVolume(static_cast<uint8_t>(value));
    return true;
}

void Gc2ConsoleParser::processLine(const String& input) {
    String line = input;
    line.trim();
    if (line.isEmpty()) return;

    bool recognized = parseZoneInfo(line);
    if (!recognized) recognized = parseZonePacket(line);
    if (!recognized) recognized = parseZoneSnapshot(line);
    if (!recognized) recognized = parseZoneState(line);
    if (!recognized) recognized = parseZoneEvent(line);
    if (!recognized) recognized = parseZoneTrouble(line);
    if (!recognized) recognized = parsePanelSecurity(line);
    if (!recognized) recognized = parseAlarmMemory(line);
    if (!recognized) recognized = parseZoneBypass(line);
    if (!recognized) recognized = parseAlarmActivity(line);
    if (!recognized) recognized = parseAlarmState(line);
    if (!recognized) recognized = parseBattery(line);
    if (!recognized) recognized = parseChimeName(line);
    if (!recognized) recognized = parseSounderVolume(line);
    if (!recognized) recognized = parseBuildInfo(line);

    if (line.indexOf("SW supervisory packet received") >= 0) {
        state_.recordRfSupervision();
        recognized = true;
    }
    if (line.indexOf("Unsecure Tx Done (ACK)") >= 0) {
        state_.recordZWaveAck();
        recognized = true;
    }
    if (line.indexOf("TRANSMIT_COMPLETE_NO_ACK") >= 0) {
        state_.recordZWaveNoAck();
        recognized = true;
    }
    if (line.indexOf("max TX retries") >= 0) {
        state_.recordZWaveMaxRetry();
        recognized = true;
    }

    // These are known derivative/noise lines. They remain available over
    // Telnet but are deliberately not promoted into automation entities.
    for (const char* knownNoise : {
             "panel_aux_output_enable:", "panel_handle_task_vote_change:",
             "sounder_play_phrase:", "Pop:", "ToPegMessage:",
             "alarm_handle_ordinary_open_zon:", "zwave_"}) {
        if (line.indexOf(knownNoise) >= 0) {
            recognized = true;
            break;
        }
    }

    state_.notePanelLine(recognized);
}
