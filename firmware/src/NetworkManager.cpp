#include "NetworkManager.h"

#include "Secrets.h"

void NetworkManager::begin(uint32_t now) {
    WiFi.setTimeout(WIFI_BEGIN_CALL_TIMEOUT_MS);
    retryScheduledAt_ = now;
    retryWaitMs_ = 0;
}

WifiTransition NetworkManager::update(uint32_t now) {
    const bool currentlyConnected = WiFi.status() == WL_CONNECTED;

    if (currentlyConnected) {
        attempting_ = false;
        retryDelayMs_ = WIFI_RETRY_INITIAL_MS;
        if (!connected_) {
            connected_ = true;
            Serial.print(F("wifi: connected ip="));
            Serial.println(WiFi.localIP());
            return WifiTransition::Connected;
        }
        return WifiTransition::None;
    }

    if (connected_) {
        connected_ = false;
        attempting_ = false;
        Serial.println(F("wifi: disconnected"));
        scheduleRetry(now);
        return WifiTransition::Disconnected;
    }

    if (attempting_) {
        if (now - attemptStartedAt_ >= WIFI_CONNECT_TIMEOUT_MS) {
            WiFi.disconnect();
            attempting_ = false;
            scheduleRetry(millis());
        }
        return WifiTransition::None;
    }

    if (now - retryScheduledAt_ >= retryWaitMs_) {
        startAttempt();
        if (WiFi.status() == WL_CONNECTED) {
            connected_ = true;
            attempting_ = false;
            retryDelayMs_ = WIFI_RETRY_INITIAL_MS;
            Serial.print(F("wifi: connected ip="));
            Serial.println(WiFi.localIP());
            return WifiTransition::Connected;
        }
    }

    return WifiTransition::None;
}

bool NetworkManager::isConnected() const {
    return connected_;
}

IPAddress NetworkManager::localIp() const {
    return connected_ ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
}

int32_t NetworkManager::rssi() const {
    return connected_ ? WiFi.RSSI() : 0;
}

void NetworkManager::startAttempt() {
    Serial.println(F("wifi: connecting"));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    attemptStartedAt_ = millis();
    attempting_ = true;
}

void NetworkManager::scheduleRetry(uint32_t now) {
    retryScheduledAt_ = now;
    retryWaitMs_ = retryDelayMs_;
    Serial.print(F("wifi: retry in "));
    Serial.print(retryWaitMs_);
    Serial.println(F(" ms"));

    if (retryDelayMs_ < WIFI_RETRY_MAX_MS) {
        const uint32_t doubled = retryDelayMs_ * 2;
        retryDelayMs_ =
            doubled > WIFI_RETRY_MAX_MS ? WIFI_RETRY_MAX_MS : doubled;
    }
}
