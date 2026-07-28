#pragma once

#include <Arduino.h>
#include <WiFiS3.h>

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t WIFI_BEGIN_CALL_TIMEOUT_MS = 0;
constexpr uint32_t WIFI_RETRY_INITIAL_MS = 2000;
constexpr uint32_t WIFI_RETRY_MAX_MS = 30000;

enum class WifiTransition : uint8_t {
    None,
    Connected,
    Disconnected
};

class NetworkManager {
public:
    void begin(uint32_t now);
    WifiTransition update(uint32_t now);
    bool isConnected() const;
    IPAddress localIp() const;
    int32_t rssi() const;

private:
    void startAttempt();
    void scheduleRetry(uint32_t now);

    bool connected_ = false;
    bool attempting_ = false;
    uint32_t attemptStartedAt_ = 0;
    uint32_t retryScheduledAt_ = 0;
    uint32_t retryWaitMs_ = 0;
    uint32_t retryDelayMs_ = WIFI_RETRY_INITIAL_MS;
};
