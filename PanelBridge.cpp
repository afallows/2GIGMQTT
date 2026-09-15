#include "PanelBridge.h"

#include "AppConfig.h"

#include <driver/uart.h>
#include <esp_idf_version.h>

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "GC2 Bridge requires Arduino-ESP32 3.3.x (ESP-IDF 5.5) or newer."
#endif

namespace {
constexpr uint8_t kTelnetIac = 255;
constexpr uint8_t kTelnetDo = 253;
constexpr uint8_t kTelnetDont = 254;
constexpr uint8_t kTelnetWill = 251;
constexpr uint8_t kTelnetWont = 252;
constexpr uint8_t kTelnetSubBegin = 250;
constexpr uint8_t kTelnetSubEnd = 240;
constexpr uint8_t kTelnetEcho = 1;
constexpr uint8_t kTelnetSuppressGoAhead = 3;

bool isHexDigit(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

uint8_t hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return value - 'A' + 10;
}

bool parseEightHexDigitsAfter(const String& line, const String& marker,
                              uint32_t& output) {
    int position = line.indexOf(marker);
    if (position < 0) return false;
    position += marker.length();

    while (position < static_cast<int>(line.length()) &&
           !isHexDigit(line[position])) {
        ++position;
    }
    if (position + 8 > static_cast<int>(line.length())) return false;

    uint32_t value = 0;
    for (int index = 0; index < 8; ++index) {
        if (!isHexDigit(line[position + index])) return false;
        value = (value << 4) | hexValue(line[position + index]);
    }
    if (value == 0 || value == 0xFFFFFFFFUL) return false;
    output = value;
    return true;
}

bool parseRtcDate(const String& line, uint8_t& month, uint8_t& day,
                  uint16_t& year) {
    const int rtcPosition = line.indexOf("RTC");
    if (rtcPosition < 0) return false;

    for (int position = rtcPosition;
         position + 9 < static_cast<int>(line.length()); ++position) {
        if (!isDigit(line[position])) continue;

        int firstSlash = line.indexOf('/', position);
        if (firstSlash < 0 || firstSlash - position > 2) continue;
        int secondSlash = line.indexOf('/', firstSlash + 1);
        if (secondSlash < 0 || secondSlash - firstSlash > 3) continue;

        const int parsedMonth = line.substring(position, firstSlash).toInt();
        const int parsedDay =
            line.substring(firstSlash + 1, secondSlash).toInt();
        int end = secondSlash + 1;
        while (end < static_cast<int>(line.length()) && isDigit(line[end])) {
            ++end;
        }
        const int parsedYear = line.substring(secondSlash + 1, end).toInt();
        if (parsedMonth < 1 || parsedMonth > 12 || parsedDay < 1 ||
            parsedDay > 31 || parsedYear < 2000 || parsedYear > 2099) {
            continue;
        }

        month = parsedMonth;
        day = parsedDay;
        year = parsedYear;
        return true;
    }
    return false;
}
}  // namespace

PanelBridge::PanelBridge(CredentialStore& credentials,
                         Gc2ConsoleParser& parser)
    : credentials_(credentials),
      parser_(parser),
      panel_(Serial1),
      server_(AppConfig::kTelnetPort) {}

void PanelBridge::begin() {
    // GPIO6 is connected to a live panel bus that may also be driven by the
    // 345 MHz receiver.  Keep it electrically high-impedance until an
    // intentional command is sent; merely avoiding write() is insufficient
    // because an attached UART TX actively drives the idle-high state.
    panel_.end();
    pinMode(AppConfig::kPanelTxPin, INPUT);

    bootTime_ = millis();
    baudStateStartedAt_ = bootTime_;
    panelLine_.reserve(512);
    validationLine_.reserve(512);
    telnetLine_.reserve(AppConfig::kMaxConsoleCommandLength);

    Serial.print(F("[uart] Passive baud detection starts "));
    Serial.print(AppConfig::kBaudDetectDelayMs / 1000);
    Serial.println(F(" seconds after power-up."));
}

bool PanelBridge::deadlineReached(uint32_t deadline) {
    return static_cast<int32_t>(millis() - deadline) >= 0;
}

void PanelBridge::setNetworkReady(bool ready) {
    networkReady_ = ready;
    if (ready && !serverStarted_) {
        server_.begin();
        server_.setNoDelay(true);
        serverStarted_ = true;
        Serial.print(F("[telnet] Listening on port "));
        Serial.println(AppConfig::kTelnetPort);
    }
    if (ready && panelReady()) {
        // Re-read retained alarm memory when Home Assistant connectivity
        // returns, in case an alarm occurred while MQTT was unavailable.
        nextAlarmMemoryPollAt_ = millis() + AppConfig::kCommandIntervalMs;
    }
    if (!ready) disconnectClient();
}

void PanelBridge::stopNetwork() {
    disconnectClient();
    if (serverStarted_) {
        server_.end();
        serverStarted_ = false;
    }
    networkReady_ = false;
}

void PanelBridge::loop() {
    processBaudDetection();
    if (baudState_ == BaudState::Validating) {
        processBaudValidation();
    }
    if (panelReady()) {
        processPanelInput();
        parser_.finishTimedSnapshots();
        processUnlock();
        processUnlockSupervisor();
        processAlarmControl();
        processMonitorPolling();
    }
    processServer();
    processManualCommandQueue();
}

void PanelBridge::processBaudDetection() {
    switch (baudState_) {
        case BaudState::Waiting:
            if (millis() - bootTime_ >= AppConfig::kBaudDetectDelayMs) {
                startBaudValidation();
            }
            break;

        case BaudState::RetryDelay:
            if (millis() - baudStateStartedAt_ >=
                AppConfig::kBaudRetryDelayMs) {
                startBaudValidation();
            }
            break;

        case BaudState::Validating:
        case BaudState::Ready: break;
    }
}

bool PanelBridge::validBaud(uint32_t baud) {
    return baud > 0 && baud <= UART_BITRATE_MAX;
}

void PanelBridge::addBaudCandidate(uint32_t baud) {
    if (!validBaud(baud) || baudCandidateCount_ >= kMaxBaudCandidates) return;
    for (uint8_t index = 0; index < baudCandidateCount_; ++index) {
        if (baudCandidates_[index] == baud) return;
    }
    baudCandidates_[baudCandidateCount_++] = baud;
}

