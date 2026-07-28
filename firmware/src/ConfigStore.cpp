#include "ConfigStore.h"

#include <EEPROM.h>

namespace {

struct PersistedConfigV1 {
    uint32_t magic;
    uint8_t version;
    uint8_t mode;
    uint8_t manualLightOn;
    uint8_t reserved;
    uint32_t checksum;
};

struct PersistedConfigV2 {
    uint32_t magic;
    uint8_t recordVersion;
    uint8_t mode;
    uint8_t manualLightOn;
    uint8_t reserved;
    uint32_t configVersion;
    uint32_t checksum;
};

static_assert(sizeof(PersistedConfigV1) == 12,
              "Version 1 configuration layout changed");
static_assert(sizeof(PersistedConfigV2) == 16,
              "Version 2 configuration layout changed");

constexpr uint8_t PERSISTED_RECORD_VERSION_V1 = 1;
constexpr uint32_t FNV_OFFSET_BASIS = 2166136261UL;
constexpr uint32_t FNV_PRIME = 16777619UL;

uint32_t addChecksumByte(uint32_t checksum, uint8_t value) {
    return (checksum ^ value) * FNV_PRIME;
}

uint32_t addChecksumUint32(uint32_t checksum, uint32_t value) {
    checksum = addChecksumByte(checksum, static_cast<uint8_t>(value));
    checksum = addChecksumByte(checksum, static_cast<uint8_t>(value >> 8));
    checksum = addChecksumByte(checksum, static_cast<uint8_t>(value >> 16));
    return addChecksumByte(checksum, static_cast<uint8_t>(value >> 24));
}

uint32_t calculateV1Checksum(const PersistedConfigV1 &config) {
    uint32_t checksum = addChecksumUint32(FNV_OFFSET_BASIS, config.magic);
    checksum = addChecksumByte(checksum, config.version);
    checksum = addChecksumByte(checksum, config.mode);
    checksum = addChecksumByte(checksum, config.manualLightOn);
    return addChecksumByte(checksum, config.reserved);
}

uint32_t calculateV2Checksum(const PersistedConfigV2 &config) {
    // Hash fields explicitly so compiler padding cannot affect storage.
    uint32_t checksum = addChecksumUint32(FNV_OFFSET_BASIS, config.magic);
    checksum = addChecksumByte(checksum, config.recordVersion);
    checksum = addChecksumByte(checksum, config.mode);
    checksum = addChecksumByte(checksum, config.manualLightOn);
    checksum = addChecksumByte(checksum, config.reserved);
    return addChecksumUint32(checksum, config.configVersion);
}

bool modeIsValid(uint8_t mode) {
    return mode == static_cast<uint8_t>(HomeMode::Manual) ||
           mode == static_cast<uint8_t>(HomeMode::Night);
}

bool isValidV1(const PersistedConfigV1 &config) {
    return config.magic == PERSISTED_CONFIG_MAGIC &&
           config.version == PERSISTED_RECORD_VERSION_V1 &&
           modeIsValid(config.mode) && config.manualLightOn <= 1 &&
           config.checksum == calculateV1Checksum(config);
}

bool isValidV2(const PersistedConfigV2 &config) {
    return config.magic == PERSISTED_CONFIG_MAGIC &&
           config.recordVersion == PERSISTED_RECORD_VERSION &&
           modeIsValid(config.mode) && config.manualLightOn <= 1 &&
           config.checksum == calculateV2Checksum(config);
}

PersistedConfigV2 makeRecord(HomeMode mode, bool manualLightOn,
                             uint32_t configVersion) {
    PersistedConfigV2 config{PERSISTED_CONFIG_MAGIC,
                             PERSISTED_RECORD_VERSION,
                             static_cast<uint8_t>(mode),
                             static_cast<uint8_t>(manualLightOn ? 1 : 0),
                             0,
                             configVersion,
                             0};
    config.checksum = calculateV2Checksum(config);
    return config;
}

bool recordsMatch(const PersistedConfigV2 &left,
                  const PersistedConfigV2 &right) {
    return left.magic == right.magic &&
           left.recordVersion == right.recordVersion &&
           left.mode == right.mode &&
           left.manualLightOn == right.manualLightOn &&
           left.reserved == right.reserved &&
           left.configVersion == right.configVersion &&
           left.checksum == right.checksum;
}

} // namespace

ConfigLoadResult ConfigStore::load(HomeMode &mode, bool &manualLightOn,
                                   uint32_t &configVersion) {
    PersistedConfigV2 current{};
    EEPROM.get(PERSISTED_CONFIG_ADDRESS, current);

    if (isValidV2(current)) {
        mode = static_cast<HomeMode>(current.mode);
        manualLightOn = current.manualLightOn == 1;
        configVersion = current.configVersion;
        savedMode_ = mode;
        savedManualLightOn_ = manualLightOn;
        savedConfigVersion_ = configVersion;
        hasSavedConfig_ = true;
        return ConfigLoadResult::Valid;
    }

    PersistedConfigV1 legacy{};
    EEPROM.get(PERSISTED_CONFIG_ADDRESS, legacy);
    if (isValidV1(legacy)) {
        mode = static_cast<HomeMode>(legacy.mode);
        manualLightOn = legacy.manualLightOn == 1;
        configVersion = 0;
        hasSavedConfig_ = false;
        return ConfigLoadResult::Migrated;
    }

    mode = HomeMode::Manual;
    manualLightOn = false;
    configVersion = 0;
    hasSavedConfig_ = false;
    return ConfigLoadResult::Invalid;
}

bool ConfigStore::save(HomeMode mode, bool manualLightOn,
                       uint32_t configVersion) {
    if (hasSavedConfig_ && mode == savedMode_ &&
        manualLightOn == savedManualLightOn_ &&
        configVersion == savedConfigVersion_) {
        return true;
    }

    const PersistedConfigV2 expected =
        makeRecord(mode, manualLightOn, configVersion);
    EEPROM.put(PERSISTED_CONFIG_ADDRESS, expected);

    PersistedConfigV2 stored{};
    EEPROM.get(PERSISTED_CONFIG_ADDRESS, stored);
    if (!isValidV2(stored) || !recordsMatch(expected, stored)) {
        return false;
    }

    savedMode_ = mode;
    savedManualLightOn_ = manualLightOn;
    savedConfigVersion_ = configVersion;
    hasSavedConfig_ = true;
    return true;
}
