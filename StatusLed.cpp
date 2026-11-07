#include "StatusLed.h"

#include <esp32-hal-rgb-led.h>

void StatusLed::begin(bool provisioned) {
    provisioned_ = provisioned;
    // Once provisioned the LED stays dark to save power drawn from the
    // panel's console header; red still marks a bridge that needs setup.
    if (provisioned_) {
        showOff();
    } else {
        showRed();
    }
}

void StatusLed::setProvisioned(bool provisioned) {
    if (provisioned_ == provisioned) return;
    begin(provisioned);
}

void StatusLed::loop() {}

void StatusLed::showRed() {
    rgbLedWrite(RGB_BUILTIN, kBrightness, 0, 0);
}

void StatusLed::showOff() {
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
}
