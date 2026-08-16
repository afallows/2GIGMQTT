#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "CredentialStore.h"

struct ProvisionedSettings {
    String ssid;
    String wifiPassword;
    String telnetPassword;
    MqttSettings mqtt;
};

class ProvisioningPortal {
  public:
    void begin();
    void loop();
    void stop();

    bool active() const { return active_; }
    bool takeConnectedSettings(ProvisionedSettings& settings);

  private:
    static constexpr uint8_t kDnsPort = 53;
    static constexpr size_t kMaxNetworks = 20;

    struct NetworkEntry {
        String ssid;
        int32_t rssi = 0;
        bool secured = false;
    };

    DNSServer dns_;
    WebServer web_{80};
    NetworkEntry networks_[kMaxNetworks];
    size_t networkCount_ = 0;

    bool active_ = false;
    bool routesConfigured_ = false;
    bool connectionPending_ = false;
    bool settingsReady_ = false;
    uint32_t connectionStartedAt_ = 0;
    String statusMessage_;
    String accessPointName_;
    ProvisionedSettings pendingSettings_;

    void scanNetworks();
    void configureRoutes();
    void handleRoot();
    void handleSave();
    void redirectToPortal();
    String buildPage() const;
    static String htmlEscape(const String& value);
};