void PanelBridge::startBaudValidation() {
    baudCandidateCount_ = 0;
    baudCandidateIndex_ = 0;
    // Hardware edge timing on ordinary ASCII repeatedly produced integer
    // harmonics. Automatic selection is therefore based solely on decoded
    // panel evidence at the three rates observed on GC2-family bench units.
    addBaudCandidate(19200);
    addBaudCandidate(115200);
    addBaudCandidate(38400);
    startCurrentBaudCandidate();
}

void PanelBridge::startCurrentBaudCandidate() {
    if (baudCandidateIndex_ >= baudCandidateCount_) {
        baudState_ = BaudState::RetryDelay;
        baudStateStartedAt_ = millis();
        trialBaud_ = 0;
        sendClientStatus(
            "No baud candidate produced readable panel text; restarting the fixed trial sequence.");
        return;
    }

    panel_.end();
    pinMode(AppConfig::kPanelTxPin, INPUT);
    panel_.setRxBufferSize(AppConfig::kPanelRxBufferSize);
    panel_.setTxBufferSize(AppConfig::kPanelTxBufferSize);
    trialBaud_ = baudCandidates_[baudCandidateIndex_];
    panel_.begin(trialBaud_, SERIAL_8N1, AppConfig::kPanelRxPin,
                 -1);
    baudState_ = BaudState::Validating;
    baudStateStartedAt_ = millis();
    validationByteCount_ = 0;
    validationPrintableCount_ = 0;
    validationLine_ = String();
    validationLine_.reserve(512);

    Serial.print(F("[uart] Passively validating candidate "));
    Serial.print(trialBaud_);
    Serial.println(F(" baud for up to 60 seconds; UART transmission remains blocked."));
    sendClientStatus(String("Passively testing ") + trialBaud_ +
                     " baud for readable panel text.");
}

bool PanelBridge::plausiblePanelText(const String& input) {
    String line = input;
    line.trim();
    if (line.length() < 6) return false;

    String lowercase = line;
    lowercase.toLowerCase();
    for (const char* marker : {
             "buildinfo:", "sensor --", "pir --", "zone ",
             "alarm_", "panel_", "sensors_", "zwave_", "sounder_",
             "topegmessage:", "supervisory packet", "powerup ",
             "debug_lock_state", "battery", " mv", "firmware",
             "version", "serial number"}) {
        if (lowercase.indexOf(marker) >= 0) return true;
    }
    if (lowercase.startsWith("event") ||
        lowercase.indexOf(" event ") >= 0 ||
        lowercase.indexOf(" event:") >= 0) {
        return true;
    }

    // Most runtime GC2-family logs use [hours:minutes:seconds.fraction].
    // Validate the shape rather than any exact timestamp or message text so
    // different firmware revisions and panel programming remain compatible.
    if (line[0] == '[') {
        const int close = line.indexOf(']');
        if (close >= 6 && close <= 20) {
            uint8_t colons = 0;
            bool timestampValid = true;
            for (int index = 1; index < close; ++index) {
                const char character = line[index];
                if (character == ':') {
                    ++colons;
                } else if (character != '.' && !isDigit(character)) {
                    timestampValid = false;
                    break;
                }
            }
            uint8_t lettersAfterTimestamp = 0;
            for (int index = close + 1;
                 index < static_cast<int>(line.length()); ++index) {
                if (isAlpha(line[index])) ++lettersAfterTimestamp;
            }
            if (timestampValid && colons >= 2 &&
                lettersAfterTimestamp >= 3) {
                return true;
            }
        }
    }
    return false;
}

void PanelBridge::processBaudValidation() {
    while (panel_.available() > 0) {
        const int value = panel_.read();
        if (value < 0) break;
        const uint8_t byte = static_cast<uint8_t>(value);
        ++validationByteCount_;

        const bool textual = (byte >= 0x20 && byte <= 0x7E) || byte == '\r' ||
                             byte == '\n' || byte == '\t';
        if (textual) ++validationPrintableCount_;

        if (byte == '\r' || byte == '\n') {
            if (!validationLine_.isEmpty()) {
                const bool streamIsText =
                    validationPrintableCount_ * 100U >=
                    validationByteCount_ * 85U;
                if (streamIsText && plausiblePanelText(validationLine_)) {
                    const String evidence = validationLine_;
                    acceptCurrentBaudCandidate(evidence);
                    return;
                }
                validationLine_ = String();
                validationLine_.reserve(512);
            }
        } else if (byte >= 0x20 && byte <= 0x7E) {
            if (validationLine_.length() < 511) {
                validationLine_ += static_cast<char>(byte);
            }
        } else if (byte != '\t') {
            validationLine_ = String();
            validationLine_.reserve(512);
        }
    }

    if (millis() - baudStateStartedAt_ >=
        AppConfig::kBaudValidationWindowMs) {
        rejectCurrentBaudCandidate(
            "no recognizable panel text in the 60-second trial");
    }
}

void PanelBridge::rejectCurrentBaudCandidate(const char* reason) {
    Serial.print(F("[uart] Candidate "));
    Serial.print(trialBaud_);
    Serial.print(F(" rejected: "));
    Serial.println(reason);
    panel_.end();
    pinMode(AppConfig::kPanelTxPin, INPUT);
    ++baudCandidateIndex_;
    startCurrentBaudCandidate();
}

void PanelBridge::acceptCurrentBaudCandidate(const String& evidence) {
    currentBaud_ = trialBaud_;
    baudState_ = BaudState::Ready;
    unlockPhase_ = UnlockPhase::NotStarted;
    nextUnlockActionAt_ = 0;
    nextUnlockSupervisionAt_ =
        millis() + AppConfig::kDebugUnlockInitialDelayMs;
    nextZoneMetadataPollAt_ =
        millis() + AppConfig::kZoneMetadataInitialDelayMs;
    nextZoneStatePollAt_ = millis() + AppConfig::kZoneStateInitialDelayMs;
    nextZoneTroublePollAt_ =
        millis() + AppConfig::kZoneTroubleInitialDelayMs;
    nextSounderStatusPollAt_ =
        millis() + AppConfig::kSounderStatusInitialDelayMs;
    nextPanelStatusPollAt_ =
        millis() + AppConfig::kPanelStatusInitialDelayMs;
    nextAlarmMemoryPollAt_ =
        millis() + AppConfig::kAlarmMemoryInitialDelayMs;
    panelLine_ = String();
    panelLine_.reserve(512);

    Serial.print(F("[uart] Validated "));
    Serial.print(currentBaud_);
    Serial.print(F(" baud from readable panel text: "));
    Serial.println(evidence.substring(0, 120));
    sendClientStatus(String("UART validated at ") + currentBaud_ +
                     " baud from readable panel text.");
    inspectPanelLine(evidence);
}

