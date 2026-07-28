#include <unity.h>

#include <stdint.h>

#include "EventBuffer.h"

namespace {

DeviceEvent readEvent(const EventBuffer &buffer, size_t index) {
    DeviceEvent event{};
    TEST_ASSERT_TRUE(buffer.get(index, event));
    return event;
}

void assertEvent(const DeviceEvent &event, uint32_t expectedSequence,
                 uint32_t expectedUptime, DeviceEventType expectedType,
                 uint8_t expectedValue) {
    TEST_ASSERT_EQUAL_UINT32(expectedSequence, event.seq);
    TEST_ASSERT_EQUAL_UINT32(expectedUptime, event.uptimeMs);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expectedType),
                           static_cast<uint8_t>(event.type));
    TEST_ASSERT_EQUAL_UINT8(expectedValue, event.value);
}

void assertEventAt(const EventBuffer &buffer, size_t index,
                   uint32_t expectedSequence, uint32_t expectedUptime,
                   DeviceEventType expectedType, uint8_t expectedValue) {
    assertEvent(readEvent(buffer, index), expectedSequence, expectedUptime,
                expectedType, expectedValue);
}

DeviceEventType typeForIndex(size_t index) {
    switch (index % 3) {
    case 0:
        return DeviceEventType::ModeChanged;
    case 1:
        return DeviceEventType::LightChanged;
    default:
        return DeviceEventType::WifiChanged;
    }
}

uint8_t valueForIndex(size_t index) {
    return static_cast<uint8_t>(index % 2);
}

void appendIndexedEvents(EventBuffer &buffer, size_t count,
                         uint32_t uptimeBase = 1000) {
    for (size_t index = 0; index < count; ++index) {
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(EventAppendResult::Appended),
            static_cast<uint8_t>(
                buffer.append(typeForIndex(index), valueForIndex(index),
                              uptimeBase + static_cast<uint32_t>(index))));
    }
}

void assertSequenceRange(const EventBuffer &buffer, uint32_t firstSequence,
                         size_t count) {
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(count),
                             static_cast<uint32_t>(buffer.count()));
    for (size_t index = 0; index < count; ++index) {
        const DeviceEvent event = readEvent(buffer, index);
        TEST_ASSERT_EQUAL_UINT32(firstSequence + static_cast<uint32_t>(index),
                                 event.seq);
    }
}

void test_new_buffer_has_empty_consistent_state() {
    EventBuffer buffer;

    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(buffer.count()));
    TEST_ASSERT_EQUAL_UINT32(0, buffer.droppedCount());
    TEST_ASSERT_EQUAL_UINT32(1, buffer.nextSequence());
    TEST_ASSERT_TRUE(buffer.sequenceAvailable());
    TEST_ASSERT_FALSE(buffer.hasLastAcknowledgedSequence());
}

void test_empty_buffer_has_no_oldest_or_newest_sequence() {
    EventBuffer buffer;
    uint32_t oldest = 41;
    uint32_t newest = 73;

    TEST_ASSERT_FALSE(buffer.oldestSequence(oldest));
    TEST_ASSERT_FALSE(buffer.newestSequence(newest));
    TEST_ASSERT_EQUAL_UINT32(41, oldest);
    TEST_ASSERT_EQUAL_UINT32(73, newest);
}

void test_get_rejects_empty_and_out_of_range_indexes() {
    EventBuffer buffer;
    DeviceEvent untouched{55, 89, DeviceEventType::WifiChanged, 1};

    TEST_ASSERT_FALSE(buffer.get(0, untouched));
    assertEvent(untouched, 55, 89, DeviceEventType::WifiChanged, 1);

    buffer.append(DeviceEventType::ModeChanged, 0, 100);
    TEST_ASSERT_FALSE(buffer.get(1, untouched));
    assertEvent(untouched, 55, 89, DeviceEventType::WifiChanged, 1);
}

void test_first_append_assigns_sequence_one() {
    EventBuffer buffer;

    const EventAppendResult result =
        buffer.append(DeviceEventType::ModeChanged, 1, 250);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EventAppendResult::Appended),
                           static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(buffer.count()));
    TEST_ASSERT_EQUAL_UINT32(2, buffer.nextSequence());
    assertEventAt(buffer, 0, 1, 250, DeviceEventType::ModeChanged, 1);
}

