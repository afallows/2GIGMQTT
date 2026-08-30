#include "MqttService.h"

#include "AppConfig.h"
#include "DeviceIdentity.h"
#include "PanelBridge.h"

namespace {
constexpr char kHomeAssistantStatusTopic[] = "homeassistant/status";
constexpr char kFirmwareRelease[] = "0.8.2";
constexpr char kTransportSchema[] = "gc2-mqtt-v1";
constexpr char const* kBaseDiscoveryObjectIds[] = {
    "panel_state",   "battery_state", "battery_voltage",
    "firmware",      "received_lines", "unknown_lines",
    "rf_supervision", "zwave_no_ack",  "zwave_max_retry", "uart_baud",
};
constexpr char const* kLegacyStateSuffixes[] = {
    "availability", "panel/state", "panel/battery", "panel/firmware",
    "diagnostic",
};

bool deadlineReached(uint32_t deadline) {
    return static_cast<int32_t>(millis() - deadline) >= 0;
}
}  // namespace

MqttService::MqttService(CredentialStore& credentials, Gc2State& state,
                         PanelBridge& bridge)
    : credentials_(credentials),
      state_(state),
      bridge_(bridge),
      client_(transport_) {}

void MqttService::begin() {
    deviceId_ = DeviceIdentity::mqttDeviceId();

    client_.setBufferSize(AppConfig::kMqttBufferSize);
    client_.setSocketTimeout(AppConfig::kMqttSocketTimeoutSeconds);
    client_.setKeepAlive(30);
    client_.setCallback([this](char* topic, uint8_t* payload,
                               unsigned int length) {
        onMessage(topic, payload, length);
    });
    reloadSettings();
}

void MqttService::reloadSettings() {
    if (client_.connected()) client_.disconnect();
    transport_.stop();
    settings_ = MqttSettings{};
    if (!credentials_.loadMqttSettings(settings_)) {
        settings_.enabled = false;
    }

    rootTopic_ = settings_.baseTopic;
    if (!rootTopic_.isEmpty()) rootTopic_ += '/';
    rootTopic_ += deviceId_;
    availabilityTopic_ = makeTopic("availability");
    baudCommandTopic_ = makeTopic("uart/baud/set");
    alarmCommandTopic_ = makeTopic("panel/set");
    bypassCommandTopic_ = makeTopic("panel/bypass/set");
    sounderVolumeCommandTopic_ = makeTopic("panel/sounder_volume/set");
    if (settings_.enabled) {
        client_.setServer(settings_.host.c_str(), settings_.port);
        Serial.print(F("[mqtt] Configured broker "));
        Serial.print(settings_.host);
        Serial.print(':');
        Serial.println(settings_.port);
    } else {
        Serial.println(F("[mqtt] Publishing is disabled."));
    }
    nextConnectAt_ = millis();
    resetPublishTracking();
}

void MqttService::setNetworkReady(bool ready) {
    networkReady_ = ready;
    if (ready) {
        nextConnectAt_ = millis();
    } else {
        stopNetwork();
    }
}

void MqttService::stopNetwork() {
    networkReady_ = false;
    if (client_.connected()) client_.disconnect();
    transport_.stop();
}

void MqttService::loop() {
    if (!settings_.enabled || !networkReady_) return;
    if (!client_.connected()) {
        connectIfNeeded();
        return;
    }

    client_.loop();
    if (!client_.connected() || !deadlineReached(nextPublishAt_)) return;
    nextPublishAt_ = millis() + AppConfig::kMqttPublishIntervalMs;
    publishNext();
}

void MqttService::connectIfNeeded() {
    if (!deadlineReached(nextConnectAt_)) return;
    nextConnectAt_ = millis() + AppConfig::kMqttReconnectDelayMs;

    bool connected = false;
    if (settings_.username.isEmpty()) {
        connected = client_.connect(deviceId_.c_str(),
                                    availabilityTopic_.c_str(), 1, true,
                                    "offline");
    } else {
        connected = client_.connect(
            deviceId_.c_str(), settings_.username.c_str(),
            settings_.password.c_str(), availabilityTopic_.c_str(), 1, true,
            "offline");
    }

    if (!connected) {
        Serial.print(F("[mqtt] Broker connection failed, state "));
        Serial.println(client_.state());
        return;
    }

    Serial.println(F("[mqtt] Connected."));
    client_.publish(availabilityTopic_.c_str(), "online", true);
    // MQTT commands are intentionally transient. Remove a stale retained
    // command before subscribing; publishBaudState() will replace it with the
    // bridge's current applied value.
    client_.publish(baudCommandTopic_.c_str(), "", true);
    client_.publish(alarmCommandTopic_.c_str(), "", true);
    client_.publish(bypassCommandTopic_.c_str(), "", true);
    client_.publish(sounderVolumeCommandTopic_.c_str(), "", true);
    if (!state_.sounderVolumeKnown()) {
        // An old retained value is not authoritative after an ESP restart.
        // Clear it until the panel supplies a fresh read-back.
        client_.publish(makeTopic("panel/sounder_volume").c_str(), "", true);
    }
    client_.subscribe(kHomeAssistantStatusTopic);
    client_.subscribe(baudCommandTopic_.c_str());
    client_.subscribe(alarmCommandTopic_.c_str());
    client_.subscribe(bypassCommandTopic_.c_str());
    client_.subscribe(sounderVolumeCommandTopic_.c_str());
    resetPublishTracking();
    state_.markAllForRepublish();
    nextPublishAt_ = millis();
}

