#pragma once

#include "esp_err.h"
#include "device_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the config store.  Call after nvs_flash_init() but before
 *        any other config_store function.  Currently a no-op placeholder for
 *        future caching / validation logic.
 */
esp_err_t config_store_init(void);

/* ── Legacy stubs (no-op — config is now persisted via JSON layout) ─────── */
/* These exist only for compile-compatibility with widget files that still
 * call them.  They will be removed once all widget files are updated. */
#include "ui_Screen3.h"
esp_err_t config_store_save_values(const value_config_t *cfg, uint8_t count);
esp_err_t config_store_load_values(value_config_t *cfg, uint8_t count);
esp_err_t config_store_save_warnings(const warning_config_t *cfg, uint8_t count);
esp_err_t config_store_load_warnings(warning_config_t *cfg, uint8_t count);
esp_err_t config_store_save_indicators(const indicator_config_t *cfg, uint8_t count);
esp_err_t config_store_load_indicators(indicator_config_t *cfg, uint8_t count);

/* ── Brightness dimmer ──────────────────────────────────────────────────── */
esp_err_t config_store_save_dimmer(const brightness_dimmer_config_t *cfg);
esp_err_t config_store_load_dimmer(brightness_dimmer_config_t *cfg);

/* ── CAN bitrate (single u8 index; shared can_config namespace) ─────────── */
esp_err_t config_store_save_bitrate(uint8_t bitrate);
esp_err_t config_store_load_bitrate(uint8_t *bitrate);

/* ── ECU preset selection ───────────────────────────────────────────────── */
esp_err_t config_store_save_ecu_preset(uint8_t preconfig, uint8_t version);
esp_err_t config_store_load_ecu_preset(uint8_t *preconfig, uint8_t *version);

/* ── WiFi credentials ──────────────────────────────────────────────────── */
typedef struct {
	char ssid[33];
	char password[65];
	bool auto_connect;
} wifi_credentials_t;

esp_err_t config_store_save_wifi(const wifi_credentials_t *creds);
esp_err_t config_store_load_wifi(wifi_credentials_t *creds);
esp_err_t config_store_clear_wifi(void);

#ifdef __cplusplus
}
#endif
