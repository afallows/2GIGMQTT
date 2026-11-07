#pragma once

#include <Arduino.h>

class StatusLed {
  public:
    void begin(bool provisioned);
    void setProvisioned(bool provisioned);
    void loop();

  private:
    static constexpr uint8_t kBrightness = 24;

    bool provisioned_ = false;

    void showRed();
    void showOff();
};
