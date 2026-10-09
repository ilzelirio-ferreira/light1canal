#pragma once
#include <stdint.h>

// Half-wave PC817: one pulse per 60 Hz cycle. No platform dependencies.
struct AcPulseDetector {
    static constexpr int64_t absence_us = 500000;
    int64_t edge_us = 0;
    int64_t valid_us = 0;
    unsigned consecutive = 0;
    bool seen = false;
    bool qualified = false;

    constexpr void edge(int64_t now) {
        const int64_t period = now - edge_us;
        // Ignore short contact/noise edges without moving the cycle reference.
        if (seen && period < 2000) return;
        // A real pulse gap ends qualification; later activity must qualify again.
        if (seen && period >= absence_us) {
            qualified = false;
            consecutive = 0;
        }
        bool valid = false;
        // Accept up to two missing cycles, but do not accept steady 50/120 Hz.
        for (unsigned cycles = 1; cycles <= 3; ++cycles)
            if (seen && period >= 14000 * cycles && period <= 19000 * cycles)
                valid = true;
        consecutive = valid ? (consecutive < 4 ? consecutive + 1 : 4) : 0;
        edge_us = now;
        seen = true;
        if (consecutive >= 4) {
            valid_us = now;
            qualified = true;
        }
    }

    constexpr bool present(int64_t now) const {
        // Once 60 Hz is confirmed, irregular edges must not invent a loss.
        // Require a real gap in pulse activity before reporting absence.
        return qualified && now - edge_us < absence_us;
    }
};

struct AcTransitionFilter {
    static constexpr int64_t settle_us = 200000;
    bool stable = false;
    bool candidate = false;
    int64_t candidate_since = 0;

    constexpr void initialize(bool present, int64_t now) {
        stable = candidate = present;
        candidate_since = now;
    }

    constexpr bool ready(bool present, int64_t now) {
        if (present != candidate) {
            candidate = present;
            candidate_since = now;
        }
        return candidate != stable && now - candidate_since >= settle_us;
    }

    constexpr void accept() { stable = candidate; }
};
