#include "device_settings.h"
#include "system/safe_restart.h"
#include "esp_attr.h"
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
#include "data/obd2_autosetup.h"
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
#include "kit/ui_kit.h"
#include "menu/main_menu.h"
#include "system/rdm_lv_async.h"

// Global WiFi status labels for updating
static lv_obj_t* wifi_status_label = NULL;
static lv_obj_t* web_status_label = NULL;
static lv_timer_t *s_wifi_status_timer = NULL;
static lv_obj_t* wifi_loading_dialog = NULL;

/* Web-editor QR popup (Connect page -> "Web editor"). Phone scans the QR,
 * browser opens the editor directly — bypasses flaky .local resolution.
 * s_qr_overlay is the kit popup card; children (and the backdrop) go with it. */
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

/* Stat label of the This dash page's "Simulator" tile (tap toggles sim on/off) */
static lv_obj_t *s_sim_card_stat = NULL;

static lv_timer_t *s_log_status_timer = NULL;

/* Odometer popup's live reading + the "Change the reading" keypad popup.
 * Hoisted to file scope so the settings-screen teardown can close the
 * keypad popup and null them without forward declarations. */
static lv_obj_t   *s_veh_odo_value_lbl = NULL;
static lv_timer_t *s_veh_odo_timer     = NULL;
static lv_obj_t   *s_odo_edit_overlay  = NULL;
static lv_obj_t   *s_odo_edit_textarea = NULL;

/* Repaints the CAN bus tile's stat once a second. */
static lv_timer_t *s_can_diag_timer    = NULL;

/* Bus-speed dropdown in the CAN bus popup. */
static lv_obj_t  *s_bitrate_dropdown   = NULL;

/* Stat line on the CAN bus card in the setup grid. Held so every path that
 * moves the bitrate can rewrite it: the card is what you read walking past,
 * and it used to carry the string literal "500 KBPS" no matter what the bus
 * was doing — a dash on 1 Mbps said 500 on the card and 1 Mbps in the popup
 * that sets it. */
static lv_obj_t  *s_can_bitrate_stat_label = NULL;

/* The rate the TWAI driver is installed at right now. Live index rather than
 * NVS: a bus scan or a wizard step moves the driver, and the question the
 * card answers is "what is this bus running at", not "what did someone last
 * save". */
static const char *_bitrate_stat_text(void) {
    static const char *labels[] = {"125 KBPS", "250 KBPS", "500 KBPS", "1 MBPS"};
    uint8_t idx = can_get_bitrate_index();
    return labels[idx > 3 ? 2 : idx];
}

static void _refresh_can_bitrate_stat(void) {
    /* lv_obj_is_valid, not just NULL: the pointer survives a screen delete
     * that skipped the teardown path. */
    if (s_can_bitrate_stat_label && lv_obj_is_valid(s_can_bitrate_stat_label))
        lv_label_set_text(s_can_bitrate_stat_label, _bitrate_stat_text());
}

/* Small muted wrapped note at an absolute spot in a popup. (_popup_note,
 * further down, is the same thing pinned to the left edge.) */
static lv_obj_t *_popup_note_at(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, const char *text) {
    lv_obj_t *n = uk_label(parent, text, UK_FONT_SMALL, UK_TONE_MUTED);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(n, w);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_text_line_space(n, 3, 0);
    return n;
}

/* Build the URL the user should scan to reach the web editor. Returns true
 * on success; url is "http://<ip>/" null-terminated.
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
    /* A stale pointer can linger if the settings screen went away while the
     * popup was open; re-check validity so the tile still works. */
    if (s_qr_overlay && lv_obj_is_valid(s_qr_overlay)) return;
    s_qr_overlay = NULL;

    char url[64];
    bool have_url = _build_web_url(url, sizeof(url));

    if (!have_url) {
        s_qr_overlay = uk_popup(480, 200, "Web editor", _qr_close_cb);
        _popup_note_at(s_qr_overlay, 0, UK_POPUP_BODY_Y, 444,
            "The dash isn't on a network yet. Join a WiFi network or turn on "
            "the hotspot in WiFi settings, then come back here.");
        return;
    }

    /* Code on the left, where to point the phone on the right. */
    s_qr_overlay = uk_popup(620, 400, "Web editor", _qr_close_cb);

    /* Black modules on white, 280px — readable from 30-60cm away */
    lv_obj_t *qr = lv_qrcode_create(s_qr_overlay, 280,
                                    lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    /* White quiet-zone ring around QR improves scanner hit-rate */
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    const lv_coord_t col_x = 316, col_w = 268;

    lv_obj_t *sec = uk_section(s_qr_overlay, "Address");
    lv_obj_align(sec, LV_ALIGN_TOP_LEFT, col_x, UK_POPUP_BODY_Y);

    lv_obj_t *url_lbl = uk_label(s_qr_overlay, url, UK_FONT_BODY, UK_TONE_TEXT);
    lv_label_set_long_mode(url_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url_lbl, col_w);
    lv_obj_align(url_lbl, LV_ALIGN_TOP_LEFT, col_x, 82);

    _popup_note_at(s_qr_overlay, col_x, 118, col_w,
        "Point your phone's camera at the code. The phone has to be on the "
        "dash's hotspot or on the same network as the dash.");

    /* If the URL is the STA-side IP (i.e. AP is off and the dash is
     * connected to a router/hotspot), the scanning device must be on
     * that same network. Most phone-hotspot APs enable client isolation
     * by default, which silently drops traffic from the phone to the
     * dash even when both are on the hotspot. Surface this so the user
     * doesn't blame the QR. AP mode (192.168.4.1) sidesteps the issue. */
    if (!wifi_manager_is_ap_enabled()) {
        _popup_note_at(s_qr_overlay, col_x, 224, col_w,
            "Page won't load? A phone hotspot usually blocks this. Turn on "
            "the dash's own hotspot in WiFi settings instead.");
    }

    /* Stash pointers + current URL, then poll once a second so the QR
     * re-renders live if the user toggles AP or the STA DHCP lease
     * lands after the popup was already opened. */
    s_qr_obj     = qr;
    s_qr_url_lbl = url_lbl;
    strncpy(s_qr_last_url, url, sizeof(s_qr_last_url) - 1);
    s_qr_last_url[sizeof(s_qr_last_url) - 1] = '\0';
    if (s_qr_refresh_timer) lv_timer_del(s_qr_refresh_timer);
    s_qr_refresh_timer = lv_timer_create(_qr_refresh_tick_cb, 1000, NULL);
}

/* Refresh the WiFi + Web editor tile stats. wifi_status_label and
 * web_status_label are the status lines of those two tiles on the Connect
 * page (see _build_connect_page). Stats are short,
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
/* Dim from the dock or the Screen popup: down to the dimmer's dim level and
 * back to exactly what it was. Not saved — like the slider, it lasts until
 * the next boot. */
static bool    s_dimmed            = false;
static uint8_t s_brightness_undim  = 100;

bool display_is_dimmed(void) {
    return s_dimmed;
}

void display_toggle_dim(void) {
    if (s_dimmed) {
        s_dimmed = false;
        set_display_brightness(s_brightness_undim);
        return;
    }
    s_brightness_undim = current_brightness;
    uint8_t level = dimmer_config.dim_brightness;
    if (level < 5 || level >= current_brightness) level = current_brightness / 2;
    if (level < 5) level = 5;
    s_dimmed = true;
    set_display_brightness(level);
}

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

/* "Dim automatically" popup (Screen popup -> "Dim automatically..."). A kit
 * popup on lv_layer_top, stacked over the Screen popup; closing it drops you
 * back there. The controls are file-scope so Save can read them and every
 * close path can null them (the old heap-allocated pointer bundle leaked
 * whenever the popup was dismissed without saving). */
static lv_obj_t *s_dimcfg_popup     = NULL;
static lv_obj_t *s_dimcfg_signal_dd = NULL;
static lv_obj_t *s_dimcfg_thresh_ta = NULL;
static lv_obj_t *s_dimcfg_mode_dd   = NULL;
static lv_obj_t *s_dimcfg_invert_sw = NULL;
static lv_obj_t *s_dimcfg_enable_sw = NULL;
static lv_obj_t *s_dimcfg_level_sl  = NULL;
static lv_obj_t *s_dimcfg_kb        = NULL;

