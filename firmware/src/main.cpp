#include <Arduino.h>
#include <cstring>

#include "AppState.h"
#include "BackendClient.h"
#include "ConfigStore.h"
#include "EventBuffer.h"
#include "HardwareConfig.h"
#include "LightController.h"
#include "LightSensor.h"
#include "ModeController.h"
#include "NetworkManager.h"

AppState appState{HomeMode::Manual, false, false, 0, 0, 0};
LightController lightController;
LightSensor lightSensor;
ModeController modeController;
ConfigStore configStore;
NetworkManager networkManager;
BackendClient backendClient;
EventBuffer eventBuffer;

char commandBuffer[SERIAL_COMMAND_CAPACITY]{};
size_t commandLength = 0;
bool commandOverflowed = false;

bool errorLedActive = false;
uint32_t errorLedStartedAt = 0;

bool configDirty = false;
uint32_t configDirtySince = 0;
uint32_t configSaveWaitMs = CONFIG_SAVE_DELAY_MS;
bool configValidAtBoot = false;
HomeMode restoredMode = HomeMode::Manual;
bool restoredManualLightOn = false;
uint32_t restoredConfigVersion = 0;
uint32_t bootId = 0;
bool hasLoggedIgnoredDesiredVersion = false;
uint32_t lastLoggedIgnoredDesiredVersion = 0;
bool eventDropWarningPrinted = false;
uint32_t lastEventDropWarningAt = 0;

void appendEvent(DeviceEventType type, uint8_t value, uint32_t now) {
    const EventAppendResult result = eventBuffer.append(type, value, now);
    if (result == EventAppendResult::Appended) {
        return;
    }

    if (!eventDropWarningPrinted ||
        now - lastEventDropWarningAt >= 1000) {
        Serial.println(result == EventAppendResult::OldestDropped
                           ? F("events: buffer full, oldest event dropped")
                           : F("events: sequence exhausted, event dropped"));
        eventDropWarningPrinted = true;
        lastEventDropWarningAt = now;
    }
}

void recordObservableChanges(HomeMode previousMode, bool previousLightOn,
                             uint32_t now) {
    if (appState.mode != previousMode) {
        appendEvent(DeviceEventType::ModeChanged,
                    static_cast<uint8_t>(appState.mode), now);
    }
    if (appState.actualLightOn != previousLightOn) {
        appendEvent(DeviceEventType::LightChanged,
                    appState.actualLightOn ? 1 : 0, now);
    }
}

void printHelp() {
    Serial.println(F("Commands:"));
    Serial.println(F("  help"));
    Serial.println(F("  status"));
    Serial.println(F("  mode manual"));
    Serial.println(F("  mode night"));
    Serial.println(F("  light on"));
    Serial.println(F("  light off"));
    Serial.println(F("  config"));
    Serial.println(F("  network"));
    Serial.println(F("  heartbeat"));
    Serial.println(F("  events"));
}

void printStatus() {
    Serial.print(F("mode="));
    Serial.print(appState.mode == HomeMode::Manual ? F("manual") : F("night"));
    Serial.print(F(" manual_light_on="));
    Serial.print(appState.manualLightOn ? F("true") : F("false"));
    Serial.print(F(" actual_light_on="));
    Serial.print(appState.actualLightOn ? F("true") : F("false"));
    Serial.print(F(" sensor_raw="));
    Serial.print(appState.sensorRaw);
    Serial.print(F(" sensor_filtered="));
    Serial.print(appState.sensorFiltered);
    Serial.print(F(" config_version="));
    Serial.print(appState.configVersion);
    Serial.print(F(" config_dirty="));
    Serial.print(configDirty ? F("true") : F("false"));
    Serial.print(F(" wifi="));
    Serial.print(networkManager.isConnected() ? F("connected")
                                              : F("disconnected"));
    Serial.print(F(" backend="));
    Serial.print(backendClient.isOnline() ? F("online") : F("offline"));
    Serial.print(F(" events_pending="));
    Serial.print(eventBuffer.count());
    Serial.print(F(" events_dropped="));
    Serial.println(eventBuffer.droppedCount());
}