void test_append_preserves_each_event_variant_and_value() {
    EventBuffer buffer;

    buffer.append(DeviceEventType::ModeChanged, 0, 100);
    buffer.append(DeviceEventType::LightChanged, 1, 200);
    buffer.append(DeviceEventType::WifiChanged, 0, 300);

    assertEventAt(buffer, 0, 1, 100, DeviceEventType::ModeChanged, 0);
    assertEventAt(buffer, 1, 2, 200, DeviceEventType::LightChanged, 1);
    assertEventAt(buffer, 2, 3, 300, DeviceEventType::WifiChanged, 0);
}

void test_append_preserves_uptime_boundaries() {
    EventBuffer buffer;

    buffer.append(DeviceEventType::LightChanged, 0, 0);
    buffer.append(DeviceEventType::LightChanged, 1, UINT32_MAX);

    assertEventAt(buffer, 0, 1, 0, DeviceEventType::LightChanged, 0);
    assertEventAt(buffer, 1, 2, UINT32_MAX, DeviceEventType::LightChanged, 1);
}

void test_sequences_increase_in_insertion_order() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 12);

    assertSequenceRange(buffer, 1, 12);
    TEST_ASSERT_EQUAL_UINT32(13, buffer.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(0, buffer.droppedCount());
}

void test_oldest_and_newest_follow_appends() {
    EventBuffer buffer;
    uint32_t oldest = 0;
    uint32_t newest = 0;

    buffer.append(DeviceEventType::ModeChanged, 0, 10);
    TEST_ASSERT_TRUE(buffer.oldestSequence(oldest));
    TEST_ASSERT_TRUE(buffer.newestSequence(newest));
    TEST_ASSERT_EQUAL_UINT32(1, oldest);
    TEST_ASSERT_EQUAL_UINT32(1, newest);

    appendIndexedEvents(buffer, 4, 20);
    TEST_ASSERT_TRUE(buffer.oldestSequence(oldest));
    TEST_ASSERT_TRUE(buffer.newestSequence(newest));
    TEST_ASSERT_EQUAL_UINT32(1, oldest);
    TEST_ASSERT_EQUAL_UINT32(5, newest);
}

void test_acknowledging_empty_buffer_records_progress() {
    EventBuffer buffer;

    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(buffer.acknowledgeThrough(8)));
    TEST_ASSERT_TRUE(buffer.hasLastAcknowledgedSequence());
    TEST_ASSERT_EQUAL_UINT32(8, buffer.lastAcknowledgedSequence());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(buffer.count()));
}

void test_ack_before_oldest_removes_nothing() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 4);

    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(buffer.acknowledgeThrough(0)));
    assertSequenceRange(buffer, 1, 4);
    TEST_ASSERT_TRUE(buffer.hasLastAcknowledgedSequence());
    TEST_ASSERT_EQUAL_UINT32(0, buffer.lastAcknowledgedSequence());
}

void test_partial_ack_removes_only_leading_events() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 8);

    TEST_ASSERT_EQUAL_UINT32(
        3, static_cast<uint32_t>(buffer.acknowledgeThrough(3)));

    assertSequenceRange(buffer, 4, 5);
    TEST_ASSERT_EQUAL_UINT32(3, buffer.lastAcknowledgedSequence());
    TEST_ASSERT_EQUAL_UINT32(9, buffer.nextSequence());
}

void test_exact_ack_empties_the_buffer() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 8);

    TEST_ASSERT_EQUAL_UINT32(
        8, static_cast<uint32_t>(buffer.acknowledgeThrough(8)));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(buffer.count()));
    TEST_ASSERT_EQUAL_UINT32(8, buffer.lastAcknowledgedSequence());
    TEST_ASSERT_EQUAL_UINT32(9, buffer.nextSequence());
}

void test_stale_ack_does_not_reduce_recorded_progress() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 6);
    buffer.acknowledgeThrough(4);

    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(buffer.acknowledgeThrough(2)));
    TEST_ASSERT_EQUAL_UINT32(4, buffer.lastAcknowledgedSequence());
    assertSequenceRange(buffer, 5, 2);
}

void test_duplicate_ack_is_idempotent() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 6);

    TEST_ASSERT_EQUAL_UINT32(
        3, static_cast<uint32_t>(buffer.acknowledgeThrough(3)));
    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(buffer.acknowledgeThrough(3)));
    TEST_ASSERT_EQUAL_UINT32(3, buffer.lastAcknowledgedSequence());
    assertSequenceRange(buffer, 4, 3);
}

