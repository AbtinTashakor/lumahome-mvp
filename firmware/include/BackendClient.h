#pragma once

#include <Arduino.h>
#include <WiFiS3.h>

#include "AppState.h"
#include "EventBuffer.h"

constexpr char DEVICE_ID[] = "lumahome-01";
constexpr uint32_t DEFAULT_HEARTBEAT_INTERVAL_MS = 2000;
constexpr uint32_t MIN_HEARTBEAT_INTERVAL_MS = 1000;
constexpr uint32_t MAX_HEARTBEAT_INTERVAL_MS = 30000;
constexpr uint32_t HTTP_TRANSACTION_TIMEOUT_MS = 1500;
constexpr size_t MAX_HEARTBEAT_RESPONSE_BYTES = 768;
constexpr size_t MAX_HEARTBEAT_REQUEST_BYTES = 4096;

struct DesiredState {
    HomeMode mode;
    bool manualLightOn;
    uint32_t configVersion;
};

struct HeartbeatResult {
    bool attempted;
    bool succeeded;
    DesiredState desired;
    bool hasEventsAck;
    uint32_t eventsAckSeq;
    size_t sentEventCount;
    uint32_t sentHighestSeq;
};

class BackendClient {
public:
    void begin();
    HeartbeatResult update(uint32_t now, bool wifiConnected,
                           const AppState &state, uint32_t bootId,
                           const EventBuffer &events);
    void onWifiConnected();
    void onWifiDisconnected();
    void requestHeartbeat();

    bool isOnline() const;
    uint32_t heartbeatIntervalMs() const;
    bool hasSuccessfulHeartbeat() const;
    uint32_t lastSuccessfulHeartbeatAt() const;

private:
    bool performHeartbeat(const AppState &state, uint32_t bootId,
                          const EventBuffer &events,
                          HeartbeatResult &result);
    bool buildHeartbeatRequest(const AppState &state, uint32_t bootId,
                               const EventBuffer &events,
                               size_t &requestLength,
                               HeartbeatResult &result);
    bool exchangeHttp(const char *requestBody, size_t requestLength,
                      char *responseBody, size_t responseCapacity,
                      size_t &responseLength);
    bool readLine(char *buffer, size_t capacity, size_t &length,
                  uint32_t startedAt);
    void setOnline(bool online);

    WiFiClient client_;
    bool online_ = false;
    bool heartbeatRequested_ = false;
    bool hasAttempted_ = false;
    bool hasSuccessfulHeartbeat_ = false;
    uint32_t heartbeatIntervalMs_ = DEFAULT_HEARTBEAT_INTERVAL_MS;
    uint32_t lastAttemptCompletedAt_ = 0;
    uint32_t lastSuccessfulHeartbeatAt_ = 0;
    char requestBody_[MAX_HEARTBEAT_REQUEST_BYTES]{};
};
