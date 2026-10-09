#pragma once
#include <esp_err.h>
#include <stdint.h>
constexpr unsigned APP_RELAY_CHANNEL_COUNT = 1;
esp_err_t app_relay_init();
esp_err_t app_relay_register_endpoint(unsigned channel, uint16_t endpoint_id);
uint16_t app_relay_endpoint(unsigned channel);
bool app_relay_has_endpoint(uint16_t endpoint_id);
esp_err_t app_relay_set_power(uint16_t endpoint_id, bool on);
esp_err_t app_relay_start_switch();

esp_err_t app_relay_configure_inputs(const uint8_t *pins);