void PanelBridge::restartBaudDetection() {
    if (baudState_ == BaudState::Ready ||
        baudState_ == BaudState::Validating) {
        panel_.end();
    }
    pinMode(AppConfig::kPanelTxPin, INPUT);
    currentBaud_ = 0;
    baudState_ = BaudState::RetryDelay;
    baudStateStartedAt_ = millis() - AppConfig::kBaudRetryDelayMs;
    panelLine_ = String();
    panelLine_.reserve(512);
    commandQueue_.clear();
    unlockPhase_ = UnlockPhase::NotStarted;
    nextUnlockActionAt_ = 0;
    nextUnlockSupervisionAt_ = 0;
    nextZoneMetadataPollAt_ = 0;
    nextZoneStatePollAt_ = 0;
    nextZoneTroublePollAt_ = 0;
    nextSounderStatusPollAt_ = 0;
    nextPanelStatusPollAt_ = 0;
    nextAlarmMemoryPollAt_ = 0;
    trialBaud_ = 0;
    validationLine_ = String();
}

void PanelBridge::requestAutoBaud() { restartBaudDetection(); }

bool PanelBridge::setBaudOverride(uint32_t baud) {
    if (!validBaud(baud)) return false;
    if (panelReady() && currentBaud_ == baud) return true;
    startPanelUart(baud);
    return true;
}

String PanelBridge::baudState() const {
    return panelReady() ? String(currentBaud_) : String("detecting");
}

void PanelBridge::startPanelUart(uint32_t baud) {
    if (baudState_ == BaudState::Ready ||
        baudState_ == BaudState::Validating) {
        panel_.end();
    }
    pinMode(AppConfig::kPanelTxPin, INPUT);
    panel_.setRxBufferSize(AppConfig::kPanelRxBufferSize);
    panel_.setTxBufferSize(AppConfig::kPanelTxBufferSize);
    panel_.begin(baud, SERIAL_8N1, AppConfig::kPanelRxPin,
                 -1);
    // Count driver-level overflow so lost panel bytes are visible in the
    // diagnostics instead of silently missing a zone transition. The
    // callback runs on the UART event task; it only bumps a counter.
    panel_.onReceiveError([this](hardwareSerial_error_t) {
        ++uartErrorCount_;
    });

    currentBaud_ = baud;
    baudState_ = BaudState::Ready;
    // Monitor mode maintains debug access and permits the narrow, read-only
    // zone metadata poll. Strict zero-transmit mode remains available through
    // /listen on.
    unlockPhase_ = UnlockPhase::NotStarted;
    nextUnlockActionAt_ = 0;
    nextUnlockSupervisionAt_ =
        millis() + AppConfig::kDebugUnlockInitialDelayMs;
    nextZoneMetadataPollAt_ =
        millis() + AppConfig::kZoneMetadataInitialDelayMs;
    nextZoneStatePollAt_ = millis() + AppConfig::kZoneStateInitialDelayMs;
    nextZoneTroublePollAt_ =
        millis() + AppConfig::kZoneTroubleInitialDelayMs;
    nextSounderStatusPollAt_ =
        millis() + AppConfig::kSounderStatusInitialDelayMs;
    nextPanelStatusPollAt_ =
        millis() + AppConfig::kPanelStatusInitialDelayMs;
    nextAlarmMemoryPollAt_ =
        millis() + AppConfig::kAlarmMemoryInitialDelayMs;
    Serial.print(F("[uart] Panel UART set to "));
    Serial.print(baud);
    Serial.println(F(" baud, 8N1."));
    sendClientStatus(String("UART detected at ") + baud + " baud.");
}

void PanelBridge::processPanelInput() {
    uint8_t input[512];
    while (panel_.available() > 0) {
        const int count = panel_.read(input, sizeof(input));
        if (count <= 0) break;
        for (int index = 0; index < count; ++index) {
            processPanelByte(input[index]);
        }
        // Parsing/state updates always happen before a potentially slow TCP
        // client is serviced, so Telnet backpressure cannot delay conditioning
        // of bytes that have already been removed from the UART FIFO.
        writePanelDataToClient(input, count);
    }
}

void PanelBridge::processPanelByte(uint8_t value) {
    if (value == '\r' || value == '\n') {
        if (!panelLine_.isEmpty()) {
            inspectPanelLine(panelLine_);
            panelLine_ = String();
            panelLine_.reserve(512);
        }
        return;
    }

    if ((value >= 0x20 && value <= 0x7E) || value == '\t') {
        if (panelLine_.length() < 511) {
            panelLine_ += static_cast<char>(value);
        } else {
            panelLine_ = String();
            panelLine_.reserve(512);
        }
    }
}

