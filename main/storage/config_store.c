#include "config_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "config_store";

/* ── NVS namespace strings ────────────────────────────────────────────── */
#define NS_CAN      "can_config"
#define NS_DIMMER   "dimmer_cfg"

/* ═══════════════════════════════════════════════════════════════════════
 *  DIMMER
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_dimmer(const brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;

    esp_err_t err;
    err = nvs_set_str(handle, "sig_name", cfg->signal_name);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set sig_name failed"); nvs_close(handle); return err; }
    err = nvs_set_u16(handle, "thresh",   (uint16_t)(cfg->threshold * 100.0f));
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set thresh failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "is_mom",   cfg->is_momentary ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set is_mom failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "invert",   cfg->invert        ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set invert failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "dim_br",   cfg->dim_brightness);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set dim_br failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "enabled",  cfg->enabled        ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set enabled failed"); nvs_close(handle); return err; }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_dimmer(brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READONLY, &handle) != ESP_OK) return ESP_FAIL;

    size_t len = sizeof(cfg->signal_name);
    if (nvs_get_str(handle, "sig_name", cfg->signal_name, &len) != ESP_OK)
        cfg->signal_name[0] = '\0';

    uint16_t u16; uint8_t u8;
    if (nvs_get_u16(handle, "thresh",  &u16) == ESP_OK) cfg->threshold      = u16 / 100.0f;
    if (nvs_get_u8 (handle, "is_mom",  &u8)  == ESP_OK) cfg->is_momentary   = (u8 == 1);
    if (nvs_get_u8 (handle, "invert",  &u8)  == ESP_OK) cfg->invert         = (u8 == 1);
    if (nvs_get_u8 (handle, "dim_br",  &u8)  == ESP_OK) cfg->dim_brightness = u8;
    if (nvs_get_u8 (handle, "enabled", &u8)  == ESP_OK) cfg->enabled        = (u8 == 1);

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
    esp_err_t err = nvs_set_u8(handle, "can_bitrate", bitrate);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_bitrate(uint8_t *bitrate)
{
    if (!bitrate) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READONLY, &handle) != ESP_OK) return ESP_FAIL;
    nvs_get_u8(handle, "can_bitrate", bitrate); /* keeps default if key absent */
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

    err = nvs_set_str(handle, "ssid", creds->ssid);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_set_str(handle, "password", creds->password);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_set_u8(handle, "auto_con", creds->auto_connect ? 1 : 0);
    if (err != ESP_OK) { nvs_close(handle); return err; }
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

/* ═══════════════════════════════════════════════════════════════════════
 *  WIFI AP (HOTSPOT) SETTINGS
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_WIFI_AP "wifi_ap_cfg"

esp_err_t config_store_save_ap_config(const rdm_ap_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI_AP, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_str(handle, "password", cfg->password);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "AP config saved (enabled=%d)", cfg->enabled);
    return err;
}

esp_err_t config_store_load_ap_config(rdm_ap_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Defaults — AP disabled until user explicitly enables it */
    cfg->enabled = false;
    strncpy(cfg->password, "rdm7dash", sizeof(cfg->password) - 1);
    cfg->password[sizeof(cfg->password) - 1] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI_AP, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_OK; /* use defaults if namespace missing */

    uint8_t u8;
    if (nvs_get_u8(handle, "enabled", &u8) == ESP_OK) cfg->enabled = (u8 != 0);

    size_t len = sizeof(cfg->password);
    nvs_get_str(handle, "password", cfg->password, &len);

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  WIFI BOOT SETTINGS
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_wifi_boot(const wifi_boot_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "on_boot", cfg->wifi_on_boot ? 1 : 0);
    nvs_set_u8(handle, "ap_en", cfg->ap_enabled ? 1 : 0);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_wifi_boot(wifi_boot_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    /* Defaults */
    cfg->wifi_on_boot = false;
    cfg->ap_enabled = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_OK; /* use defaults if namespace missing */

    uint8_t u8;
    if (nvs_get_u8(handle, "on_boot", &u8) == ESP_OK) cfg->wifi_on_boot = (u8 != 0);
    if (nvs_get_u8(handle, "ap_en", &u8) == ESP_OK) cfg->ap_enabled = (u8 != 0);

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SPLASH FADE
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_SPLASH "splash_cfg"

esp_err_t config_store_save_splash_fade(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_SPLASH, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "fade", enabled ? 1 : 0);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_splash_fade(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    *enabled = true; /* default: fade enabled */
    nvs_handle_t handle;
    if (nvs_open(NS_SPLASH, NVS_READONLY, &handle) != ESP_OK) return ESP_OK;
    uint8_t u8;
    if (nvs_get_u8(handle, "fade", &u8) == ESP_OK) *enabled = (u8 != 0);
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  FACTORY RESET
 * ═══════════════════════════════════════════════════════════════════════ */

static void _erase_nvs_namespace(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Erased NVS namespace '%s'", ns);
    }
}

static void _clear_directory(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    char filepath[128];
    int count = 0;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(filepath, sizeof(filepath), "%s/%s", path, ent->d_name);
        if (unlink(filepath) == 0)
            count++;
        else
            ESP_LOGW(TAG, "Failed to delete %s", filepath);
    }
    closedir(d);
    ESP_LOGI(TAG, "Cleared %d files from %s", count, path);
}

void config_store_factory_reset(void)
{
    ESP_LOGW(TAG, "=== FACTORY RESET ===");

    /* Erase all NVS namespaces used by the application */
    _erase_nvs_namespace(NS_CAN);
    _erase_nvs_namespace(NS_DIMMER);
    _erase_nvs_namespace(NS_WIFI);
    _erase_nvs_namespace(NS_WIFI_AP);
    _erase_nvs_namespace("layout_mgr");

    /* Clear user content from LittleFS */
    _clear_directory("/lfs/layouts");
    _clear_directory("/lfs/images");
    _clear_directory("/lfs/fonts");
    _clear_directory("/lfs/presets");

    ESP_LOGW(TAG, "Factory reset complete — reboot to apply");
}