/* The threshold field brings up a number pad inside the popup. The shared
 * text-input dialog (keyboard_event_cb) builds on lv_scr_act(), which sits
 * under lv_layer_top, so it would open behind this popup and its backdrop. */
static void _dimcfg_kb_hide(void) {
    if (s_dimcfg_kb && lv_obj_is_valid(s_dimcfg_kb)) {
        lv_keyboard_set_textarea(s_dimcfg_kb, NULL);
        lv_obj_add_flag(s_dimcfg_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_dimcfg_thresh_ta && lv_obj_is_valid(s_dimcfg_thresh_ta))
        lv_obj_clear_state(s_dimcfg_thresh_ta, LV_STATE_FOCUSED);
}

static void _dimcfg_thresh_clicked_cb(lv_event_t *e) {
    (void)e;
    if (!s_dimcfg_kb || !lv_obj_is_valid(s_dimcfg_kb)) return;
    lv_keyboard_set_textarea(s_dimcfg_kb, s_dimcfg_thresh_ta);
    lv_obj_clear_flag(s_dimcfg_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_dimcfg_kb);
}

static void _dimcfg_kb_done_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) _dimcfg_kb_hide();
}

// Brightness dimmer switch configuration popup
static void brightness_dimmer_config_cb(lv_event_t * e) {
    (void)e;
    if (s_dimcfg_popup && lv_obj_is_valid(s_dimcfg_popup)) return;

    /* Inner area is 564 x 384 (the kit card pads 18). */
    s_dimcfg_popup = uk_popup(600, 420, "Dim automatically", close_dimmer_popup_cb);
    lv_obj_t *p = s_dimcfg_popup;

    /* Row 1: which reading, and the value that counts as "on" */
    lv_obj_t *sig_sec = uk_section(p, "Reading");
    lv_obj_align(sig_sec, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    s_dimcfg_signal_dd = lv_dropdown_create(p);
    lv_obj_set_size(s_dimcfg_signal_dd, 356, UK_BTN_H);
    lv_obj_align(s_dimcfg_signal_dd, LV_ALIGN_TOP_LEFT, 0, 80);
    uk_style_dropdown(s_dimcfg_signal_dd);
    {
        static EXT_RAM_BSS_ATTR char sig_options[1024];
        uint16_t sel = _build_signal_options(sig_options, sizeof(sig_options));
        lv_dropdown_set_options(s_dimcfg_signal_dd, sig_options);
        lv_dropdown_set_selected(s_dimcfg_signal_dd, sel);
    }

    lv_obj_t *thr_sec = uk_section(p, "On at or above");
    lv_obj_align(thr_sec, LV_ALIGN_TOP_LEFT, 372, UK_POPUP_BODY_Y);

    s_dimcfg_thresh_ta = lv_textarea_create(p);
    lv_obj_set_size(s_dimcfg_thresh_ta, 192, UK_BTN_H);
    lv_obj_align(s_dimcfg_thresh_ta, LV_ALIGN_TOP_LEFT, 372, 80);
    lv_textarea_set_one_line(s_dimcfg_thresh_ta, true);
    lv_textarea_set_max_length(s_dimcfg_thresh_ta, 8);
    lv_textarea_set_accepted_chars(s_dimcfg_thresh_ta, "0123456789.-");
    uk_style_textarea(s_dimcfg_thresh_ta);
    char thresh_str[16];
    snprintf(thresh_str, sizeof(thresh_str), "%.2f", dimmer_config.threshold);
    lv_textarea_set_text(s_dimcfg_thresh_ta, thresh_str);
    lv_obj_add_event_cb(s_dimcfg_thresh_ta, _dimcfg_thresh_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Row 2: how a press behaves, reverse, and the master switch.
     * Option order matters: index 1 = momentary (see save_dimmer_config_cb). */
    lv_obj_t *mode_sec = uk_section(p, "Works like");
    lv_obj_align(mode_sec, LV_ALIGN_TOP_LEFT, 0, 136);

    s_dimcfg_mode_dd = lv_dropdown_create(p);
    lv_dropdown_set_options(s_dimcfg_mode_dd, "Press to toggle\nDim while on");
    lv_obj_set_size(s_dimcfg_mode_dd, 240, UK_BTN_H);
    lv_obj_align(s_dimcfg_mode_dd, LV_ALIGN_TOP_LEFT, 0, 160);
    uk_style_dropdown(s_dimcfg_mode_dd);
    lv_dropdown_set_selected(s_dimcfg_mode_dd, dimmer_config.is_momentary ? 1 : 0);

    lv_obj_t *inv_sec = uk_section(p, "Reverse");
    lv_obj_align(inv_sec, LV_ALIGN_TOP_LEFT, 272, 136);

    s_dimcfg_invert_sw = lv_switch_create(p);
    lv_obj_set_size(s_dimcfg_invert_sw, 50, 25);
    lv_obj_align(s_dimcfg_invert_sw, LV_ALIGN_TOP_LEFT, 272, 167);
    uk_style_switch(s_dimcfg_invert_sw);
    if (dimmer_config.invert) lv_obj_add_state(s_dimcfg_invert_sw, LV_STATE_CHECKED);

    lv_obj_t *en_sec = uk_section(p, "Turned on");
    lv_obj_align(en_sec, LV_ALIGN_TOP_LEFT, 372, 136);

    s_dimcfg_enable_sw = lv_switch_create(p);
    lv_obj_set_size(s_dimcfg_enable_sw, 50, 25);
    lv_obj_align(s_dimcfg_enable_sw, LV_ALIGN_TOP_LEFT, 372, 167);
    uk_style_switch(s_dimcfg_enable_sw);
    if (dimmer_config.enabled) lv_obj_add_state(s_dimcfg_enable_sw, LV_STATE_CHECKED);

    /* Row 3: how dim. Dragging previews it for two seconds. */
    lv_obj_t *lvl_sec = uk_section(p, "Dim to");
    lv_obj_align(lvl_sec, LV_ALIGN_TOP_LEFT, 0, 216);

    s_dimcfg_level_sl = lv_slider_create(p);
    lv_obj_set_size(s_dimcfg_level_sl, 460, 8);
    lv_obj_align(s_dimcfg_level_sl, LV_ALIGN_TOP_LEFT, 0, 256);
    lv_slider_set_range(s_dimcfg_level_sl, 5, 100);
    lv_slider_set_value(s_dimcfg_level_sl, dimmer_config.dim_brightness, LV_ANIM_OFF);
    uk_style_slider(s_dimcfg_level_sl);
    lv_obj_set_ext_click_area(s_dimcfg_level_sl, 18);

    lv_obj_t *level_value = uk_label(p, "", UK_FONT_TITLE, UK_TONE_TEXT);
    lv_label_set_text_fmt(level_value, "%d%%", dimmer_config.dim_brightness);
    lv_obj_align(level_value, LV_ALIGN_TOP_RIGHT, 0, 244);

    lv_obj_add_event_cb(s_dimcfg_level_sl, brightness_set_slider_cb, LV_EVENT_VALUE_CHANGED, level_value);

    _popup_note_at(p, 0, 288, 380,
        "Most switches read 1 when on and 0 when off, so 0.5 suits them. "
        "Reverse dims when the reading is below it instead.");

    lv_obj_t *save = uk_btn(p, UK_ICON_CHECK, "Save", UK_BTN_PRIMARY, save_dimmer_config_cb, NULL);
    lv_obj_set_width(save, 160);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    /* Number pad for the threshold — created last so it draws over the
     * lower rows while it is up. Hidden until the field is tapped. */
    s_dimcfg_kb = lv_keyboard_create(p);
    lv_obj_set_size(s_dimcfg_kb, lv_pct(100), 200);
    lv_obj_align(s_dimcfg_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_dimcfg_kb, LV_KEYBOARD_MODE_NUMBER);
    uk_style_keyboard(s_dimcfg_kb);
    lv_obj_add_event_cb(s_dimcfg_kb, _dimcfg_kb_done_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_dimcfg_kb, LV_OBJ_FLAG_HIDDEN);
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
    (void)e;
    if (s_dimcfg_popup && lv_obj_is_valid(s_dimcfg_popup)) lv_obj_del(s_dimcfg_popup);
    s_dimcfg_popup     = NULL;
    s_dimcfg_signal_dd = NULL;
    s_dimcfg_thresh_ta = NULL;
    s_dimcfg_mode_dd   = NULL;
    s_dimcfg_invert_sw = NULL;
    s_dimcfg_enable_sw = NULL;
    s_dimcfg_level_sl  = NULL;
    s_dimcfg_kb        = NULL;
}

static void save_dimmer_config_cb(lv_event_t * e) {
    (void)e;
    if (!s_dimcfg_popup || !lv_obj_is_valid(s_dimcfg_popup)) return;

    // Get signal name from dropdown
    char sig_buf[32];
    lv_dropdown_get_selected_str(s_dimcfg_signal_dd, sig_buf, sizeof(sig_buf));
    strncpy(dimmer_config.signal_name, sig_buf, sizeof(dimmer_config.signal_name) - 1);
    dimmer_config.signal_name[sizeof(dimmer_config.signal_name) - 1] = '\0';

    // Get threshold
    const char* thresh_str = lv_textarea_get_text(s_dimcfg_thresh_ta);
    dimmer_config.threshold = strtof(thresh_str, NULL);

    // Get toggle mode
    dimmer_config.is_momentary = (lv_dropdown_get_selected(s_dimcfg_mode_dd) == 1);

    // Get invert
    dimmer_config.invert = lv_obj_has_state(s_dimcfg_invert_sw, LV_STATE_CHECKED);

    // Get brightness value
    dimmer_config.dim_brightness = lv_slider_get_value(s_dimcfg_level_sl);

    // Get enabled state
    dimmer_config.enabled = lv_obj_has_state(s_dimcfg_enable_sw, LV_STATE_CHECKED);

    // Reset toggle state when saving
    s_dimmer_toggle_state = false;

    // Save to NVS
    save_dimmer_config_to_nvs();

    // Re-subscribe to the (potentially new) signal
    dimmer_subscribe();

    // Close popup
    close_dimmer_popup_cb(NULL);

    ESP_LOGI("DIMMER", "Dimmer config saved: Signal='%s', Thresh=%.2f, Mode=%s, Invert=%d, Brightness=%d%%, Enabled=%d",
        dimmer_config.signal_name, dimmer_config.threshold,
        dimmer_config.is_momentary ? "Momentary" : "Toggle",
        dimmer_config.invert, dimmer_config.dim_brightness, dimmer_config.enabled);
}

/* The CAN bus tile's stat is the only live thing left on the Your car page
 * that this timer paints (the old health panel went with the card grid). */
static void refresh_can_diag_timer_cb(lv_timer_t* timer) {
    (void)timer;
    _refresh_can_bitrate_stat();
}

/* _view_peaks_btn_cb tears down the Recording popup before swapping
 * screens; its definition lives further down with the other popups. */
static void _logger_popup_close(lv_event_t *e);

/* Bitrate dropdown callback — save to NVS then apply via can_manager */
static void bitrate_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dd = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dd);

    config_store_save_bitrate((uint8_t)selected);

    /* Apply the new bitrate (stops task, reinits TWAI, restarts task) */
    can_change_bitrate((uint8_t)selected);

    /* The card behind this popup carries the rate, and closing the popup is
     * how you get back to it. Rewrite it now rather than leaving the old
     * number sitting there. */
    _refresh_can_bitrate_stat();
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

static void _wifi_loading_close_cb(lv_event_t *e) {
    (void)e;
    if (wifi_loading_dialog && lv_obj_is_valid(wifi_loading_dialog))
        lv_obj_del(wifi_loading_dialog);
    wifi_loading_dialog = NULL;
}

// WiFi button callback
static void wifi_btn_event_cb(lv_event_t *e) {
    ESP_LOGI("dev_set", "[trace] wifi_btn_event_cb ENTER");
    // Guard against double-tap while loading dialog is already showing
    if (wifi_loading_dialog && lv_obj_is_valid(wifi_loading_dialog)) return;

    /* Brief "searching" popup: it is on the glass for the frame the WiFi
     * screen takes to build (that build blocks the LVGL task). */
    wifi_loading_dialog = uk_popup(320, 170, "WiFi", _wifi_loading_close_cb);

    lv_obj_t* spinner = lv_spinner_create(wifi_loading_dialog, 1000, 60);
    lv_obj_set_size(spinner, 36, 36);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, UK_POPUP_BODY_Y);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_INPUT_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);

    lv_obj_t* loading_text = uk_label(wifi_loading_dialog, "Looking for networks...",
                                      UK_FONT_BODY, UK_TONE_MUTED);
    lv_obj_align(loading_text, LV_ALIGN_TOP_MID, 0, 104);

    // Create timer to show WiFi screen after a brief delay (allows dialog to render)
    lv_timer_create(show_wifi_screen_delayed, 100, NULL);
}