void MqttService::onMessage(char* topic, uint8_t* payload,
                            unsigned int length) {
    String message;
    message.reserve(length);
    for (unsigned int index = 0; index < length; ++index) {
        message += static_cast<char>(payload[index]);
    }
    message.trim();
    message.toLowerCase();
    if (strcmp(topic, kHomeAssistantStatusTopic) == 0) {
        if (message == "online" && settings_.discovery) {
            // Home Assistant may have been offline when the first retained
            // discovery deletions were sent. Re-send them now so restored
            // native MQTT entities cannot survive beside the custom
            // integration entities.
            nativeCleanupStage_ = 0;
            manifestPublished_ = false;
            nextPublishAt_ = millis();
        }
        return;
    }
    if (strcmp(topic, alarmCommandTopic_.c_str()) == 0) {
        bool accepted = false;
        if (message == "arm_home") {
            accepted = bridge_.requestAlarmCommand(
                PanelBridge::AlarmCommand::ArmHome);
        } else if (message == "arm_away") {
            accepted = bridge_.requestAlarmCommand(
                PanelBridge::AlarmCommand::ArmAway);
        } else if (message == "disarm") {
            accepted = bridge_.requestAlarmCommand(
                PanelBridge::AlarmCommand::Disarm);
        } else if (!message.isEmpty()) {
            Serial.print(F("[mqtt] Rejected alarm command: "));
            Serial.println(message);
            return;
        }
        if (!message.isEmpty() && !accepted) {
            Serial.print(F("[mqtt] Alarm command could not be scheduled: "));
            Serial.println(message);
        }
        return;
    }
    if (strcmp(topic, bypassCommandTopic_.c_str()) == 0) {
        const int zoneKey = message.indexOf("\"zone\"");
        const int zoneColon = zoneKey < 0 ? -1 : message.indexOf(':', zoneKey);
        const int bypassKey = message.indexOf("\"bypassed\"");
        const int bypassColon =
            bypassKey < 0 ? -1 : message.indexOf(':', bypassKey);
        const int zone = zoneColon < 0 ? 0
                                       : message.substring(zoneColon + 1).toInt();
        bool valueKnown = false;
        bool bypassed = false;
        if (bypassColon >= 0) {
            String value = message.substring(bypassColon + 1);
            value.trim();
            if (value.startsWith("true")) {
                valueKnown = true;
                bypassed = true;
            } else if (value.startsWith("false")) {
                valueKnown = true;
            }
        }
        if (zone <= 0 || zone >= Gc2State::kMaxZones || !valueKnown ||
            !bridge_.requestZoneBypass(static_cast<uint8_t>(zone), bypassed)) {
            Serial.print(F("[mqtt] Rejected zone bypass command: "));
            Serial.println(message);
        }
        return;
    }
    if (strcmp(topic, sounderVolumeCommandTopic_.c_str()) == 0) {
        bool numeric = !message.isEmpty();
        for (const char character : message) {
            if (!isDigit(character)) {
                numeric = false;
                break;
            }
        }
        const int requested = numeric ? message.toInt() : -1;
        if (requested < 0 || requested > 100 ||
            !bridge_.requestSounderVolume(static_cast<uint8_t>(requested))) {
            Serial.print(F("[mqtt] Rejected sounder volume command: "));
            Serial.println(message);
        }
        return;
    }
    if (strcmp(topic, baudCommandTopic_.c_str()) != 0) return;

    if (message == "auto") {
        bridge_.requestAutoBaud();
        return;
    }
    if (message == "detecting") return;
    bool numeric = !message.isEmpty();
    for (const char character : message) {
        if (!isDigit(character)) {
            numeric = false;
            break;
        }
    }
    const uint32_t requested = numeric ? message.toInt() : 0;
    if (!bridge_.setBaudOverride(requested)) {
        Serial.print(F("[mqtt] Rejected UART baud command: "));
        Serial.println(message);
    }
}

void MqttService::resetPublishTracking() {
    legacyCleanupStage_ =
        DeviceIdentity::legacyMqttDeviceId() == deviceId_
            ? kLegacyCleanupCount
            : 0;
    // The custom GC2 integration owns Home Assistant entities. Clear the old
    // direct MQTT-discovery entities so a bridge cannot create a duplicate
    // alarm panel alongside the integration.
    nativeCleanupStage_ = 0;
    discoveryStage_ = kBaseDiscoveryCount;
    manifestPublished_ = false;
    memset(zoneDiscoveryPublished_, 0, sizeof(zoneDiscoveryPublished_));
    memset(publishedZoneMetadataRevision_, 0,
           sizeof(publishedZoneMetadataRevision_));
    memset(publishedZoneRevision_, 0, sizeof(publishedZoneRevision_));
    publishedAlarmRevision_ = 0;
    publishedBatteryRevision_ = 0;
    publishedSounderVolumeRevision_ = 0;
    publishedFirmwareRevision_ = 0;
    publishedTroubleRevision_ = 0;
    publishingTroubleRevision_ = 0;
    troublePublishStage_ = 0;
    publishedPanelSecurityRevision_ = 0;
    publishedAlarmMemoryRevision_ = 0;
    publishedDiagnosticRevision_ = 0;
    publishedAlarmCommandRevision_ = 0;
    publishedBaudState_ = String();
    publishedDebugUnlockState_ = String();
    nextDiagnosticAt_ = millis();
}

