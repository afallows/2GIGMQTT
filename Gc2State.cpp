#include "Gc2State.h"

namespace {
const Gc2ZoneSnapshot kEmptyZone{};
const Gc2TroubleSnapshot kEmptyTrouble{};

bool troubleEquals(const Gc2TroubleSnapshot& left,
                   const Gc2TroubleSnapshot& right) {
    return left.valid == right.valid && left.slot == right.slot &&
           left.zone == right.zone && left.active == right.active &&
           left.acknowledged == right.acknowledged &&
           left.raisedTick == right.raisedTick &&
           left.restoredTick == right.restoredTick &&
           left.device == right.device &&
           left.description == right.description;
}
}

uint32_t Gc2State::newRevision() {
    if (++nextRevision_ == 0) nextRevision_ = 1;
    return nextRevision_;
}

void Gc2State::notePanelLine(bool recognized) {
    ++receivedLineCount_;
    if (!recognized) ++unknownLineCount_;
    lastPanelLineMs_ = millis();
    diagnosticRevision_ = newRevision();
}

void Gc2State::recordZonePacket(uint8_t number, uint32_t rfId,
                                uint8_t status, uint8_t statusChange,
                                Gc2ZoneKind kind) {
    if (number == 0 || number >= kMaxZones) return;
    Gc2ZoneSnapshot& target = zones_[number];
    const bool metadataChanged = !target.discovered || target.rfId != rfId ||
                                 (kind != Gc2ZoneKind::Unknown &&
                                  target.kind != kind);
    target.discovered = true;
    target.rfId = rfId;
    target.rawStatus = status;
    target.rawStatusChange = statusChange;
    target.lastSeenMs = millis();
    if (kind != Gc2ZoneKind::Unknown) target.kind = kind;
    target.revision = newRevision();
    if (metadataChanged) target.metadataRevision = newRevision();

    // The GC2's normalized 345 MHz event byte uses bit 7 for the active/open
    // condition and bit 3 for low battery. A supervisory packet therefore
    // establishes current state even when no OPENED/RESTORED text followed it.
    recordZoneState(number, (status & 0x80U) != 0);
    recordZoneBattery(number, (status & 0x08U) != 0);
}

void Gc2State::recordZoneState(uint8_t number, bool open) {
    if (number == 0 || number >= kMaxZones) return;
    Gc2ZoneSnapshot& target = zones_[number];
    const bool changed = !target.stateKnown || target.open != open;
    target.discovered = true;
    target.stateKnown = true;
    target.open = open;
    target.lastSeenMs = millis();
    if (!changed) return;

    target.revision = newRevision();
    String event = F("{\"type\":\"zone\",\"zone\":");
    event += number;
    event += F(",\"state\":\"");
    event += open ? F("open") : F("closed");
    event += F("\",\"uptime_ms\":");
    event += millis();
    event += '}';
    queueEvent(event);
}

void Gc2State::recordZoneBattery(uint8_t number, bool low) {
    if (number == 0 || number >= kMaxZones) return;
    Gc2ZoneSnapshot& target = zones_[number];
    const bool changed = !target.batteryKnown || target.batteryLow != low;
    target.discovered = true;
    target.batteryKnown = true;
    target.batteryLow = low;
    target.lastSeenMs = millis();
    if (!changed) return;
    target.revision = newRevision();

    String event = F("{\"type\":\"zone_battery\",\"zone\":");
    event += number;
    event += F(",\"low\":");
    event += low ? F("true") : F("false");
    event += F(",\"uptime_ms\":");
    event += millis();
    event += '}';
    queueEvent(event);
}

void Gc2State::beginTroubleSnapshot() {
    for (uint8_t slot = 0; slot < kMaxTroubles; ++slot) {
        pendingTroubles_[slot] = Gc2TroubleSnapshot{};
    }
    troubleSnapshotActive_ = true;
}

void Gc2State::recordTrouble(uint8_t slot, uint8_t zone,
                             const String& device,
                             const String& description, bool active,
                             bool acknowledged, uint32_t raisedTick,
                             uint32_t restoredTick) {
    if (slot >= kMaxTroubles) return;
    if (!troubleSnapshotActive_) beginTroubleSnapshot();
    Gc2TroubleSnapshot& target = pendingTroubles_[slot];
    target.valid = true;
    target.slot = slot;
    target.zone = zone;
    target.device = device;
    target.description = description;
    target.active = active;
    target.acknowledged = acknowledged;
    target.raisedTick = raisedTick;
    target.restoredTick = restoredTick;
}

