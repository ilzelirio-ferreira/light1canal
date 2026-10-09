#pragma once
#include <esp_err.h>
// Call once, before drivers, Wi-Fi and esp_matter::start().
esp_err_t app_pairing_init();
