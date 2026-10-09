#include "../main/app_ac_input.h"
#include "../main/app_input_map.h"
constexpr uint8_t output_as_input[1] = {35};
constexpr uint8_t old_input[1] = {34};
static_assert(APP_INPUT_CHANNEL_COUNT == 1, "Exactly one input is exposed");
static_assert(APP_OUTPUT_PINS[0] == 35, "Output must use GPIO 35");
static_assert(app_input_map_valid(APP_DEFAULT_INPUTS), "Default GPIO 33 mapping must be valid");
static_assert(!app_input_map_valid(old_input), "Previous GPIO mapping must be rejected");
static_assert(!app_input_map_valid(output_as_input), "Output GPIO cannot be input");
static_assert(!app_input_map_valid(nullptr), "Null mapping must be rejected");

constexpr bool frequency(int64_t period) {
    AcPulseDetector d;
    for (unsigned i = 0; i < 20; ++i) d.edge(i * period);
    return d.present(19 * period);
}
static_assert(frequency(16667), "60 Hz must qualify");
static_assert(!frequency(8333), "120 Hz must not qualify");
static_assert(!frequency(20000), "50 Hz must not qualify");

constexpr bool missed_and_noise() {
    AcPulseDetector d;
    int64_t t = 0;
    for (unsigned i = 0; i < 30; ++i) {
        t += (i % 4 == 0 ? 3 : 1) * 16667;
        d.edge(t);
        d.edge(t + 500);
        if (i > 4 && !d.present(t + 1000)) return false;
    }
    return d.present(t + 499999) && !d.present(t + 500000)
        && !d.present(t + (1LL << 32));
}
static_assert(missed_and_noise(), "Missing cycles and short noise must not flap");

constexpr bool transitions() {
    AcTransitionFilter f;
    f.initialize(true, 0);
    if (f.ready(true, 1000000)) return false;
    if (f.ready(false, 1100000)) return false;
    if (f.ready(true, 1200000)) return false; // Brief loss, no toggle.
    if (f.ready(false, 1300000)) return false;
    if (f.ready(false, 1499999)) return false;
    if (!f.ready(false, 1500000)) return false;
    f.accept();
    if (f.ready(false, 2000000)) return false; // Held absent, no repeat.
    if (f.ready(true, 2100000)) return false;
    if (!f.ready(true, 2300000)) return false;
    f.accept();
    return !f.ready(true, 3000000);
}
static_assert(transitions(), "One toggle per stable change, none on initial/held state");

constexpr bool dc_and_independence() {
    AcPulseDetector a, b;
    a.edge(0); // Constant LOW has only one falling edge.
    for (unsigned i = 0; i < 10; ++i) b.edge(i * 16667);
    return !a.present(150003) && b.present(150003);
}
static_assert(dc_and_independence(), "DC is not AC; channels must be independent");
constexpr bool integrated() {
    AcPulseDetector d;
    AcTransitionFilter f;
    int toggles = 0;
    int64_t next = 0;
    unsigned cycle = 0;
    for (int64_t now = 0; now <= 4000000; now += 1000) {
        while (next <= now) {
            // Live initially, removed at 1.5 s and restored at 2.5 s.
            // Missing individual cycles and extra short edges must not toggle.
            if ((next < 1500000 || next >= 2500000) && cycle % 7 != 0) {
                d.edge(next);
                d.edge(next + 500);
            }
            next += 16667;
            ++cycle;
        }
        if (now == 750000) f.initialize(d.present(now), now);
        if (now > 750000 && f.ready(d.present(now), now)) {
            ++toggles;
            f.accept();
        }
        if (now < 1500000 && toggles != 0) return false;
    }
    return toggles == 2 && f.stable;
}
static_assert(integrated(), "No flashing on noisy mains; one toggle on removal and return");
constexpr bool disturbed_live_input() {
    AcPulseDetector d;
    for (int i = 0; i < 12; ++i) d.edge(i * 16667LL);
    // Repeated interference 6 ms after each mains edge breaks period tracking,
    // but an energized input must remain present, with no false transitions.
    for (int i = 12; i < 120; ++i) {
        d.edge(i * 16667LL);
        d.edge(i * 16667LL + 6000);
        if (!d.present(i * 16667LL + 10000)) return false;
    }
    const int64_t last = 119 * 16667LL + 6000;
    if (d.present(last + 500000)) return false;
    d.edge(last + 600000);
    return !d.present(last + 600001); // A stray edge cannot restore qualification.
}
static_assert(disturbed_live_input(), "Continuous noisy AC must not flash outputs; real silence must expire");
int main() { return 0; }
