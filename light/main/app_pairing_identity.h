#pragma once
#include <stdint.h>

constexpr uint32_t APP_PAIRING_SCHEMA = 1;
constexpr uint32_t APP_PAIRING_ITERATIONS = 10000;
constexpr uint32_t APP_PAIRING_MAX_PIN = 99999998;
struct AppPairingRecord {
    uint32_t version;
    uint32_t iterations;
    uint8_t mac[6];
    uint16_t discriminator;
    uint32_t passcode;
    uint8_t salt[16];
};
static_assert(sizeof(AppPairingRecord) == 36, "Persistent pairing format changed");

constexpr bool app_pairing_pin_valid(uint32_t pin) {
    if (!pin || pin > APP_PAIRING_MAX_PIN || pin == 12345678 || pin == 87654321 || pin == 20202021) return false;
    for (uint32_t repeated = 11111111; repeated <= 88888888; repeated += 11111111)
        if (pin == repeated) return false;
    return true;
}
// Rejection sampling avoids bias when converting a random 32-bit word to a PIN.
constexpr uint32_t app_pairing_pin_from_random(uint32_t word) {
    constexpr uint64_t limit = ((1ULL << 32) / APP_PAIRING_MAX_PIN) * APP_PAIRING_MAX_PIN;
    if (word >= limit) return 0;
    const uint32_t pin = word % APP_PAIRING_MAX_PIN + 1;
    return app_pairing_pin_valid(pin) ? pin : 0;
}
constexpr uint16_t app_pairing_discriminator(const uint8_t *mac) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 6; ++i) hash = (hash ^ mac[i]) * 16777619u;
    // Matter allows only 12 bits here; the discriminator is not a unique device ID.
    return static_cast<uint16_t>(hash & 0xFFF);
}
constexpr bool app_pairing_record_valid(const AppPairingRecord &record) {
    if (record.version != APP_PAIRING_SCHEMA || record.iterations != APP_PAIRING_ITERATIONS
        || !app_pairing_pin_valid(record.passcode)
        || record.discriminator != app_pairing_discriminator(record.mac)) return false;
    for (auto byte : record.salt) if (byte) return true;
    return false;
}
enum class AppPairingAction { Generate, Restore, Reject };
constexpr AppPairingAction app_pairing_action(const AppPairingRecord *record, const uint8_t *mac) {
    if (!record) return AppPairingAction::Generate;
    if (!app_pairing_record_valid(*record)) return AppPairingAction::Reject;
    for (unsigned i = 0; i < 6; ++i)
        if (record->mac[i] != mac[i]) return AppPairingAction::Generate;
    return AppPairingAction::Restore;
}
