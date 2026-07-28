#include "BackendClient.h"

#include <ArduinoJson.h>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "Secrets.h"

namespace {

constexpr size_t RESPONSE_JSON_CAPACITY = 384;
constexpr size_t HTTP_HEADER_CAPACITY = 256;
constexpr size_t HTTP_LINE_CAPACITY = 128;
constexpr size_t MAX_HTTP_HEADER_BYTES = 1024;

class FixedJsonWriter {
public:
    FixedJsonWriter(char *buffer, size_t capacity)
        : buffer_(buffer), capacity_(capacity) {
        if (capacity_ > 0) {
            buffer_[0] = '\0';
        }
    }

    bool append(const char *format, ...) {
        if (length_ >= capacity_) {
            return false;
        }

        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(buffer_ + length_,
                                           capacity_ - length_, format,
                                           arguments);
        va_end(arguments);
        if (written < 0 || static_cast<size_t>(written) >=
                               capacity_ - length_) {
            return false;
        }
        length_ += static_cast<size_t>(written);
        return true;
    }

    size_t length() const {
        return length_;
    }

private:
    char *buffer_;
    size_t capacity_;
    size_t length_ = 0;
};

bool startsWithIgnoreCase(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        if (*text == '\0' || std::tolower(static_cast<unsigned char>(*text)) !=
                                 std::tolower(static_cast<unsigned char>(*prefix))) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

bool parseContentLength(const char *value, size_t &contentLength) {
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    if (*value < '0' || *value > '9') {
        return false;
    }

    size_t parsed = 0;
    while (*value >= '0' && *value <= '9') {
        const uint8_t digit = static_cast<uint8_t>(*value - '0');
        if (parsed > (MAX_HEARTBEAT_RESPONSE_BYTES - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
        ++value;
    }

    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    if (*value != '\0' || parsed == 0) {
        return false;
    }

    contentLength = parsed;
    return true;
}

} // namespace

void BackendClient::begin() {
    client_.setConnectionTimeout(HTTP_TRANSACTION_TIMEOUT_MS);
    client_.setTimeout(HTTP_TRANSACTION_TIMEOUT_MS);
}

HeartbeatResult BackendClient::update(uint32_t now, bool wifiConnected,
                                      const AppState &state,
                                      uint32_t bootId,
                                      const EventBuffer &events) {
    HeartbeatResult result{false,
                           false,
                           {HomeMode::Manual, false, 0},
                           false,
                           0,
                           0,
                           0};
    if (!wifiConnected) {
        return result;
    }

    const bool intervalElapsed =
        !hasAttempted_ ||
        now - lastAttemptCompletedAt_ >= heartbeatIntervalMs_;
    if (!heartbeatRequested_ && !intervalElapsed) {
        return result;
    }

    heartbeatRequested_ = false;
    result.attempted = true;
    result.succeeded = performHeartbeat(state, bootId, events, result);
    hasAttempted_ = true;
    lastAttemptCompletedAt_ = millis();

    if (result.succeeded) {
        hasSuccessfulHeartbeat_ = true;
        lastSuccessfulHeartbeatAt_ = lastAttemptCompletedAt_;
    }
    setOnline(result.succeeded);
    return result;
}

void BackendClient::onWifiConnected() {
    heartbeatRequested_ = true;
}

void BackendClient::onWifiDisconnected() {
    heartbeatRequested_ = true;
    setOnline(false);
}

void BackendClient::requestHeartbeat() {
    heartbeatRequested_ = true;
}

bool BackendClient::isOnline() const {
    return online_;
}

uint32_t BackendClient::heartbeatIntervalMs() const {
    return heartbeatIntervalMs_;
}

bool BackendClient::hasSuccessfulHeartbeat() const {
    return hasSuccessfulHeartbeat_;
}

uint32_t BackendClient::lastSuccessfulHeartbeatAt() const {
    return lastSuccessfulHeartbeatAt_;
}

bool BackendClient::performHeartbeat(const AppState &state, uint32_t bootId,
                                     const EventBuffer &events,
                                     HeartbeatResult &result) {
    size_t requestLength = 0;
    if (!buildHeartbeatRequest(state, bootId, events, requestLength, result)) {
        return false;
    }

    char responseBody[MAX_HEARTBEAT_RESPONSE_BYTES + 1]{};
    size_t responseLength = 0;
    if (!exchangeHttp(requestBody_, requestLength, responseBody,
                      sizeof(responseBody), responseLength)) {
        return false;
    }

    StaticJsonDocument<RESPONSE_JSON_CAPACITY> response;
    const DeserializationError error =
        deserializeJson(response, responseBody, responseLength);
    if (error) {
        return false;
    }

    const JsonVariantConst desiredValue = response["desired"];
    if (!desiredValue.is<JsonObjectConst>()) {
        return false;
    }

    const JsonObjectConst desiredObject = desiredValue.as<JsonObjectConst>();
    const JsonVariantConst modeValue = desiredObject["mode"];
    const JsonVariantConst manualValue = desiredObject["manual_light_on"];
    const JsonVariantConst versionValue = desiredObject["config_version"];
    if (!modeValue.is<const char *>() || !manualValue.is<bool>() ||
        !versionValue.is<uint32_t>()) {
        return false;
    }

    const char *mode = modeValue.as<const char *>();
    HomeMode parsedMode;
    if (std::strcmp(mode, "manual") == 0) {
        parsedMode = HomeMode::Manual;
    } else if (std::strcmp(mode, "night") == 0) {
        parsedMode = HomeMode::Night;
    } else {
        return false;
    }

    result.desired.mode = parsedMode;
    result.desired.manualLightOn = manualValue.as<bool>();
    result.desired.configVersion = versionValue.as<uint32_t>();

    const JsonVariantConst ackValue = response["events_ack_seq"];
    if (!ackValue.isNull()) {
        if (!ackValue.is<uint32_t>()) {
            return false;
        }
        result.hasEventsAck = true;
        result.eventsAckSeq = ackValue.as<uint32_t>();
    }

    const JsonVariantConst intervalValue = response["heartbeat_interval_ms"];
    if (!intervalValue.isNull() && intervalValue.is<uint32_t>()) {
        const uint32_t interval = intervalValue.as<uint32_t>();
        if (interval >= MIN_HEARTBEAT_INTERVAL_MS &&
            interval <= MAX_HEARTBEAT_INTERVAL_MS) {
            heartbeatIntervalMs_ = interval;
        }
    }

    return true;
}

bool BackendClient::buildHeartbeatRequest(const AppState &state,
                                          uint32_t bootId,
                                          const EventBuffer &events,
                                          size_t &requestLength,
                                          HeartbeatResult &result) {
    FixedJsonWriter writer(requestBody_, sizeof(requestBody_));
    if (!writer.append(
            "{\"device_id\":\"%s\",\"boot_id\":%lu,\"mode\":\"%s\","
            "\"light_on\":%s,\"sensor_raw\":%u,\"config_version\":%lu,"
            "\"events\":[",
            DEVICE_ID, static_cast<unsigned long>(bootId),
            state.mode == HomeMode::Manual ? "manual" : "night",
            state.actualLightOn ? "true" : "false",
            static_cast<unsigned int>(state.sensorRaw),
            static_cast<unsigned long>(state.configVersion))) {
        return false;
    }

    const size_t eventCount =
        events.count() < MAX_EVENTS_PER_HEARTBEAT
            ? events.count()
            : MAX_EVENTS_PER_HEARTBEAT;
    for (size_t index = 0; index < eventCount; ++index) {
        DeviceEvent event{};
        if (!events.get(index, event)) {
            return false;
        }

        if (index > 0 && !writer.append(",")) {
            return false;
        }

        const unsigned long seq = static_cast<unsigned long>(event.seq);
        const unsigned long uptime =
            static_cast<unsigned long>(event.uptimeMs);
        bool serialized = false;
        if (event.type == DeviceEventType::ModeChanged) {
            serialized = writer.append(
                "{\"seq\":%lu,\"uptime_ms\":%lu,\"type\":"
                "\"mode_changed\",\"mode\":\"%s\"}",
                seq, uptime,
                event.value == static_cast<uint8_t>(HomeMode::Manual)
                    ? "manual"
                    : "night");
        } else if (event.type == DeviceEventType::LightChanged) {
            serialized = writer.append(
                "{\"seq\":%lu,\"uptime_ms\":%lu,\"type\":"
                "\"light_changed\",\"light_on\":%s}",
                seq, uptime, event.value == 0 ? "false" : "true");
        } else if (event.type == DeviceEventType::WifiChanged) {
            serialized = writer.append(
                "{\"seq\":%lu,\"uptime_ms\":%lu,\"type\":"
                "\"wifi_changed\",\"connected\":%s}",
                seq, uptime, event.value == 0 ? "false" : "true");
        }
        if (!serialized) {
            return false;
        }

        result.sentHighestSeq = event.seq;
        ++result.sentEventCount;
    }

    if (!writer.append("]}")) {
        return false;
    }
    requestLength = writer.length();
    return requestLength > 0;
}

bool BackendClient::exchangeHttp(const char *requestBody,
                                 size_t requestLength, char *responseBody,
                                 size_t responseCapacity,
                                 size_t &responseLength) {
    client_.stop();
    const uint32_t startedAt = millis();
    if (!client_.connect(BACKEND_HOST, BACKEND_PORT)) {
        client_.stop();
        return false;
    }

    char headers[HTTP_HEADER_CAPACITY]{};
    const int headerLength = std::snprintf(
        headers, sizeof(headers),
        "POST /api/device/heartbeat HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        BACKEND_HOST, static_cast<unsigned int>(BACKEND_PORT),
        static_cast<unsigned int>(requestLength));
    if (headerLength <= 0 ||
        static_cast<size_t>(headerLength) >= sizeof(headers) ||
        client_.write(reinterpret_cast<const uint8_t *>(headers),
                      static_cast<size_t>(headerLength)) !=
            static_cast<size_t>(headerLength) ||
        client_.write(reinterpret_cast<const uint8_t *>(requestBody),
                      requestLength) != requestLength) {
        client_.stop();
        return false;
    }

    char line[HTTP_LINE_CAPACITY]{};
    size_t lineLength = 0;
    if (!readLine(line, sizeof(line), lineLength, startedAt)) {
        client_.stop();
        return false;
    }

    const char *statusSeparator = std::strchr(line, ' ');
    if (!startsWithIgnoreCase(line, "HTTP/") || statusSeparator == nullptr ||
        statusSeparator[1] < '0' || statusSeparator[1] > '9' ||
        statusSeparator[2] < '0' || statusSeparator[2] > '9' ||
        statusSeparator[3] < '0' || statusSeparator[3] > '9') {
        client_.stop();
        return false;
    }
    const uint16_t statusCode =
        static_cast<uint16_t>((statusSeparator[1] - '0') * 100 +
                              (statusSeparator[2] - '0') * 10 +
                              (statusSeparator[3] - '0'));
    if (statusCode < 200 || statusCode >= 300) {
        client_.stop();
        return false;
    }

    bool contentLengthFound = false;
    bool headersComplete = false;
    size_t contentLength = 0;
    size_t headerBytes = lineLength + 2;
    while (!headersComplete && headerBytes <= MAX_HTTP_HEADER_BYTES) {
        if (!readLine(line, sizeof(line), lineLength, startedAt)) {
            client_.stop();
            return false;
        }
        headerBytes += lineLength + 2;
        if (headerBytes > MAX_HTTP_HEADER_BYTES) {
            client_.stop();
            return false;
        }
        if (lineLength == 0) {
            headersComplete = true;
            continue;
        }
        if (startsWithIgnoreCase(line, "Content-Length:")) {
            if (!parseContentLength(line + 15, contentLength)) {
                client_.stop();
                return false;
            }
            contentLengthFound = true;
        }
    }

    if (!headersComplete || !contentLengthFound ||
        contentLength >= responseCapacity) {
        client_.stop();
        return false;
    }

    responseLength = 0;
    while (responseLength < contentLength &&
           millis() - startedAt < HTTP_TRANSACTION_TIMEOUT_MS) {
        if (client_.available() > 0) {
            const int value = client_.read();
            if (value >= 0) {
                responseBody[responseLength++] = static_cast<char>(value);
            }
        } else if (!client_.connected()) {
            break;
        }
    }
    client_.stop();

    if (responseLength != contentLength) {
        return false;
    }
    responseBody[responseLength] = '\0';
    return true;
}

bool BackendClient::readLine(char *buffer, size_t capacity, size_t &length,
                             uint32_t startedAt) {
    length = 0;
    while (millis() - startedAt < HTTP_TRANSACTION_TIMEOUT_MS) {
        if (client_.available() > 0) {
            const int value = client_.read();
            if (value < 0) {
                continue;
            }
            const char character = static_cast<char>(value);
            if (character == '\n') {
                buffer[length] = '\0';
                return true;
            }
            if (character != '\r') {
                if (length >= capacity - 1) {
                    return false;
                }
                buffer[length++] = character;
            }
        } else if (!client_.connected()) {
            return false;
        }
    }
    return false;
}

void BackendClient::setOnline(bool online) {
    if (online == online_) {
        return;
    }
    online_ = online;
    Serial.println(online ? F("backend: online") : F("backend: offline"));
}
