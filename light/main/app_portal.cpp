#include "app_portal.h"
#include "app_relay.h"
#include "app_input_map.h"
#include <stddef.h>
#include "qrcodegen.h"
#include "app_web_helpers.h"
#include "app_portal_ui.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S2
#include <atomic>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <cJSON.h>
#include <nvs.h>
#include <esp_app_desc.h>
#include <esp_check.h>
#include <esp_http_server.h>
#include <esp_image_format.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_matter_providers.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInfoProvider.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <lib/support/CHIPMem.h>

static const char *TAG = "portal";
struct PortalConfig {
    uint32_t version;
    char module[33];
    char names[6][17]; // Preserve the existing NVS storage layout.
    char key[64];
    uint8_t inputs[6]; // Only the first entry is used by this firmware.
};
static PortalConfig config;
static uint16_t label_endpoints[APP_RELAY_CHANNEL_COUNT]{};

// Local labels do not depend on an ESP32 factory data partition or its keys.
class NameIterator : public chip::DeviceLayer::DeviceInfoProvider::FixedLabelIterator {
    unsigned channel;
    bool consumed = false;
public:
    explicit NameIterator(unsigned value) : channel(value) {}
    size_t Count() override { return channel < APP_RELAY_CHANNEL_COUNT ? 1 : 0; }
    bool Next(chip::DeviceLayer::DeviceInfoProvider::FixedLabelType &out) override {
        if (consumed || channel >= APP_RELAY_CHANNEL_COUNT) return false;
        consumed = true;
        out.label = chip::CharSpan::fromCharString("name");
        out.value = chip::CharSpan::fromCharString(config.names[channel]);
        return true;
    }
    void Release() override { chip::Platform::Delete(this); }
};

template<class T>
class EmptyIterator : public chip::DeviceLayer::DeviceInfoProvider::Iterator<T> {
public:
    size_t Count() override { return 0; }
    bool Next(T &) override { return false; }
    void Release() override { chip::Platform::Delete(this); }
};

class NameProvider : public chip::DeviceLayer::DeviceInfoProvider {
public:
    FixedLabelIterator *IterateFixedLabel(chip::EndpointId endpoint) override {
        unsigned channel = APP_RELAY_CHANNEL_COUNT;
        for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i)
            if (label_endpoints[i] == endpoint && endpoint != 0) { channel = i; break; }
        return chip::Platform::New<NameIterator>(channel);
    }
    UserLabelIterator *IterateUserLabel(chip::EndpointId) override {
        return chip::Platform::New<EmptyIterator<UserLabelType>>();
    }
    SupportedLocalesIterator *IterateSupportedLocales() override {
        return chip::Platform::New<EmptyIterator<chip::CharSpan>>();
    }
    SupportedCalendarTypesIterator *IterateSupportedCalendarTypes() override {
        return chip::Platform::New<EmptyIterator<CalendarType>>();
    }
