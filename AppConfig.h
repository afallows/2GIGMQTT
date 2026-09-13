#pragma once

#include <Arduino.h>

namespace AppConfig {

// Waveshare ESP32-S3-Zero. UART1 is routed away from UART0's boot-log pins so
// startup output cannot be interpreted as a command by the panel.
constexpr int kPanelRxPin = 5;   // Connect to panel TX.
constexpr int kPanelTxPin = 6;   // Connect to panel RX.
constexpr int kFactoryResetPin = 0;  // BOOT button; hold after the app starts.

constexpr uint8_t kPanelUartNumber = 1;
constexpr uint16_t kTelnetPort = 4444;
constexpr uint16_t kOtaPort = 3232;

constexpr uint32_t kBaudDetectDelayMs = 120000;
constexpr uint32_t kBaudRetryDelayMs = 2000;
constexpr uint32_t kBaudValidationWindowMs = 60000;
constexpr uint32_t kCommandIntervalMs = 1000;
constexpr uint32_t kDebugUnlockInitialDelayMs = 1000;
constexpr uint32_t kDebugUnlockCheckIntervalMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kDebugUnlockRetryIntervalMs = 30000;
constexpr uint32_t kZoneMetadataInitialDelayMs = 5000;
constexpr uint32_t kZoneMetadataRefreshMs = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kZoneStateInitialDelayMs = 8000;
constexpr uint32_t kZoneStateRefreshMs = 30000;
constexpr uint32_t kZoneTroubleInitialDelayMs = 10000;
constexpr uint32_t kZoneTroubleRefreshMs = 30000;
constexpr uint32_t kSounderStatusInitialDelayMs = 12000;
constexpr uint32_t kSounderStatusRefreshMs = 30000;
constexpr uint32_t kPanelStatusInitialDelayMs = 14000;
constexpr uint32_t kPanelStatusRefreshMs = 60000;
constexpr uint32_t kAlarmMemoryInitialDelayMs = 16000;
constexpr uint32_t kAlarmMemoryRefreshMs = 5UL * 60UL * 1000UL;
constexpr size_t kMaxQueuedPanelCommands = 8;

constexpr uint32_t kMqttReconnectDelayMs = 5000;
constexpr uint32_t kMqttPublishIntervalMs = 25;
constexpr uint32_t kMqttDiagnosticIntervalMs = 60000;
constexpr uint16_t kMqttBufferSize = 2048;
constexpr uint16_t kMqttSocketTimeoutSeconds = 2;

constexpr uint32_t kWifiConnectTimeoutMs = 30000;
constexpr uint32_t kWifiReconnectIntervalMs = 30000;
constexpr uint32_t kFactoryResetHoldMs = 10000;

constexpr size_t kPanelRxBufferSize = 8192;
constexpr size_t kPanelTxBufferSize = 2048;
constexpr size_t kMaxConsoleCommandLength = 255;
constexpr size_t kMaxQueuedConsoleCommands = 8;

}  // namespace AppConfig
