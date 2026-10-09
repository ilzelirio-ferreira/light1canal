#pragma once
#include <stdint.h>
constexpr unsigned APP_INPUT_CHANNEL_COUNT = 1;
constexpr uint8_t APP_DEFAULT_INPUTS[APP_INPUT_CHANNEL_COUNT] = {33};
constexpr uint8_t APP_OUTPUT_PINS[APP_INPUT_CHANNEL_COUNT] = {35};
constexpr bool app_input_map_valid(const uint8_t *pins) {
    return pins && pins[0] == APP_DEFAULT_INPUTS[0];
}
