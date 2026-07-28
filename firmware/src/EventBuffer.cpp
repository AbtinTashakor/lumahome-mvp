#include "EventBuffer.h"

EventAppendResult EventBuffer::append(DeviceEventType type, uint8_t value,
                                      uint32_t uptimeMs) {
    if (!sequenceAvailable_) {
        ++droppedCount_;
        return EventAppendResult::SequenceExhausted;
    }

    bool droppedOldest = false;
    if (count_ == EVENT_BUFFER_CAPACITY) {
        head_ = (head_ + 1) % EVENT_BUFFER_CAPACITY;
        --count_;
        ++droppedCount_;
        droppedOldest = true;
    }

    const size_t tail = (head_ + count_) % EVENT_BUFFER_CAPACITY;
    events_[tail] = DeviceEvent{nextSequence_, uptimeMs, type, value};
    ++count_;

    if (nextSequence_ == UINT32_MAX) {
        sequenceAvailable_ = false;
    } else {
        ++nextSequence_;
    }

    return droppedOldest ? EventAppendResult::OldestDropped
                         : EventAppendResult::Appended;
}

bool EventBuffer::get(size_t index, DeviceEvent &event) const {
    if (index >= count_) {
        return false;
    }
    event = events_[(head_ + index) % EVENT_BUFFER_CAPACITY];
    return true;
}

size_t EventBuffer::acknowledgeThrough(uint32_t seq) {
    size_t removed = 0;
    while (count_ > 0 && events_[head_].seq <= seq) {
        head_ = (head_ + 1) % EVENT_BUFFER_CAPACITY;
        --count_;
        ++removed;
    }

    if (!hasLastAcknowledgedSequence_ ||
        seq > lastAcknowledgedSequence_) {
        lastAcknowledgedSequence_ = seq;
        hasLastAcknowledgedSequence_ = true;
    }
    return removed;
}

size_t EventBuffer::count() const {
    return count_;
}

uint32_t EventBuffer::droppedCount() const {
    return droppedCount_;
}

uint32_t EventBuffer::nextSequence() const {
    return nextSequence_;
}

bool EventBuffer::sequenceAvailable() const {
    return sequenceAvailable_;
}

bool EventBuffer::oldestSequence(uint32_t &seq) const {
    if (count_ == 0) {
        return false;
    }
    seq = events_[head_].seq;
    return true;
}

bool EventBuffer::newestSequence(uint32_t &seq) const {
    if (count_ == 0) {
        return false;
    }
    seq = events_[(head_ + count_ - 1) % EVENT_BUFFER_CAPACITY].seq;
    return true;
}

bool EventBuffer::hasLastAcknowledgedSequence() const {
    return hasLastAcknowledgedSequence_;
}

uint32_t EventBuffer::lastAcknowledgedSequence() const {
    return lastAcknowledgedSequence_;
}