void PanelBridge::inspectPanelLine(const String& line) {
    parser_.processLine(line);

    if (line.indexOf("TROUBLE_MEMORY_UPDATED") >= 0) {
        nextZoneTroublePollAt_ = millis();
    }
    if (line.indexOf("ALARM_MEMORY_UPDATED") >= 0) {
        nextAlarmMemoryPollAt_ = millis();
    }

    String lowercase = line;
    lowercase.toLowerCase();
    if ((unlockPhase_ == UnlockPhase::WaitUnlock ||
         unlockPhase_ == UnlockPhase::SendVerification ||
         unlockPhase_ == UnlockPhase::WaitVerification) &&
        (lowercase.indexOf("unlocked") >= 0 ||
         lowercase.indexOf("not locked") >= 0)) {
        debugLockState_ = 2;
    }

    uint32_t serialNumber = 0;
    if (line.indexOf("PowerUp Serial Number") < 0 &&
        (parseEightHexDigitsAfter(line, " SN ", serialNumber) ||
         parseEightHexDigitsAfter(line, "Serial Number", serialNumber) ||
         parseEightHexDigitsAfter(line, "XCVR network ID", serialNumber))) {
        panelSerialNumber_ = serialNumber;
    }

    uint8_t month = 0;
    uint8_t day = 0;
    uint16_t year = 0;
    if (parseRtcDate(line, month, day, year)) {
        panelMonth_ = month;
        panelDay_ = day;
        panelYear_ = year;
    }

    const int lockPosition = line.indexOf("debug_lock_state");
    if (lockPosition >= 0) {
        int position = lockPosition + strlen("debug_lock_state");
        while (position < static_cast<int>(line.length()) &&
               !isDigit(line[position])) {
            ++position;
        }
        if (position < static_cast<int>(line.length())) {
            debugLockState_ = line.substring(position).toInt();
        }
    }

    if (line.indexOf("is not available when debug is locked") >= 0) {
        debugLockState_ = 1;
        if (unlockPhase_ == UnlockPhase::Complete ||
            unlockPhase_ == UnlockPhase::Failed ||
            unlockPhase_ == UnlockPhase::NotStarted) {
            unlockPhase_ = UnlockPhase::NotStarted;
            nextUnlockSupervisionAt_ = millis();
            sendClientStatus(
                "Panel debug access is locked; automatic re-unlock scheduled.");
        }
    }
    if (lowercase.indexOf("debug is now locked") >= 0) {
        debugLockState_ = 1;
        unlockPhase_ = UnlockPhase::NotStarted;
        nextUnlockSupervisionAt_ = millis();
    }
}

bool PanelBridge::attachPanelTx() {
    if (!panel_.setPins(-1, AppConfig::kPanelTxPin)) {
        Serial.println(F("[uart] Could not attach panel TX pin."));
        releasePanelTx();
        return false;
    }
    return true;
}

void PanelBridge::releasePanelTx() {
    // pinMode claims the pad from the UART peripheral manager and disconnects
    // the output matrix, returning the pin to a true high-impedance input.
    pinMode(AppConfig::kPanelTxPin, INPUT);
}

bool PanelBridge::sendPanelCommand(const String& command) {
    if (txMode_ != TxMode::Maintenance) return false;
    return sendBridgeCommand(command);
}

bool PanelBridge::sendBridgeCommand(const String& command) {
    if (txMode_ == TxMode::Passive || !panelReady() ||
        millis() - lastPanelCommandAt_ < AppConfig::kCommandIntervalMs) {
        return false;
    }
    if (!attachPanelTx()) return false;
    panel_.write(reinterpret_cast<const uint8_t*>(command.c_str()),
                 command.length());
    panel_.write('\r');
    panel_.flush(true);
    releasePanelTx();
    lastPanelCommandAt_ = millis();
    return true;
}

bool PanelBridge::sendMonitorCommand(const char* command) {
    if (txMode_ != TxMode::Monitor || command == nullptr) return false;
    return sendBridgeCommand(String(command));
}

void PanelBridge::processMonitorPolling() {
    if (txMode_ != TxMode::Monitor ||
        alarmControlPhase_ != AlarmControlPhase::Idle ||
        (unlockPhase_ != UnlockPhase::NotStarted &&
         unlockPhase_ != UnlockPhase::Complete &&
         unlockPhase_ != UnlockPhase::Failed)) {
        return;
    }

    if (deadlineReached(nextZoneMetadataPollAt_) &&
        sendMonitorCommand("zone_info 1")) {
        nextZoneMetadataPollAt_ = millis() + AppConfig::kZoneMetadataRefreshMs;
        sendClientStatus(
            "Refreshing read-only zone names and programming metadata.");
        return;
    }
    if (deadlineReached(nextZoneStatePollAt_) &&
        sendMonitorCommand("zones")) {
        nextZoneStatePollAt_ = millis() + AppConfig::kZoneStateRefreshMs;
        return;
    }
    if (deadlineReached(nextZoneTroublePollAt_) &&
        sendMonitorCommand("trouble_memory")) {
        parser_.beginTroubleSnapshot();
        nextZoneTroublePollAt_ = millis() + AppConfig::kZoneTroubleRefreshMs;
        return;
    }
    if (deadlineReached(nextSounderStatusPollAt_) &&
        sendMonitorCommand("sounder_status")) {
        nextSounderStatusPollAt_ =
            millis() + AppConfig::kSounderStatusRefreshMs;
        return;
    }
    if (deadlineReached(nextPanelStatusPollAt_) &&
        sendMonitorCommand("panel_status")) {
        nextPanelStatusPollAt_ = millis() + AppConfig::kPanelStatusRefreshMs;
        return;
    }
    if (deadlineReached(nextAlarmMemoryPollAt_) &&
        sendMonitorCommand("alarm_memory")) {
        parser_.beginAlarmMemorySnapshot();
        nextAlarmMemoryPollAt_ = millis() + AppConfig::kAlarmMemoryRefreshMs;
    }
}

uint32_t PanelBridge::calculateUnlockCode(uint32_t serialNumber, uint8_t day,
                                          uint8_t month, uint16_t year) {
    const uint8_t input[] = {
        static_cast<uint8_t>(serialNumber >> 24),
        static_cast<uint8_t>(serialNumber >> 16),
        static_cast<uint8_t>(serialNumber >> 8),
        static_cast<uint8_t>(serialNumber),
        day,
        month,
        static_cast<uint8_t>(year % 100),
        0x12,
        0x34,
        0x56,
        0x78,
    };

    uint32_t hash = 0x811C9DC5UL;
    for (const uint8_t value : input) {
        hash ^= value;
        hash *= 0x01000193UL;
    }
    return hash;
}

