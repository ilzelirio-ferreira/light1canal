#include "app_relay.h"
#include "app_ac_input.h"
#include "app_blink.h"
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
static std::atomic<bool> dirty[APP_RELAY_CHANNEL_COUNT]{};
static std::atomic<bool> matter_ready{false};
static std::atomic<TaskHandle_t> syncing_task{nullptr};
static bool power[APP_RELAY_CHANNEL_COUNT]{};
static AppBlink blink[APP_RELAY_CHANNEL_COUNT]{};
static bool locally_changed[APP_RELAY_CHANNEL_COUNT]{};
static bool initialized = false;
static portMUX_TYPE output_lock = portMUX_INITIALIZER_UNLOCKED;
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

// All writers serialize the GPIO and its logical state without taking the CHIP lock.
static esp_err_t write_output_locked(unsigned channel, bool on)
{
#if CONFIG_RELAY_ACTIVE_LOW
    const bool level = !on;
#else
    const bool level = on;
#endif
    esp_err_t err = gpio_set_level(channels[channel].output, level);
    return err;
}

static esp_err_t write_power_locked(unsigned channel, bool on)
{
    esp_err_t err = write_output_locked(channel, on);
    if (err == ESP_OK) {
        power[channel] = on;
        blink[channel].power_changed(on);
        dirty[channel].store(true);
    }
    return err;
}

static void blink_tick(unsigned channel, int64_t now)
{
    portENTER_CRITICAL(&output_lock);
    if (blink[channel].tick(power[channel], now))
        write_output_locked(channel, blink[channel].phase);
    portEXIT_CRITICAL(&output_lock);
}

static esp_err_t set_channel_power(unsigned channel, bool on)
{
    portENTER_CRITICAL(&output_lock);
    esp_err_t err = write_power_locked(channel, on);
    portEXIT_CRITICAL(&output_lock);
    return err;
}

static esp_err_t toggle_local(unsigned channel)
{
    portENTER_CRITICAL(&output_lock);
    esp_err_t err = write_power_locked(channel, !power[channel]);
    if (err == ESP_OK) { locally_changed[channel] = true; dirty[channel].store(true); }
    portEXIT_CRITICAL(&output_lock);
    return err;
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
    if (!matter_ready.load()) return app_relay_restore_power(endpoint_id, on);
    // A report of local state must never replay an older value onto the GPIO.
    if (syncing_task.load() == xTaskGetCurrentTaskHandle()) return ESP_OK;
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
        if (channels[i].endpoint == endpoint_id) return set_channel_power(i, on);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_relay_set_brightness(uint16_t endpoint_id, uint8_t level)
{
    if (syncing_task.load() == xTaskGetCurrentTaskHandle() || !matter_ready.load()) return ESP_OK;
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
        if (channels[i].endpoint != endpoint_id) continue;
        portENTER_CRITICAL(&output_lock);
        blink[i].brightness_changed(power[i] ? level : APP_BLINK_MIN_LEVEL, power[i], esp_timer_get_time());
        esp_err_t err = write_output_locked(i, power[i]);
        dirty[i].store(true);
        portEXIT_CRITICAL(&output_lock);
        return err;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_relay_restore_power(uint16_t endpoint_id, bool on)
{
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
        if (channels[i].endpoint != endpoint_id) continue;
        portENTER_CRITICAL(&output_lock);
        esp_err_t err = locally_changed[i] ? ESP_OK : write_power_locked(i, on);
        portEXIT_CRITICAL(&output_lock);
        return err;
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
    if (initialized) return ESP_OK;
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
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "input GPIOs");
    initialized = true;
    return ESP_OK;
}

void app_relay_enable_matter_sync()
{
    matter_ready.store(true);
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) dirty[i].store(true);
}

static void sync_matter(intptr_t channel)
{
    using namespace chip::app::Clusters;
    if (channel < 0 || channel >= APP_RELAY_CHANNEL_COUNT) return;
    // Clear before the snapshot: any later local change requests another report.
    dirty[channel].store(false);
    portENTER_CRITICAL(&output_lock);
    const bool on = power[channel];
    const uint8_t level = blink[channel].level;
    portEXIT_CRITICAL(&output_lock);
    esp_matter_attr_val_t value = esp_matter_bool(on);
    syncing_task.store(xTaskGetCurrentTaskHandle());
    esp_err_t err = esp_matter::attribute::update(channels[channel].endpoint, OnOff::Id,
                                                OnOff::Attributes::OnOff::Id, &value);
    if (err == ESP_OK) {
        esp_matter_attr_val_t brightness = esp_matter_nullable_uint8(level);
        err = esp_matter::attribute::update(channels[channel].endpoint, LevelControl::Id,
                                            LevelControl::Attributes::CurrentLevel::Id, &brightness);
    }
    syncing_task.store(nullptr);
    if (err != ESP_OK) {
        dirty[channel].store(true);
        ESP_LOGW(TAG, "Matter state sync failed: %s", esp_err_to_name(err));
    }
    pending[channel].store(false);
}

static void queue_sync(unsigned channel)
{
    if (matter_ready.load() && dirty[channel].load() && !pending[channel].exchange(true)) {
        if (chip::DeviceLayer::PlatformMgr().ScheduleWork(sync_matter, channel) != CHIP_NO_ERROR)
            pending[channel].store(false);
    }
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
            if (filters[i].ready(present, esp_timer_get_time())) {
                if (toggle_local(i) == ESP_OK) {
                    filters[i].accept();
                    ESP_LOGI(TAG, "Channel %u: local 60 Hz %s", i + 1, present ? "present" : "absent");
                }
            }
            // Reporting is optional; a busy/unavailable Matter queue cannot delay the GPIO.
            blink_tick(i, esp_timer_get_time());
            queue_sync(i);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_relay_start_switch()
{
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
esp_err_t app_relay_set_brightness(uint16_t, uint8_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_relay_restore_power(uint16_t, bool) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_relay_start_switch() { return ESP_OK; }
void app_relay_enable_matter_sync() {}
#endif