void MqttService::publishNext() {
    if (legacyCleanupStage_ < kLegacyCleanupCount) {
        if (publishLegacyCleanup(legacyCleanupStage_)) {
            ++legacyCleanupStage_;
        }
        return;
    }

    if (nativeCleanupStage_ < kNativeDiscoveryCleanupCount) {
        if (publishNativeDiscoveryCleanup(nativeCleanupStage_)) {
            ++nativeCleanupStage_;
        }
        return;
    }

    if (!manifestPublished_) {
        manifestPublished_ = publishManifest();
        return;
    }

    // Delete retained raw records for zones that the latest programming poll
    // says are disabled. Active zones are republished below with their full
    // metadata, so a Home Assistant integration can rebuild the inventory by
    // subscribing to rootTopic/# at any time.
    for (uint8_t zone = 1; zone < Gc2State::kMaxZones; ++zone) {
        if (state_.zone(zone).metadataKnown &&
            !state_.zone(zone).discovered &&
            publishedZoneMetadataRevision_[zone] !=
                state_.zone(zone).metadataRevision) {
            char number[3];
            snprintf(number, sizeof(number), "%02u", zone);
            if (client_.publish(
                    makeTopic(String("zone/") + number + "/state").c_str(),
                    "", true)) {
                publishedZoneMetadataRevision_[zone] =
                    state_.zone(zone).metadataRevision;
                publishedZoneRevision_[zone] = state_.zone(zone).revision;
            }
            return;
        }
    }

    if (state_.alarmRevision() != publishedAlarmRevision_) {
        if (publishAlarmState()) {
            publishedAlarmRevision_ = state_.alarmRevision();
        }
        return;
    }
    if (bridge_.alarmCommandRevision() != publishedAlarmCommandRevision_) {
        if (publishAlarmCommandStatus()) {
            publishedAlarmCommandRevision_ = bridge_.alarmCommandRevision();
        }
        return;
    }
    if (bridge_.baudState() != publishedBaudState_) {
        if (publishBaudState()) {
            publishedBaudState_ = bridge_.baudState();
        }
        return;
    }
    if (state_.batteryRevision() != publishedBatteryRevision_) {
        if (publishBatteryState()) {
            publishedBatteryRevision_ = state_.batteryRevision();
        }
        return;
    }
    if (state_.sounderVolumeRevision() !=
        publishedSounderVolumeRevision_) {
        if (publishSounderVolumeState()) {
            publishedSounderVolumeRevision_ =
                state_.sounderVolumeRevision();
        }
        return;
    }
    if (state_.firmwareRevision() != publishedFirmwareRevision_) {
        if (publishFirmwareState()) {
            publishedFirmwareRevision_ = state_.firmwareRevision();
        }
        return;
    }
    if (state_.panelSecurityRevision() != publishedPanelSecurityRevision_) {
        if (publishPanelSecurityState()) {
            publishedPanelSecurityRevision_ = state_.panelSecurityRevision();
        }
        return;
    }
    if (state_.alarmMemoryRevision() != publishedAlarmMemoryRevision_) {
        if (publishAlarmMemoryState()) {
            publishedAlarmMemoryRevision_ = state_.alarmMemoryRevision();
        }
        return;
    }
    if (troublePublishStage_ != 0 ||
        state_.troubleRevision() != publishedTroubleRevision_) {
        if (troublePublishStage_ == 0) {
            publishingTroubleRevision_ = state_.troubleRevision();
            if (publishTroubleSummary()) troublePublishStage_ = 1;
            return;
        }
        const uint8_t slot = troublePublishStage_ - 1;
        if (publishTroubleEntry(slot)) {
            ++troublePublishStage_;
            if (troublePublishStage_ > Gc2State::kMaxTroubles) {
                publishedTroubleRevision_ = publishingTroubleRevision_;
                troublePublishStage_ = 0;
            }
        }
        return;
    }
    for (uint8_t zone = 1; zone < Gc2State::kMaxZones; ++zone) {
        if (state_.zone(zone).discovered &&
            state_.zone(zone).revision != publishedZoneRevision_[zone]) {
            if (publishZoneState(zone)) {
                publishedZoneRevision_[zone] = state_.zone(zone).revision;
            }
            return;
        }
    }
    if ((state_.diagnosticRevision() != publishedDiagnosticRevision_ ||
         bridge_.debugUnlockState() != publishedDebugUnlockState_) &&
        deadlineReached(nextDiagnosticAt_)) {
        if (publishDiagnostics()) {
            publishedDiagnosticRevision_ = state_.diagnosticRevision();
            publishedDebugUnlockState_ = bridge_.debugUnlockState();
            nextDiagnosticAt_ = millis() +
                                AppConfig::kMqttDiagnosticIntervalMs;
        }
        return;
    }

    String event;
    if (state_.peekEvent(event) &&
        client_.publish(makeTopic("event").c_str(), event.c_str(), false)) {
        state_.popEvent();
    }
}

bool MqttService::publishLegacyCleanup(uint16_t stage) {
    String topic;
    if (stage < kLegacySensorDiscoveryCount) {
        topic = String("homeassistant/sensor/") +
                DeviceIdentity::legacyMqttDeviceId() + '_' +
                kBaseDiscoveryObjectIds[stage] + "/config";
    } else {
        stage -= kLegacySensorDiscoveryCount;
        if (stage < Gc2State::kMaxZones - 1) {
            char number[3];
            snprintf(number, sizeof(number), "%02u",
                     static_cast<unsigned int>(stage + 1));
            topic = String("homeassistant/binary_sensor/") +
                    DeviceIdentity::legacyMqttDeviceId() + "_zone_" +
                    number + "/config";
        } else {
            stage -= Gc2State::kMaxZones - 1;
            String legacyRoot = settings_.baseTopic + '/' +
                                DeviceIdentity::legacyMqttDeviceId();
            if (stage < 5) {
                topic = legacyRoot + '/' + kLegacyStateSuffixes[stage];
            } else {
                stage -= 5;
                char number[3];
                snprintf(number, sizeof(number), "%02u",
                         static_cast<unsigned int>(stage + 1));
                topic = legacyRoot + "/zone/" + number + "/state";
            }
        }
    }
    return client_.publish(topic.c_str(), "", true);
}

