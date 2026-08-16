#include "ProvisioningPortal.h"

#include "AppConfig.h"
#include "DeviceIdentity.h"

#include <WiFi.h>

namespace {
IPAddress kPortalAddress(192, 168, 4, 1);
IPAddress kPortalNetmask(255, 255, 255, 0);
}  // namespace

String ProvisioningPortal::htmlEscape(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 16);
    for (const char character : value) {
        switch (character) {
            case '&': escaped += F("&amp;"); break;
            case '<': escaped += F("&lt;"); break;
            case '>': escaped += F("&gt;"); break;
            case '"': escaped += F("&quot;"); break;
            case '\'': escaped += F("&#39;"); break;
            default: escaped += character; break;
        }
    }
    return escaped;
}

void ProvisioningPortal::scanNetworks() {
    networkCount_ = 0;
    const int count = WiFi.scanNetworks(false, true);
    for (int index = 0; index < count && networkCount_ < kMaxNetworks;
         ++index) {
        const String ssid = WiFi.SSID(index);
        if (ssid.isEmpty()) continue;

        bool duplicate = false;
        for (size_t existing = 0; existing < networkCount_; ++existing) {
            if (networks_[existing].ssid == ssid) {
                duplicate = true;
                if (WiFi.RSSI(index) > networks_[existing].rssi) {
                    networks_[existing].rssi = WiFi.RSSI(index);
                }
                break;
            }
        }
        if (duplicate) continue;

        networks_[networkCount_].ssid = ssid;
        networks_[networkCount_].rssi = WiFi.RSSI(index);
        networks_[networkCount_].secured =
            WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
        ++networkCount_;
    }
    WiFi.scanDelete();
}

void ProvisioningPortal::configureRoutes() {
    web_.on("/", HTTP_GET, [this]() { handleRoot(); });
    web_.on("/save", HTTP_POST, [this]() { handleSave(); });

    for (const char* path : {"/generate_204", "/hotspot-detect.html",
                             "/connecttest.txt", "/ncsi.txt"}) {
        web_.on(path, HTTP_ANY, [this]() { redirectToPortal(); });
    }
    web_.onNotFound([this]() { redirectToPortal(); });
}

void ProvisioningPortal::begin() {
    if (active_) return;

    accessPointName_ = DeviceIdentity::setupSsid();

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(kPortalAddress, kPortalAddress, kPortalNetmask);
    if (!WiFi.softAP(accessPointName_.c_str())) {
        Serial.println(F("[wifi] Failed to start provisioning access point."));
        return;
    }

    Serial.println();
    Serial.println(F("[wifi] Provisioning portal active"));
    Serial.print(F("[wifi] Network:  "));
    Serial.println(accessPointName_);
    Serial.println(F("[wifi] Password: none (open network)"));
    Serial.println(F("[wifi] Open http://192.168.4.1/ if the portal does not appear."));

    scanNetworks();
    if (!routesConfigured_) {
        configureRoutes();
        routesConfigured_ = true;
    }
    dns_.start(kDnsPort, "*", kPortalAddress);
    web_.begin();
    statusMessage_ = F("Choose the Wi-Fi network this bridge should join.");
    active_ = true;
}

void ProvisioningPortal::loop() {
    if (!active_) return;
    dns_.processNextRequest();
    web_.handleClient();

    if (!connectionPending_) return;
    if (WiFi.status() == WL_CONNECTED) {
        connectionPending_ = false;
        settingsReady_ = true;
        statusMessage_ = F("Connected. The bridge is leaving setup mode.");
        return;
    }

    if (millis() - connectionStartedAt_ >= AppConfig::kWifiConnectTimeoutMs) {
        connectionPending_ = false;
        WiFi.disconnect(false, true);
        pendingSettings_.wifiPassword = String();
        pendingSettings_.telnetPassword = String();
        pendingSettings_.mqtt.password = String();
        statusMessage_ =
            F("Connection failed. Check the network name and password, then try again.");
    }
}

bool ProvisioningPortal::takeConnectedSettings(
    ProvisionedSettings& settings) {
    if (!settingsReady_) return false;
    settings = pendingSettings_;
    settingsReady_ = false;
    return true;
}

void ProvisioningPortal::stop() {
    if (!active_) return;
    dns_.stop();
    web_.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    active_ = false;
    pendingSettings_ = ProvisionedSettings{};
}

void ProvisioningPortal::handleRoot() {
    web_.sendHeader("Cache-Control", "no-store");
    web_.send(200, "text/html; charset=utf-8", buildPage());
}

