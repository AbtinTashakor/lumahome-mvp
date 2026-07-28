#pragma once

#include <Arduino.h>

enum class HomeMode : uint8_t {
    Manual,
    Night
};

struct AppState {
    HomeMode mode;
    bool manualLightOn;
    bool actualLightOn;
    uint16_t sensorRaw;
    uint16_t sensorFiltered;
    uint32_t configVersion;
};
