#include "ui_wifi.h"
#include "../theme.h"
#include "screen_config.h"
#include "net/wifi_manager.h"
#include "storage/config_store.h"
#include "web_server.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_ui";

#define CONNECT_TIMEOUT_MS  15000
#define MAX_SCAN_RESULTS    20
#define REFRESH_PERIOD_MS   1000

/* =========================================================================
 * Static UI elements
 * ========================================================================= */
static lv_obj_t *wifi_screen       = NULL;
static lv_obj_t *wifi_toggle       = NULL;
static lv_obj_t *ap_toggle         = NULL;
static lv_obj_t *boot_toggle       = NULL;
static lv_obj_t *ap_ssid_label     = NULL;
static lv_obj_t *ap_ip_label       = NULL;
static lv_obj_t *ap_pass_input     = NULL;
static lv_obj_t *ap_pass_error     = NULL;
static lv_obj_t *status_label      = NULL;
static lv_obj_t *wifi_list         = NULL;
static lv_obj_t *password_modal    = NULL;
static lv_obj_t *password_input    = NULL;
static lv_obj_t *wifi_keyboard     = NULL;
static lv_obj_t *connection_spinner = NULL;
static lv_obj_t *scan_btn          = NULL;

/* Containers for conditional visibility */
static lv_obj_t *wifi_dependent_section = NULL;  /* everything below wifi toggle */
static lv_obj_t *ap_info_container      = NULL;  /* AP SSID/IP rows */
static lv_obj_t *right_panel            = NULL;   /* available networks panel */

/* State */
static lv_obj_t   *return_screen   = NULL;
static lv_timer_t *refresh_timer   = NULL;
static lv_timer_t *connect_timeout_timer = NULL;
static char        selected_ssid[33];
static bool        initialized     = false;

/* =========================================================================
 * Forward declarations
 * ========================================================================= */
static void _create_screen(void);
static void _destroy_screen(void);
static void _refresh_status(lv_timer_t *t);
static void _populate_scan_list(void);
static void _show_password_modal(const char *ssid);
static void _close_password_modal(void);
static void _update_visibility(void);
static void _style_switch(lv_obj_t *sw);
static void _style_section_card(lv_obj_t *card);
static void _style_section_title(lv_obj_t *label);
static lv_obj_t *_create_info_row(lv_obj_t *parent, const char *label_text,
                                  lv_obj_t **value_out);

/* Event callbacks */
static void _back_btn_cb(lv_event_t *e);
static void _wifi_toggle_cb(lv_event_t *e);
static void _ap_toggle_cb(lv_event_t *e);
static void _boot_toggle_cb(lv_event_t *e);
static void _scan_btn_cb(lv_event_t *e);
static void _network_item_cb(lv_event_t *e);
static void _password_connect_cb(lv_event_t *e);
static void _password_cancel_cb(lv_event_t *e);
static void _forget_cb(lv_event_t *e);
static void _ap_pass_set_cb(lv_event_t *e);
static void _connect_timeout_cb(lv_timer_t *t);

/* wifi_manager event callback (may run off LVGL task) */
static void _wifi_mgr_event_cb(wifi_mgr_state_t new_state, void *user_data);
static void _async_refresh(void *data);

/* =========================================================================
 * Signal strength helper
 * ========================================================================= */
static const char *_rssi_icon(int8_t rssi)
{
    (void)rssi;
    return LV_SYMBOL_WIFI;
}

static const char *_rssi_bars(int8_t rssi)
{
    if (rssi >= -55) return "||||";
    if (rssi >= -70) return "|||";
    if (rssi >= -85) return "||";
    return "|";
}