void Gc2State::recordZoneTrouble(uint8_t number, bool active, bool tamper,
                                 bool supervisionLost,
                                 const String& summary) {
    if (number == 0 || number >= kMaxZones) return;
    Gc2ZoneSnapshot& target = zones_[number];
    const bool changed = !target.troubleKnown ||
                         target.troubleActive != active ||
                         target.tamper != tamper ||
                         target.supervisionLost != supervisionLost ||
                         target.troubleSummary != summary;
    target.troubleKnown = true;
    target.troubleActive = active;
    target.tamper = tamper;
    target.supervisionLost = supervisionLost;
    target.troubleSummary = summary;
    if (changed && target.discovered) target.revision = newRevision();
}

void Gc2State::finishTroubleSnapshot() {
    if (!troubleSnapshotActive_) return;
    troubleSnapshotActive_ = false;

    bool changed = !troubleKnown_;
    uint8_t total = 0;
    uint8_t active = 0;
    uint8_t unacknowledged = 0;
    for (uint8_t slot = 0; slot < kMaxTroubles; ++slot) {
        if (!troubleEquals(troubles_[slot], pendingTroubles_[slot])) {
            changed = true;
        }
        troubles_[slot] = pendingTroubles_[slot];
        if (!troubles_[slot].valid) continue;
        ++total;
        if (troubles_[slot].active) {
            ++active;
            if (!troubles_[slot].acknowledged) ++unacknowledged;
        }
    }

    troubleKnown_ = true;
    troubleCount_ = total;
    activeTroubleCount_ = active;
    unacknowledgedTroubleCount_ = unacknowledged;
    if (changed) {
        troubleRevision_ = newRevision();
        String event = F("{\"type\":\"trouble_snapshot\",\"active_count\":");
        event += active;
        event += F(",\"unacknowledged_count\":");
        event += unacknowledged;
        event += F(",\"uptime_ms\":");
        event += millis();
        event += '}';
        queueEvent(event);
    }

    for (uint8_t zone = 1; zone < kMaxZones; ++zone) {
        bool zoneActive = false;
        bool lowBattery = false;
        bool tamper = false;
        bool supervisionLost = false;
        String summary;
        for (uint8_t slot = 0; slot < kMaxTroubles; ++slot) {
            const Gc2TroubleSnapshot& entry = troubles_[slot];
            if (!entry.valid || !entry.active || entry.zone != zone) continue;
            zoneActive = true;
            String lowercase = entry.description;
            lowercase.toLowerCase();
            if (lowercase.indexOf("low battery") >= 0) lowBattery = true;
            if (lowercase.indexOf("tamper") >= 0) tamper = true;
            if (lowercase.indexOf("loss of supervision") >= 0) {
                supervisionLost = true;
            }
            if (!summary.isEmpty()) summary += F("; ");
            summary += entry.description;
        }
        recordZoneTrouble(zone, zoneActive, tamper, supervisionLost, summary);
        if (zones_[zone].discovered &&
            (zones_[zone].batteryKnown || lowBattery)) {
            recordZoneBattery(zone, lowBattery);
        }
    }
}

const Gc2TroubleSnapshot& Gc2State::trouble(uint8_t slot) const {
    return slot < kMaxTroubles ? troubles_[slot] : kEmptyTrouble;
}

bool Gc2State::recordPanelSecurityField(const String& name, int value) {
    bool* target = nullptr;
    if (name == "AC_loss_instantaneous") {
        target = &panelSecurity_.acLossInstantaneous;
    } else if (name == "AC_loss_filtered") {
        target = &panelSecurity_.acLossFiltered;
    } else if (name == "AC_loss_lpm") {
        target = &panelSecurity_.acLossLowPower;
    } else if (name == "phone_line_failure") {
        target = &panelSecurity_.phoneLineFailure;
    } else if (name == "panel_tamper") {
        target = &panelSecurity_.panelTamper;
    } else if (name == "siren_tamper") {
        target = &panelSecurity_.sirenTamper;
    } else if (name == "RF_jam_detect") {
        target = &panelSecurity_.rfJam;
    } else if (name == "CS_failure_to_communicate") {
        target = &panelSecurity_.centralStationFailure;
    } else if (name == "cell_failure_to_communicate") {
        target = &panelSecurity_.cellularFailure;
    } else if (name == "radio_modem_network_failure") {
        target = &panelSecurity_.radioModemNetworkFailure;
    } else if (name == "eb_network_failure") {
        target = &panelSecurity_.ethernetNetworkFailure;
    } else if (name == "reset_required") {
        target = &panelSecurity_.resetRequired;
    } else {
        return false;
    }

    const bool next = value != 0;
    const bool changed = !panelSecurity_.known || *target != next;
    *target = next;
    panelSecurity_.known = true;
    panelSecurity_.sampledAtMs = millis();
    if (changed) panelSecurityRevision_ = newRevision();
    return true;
}

void Gc2State::beginAlarmMemorySnapshot() {
    alarmMemorySnapshotActive_ = true;
    alarmMemoryEvidence_ = false;
    pendingAlarmMemoryClear_ = false;
    pendingAlarmMemoryBellTimeout_ = false;
    pendingAlarmMemoryReportedType_ = 0;
}

