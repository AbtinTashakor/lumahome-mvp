#pragma once

#include <Arduino.h>

constexpr uint8_t PIN_STATUS_RED = 12;
constexpr uint8_t PIN_STATUS_BLUE = 13;

constexpr uint8_t PIN_RGB_RED = 9;
constexpr uint8_t PIN_RGB_BLUE = 10;
constexpr uint8_t PIN_RGB_GREEN = 11;

constexpr uint8_t PIN_LIGHT_SENSOR = A1;
constexpr uint8_t PIN_BUZZER = 5;

constexpr uint16_t NIGHT_ON_THRESHOLD = 250;
constexpr uint16_t NIGHT_OFF_THRESHOLD = 350;

constexpr size_t LIGHT_SAMPLE_COUNT = 8;
constexpr uint32_t LIGHT_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t ERROR_LED_DURATION_MS = 300;

constexpr size_t SERIAL_COMMAND_CAPACITY = 64;
