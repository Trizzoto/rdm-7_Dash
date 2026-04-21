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

/* ── Multi-SSID known-networks list ─────────────────────────────────────
   Stores up to CONFIG_STORE_WIFI_SLOT_COUNT named networks. The single
   config_store_save_wifi() above writes to slot 0 for backwards
   compatibility; the wifi manager iterates through all slots when an
   automatic reconnection attempt fails. Count is tracked separately so
   unused slots aren't tried. */
#define CONFIG_STORE_WIFI_SLOT_COUNT 5

/* Load the full list. Returns ESP_OK and count=0 if no slots saved. */
esp_err_t config_store_load_wifi_list(wifi_credentials_t out[CONFIG_STORE_WIFI_SLOT_COUNT], uint8_t *count);

/* Replace the full list. Truncates to CONFIG_STORE_WIFI_SLOT_COUNT entries. */
esp_err_t config_store_save_wifi_list(const wifi_credentials_t *entries, uint8_t count);

/* Add an entry to the list (overwrites any existing entry with the same SSID;
   otherwise appends; evicts the oldest if list is full). */
esp_err_t config_store_add_wifi(const wifi_credentials_t *entry);

/* Remove a known network by SSID. Returns ESP_ERR_NOT_FOUND if absent. */
esp_err_t config_store_remove_wifi(const char *ssid);

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

/* ── Splash fade setting ──────────────────────────────────────────────── */
esp_err_t config_store_save_splash_fade(bool enabled);
esp_err_t config_store_load_splash_fade(bool *enabled);

/* ── First-run flag (#17) ───────────────────────────────────────────────
   Set to true once the first-run wizard has been dismissed or completed.
   On a fresh NVS or after factory reset, this is false and the dash boots
   into a welcoming AP-enabled mode with an on-screen setup hint. */
esp_err_t config_store_save_first_run_done(bool done);
esp_err_t config_store_load_first_run_done(bool *done);

/* ── Display rotation & night mode (#23) ────────────────────────────────
   rotation: 0 / 90 / 180 / 270 — applied via lv_disp_set_rotation on boot.
   Night mode: either auto (time-based, 18:00–06:00 local derived from a
   selected hour-signal) or manual (toggled from the settings UI). When
   active, the global brightness is capped at `night_brightness`. */
typedef enum {
    DISPLAY_ROT_0   = 0,
    DISPLAY_ROT_90  = 1,
    DISPLAY_ROT_180 = 2,
    DISPLAY_ROT_270 = 3,
} display_rotation_t;

esp_err_t config_store_save_rotation(uint8_t rot);
esp_err_t config_store_load_rotation(uint8_t *rot);

typedef struct {
    bool enabled;           /* master switch for night-mode auto-dim */
    bool manual_active;     /* user-toggled (forces night mode on regardless of time) */
    uint8_t night_brightness; /* 5..100 — target brightness when night mode is active */
} night_mode_config_t;

esp_err_t config_store_save_night_mode(const night_mode_config_t *cfg);
esp_err_t config_store_load_night_mode(night_mode_config_t *cfg);

/* ── Data logger sample rate ──────────────────────────────────────────────
 * Sample rate in Hz. Special value 0 = "Max" (sample on every LVGL tick;
 * effectively bus-coalesced ~70-200 Hz depending on system load).
 * Default: 10 Hz. */
esp_err_t config_store_save_log_rate_hz(uint16_t hz);
esp_err_t config_store_load_log_rate_hz(uint16_t *hz);

/* ── ECU selection (auto-configures default layout signal bindings) ─────
 * Persists the user's selected ECU make + version. Loaded on first-run
 * wizard completion and in Device Settings to auto-populate the active
 * layout's signals[] array via ecu_preset_apply_to_layout(). */
esp_err_t config_store_save_ecu(const char *make, const char *version);
esp_err_t config_store_load_ecu(char *make, size_t m_len,
                                char *version, size_t v_len);

/* ── Calculated gear — compute current gear from RPM / SPEED ─────────────
 * With a known wheel circumference, final drive, and per-gear ratios, the
 * firmware can back-compute the currently engaged gear and inject it as
 * the CALCULATED_GEAR signal. Users configure via the Studio "Gear Setup"
 * modal (shown when they pick CALCULATED_GEAR as a data source).
 *
 * ratios[0] is reserved for Neutral / stationary (nominally 0.0 — matched
 * when speed is below the stopped threshold). ratios[1..ratio_count-1]
 * are 1st through Nth gear, in gear-order. Final drive multiplies
 * through (so ratios here are GEARBOX ratios, not overall ratios). */
#define GEAR_CAL_MAX_GEARS 9  /* N + up to 8 forward gears */

typedef struct {
    float   wheel_circumference_m;  /* metres — e.g. 1.95 for 255/40R18 */
    float   final_drive;            /* differential ratio, e.g. 4.11 */
    uint8_t ratio_count;            /* how many entries in ratios[] are valid */
    float   ratios[GEAR_CAL_MAX_GEARS];  /* [0]=N (0.0), [1]=1st, [2]=2nd, ... */
    char    rpm_signal[32];         /* name of RPM signal (default "RPM") */
    char    speed_signal[32];       /* name of speed signal (default "VEHICLE_SPEED") */
    bool    enabled;                /* master switch */
} gear_cal_config_t;

esp_err_t config_store_save_gear_cal(const gear_cal_config_t *cfg);
esp_err_t config_store_load_gear_cal(gear_cal_config_t *cfg);

/* ── Factory reset (erases all NVS + LittleFS user content) ────────────── */
void config_store_factory_reset(void);

#ifdef __cplusplus
}
#endif
