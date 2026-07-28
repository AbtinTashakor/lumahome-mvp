#pragma once

#include <Arduino.h>

#include "HardwareConfig.h"

class LightSensor {
public:
    void begin(uint32_t now);
    bool update(uint32_t now);
    bool hasSample() const;
    uint16_t raw() const;
    uint16_t filtered() const;

private:
    uint16_t samples_[LIGHT_SAMPLE_COUNT]{};
    size_t sampleCount_ = 0;
    size_t nextSampleIndex_ = 0;
    uint32_t runningSum_ = 0;
    uint32_t lastSampleAt_ = 0;
    uint16_t raw_ = 0;
    uint16_t filtered_ = 0;
};
