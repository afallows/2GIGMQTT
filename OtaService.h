#pragma once

#include <Arduino.h>

#include "CredentialStore.h"

class OtaService {
  public:
    explicit OtaService(CredentialStore& credentials);

    void begin();
    void setNetworkReady(bool ready);
    void stopNetwork();
    void loop();

  private:
    CredentialStore& credentials_;
    bool callbacksConfigured_ = false;
    bool networkReady_ = false;
    bool active_ = false;
    bool missingPasswordNoticeShown_ = false;
    uint32_t nextStartAttemptAt_ = 0;
    uint8_t lastProgressPercent_ = 255;

    void configureCallbacks();
    void startIfPossible();
    static bool deadlineReached(uint32_t deadline);
};
