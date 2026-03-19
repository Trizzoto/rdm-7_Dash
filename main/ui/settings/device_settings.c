#include "device_settings.h"
#include "theme.h"
#include "lvgl.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "ui_wifi.h"
#include "ota_handler.h"
#include "nvs_flash.h"
#include "version.h"
#include "device_id.h"
#include "ui.h"
#include "ui_helpers.h"
#include "screens/ui_Screen3.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_system.h"
#include "ota_update_dialog.h"
#include "lwip/ip4_addr.h"
#include "callbacks/ui_callbacks.h"
#include "can/can_manager.h"
#include "storage/config_store.h"
#include "widgets/signal.h"
#include <stdlib.h>
#include <string.h>

extern char* connected_ssid;

// Global WiFi status labels for updating
static lv_obj_t* wifi_status_label = NULL;
static lv_obj_t* web_status_label = NULL;

// Function to refresh WiFi status displays
static void refresh_wifi_status(void) {
    if (!wifi_status_label || !web_status_label) return;
    
    // Update WiFi status
    if (connected_ssid) {
        char status_text[32];
        snprintf(status_text, sizeof(status_text), "WiFi: %s", connected_ssid);
        lv_label_set_text(wifi_status_label, status_text);
        lv_obj_set_style_text_color(wifi_status_label, THEME_COLOR_STATUS_CONNECTED, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_label_set_text(wifi_status_label, "WiFi: Not Connected");
        lv_obj_set_style_text_color(wifi_status_label, THEME_COLOR_STATUS_WARN, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    
    // Update web server status
    if (connected_ssid) {
        // Get IP address
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char web_text[64];
            snprintf(web_text, sizeof(web_text), "Web: http://" IPSTR, IP2STR(&ip_info.ip));
            lv_label_set_text(web_status_label, web_text);
            lv_obj_set_style_text_color(web_status_label, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_label_set_text(web_status_label, "Web: Waiting for IP...");
            lv_obj_set_style_text_color(web_status_label, THEME_COLOR_ACCENT_YELLOW, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        lv_label_set_text(web_status_label, "Web: Connect WiFi first");
        lv_obj_set_style_text_color(web_status_label, THEME_COLOR_STATUS_WARN, LV_PART_MAIN | LV_STATE_DEFAULT);
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
        lv_label_set_text_fmt(brightness_label, "Brightness: %d%%", val);
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
                              0, 1, 1.0f, 0.0f, false, 1);
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
    lv_obj_add_event_cb(overlay, close_dimmer_popup_cb, LV_EVENT_CLICKED, NULL);

    // Create popup container
    lv_obj_t* popup = lv_obj_create(overlay);
    lv_obj_set_size(popup, 500, 380);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(popup, THEME_COLOR_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(popup, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(popup, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(popup, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(popup, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_CLICKABLE);

    // Title
    lv_obj_t* title = lv_label_create(popup);
    lv_label_set_text(title, "Brightness Dimmer Switch");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Close button
    lv_obj_t* close_btn = lv_btn_create(popup);
    lv_obj_set_size(close_btn, 40, 35);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_BTN_CLOSE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(close_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "X");
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close_btn, close_dimmer_popup_cb, LV_EVENT_CLICKED, overlay);

    // Signal Source dropdown
    lv_obj_t* signal_label = lv_label_create(popup);
    lv_label_set_text(signal_label, "Signal Source:");
    lv_obj_align(signal_label, LV_ALIGN_TOP_LEFT, 10, 50);
    lv_obj_set_style_text_color(signal_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* signal_dd = lv_dropdown_create(popup);
    lv_obj_set_size(signal_dd, 250, 40);
    lv_obj_align(signal_dd, LV_ALIGN_TOP_LEFT, 10, 70);
    {
        static char sig_options[1024];
        uint16_t sel = _build_signal_options(sig_options, sizeof(sig_options));
        lv_dropdown_set_options(signal_dd, sig_options);
        lv_dropdown_set_selected(signal_dd, sel);
    }

    // Threshold input
    lv_obj_t* thresh_label = lv_label_create(popup);
    lv_label_set_text(thresh_label, "Threshold:");
    lv_obj_align(thresh_label, LV_ALIGN_TOP_LEFT, 280, 50);
    lv_obj_set_style_text_color(thresh_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* thresh_input = lv_textarea_create(popup);
    lv_obj_set_size(thresh_input, 100, 40);
    lv_obj_align(thresh_input, LV_ALIGN_TOP_LEFT, 280, 70);
    lv_textarea_set_max_length(thresh_input, 8);
    lv_obj_clear_flag(thresh_input, LV_OBJ_FLAG_SCROLLABLE);
    char thresh_str[16];
    snprintf(thresh_str, sizeof(thresh_str), "%.2f", dimmer_config.threshold);
    lv_textarea_set_text(thresh_input, thresh_str);
    lv_obj_add_event_cb(thresh_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // Toggle Mode dropdown
    lv_obj_t* toggle_mode_label = lv_label_create(popup);
    lv_label_set_text(toggle_mode_label, "Toggle Mode:");
    lv_obj_align(toggle_mode_label, LV_ALIGN_TOP_LEFT, 10, 120);
    lv_obj_set_style_text_color(toggle_mode_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* toggle_mode_dd = lv_dropdown_create(popup);
    lv_obj_set_size(toggle_mode_dd, 120, 40);
    lv_obj_align(toggle_mode_dd, LV_ALIGN_TOP_LEFT, 10, 140);
    lv_dropdown_set_options(toggle_mode_dd, "On/Off\nMomentary");
    lv_dropdown_set_selected(toggle_mode_dd, dimmer_config.is_momentary ? 1 : 0);

    // Invert switch
    lv_obj_t* invert_label = lv_label_create(popup);
    lv_label_set_text(invert_label, "Invert:");
    lv_obj_align(invert_label, LV_ALIGN_TOP_LEFT, 180, 120);
    lv_obj_set_style_text_color(invert_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* invert_switch = lv_switch_create(popup);
    lv_obj_set_size(invert_switch, 50, 25);
    lv_obj_align(invert_switch, LV_ALIGN_TOP_LEFT, 180, 140);
    if (dimmer_config.invert) {
        lv_obj_add_state(invert_switch, LV_STATE_CHECKED);
    }

    // Brightness Set slider
    lv_obj_t* brightness_set_label = lv_label_create(popup);
    lv_label_set_text(brightness_set_label, "Dim Brightness:");
    lv_obj_align(brightness_set_label, LV_ALIGN_TOP_LEFT, 10, 190);
    lv_obj_set_style_text_color(brightness_set_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* brightness_set_slider = lv_slider_create(popup);
    lv_obj_set_size(brightness_set_slider, 280, 20);
    lv_obj_align(brightness_set_slider, LV_ALIGN_TOP_LEFT, 10, 210);
    lv_slider_set_range(brightness_set_slider, 5, 100);
    lv_slider_set_value(brightness_set_slider, dimmer_config.dim_brightness, LV_ANIM_OFF);

    lv_obj_t* brightness_value_label = lv_label_create(popup);
    lv_label_set_text_fmt(brightness_value_label, "%d%%", dimmer_config.dim_brightness);
    lv_obj_align(brightness_value_label, LV_ALIGN_TOP_LEFT, 300, 210);
    lv_obj_set_style_text_color(brightness_value_label, THEME_COLOR_ACCENT_YELLOW, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(brightness_set_slider, brightness_set_slider_cb, LV_EVENT_VALUE_CHANGED, brightness_value_label);

    // Enable/Disable switch
    lv_obj_t* enable_label = lv_label_create(popup);
    lv_label_set_text(enable_label, "Enabled:");
    lv_obj_align(enable_label, LV_ALIGN_TOP_LEFT, 10, 250);
    lv_obj_set_style_text_color(enable_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* enable_switch = lv_switch_create(popup);
    lv_obj_set_size(enable_switch, 50, 25);
    lv_obj_align(enable_switch, LV_ALIGN_TOP_LEFT, 10, 270);
    if (dimmer_config.enabled) {
        lv_obj_add_state(enable_switch, LV_STATE_CHECKED);
    }

    // Save button
    lv_obj_t* save_btn = lv_btn_create(popup);
    lv_obj_set_size(save_btn, 120, 40);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE_ALT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(save_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
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
    
    // Save current brightness before preview
    saved_brightness_before_preview = current_brightness;
    
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

// Close menu callback
static void close_menu_event_cb(lv_event_t * e) {
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

// Loading dialog for WiFi
static lv_obj_t* wifi_loading_dialog = NULL;

// Timer callback to show WiFi screen after loading dialog
static void show_wifi_screen_delayed(lv_timer_t* timer) {
    // Close loading dialog
    if (wifi_loading_dialog && lv_obj_is_valid(wifi_loading_dialog)) {
        lv_obj_del(wifi_loading_dialog);
        wifi_loading_dialog = NULL;
    }
    
    // Show WiFi screen
    show_wifi_screen();
    
    // Delete the timer
    lv_timer_del(timer);
}

// WiFi button callback
static void wifi_btn_event_cb(lv_event_t *e) {
    // Create loading dialog immediately
    wifi_loading_dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_loading_dialog, 300, 150);
    lv_obj_align(wifi_loading_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(wifi_loading_dialog, lv_color_hex(0x2E2E2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(wifi_loading_dialog, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(wifi_loading_dialog, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(wifi_loading_dialog, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(wifi_loading_dialog, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(wifi_loading_dialog, LV_OBJ_FLAG_SCROLLABLE);
    
    // Loading title
    lv_obj_t* title = lv_label_create(wifi_loading_dialog);
    lv_label_set_text(title, "Wi-Fi Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Loading spinner
    lv_obj_t* spinner = lv_spinner_create(wifi_loading_dialog, 1000, 60);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    
    // Loading text
    lv_obj_t* loading_text = lv_label_create(wifi_loading_dialog);
    lv_label_set_text(loading_text, "Searching for networks...");
    lv_obj_align(loading_text, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_color(loading_text, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(loading_text, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Create timer to show WiFi screen after a brief delay (allows dialog to render)
    lv_timer_create(show_wifi_screen_delayed, 100, NULL);
}

// Update button callback
static void update_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    
    ESP_LOGI("OTA", "Check for updates button clicked");
    
    // First check for updates
    check_for_update();
    
    // Check the result and show appropriate dialog
    ota_status_t status = get_ota_status();
    switch (status) {
        case OTA_UPDATE_AVAILABLE: {
            ESP_LOGI("OTA", "Update available, showing dialog");
            
            // Get update information from OTA handler
            const char* current_version = FIRMWARE_VERSION;
            const char* new_version = get_latest_version();
            const char* update_type = get_update_type_str();
            float file_size_mb = get_update_file_size_mb();
            const char* release_notes = get_release_notes();
            
            // Show the OTA update dialog
            show_ota_update_dialog(current_version, new_version, update_type, file_size_mb, release_notes);
            break;
        }
        case OTA_NO_UPDATE_AVAILABLE:
            ESP_LOGI("OTA", "No update available");
            // Could show a "No updates available" message
            break;
        case OTA_UPDATE_FAILED:
            ESP_LOGE("OTA", "Update check failed");
            // Could show an error message
            break;
        default:
            ESP_LOGW("OTA", "Unexpected OTA status: %d", status);
            break;
    }
}

/* ── Factory Reset ────────────────────────────────────────────────────── */

static void _factory_reset_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn_txt = lv_msgbox_get_active_btn_text(mbox);
    if (!btn_txt) return;

    if (strcmp(btn_txt, "RESET") == 0) {
        ESP_LOGW("RESET", "User confirmed factory reset");
        config_store_factory_reset();
        /* Brief delay so log output flushes before reboot */
        vTaskDelay(pdMS_TO_TICKS(200));
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

    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, THEME_COLOR_BTN_CANCEL, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 2, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(mbox, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_width(mbox, 380);
    lv_obj_center(mbox);

    /* Style the RESET button red */
    lv_obj_t *btn_area = lv_msgbox_get_btns(mbox);
    lv_obj_set_style_bg_color(btn_area, THEME_COLOR_BTN_CANCEL,
                              LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn_area, THEME_COLOR_TEXT_PRIMARY,
                                LV_PART_ITEMS | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(mbox, _factory_reset_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
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

// Function to create device settings with a specific return screen
void device_settings_with_return_screen(lv_obj_t* return_screen) {
    // Store the return screen for later use
    device_settings_return_screen = return_screen ? return_screen : lv_scr_act();
    lv_obj_t* settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settings_screen, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(settings_screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Main container with border - sized properly for 480px screen height
    lv_obj_t* main_container = lv_obj_create(settings_screen);
    lv_obj_set_size(main_container, 760, 440);  // Fit within 480px screen with margins
    lv_obj_align(main_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(main_container, THEME_COLOR_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(main_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(main_container, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(main_container, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(main_container, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(main_container, 15, LV_PART_MAIN | LV_STATE_DEFAULT);  // Reduced padding for more space
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    // Header section
    lv_obj_t* header = lv_obj_create(main_container);
    lv_obj_set_size(header, lv_pct(100), 50);  // Reduced height for more content space
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(header, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(header, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "DEVICE SETTINGS");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_XLARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Close button
    lv_obj_t* close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 50, 35);  // Smaller button for smaller header
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -15, 0);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_BTN_CLOSE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_BTN_CLOSE_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(close_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "X");
    lv_obj_center(close_label);
    lv_obj_set_style_text_font(close_label, THEME_FONT_MEDIUM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(close_btn, close_menu_event_cb, LV_EVENT_CLICKED, device_settings_return_screen);
    
    // Content area - properly sized for 440px container
    lv_obj_t* content = lv_obj_create(main_container);
    lv_obj_set_size(content, lv_pct(100), 350);  // Proper size to fit in 440px container
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 60);  // Position below 50px header with 10px gap
    lv_obj_set_style_bg_opa(content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(content, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(content, 25, LV_PART_MAIN | LV_STATE_DEFAULT);  // Extra right padding for scrollbar space
    lv_obj_set_scroll_dir(content, LV_DIR_VER);  // Enable vertical scrolling
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Scrollbar styling - move it away from content
    lv_obj_set_style_bg_color(content, THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, 200, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_width(content, 8, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(content, 5, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);  // Move scrollbar away from edge

    // First row container
    lv_obj_t* first_row = lv_obj_create(content);
    lv_obj_set_size(first_row, lv_pct(100), 120);  // Reduced height for better fit
    lv_obj_set_style_bg_opa(first_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(first_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(first_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(first_row, 15, LV_PART_MAIN | LV_STATE_DEFAULT);  // Manual spacing
    lv_obj_clear_flag(first_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(first_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(first_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // CAN Bus Configuration Section
    lv_obj_t* can_section = lv_obj_create(first_row);
    lv_obj_set_size(can_section, lv_pct(48), 120);  // Match first_row height
    lv_obj_set_style_bg_color(can_section, THEME_COLOR_SECTION_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(can_section, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(can_section, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(can_section, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(can_section, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(can_section, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(can_section, LV_OBJ_FLAG_SCROLLABLE);

    // CAN section title
    lv_obj_t* can_title = lv_label_create(can_section);
    lv_label_set_text(can_title, "CAN BUS");
    lv_obj_align(can_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(can_title, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(can_title, THEME_COLOR_STATUS_CONNECTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Bitrate label
    lv_obj_t* bitrate_label = lv_label_create(can_section);
    lv_label_set_text(bitrate_label, "Bitrate");
    lv_obj_align(bitrate_label, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_text_font(bitrate_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(bitrate_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Bitrate dropdown
    lv_obj_t* bitrate_dd = lv_dropdown_create(can_section);
    lv_dropdown_set_options(bitrate_dd, "125 kbps\n250 kbps\n500 kbps\n1 Mbps");
    lv_obj_set_size(bitrate_dd, 140, 35);
    lv_obj_align(bitrate_dd, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_set_style_bg_color(bitrate_dd, THEME_COLOR_CONTROL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(bitrate_dd, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(bitrate_dd, THEME_COLOR_SCROLLBAR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bitrate_dd, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bitrate_dd, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(bitrate_dd, bitrate_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Device Information Section
    lv_obj_t* info_section = lv_obj_create(first_row);
    lv_obj_set_size(info_section, lv_pct(48), 120);  // Match first_row height
    lv_obj_set_style_bg_color(info_section, THEME_COLOR_SECTION_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(info_section, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(info_section, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(info_section, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(info_section, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(info_section, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(info_section, LV_OBJ_FLAG_SCROLLABLE);

    // Info section title
    lv_obj_t* info_title = lv_label_create(info_section);
    lv_label_set_text(info_title, "DEVICE INFO");
    lv_obj_align(info_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(info_title, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(info_title, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Serial number - adjusted positions
    lv_obj_t* serial_label = lv_label_create(info_section);
    lv_label_set_text(serial_label, "Serial Number");
    lv_obj_align(serial_label, LV_ALIGN_TOP_LEFT, 0, 25);
    lv_obj_set_style_text_font(serial_label, THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(serial_label, THEME_COLOR_TEXT_HINT, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* serial_value = lv_label_create(info_section);
    char serial[MAX_SERIAL_LENGTH];
    if (get_device_serial(serial) == ESP_OK) {
        lv_label_set_text(serial_value, serial);
    } else {
        lv_label_set_text(serial_value, "Unknown");
    }
    lv_obj_align(serial_value, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_set_style_text_font(serial_value, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(serial_value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Firmware version - adjusted positions
    lv_obj_t* fw_label = lv_label_create(info_section);
    lv_label_set_text(fw_label, "Firmware");
    lv_obj_align(fw_label, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_obj_set_style_text_font(fw_label, THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(fw_label, THEME_COLOR_TEXT_HINT, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* fw_value = lv_label_create(info_section);
    lv_label_set_text(fw_value, FIRMWARE_VERSION);
    lv_obj_align(fw_value, LV_ALIGN_TOP_LEFT, 0, 87);
    lv_obj_set_style_text_font(fw_value, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(fw_value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Second row container
    lv_obj_t* second_row = lv_obj_create(content);
    lv_obj_set_size(second_row, lv_pct(100), 185);  // Increased height to give Check Updates button more room
    lv_obj_set_style_bg_opa(second_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(second_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(second_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(second_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(second_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(second_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Network & Updates Section
    lv_obj_t* network_section = lv_obj_create(second_row);
    lv_obj_set_size(network_section, lv_pct(48), 185);  // Increased height to give Check Updates button more room
    lv_obj_set_style_bg_color(network_section, THEME_COLOR_SECTION_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(network_section, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(network_section, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(network_section, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(network_section, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(network_section, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(network_section, LV_OBJ_FLAG_SCROLLABLE);

    // Network section title
    lv_obj_t* network_title = lv_label_create(network_section);
    lv_label_set_text(network_title, "NETWORK & UPDATES");
    lv_obj_align(network_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(network_title, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(network_title, THEME_COLOR_ACCENT_ORANGE, LV_PART_MAIN | LV_STATE_DEFAULT);

    // WiFi button - adjusted position
    lv_obj_t* wifi_btn = lv_btn_create(network_section);
    lv_obj_set_size(wifi_btn, 180, 35);
    lv_obj_align(wifi_btn, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_bg_color(wifi_btn, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(wifi_btn, THEME_COLOR_ACCENT_BLUE_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(wifi_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(wifi_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t* wifi_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_label, "Wi-Fi Settings");
    lv_obj_center(wifi_label);
    lv_obj_set_style_text_font(wifi_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(wifi_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(wifi_btn, wifi_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // WiFi status - adjusted position
    wifi_status_label = lv_label_create(network_section);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_LEFT, 0, 75);
    lv_obj_set_style_text_font(wifi_status_label, THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Web server status
    web_status_label = lv_label_create(network_section);
    lv_obj_align(web_status_label, LV_ALIGN_TOP_LEFT, 0, 90);
    lv_obj_set_style_text_font(web_status_label, THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Initial refresh of WiFi status
    refresh_wifi_status();

    // Update button - adjusted position
    lv_obj_t* update_btn = lv_btn_create(network_section);
    lv_obj_set_size(update_btn, 180, 35);
    lv_obj_align(update_btn, LV_ALIGN_TOP_LEFT, 0, 115);
    lv_obj_set_style_bg_color(update_btn, THEME_COLOR_BTN_SAVE_ALT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(update_btn, THEME_COLOR_BTN_SAVE_ALT_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(update_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(update_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t* update_label = lv_label_create(update_btn);
    lv_label_set_text(update_label, "Check Updates");
    lv_obj_center(update_label);
    lv_obj_set_style_text_font(update_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(update_label, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(update_btn, update_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Display Settings Section
    lv_obj_t* display_section = lv_obj_create(second_row);
    lv_obj_set_size(display_section, lv_pct(48), 185);  // Increased height to match network section
    lv_obj_set_style_bg_color(display_section, THEME_COLOR_SECTION_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(display_section, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(display_section, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(display_section, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(display_section, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(display_section, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(display_section, LV_OBJ_FLAG_SCROLLABLE);

    // Display section title
    lv_obj_t* display_title = lv_label_create(display_section);
    lv_label_set_text(display_title, "DISPLAY");
    lv_obj_align(display_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(display_title, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(display_title, THEME_COLOR_ACCENT_YELLOW, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Brightness label - adjusted position
    lv_obj_t* brightness_text = lv_label_create(display_section);
    lv_label_set_text(brightness_text, "Brightness");
    lv_obj_align(brightness_text, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_text_font(brightness_text, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(brightness_text, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Use current brightness value (don't load from NVS)
    uint8_t saved_brightness = current_brightness;

    // Brightness slider - adjusted position and size
    lv_obj_t* brightness_bar = lv_slider_create(display_section);
    lv_obj_set_size(brightness_bar, 220, 25);
    lv_obj_align(brightness_bar, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_slider_set_range(brightness_bar, 5, 100);
    lv_slider_set_value(brightness_bar, saved_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_CONTROL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(brightness_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(brightness_bar, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Brightness knob
    lv_obj_set_style_bg_color(brightness_bar, THEME_COLOR_TEXT_PRIMARY, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(brightness_bar, 255, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(brightness_bar, LV_RADIUS_CIRCLE, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(brightness_bar, 3, LV_PART_KNOB | LV_STATE_DEFAULT);

    // Brightness percentage label - adjusted position
    brightness_label = lv_label_create(display_section);
    lv_label_set_text_fmt(brightness_label, "Brightness: %d%%", saved_brightness);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 0, 95);
    lv_obj_set_style_text_font(brightness_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(brightness_label, THEME_COLOR_ACCENT_YELLOW, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(brightness_bar, brightness_bar_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Brightness Dimmer Switch button
    lv_obj_t* dimmer_btn = lv_btn_create(display_section);
    lv_obj_set_size(dimmer_btn, 220, 35);
    lv_obj_align(dimmer_btn, LV_ALIGN_TOP_LEFT, 0, 130);
    lv_obj_set_style_bg_color(dimmer_btn, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(dimmer_btn, THEME_COLOR_BTN_DIM_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(dimmer_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dimmer_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t* dimmer_label = lv_label_create(dimmer_btn);
    lv_label_set_text(dimmer_label, "Brightness Dimmer Switch");
    lv_obj_center(dimmer_label);
    lv_obj_set_style_text_font(dimmer_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(dimmer_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(dimmer_btn, brightness_dimmer_config_cb, LV_EVENT_CLICKED, NULL);

    // Factory Reset button — full width at bottom
    lv_obj_t* reset_btn = lv_btn_create(content);
    lv_obj_set_size(reset_btn, lv_pct(100), 40);
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(0x331111), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(reset_btn, THEME_COLOR_BTN_CANCEL, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(reset_btn, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(reset_btn, THEME_COLOR_BTN_CANCEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(reset_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t* reset_label = lv_label_create(reset_btn);
    lv_label_set_text(reset_label, "Reset to Default");
    lv_obj_center(reset_label);
    lv_obj_set_style_text_font(reset_label, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(reset_label, THEME_COLOR_BTN_CANCEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(reset_btn, _factory_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    uint8_t saved_bitrate = 2; /* default 500 kbps */
    config_store_load_bitrate(&saved_bitrate);
    lv_dropdown_set_selected(bitrate_dd, saved_bitrate);

    // Create timer to refresh WiFi status every 2 seconds
    lv_timer_create(refresh_wifi_status_timer_cb, 2000, NULL);

    lv_scr_load(settings_screen);
}

void device_settings_longpress_cb(lv_event_t* e) {
    device_settings_with_return_screen(NULL);
}