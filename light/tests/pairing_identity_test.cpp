#include "../main/app_pairing_identity.h"
#include <cassert>
#include <cstring>
#include <initializer_list>

int main() {
    // Matter bounds, prohibited PINs and the old example PIN must never be selected.
    assert(app_pairing_pin_valid(1));
    assert(app_pairing_pin_valid(99999998));
    for (auto pin : {0u, 99999999u, 12345678u, 87654321u, 20202021u,
                    11111111u, 22222222u, 33333333u, 44444444u,
                    55555555u, 66666666u, 77777777u, 88888888u})
        assert(!app_pairing_pin_valid(pin));
    assert(app_pairing_pin_from_random(0) == 1);
    assert(app_pairing_pin_from_random(99999997) == 99999998);
    assert(app_pairing_pin_from_random(20202020) == 0);
    assert(app_pairing_pin_from_random(UINT32_MAX) == 0);

    const uint8_t first[6] = {0x80,0x65,0x99,0x4C,0xCB,0x9C};
    const uint8_t second[6] = {0x80,0x65,0x99,0x4C,0xCB,0x9D};
    AppPairingRecord identity{};
    identity.version = APP_PAIRING_SCHEMA;
    identity.iterations = APP_PAIRING_ITERATIONS;
    memcpy(identity.mac, first, sizeof(first));
    identity.discriminator = app_pairing_discriminator(first);
    identity.passcode = 45678901;
    identity.salt[0] = 0x83;
    assert(identity.discriminator <= 4095);
    assert(app_pairing_record_valid(identity));
    assert(app_pairing_action(nullptr, first) == AppPairingAction::Generate);
    assert(app_pairing_action(&identity, first) == AppPairingAction::Restore);
    // A reboot restores the exact stored record. A cloned record on a different board regenerates.
    unsigned char stored[sizeof(identity)];
    memcpy(stored, &identity, sizeof(identity));
    AppPairingRecord restored{};
    memcpy(&restored, stored, sizeof(restored));
    assert(app_pairing_action(&restored, first) == AppPairingAction::Restore);
    assert(memcmp(&identity, &restored, sizeof(identity)) == 0);
    assert(app_pairing_action(&restored, second) == AppPairingAction::Generate);
    // Corruption must not silently rotate the QR code or enable shared test credentials.
    restored.passcode = 20202021;
    assert(app_pairing_action(&restored, first) == AppPairingAction::Reject);
    restored = identity; restored.version++;
    assert(app_pairing_action(&restored, first) == AppPairingAction::Reject);
    restored = identity; restored.iterations = 0;
    assert(app_pairing_action(&restored, first) == AppPairingAction::Reject);
    restored = identity; restored.salt[0] = 0;
    assert(app_pairing_action(&restored, first) == AppPairingAction::Reject);
    restored = identity; restored.discriminator ^= 1;
    assert(app_pairing_action(&restored, first) == AppPairingAction::Reject);
}
