/**
 * serial_commands_system.c — device/system settings serial JSON-RPC handlers.
 *
 * Methods: brightness.get, brightness.set, can.config.get, can.config.set,
 * system.health, system.reboot, dimmer.get, dimmer.set, wifi.config.get,
 * wifi.config.set.
 *
 * Owns the private _deferred_serial_dimmer_subscribe helper used only by
 * dimmer.set.
 */
#include "serial_commands_internal.h"
#include "system/safe_restart.h"
#include "system/rdm_lv_async.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "storage/config_store.h"
#include "ui/settings/device_settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* LVGL mutex (defined in main.c) */
extern bool rdm_lvgl_lock(int timeout_ms);
extern void rdm_lvgl_unlock(void);

/* ── brightness.get / brightness.set ─────────────────────────────────────── */

void _handle_brightness_get(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "brightness", (int)current_brightness);
    _send_response(id, r, NULL);
}

void _handle_brightness_set(int id, cJSON *params)
{
    cJSON *j = cJSON_GetObjectItem(params, "brightness");
    if (!cJSON_IsNumber(j)) {
        _send_error(id, "Missing brightness");
        return;
    }
    int val = (int)j->valuedouble;
    if (val < 1) val = 1;
    if (val > 100) val = 100;
    set_display_brightness(val);
    _send_ok(id);
}

/* ── can.config.get / can.config.set ────────────────────────────────────── */

void _handle_can_config_get(int id, cJSON *params)
{
    (void)params;
    uint8_t bitrate = 2;
    config_store_load_bitrate(&bitrate);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bitrate", (int)bitrate);
    _send_response(id, r, NULL);
}

void _handle_can_config_set(int id, cJSON *params)
{
    cJSON *j = cJSON_GetObjectItem(params, "bitrate");
    if (!cJSON_IsNumber(j)) {
        _send_error(id, "Missing bitrate");
        return;
    }
    uint8_t bitrate = (uint8_t)j->valuedouble;
    if (bitrate > 3) {
        _send_error(id, "Invalid bitrate index (0-3)");
        return;
    }
    config_store_save_bitrate(bitrate);
    _send_ok(id);
}

/* ── dimmer.get / dimmer.set ─────────────────────────────────────────────── */

void _handle_dimmer_get(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "signal_name", dimmer_config.signal_name);
    cJSON_AddNumberToObject(r, "threshold", dimmer_config.threshold);
    cJSON_AddBoolToObject(r, "is_momentary", dimmer_config.is_momentary);
    cJSON_AddBoolToObject(r, "invert", dimmer_config.invert);
    cJSON_AddNumberToObject(r, "dim_brightness", dimmer_config.dim_brightness);
    cJSON_AddBoolToObject(r, "enabled", dimmer_config.enabled);
    _send_response(id, r, NULL);
}

static void _deferred_serial_dimmer_subscribe(void *arg)
{
    (void)arg;
    dimmer_subscribe();
}

void _handle_dimmer_set(int id, cJSON *params)
{
    if (!rdm_lvgl_lock(1000)) {
        _send_error(id, "LVGL busy");
        return;
    }

    cJSON *j;
    if ((j = cJSON_GetObjectItem(params, "signal_name")) && cJSON_IsString(j)) {
        strncpy(dimmer_config.signal_name, j->valuestring,
                sizeof(dimmer_config.signal_name) - 1);
        dimmer_config.signal_name[sizeof(dimmer_config.signal_name) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItem(params, "threshold")) && cJSON_IsNumber(j))
        dimmer_config.threshold = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(params, "is_momentary")))
        dimmer_config.is_momentary = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(params, "invert")))
        dimmer_config.invert = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(params, "dim_brightness")) && cJSON_IsNumber(j))
        dimmer_config.dim_brightness = (uint8_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(params, "enabled")))
        dimmer_config.enabled = cJSON_IsTrue(j);

    rdm_lvgl_unlock();

    save_dimmer_config_to_nvs();
    rdm_async_call(_deferred_serial_dimmer_subscribe, NULL);

    _send_ok(id);
}

/* ── system.health ──────────────────────────────────────────────────────── */

void _handle_system_health(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime_s",
        (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(r, "heap_free",
        (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "heap_min_free",
        (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(r, "psram_free",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    int rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    cJSON_AddNumberToObject(r, "wifi_rssi", rssi);
    _send_response(id, r, NULL);
}

/* ── system.reboot ──────────────────────────────────────────────────────── */

void _handle_system_reboot(int id, cJSON *params)
{
    (void)params;
    _send_ok(id);
    vTaskDelay(pdMS_TO_TICKS(500));
    rdm_safe_restart();
}

/* ── WiFi config serial commands ────────────────────────────────────────── */

void _handle_wifi_config_get(int id, cJSON *params)
{
    (void)params;
    wifi_credentials_t creds = {0};
    config_store_load_wifi(&creds);

    wifi_boot_config_t boot = {0};
    config_store_load_wifi_boot(&boot);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "ssid", creds.ssid);
    cJSON_AddStringToObject(r, "password", creds.password);
    cJSON_AddBoolToObject(r, "auto_connect", creds.auto_connect);
    cJSON_AddBoolToObject(r, "wifi_on_boot", boot.wifi_on_boot);
    _send_response(id, r, NULL);
}

void _handle_wifi_config_set(int id, cJSON *params)
{
    wifi_credentials_t creds = {0};
    config_store_load_wifi(&creds);

    cJSON *j;
    if ((j = cJSON_GetObjectItem(params, "ssid")) && cJSON_IsString(j)) {
        strncpy(creds.ssid, j->valuestring, sizeof(creds.ssid) - 1);
        creds.ssid[sizeof(creds.ssid) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItem(params, "password")) && cJSON_IsString(j)) {
        strncpy(creds.password, j->valuestring, sizeof(creds.password) - 1);
        creds.password[sizeof(creds.password) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItem(params, "auto_connect")))
        creds.auto_connect = cJSON_IsTrue(j);
    config_store_save_wifi(&creds);

    if ((j = cJSON_GetObjectItem(params, "wifi_on_boot"))) {
        wifi_boot_config_t boot = {0};
        config_store_load_wifi_boot(&boot);
        boot.wifi_on_boot = cJSON_IsTrue(j);
        config_store_save_wifi_boot(&boot);
    }

    _send_ok(id);
}