void printConfig() {
    Serial.print(F("config_valid="));
    Serial.print(configValidAtBoot ? F("true") : F("false"));
    Serial.print(F(" record_version="));
    Serial.print(PERSISTED_RECORD_VERSION);
    Serial.print(F(" dirty="));
    Serial.print(configDirty ? F("true") : F("false"));
    Serial.print(F(" restored_mode="));
    Serial.print(restoredMode == HomeMode::Manual ? F("manual") : F("night"));
    Serial.print(F(" restored_manual_light_on="));
    Serial.print(restoredManualLightOn ? F("true") : F("false"));
    Serial.print(F(" restored_config_version="));
    Serial.println(restoredConfigVersion);
}

void printNetwork(uint32_t now) {
    Serial.print(F("wifi="));
    Serial.print(networkManager.isConnected() ? F("connected")
                                              : F("disconnected"));
    Serial.print(F(" backend="));
    Serial.print(backendClient.isOnline() ? F("online") : F("offline"));
    Serial.print(F(" ip="));
    if (networkManager.isConnected()) {
        Serial.print(networkManager.localIp());
        Serial.print(F(" rssi="));
        Serial.print(networkManager.rssi());
    } else {
        Serial.print(F("none"));
    }
    Serial.print(F(" heartbeat_interval_ms="));
    Serial.print(backendClient.heartbeatIntervalMs());
    Serial.print(F(" last_success_ms_ago="));
    if (backendClient.hasSuccessfulHeartbeat()) {
        Serial.println(now - backendClient.lastSuccessfulHeartbeatAt());
    } else {
        Serial.println(F("never"));
    }
}

void printEvents() {
    Serial.print(F("events_pending="));
    Serial.print(eventBuffer.count());
    Serial.print(F(" capacity="));
    Serial.print(EVENT_BUFFER_CAPACITY);
    Serial.print(F(" dropped="));
    Serial.print(eventBuffer.droppedCount());
    Serial.print(F(" next_seq="));
    if (eventBuffer.sequenceAvailable()) {
        Serial.print(eventBuffer.nextSequence());
    } else {
        Serial.print(F("none"));
    }

    uint32_t sequence = 0;
    Serial.print(F(" oldest_seq="));
    if (eventBuffer.oldestSequence(sequence)) {
        Serial.print(sequence);
    } else {
        Serial.print(F("none"));
    }
    Serial.print(F(" newest_seq="));
    if (eventBuffer.newestSequence(sequence)) {
        Serial.print(sequence);
    } else {
        Serial.print(F("none"));
    }
    Serial.print(F(" last_ack_seq="));
    if (eventBuffer.hasLastAcknowledgedSequence()) {
        Serial.print(eventBuffer.lastAcknowledgedSequence());
    } else {
        Serial.print(F("none"));
    }
    Serial.print(F(" batch_max="));
    Serial.println(MAX_EVENTS_PER_HEARTBEAT);
}

void markConfigDirty(uint32_t now) {
    configDirty = true;
    configDirtySince = now;
    configSaveWaitMs = CONFIG_SAVE_DELAY_MS;
}

void updateConfigSave(uint32_t now) {
    if (!configDirty || now - configDirtySince < configSaveWaitMs) {
        return;
    }

    if (configStore.save(appState.mode, appState.manualLightOn,
                         appState.configVersion)) {
        configDirty = false;
        Serial.println(F("config: saved"));
    } else {
        Serial.println(F("config: save failed"));
        configDirtySince = now;
        configSaveWaitMs = CONFIG_SAVE_RETRY_MS;
    }
}

void signalInvalidCommand(uint32_t now) {
    Serial.println(F("Error: invalid command. Type 'help' for commands."));
    errorLedActive = true;
    errorLedStartedAt = now;
}