void Gc2State::recordAlarmMemoryClear() {
    if (!alarmMemorySnapshotActive_) beginAlarmMemorySnapshot();
    pendingAlarmMemoryClear_ = true;
    alarmMemoryEvidence_ = true;
}

void Gc2State::recordAlarmMemoryBellTimeout(bool active) {
    if (!alarmMemorySnapshotActive_) beginAlarmMemorySnapshot();
    pendingAlarmMemoryBellTimeout_ = active;
    alarmMemoryEvidence_ = true;
}

void Gc2State::recordAlarmMemoryReportedType(int alarmType) {
    if (!alarmMemorySnapshotActive_) beginAlarmMemorySnapshot();
    pendingAlarmMemoryReportedType_ = alarmType;
    alarmMemoryEvidence_ = true;
}

void Gc2State::finishAlarmMemorySnapshot() {
    if (!alarmMemorySnapshotActive_) return;
    alarmMemorySnapshotActive_ = false;
    if (!alarmMemoryEvidence_) return;

    const bool latched = !pendingAlarmMemoryClear_ &&
                         (pendingAlarmMemoryBellTimeout_ ||
                          pendingAlarmMemoryReportedType_ != 0);
    const bool clear = pendingAlarmMemoryClear_ || !latched;
    const bool changed = !alarmMemory_.known || alarmMemory_.clear != clear ||
                         alarmMemory_.latched != latched ||
                         alarmMemory_.bellTimeout !=
                             pendingAlarmMemoryBellTimeout_ ||
                         alarmMemory_.reportedAlarmType !=
                             pendingAlarmMemoryReportedType_;
    alarmMemory_.known = true;
    alarmMemory_.clear = clear;
    alarmMemory_.latched = latched;
    alarmMemory_.bellTimeout = pendingAlarmMemoryBellTimeout_;
    alarmMemory_.reportedAlarmType = pendingAlarmMemoryReportedType_;
    alarmMemory_.sampledAtMs = millis();
    if (changed) alarmMemoryRevision_ = newRevision();
}

void Gc2State::recordZoneBypass(uint8_t number, bool bypassed, uint8_t user,
                                const String& bypassType,
                                const String& origin) {
    if (number == 0 || number >= kMaxZones) return;
    Gc2ZoneSnapshot& target = zones_[number];
    const bool changed = !target.bypassKnown || target.bypassed != bypassed ||
                         target.bypassUser != user ||
                         target.bypassType != bypassType ||
                         target.bypassOrigin != origin;
    target.discovered = true;
    target.bypassKnown = true;
    target.bypassed = bypassed;
    target.bypassUser = user;
    target.bypassType = bypassType;
    target.bypassOrigin = origin;
    target.lastSeenMs = millis();
    if (!changed) return;
    target.revision = newRevision();

    String event = F("{\"type\":\"zone_bypass\",\"zone\":");
    event += number;
    event += F(",\"bypassed\":");
    event += bypassed ? F("true") : F("false");
    event += F(",\"user\":");
    event += user;
    event += F(",\"bypass_type\":\"");
    event += bypassType;
    event += F("\",\"origin\":\"");
    event += origin;
    event += F("\",\"uptime_ms\":");
    event += millis();
    event += '}';
    queueEvent(event);
}

void Gc2State::recordZoneName(uint8_t number, const String& name) {
    if (number == 0 || number >= kMaxZones || name.isEmpty()) return;
    Gc2ZoneSnapshot& target = zones_[number];
    if (target.name == name) return;
    target.discovered = true;
    target.name = name;
    target.revision = newRevision();
    target.metadataRevision = newRevision();
}

void Gc2State::recordZoneConfiguration(
    uint8_t number, const String& zoneType, const String& voiceDescriptor,
    uint32_t rfId, bool enabled, uint8_t input, const String& decodedName,
    Gc2ZoneKind kind) {
    if (number >= kMaxZones) return;
    Gc2ZoneSnapshot& target = zones_[number];
    const bool active = number > 0 && enabled && input > 0;
    const bool changed = !target.metadataKnown || target.discovered != active ||
                         target.enabled != enabled || target.input != input ||
                         target.zoneType != zoneType ||
                         target.voiceDescriptor != voiceDescriptor ||
                         (rfId != 0 && target.rfId != rfId) ||
                         (!decodedName.isEmpty() && target.name != decodedName) ||
                         (kind != Gc2ZoneKind::Unknown && target.kind != kind);
    target.metadataKnown = true;
    target.discovered = active;
    target.enabled = enabled;
    target.input = input;
    target.zoneType = zoneType;
    target.voiceDescriptor = voiceDescriptor;
    if (rfId != 0) target.rfId = rfId;
    if (!decodedName.isEmpty()) target.name = decodedName;
    if (kind != Gc2ZoneKind::Unknown) target.kind = kind;
    if (changed) {
        target.revision = newRevision();
        target.metadataRevision = newRevision();
    }
}