/* ── Data Logging ─────────────────────────────────────────────────────── */

/* Raw CAN capture widgets — declared here so _update_log_ui can render
 * their state alongside the signal logger's. Assigned by _logger_popup_open. */
static lv_obj_t *s_canraw_btn          = NULL;
static lv_obj_t *s_canraw_btn_label    = NULL;
static lv_obj_t *s_canraw_status_label = NULL;

/* Share raw CAN popup — a kit popup with two text fields (make + model)
 * and an on-screen keyboard. Tracks upload status via
 * can_upload_get_status() while running. s_share_overlay is the card. */
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

static void _update_log_ui(void) {
    if (!s_log_btn_label || !s_log_status_label) return;

    if (data_logger_is_active()) {
        lv_label_set_text(s_log_btn_label, "Stop recording");
        uk_btn_set_kind(s_log_btn, UK_BTN_DANGER);

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
        lv_label_set_text(s_log_btn_label, "Start recording");
        uk_btn_set_kind(s_log_btn, UK_BTN_PRIMARY);

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
            lv_label_set_text(s_canraw_btn_label, "Stop raw CAN");
            uk_btn_set_kind(s_canraw_btn, UK_BTN_DANGER);
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
            lv_label_set_text(s_canraw_btn_label, "Start raw CAN");
            uk_btn_set_kind(s_canraw_btn, UK_BTN_NEUTRAL);
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
 * Layout: a 760x460 kit popup (Recording popup -> "Share raw CAN...") with:
 *   - file picked automatically (the newest canraw_*.csv on SD or LFS)
 *   - two text fields (make + model) sharing one on-screen keyboard
 *   - status label that polls can_upload_get_status() every 500 ms
 *   - Cancel + Upload buttons, swapped for OK once the upload lands
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
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_STATUS_ERROR, 0);
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
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_STATUS_ERROR, 0);
        return;
    }
    if (!model || !model[0]) {
        lv_label_set_text(s_share_status_lbl, "Enter the car model first");
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_STATUS_ERROR, 0);
        return;
    }
    if (s_share_picked_file[0] == '\0') {
        lv_label_set_text(s_share_status_lbl, "No Raw CAN recording found");
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_STATUS_ERROR, 0);
        return;
    }

    lv_obj_add_state(s_share_upload_btn, LV_STATE_DISABLED);
    esp_err_t err = can_upload_start(s_share_picked_file, make, model, NULL);
    if (err != ESP_OK) {
        lv_label_set_text_fmt(s_share_status_lbl, "Start failed (%s)",
                              err == ESP_ERR_INVALID_STATE ? "another upload running"
                                                           : "internal error");
        lv_obj_set_style_text_color(s_share_status_lbl, THEME_COLOR_STATUS_ERROR, 0);
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

    /* Inner area is 724 x 424 (the kit card pads 18). */
    s_share_overlay = uk_popup(760, 460, "Share raw CAN", _share_close_btn_cb);
    lv_obj_t *p = s_share_overlay;

    _popup_note_at(p, 0, UK_POPUP_BODY_Y, 724,
        "Sends your newest raw CAN recording, with your car's name, so its "
        "data can be worked out later.");

    if (have_file) {
        s_share_file_label = uk_label(p, "", UK_FONT_SMALL, UK_TONE_TEXT);
        lv_label_set_text_fmt(s_share_file_label, "Recording: %s", s_share_picked_file);
    } else {
        s_share_file_label = uk_label(p, "No raw CAN recording yet. Start one first.",
                                      UK_FONT_SMALL, UK_TONE_DANGER);
    }
    lv_obj_align(s_share_file_label, LV_ALIGN_TOP_LEFT, 0, 80);

    /* Two text fields side by side */
    lv_obj_t *make_lbl = uk_section(p, "Make");
    lv_obj_align(make_lbl, LV_ALIGN_TOP_LEFT, 0, 102);

    s_share_make_ta = lv_textarea_create(p);
    lv_obj_set_size(s_share_make_ta, 350, UK_BTN_H);
    lv_obj_align(s_share_make_ta, LV_ALIGN_TOP_LEFT, 0, 124);
    lv_textarea_set_one_line(s_share_make_ta, true);
    lv_textarea_set_max_length(s_share_make_ta, 40);
    lv_textarea_set_placeholder_text(s_share_make_ta, "Toyota");
    uk_style_textarea(s_share_make_ta);
    lv_obj_add_event_cb(s_share_make_ta, _share_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *model_lbl = uk_section(p, "Model and year");
    lv_obj_align(model_lbl, LV_ALIGN_TOP_LEFT, 362, 102);

    s_share_model_ta = lv_textarea_create(p);
    lv_obj_set_size(s_share_model_ta, 362, UK_BTN_H);
    lv_obj_align(s_share_model_ta, LV_ALIGN_TOP_LEFT, 362, 124);
    lv_textarea_set_one_line(s_share_model_ta, true);
    lv_textarea_set_max_length(s_share_model_ta, 40);
    lv_textarea_set_placeholder_text(s_share_model_ta, "Supra MK4 1998");
    uk_style_textarea(s_share_model_ta);
    lv_obj_add_event_cb(s_share_model_ta, _share_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    s_share_status_lbl = uk_label(p, "", UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_align(s_share_status_lbl, LV_ALIGN_TOP_LEFT, 0, 170);

    /* On-screen keyboard between the fields and the buttons */
    s_share_keyboard = lv_keyboard_create(p);
    lv_obj_set_size(s_share_keyboard, lv_pct(100), 180);
    lv_obj_align(s_share_keyboard, LV_ALIGN_BOTTOM_MID, 0, -(UK_BTN_H + 10));
    lv_keyboard_set_mode(s_share_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    uk_style_keyboard(s_share_keyboard);

    /* Buttons row at the very bottom — Cancel (left) and Upload (right)
     * during the entry/in-progress phases; on success they're hidden and
     * the OK button (same right slot) takes over. */
    s_share_cancel_btn = uk_btn(p, UK_ICON_NONE, "Cancel", UK_BTN_NEUTRAL,
                                _share_close_btn_cb, NULL);
    lv_obj_set_width(s_share_cancel_btn, 140);
    lv_obj_align(s_share_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_share_upload_btn = uk_btn(p, UK_ICON_WEB, "Upload", UK_BTN_PRIMARY,
                                _share_upload_btn_cb, NULL);
    lv_obj_set_width(s_share_upload_btn, 160);
    lv_obj_align(s_share_upload_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    /* OK button — replaces Upload + Cancel after a successful upload so the
     * user has a single obvious "I'm done" way back to the Recording popup.
     * Reuses _share_close_btn_cb (same teardown path). */
    s_share_ok_btn = uk_btn(p, UK_ICON_CHECK, "OK", UK_BTN_PRIMARY,
                            _share_close_btn_cb, NULL);
    lv_obj_set_width(s_share_ok_btn, 160);
    lv_obj_align(s_share_ok_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(s_share_ok_btn, LV_OBJ_FLAG_HIDDEN);

    if (!have_file) {
        lv_obj_add_state(s_share_upload_btn, LV_STATE_DISABLED);
    }
}

/* Wipe peak/min for every signal in the registry. Affects all panels with
 * show_peak set (they pull current peak/min on the next signal update). */
/* This dash page's "Simulator" tile tap handler. Tapping the tile toggles the
 * signal simulator and repaints its stat the same way _build_dash_page first
 * paints it (On green / Off muted — it used to flip to "OFF" in accent red,
 * which read like an error). */
static void _sim_card_cb(lv_event_t *e) {
    (void)e;
    if (signal_sim_is_active()) {
        signal_sim_stop();
    } else {
        signal_sim_start();
    }
    if (s_sim_card_stat && lv_obj_is_valid(s_sim_card_stat)) {
        bool on = signal_sim_is_active();
        lv_label_set_text(s_sim_card_stat, on ? "On" : "Off");
        lv_obj_set_style_text_color(s_sim_card_stat,
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
    /* Close the host popup before peaks_ui takes over the screen — popups
     * live on lv_layer_top() so they'd otherwise float above the peaks
     * screen. peaks_ui captures lv_scr_act() as its return screen at call
     * time, so leaving settings_screen active means Back returns to Device
     * Settings (not the dashboard).
     *
     * The button lives in the Recording popup (ADR-0030), so that is the
     * one to close. */
    _logger_popup_close(NULL);
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

/* Show the dashboard and delete the menu screen that was up, so a full-screen
 * tool (diagnostics, the wizard) opens over the dash with nothing left
 * running behind it. */
static void _leave_menu_for_dashboard(void) {
    lv_obj_t *ret = device_settings_return_screen;
    lv_obj_t *old = lv_scr_act();
    if (ret && lv_obj_is_valid(ret)) {
        lv_scr_load(ret);
        if (old && old != ret && lv_obj_is_valid(old)) rdm_obj_del_async(old);
    }
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
    _leave_menu_for_dashboard();
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
    _leave_menu_for_dashboard();
    /* Defer to next LVGL tick so the screen load fully commits first */
    lv_async_call(_show_wizard_async, NULL);
}

/* Message-box buttons are one button matrix, so the kit styles them all
 * neutral. Paint the first one (the action the box asks about) as a kit
 * primary or danger button at draw time; the rest stay neutral. */
static void _msgbox_first_btn_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (!dsc || dsc->class_p != &lv_btnmatrix_class ||
        dsc->type != LV_BTNMATRIX_DRAW_PART_BTN || dsc->id != 0) return;
    if (!dsc->rect_dsc || !dsc->label_dsc) return;
    lv_obj_t *bm = lv_event_get_target(e);
    bool pressed = lv_btnmatrix_get_selected_btn(bm) == dsc->id &&
                   lv_obj_has_state(bm, LV_STATE_PRESSED);
    if ((uk_btn_kind_t)(intptr_t)lv_event_get_user_data(e) == UK_BTN_DANGER) {
        dsc->rect_dsc->bg_color = pressed ? THEME_COLOR_BTN_DANGER : THEME_COLOR_BTN_DANGER_BG;
        dsc->label_dsc->color   = pressed ? THEME_COLOR_TEXT_ON_ACCENT : THEME_COLOR_STATUS_ERROR;
    } else {
        dsc->rect_dsc->bg_color = pressed ? THEME_COLOR_BTN_SAVE_PRESSED : THEME_COLOR_BTN_SAVE;
        dsc->label_dsc->color   = THEME_COLOR_TEXT_ON_ACCENT;
    }
}

/* Kit look for a confirm box, with its first button shown as @p kind. */
static void _style_confirm_msgbox(lv_obj_t *mbox, uk_btn_kind_t kind) {
    uk_style_msgbox(mbox);
    lv_obj_set_width(mbox, 400);
    lv_obj_center(mbox);
    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if (btns)
        lv_obj_add_event_cb(btns, _msgbox_first_btn_draw_cb, LV_EVENT_DRAW_PART_BEGIN,
                            (void *)(intptr_t)kind);
}

static void _run_wizard_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn_txt = lv_msgbox_get_active_btn_text(mbox);
    if (!btn_txt) return;

    if (strcmp(btn_txt, "Run setup") == 0) {
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
    static const char *btns[] = {"Run setup", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(
        NULL,
        "Setup wizard",
        "Runs first-time setup again: the CAN bus, WiFi and your car. "
        "The settings you have now are kept, and you can change them as you go.",
        btns, true);

    /* Not destructive, so the confirm button is the primary one. */
    _style_confirm_msgbox(mbox, UK_BTN_PRIMARY);

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

/* Kit popup cards (uk_popup, on lv_layer_top()). Settings-screen delete cb defensively closes each one (the popup
 * bodies hold the live label pointers that timers paint into; closing
 * NULLs them so we don't dangle past the settings screen's lifetime). */
static lv_obj_t  *s_device_info_overlay   = NULL;
static lv_obj_t  *s_dimmer_overlay        = NULL;
static lv_obj_t  *s_logger_overlay        = NULL;
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
                /* A fresh default layout has no OBD2 PIDs either; a car
                 * that reads part of itself over OBD2 gets them back the
                 * same way it got them first time (ADR-0073). */
                if (p) obd2_autosetup_for_ecu(make, ver);
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
        "Reset layout",
        "Puts the factory \"default\" layout back. Your other layouts and "
        "your channels are not touched.",
        btns, true);

    _style_confirm_msgbox(mbox, UK_BTN_DANGER);

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
        rdm_safe_restart();
    }
    /* Cancel — just close the dialog */
    lv_msgbox_close(mbox);
}

static void _factory_reset_btn_cb(lv_event_t *e) {
    (void)e;
    static const char *btns[] = {"RESET", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(
        NULL,
        "Factory reset",
        "Erases every setting, layout, image, font and preset on this dash, "
        "then restarts it as new.",
        btns, true);

    _style_confirm_msgbox(mbox, UK_BTN_DANGER);

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

/* =========================================================================
 * Setup tiles and popups — the parts every settings page and popup below is
 * built from. A page is a uk_grid of kit tiles; a tile either opens a
 * full-screen tool or one of the kit popups further down. Tiles mirror the
 * cards on Studio's Setup page (ADR-0039).
 * ========================================================================= */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *body_label;
    lv_obj_t *stat_label;
} setup_card_t;

/* One setup card = one kit tile. @p stat is the tile's status line; the
 * returned stat_label is its text, so the live timers keep repainting it by
 * pointer exactly as they did the old cards. */
static setup_card_t _make_setup_card(lv_obj_t *grid, uk_icon_t icon,
                                     const char *title, const char *body,
                                     const char *initial_stat,
                                     lv_event_cb_t on_click,
                                     uk_tile_kind_t kind) {
    setup_card_t r = {0};
    r.card = uk_tile(grid, icon, title, body, kind, on_click, NULL);
    uk_tile_set_status(r.card, NULL,
                       (initial_stat && initial_stat[0]) ? initial_stat : " ",
                       kind == UK_TILE_DANGER ? UK_TONE_DANGER : UK_TONE_TEXT);
    r.stat_label = uk_tile_status_label(r.card);
    return r;
}

/* Every settings popup is a kit popup: title top-left, Close top-right,
 * children from y = UK_POPUP_BODY_Y. */
static lv_obj_t *_make_popup_shell(int w, int h, const char *title,
                                   lv_event_cb_t close_cb) {
    return uk_popup(w, h, title, close_cb);
}

/* A kit button placed at an absolute spot in a popup. */
static lv_obj_t *_popup_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, uk_icon_t icon, const char *text,
                            uk_btn_kind_t kind, lv_event_cb_t cb) {
    lv_obj_t *b = uk_btn(parent, icon, text, kind, cb, NULL);
    if (w > 0) lv_obj_set_width(b, w);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    return b;
}

/* Small muted note under a popup's controls. */
static lv_obj_t *_popup_note(lv_obj_t *parent, lv_coord_t y, lv_coord_t w, const char *text) {
    return _popup_note_at(parent, 0, y, w, text);
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

    s_device_info_overlay = _make_popup_shell(480, 300, "This dash",
                                              _device_info_popup_close);

    lv_obj_t *rows = lv_obj_create(s_device_info_overlay);
    lv_obj_remove_style_all(rows);
    lv_obj_set_size(rows, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(rows, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLLABLE);

    char serial[MAX_SERIAL_LENGTH];
    uk_row(rows, "Serial number",
           get_device_serial(serial) == ESP_OK ? serial : "Unknown");
    uk_row(rows, "Firmware", FIRMWARE_VERSION);

    /* VIN (Mode 09 PID 0x02) and ECU name (PID 0x0A), cached statically. */
    s_vin_label_obj = uk_row(rows, "VIN", s_vin_cache[0] ? s_vin_cache : "Reading...");
    if (!s_vin_cache[0]) obd2_read_vin(_vin_done, NULL);

    s_ecuname_label_obj = uk_row(rows, "ECU", s_ecuname_cache[0] ? s_ecuname_cache : "Reading...");
    if (!s_ecuname_cache[0]) obd2_read_ecu_name(_ecuname_done, NULL);
}

/* =========================================================================
 * Tile popups (Screen / Recording / CAN bus / Odometer). Each is a kit popup
 * on lv_layer_top(); its close fn deletes the card and nulls the live label
 * statics the timers paint into.
 * ========================================================================= */

static void _dimmer_popup_close(lv_event_t *e) {
    (void)e;
    if (s_dimmer_overlay && lv_obj_is_valid(s_dimmer_overlay)) lv_obj_del(s_dimmer_overlay);
    s_dimmer_overlay = NULL;
    brightness_label = NULL;
}

static void _dim_toggle_btn_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_current_target(e);
    display_toggle_dim();
    uk_btn_set_kind(btn, display_is_dimmed() ? UK_BTN_ON : UK_BTN_NEUTRAL);
    if (brightness_label && lv_obj_is_valid(brightness_label))
        lv_label_set_text_fmt(brightness_label, "%d%%", current_brightness);
    lv_obj_t *slider = lv_event_get_user_data(e);
    if (slider && lv_obj_is_valid(slider))
        lv_slider_set_value(slider, current_brightness, LV_ANIM_OFF);
}

static void _dimmer_popup_open(lv_event_t *e) {
    (void)e;
    if (s_dimmer_overlay && lv_obj_is_valid(s_dimmer_overlay)) return;
    s_dimmer_overlay = _make_popup_shell(560, 300, "Screen",
                                         _dimmer_popup_close);

    lv_obj_t *sec = uk_section(s_dimmer_overlay, "Brightness");
    lv_obj_align(sec, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    lv_obj_t *sun = uk_icon(s_dimmer_overlay, UK_ICON_SUN, UK_ICON_MD, UK_TONE_MUTED);
    lv_obj_align(sun, LV_ALIGN_TOP_LEFT, 0, 99);

    lv_obj_t *brightness_bar = lv_slider_create(s_dimmer_overlay);
    lv_obj_set_size(brightness_bar, 400, 8);
    lv_obj_align(brightness_bar, LV_ALIGN_TOP_LEFT, 38, 106);
    lv_slider_set_range(brightness_bar, 5, 100);
    lv_slider_set_value(brightness_bar, current_brightness, LV_ANIM_OFF);
    uk_style_slider(brightness_bar);
    lv_obj_set_ext_click_area(brightness_bar, 18);
    lv_obj_add_event_cb(brightness_bar, brightness_bar_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    brightness_label = uk_label(s_dimmer_overlay, "", UK_FONT_TITLE, UK_TONE_TEXT);
    lv_label_set_text_fmt(brightness_label, "%d%%", current_brightness);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_RIGHT, 0, 92);

    lv_obj_t *dim = _popup_btn(s_dimmer_overlay, 0, 150, 180, UK_ICON_MOON, "Dim now",
                               display_is_dimmed() ? UK_BTN_ON : UK_BTN_NEUTRAL, NULL);
    lv_obj_add_event_cb(dim, _dim_toggle_btn_cb, LV_EVENT_CLICKED, brightness_bar);

    _popup_btn(s_dimmer_overlay, 192, 150, 0, UK_ICON_GEAR, "Dim automatically...",
               UK_BTN_NEUTRAL, brightness_dimmer_config_cb);

    _popup_note(s_dimmer_overlay, 208, 520,
        "Dim follows a wire or signal you choose, like the headlight switch, "
        "so the screen drops at night on its own.");
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
    s_logger_overlay = _make_popup_shell(640, 400, "Recording",
                                         _logger_popup_close);

    /* Recording: the one thing this popup is for, so the primary button. */
    lv_obj_t *sec = uk_section(s_logger_overlay, "Readings to the card");
    lv_obj_align(sec, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    s_log_btn = _popup_btn(s_logger_overlay, 0, 86, 200, UK_ICON_REC, "Start recording",
                           UK_BTN_PRIMARY, _log_toggle_btn_cb);
    s_log_btn_label = uk_btn_label(s_log_btn);

    s_log_rate_dd = lv_dropdown_create(s_logger_overlay);
    lv_dropdown_set_options_static(s_log_rate_dd,
        "1 Hz\n2 Hz\n5 Hz\n10 Hz\n20 Hz\n50 Hz\n100 Hz\n200 Hz\nMax");
    lv_obj_set_size(s_log_rate_dd, 120, UK_BTN_H);
    lv_obj_align(s_log_rate_dd, LV_ALIGN_TOP_LEFT, 212, 86);
    uk_style_dropdown(s_log_rate_dd);
    lv_dropdown_set_selected(s_log_rate_dd,
                             _log_rate_hz_to_idx(data_logger_get_rate_hz()));
    lv_obj_add_event_cb(s_log_rate_dd, _log_rate_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_log_status_label = uk_label(s_logger_overlay, "Stopped", UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_align(s_log_status_label, LV_ALIGN_TOP_LEFT, 0, 136);

    /* Raw CAN capture: every frame, for working out a car nobody has mapped. */
    lv_obj_t *sec2 = uk_section(s_logger_overlay, "Raw CAN capture");
    lv_obj_align(sec2, LV_ALIGN_TOP_LEFT, 0, 170);

    s_canraw_btn = _popup_btn(s_logger_overlay, 0, 200, 200, UK_ICON_CAN, "Start raw CAN",
                              UK_BTN_NEUTRAL, _canraw_toggle_btn_cb);
    s_canraw_btn_label = uk_btn_label(s_canraw_btn);

    _popup_btn(s_logger_overlay, 212, 200, 0, UK_ICON_WEB, "Share raw CAN...",
               UK_BTN_NEUTRAL, _share_btn_cb);

    s_canraw_status_label = uk_label(s_logger_overlay, "Raw: idle", UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_align(s_canraw_status_label, LV_ALIGN_TOP_LEFT, 0, 250);

    /* Live values, folded in from the old separate "Peak Hold" card. Watching
     * the numbers and writing them down are the same job, and Studio says so
     * too (ADR-0030). Peaks ARE the dash's live-signal view. */
    lv_obj_t *sec3 = uk_section(s_logger_overlay, "Live values");
    lv_obj_align(sec3, LV_ALIGN_TOP_LEFT, 0, 284);

    _popup_btn(s_logger_overlay, 0, 314, 200, UK_ICON_CHART, "Live values...",
               UK_BTN_NEUTRAL, _view_peaks_btn_cb);
    _popup_btn(s_logger_overlay, 212, 314, 0, UK_ICON_RESET, "Reset min / max",
               UK_BTN_NEUTRAL, _reset_peaks_btn_cb);

    _update_log_ui();
}

static void _odo_popup_close(lv_event_t *e) {
    (void)e;
    if (s_odo_overlay && lv_obj_is_valid(s_odo_overlay)) lv_obj_del(s_odo_overlay);
    s_odo_overlay        = NULL;
    s_veh_odo_value_lbl  = NULL;
}

static void _odo_popup_open(lv_event_t *e) {
    (void)e;
    if (s_odo_overlay && lv_obj_is_valid(s_odo_overlay)) return;
    s_odo_overlay = _make_popup_shell(500, 260, "Odometer", _odo_popup_close);

    lv_obj_t *sec = uk_section(s_odo_overlay, "Current reading");
    lv_obj_align(sec, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    s_veh_odo_value_lbl = uk_label(s_odo_overlay, "", UK_FONT_TITLE, UK_TONE_TEXT);
    lv_obj_align(s_veh_odo_value_lbl, LV_ALIGN_TOP_LEFT, 0, 82);
    _veh_odo_refresh_timer_cb(NULL);

    _popup_btn(s_odo_overlay, 0, 128, 0, UK_ICON_GEAR, "Change the reading...",
               UK_BTN_NEUTRAL, _odo_edit_btn_cb);

    _popup_note(s_odo_overlay, 184, 460,
        "Counts up from your speed reading, and saves every 1 km or 5 minutes.");
}

static void _can_bus_popup_close(lv_event_t *e) {
    (void)e;
    /* Stop the embedded live-CAN refresh timer BEFORE deleting the overlay,
     * so the timer can't fire against rows that are about to be freed. */
    can_list_ui_embed_stop();
    if (s_can_bus_overlay && lv_obj_is_valid(s_can_bus_overlay)) lv_obj_del(s_can_bus_overlay);
    s_can_bus_overlay     = NULL;
    s_bitrate_dropdown    = NULL;
}

static void _can_bus_popup_open(lv_event_t *e) {
    (void)e;
    if (s_can_bus_overlay && lv_obj_is_valid(s_can_bus_overlay)) return;
    s_can_bus_overlay = _make_popup_shell(700, 460, "CAN Bus",
                                          _can_bus_popup_close);

    lv_obj_t *bitrate_label = uk_section(s_can_bus_overlay, "Bus speed");
    lv_obj_align(bitrate_label, LV_ALIGN_TOP_LEFT, 0, 60);

    s_bitrate_dropdown = lv_dropdown_create(s_can_bus_overlay);
    lv_dropdown_set_options(s_bitrate_dropdown, "125 kbps\n250 kbps\n500 kbps\n1 Mbps");
    lv_obj_set_size(s_bitrate_dropdown, 170, 40);
    lv_obj_align(s_bitrate_dropdown, LV_ALIGN_TOP_LEFT, 150, 50);
    uk_style_dropdown(s_bitrate_dropdown);
    lv_obj_add_event_cb(s_bitrate_dropdown, bitrate_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Show what the bus is running at, which is what the card says too. This
     * read NVS, so a rate the driver had genuinely moved to — a wizard scan
     * applying its recommendation, say — appeared here only once it had also
     * been saved. */
    uint8_t live_bitrate = can_get_bitrate_index();
    if (live_bitrate > 3) live_bitrate = 2;
    lv_dropdown_set_selected(s_bitrate_dropdown, live_bitrate);

    /* Live CAN feed — embed the same scrolling ID/Hz/DLC/bytes table that
     * the full-screen viewer uses, directly in the popup body. Replaces the
     * old health-status panel ("No CAN traffic detected" + Show Details):
     * the live table makes bus activity self-evident at a glance. Torn down
     * in _can_bus_popup_close via can_list_ui_embed_stop(). */
    lv_obj_t *feed_host = lv_obj_create(s_can_bus_overlay);
    lv_obj_set_size(feed_host, 664, 318);
    lv_obj_align(feed_host, LV_ALIGN_TOP_LEFT, 0, 104);
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
 * Page builders — Your car / This dash / Connect
 *
 * Each page is a uk_grid of kit tiles filling the body under the bar. A
 * tile opens either a full-screen tool (channels editor, WiFi, trouble
 * codes, diagnostics, the wizard) or one of the popups defined above.
 * ========================================================================= */

/* Channels card → opens the full split-pane channels editor (the setup
 * wizard's Step 3) standalone. Replaces the old "ECU Preset" card: ECU
 * presets are now bound per-channel from inside that editor's source
 * picker, and bulk ECU auto-detect lives in the setup wizard. */
static void _channels_card_cb(lv_event_t *e) {
    (void)e;
    first_run_wizard_open_channels();
}

static void _obd2_scan_card_cb(lv_event_t *e) {
    (void)e;
    first_run_wizard_open_obd2_scan();
}

/* ── Setup grids ───────────────────────────────────────────────────────
 *
 * Same three groups, same names, same order as Studio's Setup page, so
 * the two surfaces stop drifting (ADR-0039). They had grown apart: the
 * dash filed Trouble Codes under CONNECTIVITY and Live Data under DEVICE,
 * while the web had them under Connectivity and "Logging & dashboards" —
 * same features, four different homes, and no reason a user could infer.
 *
 * Grouped by what you came to change: the car, the dash itself, or the
 * data coming off it. "Files & sharing" has no dash equivalent (no file
 * dialogs on the glass) and Studio's Developer options collapse into the
 * simulator, which lives with Data & testing here.
 * ────────────────────────────────────────────────────────────────────── */

/* ── Pages ─────────────────────────────────────────────────────────────
 * The three groups ADR-0039 settled (the car, the dash itself, how you
 * reach it) are now three pages off the launcher instead of one long
 * scroll; the names, contents and plain-language descriptions are the ones
 * Studio's Setup page uses. Every card is a kit tile.
 */

static void _build_car_page(lv_obj_t *body) {
    lv_obj_t *grid = uk_grid(body, 4, 2);

    size_t ch_total = channel_manager_count();
    size_t ch_bound = 0;
    for (size_t i = 0; i < ch_total; i++) {
        const channel_t *c = channel_manager_at(i);
        if (c && c->signal_index >= 0) ch_bound++;
    }
    char ch_txt[32];
    snprintf(ch_txt, sizeof(ch_txt), "%u mapped", (unsigned)ch_bound);
    _make_setup_card(grid, UK_ICON_CHANNELS, "Channels",
        "Everything the dash can read, and where it comes from.",
        ch_txt, _channels_card_cb, UK_TILE_HERO);

    /* Stat is the rate the driver runs at now, not what NVS last saved. */
    setup_card_t can_card = _make_setup_card(grid, UK_ICON_CAN, "CAN bus",
        "Bus speed, and whether frames are arriving.",
        _bitrate_stat_text(), _can_bus_popup_open, UK_TILE_NORMAL);
    s_can_bitrate_stat_label = can_card.stat_label;

    /* OBD2 readings — where OBD2 is set up (ADR-0073/0074). The stat counts
     * readings the car offers that nothing feeds yet. */
    char obd2_scan_txt[32];
    size_t obd2_offer_n = obd2_autosetup_offer(NULL, CH_OBD2_MATCH_MAX, NULL);
    if (obd2_autosetup_pending())
        snprintf(obd2_scan_txt, sizeof(obd2_scan_txt), "Waiting for the car");
    else if (obd2_offer_n)
        snprintf(obd2_scan_txt, sizeof(obd2_scan_txt), "%u to add", (unsigned)obd2_offer_n);
    else if (obd2_autosetup_checking())
        snprintf(obd2_scan_txt, sizeof(obd2_scan_txt), "Checking");
    else
        snprintf(obd2_scan_txt, sizeof(obd2_scan_txt), "Scan");
    _make_setup_card(grid, UK_ICON_OBD, "OBD2 readings",
        "Ask the car what it reports, and add it as channels.",
        obd2_scan_txt, _obd2_scan_card_cb, UK_TILE_NORMAL);

    _make_setup_card(grid, UK_ICON_ALERT, "Trouble codes",
        "Read and clear the car's check-engine codes.",
        "Read", _dtc_btn_cb, UK_TILE_NORMAL);

    /* The by-hand tool: exact PIDs, including ones the list doesn't know. */
    char obd2_txt[48];
    _obd2_label_compose(obd2_txt, sizeof(obd2_txt));
    setup_card_t obd2 = _make_setup_card(grid, UK_ICON_LIST, "OBD2 PIDs",
        "Pick exact PIDs by hand. Most cars want OBD2 readings.",
        obd2_txt, _obd2_btn_cb, UK_TILE_NORMAL);
    s_obd2_btn_label = obd2.stat_label;

    _make_setup_card(grid, UK_ICON_SHIFTER, "Gear",
        "Work out the gear from RPM and speed.",
        "Calculated", _veh_gear_btn_cb, UK_TILE_NORMAL);

    _make_setup_card(grid, UK_ICON_ROUTE, "Odometer",
        "Total distance, counted from your speed reading.",
        "km", _odo_popup_open, UK_TILE_NORMAL);
    uk_grid_fill_row(grid);
}

static void _build_dash_page(lv_obj_t *body) {
    lv_obj_t *grid = uk_grid(body, 4, 2);

    char fw_stat[24];
    snprintf(fw_stat, sizeof(fw_stat), "v%s", FIRMWARE_VERSION);
    _make_setup_card(grid, UK_ICON_CHIP, "About",
        "Serial, firmware, VIN and ECU name.",
        fw_stat, _device_info_popup_open, UK_TILE_NORMAL);

    char bri_stat[12];
    snprintf(bri_stat, sizeof(bri_stat), "%d%%", current_brightness);
    _make_setup_card(grid, UK_ICON_SUN, "Screen",
        "How bright the screen is, and what makes it dim.",
        bri_stat, _dimmer_popup_open, UK_TILE_NORMAL);

    bool sim_on = signal_sim_is_active();
    setup_card_t sim = _make_setup_card(grid, UK_ICON_PLAY, "Simulator",
        "Feed the dash fake readings to try a layout at the desk.",
        sim_on ? "On" : "Off", _sim_card_cb, UK_TILE_NORMAL);
    s_sim_card_stat = sim.stat_label;
    lv_obj_set_style_text_color(s_sim_card_stat,
        sim_on ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_TEXT_MUTED, 0);

    _make_setup_card(grid, UK_ICON_GAUGE, "Diagnostics",
        "CAN, WiFi, storage and memory, as they are now.",
        "Open", _diag_btn_cb, UK_TILE_NORMAL);

    _make_setup_card(grid, UK_ICON_SPARK, "Setup wizard",
        "Run first-time setup again. Your settings are kept.",
        "Run", _run_wizard_btn_cb, UK_TILE_NORMAL);

    _make_setup_card(grid, UK_ICON_RESET, "Reset layout",
        "Put the default layout back. Nothing else changes.",
        "Asks first", _layout_reset_btn_cb, UK_TILE_DANGER);

    _make_setup_card(grid, UK_ICON_TRASH, "Factory reset",
        "Wipe every setting and layout on this dash.",
        "Asks first", _factory_reset_btn_cb, UK_TILE_DANGER);
    uk_grid_fill_row(grid);
}

static void _build_connect_page(lv_obj_t *body) {
    lv_obj_t *grid = uk_grid(body, 3, 2);

    /* Stats are the SSID / address; refresh_wifi_status repaints them. */
    setup_card_t wifi = _make_setup_card(grid, UK_ICON_WIFI, "WiFi",
        "Which network the dash is on, and its hotspot.",
        "-", wifi_btn_event_cb, UK_TILE_NORMAL);
    uk_grid_place(wifi.card, 0, 0, 1, 1);
    wifi_status_label = wifi.stat_label;

    setup_card_t qr = _make_setup_card(grid, UK_ICON_QR, "Web editor",
        "Scan the code to open this dash on your phone.",
        "-", _qr_btn_cb, UK_TILE_NORMAL);
    uk_grid_place(qr.card, 1, 0, 1, 1);
    web_status_label = qr.stat_label;

    lv_obj_t *card = uk_card(grid);
    uk_grid_place(card, 2, 0, 1, 2);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    uk_section(card, "How to reach it");
    const char *ssid = wifi_manager_get_connected_ssid();
    uk_row(card, "Network", (ssid && ssid[0]) ? ssid : "Not joined");
    char ip[20] = "-";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (ssid && ssid[0] && sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    uk_row(card, "Address", ip);
    bool ap = wifi_manager_is_started() && wifi_manager_is_ap_enabled();
    uk_row(card, "Hotspot", ap ? wifi_manager_get_ap_ssid() : "Off");
    if (ap) uk_row(card, "Hotspot address", "192.168.4.1");

    lv_obj_t *note = uk_label(card,
        "Join the hotspot or the same network with your phone, then open the "
        "address, or scan the web editor code.", UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(note, 8, 0);
    lv_obj_set_style_text_line_space(note, 3, 0);

    /* Bottom-left: a wide tile for the phone app / Studio pointer. */
    lv_obj_t *studio = uk_tile(grid, UK_ICON_WEB, "RDM Studio",
        "Design layouts on a computer and send them to the dash over USB or WiFi.",
        UK_TILE_NORMAL, NULL, NULL);
    uk_grid_place(studio, 0, 1, 2, 1);
    lv_obj_clear_flag(studio, LV_OBJ_FLAG_CLICKABLE);
}

static void _share_btn_cb(lv_event_t *e) {
    (void)e;
    _share_modal_open();
}

/* ── Odometer and gear ───────────────────────────────────────────────────
 *
 * Your car page tiles for vehicle-tied state:
 *   - Gear opens the ui_gear_setup overlay (RPM/speed source, wheel
 *     circumference, final drive, per-gear ratios).
 *   - Odometer opens _odo_popup_open: the live reading (refreshed once a
 *     second from signal_internal_get_odometer_km) and a button to set it
 *     by hand on first install.
 *
 * Odometer accumulation runs continuously inside signal_internal.c's
 * tick — the popup just shows the running value and lets the user snap it
 * to a starting reading.
 *
 * State (s_veh_odo_*, s_odo_edit_*) is hoisted to the file-level static
 * block near the top so the settings teardown can reach it. */

static void _veh_odo_refresh_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_veh_odo_value_lbl || !lv_obj_is_valid(s_veh_odo_value_lbl)) return;
    float km = signal_internal_get_odometer_km();
    char buf[40];
    /* Title face under the popup's own ODOMETER title: just the number.
     * Caps by hand — uk_label only uppercases the text it is created with. */
    snprintf(buf, sizeof(buf), "%.1f KM", (double)km);
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

/* Number-pad popup for setting the odometer by hand (Odometer popup ->
 * "Change the reading..."). A kit popup stacked over the Odometer popup:
 * a text field, an LV_KEYBOARD_MODE_NUMBER pad, and Cancel / Save. */
static void _odo_edit_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_odo_edit_overlay && lv_obj_is_valid(s_odo_edit_overlay)) return;  /* already open */

    /* Inner area is 444 x 364 (the kit card pads 18). */
    s_odo_edit_overlay = uk_popup(480, 400, "Set odometer (km)", _odo_edit_cancel_cb);
    lv_obj_t *card = s_odo_edit_overlay;

    s_odo_edit_textarea = lv_textarea_create(card);
    lv_obj_set_size(s_odo_edit_textarea, lv_pct(100), UK_BTN_H);
    lv_obj_align(s_odo_edit_textarea, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);
    lv_textarea_set_one_line(s_odo_edit_textarea, true);
    lv_textarea_set_accepted_chars(s_odo_edit_textarea, "0123456789.");
    lv_textarea_set_max_length(s_odo_edit_textarea, 16);
    uk_style_textarea(s_odo_edit_textarea);
    /* Pre-fill with the current reading so a small adjustment doesn't
     * require typing the whole number again. */
    char prefill[24];
    snprintf(prefill, sizeof(prefill), "%.1f",
             (double)signal_internal_get_odometer_km());
    lv_textarea_set_text(s_odo_edit_textarea, prefill);

    lv_obj_t *kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, lv_pct(100), 208);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 0, 106);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    uk_style_keyboard(kb);
    lv_keyboard_set_textarea(kb, s_odo_edit_textarea);

    /* Cancel / Save. */
    lv_obj_t *cancel = uk_btn(card, UK_ICON_NONE, "Cancel", UK_BTN_NEUTRAL,
                              _odo_edit_cancel_cb, NULL);
    lv_obj_set_width(cancel, 212);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *save = uk_btn(card, UK_ICON_CHECK, "Save", UK_BTN_PRIMARY,
                            _odo_edit_save_cb, NULL);
    lv_obj_set_width(save, 212);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void _veh_gear_btn_cb(lv_event_t *e) {
    (void)e;
    ui_gear_setup_open(NULL, NULL);
}

/* Universal cleanup: fires no matter who deletes settings_screen — explicit
 * lv_obj_del, lv_scr_load_anim with auto_del, or an async dashboard reload
 * that wipes the previous active screen. The old close-menu handler and
 * _deferred_reload_after_ecu used to be the only paths that tore down the
 * screen-owned timers; the 2026-06 wifi-status panic came from a path where
 * neither ran, so the timer kept ticking on freed labels. Safe to invoke
 * twice because every branch is NULL-guarded. */
static lv_obj_t *s_ds_screen = NULL;   /* the menu screen the session is attached to */

static void _settings_screen_delete_cb(lv_event_t *e) {
    /* Menu screens replace each other (launcher -> page -> launcher). The new
     * one attaches before the old one is deleted, so only the screen the
     * session is attached to right now may tear it down. */
    if (lv_event_get_target(e) != s_ds_screen) return;
    s_ds_screen = NULL;

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
     * statics; the bulk NULL block below catches anything else. The popups
     * that open over another popup go first, then the ones under them. */
    close_dimmer_popup_cb(NULL);
    _share_modal_close();
    _odo_edit_close();
    _qr_close_cb(NULL);
    _wifi_loading_close_cb(NULL);
    _device_info_popup_close(NULL);
    _dimmer_popup_close(NULL);
    _logger_popup_close(NULL);
    _can_bus_popup_close(NULL);
    _odo_popup_close(NULL);

    wifi_status_label    = NULL;
    web_status_label     = NULL;
    brightness_label     = NULL;
    s_log_btn            = NULL;
    s_log_btn_label      = NULL;
    s_log_status_label   = NULL;
    s_log_rate_dd        = NULL;
    s_canraw_btn         = NULL;
    s_canraw_btn_label   = NULL;
    s_canraw_status_label= NULL;
    s_sim_card_stat      = NULL;
    s_veh_odo_value_lbl  = NULL;
    s_bitrate_dropdown   = NULL;
}

void device_settings_attach(lv_obj_t *screen) {
    device_settings_return_screen = main_menu_return_screen();
    s_ds_screen = screen;
    lv_obj_add_event_cb(screen, _settings_screen_delete_cb, LV_EVENT_DELETE, NULL);

    /* Live-update timers. Their callbacks NULL-check every label they paint
     * into, so they are safe on screens that don't show those labels. */
    if (s_log_status_timer) lv_timer_del(s_log_status_timer);
    s_log_status_timer = lv_timer_create(_log_status_timer_cb, 1000, NULL);

    if (s_wifi_status_timer) lv_timer_del(s_wifi_status_timer);
    s_wifi_status_timer = lv_timer_create(refresh_wifi_status_timer_cb, 2000, NULL);

    if (s_can_diag_timer) lv_timer_del(s_can_diag_timer);
    s_can_diag_timer = lv_timer_create(refresh_can_diag_timer_cb, 1000, NULL);

    if (s_veh_odo_timer) lv_timer_del(s_veh_odo_timer);
    s_veh_odo_timer = lv_timer_create(_veh_odo_refresh_timer_cb, 1000, NULL);
}

void device_settings_open_page(ds_page_t page) {
    static const char *titles[] = { "Your car", "This dash", "Connect" };
    if ((unsigned)page > DS_PAGE_CONNECT) return;

    lv_obj_t *scr = uk_screen();
    uk_bar(scr, titles[page], UK_BAR_BACK, main_menu_back_cb, NULL);
    lv_obj_t *body = uk_body(scr);

    /* The labels a page doesn't build must read NULL to the timers. */
    wifi_status_label = web_status_label = NULL;
    s_can_bitrate_stat_label = s_obd2_btn_label = s_sim_card_stat = NULL;

    switch (page) {
    case DS_PAGE_CAR:
        /* Channels, CAN and OBD2 all shape how the dashboard is built. */
        main_menu_mark_dirty();
        _build_car_page(body);
        break;
    case DS_PAGE_DASH:
        _build_dash_page(body);
        break;
    case DS_PAGE_CONNECT:
        _build_connect_page(body);
        break;
    }

    device_settings_attach(scr);
    refresh_wifi_status();
    main_menu_swap_to(scr);
}

void device_settings_open_popup(ds_popup_t popup) {
    switch (popup) {
    case DS_POPUP_SCREEN:    _dimmer_popup_open(NULL); break;
    case DS_POPUP_RECORDING: _logger_popup_open(NULL); break;
    }
}

/* The old entry point: Device Settings is the launcher now. */
void device_settings_with_return_screen(lv_obj_t* return_screen) {
    main_menu_open(return_screen ? return_screen : lv_scr_act());
}

void device_settings_longpress_cb(lv_event_t* e) {
    device_settings_with_return_screen(NULL);
}