void ProvisioningPortal::handleSave() {
    if (connectionPending_) {
        web_.send(409, "text/plain", "A connection attempt is already running.");
        return;
    }

    String ssid = web_.arg("ssid");
    String wifiPassword = web_.arg("wifi_password");
    String telnetPassword = web_.arg("telnet_password");
    String confirmation = web_.arg("telnet_confirmation");
    const bool mqttEnabled = web_.hasArg("mqtt_enabled");
    String mqttHost = web_.arg("mqtt_host");
    const long mqttPort = web_.arg("mqtt_port").toInt();
    String mqttUsername = web_.arg("mqtt_username");
    String mqttPassword = web_.arg("mqtt_password");
    String mqttBaseTopic = web_.arg("mqtt_base_topic");
    const bool mqttDiscovery = web_.hasArg("mqtt_discovery");
    ssid.trim();
    mqttHost.trim();
    mqttBaseTopic.trim();

    if (ssid.isEmpty() || ssid.length() > 32) {
        statusMessage_ = F("Enter a valid Wi-Fi network name.");
        handleRoot();
        return;
    }
    if (wifiPassword.length() > 64) {
        statusMessage_ = F("The Wi-Fi password is too long.");
        handleRoot();
        return;
    }
    if (telnetPassword.length() < 8 || telnetPassword.length() > 64) {
        statusMessage_ = F("The bridge password must be 8 to 64 characters.");
        handleRoot();
        return;
    }
    if (telnetPassword != confirmation) {
        statusMessage_ = F("The bridge passwords do not match.");
        handleRoot();
        return;
    }
    if (mqttEnabled &&
        (mqttHost.isEmpty() || mqttHost.length() > 253 ||
         mqttHost.indexOf(' ') >= 0 || mqttPort < 1 || mqttPort > 65535)) {
        statusMessage_ = F("Enter a valid MQTT broker and port.");
        handleRoot();
        return;
    }
    if (mqttUsername.length() > 128 || mqttPassword.length() > 128) {
        statusMessage_ = F("The MQTT username or password is too long.");
        handleRoot();
        return;
    }
    if (mqttEnabled &&
        (mqttBaseTopic.isEmpty() || mqttBaseTopic.length() > 96 ||
         mqttBaseTopic.indexOf('#') >= 0 ||
         mqttBaseTopic.indexOf('+') >= 0 ||
         mqttBaseTopic.startsWith("/") || mqttBaseTopic.endsWith("/"))) {
        statusMessage_ = F("Enter an MQTT base topic without wildcards or leading/trailing slashes.");
        handleRoot();
        return;
    }

    pendingSettings_.ssid = ssid;
    pendingSettings_.wifiPassword = wifiPassword;
    pendingSettings_.telnetPassword = telnetPassword;
    pendingSettings_.mqtt.enabled = mqttEnabled;
    pendingSettings_.mqtt.host = mqttHost;
    pendingSettings_.mqtt.port =
        mqttPort >= 1 && mqttPort <= 65535 ? static_cast<uint16_t>(mqttPort)
                                          : 1883;
    pendingSettings_.mqtt.username = mqttUsername;
    pendingSettings_.mqtt.password = mqttPassword;
    pendingSettings_.mqtt.baseTopic =
        mqttBaseTopic.isEmpty() ? String("2gig/gc2") : mqttBaseTopic;
    pendingSettings_.mqtt.discovery = mqttDiscovery;

    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), wifiPassword.c_str());
    connectionStartedAt_ = millis();
    connectionPending_ = true;
    statusMessage_ = F("Connecting to Wi-Fi. This can take up to 30 seconds.");
    handleRoot();

    wifiPassword = String();
    telnetPassword = String();
    confirmation = String();
    mqttPassword = String();
}

void ProvisioningPortal::redirectToPortal() {
    web_.sendHeader("Location", "http://192.168.4.1/", true);
    web_.send(302, "text/plain", "");
}

String ProvisioningPortal::buildPage() const {
    String page;
    page.reserve(9000);
    page += F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>");
    page += F("<title>GC2 Bridge Setup</title><style>body{font-family:system-ui,sans-serif;max-width:36rem;margin:2rem auto;padding:0 1rem;background:#f4f6f8;color:#17202a}main{background:white;padding:1.5rem;border-radius:.8rem;box-shadow:0 2px 12px #0002}label{display:block;margin-top:1rem;font-weight:600}input{box-sizing:border-box;width:100%;padding:.7rem;margin-top:.35rem}button{margin-top:1.25rem;padding:.75rem 1rem;background:#075985;color:white;border:0;border-radius:.35rem;font-weight:700}.status{padding:.8rem;background:#e0f2fe;border-left:4px solid #0284c7}small{display:block;color:#52606d;margin-top:.35rem}</style></head><body><main>");
    page += F("<h1>GC2 UART Bridge</h1><p class=status>");
    page += htmlEscape(statusMessage_);
    page += F("</p><form method=post action='/save' autocomplete=off><label for=ssid>Wi-Fi network</label><input id=ssid name=ssid list=networks maxlength=32 required><datalist id=networks>");
    for (size_t index = 0; index < networkCount_; ++index) {
        page += F("<option value=\"");
        page += htmlEscape(networks_[index].ssid);
        page += F("\">");
        page += String(networks_[index].rssi);
        page += F(" dBm");
        if (networks_[index].secured) page += F(" secured");
        page += F("</option>");
    }
    page += F("</datalist><label for=wifi_password>Wi-Fi password</label><input id=wifi_password name=wifi_password type=password maxlength=64><label for=telnet_password>Bridge password</label><input id=telnet_password name=telnet_password type=password minlength=8 maxlength=64 required><small>Tera Term will request this password before allowing panel access.</small><label for=telnet_confirmation>Confirm bridge password</label><input id=telnet_confirmation name=telnet_confirmation type=password minlength=8 maxlength=64 required><h2>MQTT</h2><label><input style='width:auto' type=checkbox name=mqtt_enabled checked> Enable MQTT publishing</label><label for=mqtt_host>Broker hostname or IP</label><input id=mqtt_host name=mqtt_host maxlength=253 placeholder='homeassistant.local'><label for=mqtt_port>Broker port</label><input id=mqtt_port name=mqtt_port type=number min=1 max=65535 value=1883><label for=mqtt_username>MQTT username</label><input id=mqtt_username name=mqtt_username maxlength=128 autocomplete=username><label for=mqtt_password>MQTT password</label><input id=mqtt_password name=mqtt_password type=password maxlength=128 autocomplete=new-password><label for=mqtt_base_topic>Base topic</label><input id=mqtt_base_topic name=mqtt_base_topic maxlength=96 value='2gig/gc2'><small>The bridge adds its unique device ID below this topic.</small><label><input style='width:auto' type=checkbox name=mqtt_discovery checked> Publish Home Assistant discovery</label><button type=submit>Save and connect</button></form></main></body></html>");
    return page;
}