bool MqttService::publishNativeDiscoveryCleanup(uint16_t stage) {
    static constexpr char const* kSensorObjectIds[] = {
        "panel_state",          "battery_state", "battery_voltage",
        "firmware",             "received_lines", "unknown_lines",
        "rf_supervision",       "zwave_no_ack", "zwave_max_retry",
        "alarm_command_status", "alarm_origin", "alarm_user",
    };
    static constexpr uint8_t kSensorCount =
        sizeof(kSensorObjectIds) / sizeof(kSensorObjectIds[0]);

    String topic;
    if (stage < kSensorCount) {
        topic = String("homeassistant/sensor/") + deviceId_ + '_' +
                kSensorObjectIds[stage] + "/config";
    } else if (stage == kSensorCount) {
        topic = String("homeassistant/text/") + deviceId_ +
                "_uart_baud/config";
    } else if (stage == kSensorCount + 1) {
        topic = String("homeassistant/alarm_control_panel/") + deviceId_ +
                "_alarm/config";
    } else if (stage == kSensorCount + 2) {
        topic = String("homeassistant/number/") + deviceId_ +
                "_sounder_volume/config";
    } else {
        stage -= kBaseDiscoveryCount;
        const bool bypass = stage >= Gc2State::kMaxZones - 1;
        if (bypass) stage -= Gc2State::kMaxZones - 1;
        char number[3];
        snprintf(number, sizeof(number), "%02u",
                 static_cast<unsigned int>(stage + 1));
        topic = String("homeassistant/") +
                (bypass ? "switch/" : "binary_sensor/") + deviceId_ +
                "_zone_" + number + (bypass ? "_bypass/config" : "/config");
    }
    return client_.publish(topic.c_str(), "", true);
}

bool MqttService::publishManifest() {
    String payload;
    payload.reserve(700);
    payload += F("{\"schema\":\"");
    payload += kTransportSchema;
    payload += F("\",\"device_id\":\"");
    payload += deviceId_;
    payload += F("\",\"mac\":\"");
    payload += DeviceIdentity::macHex();
    payload += F("\",\"name\":\"");
    payload += jsonEscape(DeviceIdentity::displayName());
    payload += F("\",\"root_topic\":\"");
    payload += jsonEscape(rootTopic_);
    payload += F("\",\"bridge_firmware\":\"");
    payload += kFirmwareRelease;
    payload += F("\",\"max_zones\":74,\"capabilities\":{\"alarm_control\":true,\"zone_bypass\":true,\"zone_inventory\":true,\"zone_battery\":true,\"zone_trouble\":true,\"panel_security\":true,\"alarm_memory\":true,\"observed_users\":true,\"uart_baud_control\":true,\"sounder_volume_control\":true}}");
    if (!publishRetained("manifest", payload)) return false;

    // This stable discovery address lets Home Assistant offer the bridge even
    // when the configured telemetry base topic is customized.
    const String discoveryTopic = String("2gig/gc2/discovery/") + deviceId_;
    if (!client_.publish(discoveryTopic.c_str(),
                         settings_.discovery ? payload.c_str() : "", true)) {
        return false;
    }
    return true;
}

bool MqttService::publishBaseDiscovery(uint8_t stage) {
    switch (stage) {
        case 0:
            return publishDiscovery("sensor", "panel_state", "Panel State",
                                    makeTopic("panel/state"), "{{ value }}",
                                    String(), String(), String(), String());
        case 1:
            return publishDiscovery(
                "sensor", "battery_state", "Backup Battery",
                makeTopic("panel/battery"), "{{ value_json.state }}");
        case 2:
            return publishDiscovery(
                "sensor", "battery_voltage", "Panel Battery Voltage",
                makeTopic("panel/battery"), "{{ value_json.millivolts }}",
                "voltage", "mV", String(), "measurement");
        case 3:
            return publishDiscovery(
                "sensor", "firmware", "Panel Firmware",
                makeTopic("panel/firmware"), "{{ value_json.version }}",
                String(), String(), "diagnostic");
        case 4:
            return publishDiscovery(
                "sensor", "received_lines", "Console Lines",
                makeTopic("diagnostic"), "{{ value_json.received_lines }}",
                String(), String(), "diagnostic", "total_increasing");
        case 5:
            return publishDiscovery(
                "sensor", "unknown_lines", "Unparsed Console Lines",
                makeTopic("diagnostic"), "{{ value_json.unknown_lines }}",
                String(), String(), "diagnostic", "total_increasing");
        case 6:
            return publishDiscovery(
                "sensor", "rf_supervision", "RF Supervisory Packets",
                makeTopic("diagnostic"), "{{ value_json.rf_supervision }}",
                String(), String(), "diagnostic", "total_increasing");
        case 7:
            return publishDiscovery(
                "sensor", "zwave_no_ack", "Z-Wave No ACK",
                makeTopic("diagnostic"), "{{ value_json.zwave_no_ack }}",
                String(), String(), "diagnostic", "total_increasing");
        case 8:
            return publishDiscovery(
                "sensor", "zwave_max_retry", "Z-Wave Max Retries",
                makeTopic("diagnostic"), "{{ value_json.zwave_max_retry }}",
                String(), String(), "diagnostic", "total_increasing");
        case 9:
            return publishDiscovery(
                "text", "uart_baud", "UART Baud", makeTopic("uart/baud"),
                "{{ value }}", String(), String(), "config", String(),
                baudCommandTopic_);
        case 10: return publishAlarmDiscovery();
        case 11:
            return publishDiscovery(
                "sensor", "alarm_command_status", "Alarm Command Status",
                makeTopic("panel/command_status"),
                "{{ value_json.status }}", String(), String(), "diagnostic");
        case 12:
            return publishDiscovery(
                "sensor", "alarm_origin", "Last Alarm Action Origin",
                makeTopic("panel/status"), "{{ value_json.origin }}",
                String(), String(), "diagnostic");
        case 13:
            return publishDiscovery(
                "sensor", "alarm_user", "Last Alarm User",
                makeTopic("panel/status"),
                "{{ value_json.user if value_json.user is not none else 'unknown' }}",
                String(), String(), "diagnostic");
        case 14:
            return publishDiscovery(
                "number", "sounder_volume", "Chime and Announcement Volume",
                makeTopic("panel/sounder_volume"), "{{ value }}", String(),
                "%", "config", String(), sounderVolumeCommandTopic_);
        default: return true;
    }
}