protected:
    // These clusters are not advertised by this relay controller.
    CHIP_ERROR SetUserLabelAt(chip::EndpointId, size_t, const UserLabelType &) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR DeleteUserLabelAt(chip::EndpointId, size_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR SetUserLabelLength(chip::EndpointId, size_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetUserLabelLength(chip::EndpointId, size_t &) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};
static NameProvider name_provider;
static esp_timer_handle_t reboot_timer;
static esp_netif_t *ap_netif;
static httpd_handle_t server;
static char ap_name[32];
static std::atomic<bool> ap_active{false};
static std::atomic<bool> changing{false};

static esp_err_t store_config(const PortalConfig &value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("light_portal", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "config", &value, sizeof(value));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t app_portal_load()
{
    config = {};
    config.version = 3;
    memcpy(config.inputs, APP_DEFAULT_INPUTS, sizeof(APP_DEFAULT_INPUTS));
    snprintf(config.module, sizeof(config.module), "Light 1 canal");
    snprintf(config.key, sizeof(config.key), "configurar123");
    for (unsigned i = 0; i < sizeof(config.names) / sizeof(config.names[0]); ++i)
        snprintf(config.names[i], sizeof(config.names[i]), "Luz %u", i + 1);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("light_portal", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        PortalConfig saved = {};
        size_t size = sizeof(saved);
        err = nvs_get_blob(nvs, "config", &saved, &size);
        nvs_close(nvs);
        // Never use unterminated strings from a damaged or older NVS blob.
        bool legacy = err == ESP_OK && size == ((offsetof(PortalConfig, inputs) + alignof(PortalConfig) - 1) / alignof(PortalConfig)) * alignof(PortalConfig) && saved.version == 1;
        bool six_channel = err == ESP_OK && size == sizeof(saved) && saved.version == 2;
        if (legacy || six_channel) {
            saved.version = 3;
            memset(saved.inputs, 0, sizeof(saved.inputs));
            memcpy(saved.inputs, APP_DEFAULT_INPUTS, sizeof(APP_DEFAULT_INPUTS));
        }
        bool valid = err == ESP_OK && (legacy || (size == sizeof(saved) && saved.version == 3))
                     && app_input_map_valid(saved.inputs);
        valid = valid && memchr(saved.module, 0, sizeof(saved.module)) && memchr(saved.key, 0, sizeof(saved.key));
        for (const auto &name : saved.names) valid = valid && memchr(name, 0, sizeof(name));
        if (valid) {
            valid = portal_text(saved.module, 32) && portal_key(saved.key);
            for (const auto &name : saved.names) valid = valid && portal_text(name, 16);
        }
        if (valid) config = saved;
        else if (err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Invalid portal config; using setup defaults");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    ESP_RETURN_ON_ERROR(app_relay_configure_inputs(config.inputs), TAG, "input mapping");
    esp_matter::set_custom_device_info_provider(&name_provider);
    return ESP_OK;
}

const char *app_portal_module_name() { return config.module; }

esp_err_t app_portal_add_channel(esp_matter::endpoint_t *endpoint, unsigned channel)
{
    if (!endpoint || channel >= APP_RELAY_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    esp_matter::cluster::fixed_label::config_t label_config;
    if (!esp_matter::cluster::fixed_label::create(endpoint, &label_config, esp_matter::CLUSTER_FLAG_SERVER))
        return ESP_ERR_NO_MEM;
    label_endpoints[channel] = esp_matter::endpoint::get_id(endpoint);
    return ESP_OK;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static bool authenticated(httpd_req_t *req)
{
    char key[64] = {};
    const size_t length = httpd_req_get_hdr_value_len(req, "X-Portal-Key");
    if (length == 0 || length >= sizeof(key) ||
        httpd_req_get_hdr_value_str(req, "X-Portal-Key", key, sizeof(key)) != ESP_OK) {
        reply(req, "401 Unauthorized", "Informe a senha do painel.");
        return false;
    }
    if (!portal_key_matches(key, length, config.key)) {
        reply(req, "401 Unauthorized", "Senha do painel incorreta.");
        return false;
    }
    // Non-simple custom header also prevents cross-origin form submissions.
    return true;
}

static esp_err_t json_reply(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return reply(req, "500 Internal Server Error", "Memória insuficiente.");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML)-1);
}

static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t)
{
    if (!ap_active.load()) return reply(req, "404 Not Found", "Página não encontrada.");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "Abra http://192.168.4.1/", HTTPD_RESP_USE_STRLEN);
}

// Generate locally and expose setup credentials only through the authenticated config API.
static void add_matter_pairing(cJSON *json)
{
    char payload_text[128] = {}, manual_text[32] = {};
    chip::MutableCharSpan qr_span(payload_text), manual_span(manual_text);
    const chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kOnNetwork);
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const bool window_open = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
    const unsigned fabrics = chip::Server::GetInstance().GetFabricTable().FabricCount();
    chip::PayloadContents payload;
    CHIP_ERROR err = GetPayloadContents(payload, flags);
    if (err == CHIP_NO_ERROR) err = GetQRCode(qr_span, payload);
    if (err == CHIP_NO_ERROR) err = GetManualPairingCode(manual_span, payload);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (err != CHIP_NO_ERROR) return;
    // Version 5 comfortably fits the standard Matter setup payload; keep stack use bounded.
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(5)], qr[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
    if (!qrcodegen_encodeText(payload_text, temp, qr, qrcodegen_Ecc_MEDIUM,
                             1, 5, qrcodegen_Mask_AUTO, true)) return;
    cJSON *pairing = cJSON_AddObjectToObject(json, "matter");
    if (!pairing) return;
    cJSON_AddBoolToObject(pairing, "windowOpen", window_open);
    cJSON_AddNumberToObject(pairing, "fabrics", fabrics);
    wifi_ap_record_t access_point = {};
    cJSON_AddBoolToObject(pairing, "wifiConnected", esp_wifi_sta_get_ap_info(&access_point) == ESP_OK);
    cJSON_AddStringToObject(pairing, "payload", payload_text);
    cJSON_AddStringToObject(pairing, "manual", manual_text);
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        char device[18];
        snprintf(device, sizeof(device), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(pairing, "device", device);
    }
    cJSON *rows = cJSON_AddArrayToObject(pairing, "qr");
    if (!rows) return;
    const int size = qrcodegen_getSize(qr);
    char row[38];
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) row[x] = qrcodegen_getModule(qr, x, y) ? '1' : '0';
        row[size] = 0;
        cJSON_AddItemToArray(rows, cJSON_CreateString(row));
    }
}

