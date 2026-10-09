#include "app_relay.h"
#include "app_ac_input.h"
#include "app_input_map.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S2
#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_matter.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <platform/CHIPDeviceLayer.h>
#include <atomic>
static const char *TAG = "relay_switch";
struct Channel {
    gpio_num_t output;
    gpio_num_t input;
    uint16_t endpoint;
};
static_assert(APP_RELAY_CHANNEL_COUNT == APP_INPUT_CHANNEL_COUNT, "Channel counts must match");
static Channel channels[APP_RELAY_CHANNEL_COUNT] = {
    {static_cast<gpio_num_t>(APP_OUTPUT_PINS[0]), static_cast<gpio_num_t>(APP_DEFAULT_INPUTS[0]), 0},
};
static std::atomic<bool> pending[APP_RELAY_CHANNEL_COUNT]{};
static AcPulseDetector pulses[APP_RELAY_CHANNEL_COUNT]{};
static portMUX_TYPE pulse_lock = portMUX_INITIALIZER_UNLOCKED;

static void pulse_isr(void *arg)
{
    const unsigned channel = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&pulse_lock);
    pulses[channel].edge(now);
    portEXIT_CRITICAL_ISR(&pulse_lock);
}

static bool signal_present(unsigned channel)
{
    portENTER_CRITICAL(&pulse_lock);
    const AcPulseDetector pulse = pulses[channel];
    portEXIT_CRITICAL(&pulse_lock);
    const int64_t now = esp_timer_get_time();
    return pulse.present(now);
}

static esp_err_t set_channel_power(unsigned channel, bool on)
{
#if CONFIG_RELAY_ACTIVE_LOW
    on = !on;
#endif
    return gpio_set_level(channels[channel].output, on);
}

esp_err_t app_relay_register_endpoint(unsigned channel, uint16_t endpoint_id)
{
    if (channel >= APP_RELAY_CHANNEL_COUNT || endpoint_id == 0) return ESP_ERR_INVALID_ARG;
    for (const auto &item : channels) {
        if (item.endpoint == endpoint_id) return ESP_ERR_INVALID_ARG;
    }
    channels[channel].endpoint = endpoint_id;
    ESP_LOGI(TAG, "Channel %u: endpoint %u, input GPIO %d, output GPIO %d",
             channel + 1, endpoint_id, channels[channel].input, channels[channel].output);
    return ESP_OK;
}

uint16_t app_relay_endpoint(unsigned channel)
{
    return channel < APP_RELAY_CHANNEL_COUNT ? channels[channel].endpoint : 0;
}

bool app_relay_has_endpoint(uint16_t endpoint_id)
{
    if (!endpoint_id) return false;
    for (const auto &item : channels) if (item.endpoint == endpoint_id) return true;
    return false;
}

esp_err_t app_relay_set_power(uint16_t endpoint_id, bool on)
{
    if (!endpoint_id) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
        if (channels[i].endpoint == endpoint_id) return set_channel_power(i, on);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_relay_configure_inputs(const uint8_t *pins)
{
    if (!app_input_map_valid(pins)) return ESP_ERR_INVALID_ARG;
    for (unsigned i=0;i<APP_RELAY_CHANNEL_COUNT;++i) channels[i].input = static_cast<gpio_num_t>(pins[i]);
    return ESP_OK;
}
esp_err_t app_relay_init()
{
    gpio_config_t output = {};
    output.mode = GPIO_MODE_OUTPUT;
    gpio_config_t input = {};
    input.mode = GPIO_MODE_INPUT;
    input.pull_up_en = GPIO_PULLUP_ENABLE;
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(channels[i].output) || !GPIO_IS_VALID_GPIO(channels[i].input))
            return ESP_ERR_INVALID_ARG;
        ESP_RETURN_ON_ERROR(set_channel_power(i, false), TAG, "relay initial level");
        output.pin_bit_mask |= 1ULL << channels[i].output;
        input.pin_bit_mask |= 1ULL << channels[i].input;
    }
    ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "relay GPIOs");
    return gpio_config(&input);
}

static void toggle(intptr_t channel)
{
    using namespace chip::app::Clusters;
    if (channel < 0 || channel >= APP_RELAY_CHANNEL_COUNT) return;
    const uint16_t endpoint = channels[channel].endpoint;
    esp_matter_attr_val_t value = {};
    esp_err_t err = esp_matter::attribute::get_val(endpoint, OnOff::Id,
                                                   OnOff::Attributes::OnOff::Id, &value);
    if (err == ESP_OK) {
        value.val.b = !value.val.b;
        err = esp_matter::attribute::update(endpoint, OnOff::Id,
                                            OnOff::Attributes::OnOff::Id, &value);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Channel %ld update failed: %s", (long)channel + 1, esp_err_to_name(err));
    pending[channel].store(false);
}

static void switch_task(void *)
{
    AcTransitionFilter filters[APP_RELAY_CHANNEL_COUNT];
    // Establish the initial AC state without changing restored Matter states.
    vTaskDelay(pdMS_TO_TICKS(750));
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i)
        filters[i].initialize(signal_present(i), esp_timer_get_time());
    while (true) {
        for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
            const bool present = signal_present(i);
            if (filters[i].ready(present, esp_timer_get_time()) && !pending[i].exchange(true)) {
                if (chip::DeviceLayer::PlatformMgr().ScheduleWork(toggle, i) == CHIP_NO_ERROR) {
                    filters[i].accept();
                    ESP_LOGI(TAG, "Channel %u: 60 Hz %s", i + 1, present ? "present" : "absent");
                } else {
                    pending[i].store(false);
                    ESP_LOGE(TAG, "Could not schedule channel %u", i + 1);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_relay_start_switch()
{
    for (const auto &item : channels) if (!item.endpoint) return ESP_ERR_INVALID_STATE;
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(channels[i].input, pulse_isr,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i))), TAG, "pulse handler");
        ESP_RETURN_ON_ERROR(gpio_set_intr_type(channels[i].input, GPIO_INTR_NEGEDGE), TAG, "pulse edge");
    }
    return xTaskCreate(switch_task, "ac_inputs", 3072, nullptr, 3, nullptr) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
#else
esp_err_t app_relay_configure_inputs(const uint8_t *) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_relay_init() { return ESP_OK; }
esp_err_t app_relay_register_endpoint(unsigned, uint16_t) { return ESP_ERR_NOT_SUPPORTED; }
uint16_t app_relay_endpoint(unsigned) { return 0; }
bool app_relay_has_endpoint(uint16_t) { return false; }
esp_err_t app_relay_set_power(uint16_t, bool) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_relay_start_switch() { return ESP_OK; }
#endif
