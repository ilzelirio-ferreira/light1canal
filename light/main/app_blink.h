#pragma once
#include <stdint.h>
constexpr uint8_t APP_BLINK_MIN_LEVEL = 0;
constexpr uint8_t APP_BLINK_MAX_LEVEL = 254;
// Brightness is a frequency command, not a dimming/PWM duty cycle.
struct AppBlink {
    uint8_t level = APP_BLINK_MIN_LEVEL;
    bool phase = false;
    int64_t next_edge = 0;
    static constexpr uint8_t clamp(uint8_t value) {
        return value > APP_BLINK_MAX_LEVEL ? APP_BLINK_MAX_LEVEL : value;
    }
    static constexpr int64_t half_period(uint8_t value) {
        if (value == 0 || value >= APP_BLINK_MAX_LEVEL) return 0;
        const uint32_t milli_hz = 1000 + (9000u * (clamp(value) - 1)) / 252;
        return 500000000LL / milli_hz;
    }
    void power_changed(bool on) { level = on ? APP_BLINK_MAX_LEVEL : APP_BLINK_MIN_LEVEL; phase = on; next_edge = 0; }
    void brightness_changed(uint8_t value, bool on, int64_t now) {
        level = clamp(value); phase = on;
        next_edge = on && level > 0 && level < APP_BLINK_MAX_LEVEL ? now + half_period(level) : 0;
    }
    bool tick(bool on, int64_t now) {
        if (!on || level == 0 || level == APP_BLINK_MAX_LEVEL) { phase = on; next_edge = 0; return false; }
        if (!next_edge) next_edge = now + half_period(level);
        if (now < next_edge) return false;
        const int64_t half = half_period(level);
        const int64_t edges = 1 + (now - next_edge) / half;
        if (edges & 1) phase = !phase;
        next_edge += edges * half;
        return true;
    }
};