void test_append_after_ack_continues_sequence_numbers() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 5);
    buffer.acknowledgeThrough(5);

    buffer.append(DeviceEventType::WifiChanged, 1, 9000);

    TEST_ASSERT_EQUAL_UINT32(7, buffer.nextSequence());
    assertEventAt(buffer, 0, 6, 9000, DeviceEventType::WifiChanged, 1);
}

void test_buffer_accepts_exactly_its_capacity_without_drops() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY);
    uint32_t oldest = 0;
    uint32_t newest = 0;

    TEST_ASSERT_EQUAL_UINT32(
        static_cast<uint32_t>(EVENT_BUFFER_CAPACITY),
        static_cast<uint32_t>(buffer.count()));
    TEST_ASSERT_EQUAL_UINT32(0, buffer.droppedCount());
    TEST_ASSERT_TRUE(buffer.oldestSequence(oldest));
    TEST_ASSERT_TRUE(buffer.newestSequence(newest));
    TEST_ASSERT_EQUAL_UINT32(1, oldest);
    TEST_ASSERT_EQUAL_UINT32(
        static_cast<uint32_t>(EVENT_BUFFER_CAPACITY), newest);
}

void test_append_to_full_buffer_drops_only_oldest() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY);

    const EventAppendResult result =
        buffer.append(DeviceEventType::WifiChanged, 1, 5000);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(EventAppendResult::OldestDropped),
        static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_UINT32(
        static_cast<uint32_t>(EVENT_BUFFER_CAPACITY),
        static_cast<uint32_t>(buffer.count()));
    TEST_ASSERT_EQUAL_UINT32(1, buffer.droppedCount());
    assertSequenceRange(buffer, 2, EVENT_BUFFER_CAPACITY);
    assertEventAt(buffer, EVENT_BUFFER_CAPACITY - 1,
                  static_cast<uint32_t>(EVENT_BUFFER_CAPACITY + 1), 5000,
                  DeviceEventType::WifiChanged, 1);
}

void test_repeated_overflow_tracks_every_drop() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY);

    for (size_t index = 0; index < 9; ++index) {
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(EventAppendResult::OldestDropped),
            static_cast<uint8_t>(
                buffer.append(DeviceEventType::LightChanged,
                              valueForIndex(index),
                              9000 + static_cast<uint32_t>(index))));
    }

    TEST_ASSERT_EQUAL_UINT32(9, buffer.droppedCount());
    assertSequenceRange(buffer, 10, EVENT_BUFFER_CAPACITY);
}

void test_ack_after_overflow_uses_surviving_sequence_range() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY);
    for (size_t index = 0; index < 5; ++index) {
        buffer.append(DeviceEventType::ModeChanged, valueForIndex(index),
                      8000 + static_cast<uint32_t>(index));
    }

    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(buffer.acknowledgeThrough(5)));
    TEST_ASSERT_EQUAL_UINT32(
        5, static_cast<uint32_t>(buffer.acknowledgeThrough(10)));
    assertSequenceRange(buffer, 11, EVENT_BUFFER_CAPACITY - 5);
    TEST_ASSERT_EQUAL_UINT32(10, buffer.lastAcknowledgedSequence());
}

void test_ack_then_append_wraps_storage_without_reordering() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY);
    buffer.acknowledgeThrough(40);

    for (size_t index = 0; index < 30; ++index) {
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(EventAppendResult::Appended),
            static_cast<uint8_t>(
                buffer.append(DeviceEventType::WifiChanged,
                              valueForIndex(index),
                              10000 + static_cast<uint32_t>(index))));
    }

    assertSequenceRange(buffer, 41, 54);
    TEST_ASSERT_EQUAL_UINT32(0, buffer.droppedCount());
}

void test_wrapped_storage_supports_partial_ack() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY);
    buffer.acknowledgeThrough(48);
    appendIndexedEvents(buffer, 32, 12000);

    TEST_ASSERT_EQUAL_UINT32(
        24, static_cast<uint32_t>(buffer.acknowledgeThrough(72)));
    assertSequenceRange(buffer, 73, 24);
    TEST_ASSERT_EQUAL_UINT32(72, buffer.lastAcknowledgedSequence());
}