void PanelBridge::processUnlock() {
    if (!deadlineReached(nextUnlockActionAt_)) return;

    switch (unlockPhase_) {
        case UnlockPhase::NotStarted:
        case UnlockPhase::Complete:
        case UnlockPhase::Failed: return;

        case UnlockPhase::SendCalibration:
            if (sendBridgeCommand("cal_values")) {
                unlockPhase_ = UnlockPhase::WaitCalibration;
                nextUnlockActionAt_ = millis() + 6500;
            }
            break;

        case UnlockPhase::WaitCalibration:
            unlockPhase_ = UnlockPhase::SendBuildInfo;
            nextUnlockActionAt_ = millis();
            break;

        case UnlockPhase::SendBuildInfo:
            if (sendBridgeCommand("build_info")) {
                unlockPhase_ = UnlockPhase::WaitBuildInfo;
                nextUnlockActionAt_ = millis() + 6000;
            }
            break;

        case UnlockPhase::WaitBuildInfo:
            unlockPhase_ = UnlockPhase::SendRtc;
            nextUnlockActionAt_ = millis();
            break;

        case UnlockPhase::SendRtc:
            if (sendBridgeCommand("rtc")) {
                unlockPhase_ = UnlockPhase::WaitRtc;
                nextUnlockActionAt_ = millis() + 6500;
            }
            break;

        case UnlockPhase::WaitRtc:
            unlockPhase_ = UnlockPhase::EvaluateIdentity;
            nextUnlockActionAt_ = millis();
            break;

        case UnlockPhase::EvaluateIdentity:
            if (panelSerialNumber_ != 0 && panelYear_ != 0) {
                unlockPhase_ = UnlockPhase::SendUnlock;
                nextUnlockActionAt_ = millis();
            } else if (++identityAttempts_ < 3) {
                unlockPhase_ = UnlockPhase::SendCalibration;
                nextUnlockActionAt_ = millis() + 3000;
            } else {
                unlockPhase_ = UnlockPhase::Failed;
                nextUnlockSupervisionAt_ =
                    millis() + AppConfig::kDebugUnlockRetryIntervalMs;
                Serial.println(F("[unlock] Could not obtain panel identity and RTC date."));
                sendClientStatus(
                    "Automatic unlock could not read the panel identity/date; another attempt will be made in 30 seconds.");
            }
            break;

        case UnlockPhase::SendUnlock: {
            char command[24];
            const uint32_t code = calculateUnlockCode(
                panelSerialNumber_, panelDay_, panelMonth_, panelYear_);
            snprintf(command, sizeof(command), "unlock %08lx",
                     static_cast<unsigned long>(code));
            if (sendBridgeCommand(command)) {
                debugLockState_ = -1;
                ++unlockAttempts_;
                unlockPhase_ = UnlockPhase::WaitUnlock;
                nextUnlockActionAt_ = millis() + 1500;
                Serial.println(F("[unlock] Daily service code submitted."));
            }
            memset(command, 0, sizeof(command));
            break;
        }

        case UnlockPhase::WaitUnlock:
            unlockPhase_ = UnlockPhase::SendVerification;
            nextUnlockActionAt_ = millis();
            break;

        case UnlockPhase::SendVerification:
            if (sendBridgeCommand("panel_misc")) {
                ++unlockVerificationAttempts_;
                unlockPhase_ = UnlockPhase::WaitVerification;
                nextUnlockActionAt_ = millis() + 6000;
            }
            break;

        case UnlockPhase::WaitVerification:
            if (debugLockState_ == 2) {
                unlockPhase_ = UnlockPhase::Complete;
                unlockAttempts_ = 0;
                unlockVerificationAttempts_ = 0;
                nextUnlockSupervisionAt_ =
                    millis() + AppConfig::kDebugUnlockCheckIntervalMs;
                Serial.println(F("[unlock] Panel reports the maintenance console is unlocked."));
                sendClientStatus(
                    "Panel maintenance console unlocked; persistent supervision active.");
            } else if (debugLockState_ == 1 && unlockAttempts_ < 3) {
                unlockPhase_ = UnlockPhase::SendUnlock;
                nextUnlockActionAt_ = millis() + 5000;
            } else if (debugLockState_ < 0 &&
                       unlockVerificationAttempts_ < 3) {
                unlockPhase_ = UnlockPhase::SendVerification;
                nextUnlockActionAt_ = millis() + 2000;
            } else {
                unlockPhase_ = UnlockPhase::Failed;
                nextUnlockSupervisionAt_ =
                    millis() + AppConfig::kDebugUnlockRetryIntervalMs;
                Serial.println(F("[unlock] Persistent debug unlock could not be verified."));
                sendClientStatus(
                    "Automatic unlock failed; another attempt will be made in 30 seconds.");
            }
            break;
    }
}

void PanelBridge::processUnlockSupervisor() {
    if (txMode_ == TxMode::Passive ||
        alarmControlPhase_ != AlarmControlPhase::Idle ||
        !deadlineReached(nextUnlockSupervisionAt_)) {
        return;
    }
    if (unlockPhase_ != UnlockPhase::NotStarted &&
        unlockPhase_ != UnlockPhase::Complete &&
        unlockPhase_ != UnlockPhase::Failed) {
        return;
    }

    if (debugLockState_ == 2 && unlockPhase_ == UnlockPhase::Complete) {
        debugLockState_ = -1;
        unlockVerificationAttempts_ = 0;
        unlockPhase_ = UnlockPhase::SendVerification;
        nextUnlockActionAt_ = millis();
        nextUnlockSupervisionAt_ = UINT32_MAX;
        Serial.println(F("[unlock] Verifying persistent debug access."));
        return;
    }

    scheduleAutomaticUnlock();
    Serial.println(F("[unlock] Starting persistent debug unlock."));
}

bool PanelBridge::requestAlarmCommand(AlarmCommand command) {
    String requestedAction;
    String requestedText;
    switch (command) {
        case AlarmCommand::ArmHome:
            requestedAction = F("arm_home");
            requestedText = F("arm_stay 0");
            break;
        case AlarmCommand::ArmAway:
            requestedAction = F("arm_away");
            requestedText = F("arm_away 0");
            break;
        case AlarmCommand::Disarm:
            requestedAction = F("disarm");
            requestedText = F("disarm 0");
            break;
    }

    return beginProtectedCommand(requestedAction, requestedText);
}

bool PanelBridge::requestZoneBypass(uint8_t zone, bool bypassed) {
    if (zone == 0 || zone >= Gc2State::kMaxZones) return false;
    char action[32];
    char command[24];
    snprintf(action, sizeof(action), "%s_zone_%02u",
             bypassed ? "bypass" : "unbypass", zone);
    snprintf(command, sizeof(command), "%s %u",
             bypassed ? "bypass" : "unbypass", zone);
    return beginProtectedCommand(action, command);
}

