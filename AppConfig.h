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
// Telnet writes never block the UART loop: output a client cannot accept is
// dropped and counted, and a client that accepts nothing for this long, or
// never authenticates, is disconnected. Keepalive detects vanished peers.
constexpr uint32_t kTelnetStalledClientMs = 30000;
constexpr uint32_t kTelnetAuthTimeoutMs = 60000;
constexpr int kTelnetKeepAliveIdleSeconds = 30;
constexpr int kTelnetKeepAliveIntervalSeconds = 10;
constexpr int kTelnetKeepAliveCount = 3;

constexpr uint32_t kBaudDetectDelayMs = 120000;
constexpr uint32_t kBaudRetryDelayMs = 2000;
constexpr uint32_t kBaudValidationWindowMs = 60000;
constexpr uint32_t kCommandIntervalMs = 1000;
// The GC2 console task, not the serial line, is the bottleneck: commands sent
// while it is still printing the previous reply push the UI task behind until
// it data-aborts and the panel reboots (MANUAL_UART_CONSOLE.md 1.4). Every
// command therefore waits for the console to go quiet, read-only polls go out
// one at a time with wide spacing, and "UI TIMER lag" pauses polling.
constexpr uint32_t kPanelQuietBeforeCommandMs = 250;
constexpr uint32_t kPanelQuietMaxWaitMs = 10000;
constexpr uint32_t kMonitorPollSpacingMs = 5000;
constexpr uint32_t kPanelLagBackoffMs = 60000;
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
constexpr uint32_t kSounderStatusRefreshMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kPanelStatusInitialDelayMs = 14000;
constexpr uint32_t kPanelStatusRefreshMs = 60000;
constexpr uint32_t kAlarmMemoryInitialDelayMs = 16000;
constexpr uint32_t kAlarmMemoryRefreshMs = 5UL * 60UL * 1000UL;
constexpr size_t kMaxQueuedPanelCommands = 8;
// A full unlock (three identity reads, up to three unlock and verification
// attempts) finishes well inside this; past it the command is abandoned.
constexpr uint32_t kAlarmControlTimeoutMs = 3UL * 60UL * 1000UL;

constexpr uint32_t kMqttReconnectDelayMs = 5000;
constexpr uint32_t kMqttPublishIntervalMs = 25;
constexpr uint32_t kMqttDiagnosticIntervalMs = 60000;
constexpr uint16_t kMqttBufferSize = 2048;
constexpr uint16_t kMqttSocketTimeoutSeconds = 2;

constexpr uint32_t kWifiConnectTimeoutMs = 30000;
constexpr uint32_t kWifiReconnectIntervalMs = 30000;
constexpr uint32_t kFactoryResetHoldMs = 10000;

// 32 KB is roughly 2.8 s of continuous console output at 115200 baud, enough
// to ride out a blocking MQTT connect attempt without dropping panel bytes.
constexpr size_t kPanelRxBufferSize = 32768;
constexpr size_t kPanelTxBufferSize = 2048;
constexpr size_t kMaxConsoleCommandLength = 255;
constexpr size_t kMaxQueuedConsoleCommands = 8;

}  // namespace AppConfig
