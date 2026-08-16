#pragma once

#include <Arduino.h>

class StatusLed {
  public:
    void begin(bool provisioned);
    void setProvisioned(bool provisioned);
    void loop();

  private:
    static constexpr uint8_t kBrightness = 24;
    static constexpr uint32_t kBlueFlashIntervalMs = 500;

    bool provisioned_ = false;
    bool blueOn_ = false;
    uint32_t nextToggleAt_ = 0;

    void showRed();
    void showBlue(bool on);
};
