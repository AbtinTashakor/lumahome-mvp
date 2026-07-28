#include "LightSensor.h"

void LightSensor::begin(uint32_t now) {
    pinMode(PIN_LIGHT_SENSOR, INPUT);
    lastSampleAt_ = now - LIGHT_SAMPLE_INTERVAL_MS;
}

bool LightSensor::update(uint32_t now) {
    if (now - lastSampleAt_ < LIGHT_SAMPLE_INTERVAL_MS) {
        return false;
    }

    lastSampleAt_ = now;
    raw_ = static_cast<uint16_t>(analogRead(PIN_LIGHT_SENSOR));

    if (sampleCount_ < LIGHT_SAMPLE_COUNT) {
        samples_[nextSampleIndex_] = raw_;
        runningSum_ += raw_;
        ++sampleCount_;
    } else {
        runningSum_ -= samples_[nextSampleIndex_];
        samples_[nextSampleIndex_] = raw_;
        runningSum_ += raw_;
    }

    nextSampleIndex_ = (nextSampleIndex_ + 1) % LIGHT_SAMPLE_COUNT;
    filtered_ = static_cast<uint16_t>(runningSum_ / sampleCount_);
    return true;
}

bool LightSensor::hasSample() const {
    return sampleCount_ > 0;
}

uint16_t LightSensor::raw() const {
    return raw_;
}

uint16_t LightSensor::filtered() const {
    return filtered_;
}
