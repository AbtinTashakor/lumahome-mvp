#pragma once

#include "AppState.h"
#include "LightController.h"

class ModeController {
public:
    void setMode(HomeMode mode, AppState &state, LightController &light,
                 bool sensorValueIsValid);
    void evaluateNightMode(AppState &state, LightController &light);

private:
    void applyLight(bool on, AppState &state, LightController &light);
};