static esp_err_t config_get(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    wifi_config_t wifi = {};
    esp_wifi_get_config(WIFI_IF_STA, &wifi);
    char ssid[33] = {};
    memcpy(ssid, wifi.sta.ssid, sizeof(wifi.sta.ssid));
    char ip[16] = "192.168.4.1";
    auto *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    cJSON *json = cJSON_CreateObject();
    if (!json) return reply(req, "500 Internal Server Error", "Memória insuficiente.");
    cJSON_AddStringToObject(json, "module", config.module);
    cJSON_AddStringToObject(json, "ssid", ssid);
    cJSON_AddStringToObject(json, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(json, "ip", ip);
    cJSON *names = cJSON_AddArrayToObject(json, "names");
    for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i)
        cJSON_AddItemToArray(names, cJSON_CreateString(config.names[i]));
    cJSON *inputs = cJSON_AddArrayToObject(json, "inputs");
    cJSON *outputs = cJSON_AddArrayToObject(json, "outputs");
    for (unsigned i=0;i<APP_RELAY_CHANNEL_COUNT;++i) {
        cJSON_AddItemToArray(inputs, cJSON_CreateNumber(config.inputs[i]));
        cJSON_AddItemToArray(outputs, cJSON_CreateNumber(APP_OUTPUT_PINS[i]));
    }
    add_matter_pairing(json);
    return json_reply(req, json);
}

static esp_err_t pairing_post(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    wifi_ap_record_t access_point = {};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK)
        return reply(req, "409 Conflict", "Conecte a placa ao mesmo Wi-Fi do Echo antes de parear.");
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto &manager = chip::Server::GetInstance().GetCommissioningWindowManager();
    // An explicit authenticated request enables the original per-board code again.
    // Keep an existing session intact; retry after its window closes if necessary.
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (!manager.IsCommissioningWindowOpen())
        err = manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(900),
                                                  chip::CommissioningWindowAdvertisement::kDnssdOnly);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Cannot open pairing window: %" CHIP_ERROR_FORMAT, err.Format());
        return reply(req, "409 Conflict", "Nao foi possivel abrir o pareamento. Aguarde a tentativa atual terminar e tente novamente.");
    }
    return config_get(req);
}

static bool receive_exact(httpd_req_t *req, char *buffer, size_t length)
{
    size_t offset = 0;
    const int64_t deadline = esp_timer_get_time() + 15000000;
    while (offset < length) {
        int got = httpd_req_recv(req, buffer+offset, length-offset);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline) continue;
        if (got <= 0 || esp_timer_get_time() >= deadline) return false;
        offset += got;
    }
    return true;
}