void test_refill_after_empty_uses_all_slots_again() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY);
    buffer.acknowledgeThrough(
        static_cast<uint32_t>(EVENT_BUFFER_CAPACITY));
    appendIndexedEvents(buffer, EVENT_BUFFER_CAPACITY, 15000);

    TEST_ASSERT_EQUAL_UINT32(0, buffer.droppedCount());
    assertSequenceRange(
        buffer, static_cast<uint32_t>(EVENT_BUFFER_CAPACITY + 1),
        EVENT_BUFFER_CAPACITY);
}

void test_multiple_drain_and_refill_cycles_preserve_order() {
    EventBuffer buffer;
    uint32_t expectedFirst = 1;

    for (size_t cycle = 0; cycle < 6; ++cycle) {
        appendIndexedEvents(buffer, 10,
                            20000 + static_cast<uint32_t>(cycle * 100));
        assertSequenceRange(buffer, expectedFirst, 10);
        const uint32_t newest = expectedFirst + 9;
        TEST_ASSERT_EQUAL_UINT32(
            10, static_cast<uint32_t>(buffer.acknowledgeThrough(newest)));
        expectedFirst = newest + 1;
    }

    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(buffer.count()));
    TEST_ASSERT_EQUAL_UINT32(61, buffer.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(60, buffer.lastAcknowledgedSequence());
}

void test_batch_limit_is_smaller_than_buffer_capacity() {
    TEST_ASSERT_EQUAL_UINT32(64,
                             static_cast<uint32_t>(EVENT_BUFFER_CAPACITY));
    TEST_ASSERT_EQUAL_UINT32(
        32, static_cast<uint32_t>(MAX_EVENTS_PER_HEARTBEAT));
    TEST_ASSERT_TRUE(MAX_EVENTS_PER_HEARTBEAT < EVENT_BUFFER_CAPACITY);
    TEST_ASSERT_EQUAL_UINT32(
        static_cast<uint32_t>(EVENT_BUFFER_CAPACITY),
        static_cast<uint32_t>(MAX_EVENTS_PER_HEARTBEAT * 2));
}

void test_acknowledgement_does_not_reuse_sequences() {
    EventBuffer buffer;
    appendIndexedEvents(buffer, 16);
    buffer.acknowledgeThrough(8);
    appendIndexedEvents(buffer, 8, 21000);
    buffer.acknowledgeThrough(20);
    buffer.append(DeviceEventType::LightChanged, 1, 22000);

    assertSequenceRange(buffer, 21, 5);
    TEST_ASSERT_EQUAL_UINT32(26, buffer.nextSequence());
}

} // namespace

void setUp() {
}

void tearDown() {
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_new_buffer_has_empty_consistent_state);
    RUN_TEST(test_empty_buffer_has_no_oldest_or_newest_sequence);
    RUN_TEST(test_get_rejects_empty_and_out_of_range_indexes);
    RUN_TEST(test_first_append_assigns_sequence_one);
    RUN_TEST(test_append_preserves_each_event_variant_and_value);
    RUN_TEST(test_append_preserves_uptime_boundaries);
    RUN_TEST(test_sequences_increase_in_insertion_order);
    RUN_TEST(test_oldest_and_newest_follow_appends);
    RUN_TEST(test_acknowledging_empty_buffer_records_progress);
    RUN_TEST(test_ack_before_oldest_removes_nothing);
    RUN_TEST(test_partial_ack_removes_only_leading_events);
    RUN_TEST(test_exact_ack_empties_the_buffer);
    RUN_TEST(test_stale_ack_does_not_reduce_recorded_progress);
    RUN_TEST(test_duplicate_ack_is_idempotent);
    RUN_TEST(test_append_after_ack_continues_sequence_numbers);
    RUN_TEST(test_buffer_accepts_exactly_its_capacity_without_drops);
    RUN_TEST(test_append_to_full_buffer_drops_only_oldest);
    RUN_TEST(test_repeated_overflow_tracks_every_drop);
    RUN_TEST(test_ack_after_overflow_uses_surviving_sequence_range);
    RUN_TEST(test_ack_then_append_wraps_storage_without_reordering);
    RUN_TEST(test_wrapped_storage_supports_partial_ack);
    RUN_TEST(test_refill_after_empty_uses_all_slots_again);
    RUN_TEST(test_multiple_drain_and_refill_cycles_preserve_order);
    RUN_TEST(test_batch_limit_is_smaller_than_buffer_capacity);
    RUN_TEST(test_acknowledgement_does_not_reuse_sequences);
    return UNITY_END();
}