void Gc2State::recordAlarmState(const String& state, const String& mode,
                                const String& origin, int user,
                                uint32_t flags) {
    if (state.isEmpty()) return;
    const String nextMode = mode.isEmpty() ? alarmMode_ : mode;
    const String nextOrigin = origin.isEmpty() ? alarmOrigin_ : origin;
    const int nextUser = user < 0 ? alarmUser_ : user;
    const bool changed = alarmState_ != state || alarmMode_ != nextMode ||
                         alarmOrigin_ != nextOrigin || alarmUser_ != nextUser ||
                         alarmFlags_ != flags;
    if (!changed) return;
    alarmState_ = state;
    alarmMode_ = nextMode;
    alarmOrigin_ = nextOrigin;
    alarmUser_ = nextUser;
    alarmFlags_ = flags;
    alarmChangedAtMs_ = millis();
    alarmRevision_ = newRevision();

    String event = F("{\"type\":\"panel_state\",\"state\":\"");
    event += state;
    event += F("\",\"mode\":\"");
    event += alarmMode_;
    event += F("\",\"origin\":\"");
    event += alarmOrigin_;
    event += F("\",\"user\":");
    if (alarmUser_ < 0) {
        event += F("null");
    } else {
        event += alarmUser_;
    }
    event += F(",\"flags\":");
    event += alarmFlags_;
    event += F(",\"uptime_ms\":");
    event += millis();
    event += '}';
    queueEvent(event);
}

void Gc2State::recordBattery(const String& state, const String& summary,
                             int millivolts) {
    if (batteryState_ == state && batterySummary_ == summary &&
        batteryMillivolts_ == millivolts) {
        return;
    }
    batteryState_ = state;
    batterySummary_ = summary;
    batteryMillivolts_ = millivolts;
    batteryRevision_ = newRevision();
}

void Gc2State::recordSounderVolume(uint8_t percent) {
    if (percent > 100 ||
        (sounderVolumeKnown_ && sounderVolumePercent_ == percent)) {
        return;
    }
    sounderVolumeKnown_ = true;
    sounderVolumePercent_ = percent;
    sounderVolumeRevision_ = newRevision();

    String event = F("{\"type\":\"sounder_volume\",\"percent\":");
    event += percent;
    event += F(",\"uptime_ms\":");
    event += millis();
    event += '}';
    queueEvent(event);
}

void Gc2State::recordFirmware(uint32_t build, const String& version) {
    if (firmwareBuild_ == build && firmwareVersion_ == version) return;
    firmwareBuild_ = build;
    firmwareVersion_ = version;
    firmwareRevision_ = newRevision();
}

void Gc2State::recordRfSupervision() {
    ++rfSupervisionCount_;
    diagnosticRevision_ = newRevision();
}

void Gc2State::recordZWaveAck() {
    ++zwaveAckCount_;
    diagnosticRevision_ = newRevision();
}

void Gc2State::recordZWaveNoAck() {
    ++zwaveNoAckCount_;
    diagnosticRevision_ = newRevision();
}

void Gc2State::recordZWaveMaxRetry() {
    ++zwaveMaxRetryCount_;
    diagnosticRevision_ = newRevision();
}

const Gc2ZoneSnapshot& Gc2State::zone(uint8_t number) const {
    return number < kMaxZones ? zones_[number] : kEmptyZone;
}

bool Gc2State::peekEvent(String& event) const {
    if (events_.empty()) return false;
    event = events_.front();
    return true;
}

void Gc2State::popEvent() {
    if (!events_.empty()) events_.pop_front();
}

void Gc2State::queueEvent(const String& event) {
    if (events_.size() >= kMaxQueuedEvents) {
        events_.pop_front();
        ++droppedEventCount_;
        diagnosticRevision_ = newRevision();
    }
    events_.push_back(event);
}

void Gc2State::markAllForRepublish() {
    alarmRevision_ = newRevision();
    if (batteryMillivolts_ >= 0) batteryRevision_ = newRevision();
    if (sounderVolumeKnown_) sounderVolumeRevision_ = newRevision();
    if (!firmwareVersion_.isEmpty()) firmwareRevision_ = newRevision();
    if (troubleKnown_) troubleRevision_ = newRevision();
    if (panelSecurity_.known) panelSecurityRevision_ = newRevision();
    if (alarmMemory_.known) alarmMemoryRevision_ = newRevision();
    diagnosticRevision_ = newRevision();
    for (uint8_t number = 1; number < kMaxZones; ++number) {
        if (zones_[number].discovered) zones_[number].revision = newRevision();
    }
}