char *trimCommand(char *command) {
    while (*command == ' ' || *command == '\t') {
        ++command;
    }

    char *end = command + std::strlen(command);
    while (end > command && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    *end = '\0';
    return command;
}

void handleCommand(char *command, uint32_t now) {
    command = trimCommand(command);
    if (*command == '\0') {
        return;
    }

    const HomeMode previousMode = appState.mode;
    const bool previousLightOn = appState.actualLightOn;

    if (std::strcmp(command, "help") == 0) {
        printHelp();
    } else if (std::strcmp(command, "status") == 0) {
        printStatus();
    } else if (std::strcmp(command, "config") == 0) {
        printConfig();
    } else if (std::strcmp(command, "network") == 0) {
        printNetwork(now);
    } else if (std::strcmp(command, "heartbeat") == 0) {
        if (networkManager.isConnected()) {
            backendClient.requestHeartbeat();
            Serial.println(F("heartbeat: scheduled"));
        } else {
            Serial.println(F("heartbeat: unavailable, wifi disconnected"));
        }
    } else if (std::strcmp(command, "events") == 0) {
        printEvents();
    } else if (std::strcmp(command, "mode manual") == 0) {
        if (appState.mode != HomeMode::Manual) {
            modeController.setMode(HomeMode::Manual, appState, lightController,
                                   lightSensor.hasSample());
            markConfigDirty(now);
        }
        printStatus();
    } else if (std::strcmp(command, "mode night") == 0) {
        if (appState.mode != HomeMode::Night) {
            modeController.setMode(HomeMode::Night, appState, lightController,
                                   lightSensor.hasSample());
            markConfigDirty(now);
        }
        printStatus();
    } else if (std::strcmp(command, "light on") == 0 ||
               std::strcmp(command, "light off") == 0) {
        const bool requestedLightOn = std::strcmp(command, "light on") == 0;
        const bool commandChanged =
            requestedLightOn != appState.manualLightOn;
        appState.manualLightOn = requestedLightOn;
        if (appState.mode == HomeMode::Manual) {
            modeController.setMode(HomeMode::Manual, appState, lightController,
                                   lightSensor.hasSample());
        }
        if (commandChanged) {
            markConfigDirty(now);
        }
        printStatus();
    } else {
        signalInvalidCommand(now);
    }

    recordObservableChanges(previousMode, previousLightOn, now);
}

void processSerial(uint32_t now) {
    while (Serial.available() > 0) {
        const char incoming = static_cast<char>(Serial.read());

        if (incoming == '\r') {
            continue;
        }

        if (incoming == '\n') {
            if (commandOverflowed) {
                signalInvalidCommand(now);
            } else {
                commandBuffer[commandLength] = '\0';
                handleCommand(commandBuffer, now);
            }
            commandLength = 0;
            commandOverflowed = false;
            continue;
        }

        if (!commandOverflowed) {
            if (commandLength < SERIAL_COMMAND_CAPACITY - 1) {
                commandBuffer[commandLength++] = incoming;
            } else {
                commandOverflowed = true;
            }
        }
    }
}

void updateErrorLed(uint32_t now) {
    if (errorLedActive &&
        now - errorLedStartedAt >= ERROR_LED_DURATION_MS) {
        errorLedActive = false;
    }
}

void updateStatusLeds() {
    const bool fullyOnline =
        networkManager.isConnected() && backendClient.isOnline();
    digitalWrite(PIN_STATUS_BLUE, fullyOnline ? HIGH : LOW);
    digitalWrite(PIN_STATUS_RED,
                 (!fullyOnline || errorLedActive) ? HIGH : LOW);
}

uint32_t generateBootId() {
    uint32_t value = micros();
    value ^= static_cast<uint32_t>(analogRead(PIN_LIGHT_SENSOR)) << 20;
    value ^= static_cast<uint32_t>(analogRead(A0)) << 8;
    value ^= micros() << 1;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value == 0 ? 0x4C554D41UL : value;
}

void applyDesiredState(const DesiredState &desired, uint32_t now) {
    if (desired.configVersion <= appState.configVersion) {
        if (!hasLoggedIgnoredDesiredVersion ||
            desired.configVersion != lastLoggedIgnoredDesiredVersion) {
            Serial.print(desired.configVersion == appState.configVersion
                             ? F("desired: unchanged config_version=")
                             : F("desired: ignored stale config_version="));
            Serial.println(desired.configVersion);
            hasLoggedIgnoredDesiredVersion = true;
            lastLoggedIgnoredDesiredVersion = desired.configVersion;
        }
        return;
    }

    const HomeMode previousMode = appState.mode;
    const bool previousLightOn = appState.actualLightOn;
    appState.manualLightOn = desired.manualLightOn;
    appState.configVersion = desired.configVersion;
    modeController.setMode(desired.mode, appState, lightController,
                           lightSensor.hasSample());
    markConfigDirty(now);
    recordObservableChanges(previousMode, previousLightOn, now);
    hasLoggedIgnoredDesiredVersion = false;

    Serial.print(F("desired: applied config_version="));
    Serial.print(appState.configVersion);
    Serial.print(F(" mode="));
    Serial.print(appState.mode == HomeMode::Manual ? F("manual") : F("night"));
    Serial.print(F(" manual_light_on="));
    Serial.println(appState.manualLightOn ? F("true") : F("false"));
}

void processEventsAck(const HeartbeatResult &heartbeat) {
    if (!heartbeat.hasEventsAck) {
        return;
    }

    if (heartbeat.sentEventCount == 0 ||
        heartbeat.eventsAckSeq > heartbeat.sentHighestSeq) {
        Serial.print(F("events: ack rejected seq="));
        Serial.println(heartbeat.eventsAckSeq);
        return;
    }

    const size_t removed =
        eventBuffer.acknowledgeThrough(heartbeat.eventsAckSeq);
    if (removed > 0) {
        Serial.print(F("events: acknowledged through seq="));
        Serial.print(heartbeat.eventsAckSeq);
        Serial.print(F(" remaining="));
        Serial.println(eventBuffer.count());
    }
}

void setup() {
    lightController.begin();

    pinMode(PIN_STATUS_RED, OUTPUT);
    pinMode(PIN_STATUS_BLUE, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    noTone(PIN_BUZZER);
    digitalWrite(PIN_BUZZER, LOW);

    tone(PIN_BUZZER, 2200, 70);
    delay(120);
    tone(PIN_BUZZER, 3000, 70);
    delay(70);
    noTone(PIN_BUZZER);
    digitalWrite(PIN_BUZZER, LOW);

    digitalWrite(PIN_STATUS_RED, HIGH);
    digitalWrite(PIN_STATUS_BLUE, LOW);

    Serial.begin(115200);
    bootId = generateBootId();

    const ConfigLoadResult loadResult = configStore.load(
        appState.mode, appState.manualLightOn, appState.configVersion);
    configValidAtBoot = loadResult != ConfigLoadResult::Invalid;
    restoredMode = appState.mode;
    restoredManualLightOn = appState.manualLightOn;
    restoredConfigVersion = appState.configVersion;
    if (loadResult == ConfigLoadResult::Valid) {
        Serial.println(F("config: restored"));
    } else if (loadResult == ConfigLoadResult::Migrated) {
        Serial.println(F("config: migrated v1, save pending"));
        markConfigDirty(millis());
    } else {
        Serial.println(F("config: invalid or missing, using defaults"));
        if (configStore.save(appState.mode, appState.manualLightOn,
                             appState.configVersion)) {
            Serial.println(F("config: saved"));
        } else {
            Serial.println(F("config: save failed"));
            configDirty = true;
            configDirtySince = millis();
            configSaveWaitMs = CONFIG_SAVE_RETRY_MS;
        }
    }

    lightSensor.begin(millis());
    modeController.setMode(appState.mode, appState, lightController, false);
    backendClient.begin();
    networkManager.begin(millis());

    Serial.println(F("LumaHome firmware ready"));
    Serial.print(F("boot_id="));
    Serial.println(bootId);
    Serial.println(F("Type 'help' for commands"));
    printStatus();
    updateStatusLeds();
}

void loop() {
    const uint32_t now = millis();

    if (lightSensor.update(now)) {
        const bool previousLightOn = appState.actualLightOn;
        appState.sensorRaw = lightSensor.raw();
        appState.sensorFiltered = lightSensor.filtered();
        modeController.evaluateNightMode(appState, lightController);
        recordObservableChanges(appState.mode, previousLightOn, now);
    }

    processSerial(now);
    updateErrorLed(now);
    updateConfigSave(now);

    const WifiTransition wifiTransition = networkManager.update(now);
    if (wifiTransition == WifiTransition::Connected) {
        appendEvent(DeviceEventType::WifiChanged, 1, millis());
        backendClient.onWifiConnected();
    } else if (wifiTransition == WifiTransition::Disconnected) {
        appendEvent(DeviceEventType::WifiChanged, 0, millis());
        backendClient.onWifiDisconnected();
    }

    const HeartbeatResult heartbeat = backendClient.update(
        millis(), networkManager.isConnected(), appState, bootId, eventBuffer);
    if (heartbeat.succeeded) {
        applyDesiredState(heartbeat.desired, millis());
        processEventsAck(heartbeat);
    }
    updateStatusLeds();
}
