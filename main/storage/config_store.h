#pragma once

#include "esp_err.h"
#include "device_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Brightness dimmer ──────────────────────────────────────────────────── */
esp_err_t config_store_save_dimmer(const brightness_dimmer_config_t *cfg);
esp_err_t config_store_load_dimmer(brightness_dimmer_config_t *cfg);

/* ── CAN bitrate (single u8 index; shared can_config namespace) ─────────── */
esp_err_t config_store_save_bitrate(uint8_t bitrate);
esp_err_t config_store_load_bitrate(uint8_t *bitrate);

/* ── WiFi credentials ──────────────────────────────────────────────────── */
typedef struct {
	char ssid[33];
	char password[65];
	bool auto_connect;
} wifi_credentials_t;

esp_err_t config_store_save_wifi(const wifi_credentials_t *creds);
esp_err_t config_store_load_wifi(wifi_credentials_t *creds);
esp_err_t config_store_clear_wifi(void);

/* ── WiFi AP (hotspot) settings ────────────────────────────────────────── */
typedef struct {
	bool enabled;           /* AP enabled (default: false) */
	char password[65];      /* AP password (default: "rdm7dash") */
} rdm_ap_config_t;

esp_err_t config_store_save_ap_config(const rdm_ap_config_t *cfg);
esp_err_t config_store_load_ap_config(rdm_ap_config_t *cfg);

/* ── WiFi boot settings ───────────────────────────────────────────────── */
typedef struct {
	bool wifi_on_boot;      /* Start WiFi on boot (default: false) */
	bool ap_enabled;        /* AP mode enabled (default: false) */
} wifi_boot_config_t;

esp_err_t config_store_save_wifi_boot(const wifi_boot_config_t *cfg);
esp_err_t config_store_load_wifi_boot(wifi_boot_config_t *cfg);

/* ── Factory reset (erases all NVS + LittleFS user content) ────────────── */
void config_store_factory_reset(void);

#ifdef __cplusplus
}
#endif