bool MqttService::publishAlarmDiscovery() {
    String payload;
    payload.reserve(900);
    payload += F("{\"name\":\"GC2 Alarm\",\"unique_id\":\"");
    payload += deviceId_ + F("_alarm\",\"state_topic\":\"");
    payload += jsonEscape(makeTopic("panel/state"));
    payload += F("\",\"command_topic\":\"");
    payload += jsonEscape(alarmCommandTopic_);
    payload += F("\",\"availability_topic\":\"");
    payload += jsonEscape(availabilityTopic_);
    payload += F("\",\"payload_arm_home\":\"ARM_HOME\",");
    payload += F("\"payload_arm_away\":\"ARM_AWAY\",");
    payload += F("\"payload_disarm\":\"DISARM\",");
    payload += F("\"supported_features\":[\"arm_home\",\"arm_away\"],");
    payload += F("\"device\":");
    payload += deviceJson();
    payload += F(",\"origin\":{\"name\":\"2GIGMQTT\",\"sw\":\"");
    payload += kFirmwareRelease;
    payload += F("\"}}");

    const String topic = String("homeassistant/alarm_control_panel/") +
                         deviceId_ + "_alarm/config";
    return client_.publish(topic.c_str(), payload.c_str(), true);
}

bool MqttService::publishZoneDiscovery(uint8_t zoneNumber) {
    const Gc2ZoneSnapshot& zone = state_.zone(zoneNumber);
    char number[3];
    snprintf(number, sizeof(number), "%02u", zoneNumber);
    String name = zone.name.isEmpty() ? String("Zone ") + zoneNumber
                                      : zone.name;
    String description = zone.name + ' ' + zone.zoneType;
    description.toLowerCase();
    String deviceClass = F("opening");
    if (description.indexOf("carbon monoxide") >= 0) {
        deviceClass = F("carbon_monoxide");
    } else if (description.indexOf("smoke") >= 0 ||
               description.indexOf("fire") >= 0 ||
               zone.kind == Gc2ZoneKind::Smoke) {
        deviceClass = F("smoke");
    } else if (description.indexOf("gas") >= 0) {
        deviceClass = F("gas");
    } else if (description.indexOf("water") >= 0 ||
               description.indexOf("flood") >= 0) {
        deviceClass = F("moisture");
    } else if (description.indexOf("glass break") >= 0) {
        deviceClass = F("sound");
    } else if (description.indexOf("motion") >= 0 ||
               description.indexOf("interior") >= 0 ||
               zone.kind == Gc2ZoneKind::Motion) {
        deviceClass = F("motion");
    } else if (description.indexOf("window") >= 0) {
        deviceClass = F("window");
    } else if (description.indexOf("garage door") >= 0) {
        deviceClass = F("garage_door");
    } else if (description.indexOf("door") >= 0 ||
               description.indexOf("entry") >= 0 ||
               description.indexOf("gate") >= 0) {
        deviceClass = F("door");
    }
    if (!publishDiscovery(
        "binary_sensor", String("zone_") + number, name,
        makeTopic(String("zone/") + number + "/state"),
        "{{ value_json.state }}", deviceClass)) {
        return false;
    }
    return publishZoneBypassDiscovery(zoneNumber);
}

bool MqttService::publishZoneBypassDiscovery(uint8_t zoneNumber) {
    const Gc2ZoneSnapshot& zone = state_.zone(zoneNumber);
    char number[3];
    snprintf(number, sizeof(number), "%02u", zoneNumber);
    const String name = (zone.name.isEmpty() ? String("Zone ") + zoneNumber
                                              : zone.name) +
                        " Bypass";
    const String stateTopic =
        makeTopic(String("zone/") + number + "/state");

    String payload;
    payload.reserve(1000);
    payload += F("{\"name\":\"");
    payload += jsonEscape(name);
    payload += F("\",\"unique_id\":\"");
    payload += deviceId_ + F("_zone_") + number;
    payload += F("_bypass\",\"state_topic\":\"");
    payload += jsonEscape(stateTopic);
    payload += F("\",\"value_template\":\"{{ 'ON' if value_json.bypassed else 'OFF' }}\",");
    payload += F("\"command_topic\":\"");
    payload += jsonEscape(bypassCommandTopic_);
    payload += F("\",\"command_template\":\"{\\\"zone\\\":");
    payload += zoneNumber;
    payload += F(",\\\"bypassed\\\":{{ 'true' if value == 'ON' else 'false' }}}\",");
    payload += F("\"availability_topic\":\"");
    payload += jsonEscape(availabilityTopic_);
    payload += F("\",\"device\":");
    payload += deviceJson();
    payload += F(",\"origin\":{\"name\":\"2GIGMQTT\",\"sw\":\"");
    payload += kFirmwareRelease;
    payload += F("\"}}");

    const String topic = String("homeassistant/switch/") + deviceId_ +
                         "_zone_" + number + "_bypass/config";
    return client_.publish(topic.c_str(), payload.c_str(), true);
}

