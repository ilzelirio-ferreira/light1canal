#pragma once
#include <esp_err.h>
#include <esp_matter.h>
esp_err_t app_portal_load();
const char *app_portal_module_name();
esp_err_t app_portal_add_channel(esp_matter::endpoint_t *endpoint, unsigned channel);
esp_err_t app_portal_start();