static const char *json_string(cJSON *json, const char *key)
{
    auto *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    if (changing.exchange(true)) return reply(req, "409 Conflict", "Outra operação está em andamento.");
    char body[1537];
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) {
        changing.store(false);
        return reply(req, "400 Bad Request", "Configuração inválida ou muito grande.");
    }
    if (!receive_exact(req, body, req->content_len)) {
        changing.store(false);
        return reply(req, "408 Request Timeout", "Envio incompleto. Tente novamente.");
    }
    body[req->content_len] = 0;
    cJSON *json = cJSON_ParseWithOpts(body, nullptr, true);
    PortalConfig next = config;
    const char *module = json_string(json, "module");
    const char *ssid = json_string(json, "ssid");
    const char *password = json_string(json, "password");
    const char *new_key = json_string(json, "newKey");
    cJSON *names = cJSON_GetObjectItemCaseSensitive(json, "names");
    bool valid = cJSON_IsObject(json) && portal_text(module, 32) && portal_text(ssid, 32, true)
                 && password && strlen(password) <= 63 && new_key && strlen(new_key) <= 63
                 && (new_key[0] == 0 || portal_key(new_key))
                 && cJSON_IsArray(names) && cJSON_GetArraySize(names) == static_cast<int>(APP_RELAY_CHANNEL_COUNT);
    if (valid) {
        snprintf(next.module, sizeof(next.module), "%s", module);
        for (unsigned i = 0; i < APP_RELAY_CHANNEL_COUNT; ++i) {
            cJSON *name = cJSON_GetArrayItem(names, i);
            if (!cJSON_IsString(name) || !portal_text(name->valuestring, 16)) { valid = false; break; }
            snprintf(next.names[i], sizeof(next.names[i]), "%s", name->valuestring);
        }
        if (new_key[0]) snprintf(next.key, sizeof(next.key), "%s", new_key);
    }
    cJSON *inputs = cJSON_GetObjectItemCaseSensitive(json, "inputs");
    if (valid && inputs) {
        valid = cJSON_IsArray(inputs) && cJSON_GetArraySize(inputs) == static_cast<int>(APP_RELAY_CHANNEL_COUNT);
        for (unsigned i=0;valid && i<APP_RELAY_CHANNEL_COUNT;++i) {
            auto *pin = cJSON_GetArrayItem(inputs, i);
            valid = cJSON_IsNumber(pin) && pin->valuedouble >= 0 && pin->valuedouble <= 40
                    && pin->valuedouble == pin->valueint;
            if (valid) next.inputs[i] = static_cast<uint8_t>(pin->valueint);
        }
        valid = valid && app_input_map_valid(next.inputs);
        if (!valid) {
            cJSON_Delete(json);
            changing.store(false);
            return reply(req, "400 Bad Request", "Entrada: use apenas GPIO 33.");
        }
    }
    wifi_config_t previous_wifi = {}, next_wifi = {};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &previous_wifi);
    next_wifi = previous_wifi;
    bool wifi_changed = valid && ssid[0];
    if (wifi_changed) {
        bool same_ssid = strlen(ssid) == strnlen(reinterpret_cast<char *>(previous_wifi.sta.ssid), 32)
                         && memcmp(ssid, previous_wifi.sta.ssid, strlen(ssid)) == 0;
        bool open = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "open"));
        if (!open && password[0] == 0 && !same_ssid) valid = false;
        if (!open && password[0] && strlen(password) < 8) valid = false;
        if (valid) {
            next_wifi = {};
            memcpy(next_wifi.sta.ssid, ssid, strlen(ssid));
            if (!open) {
                if (password[0]) memcpy(next_wifi.sta.password, password, strlen(password));
                else memcpy(next_wifi.sta.password, previous_wifi.sta.password, sizeof(next_wifi.sta.password));
            }
        }
    }
    cJSON_Delete(json);
    if (!valid) {
        changing.store(false);
        return reply(req, "400 Bad Request", "Confira os campos: módulo até 32 bytes, canais até 16 bytes, senha entre 8 e 63 caracteres.");
    }
    if (err == ESP_OK) err = store_config(next);
    if (err == ESP_OK && wifi_changed) {
        err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &next_wifi);
        if (err != ESP_OK) store_config(config);
    }
    if (err != ESP_OK) {
        changing.store(false);
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        return reply(req, "500 Internal Server Error", "Não foi possível salvar a configuração.");
    }
    err = esp_timer_start_once(reboot_timer, 1500000);
    if (err != ESP_OK) {
        store_config(config);
        if (wifi_changed) esp_wifi_set_config(WIFI_IF_STA, &previous_wifi);
        changing.store(false);
        return reply(req, "500 Internal Server Error", "Não foi possível agendar o reinício.");
    }
    cJSON *result = cJSON_CreateObject();
    if (!result) return reply(req, "200 OK", "Configuração salva. Reiniciando.");
    cJSON_AddStringToObject(result, "message", "Configuração salva. Reiniciando. Se a rede não conectar, o portal reaparecerá em 30 segundos.");
    return json_reply(req, result);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    if (changing.exchange(true)) return reply(req, "409 Conflict", "Outra operação está em andamento.");
    const auto *partition = esp_ota_get_next_update_partition(nullptr);
    constexpr size_t prefix_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    char buffer[4096];
    if (!partition || req->content_len < prefix_size || static_cast<size_t>(req->content_len) > partition->size) {
        changing.store(false);
        return reply(req, "400 Bad Request", "Arquivo inválido ou maior que a partição OTA. Use o light.bin.");
    }
    if (!receive_exact(req, buffer, prefix_size)) {
        changing.store(false);
        return reply(req, "408 Request Timeout", "Envio incompleto. Firmware atual preservado.");
    }
    esp_image_header_t header;
    esp_app_desc_t app;
    memcpy(&header, buffer, sizeof(header));
    memcpy(&app, buffer+sizeof(header)+sizeof(esp_image_segment_header_t), sizeof(app));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S2 ||
        app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(app.project_name, esp_app_get_description()->project_name, sizeof(app.project_name)) != 0) {
        changing.store(false);
        return reply(req, "400 Bad Request", "Esse arquivo não é um light.bin para ESP32-S2.");
    }
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, req->content_len, &handle);
    if (err != ESP_OK) {
        changing.store(false);
        return reply(req, "500 Internal Server Error", "Não foi possível iniciar a atualização.");
    }
    err = esp_ota_write(handle, buffer, prefix_size);
    size_t remaining = req->content_len - prefix_size;
    while (err == ESP_OK && remaining) {
        size_t chunk = std::min(remaining, sizeof(buffer));
        if (!receive_exact(req, buffer, chunk)) { err = ESP_ERR_TIMEOUT; break; }
        err = esp_ota_write(handle, buffer, chunk);
        remaining -= chunk;
    }
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        changing.store(false);
        ESP_LOGE(TAG, "OTA receive/write failed: %s", esp_err_to_name(err));
        return reply(req, "400 Bad Request", "Atualização interrompida. Firmware atual preservado.");
    }
    err = esp_ota_end(handle); // Validates the complete image before switching boot slot.
    if (err == ESP_OK) err = esp_ota_set_boot_partition(partition);
    if (err == ESP_OK) err = esp_timer_start_once(reboot_timer, 1500000);
    if (err != ESP_OK) {
        esp_ota_set_boot_partition(esp_ota_get_running_partition());
        changing.store(false);
        ESP_LOGE(TAG, "OTA validation/activation failed: %s", esp_err_to_name(err));
        return reply(req, "400 Bad Request", "Falha ao validar ou ativar o firmware. Versão atual preservada.");
    }
    ESP_LOGI(TAG, "Web OTA validated, rebooting");
    return reply(req, "200 OK", "Atualização validada. Reiniciando.");
}