bool MqttService::removeZoneDiscovery(uint8_t zoneNumber) {
    char number[3];
    snprintf(number, sizeof(number), "%02u", zoneNumber);
    const String objectId = String("zone_") + number;
    const String topic = String("homeassistant/binary_sensor/") + deviceId_ +
                         '_' + objectId + "/config";
    if (!client_.publish(topic.c_str(), "", true)) return false;
    const String bypassTopic = String("homeassistant/switch/") + deviceId_ +
                               "_zone_" + number + "_bypass/config";
    return client_.publish(bypassTopic.c_str(), "", true);
}

bool MqttService::publishDiscovery(
    const String& component, const String& objectId, const String& name,
    const String& stateTopic, const String& valueTemplate,
    const String& deviceClass, const String& unit,
    const String& entityCategory, const String& stateClass,
    const String& commandTopic) {
    String payload;
    payload.reserve(1100);
    payload += F("{\"name\":\"");
    payload += jsonEscape(name);
    payload += F("\",\"uniq_id\":\"");
    payload += deviceId_ + '_' + objectId;
    payload += F("\",\"stat_t\":\"");
    payload += jsonEscape(stateTopic);
    payload += F("\",\"avty_t\":\"");
    payload += jsonEscape(availabilityTopic_);
    payload += F("\",\"val_tpl\":\"");
    payload += jsonEscape(valueTemplate);
    payload += '"';
    if (!commandTopic.isEmpty()) {
        payload += F(",\"cmd_t\":\"");
        payload += jsonEscape(commandTopic);
        payload += '"';
    }
    if (!deviceClass.isEmpty()) {
        payload += F(",\"dev_cla\":\"");
        payload += jsonEscape(deviceClass);
        payload += '"';
    }
    if (!unit.isEmpty()) {
        payload += F(",\"unit_of_meas\":\"");
        payload += jsonEscape(unit);
        payload += '"';
    }
    if (!entityCategory.isEmpty()) {
        payload += F(",\"ent_cat\":\"");
        payload += jsonEscape(entityCategory);
        payload += '"';
    }
    if (!stateClass.isEmpty()) {
        payload += F(",\"stat_cla\":\"");
        payload += jsonEscape(stateClass);
        payload += '"';
    }
    if (component == "binary_sensor") {
        payload += F(",\"json_attr_t\":\"");
        payload += jsonEscape(stateTopic);
        payload += '"';
    }
    payload += F(",\"dev\":");
    payload += deviceJson();
    payload += F(",\"o\":{\"name\":\"2GIGMQTT\",\"sw\":\"");
    payload += kFirmwareRelease;
    payload += F("\"}}");

    const String discoveryTopic = String("homeassistant/") + component + '/' +
                                  deviceId_ + '_' + objectId + "/config";
    return client_.publish(discoveryTopic.c_str(), payload.c_str(), true);
}

bool MqttService::publishAlarmState() {
    if (state_.alarmState() == "unknown") return true;
    if (!publishRetained("panel/state", state_.alarmState())) return false;

    String payload = F("{\"state\":\"");
    payload += jsonEscape(state_.alarmState());
    payload += F("\",\"mode\":\"");
    payload += jsonEscape(state_.alarmMode());
    payload += F("\",\"origin\":\"");
    payload += jsonEscape(state_.alarmOrigin());
    payload += F("\",\"user\":");
    if (state_.alarmUser() < 0) {
        payload += F("null");
    } else {
        payload += state_.alarmUser();
    }
    payload += F(",\"flags\":");
    payload += state_.alarmFlags();
    payload += F(",\"changed_at_ms\":");
    payload += state_.alarmChangedAtMs();
    payload += '}';
    if (!publishRetained("panel/status", payload)) return false;

    // User 0 is a GC2-originated remote/system actor; positive IDs are keypad
    // users. Only the identifier and observed action are retained--never a
    // keypad code or other credential.
    if (state_.alarmUser() >= 0 && state_.alarmUser() <= 255) {
        char number[4];
        snprintf(number, sizeof(number), "%03u", state_.alarmUser());
        String userPayload = F("{\"id\":");
        userPayload += state_.alarmUser();
        userPayload += F(",\"last_action\":\"");
        userPayload += jsonEscape(state_.alarmState());
        userPayload += F("\",\"origin\":\"");
        userPayload += jsonEscape(state_.alarmOrigin());
        userPayload += F("\",\"last_seen_ms\":");
        userPayload += millis();
        userPayload += '}';
        if (!publishRetained(String("user/") + number + "/state",
                             userPayload)) {
            return false;
        }
    }
    return true;
}

bool MqttService::publishAlarmCommandStatus() {
    String payload = F("{\"action\":\"");
    payload += jsonEscape(bridge_.alarmCommandAction());
    payload += F("\",\"status\":\"");
    payload += jsonEscape(bridge_.alarmCommandStatus());
    payload += F("\",\"detail\":\"");
    payload += jsonEscape(bridge_.alarmCommandDetail());
    payload += F("\",\"uptime_ms\":");
    payload += millis();
    payload += '}';
    return publishRetained("panel/command_status", payload);
}

bool MqttService::publishBatteryState() {
    if (state_.batteryMillivolts() < 0) return true;
    String payload = F("{\"state\":\"");
    payload += jsonEscape(state_.batteryState());
    payload += F("\",\"millivolts\":");
    payload += state_.batteryMillivolts();
    payload += F(",\"summary\":\"");
    payload += jsonEscape(state_.batterySummary());
    payload += F("\"}");
    return publishRetained("panel/battery", payload);
}