bool PanelBridge::requestZoneChime(uint8_t zone, uint8_t mode) {
    // zone_chime <zone 0-74> <type>; type 0=none, 1=voice,
    // 2=voice+dingdong, 3=loud dingdong, 4=voice+loud dingdong, 5=dingdong.
    // The panel commits the zone record to its settings flash, so callers
    // must only request a value that differs from the retained one.
    if (zone == 0 || zone >= Gc2State::kMaxZones || mode > 5) return false;
    char action[32];
    char command[24];
    snprintf(action, sizeof(action), "zone_chime_%02u_%u", zone, mode);
    snprintf(command, sizeof(command), "zone_chime %u %u", zone, mode);
    return beginProtectedCommand(action, command);
}

bool PanelBridge::requestSounderVolume(uint8_t percent) {
    if (percent > 100) return false;
    char action[24];
    char command[24];
    snprintf(action, sizeof(action), "sounder_volume_%u", percent);
    snprintf(command, sizeof(command), "sounder_volume %u", percent);
    const bool accepted = beginProtectedCommand(action, command);
    if (accepted) {
        nextSounderStatusPollAt_ = millis() + AppConfig::kCommandIntervalMs;
    }
    return accepted;
}

bool PanelBridge::beginProtectedCommand(const String& action,
                                        const String& command) {
    if (alarmControlPhase_ != AlarmControlPhase::Idle) {
        // Another protected command is in flight. Queue this one so a burst
        // (two zone selects changed by one automation) is sent one per
        // second instead of being dropped. Identical pending actions are
        // collapsed.
        for (const ProtectedCommand& pending : protectedQueue_) {
            if (pending.action == action) return true;
        }
        if (protectedQueue_.size() >= AppConfig::kMaxQueuedPanelCommands) {
            Serial.print(F("[panel] Command queue full; dropped "));
            Serial.println(action);
            return false;
        }
        protectedQueue_.push_back(ProtectedCommand{action, command});
        Serial.print(F("[panel] Queued "));
        Serial.print(action);
        Serial.print(F(" behind "));
        Serial.println(alarmCommandAction_);
        sendClientStatus(String("MQTT panel action queued: ") + action + '.');
        return true;
    }
    return startProtectedCommand(action, command);
}

bool PanelBridge::startProtectedCommand(const String& action,
                                        const String& command) {
    alarmCommandAction_ = action;
    alarmCommandText_ = command;
    if (!panelReady()) {
        setAlarmCommandStatus("rejected", "UART baud is not locked yet");
        return false;
    }
    if (txMode_ == TxMode::Passive) {
        setAlarmCommandStatus("rejected", "strict passive mode is active");
        return false;
    }
    alarmCommandTransmitted_ = false;
    if (unlockPhase_ == UnlockPhase::Complete && debugLockState_ == 2) {
        alarmControlPhase_ = AlarmControlPhase::SendCommand;
        nextAlarmControlActionAt_ = millis();
        setAlarmCommandStatus("sending", "persistent debug unlock verified");
    } else {
        if (unlockPhase_ == UnlockPhase::NotStarted ||
            unlockPhase_ == UnlockPhase::Failed) {
            scheduleAutomaticUnlock();
        }
        alarmControlPhase_ = AlarmControlPhase::Unlocking;
        nextAlarmControlActionAt_ = millis();
        setAlarmCommandStatus("unlocking", "waiting for persistent debug access");
    }
    sendClientStatus(String("MQTT panel action accepted: ") +
                     alarmCommandAction_ + '.');
    return true;
}

void PanelBridge::processAlarmControl() {
    if (alarmControlPhase_ == AlarmControlPhase::Idle) {
        if (!protectedQueue_.empty()) {
            const ProtectedCommand next = protectedQueue_.front();
            protectedQueue_.pop_front();
            startProtectedCommand(next.action, next.command);
        }
        return;
    }
    if (!deadlineReached(nextAlarmControlActionAt_)) return;

    switch (alarmControlPhase_) {
        case AlarmControlPhase::Idle: return;

        case AlarmControlPhase::Unlocking:
            if (unlockPhase_ == UnlockPhase::Complete) {
                if (debugLockState_ == 2) {
                    alarmControlPhase_ = AlarmControlPhase::SendCommand;
                    setAlarmCommandStatus("sending", "debug unlock verified");
                } else {
                    alarmControlPhase_ = AlarmControlPhase::Idle;
                    setAlarmCommandStatus("failed", "debug unlock could not be verified");
                    alarmCommandText_ = String();
                }
                nextAlarmControlActionAt_ = millis();
            } else if (unlockPhase_ == UnlockPhase::Failed) {
                alarmControlPhase_ = AlarmControlPhase::Idle;
                nextAlarmControlActionAt_ = millis();
                setAlarmCommandStatus("failed", "debug unlock failed");
                alarmCommandText_ = String();
            }
            break;

        case AlarmControlPhase::SendCommand:
            if (unlockPhase_ != UnlockPhase::Complete ||
                debugLockState_ != 2) {
                if (unlockPhase_ == UnlockPhase::NotStarted ||
                    unlockPhase_ == UnlockPhase::Failed) {
                    scheduleAutomaticUnlock();
                }
                alarmControlPhase_ = AlarmControlPhase::Unlocking;
                nextAlarmControlActionAt_ = millis();
                setAlarmCommandStatus(
                    "unlocking",
                    "debug access changed before command transmission");
                break;
            }
            if (sendBridgeCommand(alarmCommandText_)) {
                parser_.noteBridgeCommand(alarmCommandAction_);
                alarmCommandTransmitted_ = true;
                alarmControlPhase_ = AlarmControlPhase::WaitCommand;
                nextAlarmControlActionAt_ = millis() + 250;
                setAlarmCommandStatus("submitted", "command sent with persistent debug access");
            }
            break;

        case AlarmControlPhase::WaitCommand:
            finishAlarmControl();
            break;
    }
}

