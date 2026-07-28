#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t EVENT_BUFFER_CAPACITY = 64;
constexpr size_t MAX_EVENTS_PER_HEARTBEAT = 32;

enum class DeviceEventType : uint8_t {
    ModeChanged,
    LightChanged,
    WifiChanged
};

struct DeviceEvent {
    uint32_t seq;
    uint32_t uptimeMs;
    DeviceEventType type;
    uint8_t value;
};

static_assert(sizeof(DeviceEvent) == 12, "Device event layout changed");

enum class EventAppendResult : uint8_t {
    Appended,
    OldestDropped,
    SequenceExhausted
};

class EventBuffer {
public:
    EventAppendResult append(DeviceEventType type, uint8_t value,
                             uint32_t uptimeMs);
    bool get(size_t index, DeviceEvent &event) const;
    size_t acknowledgeThrough(uint32_t seq);

    size_t count() const;
    uint32_t droppedCount() const;
    uint32_t nextSequence() const;
    bool sequenceAvailable() const;
    bool oldestSequence(uint32_t &seq) const;
    bool newestSequence(uint32_t &seq) const;
    bool hasLastAcknowledgedSequence() const;
    uint32_t lastAcknowledgedSequence() const;

private:
    DeviceEvent events_[EVENT_BUFFER_CAPACITY]{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t nextSequence_ = 1;
    uint32_t droppedCount_ = 0;
    uint32_t lastAcknowledgedSequence_ = 0;
    bool hasLastAcknowledgedSequence_ = false;
    bool sequenceAvailable_ = true;
};