bool MqttService::publishSounderVolumeState() {
    if (!state_.sounderVolumeKnown()) return true;
    return publishRetained("panel/sounder_volume",
                           String(state_.sounderVolumePercent()));
}

bool MqttService::publishFirmwareState() {
    if (state_.firmwareVersion().isEmpty()) return true;
    String payload = F("{\"build\":");
    payload += state_.firmwareBuild();
    payload += F(",\"version\":\"");
    payload += jsonEscape(state_.firmwareVersion());
    payload += F("\"}");
    return publishRetained("panel/firmware", payload);
}

bool MqttService::publishTroubleSummary() {
    if (!state_.troubleKnown()) return true;
    String payload = F("{\"known\":true,\"active\":");
    payload += state_.activeTroubleCount() > 0 ? F("true") : F("false");
    payload += F(",\"count\":");
    payload += state_.troubleCount();
    payload += F(",\"active_count\":");
    payload += state_.activeTroubleCount();
    payload += F(",\"unacknowledged_count\":");
    payload += state_.unacknowledgedTroubleCount();
    payload += '}';
    return publishRetained("panel/trouble", payload);
}

bool MqttService::publishTroubleEntry(uint8_t slot) {
    if (slot >= Gc2State::kMaxTroubles) return true;
    char number[3];
    snprintf(number, sizeof(number), "%02u", slot);
    const String suffix = String("panel/trouble/") + number;
    const Gc2TroubleSnapshot& trouble = state_.trouble(slot);
    if (!trouble.valid) return publishRetained(suffix, String());

    String payload;
    payload.reserve(300);
    payload += F("{\"slot\":");
    payload += trouble.slot;
    payload += F(",\"zone\":");
    payload += trouble.zone;
    payload += F(",\"device\":\"");
    payload += jsonEscape(trouble.device);
    payload += F("\",\"description\":\"");
    payload += jsonEscape(trouble.description);
    payload += F("\",\"active\":");
    payload += trouble.active ? F("true") : F("false");
    payload += F(",\"acknowledged\":");
    payload += trouble.acknowledged ? F("true") : F("false");
    payload += F(",\"raised_tick\":\"");
    payload += hexValue(trouble.raisedTick, 8);
    payload += F("\",\"restored_tick\":\"");
    payload += hexValue(trouble.restoredTick, 8);
    payload += F("\"}");
    return publishRetained(suffix, payload);
}

bool MqttService::publishPanelSecurityState() {
    const Gc2PanelSecuritySnapshot& security = state_.panelSecurity();
    if (!security.known) return true;
    const bool acLoss = security.acLossInstantaneous ||
                        security.acLossFiltered || security.acLossLowPower;
    const bool communicationFailure =
        security.phoneLineFailure || security.centralStationFailure ||
        security.cellularFailure || security.radioModemNetworkFailure ||
        security.ethernetNetworkFailure;
    String payload = F("{\"known\":true,\"ac_loss\":");
    payload += acLoss ? F("true") : F("false");
    payload += F(",\"ac_loss_instantaneous\":");
    payload += security.acLossInstantaneous ? F("true") : F("false");
    payload += F(",\"ac_loss_filtered\":");
    payload += security.acLossFiltered ? F("true") : F("false");
    payload += F(",\"ac_loss_lpm\":");
    payload += security.acLossLowPower ? F("true") : F("false");
    payload += F(",\"panel_tamper\":");
    payload += security.panelTamper ? F("true") : F("false");
    payload += F(",\"siren_tamper\":");
    payload += security.sirenTamper ? F("true") : F("false");
    payload += F(",\"rf_jam\":");
    payload += security.rfJam ? F("true") : F("false");
    payload += F(",\"communication_failure\":");
    payload += communicationFailure ? F("true") : F("false");
    payload += F(",\"phone_line_failure\":");
    payload += security.phoneLineFailure ? F("true") : F("false");
    payload += F(",\"central_station_failure\":");
    payload += security.centralStationFailure ? F("true") : F("false");
    payload += F(",\"cellular_failure\":");
    payload += security.cellularFailure ? F("true") : F("false");
    payload += F(",\"radio_modem_network_failure\":");
    payload += security.radioModemNetworkFailure ? F("true") : F("false");
    payload += F(",\"ethernet_network_failure\":");
    payload += security.ethernetNetworkFailure ? F("true") : F("false");
    payload += F(",\"reset_required\":");
    payload += security.resetRequired ? F("true") : F("false");
    payload += F(",\"sampled_at_ms\":");
    payload += security.sampledAtMs;
    payload += '}';
    return publishRetained("panel/security", payload);
}

bool MqttService::publishAlarmMemoryState() {
    const Gc2AlarmMemorySnapshot& memory = state_.alarmMemory();
    if (!memory.known) return true;
    String payload = F("{\"known\":true,\"clear\":");
    payload += memory.clear ? F("true") : F("false");
    payload += F(",\"latched\":");
    payload += memory.latched ? F("true") : F("false");
    payload += F(",\"bell_timeout\":");
    payload += memory.bellTimeout ? F("true") : F("false");
    payload += F(",\"reported_alarm_type\":");
    payload += memory.reportedAlarmType;
    payload += F(",\"sampled_at_ms\":");
    payload += memory.sampledAtMs;
    payload += '}';
    return publishRetained("panel/alarm_memory", payload);
}