void PanelBridge::setAlarmCommandStatus(const String& status,
                                        const String& detail) {
    alarmCommandStatus_ = status;
    alarmCommandDetail_ = detail;
    if (++alarmCommandRevision_ == 0) alarmCommandRevision_ = 1;
    Serial.print(F("[panel] "));
    Serial.print(alarmCommandAction_);
    Serial.print(F(": "));
    Serial.print(status);
    Serial.print(F(" - "));
    Serial.println(detail);
}

void PanelBridge::finishAlarmControl() {
    releasePanelTx();
    alarmControlPhase_ = AlarmControlPhase::Idle;
    nextAlarmControlActionAt_ = 0;

    if (alarmCommandTransmitted_) {
        setAlarmCommandStatus("submitted",
                              "command sent; panel output confirmation pending");
        if (alarmCommandAction_.startsWith("zone_chime_")) {
            // The zone_chime echo is only a database transaction trace. The
            // authoritative value is the "Ch" column of a fresh zone_info
            // read, so pull the programming table forward instead of waiting
            // for the normal six-hour refresh.
            nextZoneMetadataPollAt_ =
                millis() + 2 * AppConfig::kCommandIntervalMs;
        }
    } else {
        setAlarmCommandStatus("failed", "command was not sent");
    }
    sendClientStatus(String("MQTT panel action ") + alarmCommandAction_ +
                     ": " + alarmCommandStatus_ + ".");
    alarmCommandText_ = String();
}

String PanelBridge::debugUnlockState() const {
    if (debugLockState_ == 2 && unlockPhase_ == UnlockPhase::Complete) {
        return F("unlocked");
    }
    if (debugLockState_ == 1) return F("locked");
    if (unlockPhase_ != UnlockPhase::NotStarted &&
        unlockPhase_ != UnlockPhase::Complete &&
        unlockPhase_ != UnlockPhase::Failed) {
        return F("unlocking");
    }
    return F("unknown");
}

void PanelBridge::processServer() {
    if (!networkReady_ || !serverStarted_) return;
    acceptClient();
    processTelnetInput();
}

void PanelBridge::acceptClient() {
    if (!server_.hasClient()) return;
    WiFiClient candidate = server_.accept();
    if (!candidate) return;

    if (client_ && client_.connected()) {
        candidate.print(F("\r\nGC2 Bridge already has an active client.\r\n"));
        candidate.stop();
        return;
    }

    client_ = candidate;
    client_.setNoDelay(true);
    clientAuthState_ = ClientAuthState::AwaitingPassword;
    failedPasswordAttempts_ = 0;
    telnetParseState_ = TelnetParseState::Data;
    telnetLine_ = String();
    commandQueue_.clear();
    sendConnectionBanner();
}

void PanelBridge::disconnectClient() {
    if (client_) client_.stop();
    clientAuthState_ = ClientAuthState::Disconnected;
    telnetLine_ = String();
    commandQueue_.clear();
}

void PanelBridge::sendConnectionBanner() {
    if (!client_ || !client_.connected()) return;
    const uint8_t negotiation[] = {
        kTelnetIac, kTelnetWill, kTelnetEcho,
        kTelnetIac, kTelnetWill, kTelnetSuppressGoAhead,
        kTelnetIac, kTelnetDo, kTelnetSuppressGoAhead,
    };
    client_.write(negotiation, sizeof(negotiation));
    client_.print(F("\r\nGC2 UART Bridge\r\nPassword: "));
}

void PanelBridge::processTelnetInput() {
    if (!client_ || !client_.connected()) {
        if (clientAuthState_ != ClientAuthState::Disconnected) {
            disconnectClient();
        }
        return;
    }

    while (client_.available() > 0) {
        const int value = client_.read();
        if (value < 0) break;
        processTelnetByte(static_cast<uint8_t>(value));
    }
}

void PanelBridge::processTelnetByte(uint8_t value) {
    switch (telnetParseState_) {
        case TelnetParseState::Data:
            if (value == kTelnetIac) {
                telnetParseState_ = TelnetParseState::Iac;
            } else {
                processTelnetData(value);
            }
            break;

        case TelnetParseState::Iac:
            if (value == kTelnetIac) {
                processTelnetData(value);
                telnetParseState_ = TelnetParseState::Data;
            } else if (value == kTelnetDo || value == kTelnetDont ||
                       value == kTelnetWill || value == kTelnetWont) {
                telnetParseState_ = TelnetParseState::Option;
            } else if (value == kTelnetSubBegin) {
                telnetParseState_ = TelnetParseState::Subnegotiation;
            } else {
                telnetParseState_ = TelnetParseState::Data;
            }
            break;

        case TelnetParseState::Option:
            telnetParseState_ = TelnetParseState::Data;
            break;

        case TelnetParseState::Subnegotiation:
            if (value == kTelnetIac) {
                telnetParseState_ = TelnetParseState::SubIac;
            }
            break;

        case TelnetParseState::SubIac:
            telnetParseState_ = value == kTelnetSubEnd
                                     ? TelnetParseState::Data
                                     : TelnetParseState::Subnegotiation;
            break;
    }
}

void PanelBridge::processTelnetData(uint8_t value) {
    if (suppressByteAfterCr_ && (value == '\n' || value == 0)) {
        suppressByteAfterCr_ = false;
        return;
    }
    suppressByteAfterCr_ = false;

    if (value == '\r' || value == '\n') {
        suppressByteAfterCr_ = value == '\r';
        finishTelnetLine();
        return;
    }

    if (value == 0x08 || value == 0x7F) {
        if (!telnetLine_.isEmpty()) {
            telnetLine_.remove(telnetLine_.length() - 1);
            if (clientAuthState_ == ClientAuthState::Authenticated) {
                client_.print(F("\b \b"));
            }
        }
        return;
    }

    if (value < 0x20 || value > 0x7E ||
        telnetLine_.length() >= AppConfig::kMaxConsoleCommandLength) {
        return;
    }

    telnetLine_ += static_cast<char>(value);
    if (clientAuthState_ == ClientAuthState::Authenticated) {
        client_.write(value);
    }
}

