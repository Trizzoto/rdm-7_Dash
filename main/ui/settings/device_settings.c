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
#include "screens/ui_ecu_picker.h"
#include "screens/ui_can_list.h"
#include "layout/ecu_presets.h"
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

/* Display rotation + night-mode (#23) */
static lv_obj_t *s_rotation_btn_label = NULL;
static lv_obj_t *s_night_btn_label = NULL;

/* Developer options */
static lv_obj_t *s_wire_input_btn_label = NULL;

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

/* ECU selection row (CAN BUS section) */
static lv_obj_t  *s_ecu_value_label    = NULL;

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
static void refresh_wifi_status(void) {
    if (!wifi_status_label || !web_status_label) return;

    // Update WiFi STA status
    const char *sta_ssid = wifi_manager_get_connected_ssid();
    if (sta_ssid && sta_ssid[0] != '\0') {
        char status_text[48];
        snprintf(status_text, sizeof(status_text), "WiFi: %s", sta_ssid);
        lv_label_set_text(wifi_status_label, status_text);
        lv_obj_set_style_text_color(wifi_status_label, THEME_COLOR_STATUS_CONNECTED, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_label_set_text(wifi_status_label, "WiFi: Not Connected");
        lv_obj_set_style_text_color(wifi_status_label, THEME_COLOR_STATUS_WARN, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    // Update web server status — show the raw IP; mDNS (.local) is disabled
    // because the espressif__mdns component can't allocate internal-RAM
    // buffers in this build. Users reach the dash via the IP or QR code.
    if (sta_ssid && sta_ssid[0] != '\0') {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char web_text[64];
            snprintf(web_text, sizeof(web_text),
                     "Web: http://" IPSTR, IP2STR(&ip_info.ip));
            lv_label_set_text(web_status_label, web_text);
            lv_obj_set_style_text_color(web_status_label, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_label_set_text(web_status_label, "Web: Waiting for IP...");
            lv_obj_set_style_text_color(web_status_label, THEME_COLOR_ACCENT_YELLOW, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        /* Check if AP mode provides an alternative */
        if (wifi_manager_is_started() && wifi_manager_is_ap_enabled()) {
            lv_label_set_text(web_status_label, "Web: http://192.168.4.1");
            lv_obj_set_style_text_color(web_status_label, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_label_set_text(web_status_label, "Web: Connect WiFi first");
            lv_obj_set_style_text_color(web_status_label, THEME_COLOR_STATUS_WARN, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }

    // Update AP hotspot status
    if (ap_status_label && lv_obj_is_valid(ap_status_label)) {
        if (wifi_manager_is_started() && wifi_manager_is_ap_enabled()) {
            wifi_sta_list_t sta_list;
            esp_wifi_ap_get_sta_list(&sta_list);
            char ap_text[96];
            snprintf(ap_text, sizeof(ap_text),
                     "Hotspot: %s - 192.168.4.1 (%d client%s)",
                     wifi_manager_get_ap_ssid(), sta_list.num,
                     sta_list.num == 1 ? "" : "s");
            lv_label_set_text(ap_status_label, ap_text);
            lv_obj_set_style_text_color(ap_status_label, THEME_COLOR_STATUS_CONNECTED, LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_label_set_text(ap_status_label, "Hotspot: Disabled");
            lv_obj_set_style_text_color(ap_status_label, THEME_COLOR_TEXT_HINT, LV_PART_MAIN | LV_STATE_DEFAULT);
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
        idx = signal_register(dimmer_config.signal_name, 0,
                              0, 1, 1.0f, 0.0f, false, 1, "");
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
    if (!s_can_health_dot) return;

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
                "Scan finished — no valid CAN frames on any supported bitrate.");
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

/* "View More" button in the CAN BUS section header. Opens the live CAN ID
 * list (ui_can_list) so the user can see every ID + binary bytes ticking
 * through. Bitrate scanning is no longer surfaced here — it's accessible
 * via the "Re-run Setup Wizard" button below, which is the right place
 * for a setup-time operation that suspends the bus. */
static void _can_view_more_btn_cb(lv_event_t *e) {
    (void)e;
    /* Drop back to the dashboard so the can_list screen has a clean
     * backdrop, then show on the next tick (same pattern as the diag
     * and wizard launchers in this file). */
    lv_obj_t *ret = device_settings_return_screen;
    if (ret && lv_obj_is_valid(ret)) {
        lv_scr_load(ret);
    }
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

// Update button callback
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
            "No Raw CAN recording found — start one first.");
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
static void _sim_toggle_btn_cb(lv_event_t *e) {
    (void)e;
    if (signal_sim_is_active()) {
        signal_sim_stop();
    } else {
        signal_sim_start();
    }
    if (s_sim_btn_label && lv_obj_is_valid(s_sim_btn_label)) {
        bool on = signal_sim_is_active();
        lv_label_set_text(s_sim_btn_label, on ? "Sim: ON" : "Sim: OFF");
        lv_obj_set_style_text_color(s_sim_btn_label,
            on ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_TEXT_MUTED, 0);
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
    lv_obj_t *ret = device_settings_return_screen;
    if (ret && lv_obj_is_valid(ret)) {
        lv_scr_load(ret);
    }
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

/* ── ECU selection ───────────────────────────────────────────────────── */

/* Compose the "Make Version" or "Not selected" label for the current ECU. */
static void _ecu_label_compose(char *buf, size_t n) {
    char make[32] = {0}, ver[32] = {0};
    if (config_store_load_ecu(make, sizeof(make), ver, sizeof(ver)) == ESP_OK &&
        make[0] && ver[0]) {
        const ecu_preset_t *p = ecu_preset_find(make, ver);
        if (p && p->display) { snprintf(buf, n, "%s", p->display); return; }
        snprintf(buf, n, "%s %s", make, ver);
        return;
    }
    snprintf(buf, n, "Not selected");
}

/* Runs on the LVGL async queue so the picker's overlay del_async has
 * processed first. Without this deferral, lv_obj_del(old) races against
 * the pending async-del of the picker's overlay (same crash pattern as
 * the first_run_wizard Finish flow). */
static void _deferred_reload_after_ecu(void *arg) {
    (void)arg;
    lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
    ui_Screen3_screen_init();
    lv_scr_load(ui_Screen3);
    if (old && old != ui_Screen3 && lv_obj_is_valid(old))
        lv_obj_del(old);
}

static void _ecu_picker_done_cb(bool applied, void *ctx) {
    (void)ctx;
    /* Refresh the value label. */
    if (s_ecu_value_label && lv_obj_is_valid(s_ecu_value_label)) {
        char txt[64];
        _ecu_label_compose(txt, sizeof(txt));
        lv_label_set_text(s_ecu_value_label, txt);
    }
    if (applied) {
        lv_async_call(_deferred_reload_after_ecu, NULL);
    }
}

static void _ecu_btn_cb(lv_event_t *e) {
    (void)e;
    ecu_picker_open("default", true, _ecu_picker_done_cb, NULL);
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

/* ── Wire Input Mode ───────────────────────────────────────────────────── */

static void _wire_input_reboot_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn_txt = lv_msgbox_get_active_btn_text(mbox);
    if (!btn_txt) return;
    if (strcmp(btn_txt, "Reboot") == 0)
        esp_restart();
    lv_msgbox_close(mbox);
}

static void _wire_input_mode_btn_cb(lv_event_t *e) {
    (void)e;
    bool enabled = false;
    config_store_load_wire_input_mode(&enabled);
    enabled = !enabled;
    config_store_save_wire_input_mode(enabled);

    if (s_wire_input_btn_label && lv_obj_is_valid(s_wire_input_btn_label)) {
        lv_label_set_text(s_wire_input_btn_label,
            enabled ? "Wire Inputs: ON" : "Wire Inputs: OFF");
        lv_obj_set_style_text_color(s_wire_input_btn_label,
            enabled ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_TEXT_MUTED, 0);
    }

    static const char *btns[] = {"Reboot", "Later", ""};
    lv_obj_t *mbox = lv_msgbox_create(
        NULL,
        "Reboot Required",
        enabled
            ? "Wire input mode ON.\nGPIO 43/44 will be used for turn\nsignals after reboot (UART1 disabled)."
            : "Wire input mode OFF.\nUART1 serial will be re-enabled\nafter reboot.",
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
    lv_obj_add_event_cb(mbox, _wire_input_reboot_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

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

static lv_obj_t *_build_row(lv_obj_t *parent, int32_t h) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), h);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    return row;
}

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

static void _build_section_can_config(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "CAN BUS");

    lv_obj_t *bitrate_label = lv_label_create(s);
    lv_label_set_text(bitrate_label, "Bitrate");
    lv_obj_align(bitrate_label, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_text_font(bitrate_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(bitrate_label, THEME_COLOR_TEXT_MUTED, 0);

    lv_obj_t *bitrate_dd = lv_dropdown_create(s);
    lv_dropdown_set_options(bitrate_dd, "125 kbps\n250 kbps\n500 kbps\n1 Mbps");
    lv_obj_set_size(bitrate_dd, 140, 32);
    lv_obj_align(bitrate_dd, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_set_style_bg_color(bitrate_dd, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(bitrate_dd, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(bitrate_dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(bitrate_dd, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(bitrate_dd, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bitrate_dd, 1, 0);
    lv_obj_set_style_radius(bitrate_dd, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(bitrate_dd, 4, 0);
    lv_obj_set_style_text_color(bitrate_dd, THEME_COLOR_TEXT_MUTED, LV_PART_INDICATOR);
    lv_obj_add_event_cb(bitrate_dd, bitrate_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_bitrate_dropdown = bitrate_dd;

    lv_obj_t *ecu_label = lv_label_create(s);
    lv_label_set_text(ecu_label, "ECU Type");
    lv_obj_align(ecu_label, LV_ALIGN_TOP_LEFT, 0, 90);
    lv_obj_set_style_text_font(ecu_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(ecu_label, THEME_COLOR_TEXT_MUTED, 0);

    lv_obj_t *ecu_btn = lv_btn_create(s);
    lv_obj_set_size(ecu_btn, lv_pct(62), 32);
    lv_obj_align(ecu_btn, LV_ALIGN_TOP_LEFT, 80, 86);
    lv_obj_set_style_bg_color(ecu_btn, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(ecu_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ecu_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(ecu_btn, 1, 0);
    lv_obj_set_style_radius(ecu_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(ecu_btn, 0, 0);
    s_ecu_value_label = lv_label_create(ecu_btn);
    {
        char txt[64];
        _ecu_label_compose(txt, sizeof(txt));
        lv_label_set_text(s_ecu_value_label, txt);
    }
    lv_label_set_long_mode(s_ecu_value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ecu_value_label, lv_pct(95));
    lv_obj_align(s_ecu_value_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(s_ecu_value_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_ecu_value_label, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(ecu_btn, _ecu_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void _build_section_device_info(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "DEVICE INFO");

    lv_obj_t *serial_label = lv_label_create(s);
    lv_label_set_text(serial_label, "Serial Number");
    lv_obj_align(serial_label, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_text_font(serial_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(serial_label, THEME_COLOR_TEXT_HINT, 0);

    lv_obj_t *serial_value = lv_label_create(s);
    char serial[MAX_SERIAL_LENGTH];
    if (get_device_serial(serial) == ESP_OK) {
        lv_label_set_text(serial_value, serial);
    } else {
        lv_label_set_text(serial_value, "Unknown");
    }
    lv_obj_align(serial_value, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_set_style_text_font(serial_value, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(serial_value, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *fw_label = lv_label_create(s);
    lv_label_set_text(fw_label, "Firmware");
    lv_obj_align(fw_label, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_set_style_text_font(fw_label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(fw_label, THEME_COLOR_TEXT_HINT, 0);

    lv_obj_t *fw_value = lv_label_create(s);
    lv_label_set_text(fw_value, FIRMWARE_VERSION);
    lv_obj_align(fw_value, LV_ALIGN_TOP_LEFT, 0, 76);
    lv_obj_set_style_text_font(fw_value, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(fw_value, THEME_COLOR_TEXT_PRIMARY, 0);
}

static void _build_section_network(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "NETWORK & UPDATES");

    lv_obj_t *wifi_btn = lv_btn_create(s);
    lv_obj_set_size(wifi_btn, 180, 30);
    lv_obj_align(wifi_btn, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(wifi_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(wifi_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(wifi_btn, 0, 0);
    lv_obj_set_style_shadow_width(wifi_btn, 0, 0);
    lv_obj_t *wifi_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_label, "Wi-Fi Settings");
    lv_obj_center(wifi_label);
    lv_obj_set_style_text_font(wifi_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(wifi_label, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(wifi_btn, wifi_btn_event_cb, LV_EVENT_CLICKED, NULL);

    wifi_status_label = lv_label_create(s);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_text_font(wifi_status_label, THEME_FONT_TINY, 0);

    web_status_label = lv_label_create(s);
    lv_obj_align(web_status_label, LV_ALIGN_TOP_LEFT, 0, 75);
    lv_obj_set_style_text_font(web_status_label, THEME_FONT_TINY, 0);

    ap_status_label = lv_label_create(s);
    lv_obj_align(ap_status_label, LV_ALIGN_TOP_LEFT, 0, 90);
    lv_obj_set_style_text_font(ap_status_label, THEME_FONT_TINY, 0);

    refresh_wifi_status();

    lv_obj_t *qr_btn = lv_btn_create(s);
    lv_obj_set_size(qr_btn, 110, 30);
    lv_obj_align(qr_btn, LV_ALIGN_TOP_LEFT, 0, 115);
    lv_obj_set_style_bg_color(qr_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_bg_opa(qr_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(qr_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(qr_btn, 0, 0);
    lv_obj_set_style_shadow_width(qr_btn, 0, 0);
    lv_obj_t *qr_label = lv_label_create(qr_btn);
    lv_label_set_text(qr_label, "Show QR");
    lv_obj_center(qr_label);
    lv_obj_set_style_text_font(qr_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(qr_label, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(qr_btn, _qr_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *update_btn = lv_btn_create(s);
    lv_obj_set_size(update_btn, 140, 30);
    lv_obj_align(update_btn, LV_ALIGN_TOP_LEFT, 120, 115);
    lv_obj_set_style_bg_color(update_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(update_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(update_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(update_btn, 1, 0);
    lv_obj_set_style_border_color(update_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(update_btn, 0, 0);
    lv_obj_t *update_label = lv_label_create(update_btn);
    lv_label_set_text(update_label, "Check Updates");
    lv_obj_center(update_label);
    lv_obj_set_style_text_font(update_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(update_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(update_btn, update_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

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

static void _build_section_data_logging(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "DATA LOGGING");

    s_log_btn = lv_btn_create(s);
    lv_obj_set_size(s_log_btn, 130, 30);
    lv_obj_align(s_log_btn, LV_ALIGN_TOP_LEFT, 0, 22);
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

    s_log_rate_dd = lv_dropdown_create(s);
    lv_dropdown_set_options_static(s_log_rate_dd,
        "1 Hz\n2 Hz\n5 Hz\n10 Hz\n20 Hz\n50 Hz\n100 Hz\n200 Hz\nMax");
    lv_obj_set_size(s_log_rate_dd, 90, 30);
    lv_obj_align(s_log_rate_dd, LV_ALIGN_TOP_LEFT, 138, 22);
    lv_obj_set_style_bg_color(s_log_rate_dd, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_color(s_log_rate_dd, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_log_rate_dd, 1, 0);
    lv_obj_set_style_radius(s_log_rate_dd, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_text_color(s_log_rate_dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(s_log_rate_dd, THEME_FONT_SMALL, 0);
    lv_dropdown_set_selected(s_log_rate_dd,
                             _log_rate_hz_to_idx(data_logger_get_rate_hz()));
    lv_obj_add_event_cb(s_log_rate_dd, _log_rate_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_log_status_label = lv_label_create(s);
    lv_label_set_text(s_log_status_label, "Stopped");
    lv_obj_align(s_log_status_label, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_set_style_text_font(s_log_status_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_log_status_label, THEME_COLOR_TEXT_MUTED, 0);

    /* Raw CAN Capture — records every CAN frame (no decoding) to its own
     * CSV. Useful for users sending unknown-protocol traces off-device for
     * decoding. Uses the same SD/LFS fallback + 1 MB cap as the signal log. */
    s_canraw_btn = lv_btn_create(s);
    lv_obj_set_size(s_canraw_btn, 150, 30);
    lv_obj_align(s_canraw_btn, LV_ALIGN_TOP_LEFT, 240, 22);
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

    s_canraw_status_label = lv_label_create(s);
    lv_label_set_text(s_canraw_status_label, "Raw: idle");
    lv_obj_align(s_canraw_status_label, LV_ALIGN_TOP_LEFT, 280, 56);
    lv_obj_set_style_text_font(s_canraw_status_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_canraw_status_label, THEME_COLOR_TEXT_MUTED, 0);

    /* Share Raw CAN — opens a modal that uploads the latest canraw_*.csv
     * to the project's cloud bucket tagged with the user-entered car
     * make/model. Uses can_upload_start() under the hood (same path as
     * the web editor's Share button). Sits to the right of Start Raw CAN
     * (240+150+10 = 400 px from the section's left edge). */
    lv_obj_t *share_btn = lv_btn_create(s);
    lv_obj_set_size(share_btn, 150, 30);
    lv_obj_align(share_btn, LV_ALIGN_TOP_LEFT, 400, 22);
    lv_obj_set_style_bg_color(share_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(share_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(share_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(share_btn, 1, 0);
    lv_obj_set_style_border_color(share_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(share_btn, 0, 0);
    lv_obj_t *share_lbl = lv_label_create(share_btn);
    lv_label_set_text(share_lbl, "Share Raw CAN");
    lv_obj_center(share_lbl);
    lv_obj_set_style_text_font(share_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(share_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(share_btn, _share_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void _share_btn_cb(lv_event_t *e) {
    (void)e;
    _share_modal_open();
}

static void _build_section_peak_hold(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "PEAK HOLD");

    lv_obj_t *view_btn = lv_btn_create(s);
    lv_obj_set_size(view_btn, 110, 30);
    lv_obj_align(view_btn, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(view_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(view_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(view_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(view_btn, 1, 0);
    lv_obj_set_style_border_color(view_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(view_btn, 0, 0);
    lv_obj_t *view_lbl = lv_label_create(view_btn);
    lv_label_set_text(view_lbl, "View Peaks");
    lv_obj_center(view_lbl);
    lv_obj_set_style_text_font(view_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(view_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(view_btn, _view_peaks_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset_peaks_btn = lv_btn_create(s);
    lv_obj_set_size(reset_peaks_btn, 110, 30);
    lv_obj_align(reset_peaks_btn, LV_ALIGN_TOP_LEFT, 118, 22);
    lv_obj_set_style_bg_color(reset_peaks_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(reset_peaks_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(reset_peaks_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(reset_peaks_btn, 1, 0);
    lv_obj_set_style_border_color(reset_peaks_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(reset_peaks_btn, 0, 0);
    lv_obj_t *reset_peaks_lbl = lv_label_create(reset_peaks_btn);
    lv_label_set_text(reset_peaks_lbl, "Reset Peaks");
    lv_obj_center(reset_peaks_lbl);
    lv_obj_set_style_text_font(reset_peaks_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(reset_peaks_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(reset_peaks_btn, _reset_peaks_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *peak_note = lv_label_create(s);
    lv_label_set_text(peak_note, "All-time - persists until reset");
    lv_obj_align(peak_note, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_text_font(peak_note, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(peak_note, THEME_COLOR_TEXT_MUTED, 0);
}

static void _build_section_testing(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "TESTING");

    lv_obj_t *sim_btn = lv_btn_create(s);
    lv_obj_set_size(sim_btn, 130, 30);
    lv_obj_align(sim_btn, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(sim_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(sim_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(sim_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(sim_btn, 1, 0);
    lv_obj_set_style_border_color(sim_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(sim_btn, 0, 0);
    s_sim_btn_label = lv_label_create(sim_btn);
    {
        bool on = signal_sim_is_active();
        lv_label_set_text(s_sim_btn_label, on ? "Sim: ON" : "Sim: OFF");
        lv_obj_set_style_text_color(s_sim_btn_label,
            on ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_TEXT_MUTED, 0);
    }
    lv_obj_center(s_sim_btn_label);
    lv_obj_set_style_text_font(s_sim_btn_label, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(sim_btn, _sim_toggle_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Wire Inputs toggle — moved here from the (hidden) Developer section
     * so users can flip GPIO 43/44 between UART1 mode and indicator-wire
     * input mode without needing a serial console. Affects pull
     * configuration on those pins (see wire_inputs_init / uart_protocol). */
    lv_obj_t *wire_btn = lv_btn_create(s);
    lv_obj_set_size(wire_btn, 150, 30);
    lv_obj_align(wire_btn, LV_ALIGN_TOP_LEFT, 140, 22);
    lv_obj_set_style_bg_color(wire_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(wire_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(wire_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(wire_btn, 1, 0);
    lv_obj_set_style_border_color(wire_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(wire_btn, 0, 0);
    s_wire_input_btn_label = lv_label_create(wire_btn);
    {
        bool on = false;
        config_store_load_wire_input_mode(&on);
        lv_label_set_text(s_wire_input_btn_label, on ? "Wire Inputs: ON" : "Wire Inputs: OFF");
        lv_obj_set_style_text_color(s_wire_input_btn_label,
            on ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_TEXT_MUTED, 0);
    }
    lv_obj_center(s_wire_input_btn_label);
    lv_obj_set_style_text_font(s_wire_input_btn_label, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(wire_btn, _wire_input_mode_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *test_note = lv_label_create(s);
    lv_label_set_text(test_note, "Sim sweeps all signals  •  Wire Inputs: GPIO 43/44 as turn signals");
    lv_obj_align(test_note, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_text_font(test_note, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(test_note, THEME_COLOR_TEXT_MUTED, 0);
}

__attribute__((unused))
static void _build_section_developer(lv_obj_t *row) {
    lv_obj_t *s = _make_flex_section(row);
    _make_section_title(s, "DEVELOPER");

    lv_obj_t *wire_btn = lv_btn_create(s);
    lv_obj_set_size(wire_btn, 150, 30);
    lv_obj_align(wire_btn, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(wire_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(wire_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(wire_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(wire_btn, 1, 0);
    lv_obj_set_style_border_color(wire_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(wire_btn, 0, 0);
    s_wire_input_btn_label = lv_label_create(wire_btn);
    {
        bool on = false;
        config_store_load_wire_input_mode(&on);
        lv_label_set_text(s_wire_input_btn_label, on ? "Wire Inputs: ON" : "Wire Inputs: OFF");
        lv_obj_set_style_text_color(s_wire_input_btn_label,
            on ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_TEXT_MUTED, 0);
    }
    lv_obj_center(s_wire_input_btn_label);
    lv_obj_set_style_text_font(s_wire_input_btn_label, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(wire_btn, _wire_input_mode_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wire_note = lv_label_create(s);
    lv_label_set_text(wire_note, "GPIO 43/44 as turn signal inputs");
    lv_obj_align(wire_note, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_text_font(wire_note, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(wire_note, THEME_COLOR_TEXT_MUTED, 0);
}

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

    lv_obj_t *reset_btn = lv_btn_create(content);
    lv_obj_set_size(reset_btn, lv_pct(100), 34);
    lv_obj_set_style_bg_color(reset_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(reset_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(reset_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_color(reset_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(reset_btn, 1, 0);
    lv_obj_set_style_shadow_width(reset_btn, 0, 0);
    lv_obj_t *reset_label = lv_label_create(reset_btn);
    lv_label_set_text(reset_label, "Reset to Default");
    lv_obj_center(reset_label);
    lv_obj_set_style_text_font(reset_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(reset_label, THEME_COLOR_STATUS_ERROR, 0);
    lv_obj_add_event_cb(reset_btn, _factory_reset_btn_cb, LV_EVENT_CLICKED, NULL);
}

void device_settings_with_return_screen(lv_obj_t* return_screen) {
    device_settings_return_screen = return_screen ? return_screen : lv_scr_act();

    lv_obj_t *settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settings_screen, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(settings_screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);

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

    /* Row 1 (h=160): CAN BUS config + DEVICE INFO */
    lv_obj_t *row1 = _build_row(content, 160);
    _build_section_can_config(row1);
    _build_section_device_info(row1);

    /* Row 2 (h=260): NETWORK & UPDATES + DISPLAY */
    lv_obj_t *row2 = _build_row(content, 260);
    _build_section_network(row2);
    _build_section_display(row2);

    /* Rows 3/4/5: DATA LOGGING / PEAK HOLD / TESTING each get a full-width
     * row of their own — previously crammed into one 3-column 95 px row
     * which left no horizontal room for additional controls (e.g. the new
     * Raw CAN Capture button in DATA LOGGING). */
    lv_obj_t *log_row    = _build_row(content, 95);
    _build_section_data_logging(log_row);
    lv_obj_t *peak_row   = _build_row(content, 95);
    _build_section_peak_hold(peak_row);
    lv_obj_t *test_row   = _build_row(content, 95);
    _build_section_testing(test_row);

    /* Row 4 (h=95): DEVELOPER OPTIONS — hidden in production. To re-enable,
     * uncomment the two lines below; the _build_section_developer function
     * is still compiled in. */
    /*
    lv_obj_t *dev_row = _build_row(content, 95);
    _build_section_developer(dev_row);
    */

    _build_section_can_diagnostics(content);
    _build_action_buttons(content);

    /* Restore persisted bitrate into the dropdown built by _build_section_can_config */
    uint8_t saved_bitrate = 2; /* default 500 kbps */
    config_store_load_bitrate(&saved_bitrate);
    lv_dropdown_set_selected(s_bitrate_dropdown, saved_bitrate);

    _update_log_ui();

    if (s_log_status_timer) lv_timer_del(s_log_status_timer);
    s_log_status_timer = lv_timer_create(_log_status_timer_cb, 1000, NULL);

    if (s_wifi_status_timer) lv_timer_del(s_wifi_status_timer);
    s_wifi_status_timer = lv_timer_create(refresh_wifi_status_timer_cb, 2000, NULL);

    if (s_can_diag_timer) lv_timer_del(s_can_diag_timer);
    s_can_diag_timer = lv_timer_create(refresh_can_diag_timer_cb, 1000, NULL);

    lv_scr_load(settings_screen);
}

void device_settings_longpress_cb(lv_event_t* e) {
    device_settings_with_return_screen(NULL);
}