bool MqttService::publishZoneState(uint8_t zoneNumber) {
    const Gc2ZoneSnapshot& zone = state_.zone(zoneNumber);
    char number[3];
    snprintf(number, sizeof(number), "%02u", zoneNumber);
    String payload = F("{\"state\":\"");
    payload += zone.stateKnown ? (zone.open ? F("ON") : F("OFF"))
                               : F("UNKNOWN");
    payload += F("\",\"name\":\"");
    payload += jsonEscape(zone.name);
    payload += F("\",\"zone_type\":\"");
    payload += jsonEscape(zone.zoneType);
    payload += F("\",\"enabled\":");
    payload += zone.enabled ? F("true") : F("false");
    payload += F(",\"input\":");
    payload += zone.input;
    payload += F(",\"bypass_known\":");
    payload += zone.bypassKnown ? F("true") : F("false");
    payload += F(",\"bypassed\":");
    payload += zone.bypassed ? F("true") : F("false");
    payload += F(",\"bypass_user\":");
    payload += zone.bypassUser;
    payload += F(",\"bypass_type\":\"");
    payload += jsonEscape(zone.bypassType);
    payload += '"';
    payload += F(",\"bypass_origin\":\"");
    payload += jsonEscape(zone.bypassOrigin);
    payload += '"';
    payload += F(",\"rf_id\":\"");
    payload += hexValue(zone.rfId, 6);
    payload += F("\",\"raw_status\":\"");
    payload += hexValue(zone.rawStatus, 2);
    payload += F("\",\"raw_change\":\"");
    payload += hexValue(zone.rawStatusChange, 2);
    payload += F("\",\"battery_known\":");
    payload += zone.batteryKnown ? F("true") : F("false");
    payload += F(",\"battery_low\":");
    payload += zone.batteryLow ? F("true") : F("false");
    payload += F(",\"trouble_known\":");
    payload += zone.troubleKnown ? F("true") : F("false");
    payload += F(",\"trouble_active\":");
    payload += zone.troubleActive ? F("true") : F("false");
    payload += F(",\"tamper\":");
    payload += zone.tamper ? F("true") : F("false");
    payload += F(",\"supervision_lost\":");
    payload += zone.supervisionLost ? F("true") : F("false");
    payload += F(",\"trouble_summary\":\"");
    payload += jsonEscape(zone.troubleSummary);
    payload += '"';
    payload += F(",\"last_seen_ms\":");
    payload += zone.lastSeenMs;
    payload += '}';
    if (!publishRetained(String("zone/") + number + "/state", payload)) {
        return false;
    }

    if (zone.bypassKnown) {
        char userNumber[4];
        snprintf(userNumber, sizeof(userNumber), "%03u", zone.bypassUser);
        String userPayload = F("{\"id\":");
        userPayload += zone.bypassUser;
        userPayload += F(",\"last_action\":\"");
        userPayload += zone.bypassed ? F("bypass_zone_") : F("unbypass_zone_");
        userPayload += zoneNumber;
        userPayload += F("\",\"origin\":\"");
        userPayload += jsonEscape(zone.bypassOrigin);
        userPayload += F("\",\"last_seen_ms\":");
        userPayload += millis();
        userPayload += '}';
        if (!publishRetained(String("user/") + userNumber + "/state",
                             userPayload)) {
            return false;
        }
    }
    return true;
}

bool MqttService::publishDiagnostics() {
    String payload = F("{\"received_lines\":");
    payload += state_.receivedLineCount();
    payload += F(",\"unknown_lines\":");
    payload += state_.unknownLineCount();
    payload += F(",\"rf_supervision\":");
    payload += state_.rfSupervisionCount();
    payload += F(",\"zwave_ack\":");
    payload += state_.zwaveAckCount();
    payload += F(",\"zwave_no_ack\":");
    payload += state_.zwaveNoAckCount();
    payload += F(",\"zwave_max_retry\":");
    payload += state_.zwaveMaxRetryCount();
    payload += F(",\"dropped_events\":");
    payload += state_.droppedEventCount();
    payload += F(",\"last_panel_line_ms\":");
    payload += state_.lastPanelLineMs();
    payload += F(",\"debug_unlock\":\"");
    payload += bridge_.debugUnlockState();
    payload += '"';
    payload += '}';
    return publishRetained("diagnostic", payload);
}

bool MqttService::publishBaudState() {
    const String state = bridge_.baudState();
    if (!publishRetained("uart/baud", state)) return false;
    return client_.publish(baudCommandTopic_.c_str(), state.c_str(), true);
}

bool MqttService::publishRetained(const String& suffix,
                                  const String& payload) {
    return client_.publish(makeTopic(suffix).c_str(), payload.c_str(), true);
}

String MqttService::makeTopic(const String& suffix) const {
    return rootTopic_ + '/' + suffix;
}

String MqttService::deviceJson() const {
    String json = F("{\"ids\":[\"");
    json += deviceId_;
    json += F("\"],\"name\":\"");
    json += jsonEscape(DeviceIdentity::displayName());
    json += F("\",\"mf\":\"2GIG / Waveshare\",\"mdl\":\"ESP32-S3 UART Bridge\",\"sw\":\"");
    json += kFirmwareRelease;
    json += F("\"}");
    return json;
}

String MqttService::jsonEscape(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (const char character : value) {
        switch (character) {
            case '\\': escaped += F("\\\\"); break;
            case '"': escaped += F("\\\""); break;
            case '\n': escaped += F("\\n"); break;
            case '\r': escaped += F("\\r"); break;
            case '\t': escaped += F("\\t"); break;
            default:
                if (static_cast<uint8_t>(character) >= 0x20) {
                    escaped += character;
                }
                break;
        }
    }
    return escaped;
}

String MqttService::hexValue(uint32_t value, uint8_t width) {
    char formatted[9];
    snprintf(formatted, sizeof(formatted), "%0*lX", width,
             static_cast<unsigned long>(value));
    return String(formatted);
}
