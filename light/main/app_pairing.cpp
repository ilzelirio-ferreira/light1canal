#include "app_pairing.h"
#include "app_pairing_identity.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S2
#if !CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER
#error "ESP32-S2 requires the per-device commissionable data provider"
#endif
#include <bootloader_random.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_matter_providers.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/CodeUtils.h>
#include <platform/CommissionableDataProvider.h>
#include <setup_payload/SetupPayload.h>

namespace {
constexpr const char *TAG = "matter_pairing";
// Early startup only: restore entropy before Wi-Fi/ADC drivers are initialized.
struct EntropySource {
    EntropySource() { bootloader_random_enable(); }
    ~EntropySource() { bootloader_random_disable(); }
};
struct NvsHandle {
    nvs_handle_t value;
    ~NvsHandle() { nvs_close(value); }
};
class PairingProvider final : public chip::DeviceLayer::CommissionableDataProvider {
    AppPairingRecord record{};
    chip::Crypto::Spake2pVerifierSerialized verifier{};
    bool ready = false;
public:
    esp_err_t Init() {
        if (ready) return ESP_OK;
        uint8_t mac[6];
        ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(mac), TAG, "eFuse MAC");
        // fctry is already reserved in partitions.csv; ordinary flashing/OTA does not overwrite it.
        ESP_RETURN_ON_ERROR(nvs_flash_init_partition("fctry"), TAG, "pairing partition");
        nvs_handle_t handle;
        ESP_RETURN_ON_ERROR(nvs_open_from_partition("fctry", "light_pair", NVS_READWRITE, &handle), TAG, "pairing NVS");
        NvsHandle nvs{handle};
        size_t size = sizeof(record);
        esp_err_t err = nvs_get_blob(handle, "identity", &record, &size);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        if (err == ESP_OK && size != sizeof(record)) return ESP_ERR_INVALID_SIZE;
        const auto action = app_pairing_action(err == ESP_ERR_NVS_NOT_FOUND ? nullptr : &record, mac);
        // Do not silently rotate credentials on corruption or fall back to the shared example PIN.
        if (action == AppPairingAction::Reject) return ESP_ERR_INVALID_STATE;
        {
            EntropySource entropy;
            if (action == AppPairingAction::Generate) {
                record = {};
                record.version = APP_PAIRING_SCHEMA;
                record.iterations = APP_PAIRING_ITERATIONS;
                memcpy(record.mac, mac, sizeof(mac));
                record.discriminator = app_pairing_discriminator(mac);
                for (unsigned attempt = 0; attempt < 128 && !record.passcode; ++attempt)
                    record.passcode = app_pairing_pin_from_random(esp_random());
                esp_fill_random(record.salt, sizeof(record.salt));
                if (!app_pairing_record_valid(record) || !chip::SetupPayload::IsValidSetupPIN(record.passcode))
                    return ESP_FAIL;
            }
            chip::Crypto::Spake2pVerifier derived;
            CHIP_ERROR crypto_err = derived.Generate(record.iterations, chip::ByteSpan(record.salt), record.passcode);
            chip::MutableByteSpan serialized(verifier);
            if (crypto_err == CHIP_NO_ERROR) crypto_err = derived.Serialize(serialized);
            if (crypto_err != CHIP_NO_ERROR || serialized.size() != sizeof(verifier)) return ESP_FAIL;
        }
        if (action == AppPairingAction::Generate) {
            ESP_RETURN_ON_ERROR(nvs_set_blob(handle, "identity", &record, sizeof(record)), TAG, "store pairing identity");
            ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "commit pairing identity");
        }
        ready = true;
        ESP_LOGI(TAG, "%s pairing identity for %02X:%02X:%02X:%02X:%02X:%02X",
                 action == AppPairingAction::Generate ? "Created" : "Restored",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return ESP_OK;
    }
    CHIP_ERROR GetSetupDiscriminator(uint16_t &value) override {
        VerifyOrReturnError(ready, CHIP_ERROR_INCORRECT_STATE);
        value = record.discriminator;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSetupPasscode(uint32_t &value) override {
        VerifyOrReturnError(ready, CHIP_ERROR_INCORRECT_STATE);
        value = record.passcode;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &value) override {
        VerifyOrReturnError(ready, CHIP_ERROR_INCORRECT_STATE);
        value = record.iterations;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &out) override {
        VerifyOrReturnError(ready, CHIP_ERROR_INCORRECT_STATE);
        VerifyOrReturnError(out.size() >= sizeof(record.salt), CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(out.data(), record.salt, sizeof(record.salt));
        out.reduce_size(sizeof(record.salt));
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &out, size_t &length) override {
        VerifyOrReturnError(ready, CHIP_ERROR_INCORRECT_STATE);
        length = sizeof(verifier);
        VerifyOrReturnError(out.size() >= length, CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(out.data(), verifier, length);
        out.reduce_size(length);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};
PairingProvider provider;
} // namespace

esp_err_t app_pairing_init() {
    ESP_RETURN_ON_ERROR(provider.Init(), TAG, "per-device commissioning data");
    esp_matter::set_custom_commissionable_data_provider(&provider);
    return ESP_OK;
}
#else
esp_err_t app_pairing_init() { return ESP_OK; }
#endif
