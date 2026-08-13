#include "device_settings.h"
#include "theme.h"
#include "lvgl.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "ui_wifi.h"
#include "ui_diagnostics.h"
#include "screens/ui_peaks.h"
#include "ota_handler.h"
#include "nvs_flash.h"
#include "version.h"
#include "device_id.h"
#include "ui.h"
#include "ui_helpers.h"
#include "screens/ui_Screen3.h"
#include "screens/first_run_wizard.h"
#include "screens/ui_can_list.h"
#include "layout/ecu_presets.h"
#include "layout/layout_manager.h"
#include "layout/default_layout.h"
#include "data/channel_manager.h"
#include "obd2_picker.h"
#include "obd2.h"
#include "dtc_reader.h"
#include "settings/ui_gear_setup.h"
#include "widgets/signal_internal.h"
#include "system/night_mode.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "ota_update_dialog.h"
#include "lwip/ip4_addr.h"
#include "callbacks/ui_callbacks.h"
#include "can/can_manager.h"
#include "can/can_bus_test.h"
#include "storage/config_store.h"
#include "storage/data_logger.h"
#include "storage/can_raw_logger.h"
#include "storage/can_upload.h"
#include <dirent.h>
#include <sys/stat.h>
#include "storage/sd_manager.h"
#include "widgets/signal.h"
#include "widgets/signal_sim.h"
#include <stdlib.h>
#include <string.h>

#include "net/wifi_manager.h"

// Global WiFi status labels for updating
static lv_obj_t* wifi_status_label = NULL;
static lv_obj_t* web_status_label = NULL;
static lv_timer_t *s_wifi_status_timer = NULL;
static lv_obj_t* wifi_loading_dialog = NULL;

/* Web-URL QR modal (Network section → "Show QR"). Phone scans the QR,
 * browser opens the editor directly — bypasses flaky .local resolution.
 * Children are auto-deleted with the overlay; only the root needs tracking. */
static lv_obj_t *s_qr_overlay   = NULL;
static lv_obj_t *s_qr_obj       = NULL;
static lv_obj_t *s_qr_url_lbl   = NULL;
static lv_timer_t *s_qr_refresh_timer = NULL;
static char       s_qr_last_url[64] = {0};

// Data logging UI state
static lv_obj_t *s_log_btn = NULL;
static lv_obj_t *s_log_btn_label = NULL;
static lv_obj_t *s_log_status_label = NULL;
static lv_obj_t *s_log_rate_dd = NULL;

/* Signal simulator toggle (demo mode) */
static lv_obj_t *s_sim_btn_label = NULL;
/* Stat label of the DEVICE-grid "Simulator" card (tap toggles sim ON/OFF) */
static lv_obj_t *s_sim_card_stat = NULL;

/* Display rotation + night-mode (#23) */
static lv_obj_t *s_rotation_btn_label = NULL;
static lv_obj_t *s_night_btn_label = NULL;

/* Developer options */

static void _rotation_btn_cb(lv_event_t *e) {
    (void)e;
    uint8_t rot = 0;
    config_store_load_rotation(&rot);
    rot = (uint8_t)((rot + 1) % 4); /* 0 → 1 → 2 → 3 → 0 */
    config_store_save_rotation(rot);
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) lv_disp_set_rotation(disp, (lv_disp_rot_t)rot);
    if (s_rotation_btn_label) {
        static const char *names[] = { "Rotation: 0\xC2\xB0", "Rotation: 90\xC2\xB0", "Rotation: 180\xC2\xB0", "Rotation: 270\xC2\xB0" };
        lv_label_set_text(s_rotation_btn_label, names[rot]);
    }
}

static void _night_btn_cb(lv_event_t *e) {
    (void)e;
    night_mode_config_t cfg;
    config_store_load_night_mode(&cfg);
    cfg.manual_active = !cfg.manual_active;
    cfg.enabled = true; /* turning it on via the button implies the feature is on */
    config_store_save_night_mode(&cfg);
    /* Apply immediately: clamp current brightness to night_brightness when active */
    if (cfg.manual_active && current_brightness > cfg.night_brightness) {
        /* Use the existing PWM update path through LEDC */
        uint32_t duty = (uint32_t)cfg.night_brightness * ((1u << 13) - 1u) / 100u;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    /* Notify the night-mode subsystem so all subscribed widgets apply
     * their per-layout color/image overrides. */
    night_mode_set_active(cfg.manual_active);
    if (s_night_btn_label) {
        lv_label_set_text(s_night_btn_label, cfg.manual_active ? "Night Mode: ON" : "Night Mode: OFF");
    }
}
static lv_timer_t *s_log_status_timer = NULL;

/* VEHICLE section — calculated gear setup button + live odometer label.
 * Defined here (rather than next to _build_section_vehicle) so that
 * close_menu_event_cb can null/free them without forward-declaration. */
static lv_obj_t   *s_veh_odo_value_lbl = NULL;
static lv_timer_t *s_veh_odo_timer     = NULL;
static lv_obj_t   *s_odo_edit_overlay  = NULL;
static lv_obj_t   *s_odo_edit_textarea = NULL;

/* CAN diagnostics — redesigned with health indicator + collapsible details */
static lv_obj_t  *s_can_health_dot     = NULL;
static lv_obj_t  *s_can_health_label   = NULL;
static lv_obj_t  *s_can_summary_label  = NULL;
static lv_obj_t  *s_can_details_grid   = NULL;
static lv_obj_t  *s_can_details_toggle = NULL;
static lv_obj_t  *s_can_detail_labels[6];  /* RX Count, TX Count, RX Err, TX Err, Bus Err, RX Missed */
static lv_timer_t *s_can_diag_timer    = NULL;
static uint32_t   s_prev_rx_count      = 0;
static uint32_t   s_rx_rate            = 0;

/* CAN bus scan overlay */
static lv_obj_t  *s_scan_overlay       = NULL;
static lv_obj_t  *s_scan_title_label   = NULL;
static lv_obj_t  *s_scan_status_label  = NULL;
static lv_obj_t  *s_scan_bar           = NULL;
static lv_obj_t  *s_scan_progress_label = NULL;
static lv_obj_t  *s_scan_result_labels[4];
static lv_obj_t  *s_scan_detail_label  = NULL;
static lv_obj_t  *s_scan_apply_btn     = NULL;
static lv_obj_t  *s_scan_close_btn     = NULL;
static lv_obj_t  *s_scan_cancel_btn    = NULL;

/* Bitrate dropdown pointer for scan apply */
static lv_obj_t  *s_bitrate_dropdown   = NULL;

// AP hotspot status label
static lv_obj_t* ap_status_label = NULL;

/* Resolve the current web-editor URL. Preference order:
 *   1. STA IP — works when the device is on the user's LAN
 *   2. AP IP  — works when phone is joined to the dash hotspot (192.168.4.1)
 *   3. NULL   — no network available
 * Returns true on success; url is "http://<ip>/" null-terminated. */
/* Build the URL the user should scan to reach the web editor.
 *
 * Priority: if the hotspot is enabled, prefer the AP IP — the typical scan
 * scenario is "phone connects to the dash's hotspot, then scans" which only
 * works against the AP-side address. STA is the fallback for when the dash
 * is on a shared network with the phone and there's no hotspot active. On
 * concurrent APSTA the AP still wins because that's the deliberate scan
 * target the user just enabled.
 *
 *   1. AP IP   (when AP enabled + started; 192.168.4.1 fallback if netif unknown)
 *   2. STA IP  (when connected to a router and the dash shares that network)
 *   3. NULL    — no network available
 */
static bool _build_web_url(char *url, size_t sz) {
    esp_netif_ip_info_t ip_info;
    if (wifi_manager_is_started() && wifi_manager_is_ap_enabled()) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(url, sz, "http://" IPSTR "/", IP2STR(&ip_info.ip));
            return true;
        }
        /* AP default if netif query fails */
        snprintf(url, sz, "http://192.168.4.1/");
        return true;
    }
    const char *sta_ssid = wifi_manager_get_connected_ssid();
    if (sta_ssid && sta_ssid[0] != '\0') {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(url, sz, "http://" IPSTR "/", IP2STR(&ip_info.ip));
            return true;
        }
    }
    return false;
}

/* Timer tick while the QR modal is open: if the URL changed since the last
 * tick (because the user toggled AP, STA acquired DHCP, etc.) re-render the
 * QR in place and update the printed URL label. Cheap because lv_qrcode
 * keeps the same host object — no overlay rebuild. Silent no-op when the
 * URL hasn't changed. */
static void _qr_refresh_tick_cb(lv_timer_t *t) {
    (void)t;
    if (!s_qr_overlay || !lv_obj_is_valid(s_qr_overlay)) return;
    if (!s_qr_obj || !lv_obj_is_valid(s_qr_obj))         return;

    char url[64];
    if (!_build_web_url(url, sizeof(url))) return;
    if (strncmp(url, s_qr_last_url, sizeof(s_qr_last_url)) == 0) return;

    strncpy(s_qr_last_url, url, sizeof(s_qr_last_url) - 1);
    s_qr_last_url[sizeof(s_qr_last_url) - 1] = '\0';
    lv_qrcode_update(s_qr_obj, url, strlen(url));
    if (s_qr_url_lbl && lv_obj_is_valid(s_qr_url_lbl)) {
        lv_label_set_text(s_qr_url_lbl, url);
    }
}

static void _qr_close_cb(lv_event_t *e) {
    (void)e;
    if (s_qr_refresh_timer) {
        lv_timer_del(s_qr_refresh_timer);
        s_qr_refresh_timer = NULL;
    }
    if (s_qr_overlay && lv_obj_is_valid(s_qr_overlay)) {
        lv_obj_del(s_qr_overlay);
    }
    s_qr_overlay = NULL;
    s_qr_obj     = NULL;
    s_qr_url_lbl = NULL;
    s_qr_last_url[0] = '\0';
}

