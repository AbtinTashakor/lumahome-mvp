#pragma once

#include <Arduino.h>

#include "AppState.h"

constexpr uint32_t PERSISTED_CONFIG_MAGIC = 0x4C554D41;
constexpr uint8_t PERSISTED_RECORD_VERSION = 2;
constexpr int PERSISTED_CONFIG_ADDRESS = 0;
constexpr uint32_t CONFIG_SAVE_DELAY_MS = 750;
constexpr uint32_t CONFIG_SAVE_RETRY_MS = 5000;

enum class ConfigLoadResult : uint8_t {
    Valid,
    Migrated,
    Invalid
};

class ConfigStore {
public:
    ConfigLoadResult load(HomeMode &mode, bool &manualLightOn,
                          uint32_t &configVersion);
    bool save(HomeMode mode, bool manualLightOn, uint32_t configVersion);

private:
    bool hasSavedConfig_ = false;
    HomeMode savedMode_ = HomeMode::Manual;
    bool savedManualLightOn_ = false;
    uint32_t savedConfigVersion_ = 0;
};
