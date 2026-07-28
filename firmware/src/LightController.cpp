#include "LightController.h"

#include <Arduino.h>

#include "HardwareConfig.h"

void LightController::begin() {
    pinMode(PIN_RGB_RED, OUTPUT);
    pinMode(PIN_RGB_BLUE, OUTPUT);
    pinMode(PIN_RGB_GREEN, OUTPUT);

    digitalWrite(PIN_RGB_RED, LOW);
    digitalWrite(PIN_RGB_BLUE, LOW);
    digitalWrite(PIN_RGB_GREEN, LOW);
    lightOn_ = false;
}

void LightController::setLight(bool on) {
    if (on == lightOn_) {
        return;
    }

    const uint8_t level = on ? HIGH : LOW;
    digitalWrite(PIN_RGB_RED, level);
    digitalWrite(PIN_RGB_BLUE, level);
    digitalWrite(PIN_RGB_GREEN, level);
    lightOn_ = on;
}