static void _qr_btn_cb(lv_event_t *e) {
    (void)e;
    /* A stale pointer can linger if the user left Device Settings while the
     * modal was open; re-check validity so the button still works. */
    if (s_qr_overlay && lv_obj_is_valid(s_qr_overlay)) return;
    s_qr_overlay = NULL;

    char url[64];
    bool have_url = _build_web_url(url, sizeof(url));

    /* Modal root on lv_layer_top so it floats above Device Settings.
     * Height accounts for the optional STA-mode warning block (~50 px). */
    s_qr_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_qr_overlay, 400, 480);
    lv_obj_center(s_qr_overlay);
    lv_obj_set_style_bg_color(s_qr_overlay, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_qr_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_qr_overlay, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_border_color(s_qr_overlay, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_border_width(s_qr_overlay, 1, 0);
    lv_obj_set_style_shadow_width(s_qr_overlay, THEME_SHADOW_W_POPUP, 0);
    lv_obj_set_style_shadow_color(s_qr_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_qr_overlay, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_qr_overlay, 16, 0);
    lv_obj_set_flex_flow(s_qr_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_qr_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_qr_overlay, 10, 0);
    lv_obj_clear_flag(s_qr_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_qr_overlay);
    lv_label_set_text(title, "Scan with Phone");
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    if (have_url) {
        /* Black modules on white, 280px — readable from 30-60cm away */
        lv_obj_t *qr = lv_qrcode_create(s_qr_overlay, 280,
                                        lv_color_hex(0x000000),
                                        lv_color_hex(0xFFFFFF));
        lv_qrcode_update(qr, url, strlen(url));
        /* White quiet-zone ring around QR improves scanner hit-rate */
        lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(qr, 8, 0);

        lv_obj_t *url_lbl = lv_label_create(s_qr_overlay);
        lv_label_set_text(url_lbl, url);
        lv_obj_set_style_text_font(url_lbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(url_lbl, THEME_COLOR_ACCENT_BLUE, 0);

        /* If the URL is the STA-side IP (i.e. AP is off and the dash is
         * connected to a router/hotspot), the scanning device must be on
         * that same network. Most phone-hotspot APs enable client isolation
         * by default, which silently drops traffic from the phone to the
         * dash even when both are on the hotspot. Surface this so the user
         * doesn't blame the QR. AP mode (192.168.4.1) sidesteps the issue. */
        if (!wifi_manager_is_ap_enabled()) {
            lv_obj_t *warn = lv_label_create(s_qr_overlay);
            lv_label_set_text(warn,
                "If this URL won't load:\n"
                "phone hotspots usually block direct access.\n"
                "Switch to Hotspot mode in WiFi settings.");
            lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(warn, 360);
            lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(warn, THEME_FONT_SMALL, 0);
            lv_obj_set_style_text_color(warn, THEME_COLOR_TEXT_MUTED, 0);
        }

        /* Stash pointers + current URL, then poll once a second so the QR
         * re-renders live if the user toggles AP or the STA DHCP lease
         * lands after the modal was already opened. */
        s_qr_obj     = qr;
        s_qr_url_lbl = url_lbl;
        strncpy(s_qr_last_url, url, sizeof(s_qr_last_url) - 1);
        s_qr_last_url[sizeof(s_qr_last_url) - 1] = '\0';
        if (s_qr_refresh_timer) lv_timer_del(s_qr_refresh_timer);
        s_qr_refresh_timer = lv_timer_create(_qr_refresh_tick_cb, 1000, NULL);
    } else {
        lv_obj_t *msg = lv_label_create(s_qr_overlay);
        lv_label_set_text(msg,
            "No network available.\n"
            "Connect to WiFi or enable the hotspot\n"
            "from the WiFi settings first.");
        lv_obj_set_style_text_font(msg, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(msg, THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_height(msg, 280);
    }

    lv_obj_t *close_btn = lv_btn_create(s_qr_overlay);
    lv_obj_set_size(close_btn, 120, 36);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(close_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);
    lv_obj_set_style_text_font(close_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(close_label, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(close_btn, _qr_close_cb, LV_EVENT_CLICKED, NULL);
}

// Function to refresh WiFi status displays
/* Refresh the WiFi + Web Editor card stats. wifi_status_label and
 * web_status_label are now the .stat_label of those two cards in the
 * CONNECTIVITY grid (see _build_connectivity_grid). Stats are short,
 * uppercase-friendly — title + body already describe the section, so
 * the stat is just the live status (SSID, IP, "OFFLINE", etc). */
static void refresh_wifi_status(void) {
    /* Defensive bail — labels can survive screen delete if cleanup
     * skipped a path. lv_obj_is_valid catches freed-but-non-NULL. */
    if (!wifi_status_label || !lv_obj_is_valid(wifi_status_label)) {
        wifi_status_label = NULL;
        if (web_status_label && !lv_obj_is_valid(web_status_label)) {
            web_status_label = NULL;
        }
        return;
    }

    const char *sta_ssid = wifi_manager_get_connected_ssid();
    if (sta_ssid && sta_ssid[0] != '\0') {
        lv_label_set_text(wifi_status_label, sta_ssid);
    } else if (wifi_manager_is_started() && wifi_manager_is_ap_enabled()) {
        char buf[48];
        snprintf(buf, sizeof(buf), "AP: %s", wifi_manager_get_ap_ssid());
        lv_label_set_text(wifi_status_label, buf);
    } else {
        lv_label_set_text(wifi_status_label, "OFFLINE");
    }

    if (web_status_label && lv_obj_is_valid(web_status_label)) {
        char buf[48];
        if (sta_ssid && sta_ssid[0] != '\0') {
            esp_netif_ip_info_t ip_info;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(web_status_label, buf);
            } else {
                lv_label_set_text(web_status_label, "WAITING");
            }
        } else if (wifi_manager_is_started() && wifi_manager_is_ap_enabled()) {
            lv_label_set_text(web_status_label, "192.168.4.1");
        } else {
            lv_label_set_text(web_status_label, "OFFLINE");
        }
    }
}

// Timer callback wrapper for WiFi status refresh
static void refresh_wifi_status_timer_cb(lv_timer_t* timer) {
    refresh_wifi_status();
}

// LEDC configuration defines
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          16 // GPIO16
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // 13 bits
#define LEDC_FREQUENCY          5000 // 5 kHz

// Static variables
static bool ledc_initialized = false;
static lv_obj_t* brightness_label = NULL;
uint8_t current_brightness = 100; // Track current brightness value (non-static for extern access)

// Brightness dimmer switch configuration (typedef is in header)
brightness_dimmer_config_t dimmer_config = {
    .signal_name = "",
    .threshold = 0.5f,
    .is_momentary = true,
    .invert = false,
    .dim_brightness = 50,
    .enabled = false
};

static bool s_dimmer_toggle_state = false; // Toggle mode state
static int16_t s_dimmer_signal_idx = -1;   // Cached signal index
static lv_timer_t* brightness_preview_timer = NULL; // Timer for brightness preview demo
static uint8_t saved_brightness_before_preview = 100; // Store brightness before preview
static bool s_brightness_previewing = false; // Guard against capturing preview value as saved

static lv_obj_t* device_settings_return_screen = NULL; // Screen to return to when closing device settings

void set_display_brightness(int percent) {
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    
    // Track current brightness value
    current_brightness = percent;

    if (!ledc_initialized) {
        ledc_timer_config_t ledc_timer = {
            .speed_mode       = LEDC_MODE,
            .timer_num        = LEDC_TIMER,
            .duty_resolution  = LEDC_DUTY_RES,
            .freq_hz          = LEDC_FREQUENCY,
            .clk_cfg          = LEDC_AUTO_CLK
        };
        ledc_timer_config(&ledc_timer);

        ledc_channel_config_t ledc_channel = {
            .speed_mode     = LEDC_MODE,
            .channel        = LEDC_CHANNEL,
            .timer_sel      = LEDC_TIMER,
            .intr_type      = LEDC_INTR_DISABLE,
            .gpio_num       = LEDC_OUTPUT_IO,
            .duty           = 0,
            .hpoint         = 0
        };
        ledc_channel_config(&ledc_channel);
        ledc_initialized = true;
    }

    // Map percent (5-100) to duty (0-8191 for 13 bits)
    uint32_t duty = (uint32_t)((percent / 100.0f) * ((1 << 13) - 1));
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

// Callback to update brightness
static void brightness_bar_event_cb(lv_event_t * e) {
    lv_obj_t * bar = lv_event_get_target(e);
    int val = lv_slider_get_value(bar);
    set_display_brightness(val);
    
    // Update label only - don't save to NVS
    if (brightness_label) {
        lv_label_set_text_fmt(brightness_label, "%d%%", val);
    }
}

// Forward declarations
static void brightness_dimmer_config_cb(lv_event_t * e);
static void save_dimmer_config_cb(lv_event_t * e);
static void close_dimmer_popup_cb(lv_event_t * e);
static void brightness_set_slider_cb(lv_event_t * e);
void save_dimmer_config_to_nvs(void);
void load_dimmer_config_from_nvs(void);

/* ── Dimmer signal callback ──────────────────────────────────────────── */

static void _dimmer_signal_cb(float value, bool is_stale, void *user_data) {
    (void)user_data;
    if (!dimmer_config.enabled || is_stale) return;

    bool active = (value >= dimmer_config.threshold);
    if (dimmer_config.invert) active = !active;

    if (dimmer_config.is_momentary) {
        /* Momentary: dim while active, restore when inactive */
        if (active)
            set_display_brightness(dimmer_config.dim_brightness);
        else
            set_display_brightness(100);
    } else {
        /* Toggle: each activation toggles the dim state */
        if (active && !s_dimmer_toggle_state) {
            s_dimmer_toggle_state = true;
            /* Toggle dim on/off based on current brightness */
            if (current_brightness == dimmer_config.dim_brightness)
                set_display_brightness(100);
            else
                set_display_brightness(dimmer_config.dim_brightness);
        } else if (!active) {
            s_dimmer_toggle_state = false;
        }
    }
}

void dimmer_subscribe(void) {
    s_dimmer_signal_idx = -1;
    s_dimmer_toggle_state = false;

    if (!dimmer_config.enabled || dimmer_config.signal_name[0] == '\0')
        return;

    int16_t idx = signal_find_by_name(dimmer_config.signal_name);
    if (idx < 0) {
        /* Signal not in layout — auto-register a placeholder so internal
           signal injection (GPIO indicators, etc.) can still feed it.
           CAN ID 0 ensures the filter builder ignores this entry. */
        idx = signal_register_with_source(dimmer_config.signal_name, 0,
                              0, 1, 1.0f, 0.0f, false, 1, "",
                              SIGNAL_SOURCE_INTERNAL);
    }
    if (idx >= 0) {
        signal_subscribe(idx, _dimmer_signal_cb, NULL);
        s_dimmer_signal_idx = idx;
        ESP_LOGI("DIMMER", "Subscribed to signal '%s' (idx %d)",
                 dimmer_config.signal_name, idx);
    }
}

/* ── Dimmer popup: build signal options string for dropdown ────────── */

static uint16_t _build_signal_options(char *buf, size_t buf_size) {
    /* Internal signals (always available) */
    static const char *internal_signals[] = {
        "INDICATOR_LEFT", "INDICATOR_RIGHT", "FUEL_SENDER_V",
        "CHIP_TEMP", "FPS", "CPU_PERCENT", "FREE_HEAP_KB",
        "FREE_PSRAM_KB", "UPTIME_S", "WIFI_RSSI"
    };
    size_t pos = 0;
    uint16_t count = 0;
    uint16_t selected = 0;

    for (int i = 0; i < 10; i++) {
        if (pos > 0 && pos < buf_size - 1) buf[pos++] = '\n';
        size_t slen = strlen(internal_signals[i]);
        if (pos + slen >= buf_size - 1) break;
        memcpy(buf + pos, internal_signals[i], slen);
        if (strcmp(internal_signals[i], dimmer_config.signal_name) == 0)
            selected = count;
        pos += slen;
        count++;
    }

    /* Layout signals (from the current signal registry) */
    uint16_t sig_count = signal_get_count();
    for (uint16_t s = 0; s < sig_count; s++) {
        signal_t *sig = signal_get_by_index(s);
        if (!sig || sig->name[0] == '\0') continue;
        /* Skip duplicates (internal signals already listed) */
        bool dup = false;
        for (int i = 0; i < 10; i++) {
            if (strcmp(sig->name, internal_signals[i]) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (pos > 0 && pos < buf_size - 1) buf[pos++] = '\n';
        size_t slen = strlen(sig->name);
        if (pos + slen >= buf_size - 1) break;
        memcpy(buf + pos, sig->name, slen);
        if (strcmp(sig->name, dimmer_config.signal_name) == 0)
            selected = count;
        pos += slen;
        count++;
    }

    buf[pos] = '\0';
    return selected;
}

// Brightness dimmer switch configuration popup
static void brightness_dimmer_config_cb(lv_event_t * e) {
    // Create semi-transparent overlay
    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, close_dimmer_popup_cb, LV_EVENT_CLICKED, overlay);

    // Create popup container — uses settings_panel for consistent look
    lv_obj_t* popup = lv_obj_create(overlay);
    lv_obj_set_size(popup, 480, 360);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(popup, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(popup, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(popup, 1, 0);
    lv_obj_set_style_radius(popup, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_shadow_width(popup, 20, 0);
    lv_obj_set_style_shadow_ofs_y(popup, 4, 0);
    lv_obj_set_style_shadow_color(popup, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(popup, 140, 0);
    lv_obj_set_style_pad_all(popup, 0, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_CLICKABLE);

    /* ── Header bar ─────────────────────────────────────────────────── */
    lv_obj_t* hdr = lv_obj_create(popup);
    lv_obj_set_size(hdr, lv_pct(100), 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, "Brightness Dimmer Switch");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t* close_btn = lv_btn_create(hdr);
    lv_obj_set_size(close_btn, 32, 28);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SCROLLBAR, LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_center(close_label);
    lv_obj_set_style_text_font(close_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(close_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(close_btn, close_dimmer_popup_cb, LV_EVENT_CLICKED, overlay);

    /* ── Content area — flex column for clean layout ────────────────── */
    lv_obj_t* body = lv_obj_create(popup);
    lv_obj_set_size(body, lv_pct(100), 260);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 14, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* Row 1: Signal Source + Threshold side by side */
    lv_obj_t* row1 = lv_obj_create(body);
    lv_obj_set_size(row1, lv_pct(100), 52);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row1, 0, 0);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* signal_label = lv_label_create(row1);
    lv_label_set_text(signal_label, "Signal Source");
    lv_obj_align(signal_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(signal_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(signal_label, THEME_FONT_TINY, 0);

    lv_obj_t* signal_dd = lv_dropdown_create(row1);
    lv_obj_set_size(signal_dd, 260, 30);
    lv_obj_align(signal_dd, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(signal_dd, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(signal_dd, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(signal_dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(signal_dd, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(signal_dd, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(signal_dd, 1, 0);
    lv_obj_set_style_radius(signal_dd, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(signal_dd, 4, 0);
    lv_obj_set_style_text_color(signal_dd, THEME_COLOR_TEXT_MUTED, LV_PART_INDICATOR);
    {
        static char sig_options[1024];
        uint16_t sel = _build_signal_options(sig_options, sizeof(sig_options));
        lv_dropdown_set_options(signal_dd, sig_options);
        lv_dropdown_set_selected(signal_dd, sel);
    }

    lv_obj_t* thresh_label = lv_label_create(row1);
    lv_label_set_text(thresh_label, "Threshold");
    lv_obj_align(thresh_label, LV_ALIGN_TOP_LEFT, 275, 0);
    lv_obj_set_style_text_color(thresh_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(thresh_label, THEME_FONT_TINY, 0);

    lv_obj_t* thresh_input = lv_textarea_create(row1);
    lv_obj_set_size(thresh_input, 100, 30);
    lv_obj_align(thresh_input, LV_ALIGN_BOTTOM_LEFT, 275, 0);
    lv_textarea_set_one_line(thresh_input, true);
    lv_textarea_set_max_length(thresh_input, 8);
    lv_obj_set_style_bg_color(thresh_input, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(thresh_input, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(thresh_input, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(thresh_input, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(thresh_input, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(thresh_input, 1, 0);
    lv_obj_set_style_radius(thresh_input, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(thresh_input, 4, 0);
    lv_obj_set_style_border_color(thresh_input, THEME_COLOR_ACCENT_BLUE, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(thresh_input, 2, LV_STATE_FOCUSED);
    char thresh_str[16];
    snprintf(thresh_str, sizeof(thresh_str), "%.2f", dimmer_config.threshold);
    lv_textarea_set_text(thresh_input, thresh_str);
    lv_obj_add_event_cb(thresh_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

    /* Row 2: Toggle Mode + Invert side by side */
    lv_obj_t* row2 = lv_obj_create(body);
    lv_obj_set_size(row2, lv_pct(100), 52);
    lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_style_pad_all(row2, 0, 0);
    lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* toggle_mode_label = lv_label_create(row2);
    lv_label_set_text(toggle_mode_label, "Toggle Mode");
    lv_obj_align(toggle_mode_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(toggle_mode_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(toggle_mode_label, THEME_FONT_TINY, 0);

    lv_obj_t* toggle_mode_dd = lv_dropdown_create(row2);
    lv_obj_set_size(toggle_mode_dd, 130, 30);
    lv_obj_align(toggle_mode_dd, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(toggle_mode_dd, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(toggle_mode_dd, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(toggle_mode_dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(toggle_mode_dd, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(toggle_mode_dd, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(toggle_mode_dd, 1, 0);
    lv_obj_set_style_radius(toggle_mode_dd, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(toggle_mode_dd, 4, 0);
    lv_obj_set_style_text_color(toggle_mode_dd, THEME_COLOR_TEXT_MUTED, LV_PART_INDICATOR);
    lv_dropdown_set_options(toggle_mode_dd, "On/Off\nMomentary");
    lv_dropdown_set_selected(toggle_mode_dd, dimmer_config.is_momentary ? 1 : 0);

    lv_obj_t* invert_label = lv_label_create(row2);
    lv_label_set_text(invert_label, "Invert");
    lv_obj_align(invert_label, LV_ALIGN_TOP_LEFT, 180, 0);
    lv_obj_set_style_text_color(invert_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(invert_label, THEME_FONT_TINY, 0);

    lv_obj_t* invert_switch = lv_switch_create(row2);
    lv_obj_set_size(invert_switch, 50, 25);
    lv_obj_align(invert_switch, LV_ALIGN_BOTTOM_LEFT, 180, 0);
    if (dimmer_config.invert) {
        lv_obj_add_state(invert_switch, LV_STATE_CHECKED);
    }

    lv_obj_t* enable_label = lv_label_create(row2);
    lv_label_set_text(enable_label, "Enabled");
    lv_obj_align(enable_label, LV_ALIGN_TOP_LEFT, 275, 0);
    lv_obj_set_style_text_color(enable_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(enable_label, THEME_FONT_TINY, 0);

    lv_obj_t* enable_switch = lv_switch_create(row2);
    lv_obj_set_size(enable_switch, 50, 25);
    lv_obj_align(enable_switch, LV_ALIGN_BOTTOM_LEFT, 275, 0);
    if (dimmer_config.enabled) {
        lv_obj_add_state(enable_switch, LV_STATE_CHECKED);
    }

    /* Row 3: Dim Brightness slider */
    lv_obj_t* row3 = lv_obj_create(body);
    lv_obj_set_size(row3, lv_pct(100), 48);
    lv_obj_set_style_bg_opa(row3, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row3, 0, 0);
    lv_obj_set_style_pad_all(row3, 0, 0);
    lv_obj_clear_flag(row3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* brightness_set_label = lv_label_create(row3);
    lv_label_set_text(brightness_set_label, "Dim Brightness");
    lv_obj_align(brightness_set_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(brightness_set_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(brightness_set_label, THEME_FONT_TINY, 0);

    lv_obj_t* brightness_set_slider = lv_slider_create(row3);
    lv_obj_set_size(brightness_set_slider, 340, 18);
    lv_obj_align(brightness_set_slider, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_slider_set_range(brightness_set_slider, 5, 100);
    lv_slider_set_value(brightness_set_slider, dimmer_config.dim_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_set_slider, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_radius(brightness_set_slider, THEME_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(brightness_set_slider, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightness_set_slider, THEME_RADIUS_PILL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_set_slider, THEME_COLOR_TEXT_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_radius(brightness_set_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_set_slider, 2, LV_PART_KNOB);

    lv_obj_t* brightness_value_label = lv_label_create(row3);
    lv_label_set_text_fmt(brightness_value_label, "%d%%", dimmer_config.dim_brightness);
    lv_obj_align(brightness_value_label, LV_ALIGN_BOTTOM_LEFT, 355, 0);
    lv_obj_set_style_text_color(brightness_value_label, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(brightness_value_label, THEME_FONT_SMALL, 0);

    lv_obj_add_event_cb(brightness_set_slider, brightness_set_slider_cb, LV_EVENT_VALUE_CHANGED, brightness_value_label);

    /* ── Footer with Save button ────────────────────────────────────── */
    lv_obj_t* footer = lv_obj_create(popup);
    lv_obj_set_size(footer, lv_pct(100), 52);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(footer, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(footer, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* save_btn = lv_btn_create(footer);
    lv_obj_set_size(save_btn, 160, 34);
    lv_obj_align(save_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(save_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, LV_SYMBOL_SAVE "  Save");
    lv_obj_set_style_text_color(save_label, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_set_style_text_font(save_label, THEME_FONT_SMALL, 0);
    lv_obj_center(save_label);

    // Store all inputs in user data for save callback
    typedef struct {
        lv_obj_t* overlay;
        lv_obj_t* signal_dd;
        lv_obj_t* thresh_input;
        lv_obj_t* toggle_mode_dd;
        lv_obj_t* invert_switch;
        lv_obj_t* brightness_slider;
        lv_obj_t* enable_switch;
    } dimmer_popup_data_t;

    dimmer_popup_data_t* popup_data = lv_mem_alloc(sizeof(dimmer_popup_data_t));
    popup_data->overlay = overlay;
    popup_data->signal_dd = signal_dd;
    popup_data->thresh_input = thresh_input;
    popup_data->toggle_mode_dd = toggle_mode_dd;
    popup_data->invert_switch = invert_switch;
    popup_data->brightness_slider = brightness_set_slider;
    popup_data->enable_switch = enable_switch;

    lv_obj_add_event_cb(save_btn, save_dimmer_config_cb, LV_EVENT_CLICKED, popup_data);
}

// Timer callback to restore brightness after preview
static void brightness_preview_restore_cb(lv_timer_t * timer) {
    set_display_brightness(saved_brightness_before_preview);
    if (brightness_preview_timer) {
        lv_timer_del(brightness_preview_timer);
        brightness_preview_timer = NULL;
    }
    s_brightness_previewing = false;
}

static void brightness_set_slider_cb(lv_event_t * e) {
    lv_obj_t* slider = lv_event_get_target(e);
    lv_obj_t* label = (lv_obj_t*)lv_event_get_user_data(e);
    int val = lv_slider_get_value(slider);
    lv_label_set_text_fmt(label, "%d%%", val);
    
    // Cancel any existing preview timer
    if (brightness_preview_timer) {
        lv_timer_del(brightness_preview_timer);
        brightness_preview_timer = NULL;
    }

    // Save current brightness before preview (only on first drag)
    if (!s_brightness_previewing) {
        saved_brightness_before_preview = current_brightness;
        s_brightness_previewing = true;
    }
    
    // Set brightness to preview value
    set_display_brightness(val);
    
    // Create timer to restore brightness after 2 seconds
    brightness_preview_timer = lv_timer_create(brightness_preview_restore_cb, 2000, NULL);
    lv_timer_set_repeat_count(brightness_preview_timer, 1); // Run once
}

static void close_dimmer_popup_cb(lv_event_t * e) {
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (overlay) {
        lv_obj_del(overlay);
    }
}

static void save_dimmer_config_cb(lv_event_t * e) {
    typedef struct {
        lv_obj_t* overlay;
        lv_obj_t* signal_dd;
        lv_obj_t* thresh_input;
        lv_obj_t* toggle_mode_dd;
        lv_obj_t* invert_switch;
        lv_obj_t* brightness_slider;
        lv_obj_t* enable_switch;
    } dimmer_popup_data_t;

    dimmer_popup_data_t* popup_data = (dimmer_popup_data_t*)lv_event_get_user_data(e);
    if (!popup_data) return;

    // Get signal name from dropdown
    char sig_buf[32];
    lv_dropdown_get_selected_str(popup_data->signal_dd, sig_buf, sizeof(sig_buf));
    strncpy(dimmer_config.signal_name, sig_buf, sizeof(dimmer_config.signal_name) - 1);
    dimmer_config.signal_name[sizeof(dimmer_config.signal_name) - 1] = '\0';

    // Get threshold
    const char* thresh_str = lv_textarea_get_text(popup_data->thresh_input);
    dimmer_config.threshold = strtof(thresh_str, NULL);

    // Get toggle mode
    dimmer_config.is_momentary = (lv_dropdown_get_selected(popup_data->toggle_mode_dd) == 1);

    // Get invert
    dimmer_config.invert = lv_obj_has_state(popup_data->invert_switch, LV_STATE_CHECKED);

    // Get brightness value
    dimmer_config.dim_brightness = lv_slider_get_value(popup_data->brightness_slider);

    // Get enabled state
    dimmer_config.enabled = lv_obj_has_state(popup_data->enable_switch, LV_STATE_CHECKED);

    // Reset toggle state when saving
    s_dimmer_toggle_state = false;

    // Save to NVS
    save_dimmer_config_to_nvs();

    // Re-subscribe to the (potentially new) signal
    dimmer_subscribe();

    // Close popup
    lv_obj_del(popup_data->overlay);
    lv_mem_free(popup_data);

    ESP_LOGI("DIMMER", "Dimmer config saved: Signal='%s', Thresh=%.2f, Mode=%s, Invert=%d, Brightness=%d%%, Enabled=%d",
        dimmer_config.signal_name, dimmer_config.threshold,
        dimmer_config.is_momentary ? "Momentary" : "Toggle",
        dimmer_config.invert, dimmer_config.dim_brightness, dimmer_config.enabled);
}

static void refresh_can_diagnostics(void) {
    /* Defensive bail: any of these statics can be a stale pointer if the
     * settings screen was torn down by a deferred dashboard reload (e.g.
     * ECU picker apply) without close_menu_event_cb running first. The
     * NULL check alone isn't enough — the pointer survives the screen
     * delete; only lv_obj_is_valid catches the freed-but-non-NULL case.
     * If anything is invalid, kill the timer too so we don't keep tripping
     * the next tick. */
    if (!s_can_health_dot   || !lv_obj_is_valid(s_can_health_dot)   ||
        !s_can_health_label || !lv_obj_is_valid(s_can_health_label) ||
        !s_can_summary_label|| !lv_obj_is_valid(s_can_summary_label)) {
        if (s_can_diag_timer) {
            lv_timer_del(s_can_diag_timer);
            s_can_diag_timer = NULL;
        }
        s_can_health_dot   = NULL;
        s_can_health_label = NULL;
        s_can_summary_label= NULL;
        memset(s_can_detail_labels, 0, sizeof(s_can_detail_labels));
        return;
    }

    /* If scan is running, show "Scanning..." state */
    if (can_bus_test_is_running()) {
        lv_obj_set_style_bg_color(s_can_health_dot, THEME_COLOR_ACCENT_YELLOW, 0);
        lv_label_set_text(s_can_health_label, "Bus scan in progress...");
        lv_obj_set_style_text_color(s_can_health_label,
                                     THEME_COLOR_ACCENT_YELLOW, 0);
        lv_label_set_text(s_can_summary_label, "");
        return;
    }

    uint32_t state = 0, msgs_to_tx = 0, msgs_to_rx = 0;
    uint32_t tx_err = 0, rx_err = 0, bus_err = 0, rx_missed = 0;

    esp_err_t err = can_get_diagnostics(&state, &msgs_to_tx, &msgs_to_rx,
                                        &tx_err, &rx_err, &bus_err, &rx_missed);
    if (err != ESP_OK) {
        lv_obj_set_style_bg_color(s_can_health_dot, THEME_COLOR_TEXT_HINT, 0);
        /* Try to bring CAN back. can_recover is rate-limited internally to
         * one attempt per 5 s so this is safe to call from the 500 ms
         * diagnostics tick. If the driver was just left in a bad state by
         * a prior failed wizard scan, this gets it talking again without
         * the user having to reboot. */
        bool recovering = can_recover();
        lv_label_set_text(s_can_health_label,
            recovering ? "Reinitialising CAN..."
                       : "CAN status unavailable");
        lv_obj_set_style_text_color(s_can_health_label,
                                     THEME_COLOR_TEXT_HINT, 0);
        lv_label_set_text(s_can_summary_label, "");
        return;
    }

    /* Compute RX rate (frames/sec) */
    uint32_t current_count = can_get_rx_frame_count();
    s_rx_rate = current_count - s_prev_rx_count;
    s_prev_rx_count = current_count;

    /* Determine health status */
    const char *health_msg;
    lv_color_t dot_color;

    if (state == TWAI_STATE_STOPPED) {
        health_msg = "CAN bus stopped";
        dot_color = THEME_COLOR_TEXT_HINT;
    } else if (state == TWAI_STATE_BUS_OFF) {
        health_msg = "No CAN traffic detected";
        dot_color = THEME_COLOR_STATUS_ERROR;
    } else if (s_rx_rate == 0 && current_count == 0) {
        health_msg = "No CAN traffic detected";
        dot_color = THEME_COLOR_STATUS_ERROR;
    } else if (state == TWAI_STATE_RECOVERING ||
               (bus_err > 10 && s_rx_rate > 0)) {
        health_msg = "CAN bus has errors";
        dot_color = THEME_COLOR_ACCENT_YELLOW;
    } else if (s_rx_rate > 0) {
        health_msg = "Receiving CAN data normally";
        dot_color = THEME_COLOR_STATUS_CONNECTED;
    } else {
        health_msg = "No CAN traffic detected";
        dot_color = THEME_COLOR_STATUS_ERROR;
    }

    lv_obj_set_style_bg_color(s_can_health_dot, dot_color, 0);
    lv_label_set_text(s_can_health_label, health_msg);
    lv_obj_set_style_text_color(s_can_health_label, dot_color, 0);

    /* Summary line: bitrate | last ID | rate */
    static const char *br_labels[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};
    uint8_t saved_br = 2;
    config_store_load_bitrate(&saved_br);
    if (saved_br > 3) saved_br = 2;

    uint32_t last_id = can_get_last_rx_id();
    if (last_id > 0) {
        lv_label_set_text_fmt(s_can_summary_label,
            "%s  |  Last ID: 0x%03lX  |  ~%lu frames/sec",
            br_labels[saved_br], (unsigned long)last_id,
            (unsigned long)s_rx_rate);
    } else {
        lv_label_set_text_fmt(s_can_summary_label,
            "%s  |  No frames received",
            br_labels[saved_br]);
    }

    /* Update detail labels */
    lv_label_set_text_fmt(s_can_detail_labels[0], "RX Count: %lu",
                          (unsigned long)msgs_to_rx);
    lv_label_set_text_fmt(s_can_detail_labels[1], "RX Errors: %lu",
                          (unsigned long)rx_err);
    lv_label_set_text_fmt(s_can_detail_labels[2], "RX Missed: %lu",
                          (unsigned long)rx_missed);
    lv_label_set_text_fmt(s_can_detail_labels[3], "TX Count: %lu",
                          (unsigned long)msgs_to_tx);
    lv_label_set_text_fmt(s_can_detail_labels[4], "TX Errors: %lu",
                          (unsigned long)tx_err);
    lv_label_set_text_fmt(s_can_detail_labels[5], "Bus Errors: %lu",
                          (unsigned long)bus_err);
}

static void refresh_can_diag_timer_cb(lv_timer_t* timer) {
    refresh_can_diagnostics();
}

/* ── Details toggle callback ───────────────────────────────────────────── */

static void _details_toggle_cb(lv_event_t *e) {
    (void)e;
    if (!s_can_details_grid || !s_can_details_toggle) return;
    bool hidden = lv_obj_has_flag(s_can_details_grid, LV_OBJ_FLAG_HIDDEN);
    if (hidden) {
        lv_obj_clear_flag(s_can_details_grid, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_can_details_toggle, LV_SYMBOL_DOWN " Hide Details");
    } else {
        lv_obj_add_flag(s_can_details_grid, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_can_details_toggle, LV_SYMBOL_RIGHT " Show Details");
    }
}

/* ── Scan overlay ──────────────────────────────────────────────────────── */

static void _close_scan_overlay(void) {
    if (s_scan_overlay && lv_obj_is_valid(s_scan_overlay)) {
        lv_obj_del(s_scan_overlay);
    }
    s_scan_overlay = NULL;
    can_bus_test_set_ui_callback(NULL);
}

static void _scan_close_cb(lv_event_t *e) {
    (void)e;
    _close_scan_overlay();
}

static void _scan_cancel_cb(lv_event_t *e) {
    (void)e;
    can_bus_test_cancel();
}

static void _scan_apply_cb(lv_event_t *e) {
    (void)e;
    const can_scan_report_t *r = can_bus_test_get_report();
    if (r->recommended_bitrate < 0) return;

    uint8_t idx = (uint8_t)r->recommended_bitrate;

    /* Apply bitrate */
    config_store_save_bitrate(idx);
    can_change_bitrate(idx);

    /* Update dropdown if still valid */
    if (s_bitrate_dropdown && lv_obj_is_valid(s_bitrate_dropdown)) {
        lv_dropdown_set_selected(s_bitrate_dropdown, idx);
    }

    _close_scan_overlay();
    refresh_can_diagnostics();
}

/** Called via lv_async_call from can_bus_test task on state changes. */
/* Legacy in-page CAN scan overlay — superseded by the first-run wizard's
 * CAN scan step. Kept compiled in for now in case it's resurrected; the
 * unused attribute silences the warning until then. */
__attribute__((unused))
static void _scan_ui_update(void) {
    if (!s_scan_overlay || !lv_obj_is_valid(s_scan_overlay)) return;

    const can_scan_report_t *r = can_bus_test_get_report();
    static const char *br_names[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};

    switch (r->state) {
    case CAN_SCAN_STOPPING:
        lv_label_set_text(s_scan_status_label, "Stopping CAN for scan...");
        break;

    case CAN_SCAN_TESTING_BITRATE: {
        lv_label_set_text(s_scan_status_label, "Scanning for CAN traffic...");
        uint8_t idx = r->current_bitrate_idx;
        lv_label_set_text_fmt(s_scan_progress_label,
            "Testing %s  (%d of 4)", br_names[idx], idx + 1);
        /* Update progress bar (0-100) */
        lv_bar_set_value(s_scan_bar, (idx * 25), LV_ANIM_ON);

        /* Update per-bitrate result labels */
        for (uint8_t i = 0; i < 4; i++) {
            if (i < idx) {
                if (r->results[i].traffic_detected) {
                    lv_label_set_text_fmt(s_scan_result_labels[i],
                        "%s --- %lu frames", br_names[i],
                        (unsigned long)r->results[i].frames_received);
                    lv_obj_set_style_text_color(s_scan_result_labels[i],
                        THEME_COLOR_STATUS_CONNECTED, 0);
                } else {
                    lv_label_set_text_fmt(s_scan_result_labels[i],
                        "%s --- No traffic", br_names[i]);
                    lv_obj_set_style_text_color(s_scan_result_labels[i],
                        THEME_COLOR_TEXT_MUTED, 0);
                }
            } else if (i == idx) {
                lv_label_set_text_fmt(s_scan_result_labels[i],
                    "%s --- Testing...", br_names[i]);
                lv_obj_set_style_text_color(s_scan_result_labels[i],
                    THEME_COLOR_ACCENT_YELLOW, 0);
            }
            /* i > idx: leave as "..." */
        }
        break;
    }

    case CAN_SCAN_RESTORING:
        lv_bar_set_value(s_scan_bar, 95, LV_ANIM_ON);
        lv_label_set_text(s_scan_status_label, "Restoring CAN...");
        break;

    case CAN_SCAN_COMPLETE:
    case CAN_SCAN_CANCELLED: {
        lv_bar_set_value(s_scan_bar, 100, LV_ANIM_ON);

        /* Update all result labels */
        for (uint8_t i = 0; i < 4; i++) {
            if (r->results[i].traffic_detected) {
                /* Build ID list string */
                char id_buf[128] = "";
                int pos = 0;
                uint8_t show = r->results[i].unique_id_count > 6 ?
                               6 : r->results[i].unique_id_count;
                for (uint8_t j = 0; j < show; j++) {
                    pos += snprintf(id_buf + pos, sizeof(id_buf) - pos,
                        "%s0x%03lX", j > 0 ? ", " : "",
                        (unsigned long)r->results[i].unique_ids[j]);
                }
                if (r->results[i].unique_id_count > 6) {
                    snprintf(id_buf + pos, sizeof(id_buf) - pos, ", ...");
                }
                lv_label_set_text_fmt(s_scan_result_labels[i],
                    "%s --- %lu frames (%s)",
                    br_names[i],
                    (unsigned long)r->results[i].frames_received,
                    id_buf);
                lv_obj_set_style_text_color(s_scan_result_labels[i],
                    THEME_COLOR_STATUS_CONNECTED, 0);
            } else {
                lv_label_set_text_fmt(s_scan_result_labels[i],
                    "%s --- No traffic", br_names[i]);
                lv_obj_set_style_text_color(s_scan_result_labels[i],
                    THEME_COLOR_TEXT_MUTED, 0);
            }
        }

        /* Title and summary */
        if (r->state == CAN_SCAN_CANCELLED) {
            lv_label_set_text(s_scan_title_label, "Scan Cancelled");
            lv_label_set_text(s_scan_status_label, "Scan was cancelled");
        } else {
            lv_label_set_text(s_scan_title_label, "Scan Complete");
        }

        if (r->recommended_bitrate >= 0) {
            uint8_t bi = (uint8_t)r->recommended_bitrate;
            lv_label_set_text_fmt(s_scan_status_label,
                "Found CAN traffic at %s", br_names[bi]);
            lv_label_set_text_fmt(s_scan_detail_label,
                "%lu frames received, %u unique IDs",
                (unsigned long)r->results[bi].frames_received,
                r->results[bi].unique_id_count);
            lv_obj_set_style_text_color(s_scan_status_label,
                THEME_COLOR_STATUS_CONNECTED, 0);
        } else {
            /* #13 Improved "no bus detected" UX — clear troubleshooting checklist.
               Count total bus errors vs frames across all bitrates to distinguish
               "bus silent" (likely wiring / not powered) from "bus noisy but
               unreadable" (wrong termination, crossed wires, or a baud outside
               our supported range). */
            uint32_t total_errors = 0;
            uint32_t total_frames = 0;
            for (uint8_t i = 0; i < 4; i++) {
                total_errors += r->results[i].bus_errors;
                total_frames += r->results[i].frames_received;
            }

            /* Override the title so users don't mistake "Scan Complete" for success */
            lv_label_set_text(s_scan_title_label, "No CAN Bus Detected");
            lv_obj_set_style_text_color(s_scan_title_label,
                THEME_COLOR_STATUS_ERROR, 0);

            lv_label_set_text(s_scan_status_label,
                "Scan finished - no valid CAN frames on any supported bitrate.");
            lv_obj_set_style_text_color(s_scan_status_label,
                THEME_COLOR_STATUS_ERROR, 0);

            /* Ensure detail text wraps and can hold multiple lines visibly */
            lv_label_set_long_mode(s_scan_detail_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(s_scan_detail_label, lv_pct(95));
            /* Keep the muted colour — lets status label's red stand out */
            lv_obj_set_style_text_color(s_scan_detail_label,
                THEME_COLOR_TEXT_MUTED, 0);

            if (total_errors > 0 && total_frames == 0) {
                /* Bus is noisy — likely wrong baud outside our range, or a
                   wiring fault that induces errors without decodable frames. */
                lv_label_set_text_fmt(s_scan_detail_label,
                    "Bus activity seen (%lu errors) but no valid frames.\n"
                    "Troubleshoot:\n"
                    "  - CAN-H / CAN-L may be swapped\n"
                    "  - Check termination (120 Ohm at each end of the bus)\n"
                    "  - ECU baud may be outside 125/250/500/1000 kbps\n"
                    "  - Look for electrical noise on the twisted pair",
                    (unsigned long)total_errors);
            } else {
                /* Bus is silent — ECU not talking, dash not on the bus, or
                   the vehicle is off. */
                lv_label_set_text(s_scan_detail_label,
                    "Bus appears silent. Check:\n"
                    "  - Ignition ON so the ECU is powered + transmitting\n"
                    "  - CAN-H / CAN-L wired to the correct pins\n"
                    "  - Bus termination (120 Ohm at each end)\n"
                    "  - Yellow connector on the rear (dash-end terminator)\n"
                    "  - Vehicle CAN exposed at your tap-in point");
            }
        }

        /* Show/hide buttons */
        lv_obj_add_flag(s_scan_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_scan_close_btn, LV_OBJ_FLAG_HIDDEN);
        if (r->recommended_bitrate >= 0) {
            /* Only show Apply if different from current bitrate */
            uint8_t saved = 2;
            config_store_load_bitrate(&saved);
            if (saved != (uint8_t)r->recommended_bitrate) {
                lv_obj_t *apply_lbl = lv_obj_get_child(s_scan_apply_btn, 0);
                lv_label_set_text_fmt(apply_lbl, "Apply %s",
                    br_names[(uint8_t)r->recommended_bitrate]);
                lv_obj_clear_flag(s_scan_apply_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }

        lv_label_set_text(s_scan_progress_label, "");
        break;
    }

    default:
        break;
    }
}

__attribute__((unused))
static void _open_scan_overlay(void) {
    /* Create modal overlay on lv_layer_top */
    s_scan_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_scan_overlay, 420, 320);
    lv_obj_center(s_scan_overlay);
    lv_obj_set_style_bg_color(s_scan_overlay, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_scan_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_scan_overlay, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_border_color(s_scan_overlay, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_border_width(s_scan_overlay, 1, 0);
    lv_obj_set_style_shadow_width(s_scan_overlay, THEME_SHADOW_W_POPUP, 0);
    lv_obj_set_style_shadow_color(s_scan_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_scan_overlay, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_scan_overlay, 16, 0);
    lv_obj_set_style_pad_row(s_scan_overlay, 6, 0);
    lv_obj_set_flex_flow(s_scan_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_scan_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    s_scan_title_label = lv_label_create(s_scan_overlay);
    lv_label_set_text(s_scan_title_label, "CAN Bus Scan");
    lv_obj_set_style_text_font(s_scan_title_label, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(s_scan_title_label, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Status message */
    s_scan_status_label = lv_label_create(s_scan_overlay);
    lv_label_set_text(s_scan_status_label, "Starting scan...");
    lv_obj_set_style_text_font(s_scan_status_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_scan_status_label, THEME_COLOR_TEXT_MUTED, 0);

    /* Progress bar */
    s_scan_bar = lv_bar_create(s_scan_overlay);
    lv_obj_set_size(s_scan_bar, lv_pct(100), 12);
    lv_bar_set_range(s_scan_bar, 0, 100);
    lv_bar_set_value(s_scan_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_scan_bar, THEME_COLOR_INPUT_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_scan_bar, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_scan_bar, THEME_RADIUS_SMALL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_scan_bar, THEME_RADIUS_SMALL, LV_PART_INDICATOR);

    /* Progress text (e.g. "Testing 250 kbps (2 of 4)") */
    s_scan_progress_label = lv_label_create(s_scan_overlay);
    lv_label_set_text(s_scan_progress_label, "");
    lv_obj_set_style_text_font(s_scan_progress_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_scan_progress_label, THEME_COLOR_TEXT_MUTED, 0);

    /* Per-bitrate result lines */
    for (int i = 0; i < 4; i++) {
        static const char *br_init[] = {
            "125 kbps --- ...", "250 kbps --- ...",
            "500 kbps --- ...", "1 Mbps   --- ..."
        };
        s_scan_result_labels[i] = lv_label_create(s_scan_overlay);
        lv_label_set_text(s_scan_result_labels[i], br_init[i]);
        lv_obj_set_style_text_font(s_scan_result_labels[i], THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_scan_result_labels[i], THEME_COLOR_TEXT_HINT, 0);
    }

    /* Detail label (frame count + unique IDs after completion) */
    s_scan_detail_label = lv_label_create(s_scan_overlay);
    lv_label_set_text(s_scan_detail_label, "");
    lv_obj_set_style_text_font(s_scan_detail_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_scan_detail_label, THEME_COLOR_TEXT_MUTED, 0);

    /* Button row */
    lv_obj_t *btn_row = lv_obj_create(s_scan_overlay);
    lv_obj_set_size(btn_row, lv_pct(100), 34);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Cancel button (visible during scan) */
    s_scan_cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_scan_cancel_btn, 90, 30);
    lv_obj_align(s_scan_cancel_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_scan_cancel_btn, THEME_COLOR_BTN_GRAY, 0);
    lv_obj_set_style_bg_color(s_scan_cancel_btn, THEME_COLOR_BTN_GRAY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_scan_cancel_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(s_scan_cancel_btn, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(s_scan_cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(cancel_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(s_scan_cancel_btn, _scan_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* Close button (hidden during scan) */
    s_scan_close_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_scan_close_btn, 90, 30);
    lv_obj_align(s_scan_close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_scan_close_btn, THEME_COLOR_BTN_GRAY, 0);
    lv_obj_set_style_bg_color(s_scan_close_btn, THEME_COLOR_BTN_GRAY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_scan_close_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(s_scan_close_btn, 0, 0);
    lv_obj_t *close_lbl = lv_label_create(s_scan_close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_font(close_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(close_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(s_scan_close_btn, _scan_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_scan_close_btn, LV_OBJ_FLAG_HIDDEN);

    /* Apply button (hidden until results) */
    s_scan_apply_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_scan_apply_btn, 120, 30);
    lv_obj_align(s_scan_apply_btn, LV_ALIGN_RIGHT_MID, -100, 0);
    lv_obj_set_style_bg_color(s_scan_apply_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_bg_color(s_scan_apply_btn, THEME_COLOR_ACCENT_BLUE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_scan_apply_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(s_scan_apply_btn, 0, 0);
    lv_obj_t *apply_lbl = lv_label_create(s_scan_apply_btn);
    lv_label_set_text(apply_lbl, "Apply");
    lv_obj_center(apply_lbl);
    lv_obj_set_style_text_font(apply_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(apply_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(s_scan_apply_btn, _scan_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_scan_apply_btn, LV_OBJ_FLAG_HIDDEN);
}

/* Popup-close forward decls — _can_view_more_btn_cb / _view_peaks_btn_cb
 * need to tear down the card popup before swapping screens. The actual
 * popup_close definitions live further down with the rest of the popup
 * builders; declaring here keeps the call-site order legal. */
static void _can_bus_popup_close(lv_event_t *e);
static void _testing_popup_close(lv_event_t *e);
static void _logger_popup_close(lv_event_t *e);

/* "View More" button in the CAN BUS section header. Opens the live CAN ID
 * list (ui_can_list) so the user can see every ID + binary bytes ticking
 * through. Bitrate scanning is no longer surfaced here — it's accessible
 * via the "Re-run Setup Wizard" button below, which is the right place
 * for a setup-time operation that suspends the bus. */
static void _can_view_more_btn_cb(lv_event_t *e) {
    (void)e;
    /* Close the CAN Bus popup BEFORE swapping screens. Popups live on
     * lv_layer_top() which survives lv_scr_load() — without this close
     * the popup card would sit on top of the can_list screen,
     * obscuring the live CAN ID feed. */
    _can_bus_popup_close(NULL);
    /* Show can_list directly. It captures lv_scr_act() as its return
     * screen at call-time — which is still settings_screen here — so
     * Back correctly drops the user back into Device Settings rather
     * than skipping out to the dashboard. */
    can_list_ui_show();
}

// Close menu callback
static void close_menu_event_cb(lv_event_t * e) {
    // Delete WiFi status timer to prevent leak
    if (s_wifi_status_timer) {
        lv_timer_del(s_wifi_status_timer);
        s_wifi_status_timer = NULL;
    }

    // Delete CAN diagnostics timer
    if (s_can_diag_timer) {
        lv_timer_del(s_can_diag_timer);
        s_can_diag_timer = NULL;
    }

    // Delete Vehicle odometer refresh timer
    if (s_veh_odo_timer) {
        lv_timer_del(s_veh_odo_timer);
        s_veh_odo_timer = NULL;
    }
    s_veh_odo_value_lbl = NULL;

    /* Close odometer-edit overlay if user navigated away with it open
     * (lv_layer_top isn't part of the screen, so it doesn't auto-delete). */
    if (s_odo_edit_overlay && lv_obj_is_valid(s_odo_edit_overlay)) {
        lv_obj_del(s_odo_edit_overlay);
    }
    s_odo_edit_overlay  = NULL;
    s_odo_edit_textarea = NULL;

    /* Close scan overlay (lives on lv_layer_top, not auto-deleted) */
    if (s_scan_overlay && lv_obj_is_valid(s_scan_overlay)) {
        lv_obj_del(s_scan_overlay);
    }

    /* Same for the web-URL QR modal */
    if (s_qr_overlay && lv_obj_is_valid(s_qr_overlay)) {
        lv_obj_del(s_qr_overlay);
    }
    s_qr_overlay = NULL;

    /* Cancel any running bus scan and detach UI callback */
    can_bus_test_set_ui_callback(NULL);
    if (can_bus_test_is_running()) {
        can_bus_test_cancel();
    }

    // NULL out all static LVGL pointers (screen is about to be deleted)
    wifi_status_label = NULL;
    web_status_label = NULL;
    ap_status_label = NULL;
    brightness_label = NULL;
    wifi_loading_dialog = NULL;
    s_can_health_dot = NULL;
    s_can_health_label = NULL;
    s_can_summary_label = NULL;
    s_can_details_grid = NULL;
    s_can_details_toggle = NULL;
    memset(s_can_detail_labels, 0, sizeof(s_can_detail_labels));
    s_scan_overlay = NULL;
    s_scan_title_label = NULL;
    s_scan_status_label = NULL;
    s_scan_bar = NULL;
    s_scan_progress_label = NULL;
    memset(s_scan_result_labels, 0, sizeof(s_scan_result_labels));
    s_scan_detail_label = NULL;
    s_scan_apply_btn = NULL;
    s_scan_close_btn = NULL;
    s_scan_cancel_btn = NULL;
    s_bitrate_dropdown = NULL;

    lv_obj_t * old_screen = (lv_obj_t *)lv_event_get_user_data(e);
    if (old_screen) {
        lv_scr_load(old_screen);
    }
}

/* Bitrate dropdown callback — save to NVS then apply via can_manager */
static void bitrate_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dd = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dd);

    config_store_save_bitrate((uint8_t)selected);

    /* Apply the new bitrate (stops task, reinits TWAI, restarts task) */
    can_change_bitrate((uint8_t)selected);
}

// Timer callback to show WiFi screen after loading dialog
static void show_wifi_screen_delayed(lv_timer_t* timer) {
    ESP_LOGI("dev_set", "[trace] show_wifi_screen_delayed ENTER");
    // Close loading dialog
    if (wifi_loading_dialog && lv_obj_is_valid(wifi_loading_dialog)) {
        lv_obj_del(wifi_loading_dialog);
        wifi_loading_dialog = NULL;
    }
    ESP_LOGI("dev_set", "[trace] about to call wifi_ui_show()");
    // Show WiFi screen
    wifi_ui_show();
    ESP_LOGI("dev_set", "[trace] wifi_ui_show returned");

    // Delete the timer
    lv_timer_del(timer);
}

// WiFi button callback
static void wifi_btn_event_cb(lv_event_t *e) {
    ESP_LOGI("dev_set", "[trace] wifi_btn_event_cb ENTER");
    // Guard against double-tap while loading dialog is already showing
    if (wifi_loading_dialog && lv_obj_is_valid(wifi_loading_dialog)) return;

    /* Loading dialog — centered with shadow */
    wifi_loading_dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_loading_dialog, 280, 140);
    lv_obj_align(wifi_loading_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(wifi_loading_dialog, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(wifi_loading_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wifi_loading_dialog, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(wifi_loading_dialog, 1, 0);
    lv_obj_set_style_radius(wifi_loading_dialog, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_shadow_width(wifi_loading_dialog, 20, 0);
    lv_obj_set_style_shadow_ofs_y(wifi_loading_dialog, 4, 0);
    lv_obj_set_style_shadow_color(wifi_loading_dialog, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(wifi_loading_dialog, 140, 0);
    lv_obj_set_style_pad_all(wifi_loading_dialog, 16, 0);
    lv_obj_clear_flag(wifi_loading_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(wifi_loading_dialog);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Wi-Fi Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);

    lv_obj_t* spinner = lv_spinner_create(wifi_loading_dialog, 1000, 60);
    lv_obj_set_size(spinner, 36, 36);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_SECTION_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);

    lv_obj_t* loading_text = lv_label_create(wifi_loading_dialog);
    lv_label_set_text(loading_text, "Searching for networks...");
    lv_obj_align(loading_text, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_color(loading_text, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(loading_text, THEME_FONT_SMALL, 0);
    
    // Create timer to show WiFi screen after a brief delay (allows dialog to render)
    lv_timer_create(show_wifi_screen_delayed, 100, NULL);
}

// Async callbacks — dispatched to LVGL thread after OTA check completes
static void _ota_show_update_available(void *param) {
    (void)param;
    show_ota_update_dialog(FIRMWARE_VERSION, get_latest_version(),
                           get_update_file_size_mb(),
                           get_release_notes());
}

static void _ota_show_up_to_date(void *param) {
    (void)param;
    show_ota_up_to_date_dialog(FIRMWARE_VERSION);
}

static void _ota_show_check_failed(void *param) {
    (void)param;
    show_ota_check_failed_dialog();
}

// OTA check task — runs off the LVGL thread so the UI stays responsive
static void _ota_check_task(void *param) {
    check_for_update();

    ota_status_t status = get_ota_status();
    if (status == OTA_UPDATE_AVAILABLE) {
        lv_async_call(_ota_show_update_available, NULL);
    } else if (status == OTA_NO_UPDATE_AVAILABLE) {
        lv_async_call(_ota_show_up_to_date, NULL);
    } else {
        lv_async_call(_ota_show_check_failed, NULL);
    }

    vTaskDelete(NULL);
}

/* Orphaned by the card-grid redesign (like the removed wire-input toggle): the
 * manual "Check Updates" button used to live in the now-deleted
 * _build_section_network, and nothing in the card grid replaced it. Lower
 * severity than the wire-input gap, though — OTA checking is still
 * reachable via the web editor (POST /api/ota/check, GET /api/ota/status
 * in web_server_ota.c), just not from the on-device touchscreen directly.
 * Kept compiled (not deleted) as a working reference if a "Check Updates"
 * card gets added back to the Device grid. */
__attribute__((unused))
static void update_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ESP_LOGI("OTA", "Check for updates button clicked");

    show_ota_checking_dialog();

    /* Allocate task memory: TCB in internal RAM (required), stack in PSRAM */
    static StaticTask_t s_ota_chk_tcb;
    static StackType_t *s_ota_chk_stack;
    const uint32_t OTA_CHK_STACK = 8192;

    if (!s_ota_chk_stack) {
        s_ota_chk_stack = heap_caps_calloc(OTA_CHK_STACK, sizeof(StackType_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_ota_chk_stack) {
        ESP_LOGE("OTA", "Failed to allocate OTA check stack");
        show_ota_check_failed_dialog();
        return;
    }

    xTaskCreateStaticPinnedToCore(_ota_check_task, "ota_chk", OTA_CHK_STACK,
                                  NULL, 3, s_ota_chk_stack, &s_ota_chk_tcb, 0);
}

/* ── Data Logging ─────────────────────────────────────────────────────── */

/* Raw CAN capture widgets — declared here so _update_log_ui can render
 * their state alongside the existing signal logger UI. Assigned by the
 * builder in _build_section_data_logging. */
static lv_obj_t *s_canraw_btn          = NULL;
static lv_obj_t *s_canraw_btn_label    = NULL;
static lv_obj_t *s_canraw_status_label = NULL;

/* Share Raw CAN modal — opens a small fullscreen overlay with two
 * textareas (Make + Model) and an on-screen keyboard. Tracks upload
 * status via can_upload_get_status() while running. */
static lv_obj_t  *s_share_overlay      = NULL;
static lv_obj_t  *s_share_file_label   = NULL;
static lv_obj_t  *s_share_make_ta      = NULL;
static lv_obj_t  *s_share_model_ta     = NULL;
static lv_obj_t  *s_share_status_lbl   = NULL;
static lv_obj_t  *s_share_upload_btn   = NULL;
static lv_obj_t  *s_share_cancel_btn   = NULL;
static lv_obj_t  *s_share_ok_btn       = NULL;
static lv_obj_t  *s_share_keyboard     = NULL;
static lv_timer_t *s_share_status_timer = NULL;
static char       s_share_picked_file[64] = {0};
static void _share_modal_open(void);
static void _share_modal_close(void);
static void _share_btn_cb(lv_event_t *e);

/* Forward decls for callbacks/helpers referenced by the popup builders
 * (defined further down the file). Card-grid migration moved these calls
 * earlier in source order — declaration here avoids implicit-function /
 * undeclared-identifier errors. */
static void _veh_odo_refresh_timer_cb(lv_timer_t *t);
static void _odo_edit_btn_cb(lv_event_t *e);
static void _veh_gear_btn_cb(lv_event_t *e);
static void _build_section_can_diagnostics(lv_obj_t *content);
static void refresh_can_diagnostics(void);

static void _update_log_ui(void) {
    if (!s_log_btn_label || !s_log_status_label) return;

    if (data_logger_is_active()) {
        lv_label_set_text(s_log_btn_label, "Stop Signal Log");
        lv_obj_set_style_bg_color(s_log_btn, THEME_COLOR_BTN_DANGER,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);

        uint32_t samples = data_logger_get_sample_count();
        uint32_t elapsed = data_logger_get_elapsed_ms();
        uint32_t secs = elapsed / 1000;
        uint32_t mins = secs / 60;
        secs %= 60;
        lv_label_set_text_fmt(s_log_status_label,
                              "Recording: %lu samples (%lum %lus, %s)",
                              (unsigned long)samples,
                              (unsigned long)mins, (unsigned long)secs,
                              data_logger_get_storage());
        lv_obj_set_style_text_color(s_log_status_label,
                                    THEME_COLOR_STATUS_CONNECTED,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_label_set_text(s_log_btn_label, "Start Signal Log");
        lv_obj_set_style_bg_color(s_log_btn, THEME_COLOR_BORDER,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);

        const char *file = data_logger_current_file();
        if (file[0] != '\0') {
            const char *basename = strrchr(file, '/');
            basename = basename ? basename + 1 : file;
            lv_label_set_text_fmt(s_log_status_label, "Stopped: %s", basename);
        } else {
            /* LFS fallback is always available now, so "no SD" is no longer
             * a blocking condition — just a tier hint. */
            lv_label_set_text(s_log_status_label,
                sd_manager_is_mounted() ? "Stopped (SD ready)"
                                        : "Stopped (flash, no SD)");
        }
        lv_obj_set_style_text_color(s_log_status_label,
                                    THEME_COLOR_TEXT_MUTED,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    /* Raw CAN capture mirrors the same UI pattern in its own button + label. */
    if (s_canraw_btn_label && s_canraw_status_label) {
        if (can_raw_logger_is_active()) {
            lv_label_set_text(s_canraw_btn_label, "Stop Raw CAN");
            lv_obj_set_style_bg_color(s_canraw_btn, THEME_COLOR_BTN_DANGER,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
            uint32_t frames  = can_raw_logger_frame_count();
            uint32_t elapsed = can_raw_logger_elapsed_ms();
            uint32_t secs = elapsed / 1000, mins = secs / 60; secs %= 60;
            lv_label_set_text_fmt(s_canraw_status_label,
                                  "Raw: %lu frames (%lum %lus, %s)",
                                  (unsigned long)frames,
                                  (unsigned long)mins, (unsigned long)secs,
                                  can_raw_logger_get_storage());
            lv_obj_set_style_text_color(s_canraw_status_label,
                                        THEME_COLOR_STATUS_CONNECTED,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_label_set_text(s_canraw_btn_label, "Start Raw CAN");
            lv_obj_set_style_bg_color(s_canraw_btn, THEME_COLOR_BORDER,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(s_canraw_status_label, "Raw: idle");
            lv_obj_set_style_text_color(s_canraw_status_label,
                                        THEME_COLOR_TEXT_MUTED,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void _log_status_timer_cb(lv_timer_t *timer) {
    (void)timer;
    _update_log_ui();
}

static void _log_toggle_btn_cb(lv_event_t *e) {
    (void)e;
    if (data_logger_is_active()) {
        data_logger_stop();
    } else {
        data_logger_start();
    }
    _update_log_ui();
}

static void _canraw_toggle_btn_cb(lv_event_t *e) {
    (void)e;
    if (can_raw_logger_is_active()) {
        can_raw_logger_stop();
    } else {
        can_raw_logger_start();
    }
    _update_log_ui();
}

/* ── Share Raw CAN modal ─────────────────────────────────────────────────
 *
 * Lets the user upload the most recent canraw_*.csv to the project cloud
 * bucket from on-device — without needing the web editor. Same backend as
 * the web editor flow (can_upload_start).
 *
 * Layout: a 760×460 overlay on lv_layer_top with:
 *   - file picked automatically (the newest canraw_*.csv on SD or LFS)
 *   - two textareas (Make + Model) wired to a shared LVGL keyboard
 *   - status label that polls can_upload_get_status() every 500 ms
 *   - Cancel + Upload buttons
 * ──────────────────────────────────────────────────────────────────────── */

/* Scan /sdcard/logs and /lfs/logs for canraw_*.csv files, return the newest
 * basename in out_buf. Returns true if anything was found. */
static bool _share_find_latest_canraw(char *out_buf, size_t out_len) {
    const char *dirs[] = { "/sdcard/logs", "/lfs/logs" };
    char best_name[64] = {0};
    time_t best_mtime = 0;

    for (int d = 0; d < (int)(sizeof(dirs) / sizeof(dirs[0])); d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "canraw_", 7) != 0) continue;
            size_t nlen = strlen(ent->d_name);
            if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".csv") != 0) continue;

            char path[160];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            if (st.st_mtime > best_mtime) {
                best_mtime = st.st_mtime;
                strncpy(best_name, ent->d_name, sizeof(best_name) - 1);
                best_name[sizeof(best_name) - 1] = '\0';
            }
        }
        closedir(dir);
    }

    if (best_name[0] == '\0') return false;
    strncpy(out_buf, best_name, out_len - 1);
    out_buf[out_len - 1] = '\0';
    return true;
}

static void _share_status_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_share_status_lbl || !lv_obj_is_valid(s_share_status_lbl)) return;
    can_upload_status_t st;
    can_upload_get_status(&st);
    lv_label_set_text(s_share_status_lbl, st.message[0] ? st.message : "Idle");
    if (st.state == CAN_UPLOAD_SUCCESS) {
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_STATUS_CONNECTED, 0);
        /* Swap Cancel + Upload for a single OK button — once the trace is up,
         * the only meaningful action is closing the modal back to settings. */
        if (s_share_cancel_btn && lv_obj_is_valid(s_share_cancel_btn))
            lv_obj_add_flag(s_share_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_share_upload_btn && lv_obj_is_valid(s_share_upload_btn))
            lv_obj_add_flag(s_share_upload_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_share_ok_btn && lv_obj_is_valid(s_share_ok_btn))
            lv_obj_clear_flag(s_share_ok_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_share_status_timer) { lv_timer_del(s_share_status_timer); s_share_status_timer = NULL; }
    } else if (st.state == CAN_UPLOAD_FAILED) {
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_BTN_DANGER, 0);
        if (s_share_upload_btn && lv_obj_is_valid(s_share_upload_btn))
            lv_obj_clear_state(s_share_upload_btn, LV_STATE_DISABLED);
        if (s_share_status_timer) { lv_timer_del(s_share_status_timer); s_share_status_timer = NULL; }
    } else {
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_TEXT_MUTED, 0);
    }
}

static void _share_ta_focused_cb(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_share_keyboard && lv_obj_is_valid(s_share_keyboard))
        lv_keyboard_set_textarea(s_share_keyboard, ta);
}

static void _share_upload_btn_cb(lv_event_t *e) {
    (void)e;
    if (!s_share_make_ta || !s_share_model_ta || !s_share_status_lbl) return;

    const char *make  = lv_textarea_get_text(s_share_make_ta);
    const char *model = lv_textarea_get_text(s_share_model_ta);

    if (!make || !make[0]) {
        lv_label_set_text(s_share_status_lbl, "Enter the car make first");
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_BTN_DANGER, 0);
        return;
    }
    if (!model || !model[0]) {
        lv_label_set_text(s_share_status_lbl, "Enter the car model first");
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_BTN_DANGER, 0);
        return;
    }
    if (s_share_picked_file[0] == '\0') {
        lv_label_set_text(s_share_status_lbl, "No Raw CAN recording found");
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_BTN_DANGER, 0);
        return;
    }

    lv_obj_add_state(s_share_upload_btn, LV_STATE_DISABLED);
    esp_err_t err = can_upload_start(s_share_picked_file, make, model, NULL);
    if (err != ESP_OK) {
        lv_label_set_text_fmt(s_share_status_lbl, "Start failed (%s)",
                              err == ESP_ERR_INVALID_STATE ? "another upload running"
                                                           : "internal error");
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_BTN_DANGER, 0);
        lv_obj_clear_state(s_share_upload_btn, LV_STATE_DISABLED);
        return;
    }

    /* Begin polling status every 500 ms — _share_status_timer_cb will
       update the label and re-enable the button on terminal state. */
    if (s_share_status_timer) { lv_timer_del(s_share_status_timer); }
    s_share_status_timer = lv_timer_create(_share_status_timer_cb, 500, NULL);
}

static void _share_close_btn_cb(lv_event_t *e) {
    (void)e;
    _share_modal_close();
}

static void _share_modal_close(void) {
    if (s_share_status_timer) { lv_timer_del(s_share_status_timer); s_share_status_timer = NULL; }
    if (s_share_overlay && lv_obj_is_valid(s_share_overlay)) lv_obj_del(s_share_overlay);
    s_share_overlay      = NULL;
    s_share_file_label   = NULL;
    s_share_make_ta      = NULL;
    s_share_model_ta     = NULL;
    s_share_status_lbl   = NULL;
    s_share_upload_btn   = NULL;
    s_share_cancel_btn   = NULL;
    s_share_ok_btn       = NULL;
    s_share_keyboard     = NULL;
}

static void _share_modal_open(void) {
    if (s_share_overlay && lv_obj_is_valid(s_share_overlay)) return;

    /* Find the latest canraw file up front so we can either show its name
       or fail fast with a clear message. */
    bool have_file = _share_find_latest_canraw(s_share_picked_file,
                                               sizeof(s_share_picked_file));

    s_share_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_share_overlay, 760, 460);
    lv_obj_center(s_share_overlay);
    lv_obj_set_style_bg_color(s_share_overlay, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_share_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_share_overlay, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_border_color(s_share_overlay, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_border_width(s_share_overlay, 1, 0);
    lv_obj_set_style_pad_all(s_share_overlay, 14, 0);
    lv_obj_clear_flag(s_share_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_share_overlay);
    lv_label_set_text(title, "Share Raw CAN Trace");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *hint = lv_label_create(s_share_overlay);
    lv_label_set_text(hint,
        "Uploads the latest Raw CAN recording tagged with your car so it can be debugged off-device.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, 720);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_set_style_text_font(hint, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_MUTED, 0);

    s_share_file_label = lv_label_create(s_share_overlay);
    if (have_file) {
        lv_label_set_text_fmt(s_share_file_label, "Recording: %s", s_share_picked_file);
        lv_obj_set_style_text_color(s_share_file_label, THEME_COLOR_TEXT_PRIMARY, 0);
    } else {
        lv_label_set_text(s_share_file_label,
            "No Raw CAN recording found - start one first.");
        lv_obj_set_style_text_color(s_share_file_label, THEME_COLOR_BTN_DANGER, 0);
    }
    lv_obj_align(s_share_file_label, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_text_font(s_share_file_label, THEME_FONT_SMALL, 0);

    /* Two textareas side by side */
    lv_obj_t *make_lbl = lv_label_create(s_share_overlay);
    lv_label_set_text(make_lbl, "Car Make");
    lv_obj_align(make_lbl, LV_ALIGN_TOP_LEFT, 0, 88);
    lv_obj_set_style_text_font(make_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(make_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s_share_make_ta = lv_textarea_create(s_share_overlay);
    lv_obj_set_size(s_share_make_ta, 340, 36);
    lv_obj_align(s_share_make_ta, LV_ALIGN_TOP_LEFT, 0, 108);
    lv_textarea_set_one_line(s_share_make_ta, true);
    lv_textarea_set_max_length(s_share_make_ta, 40);
    lv_textarea_set_placeholder_text(s_share_make_ta, "Toyota");
    lv_obj_set_style_text_font(s_share_make_ta, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(s_share_make_ta, _share_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *model_lbl = lv_label_create(s_share_overlay);
    lv_label_set_text(model_lbl, "Model and Year");
    lv_obj_align(model_lbl, LV_ALIGN_TOP_LEFT, 360, 88);
    lv_obj_set_style_text_font(model_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(model_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s_share_model_ta = lv_textarea_create(s_share_overlay);
    lv_obj_set_size(s_share_model_ta, 360, 36);
    lv_obj_align(s_share_model_ta, LV_ALIGN_TOP_LEFT, 360, 108);
    lv_textarea_set_one_line(s_share_model_ta, true);
    lv_textarea_set_max_length(s_share_model_ta, 40);
    lv_textarea_set_placeholder_text(s_share_model_ta, "Supra MK4 1998");
    lv_obj_set_style_text_font(s_share_model_ta, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(s_share_model_ta, _share_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    s_share_status_lbl = lv_label_create(s_share_overlay);
    lv_label_set_text(s_share_status_lbl, "");
    lv_obj_align(s_share_status_lbl, LV_ALIGN_TOP_LEFT, 0, 156);
    lv_obj_set_style_text_font(s_share_status_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_TEXT_MUTED, 0);

    /* On-screen keyboard, anchored to the bottom — fills the lower half */
    s_share_keyboard = lv_keyboard_create(s_share_overlay);
    lv_obj_set_size(s_share_keyboard, 720, 200);
    lv_obj_align(s_share_keyboard, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_keyboard_set_mode(s_share_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    /* Buttons row at the very bottom — Cancel (left) and Upload (right)
     * during the entry/in-progress phases; on success they're hidden and
     * the OK button (same right slot) takes over. */
    s_share_cancel_btn = lv_btn_create(s_share_overlay);
    lv_obj_set_size(s_share_cancel_btn, 110, 36);
    lv_obj_align(s_share_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_share_cancel_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_width(s_share_cancel_btn, 1, 0);
    lv_obj_set_style_border_color(s_share_cancel_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_radius(s_share_cancel_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(s_share_cancel_btn, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(s_share_cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(cancel_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(s_share_cancel_btn, _share_close_btn_cb, LV_EVENT_CLICKED, NULL);

    s_share_upload_btn = lv_btn_create(s_share_overlay);
    lv_obj_set_size(s_share_upload_btn, 130, 36);
    lv_obj_align(s_share_upload_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_share_upload_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(s_share_upload_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(s_share_upload_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_share_upload_btn, 0, 0);
    lv_obj_t *upload_lbl = lv_label_create(s_share_upload_btn);
    lv_label_set_text(upload_lbl, "Upload");
    lv_obj_center(upload_lbl);
    lv_obj_set_style_text_font(upload_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(upload_lbl, lv_color_white(), 0);
    lv_obj_add_event_cb(s_share_upload_btn, _share_upload_btn_cb, LV_EVENT_CLICKED, NULL);

    /* OK button — replaces Upload + Cancel after a successful upload so the
     * user has a single obvious "I'm done" affordance back to Device Settings.
     * Reuses _share_close_btn_cb (same teardown path). */
    s_share_ok_btn = lv_btn_create(s_share_overlay);
    lv_obj_set_size(s_share_ok_btn, 130, 36);
    lv_obj_align(s_share_ok_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_share_ok_btn, THEME_COLOR_STATUS_CONNECTED, 0);
    lv_obj_set_style_radius(s_share_ok_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(s_share_ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_share_ok_btn, 0, 0);
    lv_obj_t *ok_lbl = lv_label_create(s_share_ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_center(ok_lbl);
    lv_obj_set_style_text_font(ok_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_white(), 0);
    lv_obj_add_event_cb(s_share_ok_btn, _share_close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_share_ok_btn, LV_OBJ_FLAG_HIDDEN);

    if (!have_file) {
        lv_obj_add_state(s_share_upload_btn, LV_STATE_DISABLED);
    }
}

/* Wipe peak/min for every signal in the registry. Affects all panels with
 * show_peak set (they pull current peak/min on the next signal update). */
/* DEVICE-grid "Simulator" card tap handler. Tapping the card toggles the
 * signal simulator and repaints the card's stat label (ON green / OFF blue).
 * Standalone card so the demo sweep is a top-level action, not buried in the
 * Peak Hold popup. */
static void _sim_card_cb(lv_event_t *e) {
    (void)e;
    if (signal_sim_is_active()) {
        signal_sim_stop();
    } else {
        signal_sim_start();
    }
    if (s_sim_card_stat && lv_obj_is_valid(s_sim_card_stat)) {
        bool on = signal_sim_is_active();
        lv_label_set_text(s_sim_card_stat, on ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_sim_card_stat,
            on ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_ACCENT, 0);
    }
}

static void _reset_peaks_btn_cb(lv_event_t *e) {
    (void)e;
    signal_reset_peaks();
}

/* Open the Signal Peaks live-table screen. Same defer-then-show pattern as
 * the diagnostics launcher so the underlying screen has already loaded by
 * the time peaks_ui_show() flips screens. */
static void _show_peaks_async(void *arg) {
    (void)arg;
    peaks_ui_show();
}

static void _view_peaks_btn_cb(lv_event_t *e) {
    (void)e;
    /* Close the host popup before peaks_ui takes over the screen — popups
     * live on lv_layer_top() so they'd otherwise float above the peaks
     * screen. peaks_ui captures lv_scr_act() as its return screen at call
     * time, so leaving settings_screen active means Back returns to Device
     * Settings (not the dashboard).
     *
     * The button now lives in the Live Data & Logging popup (ADR-0030), so
     * that is the one to close; _testing_popup_close stays because the old
     * Testing overlay pointer is still torn down on settings teardown. */
    _logger_popup_close(NULL);
    _testing_popup_close(NULL);
    lv_async_call(_show_peaks_async, NULL);
}

/* Map dropdown index ↔ rate Hz. Order MUST match the static options string
 * passed to lv_dropdown_set_options_static in the build code below:
 *   0=1, 1=2, 2=5, 3=10, 4=20, 5=50, 6=100, 7=200, 8=Max(0). */
static uint16_t _log_rate_idx_to_hz(uint16_t idx) {
    static const uint16_t table[] = {1, 2, 5, 10, 20, 50, 100, 200, 0};
    if (idx >= sizeof(table) / sizeof(table[0])) idx = 3; /* default 10Hz */
    return table[idx];
}
static uint16_t _log_rate_hz_to_idx(uint16_t hz) {
    switch (hz) {
        case 1:   return 0;
        case 2:   return 1;
        case 5:   return 2;
        case 10:  return 3;
        case 20:  return 4;
        case 50:  return 5;
        case 100: return 6;
        case 200: return 7;
        case 0:   return 8;  /* Max */
        default:  return 3;  /* unknown values fall back to 10Hz */
    }
}

static void _log_rate_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t idx = lv_dropdown_get_selected(dd);
    data_logger_set_rate_hz(_log_rate_idx_to_hz(idx));
}

/* ── System Diagnostics launcher ──────────────────────────────────────── */

static void _show_diag_async(void *arg) {
    (void)arg;
    diagnostics_ui_show();
}

static void _diag_btn_cb(lv_event_t *e) {
    (void)e;
    /* Same pattern as the wizard launcher: drop back to the dashboard so the
     * diagnostics screen has a clean backdrop, then show on the next tick so
     * the screen-load fully commits before we paint. */
    lv_obj_t *ret = device_settings_return_screen;
    if (ret && lv_obj_is_valid(ret)) {
        lv_scr_load(ret);
    }
    lv_async_call(_show_diag_async, NULL);
}

/* ── Re-run First-Run Wizard ──────────────────────────────────────────── */

static void _show_wizard_async(void *arg) {
    (void)arg;
    show_first_run_wizard();
}

/* Drop back to the dashboard then show the wizard overlay on the next
 * LVGL tick. The wizard's show_first_run_wizard() is reentrant — it
 * builds a fresh overlay on the active screen, so the saved network
 * and ECU settings remain untouched (the wizard surfaces current values
 * and lets the user re-confirm or change them). */
static void _run_wizard_now(void) {
    lv_obj_t *ret = device_settings_return_screen;
    if (ret && lv_obj_is_valid(ret)) {
        lv_scr_load(ret);
    }
    /* Defer to next LVGL tick so the screen load fully commits first */
    lv_async_call(_show_wizard_async, NULL);
}

static void _run_wizard_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn_txt = lv_msgbox_get_active_btn_text(mbox);
    if (!btn_txt) return;

    if (strcmp(btn_txt, "Run Wizard") == 0) {
        ESP_LOGI("WIZARD", "User confirmed re-run setup wizard");
        lv_msgbox_close(mbox);
        _run_wizard_now();
        return;
    }
    /* Cancel — just close the dialog */
    lv_msgbox_close(mbox);
}

static void _run_wizard_btn_cb(lv_event_t *e) {
    (void)e;
    static const char *btns[] = {"Run Wizard", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(
        NULL,
        "Re-run Setup Wizard",
        "This will re-launch the setup wizard:\n"
        "CAN auto-detect, Wi-Fi, and ECU pick.\n\n"
        "Your saved network and ECU settings\n"
        "will be preserved. Continue?",
        btns, true);

    lv_obj_set_style_bg_color(mbox, THEME_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, THEME_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(mbox, THEME_RADIUS_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(mbox, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mbox, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(mbox, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(mbox, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(mbox, 80, LV_PART_MAIN);
    lv_obj_set_width(mbox, 380);
    lv_obj_center(mbox);

    /* Style the buttons — neutral, this is a non-destructive op */
    lv_obj_t *btn_area = lv_msgbox_get_btns(mbox);
    lv_obj_set_style_bg_color(btn_area, THEME_COLOR_SECTION_BG,
                              LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn_area, THEME_COLOR_TEXT_PRIMARY,
                                LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_area, 1,
                                  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn_area, THEME_COLOR_BORDER,
                                  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_area, THEME_RADIUS_NORMAL,
                            LV_PART_ITEMS | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(mbox, _run_wizard_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ── ECU selection — REMOVED ──────────────────────────────────────────
 * The standalone ECU-preset picker (ui_ecu_picker.c) is retired. ECU
 * presets now bind per-channel through the Channels editor's source
 * picker, and bulk ECU auto-detect lives in the setup wizard. The old
 * _ecu_label_compose / _deferred_reload_after_ecu / _ecu_picker_done_cb /
 * _ecu_btn_cb helpers went with it. ───────────────────────────────────── */

/* ── OBD2 PIDs button ────────────────────────────────────────────────── */

static lv_obj_t *s_obd2_btn_label = NULL;

/* Compose the button label based on current state:
 *   "Choose PIDs..."   (none polled)
 *   "N polled"         (some polled)
 *
 * Reads from the in-memory poll state, NOT the layout JSON on disk.
 * Earlier versions called ecu_preset_read_obd2_pids() which fopen()ed
 * the layout file every time Device Settings opened — and fopen needs
 * an internal-RAM-only newlib lock that can fail with abort() when
 * internal SRAM is fragmented (esp. after WiFi init + dashboard widgets
 * load). obd2_get_enabled() returns the same count from zero-allocation
 * static memory. */
static void _obd2_label_compose(char *buf, size_t n)
{
    uint32_t pids[OBD2_MAX_ENABLED];
    uint8_t count = obd2_get_enabled(pids, OBD2_MAX_ENABLED);
    if (count == 0) {
        snprintf(buf, n, "Choose PIDs...");
    } else {
        snprintf(buf, n, "%u polled", count);
    }
}

static void _obd2_btn_cb(lv_event_t *e)
{
    (void)e;
    obd2_picker_open();
}

static void _dtc_btn_cb(lv_event_t *e)
{
    (void)e;
    dtc_reader_open();
}

/* Vehicle identity caches — both VIN and ECU name come from Mode 09
 * and don't change for the life of the dash mount. First successful
 * read populates the cache and subsequent Settings opens repaint from
 * cache instantly. Empty = "not fetched yet"; "Unknown" = "tried but
 * ECU didn't answer or rejected". */
static char       s_vin_cache[20]      = {0};
static char       s_ecuname_cache[24]  = {0};
static lv_obj_t  *s_vin_label_obj      = NULL;
static lv_obj_t  *s_ecuname_label_obj  = NULL;

/* Card-grid popup overlays — all parented to lv_layer_top() like the share
 * modal. Settings-screen delete cb defensively closes each one (the popup
 * bodies hold the live label pointers that timers paint into; closing
 * NULLs them so we don't dangle past the settings screen's lifetime). */
static lv_obj_t  *s_device_info_overlay   = NULL;
static lv_obj_t  *s_dimmer_overlay        = NULL;
static lv_obj_t  *s_logger_overlay        = NULL;
static lv_obj_t  *s_testing_overlay       = NULL;
static lv_obj_t  *s_can_bus_overlay       = NULL;
static lv_obj_t  *s_odo_overlay           = NULL;

static void _vin_done(bool ok, const char *vin, void *user)
{
    (void)user;
    if (ok && vin && vin[0]) {
        strncpy(s_vin_cache, vin, sizeof(s_vin_cache) - 1);
        s_vin_cache[sizeof(s_vin_cache) - 1] = '\0';
    } else if (!s_vin_cache[0]) {
        strncpy(s_vin_cache, "Unknown", sizeof(s_vin_cache) - 1);
    }
    if (s_vin_label_obj && lv_obj_is_valid(s_vin_label_obj)) {
        lv_label_set_text(s_vin_label_obj, s_vin_cache);
    }
}

static void _ecuname_done(bool ok, const char *name, void *user)
{
    (void)user;
    if (ok && name && name[0]) {
        strncpy(s_ecuname_cache, name, sizeof(s_ecuname_cache) - 1);
        s_ecuname_cache[sizeof(s_ecuname_cache) - 1] = '\0';
    } else if (!s_ecuname_cache[0]) {
        strncpy(s_ecuname_cache, "Unknown", sizeof(s_ecuname_cache) - 1);
    }
    if (s_ecuname_label_obj && lv_obj_is_valid(s_ecuname_label_obj)) {
        lv_label_set_text(s_ecuname_label_obj, s_ecuname_cache);
    }
}

/* ── Reset Default Layout (layout-only, NOT factory reset) ───────────── */

static void _layout_reset_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn_txt = lv_msgbox_get_active_btn_text(mbox);
    if (!btn_txt) return;

    if (strcmp(btn_txt, "RESET") == 0) {
        ESP_LOGW("RESET", "User confirmed default-layout reset");
        if (generate_default_layout() == ESP_OK) {
            /* Re-apply the remembered ECU preset so the fresh default
             * comes back with live bindings. */
            char make[32] = {0}, ver[32] = {0};
            if (config_store_load_ecu(make, sizeof(make), ver, sizeof(ver)) == ESP_OK &&
                make[0] && ver[0]) {
                const ecu_preset_t *p = ecu_preset_find(make, ver);
                if (p) ecu_preset_apply_to_layout("default", p);
            }
            layout_manager_bump_version();

            /* If "default" is the active dash, show the result immediately —
             * rebuild Screen3 and leave settings (we ARE the LVGL task here).
             * The settings screen's LV_EVENT_DELETE cleanup tears down its
             * timers, same as every other exit path. */
            char active[LAYOUT_MAX_NAME];
            layout_manager_get_active(active, sizeof(active));
            if (strcmp(active, "default") == 0) {
                lv_msgbox_close(mbox);
                lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
                ui_Screen3_screen_init();
                lv_scr_load(ui_Screen3);
                if (old && old != ui_Screen3 && lv_obj_is_valid(old))
                    lv_obj_del(old);
                return;
            }
        }
    }
    lv_msgbox_close(mbox);
}

static void _layout_reset_btn_cb(lv_event_t *e) {
    (void)e;
    static const char *btns[] = {"RESET", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(
        NULL,
        "Reset Default Layout",
        "Restores the factory \"default\" layout.\n"
        "Your other layouts and your channel\n"
        "setup are not touched.",
        btns, true);

    lv_obj_set_style_bg_color(mbox, THEME_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, THEME_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(mbox, THEME_RADIUS_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(mbox, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_width(mbox, 380);
    lv_obj_center(mbox);

    lv_obj_t *btn_area = lv_msgbox_get_btns(mbox);
    lv_obj_set_style_bg_color(btn_area, THEME_COLOR_SECTION_BG,
                              LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn_area, THEME_COLOR_STATUS_ERROR,
                                LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_area, 1,
                                  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn_area, THEME_COLOR_BORDER,
                                  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_area, THEME_RADIUS_NORMAL,
                            LV_PART_ITEMS | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(mbox, _layout_reset_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ── Factory Reset ────────────────────────────────────────────────────── */

static void _factory_reset_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn_txt = lv_msgbox_get_active_btn_text(mbox);
    if (!btn_txt) return;

    if (strcmp(btn_txt, "RESET") == 0) {
        ESP_LOGW("RESET", "User confirmed factory reset");
        config_store_factory_reset();
        esp_restart();
    }
    /* Cancel — just close the dialog */
    lv_msgbox_close(mbox);
}

static void _factory_reset_btn_cb(lv_event_t *e) {
    (void)e;
    static const char *btns[] = {"RESET", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(
        NULL,
        "Factory Reset",
        "This will erase ALL settings, layouts,\n"
        "images, fonts, and custom presets.\n\n"
        "The device will reboot with defaults.",
        btns, true);

    lv_obj_set_style_bg_color(mbox, THEME_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, THEME_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(mbox, THEME_RADIUS_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(mbox, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mbox, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(mbox, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(mbox, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(mbox, 80, LV_PART_MAIN);
    lv_obj_set_width(mbox, 380);
    lv_obj_center(mbox);

    /* Style the RESET button — red text on neutral bg */
    lv_obj_t *btn_area = lv_msgbox_get_btns(mbox);
    lv_obj_set_style_bg_color(btn_area, THEME_COLOR_SECTION_BG,
                              LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn_area, THEME_COLOR_STATUS_ERROR,
                                LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_area, 1,
                                  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn_area, THEME_COLOR_BORDER,
                                  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_area, THEME_RADIUS_NORMAL,
                            LV_PART_ITEMS | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(mbox, _factory_reset_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* Wire Input Mode software toggle removed 2026-07-05: the UART1(programming) /
 * UART2(indicators) routing on GPIO 43/44 is now a PHYSICAL hardware switch, so
 * the on-device software toggle is obsolete. The wire-input runtime itself
 * (io/wire_inputs.c reading the pins, main.c wiring, the config_store flag) is
 * left intact. */

void init_display_brightness(void) {
    // Always boot at 100% brightness
    set_display_brightness(100);
    load_dimmer_config_from_nvs();
}

// Save dimmer config to NVS
void save_dimmer_config_to_nvs(void) {
    config_store_save_dimmer(&dimmer_config);
    ESP_LOGI("DIMMER", "Dimmer config saved to NVS");
}

// Load dimmer config from NVS
void load_dimmer_config_from_nvs(void) {
    config_store_load_dimmer(&dimmer_config);
    ESP_LOGI("DIMMER", "Dimmer config loaded: Signal='%s', Thresh=%.2f, Enabled=%d",
        dimmer_config.signal_name, dimmer_config.threshold, dimmer_config.enabled);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Section builder helpers — called from device_settings_with_return_screen().
 * Each accepts a parent LVGL object and appends its children to it.
 * Module-level static pointers are set as a side-effect where needed.
 * ─────────────────────────────────────────────────────────────────────────── */

static lv_obj_t *_make_flex_section(lv_obj_t *row) {
    lv_obj_t *s = lv_obj_create(row);
    lv_obj_set_size(s, 0, lv_pct(100));
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_style_bg_color(s, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_color(s, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s, 1, 0);
    lv_obj_set_style_pad_all(s, 12, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static void _make_section_title(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
}

/* =========================================================================
 * Setup card grid — mirrors the web editor's Setup-mode panel look (section
 * title + grid of clickable card tiles, each opening a detail popup). Used
 * to consolidate the old _build_section_* rows into a scannable launcher.
 *
 * Grid geometry: 3 cols x 232px-wide cards, 12px gaps, wraps as needed.
 * Card geometry: 232 x 110, pad 12, optional LV_SYMBOL_* icon, title (BODY
 * font), wrapped body line (TINY font, muted), accent-blue stat (TINY,
 * uppercase, tracked).
 *
 * Sized to fit inside the 760-wide settings modal's content area at 720
 * effective inner width (3*232 + 2*12 = 720).
 * ========================================================================= */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *body_label;
    lv_obj_t *stat_label;
} setup_card_t;

static lv_obj_t *_make_setup_section_title(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, 720);
    lv_obj_set_style_text_font(lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 4, 0);
    return lbl;
}

static lv_obj_t *_make_setup_grid(lv_obj_t *parent) {
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_set_size(grid, 720, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    return grid;
}

/* Build a single setup card. icon_sym is an optional LV_SYMBOL_* string
 * (rendered accent-blue via label recolor) or NULL to skip. title/body/
 * initial_stat may each be NULL. on_click is wired as LV_EVENT_CLICKED.
 * The returned struct lets callers stash pointers for live timer updates
 * (e.g. point s_wifi_status_timer at the stat_label). */
static setup_card_t _make_setup_card(lv_obj_t *grid,
                                     const char *icon_sym,
                                     const char *title,
                                     const char *body,
                                     const char *initial_stat,
                                     lv_event_cb_t on_click) {
    setup_card_t r = {0};
    r.card = lv_btn_create(grid);
    lv_obj_set_size(r.card, 232, 110);
    lv_obj_set_style_bg_color(r.card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(r.card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r.card, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_border_color(r.card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(r.card, 1, 0);
    lv_obj_set_style_border_color(r.card, THEME_COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(r.card, 0, 0);
    lv_obj_set_style_pad_all(r.card, 12, 0);
    lv_obj_clear_flag(r.card, LV_OBJ_FLAG_SCROLLABLE);
    if (on_click) lv_obj_add_event_cb(r.card, on_click, LV_EVENT_CLICKED, NULL);

    /* Title — supports LVGL inline recolor so the icon can be accent-blue
     * while the title text stays primary. */
    r.title_label = lv_label_create(r.card);
    lv_label_set_recolor(r.title_label, true);
    if (icon_sym && icon_sym[0]) {
        char buf[96];
        snprintf(buf, sizeof(buf), "#2196F3 %s#  %s",
                 icon_sym, title ? title : "");
        lv_label_set_text(r.title_label, buf);
    } else {
        lv_label_set_text(r.title_label, title ? title : "");
    }
    lv_obj_align(r.title_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(r.title_label, THEME_FONT_BODY, 0);
    lv_obj_set_style_text_color(r.title_label, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Body — small, muted, wraps to up to 2 lines within the 208px card
     * inner width (232 - 2*12 pad = 208). */
    r.body_label = lv_label_create(r.card);
    lv_label_set_long_mode(r.body_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(r.body_label, body ? body : "");
    lv_obj_set_width(r.body_label, 208);
    lv_obj_align(r.body_label, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_set_style_text_font(r.body_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(r.body_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_line_space(r.body_label, 2, 0);

    /* Stat — accent-blue, tiny, uppercase tracked. Anchored bottom-left so
     * variable-length body copy above can flex. */
    r.stat_label = lv_label_create(r.card);
    lv_label_set_text(r.stat_label, initial_stat ? initial_stat : "");
    lv_obj_align(r.stat_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_text_font(r.stat_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(r.stat_label, THEME_COLOR_ACCENT, 0);
    lv_obj_set_style_text_letter_space(r.stat_label, 2, 0);
    return r;
}

static lv_obj_t *_build_content_area(lv_obj_t *parent) {
    lv_obj_t *content = lv_obj_create(parent);
    lv_obj_set_size(content, lv_pct(100), 388);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_opa(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, THEME_PAD_NORMAL, 0);
    lv_obj_set_style_pad_right(content, 20, 0);
    lv_obj_set_style_pad_row(content, THEME_PAD_SMALL, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(content, THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, 150, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(content, THEME_RADIUS_SMALL, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    return content;
}

static void _build_header(lv_obj_t *parent) {
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, lv_pct(100), 44);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Device Settings");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 15, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 60, 28);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);
    lv_obj_set_style_text_font(close_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(close_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(close_btn, close_menu_event_cb, LV_EVENT_CLICKED,
                        device_settings_return_screen);
}

/* Build a centered popup shell: w x h surface on lv_layer_top(), title in
 * the top-left, "Close" button in the top-right wired to close_cb. Returns
 * the overlay; callers position child widgets with absolute coords from
 * (0, 56) — leaving the top 56 px free for the title bar. */
static lv_obj_t *_make_popup_shell(int w, int h, const char *title,
                                   lv_event_cb_t close_cb) {
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, w, h);
    lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(overlay, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_border_color(overlay, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_border_width(overlay, 1, 0);
    lv_obj_set_style_pad_all(overlay, 18, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(overlay);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(t, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(t, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *close_btn = lv_btn_create(overlay);
    lv_obj_set_size(close_btn, 60, 28);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_radius(close_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_border_color(close_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_set_style_text_font(cl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(cl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, NULL);

    return overlay;
}

/* Tear down the Device Info popup. Safe to call when nothing is open. Also
 * invoked from _settings_screen_delete_cb so the popup doesn't outlive its
 * parent settings screen (the VIN/ECU label statics would dangle otherwise). */
static void _device_info_popup_close(lv_event_t *e) {
    (void)e;
    if (s_device_info_overlay && lv_obj_is_valid(s_device_info_overlay)) {
        lv_obj_del(s_device_info_overlay);
    }
    s_device_info_overlay = NULL;
    /* The label statics live inside the overlay — null them now so any in-
     * flight obd2 callback NULL-checks before painting (helpers already do). */
    s_vin_label_obj      = NULL;
    s_ecuname_label_obj  = NULL;
}

/* Open the Device Info popup. Mirrors the content of the old DEVICE INFO
 * card section but presented in a clean centered modal with a Close button.
 * Re-uses the existing VIN / ECU-name OBD2 read callbacks + caches. */
static void _device_info_popup_open(lv_event_t *e) {
    (void)e;
    if (s_device_info_overlay && lv_obj_is_valid(s_device_info_overlay)) return;

    s_device_info_overlay = _make_popup_shell(460, 340, "Device Info",
                                              _device_info_popup_close);

    /* Four info rows: Serial / Firmware / VIN / ECU. Each row is a
     * stacked hint-label + primary-value pair, 48 px tall. */
    int y = 56;

    /* Serial */
    lv_obj_t *l1 = lv_label_create(s_device_info_overlay);
    lv_label_set_text(l1, "Serial Number");
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_text_font(l1, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(l1, THEME_COLOR_TEXT_HINT, 0);
    lv_obj_t *v1 = lv_label_create(s_device_info_overlay);
    char serial[MAX_SERIAL_LENGTH];
    if (get_device_serial(serial) == ESP_OK) {
        lv_label_set_text(v1, serial);
    } else {
        lv_label_set_text(v1, "Unknown");
    }
    lv_obj_align(v1, LV_ALIGN_TOP_LEFT, 0, y + 16);
    lv_obj_set_style_text_font(v1, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(v1, THEME_COLOR_TEXT_PRIMARY, 0);
    y += 48;

    /* Firmware */
    lv_obj_t *l2 = lv_label_create(s_device_info_overlay);
    lv_label_set_text(l2, "Firmware");
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_text_font(l2, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(l2, THEME_COLOR_TEXT_HINT, 0);
    lv_obj_t *v2 = lv_label_create(s_device_info_overlay);
    lv_label_set_text(v2, FIRMWARE_VERSION);
    lv_obj_align(v2, LV_ALIGN_TOP_LEFT, 0, y + 16);
    lv_obj_set_style_text_font(v2, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(v2, THEME_COLOR_TEXT_PRIMARY, 0);
    y += 48;

    /* VIN (Mode 09 PID 0x02, cached statically) */
    lv_obj_t *l3 = lv_label_create(s_device_info_overlay);
    lv_label_set_text(l3, "VIN");
    lv_obj_align(l3, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_text_font(l3, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(l3, THEME_COLOR_TEXT_HINT, 0);
    s_vin_label_obj = lv_label_create(s_device_info_overlay);
    lv_obj_align(s_vin_label_obj, LV_ALIGN_TOP_LEFT, 0, y + 16);
    lv_obj_set_style_text_font(s_vin_label_obj, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_vin_label_obj, THEME_COLOR_TEXT_PRIMARY, 0);
    if (s_vin_cache[0]) {
        lv_label_set_text(s_vin_label_obj, s_vin_cache);
    } else {
        lv_label_set_text(s_vin_label_obj, "Reading...");
        obd2_read_vin(_vin_done, NULL);
    }
    y += 48;

    /* ECU Name (Mode 09 PID 0x0A, cached statically) */
    lv_obj_t *l4 = lv_label_create(s_device_info_overlay);
    lv_label_set_text(l4, "ECU");
    lv_obj_align(l4, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_text_font(l4, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(l4, THEME_COLOR_TEXT_HINT, 0);
    s_ecuname_label_obj = lv_label_create(s_device_info_overlay);
    lv_obj_align(s_ecuname_label_obj, LV_ALIGN_TOP_LEFT, 0, y + 16);
    lv_obj_set_style_text_font(s_ecuname_label_obj, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_ecuname_label_obj, THEME_COLOR_TEXT_PRIMARY, 0);
    if (s_ecuname_cache[0]) {
        lv_label_set_text(s_ecuname_label_obj, s_ecuname_cache);
    } else {
        lv_label_set_text(s_ecuname_label_obj, "Reading...");
        obd2_read_ecu_name(_ecuname_done, NULL);
    }
}

/* Build the new DEVICE section (card-grid style mirroring the web Setup
 * panel). Step 1 of the device-settings restyle: just the Device Info
 * card. Subsequent steps will add Dimmer/Logging/Testing cards alongside
 * it, and migrate the other grids (VEHICLE & CHANNELS, CONNECTIVITY). */
/* =========================================================================
 * Card-grid popup builders (Dimmer / Data Logger / Testing / CAN Bus /
 * Odometer). Each one mirrors the content of the equivalent
 * _build_section_* function but laid out inside a centered popup on
 * lv_layer_top(). Click handlers, live label statics, and any sub-modals
 * are reused as-is — only the parent geometry changes.
 * ========================================================================= */

static void _dimmer_popup_close(lv_event_t *e) {
    (void)e;
    if (s_dimmer_overlay && lv_obj_is_valid(s_dimmer_overlay)) lv_obj_del(s_dimmer_overlay);
    s_dimmer_overlay = NULL;
    brightness_label = NULL;
}

static void _dimmer_popup_open(lv_event_t *e) {
    (void)e;
    if (s_dimmer_overlay && lv_obj_is_valid(s_dimmer_overlay)) return;
    s_dimmer_overlay = _make_popup_shell(540, 280, "Brightness & Dimmer",
                                         _dimmer_popup_close);

    lv_obj_t *brightness_text = lv_label_create(s_dimmer_overlay);
    lv_label_set_text(brightness_text, "Brightness");
    lv_obj_align(brightness_text, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_text_font(brightness_text, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(brightness_text, THEME_COLOR_TEXT_MUTED, 0);

    uint8_t saved_brightness = current_brightness;
    lv_obj_t *brightness_bar = lv_slider_create(s_dimmer_overlay);
    lv_obj_set_size(brightness_bar, 380, 20);
    lv_obj_align(brightness_bar, LV_ALIGN_TOP_LEFT, 0, 82);
    lv_slider_set_range(brightness_bar, 5, 100);
    lv_slider_set_value(brightness_bar, saved_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(brightness_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(brightness_bar, THEME_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(brightness_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightness_bar, THEME_RADIUS_PILL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_TEXT_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(brightness_bar, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(brightness_bar, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_bar, 2, LV_PART_KNOB);

    brightness_label = lv_label_create(s_dimmer_overlay);
    lv_label_set_text_fmt(brightness_label, "%d%%", saved_brightness);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 390, 84);
    lv_obj_set_style_text_font(brightness_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(brightness_label, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(brightness_bar, brightness_bar_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *dimmer_btn = lv_btn_create(s_dimmer_overlay);
    lv_obj_set_size(dimmer_btn, 260, 34);
    lv_obj_align(dimmer_btn, LV_ALIGN_TOP_LEFT, 0, 130);
    lv_obj_set_style_bg_color(dimmer_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(dimmer_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(dimmer_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(dimmer_btn, 1, 0);
    lv_obj_set_style_border_color(dimmer_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(dimmer_btn, 0, 0);
    lv_obj_t *dimmer_lbl = lv_label_create(dimmer_btn);
    lv_label_set_text(dimmer_lbl, "Dimmer Switch Config...");
    lv_obj_center(dimmer_lbl);
    lv_obj_set_style_text_font(dimmer_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(dimmer_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(dimmer_btn, brightness_dimmer_config_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *note = lv_label_create(s_dimmer_overlay);
    lv_label_set_text(note,
        "Set a dimmer wire input or DIMMER_LEVEL signal to auto-dim at night.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 480);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 180);
    lv_obj_set_style_text_font(note, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(note, THEME_COLOR_TEXT_MUTED, 0);
}

static void _logger_popup_close(lv_event_t *e) {
    (void)e;
    if (s_logger_overlay && lv_obj_is_valid(s_logger_overlay)) lv_obj_del(s_logger_overlay);
    s_logger_overlay      = NULL;
    s_log_btn             = NULL;
    s_log_btn_label       = NULL;
    s_log_status_label    = NULL;
    s_log_rate_dd         = NULL;
    s_canraw_btn          = NULL;
    s_canraw_btn_label    = NULL;
    s_canraw_status_label = NULL;
}

static void _logger_popup_open(lv_event_t *e) {
    (void)e;
    if (s_logger_overlay && lv_obj_is_valid(s_logger_overlay)) return;
    s_logger_overlay = _make_popup_shell(620, 380, "Live Data & Logging",
                                         _logger_popup_close);

    /* Signal log: Start button + rate dropdown + status text. */
    s_log_btn = lv_btn_create(s_logger_overlay);
    lv_obj_set_size(s_log_btn, 150, 32);
    lv_obj_align(s_log_btn, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_bg_color(s_log_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(s_log_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_log_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(s_log_btn, 1, 0);
    lv_obj_set_style_border_color(s_log_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(s_log_btn, 0, 0);
    s_log_btn_label = lv_label_create(s_log_btn);
    lv_label_set_text(s_log_btn_label, "Start Signal Log");
    lv_obj_center(s_log_btn_label);
    lv_obj_set_style_text_font(s_log_btn_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_log_btn_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(s_log_btn, _log_toggle_btn_cb, LV_EVENT_CLICKED, NULL);

    s_log_rate_dd = lv_dropdown_create(s_logger_overlay);
    lv_dropdown_set_options_static(s_log_rate_dd,
        "1 Hz\n2 Hz\n5 Hz\n10 Hz\n20 Hz\n50 Hz\n100 Hz\n200 Hz\nMax");
    lv_obj_set_size(s_log_rate_dd, 100, 32);
    lv_obj_align(s_log_rate_dd, LV_ALIGN_TOP_LEFT, 160, 58);
    lv_obj_set_style_bg_color(s_log_rate_dd, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_color(s_log_rate_dd, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_log_rate_dd, 1, 0);
    lv_obj_set_style_radius(s_log_rate_dd, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_text_color(s_log_rate_dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(s_log_rate_dd, THEME_FONT_SMALL, 0);
    lv_dropdown_set_selected(s_log_rate_dd,
                             _log_rate_hz_to_idx(data_logger_get_rate_hz()));
    lv_obj_add_event_cb(s_log_rate_dd, _log_rate_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_log_status_label = lv_label_create(s_logger_overlay);
    lv_label_set_text(s_log_status_label, "Stopped");
    lv_obj_align(s_log_status_label, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_obj_set_style_text_font(s_log_status_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_log_status_label, THEME_COLOR_TEXT_MUTED, 0);

    /* Raw CAN capture: Start button + status text + Share button. */
    s_canraw_btn = lv_btn_create(s_logger_overlay);
    lv_obj_set_size(s_canraw_btn, 170, 32);
    lv_obj_align(s_canraw_btn, LV_ALIGN_TOP_LEFT, 0, 140);
    lv_obj_set_style_bg_color(s_canraw_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(s_canraw_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_canraw_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(s_canraw_btn, 1, 0);
    lv_obj_set_style_border_color(s_canraw_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(s_canraw_btn, 0, 0);
    s_canraw_btn_label = lv_label_create(s_canraw_btn);
    lv_label_set_text(s_canraw_btn_label, "Start Raw CAN");
    lv_obj_center(s_canraw_btn_label);
    lv_obj_set_style_text_font(s_canraw_btn_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_canraw_btn_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(s_canraw_btn, _canraw_toggle_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *share_btn = lv_btn_create(s_logger_overlay);
    lv_obj_set_size(share_btn, 170, 32);
    lv_obj_align(share_btn, LV_ALIGN_TOP_LEFT, 180, 140);
    lv_obj_set_style_bg_color(share_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(share_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(share_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(share_btn, 1, 0);
    lv_obj_set_style_border_color(share_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(share_btn, 0, 0);
    lv_obj_t *share_lbl = lv_label_create(share_btn);
    lv_label_set_text(share_lbl, "Share Raw CAN...");
    lv_obj_center(share_lbl);
    lv_obj_set_style_text_font(share_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(share_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(share_btn, _share_btn_cb, LV_EVENT_CLICKED, NULL);

    s_canraw_status_label = lv_label_create(s_logger_overlay);
    lv_label_set_text(s_canraw_status_label, "Raw: idle");
    lv_obj_align(s_canraw_status_label, LV_ALIGN_TOP_LEFT, 0, 182);
    lv_obj_set_style_text_font(s_canraw_status_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_canraw_status_label, THEME_COLOR_TEXT_MUTED, 0);

    /* Live values, folded in from the old separate "Peak Hold" card. Watching
     * the numbers and writing them down are the same job, and Studio now says
     * so too — this keeps the dash and the web editor telling one story
     * (ADR-0030). Peaks ARE the dash's live-signal view: every signal, with
     * its min and max. */
    lv_obj_t *view_btn = lv_btn_create(s_logger_overlay);
    lv_obj_set_size(view_btn, 170, 32);
    lv_obj_align(view_btn, LV_ALIGN_TOP_LEFT, 0, 222);
    lv_obj_set_style_bg_color(view_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(view_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(view_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(view_btn, 1, 0);
    lv_obj_set_style_border_color(view_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(view_btn, 0, 0);
    lv_obj_t *view_lbl = lv_label_create(view_btn);
    lv_label_set_text(view_lbl, "Live Values...");
    lv_obj_center(view_lbl);
    lv_obj_set_style_text_font(view_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(view_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(view_btn, _view_peaks_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset_btn = lv_btn_create(s_logger_overlay);
    lv_obj_set_size(reset_btn, 170, 32);
    lv_obj_align(reset_btn, LV_ALIGN_TOP_LEFT, 180, 222);
    lv_obj_set_style_bg_color(reset_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(reset_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(reset_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(reset_btn, 1, 0);
    lv_obj_set_style_border_color(reset_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(reset_btn, 0, 0);
    lv_obj_t *reset_lbl = lv_label_create(reset_btn);
    lv_label_set_text(reset_lbl, "Reset Min/Max");
    lv_obj_center(reset_lbl);
    lv_obj_set_style_text_font(reset_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(reset_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(reset_btn, _reset_peaks_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *note = lv_label_create(s_logger_overlay);
    lv_label_set_text(note,
        "Live Values shows every signal with its min and max. "
        "Signal log writes decoded values (CSV). Raw CAN captures every frame.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 560);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 266);
    lv_obj_set_style_text_font(note, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(note, THEME_COLOR_TEXT_MUTED, 0);

    _update_log_ui();
}

static void _testing_popup_close(lv_event_t *e) {
    (void)e;
    if (s_testing_overlay && lv_obj_is_valid(s_testing_overlay)) lv_obj_del(s_testing_overlay);
    s_testing_overlay       = NULL;
    s_sim_btn_label         = NULL;
}

/* _testing_popup_open (the standalone "Peak Hold" popup) is gone — its two
 * buttons moved into the Live Data & Logging popup, where watching values and
 * recording them sit together (ADR-0030). _testing_popup_close survives above:
 * settings teardown still clears the overlay pointer, and it is now a no-op in
 * the normal case. */

static void _odo_popup_close(lv_event_t *e) {
    (void)e;
    if (s_odo_overlay && lv_obj_is_valid(s_odo_overlay)) lv_obj_del(s_odo_overlay);
    s_odo_overlay        = NULL;
    s_veh_odo_value_lbl  = NULL;
}

static void _odo_popup_open(lv_event_t *e) {
    (void)e;
    if (s_odo_overlay && lv_obj_is_valid(s_odo_overlay)) return;
    s_odo_overlay = _make_popup_shell(500, 240, "Odometer", _odo_popup_close);

    lv_obj_t *cur_label = lv_label_create(s_odo_overlay);
    lv_label_set_text(cur_label, "Current reading");
    lv_obj_align(cur_label, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_text_font(cur_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(cur_label, THEME_COLOR_TEXT_MUTED, 0);

    s_veh_odo_value_lbl = lv_label_create(s_odo_overlay);
    lv_obj_align(s_veh_odo_value_lbl, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_obj_set_style_text_font(s_veh_odo_value_lbl, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(s_veh_odo_value_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    _veh_odo_refresh_timer_cb(NULL);

    lv_obj_t *edit_btn = lv_btn_create(s_odo_overlay);
    lv_obj_set_size(edit_btn, 200, 34);
    lv_obj_align(edit_btn, LV_ALIGN_TOP_LEFT, 0, 124);
    lv_obj_set_style_bg_color(edit_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(edit_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(edit_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(edit_btn, 1, 0);
    lv_obj_set_style_border_color(edit_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(edit_btn, 0, 0);
    lv_obj_t *edit_lbl = lv_label_create(edit_btn);
    lv_label_set_text(edit_lbl, "Edit Odometer...");
    lv_obj_center(edit_lbl);
    lv_obj_set_style_text_font(edit_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(edit_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(edit_btn, _odo_edit_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *note = lv_label_create(s_odo_overlay);
    lv_label_set_text(note,
        "Auto-accumulates from VEHICLE_SPEED. Persists every 1 km or 5 min.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 440);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 172);
    lv_obj_set_style_text_font(note, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(note, THEME_COLOR_TEXT_MUTED, 0);
}

static void _can_bus_popup_close(lv_event_t *e) {
    (void)e;
    /* Stop the embedded live-CAN refresh timer BEFORE deleting the overlay,
     * so the timer can't fire against rows that are about to be freed. */
    can_list_ui_embed_stop();
    if (s_can_bus_overlay && lv_obj_is_valid(s_can_bus_overlay)) lv_obj_del(s_can_bus_overlay);
    s_can_bus_overlay     = NULL;
    s_bitrate_dropdown    = NULL;
    s_can_health_dot      = NULL;
    s_can_health_label    = NULL;
    s_can_summary_label   = NULL;
    s_can_details_grid    = NULL;
    s_can_details_toggle  = NULL;
    memset(s_can_detail_labels, 0, sizeof(s_can_detail_labels));
}

static void _can_bus_popup_open(lv_event_t *e) {
    (void)e;
    if (s_can_bus_overlay && lv_obj_is_valid(s_can_bus_overlay)) return;
    s_can_bus_overlay = _make_popup_shell(700, 460, "CAN Bus",
                                          _can_bus_popup_close);

    lv_obj_t *bitrate_label = lv_label_create(s_can_bus_overlay);
    lv_label_set_text(bitrate_label, "Bitrate");
    lv_obj_align(bitrate_label, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_text_font(bitrate_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(bitrate_label, THEME_COLOR_TEXT_MUTED, 0);

    s_bitrate_dropdown = lv_dropdown_create(s_can_bus_overlay);
    lv_dropdown_set_options(s_bitrate_dropdown, "125 kbps\n250 kbps\n500 kbps\n1 Mbps");
    lv_obj_set_size(s_bitrate_dropdown, 160, 34);
    lv_obj_align(s_bitrate_dropdown, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_obj_set_style_bg_color(s_bitrate_dropdown, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(s_bitrate_dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_bitrate_dropdown, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(s_bitrate_dropdown, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(s_bitrate_dropdown, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_bitrate_dropdown, 1, 0);
    lv_obj_set_style_radius(s_bitrate_dropdown, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(s_bitrate_dropdown, 4, 0);
    lv_obj_set_style_text_color(s_bitrate_dropdown, THEME_COLOR_TEXT_MUTED, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_bitrate_dropdown, bitrate_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Restore persisted bitrate (previously done in
     * device_settings_with_return_screen after _build_section_can_config). */
    uint8_t saved_bitrate = 2;
    config_store_load_bitrate(&saved_bitrate);
    lv_dropdown_set_selected(s_bitrate_dropdown, saved_bitrate);

    /* Live CAN feed — embed the same scrolling ID/Hz/DLC/bytes table that
     * the full-screen viewer uses, directly in the popup body. Replaces the
     * old health-status panel ("No CAN traffic detected" + Show Details):
     * the live table makes bus activity self-evident at a glance. Torn down
     * in _can_bus_popup_close via can_list_ui_embed_stop(). */
    lv_obj_t *feed_host = lv_obj_create(s_can_bus_overlay);
    lv_obj_set_size(feed_host, 664, 300);
    lv_obj_align(feed_host, LV_ALIGN_TOP_LEFT, 0, 120);
    lv_obj_set_style_bg_opa(feed_host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(feed_host, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(feed_host, 1, 0);
    lv_obj_set_style_radius(feed_host, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(feed_host, 0, 0);
    lv_obj_set_style_clip_corner(feed_host, true, 0);
    lv_obj_clear_flag(feed_host, LV_OBJ_FLAG_SCROLLABLE);
    can_list_ui_embed(feed_host);
}

/* =========================================================================
 * Grid builders — VEHICLE & CHANNELS / CONNECTIVITY / DEVICE
 *
 * Mirrors the web Setup-mode layout. Cards are 232 x 110, 3 per row,
 * inside a 720-wide grid. Each card opens either an existing screen
 * (ECU picker, WiFi, Trouble Codes) or one of the popups defined above
 * (Dimmer, Logger, Testing, CAN Bus, Odometer). The action buttons row
 * (System Diagnostics / Setup Wizard / Reset) renders below the grids.
 * ========================================================================= */

/* Channels card → opens the full split-pane channels editor (the setup
 * wizard's Step 3) standalone. Replaces the old "ECU Preset" card: ECU
 * presets are now bound per-channel from inside that editor's source
 * picker, and bulk ECU auto-detect lives in the setup wizard. */
static void _channels_card_cb(lv_event_t *e) {
    (void)e;
    first_run_wizard_open_channels();
}

static void _build_vehicle_grid(lv_obj_t *content) {
    _make_setup_section_title(content, "VEHICLE & CHANNELS");
    lv_obj_t *grid = _make_setup_grid(content);

    /* Channels card — primary vehicle-setup surface. Stat shows how many
     * channels are currently mapped to a live signal. */
    size_t ch_total = channel_manager_count();
    size_t ch_bound = 0;
    for (size_t i = 0; i < ch_total; i++) {
        const channel_t *c = channel_manager_at(i);
        if (c && c->signal_index >= 0) ch_bound++;
    }
    char ch_txt[32];
    snprintf(ch_txt, sizeof(ch_txt), "%u MAPPED", (unsigned)ch_bound);
    _make_setup_card(grid, LV_SYMBOL_LIST, "Channels",
        "Map signals to channels, set ranges + warnings.",
        ch_txt, _channels_card_cb);

    /* OBD2 PIDs card — the by-hand tool, named as such. Getting OBD2
     * readings onto the dash is one action and it lives in Channels
     * ("Scan for OBD2"); this card is for picking exact PIDs and adding
     * ones the standard list doesn't know (ADR-0037). Same
     * repoint-the-stat-label pattern as the cards above. */
    char obd2_txt[48];
    _obd2_label_compose(obd2_txt, sizeof(obd2_txt));
    setup_card_t obd2 = _make_setup_card(grid, LV_SYMBOL_DRIVE, "OBD2 PIDs",
        "Pick exact PIDs by hand. For readings, use Channels.",
        obd2_txt, _obd2_btn_cb);
    s_obd2_btn_label = obd2.stat_label;

    _make_setup_card(grid, LV_SYMBOL_SETTINGS, "Gear Calc",
        "RPM + speed = calculated gear.",
        "CALCULATED_GEAR", _veh_gear_btn_cb);

    _make_setup_card(grid, LV_SYMBOL_CHARGE, "Odometer",
        "Auto-accumulates from VEHICLE_SPEED.",
        "KM", _odo_popup_open);
}

static void _build_connectivity_grid(lv_obj_t *content) {
    _make_setup_section_title(content, "CONNECTIVITY");
    lv_obj_t *grid = _make_setup_grid(content);

    /* WiFi card. Stat shows the current SSID — refresh_wifi_status writes
     * into wifi_status_label every 2 s via s_wifi_status_timer. */
    setup_card_t wifi = _make_setup_card(grid, LV_SYMBOL_WIFI, "WiFi & Network",
        "Configure WiFi, view IP and hotspot status.",
        "—", wifi_btn_event_cb);
    wifi_status_label = wifi.stat_label;

    /* Web Editor QR card. Stat shows the IP. Same timer drives it via the
     * repurposed web_status_label pointer. */
    setup_card_t qr = _make_setup_card(grid, LV_SYMBOL_EYE_OPEN, "Web Editor",
        "Scan QR to open the dash web UI on your phone.",
        "—", _qr_btn_cb);
    web_status_label = qr.stat_label;

    _make_setup_card(grid, LV_SYMBOL_SHUFFLE, "CAN Bus",
        "Bitrate + live bus health diagnostics.",
        "500 KBPS", _can_bus_popup_open);

    _make_setup_card(grid, LV_SYMBOL_WARNING, "Trouble Codes",
        "Read & clear DTCs over OBD2.",
        "READ", _dtc_btn_cb);
}

static void _build_device_grid(lv_obj_t *content) {
    _make_setup_section_title(content, "DEVICE");
    lv_obj_t *grid = _make_setup_grid(content);

    char fw_stat[24];
    snprintf(fw_stat, sizeof(fw_stat), "v%s", FIRMWARE_VERSION);
    _make_setup_card(grid, LV_SYMBOL_HOME, "Device Info",
        "Serial, firmware, VIN, ECU name.",
        fw_stat, _device_info_popup_open);

    char bri_stat[12];
    snprintf(bri_stat, sizeof(bri_stat), "%d%%", current_brightness);
    _make_setup_card(grid, LV_SYMBOL_EYE_OPEN, "Brightness",
        "Slider + auto-dim wire/signal hookup.",
        bri_stat, _dimmer_popup_open);

    /* One card. The old "Peak Hold" card next door was the live-signal view;
     * it now opens from inside this popup, matching Studio's merged Live Data
     * & Logging page (ADR-0030). */
    _make_setup_card(grid, LV_SYMBOL_SD_CARD, "Live Data & Logging",
        "Live values, min/max, signal log + Raw CAN.",
        "IDLE", _logger_popup_open);

    /* Simulator — moved out of the old Peak Hold & Testing popup into its own
     * card. Tapping it toggles the demo sweep directly; the stat shows the
     * live ON/OFF state. */
    bool sim_on = signal_sim_is_active();
    setup_card_t sim = _make_setup_card(grid, LV_SYMBOL_PLAY, "Simulator",
        "Replay fake CAN frames to preview the dash.",
        sim_on ? "ON" : "OFF", _sim_card_cb);
    s_sim_card_stat = sim.stat_label;
    lv_obj_set_style_text_color(s_sim_card_stat,
        sim_on ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_ACCENT, 0);
}

__attribute__((unused))
static void _build_section_display(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "DISPLAY");

    lv_obj_t *brightness_text = lv_label_create(s);
    lv_label_set_text(brightness_text, "Brightness");
    lv_obj_align(brightness_text, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_text_font(brightness_text, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(brightness_text, THEME_COLOR_TEXT_MUTED, 0);

    uint8_t saved_brightness = current_brightness;

    lv_obj_t *brightness_bar = lv_slider_create(s);
    lv_obj_set_size(brightness_bar, 220, 20);
    lv_obj_align(brightness_bar, LV_ALIGN_TOP_LEFT, 0, 45);
    lv_slider_set_range(brightness_bar, 5, 100);
    lv_slider_set_value(brightness_bar, saved_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(brightness_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(brightness_bar, THEME_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(brightness_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightness_bar, THEME_RADIUS_PILL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_TEXT_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(brightness_bar, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(brightness_bar, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_bar, 2, LV_PART_KNOB);

    brightness_label = lv_label_create(s);
    lv_label_set_text_fmt(brightness_label, "%d%%", saved_brightness);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 230, 48);
    lv_obj_set_style_text_font(brightness_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(brightness_label, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(brightness_bar, brightness_bar_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *dimmer_btn = lv_btn_create(s);
    lv_obj_set_size(dimmer_btn, 250, 30);
    lv_obj_align(dimmer_btn, LV_ALIGN_TOP_LEFT, 0, 80);
    lv_obj_set_style_bg_color(dimmer_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(dimmer_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(dimmer_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(dimmer_btn, 1, 0);
    lv_obj_set_style_border_color(dimmer_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(dimmer_btn, 0, 0);
    lv_obj_t *dimmer_label = lv_label_create(dimmer_btn);
    lv_label_set_text(dimmer_label, "Dimmer Switch Config");
    lv_obj_center(dimmer_label);
    lv_obj_set_style_text_font(dimmer_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(dimmer_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(dimmer_btn, brightness_dimmer_config_cb, LV_EVENT_CLICKED, NULL);

    /* Rotation button hidden until RGB-panel driver gets sw_rotate support.
     * Persistence code stays so a future firmware can pick up the saved value. */
    (void) _rotation_btn_cb;
    (void) s_rotation_btn_label;

    (void) _night_btn_cb;
    (void) s_night_btn_label;
}

static void _share_btn_cb(lv_event_t *e) {
    (void)e;
    _share_modal_open();
}

/* ── VEHICLE section ─────────────────────────────────────────────────────
 *
 * Container for vehicle-tied user state that doesn't fit cleanly under
 * CAN BUS or DATA LOGGING:
 *   • Calculated gear setup (RPM/speed source, wheel circumference,
 *     final drive, per-gear ratios). Opens the existing ui_gear_setup
 *     overlay — same modal that auto-pops after picking RDM-7 Internal.
 *   • Odometer reading: live display (refreshed once a second from
 *     signal_internal_get_odometer_km), plus an Edit button that opens
 *     a numeric-keyboard overlay for manual entry on first install.
 *
 * Odometer accumulation runs continuously inside signal_internal.c's
 * tick — this card just exposes the running value and lets the user
 * snap it to a starting reading.
 *
 * State for this section (s_veh_odo_*, s_odo_edit_*) is hoisted to the
 * file-level static block near the top so the screen-close handler can
 * tear it down without forward-declaration. */

static void _veh_odo_refresh_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_veh_odo_value_lbl || !lv_obj_is_valid(s_veh_odo_value_lbl)) return;
    float km = signal_internal_get_odometer_km();
    char buf[40];
    snprintf(buf, sizeof(buf), "Odometer: %.1f km", (double)km);
    lv_label_set_text(s_veh_odo_value_lbl, buf);
}

static void _odo_edit_close(void) {
    if (s_odo_edit_overlay && lv_obj_is_valid(s_odo_edit_overlay)) {
        lv_obj_del(s_odo_edit_overlay);
    }
    s_odo_edit_overlay  = NULL;
    s_odo_edit_textarea = NULL;
}

static void _odo_edit_save_cb(lv_event_t *e) {
    (void)e;
    if (!s_odo_edit_textarea) { _odo_edit_close(); return; }
    const char *txt = lv_textarea_get_text(s_odo_edit_textarea);
    float km = txt ? strtof(txt, NULL) : 0.0f;
    /* signal_internal_set_odometer_km clamps + persists + publishes. */
    signal_internal_set_odometer_km(km);
    _odo_edit_close();
    /* Force-refresh the section's value label so the user sees the new
     * reading immediately rather than waiting up to a second for the
     * timer. */
    _veh_odo_refresh_timer_cb(NULL);
}

static void _odo_edit_cancel_cb(lv_event_t *e) {
    (void)e;
    _odo_edit_close();
}

/* Numeric-keyboard overlay for manual odometer entry. Mirrors the
 * WiFi password modal pattern (ui_wifi.c) — full-screen dimmer plus a
 * centred card holding a textarea + LV_KEYBOARD_MODE_NUMBER keyboard +
 * Save / Cancel buttons. */
static void _odo_edit_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_odo_edit_overlay) return;  /* already open */

    lv_obj_t *scr = lv_layer_top();
    s_odo_edit_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_odo_edit_overlay);
    lv_obj_set_size(s_odo_edit_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_odo_edit_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_odo_edit_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(s_odo_edit_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_odo_edit_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(s_odo_edit_overlay);
    lv_obj_set_size(card, 480, 380);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Set Odometer (km)");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    s_odo_edit_textarea = lv_textarea_create(card);
    lv_obj_set_size(s_odo_edit_textarea, 448, 36);
    lv_obj_align(s_odo_edit_textarea, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_textarea_set_one_line(s_odo_edit_textarea, true);
    lv_textarea_set_accepted_chars(s_odo_edit_textarea, "0123456789.");
    lv_textarea_set_max_length(s_odo_edit_textarea, 16);
    lv_obj_set_style_text_font(s_odo_edit_textarea, THEME_FONT_MEDIUM, 0);
    /* Pre-fill with the current reading so a small adjustment doesn't
     * require typing the whole number again. */
    char prefill[24];
    snprintf(prefill, sizeof(prefill), "%.1f",
             (double)signal_internal_get_odometer_km());
    lv_textarea_set_text(s_odo_edit_textarea, prefill);

    lv_obj_t *kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, 448, 220);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, s_odo_edit_textarea);

    /* Save / Cancel buttons. */
    lv_obj_t *save = lv_btn_create(card);
    lv_obj_set_size(save, 200, 36);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, -6);
    lv_obj_set_style_bg_color(save, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(save, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(save, 0, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_t *save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);
    lv_obj_set_style_text_color(save_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_set_style_text_font(save_lbl, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(save, _odo_edit_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 200, 36);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    lv_obj_set_style_bg_color(cancel, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_radius(cancel, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_set_style_text_color(cancel_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(cancel, _odo_edit_cancel_cb, LV_EVENT_CLICKED, NULL);
}

static void _veh_gear_btn_cb(lv_event_t *e) {
    (void)e;
    ui_gear_setup_open(NULL, NULL);
}

/* Legacy CAN health panel (dot + summary + Show Details). Superseded by the
 * embedded live-CAN feed in _can_bus_popup_open; kept compiled in case the
 * at-a-glance health summary is wanted again. Unused-attr silences the warn. */
__attribute__((unused))
static void _build_section_can_diagnostics(lv_obj_t *content) {
    lv_obj_t *s = lv_obj_create(content);
    lv_obj_set_size(s, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_side(s, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(s, THEME_COLOR_SECTION_CAN_TITLE, 0);
    lv_obj_set_style_border_width(s, 3, 0);
    lv_obj_set_style_pad_all(s, 10, 0);
    lv_obj_set_style_pad_left(s, 12, 0);
    lv_obj_set_style_pad_row(s, 5, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title_row = lv_obj_create(s);
    lv_obj_set_size(title_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *can_diag_title = lv_label_create(title_row);
    lv_label_set_text(can_diag_title, "CAN BUS");
    lv_obj_align(can_diag_title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(can_diag_title, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(can_diag_title, THEME_COLOR_SECTION_CAN_TITLE, 0);
    lv_obj_set_style_text_letter_space(can_diag_title, 1, 0);

    /* "View More" — opens the live CAN ID list (raw ID + bytes ticking
     * through). Bitrate scan moved to the "Re-run Setup Wizard" path
     * since suspending the bus belongs in the setup flow, not casual
     * diagnostics. */
    lv_obj_t *view_more_btn = lv_btn_create(title_row);
    lv_obj_set_size(view_more_btn, 110, 24);
    lv_obj_align(view_more_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(view_more_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_bg_color(view_more_btn, THEME_COLOR_ACCENT_BLUE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(view_more_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(view_more_btn, 0, 0);
    lv_obj_t *view_more_lbl = lv_label_create(view_more_btn);
    lv_label_set_text(view_more_lbl, "View More");
    lv_obj_center(view_more_lbl);
    lv_obj_set_style_text_font(view_more_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(view_more_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(view_more_btn, _can_view_more_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *health_row = lv_obj_create(s);
    lv_obj_set_size(health_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(health_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(health_row, 0, 0);
    lv_obj_set_style_pad_all(health_row, 0, 0);
    lv_obj_set_style_pad_column(health_row, 6, 0);
    lv_obj_clear_flag(health_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(health_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(health_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_can_health_dot = lv_obj_create(health_row);
    lv_obj_set_size(s_can_health_dot, 8, 8);
    lv_obj_set_style_radius(s_can_health_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_can_health_dot, THEME_COLOR_TEXT_HINT, 0);
    lv_obj_set_style_bg_opa(s_can_health_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_can_health_dot, 0, 0);
    lv_obj_clear_flag(s_can_health_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_can_health_label = lv_label_create(health_row);
    lv_label_set_text(s_can_health_label, "Checking...");
    lv_obj_set_style_text_font(s_can_health_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_can_health_label, THEME_COLOR_TEXT_MUTED, 0);

    s_can_summary_label = lv_label_create(s);
    lv_label_set_text(s_can_summary_label, "");
    lv_obj_set_style_text_font(s_can_summary_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_can_summary_label, THEME_COLOR_TEXT_MUTED, 0);

    s_can_details_toggle = lv_label_create(s);
    lv_label_set_text(s_can_details_toggle, LV_SYMBOL_RIGHT " Show Details");
    lv_obj_set_style_text_font(s_can_details_toggle, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_can_details_toggle, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_add_flag(s_can_details_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_can_details_toggle, _details_toggle_cb, LV_EVENT_CLICKED, NULL);

    s_can_details_grid = lv_obj_create(s);
    lv_obj_set_size(s_can_details_grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_can_details_grid, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(s_can_details_grid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_can_details_grid, 0, 0);
    lv_obj_set_style_radius(s_can_details_grid, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_pad_all(s_can_details_grid, 6, 0);
    lv_obj_set_style_pad_column(s_can_details_grid, 8, 0);
    lv_obj_set_style_pad_row(s_can_details_grid, 2, 0);
    lv_obj_clear_flag(s_can_details_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_can_details_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_add_flag(s_can_details_grid, LV_OBJ_FLAG_HIDDEN);

    #define DIAG_COL_W 200
    static const char *detail_defaults[] = {
        "RX Count: ---", "RX Errors: ---", "RX Missed: ---",
        "TX Count: ---", "TX Errors: ---", "Bus Errors: ---"
    };
    for (int i = 0; i < 6; i++) {
        s_can_detail_labels[i] = lv_label_create(s_can_details_grid);
        lv_label_set_text(s_can_detail_labels[i], detail_defaults[i]);
        lv_obj_set_width(s_can_detail_labels[i], DIAG_COL_W);
        lv_obj_set_style_text_font(s_can_detail_labels[i], THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_can_detail_labels[i], THEME_COLOR_TEXT_MUTED, 0);
    }
    #undef DIAG_COL_W

    s_prev_rx_count = can_get_rx_frame_count();
    s_rx_rate = 0;
    refresh_can_diagnostics();
}

static void _build_action_buttons(lv_obj_t *content) {
    lv_obj_t *diag_btn = lv_btn_create(content);
    lv_obj_set_size(diag_btn, lv_pct(100), 34);
    lv_obj_set_style_bg_color(diag_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(diag_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(diag_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_color(diag_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(diag_btn, 1, 0);
    lv_obj_set_style_shadow_width(diag_btn, 0, 0);
    lv_obj_t *diag_label = lv_label_create(diag_btn);
    lv_label_set_text(diag_label, "System Diagnostics");
    lv_obj_center(diag_label);
    lv_obj_set_style_text_font(diag_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(diag_label, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(diag_btn, _diag_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wizard_btn = lv_btn_create(content);
    lv_obj_set_size(wizard_btn, lv_pct(100), 34);
    lv_obj_set_style_bg_color(wizard_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(wizard_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(wizard_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_color(wizard_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(wizard_btn, 1, 0);
    lv_obj_set_style_shadow_width(wizard_btn, 0, 0);
    lv_obj_t *wizard_label = lv_label_create(wizard_btn);
    lv_label_set_text(wizard_label, "Run Setup Wizard");
    lv_obj_center(wizard_label);
    lv_obj_set_style_text_font(wizard_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(wizard_label, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(wizard_btn, _run_wizard_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Layout-only reset — restores the factory "default" layout, nothing
     * else. Sits above the full factory reset with a clearly scoped label
     * so the two "reset" actions can't be confused. */
    lv_obj_t *lreset_btn = lv_btn_create(content);
    lv_obj_set_size(lreset_btn, lv_pct(100), 34);
    lv_obj_set_style_bg_color(lreset_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(lreset_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(lreset_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_color(lreset_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(lreset_btn, 1, 0);
    lv_obj_set_style_shadow_width(lreset_btn, 0, 0);
    lv_obj_t *lreset_label = lv_label_create(lreset_btn);
    lv_label_set_text(lreset_label, "Reset Default Layout");
    lv_obj_center(lreset_label);
    lv_obj_set_style_text_font(lreset_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lreset_label, THEME_COLOR_STATUS_ERROR, 0);
    lv_obj_add_event_cb(lreset_btn, _layout_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset_btn = lv_btn_create(content);
    lv_obj_set_size(reset_btn, lv_pct(100), 34);
    lv_obj_set_style_bg_color(reset_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(reset_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(reset_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_color(reset_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(reset_btn, 1, 0);
    lv_obj_set_style_shadow_width(reset_btn, 0, 0);
    lv_obj_t *reset_label = lv_label_create(reset_btn);
    /* Renamed from "Reset to Default" — that label was ambiguous next to
     * the layout-only reset above (this one wipes EVERYTHING). */
    lv_label_set_text(reset_label, "Factory Reset");
    lv_obj_center(reset_label);
    lv_obj_set_style_text_font(reset_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(reset_label, THEME_COLOR_STATUS_ERROR, 0);
    lv_obj_add_event_cb(reset_btn, _factory_reset_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* Universal cleanup: fires no matter who deletes settings_screen — explicit
 * lv_obj_del, lv_scr_load_anim with auto_del, or an async dashboard reload
 * that wipes the previous active screen. close_menu_event_cb and
 * _deferred_reload_after_ecu used to be the only paths that tore down the
 * screen-owned timers; the 2026-06 wifi-status panic came from a path where
 * neither ran, so the timer kept ticking on freed labels. Safe to invoke
 * twice (those callers still do their own cleanup pre-load) because every
 * branch is NULL-guarded. */
static void _settings_screen_delete_cb(lv_event_t *e) {
    (void)e;

    if (s_wifi_status_timer) {
        lv_timer_del(s_wifi_status_timer);
        s_wifi_status_timer = NULL;
    }
    if (s_log_status_timer) {
        lv_timer_del(s_log_status_timer);
        s_log_status_timer = NULL;
    }
    if (s_can_diag_timer) {
        lv_timer_del(s_can_diag_timer);
        s_can_diag_timer = NULL;
    }
    if (s_veh_odo_timer) {
        lv_timer_del(s_veh_odo_timer);
        s_veh_odo_timer = NULL;
    }

    /* Tear down lv_layer_top() popups that may have been left open. They
     * don't share a parent with settings_screen, so they'd otherwise leak
     * (and their static label pointers would dangle the next time the
     * settings screen reopens). Each popup_close NULLs its own internal
     * statics; the bulk NULL block below catches anything else. */
    _device_info_popup_close(NULL);
    _dimmer_popup_close(NULL);
    _logger_popup_close(NULL);
    _testing_popup_close(NULL);
    _can_bus_popup_close(NULL);
    _odo_popup_close(NULL);

    wifi_status_label    = NULL;
    web_status_label     = NULL;
    ap_status_label      = NULL;
    brightness_label     = NULL;
    s_log_btn            = NULL;
    s_log_btn_label      = NULL;
    s_log_status_label   = NULL;
    s_log_rate_dd        = NULL;
    s_canraw_btn         = NULL;
    s_canraw_btn_label   = NULL;
    s_canraw_status_label= NULL;
    s_sim_btn_label      = NULL;
    s_sim_card_stat      = NULL;
    s_rotation_btn_label = NULL;
    s_night_btn_label    = NULL;
    s_veh_odo_value_lbl  = NULL;
    s_can_health_dot     = NULL;
    s_can_health_label   = NULL;
    s_can_summary_label  = NULL;
    s_can_details_grid   = NULL;
    s_can_details_toggle = NULL;
    memset(s_can_detail_labels, 0, sizeof(s_can_detail_labels));
    s_bitrate_dropdown   = NULL;
}

void device_settings_with_return_screen(lv_obj_t* return_screen) {
    device_settings_return_screen = return_screen ? return_screen : lv_scr_act();

    lv_obj_t *settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settings_screen, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(settings_screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(settings_screen, _settings_screen_delete_cb,
                        LV_EVENT_DELETE, NULL);

    lv_obj_t *main_container = lv_obj_create(settings_screen);
    lv_obj_set_size(main_container, 760, 440);
    lv_obj_align(main_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(main_container, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(main_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(main_container, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(main_container, 1, 0);
    lv_obj_set_style_radius(main_container, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(main_container, 0, 0);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    _build_header(main_container);
    lv_obj_t *content = _build_content_area(main_container);

    /* Card-grid layout mirroring the web Studio Setup-mode panel. Three
     * grids stacked vertically (VEHICLE & CHANNELS / CONNECTIVITY /
     * DEVICE), each a section title + 4 setup cards. Cards open popups
     * or existing screens on tap — all live status is repainted into the
     * card stat labels by the existing wifi / log / can / odo timers. */
    _build_vehicle_grid(content);
    _build_connectivity_grid(content);
    _build_device_grid(content);

    /* Action buttons row (System Diagnostics / Setup Wizard / Reset). */
    _build_action_buttons(content);

    /* Live-update timers. Their callbacks NULL-check every label they
     * paint into, so it's safe to keep them running even when a popup
     * isn't open (the card stat labels are still alive in the grid). */
    if (s_log_status_timer) lv_timer_del(s_log_status_timer);
    s_log_status_timer = lv_timer_create(_log_status_timer_cb, 1000, NULL);

    if (s_wifi_status_timer) lv_timer_del(s_wifi_status_timer);
    s_wifi_status_timer = lv_timer_create(refresh_wifi_status_timer_cb, 2000, NULL);
    refresh_wifi_status();

    if (s_can_diag_timer) lv_timer_del(s_can_diag_timer);
    s_can_diag_timer = lv_timer_create(refresh_can_diag_timer_cb, 1000, NULL);

    if (s_veh_odo_timer) lv_timer_del(s_veh_odo_timer);
    s_veh_odo_timer = lv_timer_create(_veh_odo_refresh_timer_cb, 1000, NULL);

    lv_scr_load(settings_screen);
}

void device_settings_longpress_cb(lv_event_t* e) {
    device_settings_with_return_screen(NULL);
}