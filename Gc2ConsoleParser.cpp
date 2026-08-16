#include "Gc2ConsoleParser.h"

#include <ctype.h>
#include <stdio.h>

#include "ZoneVocabulary.h"

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

bool Gc2ConsoleParser::parseZoneState(const String& line) {
    const int marker = line.indexOf("Zone ");
    if (marker < 0) return false;

    unsigned int zone = 0;
    char state[16] = {};
    if (sscanf(line.c_str() + marker, "Zone %u %15s", &zone, state) != 2 ||
        zone == 0 || zone >= Gc2State::kMaxZones) {
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
           (isspace(static_cast<unsigned char>(line[nameStart])) ||
            isdigit(static_cast<unsigned char>(line[nameStart])))) {
        ++nameStart;
    }
    String name = collapseSpaces(line.substring(nameStart));
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
    const String decodedName = ZoneVocabulary::decode(voiceDescriptor);
    state_.recordZoneConfiguration(
        zone, zoneType, voiceDescriptor, rfId, enabled, input, decodedName,
        classifyZone(zoneType, decodedName));
    return true;
}

void Gc2ConsoleParser::processLine(const String& input) {
    String line = input;
    line.trim();
    if (line.isEmpty()) return;

    bool recognized = parseZoneInfo(line);
    if (!recognized) recognized = parseZonePacket(line);
    if (!recognized) recognized = parseZoneState(line);
    if (!recognized) recognized = parseZoneBypass(line);
    if (!recognized) recognized = parseAlarmActivity(line);
    if (!recognized) recognized = parseAlarmState(line);
    if (!recognized) recognized = parseBattery(line);
    if (!recognized) recognized = parseBuildInfo(line);
    if (!recognized) recognized = parseChimeName(line);

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
             "alarm_handle_zone_opened_or_cl:",
             "alarm_handle_ordinary_open_zon:", "zwave_"}) {
        if (line.indexOf(knownNoise) >= 0) {
            recognized = true;
            break;
        }
    }

    state_.notePanelLine(recognized);
}