static const char *_auth_text(uint8_t auth_mode)
{
    if (auth_mode == 0) return "Open";
    return LV_SYMBOL_EYE_CLOSE;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void wifi_ui_init(void)
{
    if (initialized) return;
    wifi_manager_set_event_cb(_wifi_mgr_event_cb, NULL);
    initialized = true;
}

void wifi_ui_show(void)
{
    if (wifi_screen) return;
    return_screen = lv_scr_act();
    _create_screen();
    lv_scr_load(wifi_screen);
}

void wifi_ui_hide(void)
{
    if (!wifi_screen) return;
    /* Load the return screen BEFORE deleting wifi_screen.
     * Deleting the active screen causes a crash in LVGL v8. */
    lv_obj_t *ret = return_screen;
    return_screen = NULL;
    if (ret) {
        lv_scr_load(ret);
    }
    _destroy_screen();
}

bool wifi_ui_is_active(void)
{
    return wifi_screen != NULL;
}

const char *wifi_get_ap_ssid(void)
{
    return wifi_manager_get_ap_ssid();
}

/* =========================================================================
 * Screen creation
 * ========================================================================= */

static void _create_screen(void)
{
    wifi_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wifi_screen, THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(wifi_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* -- Main container -------------------------------------------------- */
    lv_obj_t *main_cont = lv_obj_create(wifi_screen);
    lv_obj_set_size(main_cont, 780, 460);
    lv_obj_align(main_cont, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(main_cont, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(main_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(main_cont, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(main_cont, 1, 0);
    lv_obj_set_style_radius(main_cont, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(main_cont, 0, 0);
    lv_obj_clear_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* -- Header bar ------------------------------------------------------ */
    lv_obj_t *header = lv_obj_create(main_cont);
    lv_obj_set_size(header, 780, 44);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 10, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 70, 30);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(back_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_radius(back_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, _back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(back_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_center(back_lbl);

    /* Title */
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Wi-Fi Settings");
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Status */
    status_label = lv_label_create(header);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_font(status_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(status_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_align(status_label, LV_ALIGN_RIGHT_MID, -5, 0);

    /* -- Body area ------------------------------------------------------- */
    lv_obj_t *body = lv_obj_create(main_cont);
    lv_obj_set_size(body, 780, 416);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* == LEFT PANEL (controls) =========================================== */
    lv_obj_t *left_panel = lv_obj_create(body);
    lv_obj_set_size(left_panel, 350, 396);
    lv_obj_align(left_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(left_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_panel, 0, 0);
    lv_obj_set_style_pad_all(left_panel, 0, 0);
    lv_obj_set_style_pad_gap(left_panel, 10, 0);
    lv_obj_set_flex_flow(left_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* -- Controls section card ------------------------------------------- */
    lv_obj_t *ctrl_card = lv_obj_create(left_panel);
    lv_obj_set_size(ctrl_card, 350, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(ctrl_card, 80, 0);
    _style_section_card(ctrl_card);
    lv_obj_set_flex_flow(ctrl_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ctrl_card, 6, 0);

    /* Section title */
    lv_obj_t *ctrl_title = lv_label_create(ctrl_card);
    lv_label_set_text(ctrl_title, "CONTROLS");
    _style_section_title(ctrl_title);

    /* WiFi toggle row (always visible) */
    lv_obj_t *wifi_row = lv_obj_create(ctrl_card);
    lv_obj_set_size(wifi_row, LV_PCT(100), 32);
    lv_obj_set_style_bg_opa(wifi_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_row, 0, 0);
    lv_obj_set_style_pad_all(wifi_row, 0, 0);
    lv_obj_clear_flag(wifi_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wifi_lbl = lv_label_create(wifi_row);
    lv_label_set_text(wifi_lbl, "WiFi");
    lv_obj_set_style_text_font(wifi_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(wifi_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(wifi_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    wifi_toggle = lv_switch_create(wifi_row);
    lv_obj_set_size(wifi_toggle, 44, 22);
    lv_obj_align(wifi_toggle, LV_ALIGN_RIGHT_MID, 0, 0);
    _style_switch(wifi_toggle);
    lv_obj_add_event_cb(wifi_toggle, _wifi_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* -- WiFi-dependent section (hidden when WiFi is off) ---------------- */
    wifi_dependent_section = lv_obj_create(ctrl_card);
    lv_obj_set_size(wifi_dependent_section, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wifi_dependent_section, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_dependent_section, 0, 0);
    lv_obj_set_style_pad_all(wifi_dependent_section, 0, 0);
    lv_obj_set_style_pad_gap(wifi_dependent_section, 6, 0);
    lv_obj_set_flex_flow(wifi_dependent_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wifi_dependent_section, LV_OBJ_FLAG_SCROLLABLE);

    /* Hotspot toggle row */
    lv_obj_t *ap_row = lv_obj_create(wifi_dependent_section);
    lv_obj_set_size(ap_row, LV_PCT(100), 32);
    lv_obj_set_style_bg_opa(ap_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ap_row, 0, 0);
    lv_obj_set_style_pad_all(ap_row, 0, 0);
    lv_obj_clear_flag(ap_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ap_lbl = lv_label_create(ap_row);
    lv_label_set_text(ap_lbl, "Hotspot");
    lv_obj_set_style_text_font(ap_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(ap_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(ap_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    ap_toggle = lv_switch_create(ap_row);
    lv_obj_set_size(ap_toggle, 44, 22);
    lv_obj_align(ap_toggle, LV_ALIGN_RIGHT_MID, 0, 0);
    _style_switch(ap_toggle);
    lv_obj_add_event_cb(ap_toggle, _ap_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* AP info container (hidden when hotspot is off) */
    ap_info_container = lv_obj_create(wifi_dependent_section);
    lv_obj_set_size(ap_info_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ap_info_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ap_info_container, 0, 0);
    lv_obj_set_style_pad_all(ap_info_container, 0, 0);
    lv_obj_set_style_pad_gap(ap_info_container, 4, 0);
    lv_obj_set_flex_flow(ap_info_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(ap_info_container, LV_OBJ_FLAG_SCROLLABLE);

    _create_info_row(ap_info_container, "SSID:", &ap_ssid_label);
    _create_info_row(ap_info_container, "IP:", &ap_ip_label);

    /* AP password row */
    lv_obj_t *pass_lbl = lv_label_create(ap_info_container);
    lv_label_set_text(pass_lbl, "Hotspot Password:");
    lv_obj_set_style_text_font(pass_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(pass_lbl, THEME_COLOR_TEXT_MUTED, 0);

    lv_obj_t *pass_row = lv_obj_create(ap_info_container);
    lv_obj_set_size(pass_row, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(pass_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pass_row, 0, 0);
    lv_obj_set_style_pad_all(pass_row, 0, 0);
    lv_obj_clear_flag(pass_row, LV_OBJ_FLAG_SCROLLABLE);

    ap_pass_input = lv_textarea_create(pass_row);
    lv_obj_set_size(ap_pass_input, 200, 30);
    lv_obj_align(ap_pass_input, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_one_line(ap_pass_input, true);
    lv_textarea_set_max_length(ap_pass_input, 63);
    lv_textarea_set_placeholder_text(ap_pass_input, "Min 8 chars");
    lv_obj_set_style_bg_color(ap_pass_input, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(ap_pass_input, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ap_pass_input, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(ap_pass_input, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(ap_pass_input, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(ap_pass_input, 1, 0);
    lv_obj_set_style_radius(ap_pass_input, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_text_color(ap_pass_input, THEME_COLOR_TEXT_GHOST,
                                LV_PART_TEXTAREA_PLACEHOLDER);

    lv_obj_t *set_btn = lv_btn_create(pass_row);
    lv_obj_set_size(set_btn, 60, 28);
    lv_obj_align(set_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(set_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_bg_opa(set_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(set_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(set_btn, 0, 0);
    lv_obj_set_style_bg_color(set_btn, THEME_COLOR_BTN_SAVE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(set_btn, _ap_pass_set_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *set_lbl = lv_label_create(set_btn);
    lv_label_set_text(set_lbl, "Set");
    lv_obj_set_style_text_font(set_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(set_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_center(set_lbl);

    /* Error/status label below password row */
    ap_pass_error = lv_label_create(ap_info_container);
    lv_label_set_text(ap_pass_error, "");
    lv_obj_set_style_text_font(ap_pass_error, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(ap_pass_error, THEME_COLOR_STATUS_ERROR, 0);

    /* WiFi on Boot toggle row */
    lv_obj_t *boot_row = lv_obj_create(wifi_dependent_section);
    lv_obj_set_size(boot_row, LV_PCT(100), 32);
    lv_obj_set_style_bg_opa(boot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(boot_row, 0, 0);
    lv_obj_set_style_pad_all(boot_row, 0, 0);
    lv_obj_clear_flag(boot_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *boot_lbl = lv_label_create(boot_row);
    lv_label_set_text(boot_lbl, "WiFi on Boot");
    lv_obj_set_style_text_font(boot_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(boot_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(boot_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    boot_toggle = lv_switch_create(boot_row);
    lv_obj_set_size(boot_toggle, 44, 22);
    lv_obj_align(boot_toggle, LV_ALIGN_RIGHT_MID, 0, 0);
    _style_switch(boot_toggle);
    lv_obj_add_event_cb(boot_toggle, _boot_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* == RIGHT PANEL (available networks) ================================ */
    right_panel = lv_obj_create(body);
    lv_obj_set_size(right_panel, 390, 396);
    lv_obj_align(right_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    _style_section_card(right_panel);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(right_panel, 6, 0);

    lv_obj_t *scan_title = lv_label_create(right_panel);
    lv_label_set_text(scan_title, "AVAILABLE NETWORKS");
    _style_section_title(scan_title);

    /* Scan results list */
    wifi_list = lv_list_create(right_panel);
    lv_obj_set_size(wifi_list, LV_PCT(100), 300);
    lv_obj_set_style_bg_color(wifi_list, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(wifi_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_list, 0, 0);
    lv_obj_set_style_radius(wifi_list, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_pad_all(wifi_list, 2, 0);
    lv_obj_set_style_pad_gap(wifi_list, 2, 0);
    lv_obj_set_style_bg_color(wifi_list, THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(wifi_list, LV_OPA_50, LV_PART_SCROLLBAR);

    /* Connection spinner (hidden by default) */
    connection_spinner = lv_spinner_create(right_panel, 1000, 60);
    lv_obj_set_size(connection_spinner, 30, 30);
    lv_obj_set_style_arc_color(connection_spinner, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(connection_spinner, THEME_COLOR_SECTION_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(connection_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(connection_spinner, 4, LV_PART_MAIN);
    lv_obj_add_flag(connection_spinner, LV_OBJ_FLAG_HIDDEN);

    /* Scan button */
    scan_btn = lv_btn_create(right_panel);
    lv_obj_set_size(scan_btn, 140, 32);
    lv_obj_set_style_bg_color(scan_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_bg_opa(scan_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scan_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(scan_btn, 0, 0);
    lv_obj_set_style_bg_color(scan_btn, THEME_COLOR_BTN_SAVE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(scan_btn, _scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_grow(scan_btn, 0);

    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, "Scan Networks");
    lv_obj_set_style_text_font(scan_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(scan_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_center(scan_lbl);

    /* -- Load initial state ---------------------------------------------- */
    if (wifi_manager_is_started()) {
        lv_obj_add_state(wifi_toggle, LV_STATE_CHECKED);
    }
    if (wifi_manager_is_ap_enabled()) {
        lv_obj_add_state(ap_toggle, LV_STATE_CHECKED);
    }

    /* Load boot config */
    wifi_boot_config_t boot_cfg = { .wifi_on_boot = false, .ap_enabled = false };
    config_store_load_wifi_boot(&boot_cfg);
    if (boot_cfg.wifi_on_boot) {
        lv_obj_add_state(boot_toggle, LV_STATE_CHECKED);
    }

    /* Load AP password into input */
    rdm_ap_config_t ap_cfg;
    config_store_load_ap_config(&ap_cfg);
    if (ap_pass_input) {
        lv_textarea_set_text(ap_pass_input, ap_cfg.password);
    }

    /* Set initial visibility */
    _update_visibility();
    _refresh_status(NULL);

    /* Auto-scan on open if WiFi is running and not mid-connect */
    if (wifi_manager_is_started() &&
        wifi_manager_get_state() != WIFI_MGR_STATE_CONNECTING) {
        wifi_manager_scan();
    }

    /* Start periodic refresh timer */
    refresh_timer = lv_timer_create(_refresh_status, REFRESH_PERIOD_MS, NULL);
}

static void _destroy_screen(void)
{
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = NULL;
    }
    if (connect_timeout_timer) {
        lv_timer_del(connect_timeout_timer);
        connect_timeout_timer = NULL;
    }
    /* Clear modal pointers before deleting screen (children deleted by parent) */
    password_modal = NULL;
    password_input = NULL;
    wifi_keyboard  = NULL;
    if (wifi_screen) {
        lv_obj_del(wifi_screen);
        wifi_screen = NULL;
    }
    /* Clear pointers (children deleted by parent) */
    wifi_toggle = NULL;
    ap_toggle = NULL;
    boot_toggle = NULL;
    ap_ssid_label = NULL;
    ap_ip_label = NULL;
    ap_pass_input = NULL;
    ap_pass_error = NULL;
    status_label = NULL;
    wifi_list = NULL;
    connection_spinner = NULL;
    scan_btn = NULL;
    wifi_dependent_section = NULL;
    ap_info_container = NULL;
    right_panel = NULL;
}

/* =========================================================================
 * Style helpers
 * ========================================================================= */

static void _style_switch(lv_obj_t *sw)
{
    /* Track OFF */
    lv_obj_set_style_bg_color(sw, THEME_COLOR_INPUT_BG,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(sw, THEME_COLOR_BORDER,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Track ON */
    lv_obj_set_style_bg_color(sw, THEME_COLOR_ACCENT_BLUE,
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(sw, THEME_COLOR_ACCENT_BLUE,
                                  LV_PART_MAIN | LV_STATE_CHECKED);

    /* Knob */
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xE8E8E8), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -2, LV_PART_KNOB);
}

static void _style_section_card(lv_obj_t *card)
{
    lv_obj_set_style_bg_color(card, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(card, THEME_PAD_NORMAL, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

static void _style_section_title(lv_obj_t *label)
{
    lv_obj_set_style_text_font(label, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
}

static lv_obj_t *_create_info_row(lv_obj_t *parent, const char *label_text,
                                  lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 20);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "---");
    lv_obj_set_style_text_font(val, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(val, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);

    if (value_out) *value_out = val;
    return row;
}

/* =========================================================================
 * Visibility management
 * ========================================================================= */

static void _update_visibility(void)
{
    bool wifi_on = wifi_manager_is_started();

    /* WiFi-dependent controls in left panel */
    if (wifi_dependent_section) {
        if (wifi_on) {
            lv_obj_clear_flag(wifi_dependent_section, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wifi_dependent_section, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* AP info rows only when hotspot is on */
    if (ap_info_container) {
        bool ap_on = wifi_manager_is_ap_enabled();
        if (wifi_on && ap_on) {
            lv_obj_clear_flag(ap_info_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ap_info_container, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Right panel (available networks) only when WiFi is on */
    if (right_panel) {
        if (wifi_on) {
            lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(right_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* =========================================================================
 * Refresh / status updates
 * ========================================================================= */

static void _refresh_status(lv_timer_t *t)
{
    (void)t;
    if (!wifi_screen) return;

    wifi_mgr_state_t state = wifi_manager_get_state();

    /* Header status text */
    if (status_label) {
        static char status_buf[48];
        const char *status_text = "";
        lv_color_t status_color = THEME_COLOR_TEXT_MUTED;
        switch (state) {
        case WIFI_MGR_STATE_OFF:
            status_text = "Off";
            break;
        case WIFI_MGR_STATE_IDLE:
            status_text = "Idle";
            break;
        case WIFI_MGR_STATE_SCANNING:
            status_text = "Scanning...";
            status_color = THEME_COLOR_ACCENT_BLUE;
            break;
        case WIFI_MGR_STATE_CONNECTING:
            status_text = "Connecting...";
            status_color = THEME_COLOR_ACCENT_BLUE;
            break;
        case WIFI_MGR_STATE_CONNECTED: {
            const char *ip = wifi_manager_get_sta_ip();
            if (ip) {
                snprintf(status_buf, sizeof(status_buf), "Connected (%s)", ip);
                status_text = status_buf;
            } else {
                status_text = "Connected";
            }
            status_color = THEME_COLOR_STATUS_CONNECTED;
            break;
        }
        case WIFI_MGR_STATE_AP_ONLY:
            status_text = "AP Only";
            status_color = THEME_COLOR_ACCENT_ORANGE;
            break;
        case WIFI_MGR_STATE_FAILED:
            status_text = "Connection Failed";
            status_color = THEME_COLOR_STATUS_ERROR;
            break;
        }
        lv_label_set_text(status_label, status_text);
        lv_obj_set_style_text_color(status_label, status_color, 0);
    }

    /* AP info */
    if (ap_ssid_label) {
        const char *ap_ssid = wifi_manager_get_ap_ssid();
        lv_label_set_text(ap_ssid_label, ap_ssid ? ap_ssid : "---");
    }
    if (ap_ip_label) {
        const char *ap_ip = wifi_manager_get_ap_ip();
        lv_label_set_text(ap_ip_label, ap_ip ? ap_ip : "---");
    }

    /* Spinner visibility */
    if (connection_spinner) {
        if (state == WIFI_MGR_STATE_SCANNING || state == WIFI_MGR_STATE_CONNECTING) {
            lv_obj_clear_flag(connection_spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(connection_spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* WiFi toggle sync */
    if (wifi_toggle) {
        bool started = wifi_manager_is_started();
        if (started && !lv_obj_has_state(wifi_toggle, LV_STATE_CHECKED)) {
            lv_obj_add_state(wifi_toggle, LV_STATE_CHECKED);
        } else if (!started && lv_obj_has_state(wifi_toggle, LV_STATE_CHECKED)) {
            lv_obj_clear_state(wifi_toggle, LV_STATE_CHECKED);
        }
    }

    /* AP toggle sync */
    if (ap_toggle) {
        bool ap_on = wifi_manager_is_ap_enabled();
        if (ap_on && !lv_obj_has_state(ap_toggle, LV_STATE_CHECKED)) {
            lv_obj_add_state(ap_toggle, LV_STATE_CHECKED);
        } else if (!ap_on && lv_obj_has_state(ap_toggle, LV_STATE_CHECKED)) {
            lv_obj_clear_state(ap_toggle, LV_STATE_CHECKED);
        }
    }

    /* Update conditional visibility */
    _update_visibility();
}

/* =========================================================================
 * SSID sanitization — replace characters unsupported by built-in fonts
 * ========================================================================= */

static void _sanitize_ssid(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    size_t si = 0;
    size_t src_len = strlen(src);

    while (si < src_len && di < dst_size - 1) {
        uint8_t c = (uint8_t)src[si];
        if (c >= 0x20 && c <= 0x7E) {
            /* Printable ASCII — always safe */
            dst[di++] = (char)c;
            si++;
        } else if (c < 0x20) {
            /* Control character — replace */
            dst[di++] = '?';
            si++;
        } else {
            /* Non-ASCII byte (UTF-8 lead or continuation).
             * Skip the entire multi-byte sequence and emit one '?' */
            if ((c & 0xE0) == 0xC0)      si += 2;   /* 2-byte seq */
            else if ((c & 0xF0) == 0xE0)  si += 3;   /* 3-byte seq */
            else if ((c & 0xF8) == 0xF0)  si += 4;   /* 4-byte seq */
            else                           si += 1;   /* invalid byte */
            dst[di++] = '?';
        }
    }
    dst[di] = '\0';
}

/* =========================================================================
 * Scan results list population
 * ========================================================================= */

/* Returns true if the given SSID is in the multi-network saved list. */
static bool _ssid_is_saved(const wifi_credentials_t *list, uint8_t list_count,
                           const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return false;
    for (uint8_t i = 0; i < list_count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) return true;
    }
    return false;
}

static void _populate_scan_list(void)
{
    if (!wifi_list) return;

    lv_obj_clean(wifi_list);

    wifi_mgr_ap_record_t records[MAX_SCAN_RESULTS];
    uint16_t count = wifi_manager_get_scan_results(records, MAX_SCAN_RESULTS);

    /* Load the full multi-SSID saved list — any network in the list gets the
     * "saved" highlight + per-row Forget button. (Was previously slot-0 only.) */
    wifi_credentials_t saved_list[CONFIG_STORE_WIFI_SLOT_COUNT];
    uint8_t saved_count = 0;
    config_store_load_wifi_list(saved_list, &saved_count);

    if (count == 0 && saved_count == 0) {
        lv_obj_t *empty = lv_list_add_text(wifi_list, "No networks found");
        lv_obj_set_style_text_font(empty, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(empty, THEME_COLOR_TEXT_MUTED, 0);
        return;
    }

    if (count == 0) {
        /* No scan results but we have saved networks — fall through to the
         * "out of range" loop at the bottom so they're at least manageable. */
        lv_obj_t *empty = lv_list_add_text(wifi_list, "No networks in range");
        lv_obj_set_style_text_font(empty, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(empty, THEME_COLOR_TEXT_MUTED, 0);
    }

    for (uint16_t i = 0; i < count; i++) {
        bool is_saved = _ssid_is_saved(saved_list, saved_count, records[i].ssid);

        /* Sanitize SSID for display (font may lack non-ASCII glyphs) */
        char safe_ssid[33];
        _sanitize_ssid(records[i].ssid, safe_ssid, sizeof(safe_ssid));

        /* Build display string */
        char item_text[80];
        snprintf(item_text, sizeof(item_text), "%s   %s  %s",
                 safe_ssid,
                 _rssi_bars(records[i].rssi),
                 _auth_text(records[i].auth_mode));

        lv_obj_t *btn = lv_list_add_btn(wifi_list, _rssi_icon(records[i].rssi),
                                         item_text);
        lv_obj_set_style_bg_color(btn, THEME_COLOR_SECTION_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, THEME_COLOR_SCROLLBAR, LV_STATE_PRESSED);
        lv_obj_set_style_text_font(btn, THEME_FONT_SMALL, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_ver(btn, 6, 0);

        if (is_saved) {
            /* Highlight saved network in blue */
            lv_obj_set_style_text_color(btn, THEME_COLOR_STATUS_CONNECTED, 0);

            /* Add a small "Forget" button on the right side. Per-row SSID
             * context is passed via user_data so we forget the right entry
             * (was previously slot-0 only). */
            char *forget_ssid = lv_mem_alloc(33);
            if (forget_ssid) {
                strncpy(forget_ssid, records[i].ssid, 32);
                forget_ssid[32] = '\0';
            }

            lv_obj_t *forget_btn_inline = lv_btn_create(btn);
            lv_obj_set_size(forget_btn_inline, 56, 22);
            lv_obj_align(forget_btn_inline, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(forget_btn_inline, THEME_COLOR_BTN_DANGER_BG, 0);
            lv_obj_set_style_bg_opa(forget_btn_inline, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(forget_btn_inline, THEME_COLOR_STATUS_ERROR, 0);
            lv_obj_set_style_border_width(forget_btn_inline, 1, 0);
            lv_obj_set_style_radius(forget_btn_inline, THEME_RADIUS_SMALL, 0);
            lv_obj_set_style_shadow_width(forget_btn_inline, 0, 0);
            lv_obj_set_style_pad_all(forget_btn_inline, 0, 0);
            lv_obj_add_event_cb(forget_btn_inline, _forget_cb, LV_EVENT_ALL, forget_ssid);

            lv_obj_t *fgt_lbl = lv_label_create(forget_btn_inline);
            lv_label_set_text(fgt_lbl, "Forget");
            lv_obj_set_style_text_font(fgt_lbl, THEME_FONT_TINY, 0);
            lv_obj_set_style_text_color(fgt_lbl, THEME_COLOR_STATUS_ERROR, 0);
            lv_obj_center(fgt_lbl);
        } else {
            lv_obj_set_style_text_color(btn, THEME_COLOR_TEXT_PRIMARY, 0);
        }

        /* Store SSID in user_data for click handler */
        char *ssid_copy = lv_mem_alloc(33);
        if (ssid_copy) {
            strncpy(ssid_copy, records[i].ssid, 32);
            ssid_copy[32] = '\0';
        }
        lv_obj_add_event_cb(btn, _network_item_cb, LV_EVENT_ALL, ssid_copy);
    }

    /* Append "Saved (out of range)" entries — known networks that didn't
     * appear in the scan, so the user can still forget them or auto-connect
     * if they roam back into range. */
    bool any_out_of_range = false;
    for (uint8_t s = 0; s < saved_count; s++) {
        bool in_scan = false;
        for (uint16_t i = 0; i < count; i++) {
            if (strcmp(records[i].ssid, saved_list[s].ssid) == 0) {
                in_scan = true;
                break;
            }
        }
        if (in_scan) continue;

        /* Section header before the first out-of-range entry */
        if (!any_out_of_range) {
            lv_obj_t *hdr = lv_list_add_text(wifi_list, "SAVED (out of range)");
            lv_obj_set_style_text_font(hdr, THEME_FONT_TINY, 0);
            lv_obj_set_style_text_color(hdr, THEME_COLOR_TEXT_MUTED, 0);
            lv_obj_set_style_pad_top(hdr, 8, 0);
            any_out_of_range = true;
        }

        char safe_ssid[33];
        _sanitize_ssid(saved_list[s].ssid, safe_ssid, sizeof(safe_ssid));

        lv_obj_t *btn = lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, safe_ssid);
        lv_obj_set_style_bg_color(btn, THEME_COLOR_SECTION_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, THEME_COLOR_SCROLLBAR, LV_STATE_PRESSED);
        lv_obj_set_style_text_font(btn, THEME_FONT_SMALL, 0);
        /* Dim out-of-range so they read as "saved but unreachable" */
        lv_obj_set_style_text_color(btn, THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_ver(btn, 6, 0);

        /* Forget button — same per-row pattern, dispatches to _forget_cb */
        char *forget_ssid = lv_mem_alloc(33);
        if (forget_ssid) {
            strncpy(forget_ssid, saved_list[s].ssid, 32);
            forget_ssid[32] = '\0';
        }
        lv_obj_t *forget_btn = lv_btn_create(btn);
        lv_obj_set_size(forget_btn, 56, 22);
        lv_obj_align(forget_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(forget_btn, THEME_COLOR_BTN_DANGER_BG, 0);
        lv_obj_set_style_bg_opa(forget_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(forget_btn, THEME_COLOR_STATUS_ERROR, 0);
        lv_obj_set_style_border_width(forget_btn, 1, 0);
        lv_obj_set_style_radius(forget_btn, THEME_RADIUS_SMALL, 0);
        lv_obj_set_style_shadow_width(forget_btn, 0, 0);
        lv_obj_set_style_pad_all(forget_btn, 0, 0);
        lv_obj_add_event_cb(forget_btn, _forget_cb, LV_EVENT_ALL, forget_ssid);

        lv_obj_t *fgt_lbl = lv_label_create(forget_btn);
        lv_label_set_text(fgt_lbl, "Forget");
        lv_obj_set_style_text_font(fgt_lbl, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(fgt_lbl, THEME_COLOR_STATUS_ERROR, 0);
        lv_obj_center(fgt_lbl);

        /* Row tap is a no-op for out-of-range — there's nothing to connect to.
         * If the user wants to connect, they need to wait for it to appear in
         * the next scan. */
    }
}

/* =========================================================================
 * Password modal
 * ========================================================================= */

static void _show_password_modal(const char *ssid)
{
    if (password_modal) return;

    strncpy(selected_ssid, ssid, sizeof(selected_ssid) - 1);
    selected_ssid[sizeof(selected_ssid) - 1] = '\0';

    /* Overlay */
    password_modal = lv_obj_create(wifi_screen);
    lv_obj_set_size(password_modal, SCREEN_W, SCREEN_H);
    lv_obj_align(password_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(password_modal, THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(password_modal, LV_OPA_50, 0);
    lv_obj_set_style_border_width(password_modal, 0, 0);
    lv_obj_clear_flag(password_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Modal dialog */
    lv_obj_t *dialog = lv_obj_create(password_modal);
    lv_obj_set_size(dialog, 420, 260);
    lv_obj_align(dialog, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(dialog, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_radius(dialog, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_shadow_width(dialog, 20, 0);
    lv_obj_set_style_shadow_ofs_y(dialog, 4, 0);
    lv_obj_set_style_shadow_opa(dialog, 140, 0);
    lv_obj_set_style_shadow_color(dialog, lv_color_black(), 0);
    lv_obj_set_style_pad_all(dialog, THEME_PAD_MEDIUM, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *modal_title = lv_label_create(dialog);
    char safe_modal_ssid[33];
    _sanitize_ssid(selected_ssid, safe_modal_ssid, sizeof(safe_modal_ssid));
    lv_label_set_text_fmt(modal_title, LV_SYMBOL_WIFI "  Connect to %s", safe_modal_ssid);
    lv_obj_set_style_text_font(modal_title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(modal_title, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(modal_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_width(modal_title, 390);
    lv_label_set_long_mode(modal_title, LV_LABEL_LONG_DOT);

    /* Password label */
    lv_obj_t *pw_label = lv_label_create(dialog);
    lv_label_set_text(pw_label, "Password:");
    lv_obj_set_style_text_font(pw_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(pw_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_align(pw_label, LV_ALIGN_TOP_LEFT, 0, 35);

    /* Password text area */
    password_input = lv_textarea_create(dialog);
    lv_obj_set_size(password_input, 390, 40);
    lv_obj_align(password_input, LV_ALIGN_TOP_MID, 0, 55);
    lv_textarea_set_placeholder_text(password_input, "Enter password...");
    lv_textarea_set_password_mode(password_input, true);
    lv_textarea_set_one_line(password_input, true);
    lv_textarea_set_max_length(password_input, 64);
    lv_obj_set_style_bg_color(password_input, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(password_input, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(password_input, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(password_input, THEME_FONT_BODY, 0);
    lv_obj_set_style_border_color(password_input, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(password_input, 1, 0);
    lv_obj_set_style_border_color(password_input, THEME_COLOR_ACCENT_BLUE,
                                  LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_radius(password_input, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_text_color(password_input, THEME_COLOR_TEXT_GHOST,
                                LV_PART_TEXTAREA_PLACEHOLDER);

    /* Buttons row */
    lv_obj_t *cancel_btn = lv_btn_create(dialog);
    lv_obj_set_size(cancel_btn, 140, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 20, 110);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancel_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_SCROLLBAR, LV_STATE_PRESSED);
    lv_obj_add_event_cb(cancel_btn, _password_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE "  Cancel");
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_BODY, 0);
    lv_obj_set_style_text_color(cancel_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *connect_btn = lv_btn_create(dialog);
    lv_obj_set_size(connect_btn, 140, 36);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, -20, 110);
    lv_obj_set_style_bg_color(connect_btn, THEME_COLOR_BTN_CONNECT, 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(connect_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(connect_btn, 0, 0);
    lv_obj_set_style_bg_color(connect_btn, THEME_COLOR_BTN_CONNECT_PRESSED,
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(connect_btn, _password_connect_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *conn_lbl = lv_label_create(connect_btn);
    lv_label_set_text(conn_lbl, LV_SYMBOL_OK "  Connect");
    lv_obj_set_style_text_font(conn_lbl, THEME_FONT_BODY, 0);
    lv_obj_set_style_text_color(conn_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_center(conn_lbl);

    /* Keyboard */
    wifi_keyboard = lv_keyboard_create(password_modal);
    lv_obj_set_size(wifi_keyboard, SCREEN_W, 200);
    lv_obj_align(wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(wifi_keyboard, password_input);

    lv_obj_set_style_bg_color(wifi_keyboard, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(wifi_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_keyboard, 0, 0);
    lv_obj_set_style_pad_all(wifi_keyboard, 4, 0);
    lv_obj_set_style_pad_gap(wifi_keyboard, 4, 0);

    lv_obj_set_style_bg_color(wifi_keyboard, THEME_COLOR_SECTION_BG,
                              LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(wifi_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(wifi_keyboard, THEME_COLOR_TEXT_PRIMARY,
                                LV_PART_ITEMS);
    lv_obj_set_style_text_font(wifi_keyboard, THEME_FONT_BODY, LV_PART_ITEMS);
    lv_obj_set_style_border_width(wifi_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(wifi_keyboard, THEME_COLOR_BORDER,
                                  LV_PART_ITEMS);
    lv_obj_set_style_radius(wifi_keyboard, THEME_RADIUS_NORMAL, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(wifi_keyboard, THEME_COLOR_ACCENT_BLUE,
                              LV_PART_ITEMS | LV_STATE_PRESSED);
}

static void _close_password_modal(void)
{
    if (!password_modal) return;
    lv_obj_del(password_modal);
    password_modal = NULL;
    password_input = NULL;
    wifi_keyboard = NULL;
}

/* =========================================================================
 * Event callbacks
 * ========================================================================= */

static void _back_btn_cb(lv_event_t *e)
{
    (void)e;
    wifi_ui_hide();
}

static void _wifi_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool checked = lv_obj_has_state(wifi_toggle, LV_STATE_CHECKED);

    if (checked) {
        wifi_manager_start();
        web_server_start();
        ESP_LOGI(TAG, "WiFi started");
        /* Try to reconnect to saved network (scans first, connects if found).
         * Falls back to a plain scan so the network list still populates. */
        wifi_manager_auto_connect();
    } else {
        web_server_stop();
        wifi_manager_stop();
        ESP_LOGI(TAG, "WiFi stopped");
    }

    _update_visibility();
}

static void _ap_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool checked = lv_obj_has_state(ap_toggle, LV_STATE_CHECKED);
    wifi_manager_enable_ap(checked);

    /* Persist AP enabled state to NVS so it survives reboot */
    rdm_ap_config_t ap_cfg;
    config_store_load_ap_config(&ap_cfg);
    ap_cfg.enabled = checked;
    config_store_save_ap_config(&ap_cfg);

    ESP_LOGI(TAG, "AP %s (saved to NVS)", checked ? "enabled" : "disabled");

    _update_visibility();
}

static void _boot_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool checked = lv_obj_has_state(boot_toggle, LV_STATE_CHECKED);

    wifi_boot_config_t boot_cfg;
    config_store_load_wifi_boot(&boot_cfg);
    boot_cfg.wifi_on_boot = checked;
    config_store_save_wifi_boot(&boot_cfg);

    ESP_LOGI(TAG, "WiFi on boot: %s", checked ? "enabled" : "disabled");
}


static void _ap_pass_set_cb(lv_event_t *e)
{
    (void)e;
    if (!ap_pass_input) return;

    const char *new_pass = lv_textarea_get_text(ap_pass_input);
    size_t len = new_pass ? strlen(new_pass) : 0;

    if (len > 0 && len < 8) {
        if (ap_pass_error) {
            lv_label_set_text(ap_pass_error, "Minimum 8 characters required");
            lv_obj_set_style_text_color(ap_pass_error, THEME_COLOR_STATUS_ERROR, 0);
        }
        return;
    }

    /* Save to NVS */
    rdm_ap_config_t ap_cfg;
    config_store_load_ap_config(&ap_cfg);
    memset(ap_cfg.password, 0, sizeof(ap_cfg.password));
    if (len >= 8) {
        strncpy(ap_cfg.password, new_pass, sizeof(ap_cfg.password) - 1);
    }
    config_store_save_ap_config(&ap_cfg);

    /* Apply to running AP */
    wifi_manager_set_ap_password(len >= 8 ? new_pass : "");

    if (ap_pass_error) {
        if (len == 0) {
            lv_label_set_text(ap_pass_error, "Password cleared (hotspot is open)");
            lv_obj_set_style_text_color(ap_pass_error, THEME_COLOR_ACCENT_ORANGE, 0);
        } else {
            lv_label_set_text(ap_pass_error, "Password updated");
            lv_obj_set_style_text_color(ap_pass_error, THEME_COLOR_STATUS_CONNECTED, 0);
        }
    }

    ESP_LOGI(TAG, "AP password %s", len >= 8 ? "updated" : "cleared (open)");
}

static void _scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!wifi_manager_is_started()) {
        ESP_LOGW(TAG, "WiFi not started, cannot scan");
        return;
    }
    if (wifi_manager_get_state() == WIFI_MGR_STATE_SCANNING) {
        ESP_LOGD(TAG, "Scan already in progress");
        return;
    }
    wifi_manager_scan();
    ESP_LOGI(TAG, "Scan initiated");
}

/* Look up an SSID's saved credentials. Returns true if found and copies the
 * full record into *out. Walks the multi-list — same source of truth used by
 * the wifi manager's auto-reconnect path. */
static bool _find_saved_credentials(const char *ssid, wifi_credentials_t *out)
{
    if (!ssid || !out) return false;
    wifi_credentials_t list[CONFIG_STORE_WIFI_SLOT_COUNT];
    uint8_t list_count = 0;
    if (config_store_load_wifi_list(list, &list_count) != ESP_OK) return false;
    for (uint8_t i = 0; i < list_count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            *out = list[i];
            return true;
        }
    }
    return false;
}

/* Initiate a connect using already-known credentials. Used both by the
 * tap-on-saved-network path (no password prompt) and the Saved Networks
 * card (below). Updates slot 0 so auto_connect on next boot picks the
 * most-recently-used network. */
static void _connect_using_saved(const wifi_credentials_t *creds)
{
    if (!creds || creds->ssid[0] == '\0') return;

    ESP_LOGI(TAG, "Connecting to saved network '%s' (no prompt)", creds->ssid);

    wifi_credentials_t to_save = *creds;
    to_save.auto_connect = true;
    config_store_save_wifi(&to_save);

    wifi_manager_connect(creds->ssid, creds->password);

    if (connect_timeout_timer) {
        lv_timer_del(connect_timeout_timer);
        connect_timeout_timer = NULL;
    }
    connect_timeout_timer = lv_timer_create(_connect_timeout_cb,
                                             CONNECT_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(connect_timeout_timer, 1);
}

static void _network_item_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    char *ssid = (char *)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED) {
        if (!ssid) return;
        /* If this SSID is already saved, skip the password modal and
         * connect using the stored password. */
        wifi_credentials_t saved = {0};
        if (_find_saved_credentials(ssid, &saved)) {
            _connect_using_saved(&saved);
        } else {
            _show_password_modal(ssid);
        }
    } else if (code == LV_EVENT_DELETE) {
        if (ssid) lv_mem_free(ssid);
    }
}

static void _password_connect_cb(lv_event_t *e)
{
    (void)e;
    if (!password_input) return;

    const char *password = lv_textarea_get_text(password_input);
    ESP_LOGI(TAG, "Connecting to '%s'", selected_ssid);

    /* Save credentials to slot 0 (auto-connect target) AND to the multi-list
     * (so it appears as a saved network in the Saved Networks card and
     * subsequent scans). wifi_manager.c also adds to the list on a successful
     * connection event, but we add eagerly here so the UI reflects the save
     * even if the connect fails — matches the user's mental model of
     * "I typed the password, it's saved now." */
    wifi_credentials_t creds = {0};
    strncpy(creds.ssid, selected_ssid, sizeof(creds.ssid) - 1);
    if (password) {
        strncpy(creds.password, password, sizeof(creds.password) - 1);
    }
    creds.auto_connect = true;
    config_store_save_wifi(&creds);
    config_store_add_wifi(&creds);

    /* Initiate connection */
    wifi_manager_connect(selected_ssid, password);

    /* Start connection timeout */
    if (connect_timeout_timer) {
        lv_timer_del(connect_timeout_timer);
        connect_timeout_timer = NULL;
    }
    connect_timeout_timer = lv_timer_create(_connect_timeout_cb,
                                             CONNECT_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(connect_timeout_timer, 1);

    _close_password_modal();
}

static void _password_cancel_cb(lv_event_t *e)
{
    (void)e;
    _close_password_modal();
}

static void _forget_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    char *ssid = (char *)lv_event_get_user_data(e);

    if (code == LV_EVENT_DELETE) {
        if (ssid) lv_mem_free(ssid);
        return;
    }

    if (code != LV_EVENT_CLICKED) return;

    if (!ssid || ssid[0] == '\0') {
        /* Fallback to legacy behavior — clear slot 0 + disconnect. */
        wifi_manager_forget();
        ESP_LOGI(TAG, "Network forgotten (legacy slot-0)");
        _populate_scan_list();
        return;
    }

    /* Remove from the multi-list */
    esp_err_t err = config_store_remove_wifi(ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config_store_remove_wifi('%s') -> %s",
                 ssid, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Forgotten saved network '%s'", ssid);
    }

    /* If the forgotten SSID is the active connection, also disconnect and
     * clear slot 0 so we don't auto-reconnect to it on next boot. */
    const char *connected = wifi_manager_get_connected_ssid();
    if (connected && strcmp(connected, ssid) == 0) {
        ESP_LOGI(TAG, "Forgetting active network — disconnecting");
        wifi_manager_forget();
    }

    _populate_scan_list();
}

static void _connect_timeout_cb(lv_timer_t *t)
{
    (void)t;
    connect_timeout_timer = NULL;

    wifi_mgr_state_t state = wifi_manager_get_state();
    if (state == WIFI_MGR_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Connection timeout");
        wifi_manager_disconnect();
    }
}

/* =========================================================================
 * wifi_manager event callback (may run off LVGL task)
 * ========================================================================= */

static void _async_refresh(void *data)
{
    (void)data;
    if (!wifi_screen) return;

    wifi_mgr_state_t state = wifi_manager_get_state();

    /* If scan just completed, populate list */
    if (state != WIFI_MGR_STATE_SCANNING) {
        _populate_scan_list();
    }

    /* If connected, cancel timeout timer */
    if (state == WIFI_MGR_STATE_CONNECTED && connect_timeout_timer) {
        lv_timer_del(connect_timeout_timer);
        connect_timeout_timer = NULL;
    }

    /* Refresh all UI elements */
    _refresh_status(NULL);
}

static void _wifi_mgr_event_cb(wifi_mgr_state_t new_state, void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "WiFi manager state change: %d", (int)new_state);
    /* _set_state already defers via lv_async_call, so this callback
     * runs on the LVGL task — safe to call lv_async_call directly. */
    lv_async_call(_async_refresh, NULL);
}
