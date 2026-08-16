#include "StatusLed.h"

#include <esp32-hal-rgb-led.h>

namespace {
bool deadlineReached(uint32_t deadline) {
    return static_cast<int32_t>(millis() - deadline) >= 0;
}
}  // namespace

void StatusLed::begin(bool provisioned) {
    provisioned_ = provisioned;
    blueOn_ = false;
    if (provisioned_) {
        showBlue(true);
        blueOn_ = true;
        nextToggleAt_ = millis() + kBlueFlashIntervalMs;
    } else {
        showRed();
    }
}

void StatusLed::setProvisioned(bool provisioned) {
    if (provisioned_ == provisioned) return;
    begin(provisioned);
}

void StatusLed::loop() {
    if (!provisioned_ || !deadlineReached(nextToggleAt_)) return;
    blueOn_ = !blueOn_;
    showBlue(blueOn_);
    nextToggleAt_ = millis() + kBlueFlashIntervalMs;
}

void StatusLed::showRed() {
    rgbLedWrite(RGB_BUILTIN, kBrightness, 0, 0);
}

void StatusLed::showBlue(bool on) {
    rgbLedWrite(RGB_BUILTIN, 0, 0, on ? kBrightness : 0);
}