static esp_err_t set_ap(bool enabled)
{
    chip::DeviceLayer::StackLock lock;
    auto &connectivity = chip::DeviceLayer::ConnectivityMgr();
    if (connectivity.GetWiFiAPMode() != chip::DeviceLayer::ConnectivityManager::kWiFiAPMode_NotSupported) {
        if (connectivity.SetWiFiAPMode(chip::DeviceLayer::ConnectivityManager::kWiFiAPMode_ApplicationControlled) != CHIP_NO_ERROR)
            return ESP_FAIL;
    }
    if (enabled) {
        if (!ap_netif) {
            ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();
            if (!ap_netif) return ESP_ERR_NO_MEM;
        }
        esp_netif_ip_info_t ip = {};
        ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap_netif, &ip), TAG, "AP IP");
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4 = ip.ip;
        uint8_t offer_dns = 0x02; // DHCPS_OFFER_DNS, as in IDF softap_sta example.
        esp_netif_dhcps_stop(ap_netif);
        ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                            ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)), TAG, "DHCP DNS");
        ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns), TAG, "AP DNS");
        esp_err_t dhcp = esp_netif_dhcps_start(ap_netif);
        if (dhcp != ESP_OK && dhcp != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) return dhcp;
        wifi_config_t ap = {};
        snprintf(reinterpret_cast<char *>(ap.ap.ssid), sizeof(ap.ap.ssid), "%s", ap_name);
        ap.ap.ssid_len = strlen(ap_name);
        snprintf(reinterpret_cast<char *>(ap.ap.password), sizeof(ap.ap.password), "%s", config.key);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.max_connection = 2;
        // Stop scans/reconnects before enabling APSTA; otherwise the driver
        // can advertise its open default AP before accepting our configuration.
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "AP prepare");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (err == ESP_OK) err = esp_wifi_start();
        if (err != ESP_OK) {
            // Do not leave an unconfigured open AP running after a failure.
            esp_err_t restore = esp_wifi_set_mode(WIFI_MODE_STA);
            if (restore == ESP_OK) restore = esp_wifi_start();
            if (restore != ESP_OK) ESP_LOGE(TAG, "Wi-Fi recovery failed: %s", esp_err_to_name(restore));
            ESP_LOGE(TAG, "AP configuration failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "AP stop");
    }
    ap_active.store(enabled);
    ESP_LOGI(TAG, "Configuration AP %s: %s", enabled ? "enabled" : "disabled", ap_name);
    return ESP_OK;
}

