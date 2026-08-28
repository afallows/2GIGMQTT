#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include <deque>

#include "CredentialStore.h"
#include "Gc2ConsoleParser.h"

class PanelBridge {
  public:
    enum class AlarmCommand { ArmHome, ArmAway, Disarm };

    PanelBridge(CredentialStore& credentials, Gc2ConsoleParser& parser);

    void begin();
    void loop();
    void setNetworkReady(bool ready);
    void stopNetwork();
    void requestAutoBaud();
    bool setBaudOverride(uint32_t baud);
    String baudState() const;
    bool requestAlarmCommand(AlarmCommand command);
    bool requestZoneBypass(uint8_t zone, bool bypassed);
    String debugUnlockState() const;
    const String& alarmCommandAction() const { return alarmCommandAction_; }
    const String& alarmCommandStatus() const { return alarmCommandStatus_; }
    const String& alarmCommandDetail() const { return alarmCommandDetail_; }
    uint32_t alarmCommandRevision() const { return alarmCommandRevision_; }

  private:
    enum class BaudState { Waiting, RetryDelay, Validating, Ready };
    enum class TxMode { Passive, Monitor, Maintenance };
    enum class UnlockPhase {
        NotStarted,
        SendCalibration,
        WaitCalibration,
        SendBuildInfo,
        WaitBuildInfo,
        SendRtc,
        WaitRtc,
        EvaluateIdentity,
        SendUnlock,
        WaitUnlock,
        SendVerification,
        WaitVerification,
        Complete,
        Failed
    };
    enum class TelnetParseState { Data, Iac, Option, Subnegotiation, SubIac };
    enum class ClientAuthState { Disconnected, AwaitingPassword, Authenticated };
    enum class AlarmControlPhase {
        Idle,
        Unlocking,
        SendCommand,
        WaitCommand
    };

    CredentialStore& credentials_;
    Gc2ConsoleParser& parser_;
    HardwareSerial& panel_;
    WiFiServer server_;
    WiFiClient client_;

    bool networkReady_ = false;
    bool serverStarted_ = false;
    uint32_t bootTime_ = 0;
    uint32_t currentBaud_ = 0;
    BaudState baudState_ = BaudState::Waiting;
    uint32_t baudStateStartedAt_ = 0;
    static constexpr uint8_t kMaxBaudCandidates = 11;
    uint32_t baudCandidates_[kMaxBaudCandidates]{};
    uint8_t baudCandidateCount_ = 0;
    uint8_t baudCandidateIndex_ = 0;
    uint32_t trialBaud_ = 0;
    size_t validationByteCount_ = 0;
    size_t validationPrintableCount_ = 0;
    String validationLine_;

    UnlockPhase unlockPhase_ = UnlockPhase::NotStarted;
    uint32_t nextUnlockActionAt_ = 0;
    uint32_t lastPanelCommandAt_ = 0;
    uint8_t identityAttempts_ = 0;
    uint8_t unlockAttempts_ = 0;
    uint8_t unlockVerificationAttempts_ = 0;
    uint32_t nextUnlockSupervisionAt_ = 0;
    uint32_t panelSerialNumber_ = 0;
    uint8_t panelMonth_ = 0;
    uint8_t panelDay_ = 0;
    uint16_t panelYear_ = 0;
    int debugLockState_ = -1;

    String panelLine_;
    String telnetLine_;
    std::deque<String> commandQueue_;
    TelnetParseState telnetParseState_ = TelnetParseState::Data;
    ClientAuthState clientAuthState_ = ClientAuthState::Disconnected;
    uint8_t failedPasswordAttempts_ = 0;
    bool suppressByteAfterCr_ = false;
    // Monitor mode permits supervised unlock, allow-listed MQTT control, and
    // zone polling. Every command attaches TX only for its transmitted bytes.
    TxMode txMode_ = TxMode::Monitor;
    uint32_t nextZoneMetadataPollAt_ = 0;
    uint32_t nextZoneStatePollAt_ = 0;
    uint32_t nextZoneTroublePollAt_ = 0;
    AlarmControlPhase alarmControlPhase_ = AlarmControlPhase::Idle;
    String alarmCommandText_;
    String alarmCommandAction_ = "none";
    String alarmCommandStatus_ = "idle";
    String alarmCommandDetail_;
    uint32_t alarmCommandRevision_ = 1;
    uint32_t nextAlarmControlActionAt_ = 0;
    bool alarmCommandTransmitted_ = false;

    void processBaudDetection();
    static bool validBaud(uint32_t baud);
    void addBaudCandidate(uint32_t baud);
    void startBaudValidation();
    void startCurrentBaudCandidate();
    void processBaudValidation();
    void rejectCurrentBaudCandidate(const char* reason);
    void acceptCurrentBaudCandidate(const String& evidence);
    static bool plausiblePanelText(const String& line);
    void restartBaudDetection();
    void startPanelUart(uint32_t baud);

    void processPanelInput();
    void processPanelByte(uint8_t value);
    void inspectPanelLine(const String& line);
    void processUnlock();
    void processUnlockSupervisor();
    void processAlarmControl();
    bool beginProtectedCommand(const String& action, const String& command);
    void setAlarmCommandStatus(const String& status, const String& detail);
    void finishAlarmControl();
    bool attachPanelTx();
    void releasePanelTx();
    bool sendPanelCommand(const String& command);
    bool sendBridgeCommand(const String& command);
    bool sendMonitorCommand(const char* command);
    void processMonitorPolling();
    static uint32_t calculateUnlockCode(uint32_t serialNumber, uint8_t day,
                                        uint8_t month, uint16_t year);

    void processServer();
    void acceptClient();
    void disconnectClient();
    void processTelnetInput();
    void processTelnetByte(uint8_t value);
    void processTelnetData(uint8_t value);
    void finishTelnetLine();
    void processManualCommandQueue();
    bool processLocalCommand(const String& command);
    void scheduleAutomaticUnlock();
    void writePanelDataToClient(const uint8_t* data, size_t length);
    void sendClientStatus(const String& message);
    void sendConnectionBanner();

    bool userInputAllowed() const;
    bool panelReady() const { return baudState_ == BaudState::Ready; }
    static bool deadlineReached(uint32_t deadline);
};
