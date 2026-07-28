#include "ModeController.h"

#include "HardwareConfig.h"

void ModeController::setMode(HomeMode mode, AppState &state,
                             LightController &light,
                             bool sensorValueIsValid) {
    state.mode = mode;

    if (mode == HomeMode::Manual) {
        applyLight(state.manualLightOn, state, light);
    } else {
        if (sensorValueIsValid) {
            evaluateNightMode(state, light);
        } else {
            applyLight(false, state, light);
        }
    }
}

void ModeController::evaluateNightMode(AppState &state,
                                       LightController &light) {
    if (state.mode != HomeMode::Night) {
        return;
    }

    if (state.sensorFiltered < NIGHT_ON_THRESHOLD) {
        applyLight(true, state, light);
    } else if (state.sensorFiltered > NIGHT_OFF_THRESHOLD) {
        applyLight(false, state, light);
    }
}

void ModeController::applyLight(bool on, AppState &state,
                                LightController &light) {
    light.setLight(on);
    state.actualLightOn = on;
}