static void network_task(void *)
{
    int64_t offline_since = esp_timer_get_time();
    int64_t online_since = 0;
    while (true) {
        esp_netif_ip_info_t info = {};
        auto *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        const bool online = sta && esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr;
        const int64_t now = esp_timer_get_time();
        if (online) {
            offline_since = now;
            if (!online_since) online_since = now;
            wifi_sta_list_t clients = {};
            if (!changing.load() && ap_active.load() && now - online_since >= 30000000 &&
                esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num == 0) set_ap(false);
        } else {
            online_since = 0;
            if (!changing.load() && !ap_active.load() && now - offline_since >= 30000000) set_ap(true);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void dns_task(void *)
{
    const uint8_t ip[4] = {192,168,4,1};
    while (true) {
        if (!ap_active.load()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        sockaddr_in local = {};
        local.sin_family = AF_INET; local.sin_port = htons(53);
        inet_pton(AF_INET, "192.168.4.1", &local.sin_addr);
        timeval timeout = {1,0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0) {
            while (ap_active.load()) {
                uint8_t packet[512];
                sockaddr_in peer = {};
                socklen_t length = sizeof(peer);
                int got = recvfrom(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&peer), &length);
                if (got <= 0) continue;
                size_t size = portal_dns_reply(packet, got, sizeof(packet), ip);
                if (size) sendto(fd, packet, size, 0, reinterpret_cast<sockaddr *>(&peer), length);
            }
        }
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t app_portal_start()
{
    ESP_LOGI(TAG, "Starting web configuration");
    // attribute::update takes the Matter lock itself. Holding StackLock here
    // would deadlock on SDK builds without recursive lock tracking.
    {
        using namespace chip::app::Clusters;
        esp_matter_attr_val_t value = esp_matter_char_str(config.module, strlen(config.module));
        ESP_RETURN_ON_ERROR(esp_matter::attribute::update(0, BasicInformation::Id,
                            BasicInformation::Attributes::NodeLabel::Id, &value), TAG, "module name");
    }
    // A previously stored APSTA mode can expose the driver's default open AP.
    // Keep only the station until the offline recovery task enables our AP.
    {
        chip::DeviceLayer::StackLock lock;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "initial Wi-Fi mode");
    }
    esp_timer_create_args_t timer = {};
    timer.callback = [](void *) { esp_restart(); };
    timer.name = "portal_reboot";
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &reboot_timer), TAG, "reboot timer");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "AP MAC");
    snprintf(ap_name, sizeof(ap_name), "Light-%02X%02X%02X", mac[3], mac[4], mac[5]);
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 8192;
    http.max_uri_handlers = 5;
    http.max_open_sockets = 4;
    http.lru_purge_enable = true;
    http.recv_wait_timeout = 5;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &http), TAG, "HTTP server");
    httpd_uri_t root = {}; root.uri = "/"; root.method = HTTP_GET; root.handler = root_handler;
    httpd_uri_t get = {}; get.uri = "/api/config"; get.method = HTTP_GET; get.handler = config_get;
    httpd_uri_t save = {}; save.uri = "/api/config"; save.method = HTTP_POST; save.handler = config_post;
    httpd_uri_t pairing = {}; pairing.uri = "/api/matter/pair"; pairing.method = HTTP_POST; pairing.handler = pairing_post;
    httpd_uri_t ota = {}; ota.uri = "/api/ota"; ota.method = HTTP_POST; ota.handler = ota_post;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "root handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get), TAG, "config handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save), TAG, "save handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &pairing), TAG, "pairing handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &ota), TAG, "OTA handler");
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_redirect), TAG, "captive redirect");
    if (xTaskCreate(network_task, "portal_network", 4096, nullptr, 2, nullptr) != pdPASS ||
        xTaskCreate(dns_task, "portal_dns", 3072, nullptr, 2, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    // Startup has reached Matter, GPIO and web initialization successfully.
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
        ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG, "OTA confirmation");
    ESP_LOGI(TAG, "Web configuration ready; recovery AP %s after 30 seconds offline", ap_name);
    return ESP_OK;
}
#else
esp_err_t app_portal_load() { return ESP_OK; }
const char *app_portal_module_name() { return "Light"; }
esp_err_t app_portal_add_channel(esp_matter::endpoint_t *, unsigned) { return ESP_OK; }
esp_err_t app_portal_start() { return ESP_OK; }
#endif