void PanelBridge::finishTelnetLine() {
    if (clientAuthState_ == ClientAuthState::AwaitingPassword) {
        const bool accepted = credentials_.verifyTelnetPassword(telnetLine_);
        if (accepted &&
            !credentials_.ensureOtaPasswordHashForVerifiedPassword(
                telnetLine_)) {
            Serial.println(F("[ota] Could not initialize OTA authentication."));
        }
        telnetLine_ = String();
        if (accepted) {
            clientAuthState_ = ClientAuthState::Authenticated;
            failedPasswordAttempts_ = 0;
            client_.print(F("\r\nAccess granted.\r\n"));
            sendClientStatus(panelReady()
                                 ? String("UART ready at ") + currentBaud_ +
                                       " baud."
                                 : "Waiting for delayed UART baud detection.");
            sendClientStatus(
                txMode_ == TxMode::Passive
                    ? "Passive mode is active; all UART transmissions are blocked."
                    : txMode_ == TxMode::Monitor
                          ? "Monitor mode is active; debug access is supervised."
                          : "Maintenance transmission is enabled.");
        } else if (++failedPasswordAttempts_ >= 3) {
            client_.print(F("\r\nToo many failed attempts.\r\n"));
            delay(20);
            disconnectClient();
        } else {
            client_.print(F("\r\nIncorrect password.\r\nPassword: "));
        }
        return;
    }

    if (clientAuthState_ != ClientAuthState::Authenticated) {
        telnetLine_ = String();
        return;
    }

    client_.print(F("\r\n"));
    if (telnetLine_.isEmpty()) return;

    if (processLocalCommand(telnetLine_)) {
        telnetLine_ = String();
        return;
    }

    if (txMode_ != TxMode::Maintenance) {
        sendClientStatus(
            "Command blocked; use /listen off to enter maintenance mode.");
    } else if (commandQueue_.size() >=
               AppConfig::kMaxQueuedConsoleCommands) {
        sendClientStatus("Command queue full; line discarded.");
    } else {
        commandQueue_.push_back(telnetLine_);
    }
    telnetLine_ = String();
}

bool PanelBridge::processLocalCommand(const String& command) {
    String normalized = command;
    normalized.trim();
    normalized.toLowerCase();

    if (normalized == "/listen on" || normalized == "/mode passive") {
        txMode_ = TxMode::Passive;
        commandQueue_.clear();
        releasePanelTx();
        sendClientStatus(
            "Listen-only mode enabled; all UART transmissions are blocked.");
        return true;
    }

    if (normalized == "/monitor" || normalized == "/mode monitor") {
        txMode_ = TxMode::Monitor;
        commandQueue_.clear();
        nextZoneMetadataPollAt_ = millis();
        nextZoneStatePollAt_ = millis();
        nextZoneTroublePollAt_ = millis();
        nextSounderStatusPollAt_ = millis();
        nextPanelStatusPollAt_ = millis();
        nextAlarmMemoryPollAt_ = millis();
        nextUnlockSupervisionAt_ = millis();
        sendClientStatus(
            "Monitor mode enabled; persistent unlock supervision and read-only polling are active.");
        return true;
    }

    if (normalized == "/listen off" ||
        normalized == "/mode maintenance") {
        txMode_ = TxMode::Maintenance;
        nextUnlockSupervisionAt_ = millis();
        sendClientStatus(
            "Maintenance mode enabled; authenticated UART commands and persistent unlock supervision are active.");
        return true;
    }

    if (normalized == "/unlock") {
        if (txMode_ == TxMode::Passive) {
            sendClientStatus(
                "Unlock blocked by strict passive mode; use /monitor or /listen off first.");
        } else {
            scheduleAutomaticUnlock();
            sendClientStatus("Persistent automatic unlock scheduled.");
        }
        return true;
    }

    if (normalized == "/baud auto") {
        requestAutoBaud();
        sendClientStatus("Automatic UART baud detection restarted.");
        return true;
    }

    if (normalized.startsWith("/baud ")) {
        String value = normalized.substring(6);
        value.trim();
        bool numeric = !value.isEmpty();
        for (const char character : value) {
            if (!isDigit(character)) {
                numeric = false;
                break;
            }
        }
        const uint32_t requested = numeric ? value.toInt() : 0;
        if (!setBaudOverride(requested)) {
            sendClientStatus("Unsupported baud rate.");
        } else {
            sendClientStatus(String("UART manually set to ") + requested +
                             " baud.");
        }
        return true;
    }

    return false;
}

void PanelBridge::scheduleAutomaticUnlock() {
    panelSerialNumber_ = 0;
    panelMonth_ = 0;
    panelDay_ = 0;
    panelYear_ = 0;
    debugLockState_ = -1;
    identityAttempts_ = 0;
    unlockAttempts_ = 0;
    unlockVerificationAttempts_ = 0;
    unlockPhase_ = UnlockPhase::SendCalibration;
    nextUnlockActionAt_ = millis();
    nextUnlockSupervisionAt_ = UINT32_MAX;
}

bool PanelBridge::userInputAllowed() const {
    return alarmControlPhase_ == AlarmControlPhase::Idle &&
           (unlockPhase_ == UnlockPhase::NotStarted ||
            unlockPhase_ == UnlockPhase::Complete ||
            unlockPhase_ == UnlockPhase::Failed);
}

void PanelBridge::processManualCommandQueue() {
    if (txMode_ != TxMode::Maintenance || !panelReady() ||
        !userInputAllowed() ||
        commandQueue_.empty()) {
        return;
    }
    if (sendPanelCommand(commandQueue_.front())) {
        commandQueue_.pop_front();
    }
}

void PanelBridge::writePanelDataToClient(const uint8_t* data, size_t length) {
    if (!client_ || !client_.connected() ||
        clientAuthState_ != ClientAuthState::Authenticated) {
        return;
    }

    uint8_t encoded[1024];
    size_t outputLength = 0;
    for (size_t index = 0; index < length; ++index) {
        if (outputLength >= sizeof(encoded) - 2) {
            client_.write(encoded, outputLength);
            outputLength = 0;
        }
        encoded[outputLength++] = data[index];
        if (data[index] == kTelnetIac) encoded[outputLength++] = kTelnetIac;
    }
    if (outputLength > 0) client_.write(encoded, outputLength);
}

void PanelBridge::sendClientStatus(const String& message) {
    if (!client_ || !client_.connected() ||
        clientAuthState_ != ClientAuthState::Authenticated) {
        return;
    }
    client_.print(F("\r\n[bridge] "));
    client_.print(message);
    client_.print(F("\r\n"));
}
