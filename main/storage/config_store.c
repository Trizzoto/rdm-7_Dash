#include "config_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "config_store";

/* ── NVS namespace strings ────────────────────────────────────────────── */
#define NS_CAN      "can_config"
#define NS_DIMMER   "dimmer_cfg"
#define NS_ECU      "ecu_config"

/* ═══════════════════════════════════════════════════════════════════════
 *  DIMMER
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_dimmer(const brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;

    nvs_set_u32(handle, "can_id",  cfg->can_id);
    nvs_set_u8 (handle, "bit_pos", cfg->bit_position);
    nvs_set_u8 (handle, "is_mom",  cfg->is_momentary   ? 1 : 0);
    nvs_set_u8 (handle, "invert",  cfg->invert_toggle   ? 1 : 0);
    nvs_set_u8 (handle, "bright",  cfg->brightness_value);
    nvs_set_u8 (handle, "enabled", cfg->enabled          ? 1 : 0);

    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_dimmer(brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READONLY, &handle) != ESP_OK) return ESP_FAIL;

    uint32_t u32; uint8_t u8;
    if (nvs_get_u32(handle, "can_id",  &u32) == ESP_OK) cfg->can_id           = u32;
    if (nvs_get_u8 (handle, "bit_pos", &u8)  == ESP_OK) cfg->bit_position      = u8;
    if (nvs_get_u8 (handle, "is_mom",  &u8)  == ESP_OK) cfg->is_momentary      = (u8 == 1);
    if (nvs_get_u8 (handle, "invert",  &u8)  == ESP_OK) cfg->invert_toggle     = (u8 == 1);
    if (nvs_get_u8 (handle, "bright",  &u8)  == ESP_OK) cfg->brightness_value  = u8;
    if (nvs_get_u8 (handle, "enabled", &u8)  == ESP_OK) cfg->enabled           = (u8 == 1);

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  CAN BITRATE
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_bitrate(uint8_t bitrate)
{
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;
    nvs_set_u8(handle, "can_bitrate", bitrate);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_bitrate(uint8_t *bitrate)
{
    if (!bitrate) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;
    nvs_get_u8(handle, "can_bitrate", bitrate); /* keeps default if key absent */
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ECU PRESET
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_ecu_preset(uint8_t preconfig, uint8_t version)
{
    nvs_handle_t handle;
    if (nvs_open(NS_ECU, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;
    nvs_set_u8(handle, "ecu_preconfig", preconfig);
    nvs_set_u8(handle, "ecu_version",   version);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_ecu_preset(uint8_t *preconfig, uint8_t *version)
{
    if (!preconfig || !version) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (nvs_open(NS_ECU, NVS_READONLY, &handle) != ESP_OK) {
        *preconfig = 0;
        *version   = 0;
        return ESP_FAIL;
    }
    if (nvs_get_u8(handle, "ecu_preconfig", preconfig) != ESP_OK) *preconfig = 0;
    if (nvs_get_u8(handle, "ecu_version",   version)   != ESP_OK) *version   = 0;
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  WIFI CREDENTIALS
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_WIFI "wifi_cfg"

esp_err_t config_store_save_wifi(const wifi_credentials_t *creds)
{
    if (!creds) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "ssid",     creds->ssid);
    nvs_set_str(handle, "password", creds->password);
    nvs_set_u8(handle,  "auto_con", creds->auto_connect ? 1 : 0);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials saved for '%s'", creds->ssid);
    return err;
}

esp_err_t config_store_load_wifi(wifi_credentials_t *creds)
{
    if (!creds) return ESP_ERR_INVALID_ARG;
    memset(creds, 0, sizeof(*creds));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = sizeof(creds->ssid);
    if (nvs_get_str(handle, "ssid", creds->ssid, &len) != ESP_OK)
        creds->ssid[0] = '\0';

    len = sizeof(creds->password);
    if (nvs_get_str(handle, "password", creds->password, &len) != ESP_OK)
        creds->password[0] = '\0';

    uint8_t ac = 0;
    if (nvs_get_u8(handle, "auto_con", &ac) == ESP_OK)
        creds->auto_connect = (ac != 0);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_store_clear_wifi(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_erase_all(handle);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials cleared");
    return err;
}
