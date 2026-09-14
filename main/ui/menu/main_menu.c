/*
 * main_menu.c — the launcher and its Layouts page (ADR-0075).
 *
 * Built only from ui_kit parts. Everything the tiles open that already
 * existed (brightness, recording, the car / dash / connect settings) still
 * lives in device_settings.c; this file is the front door and the one place
 * that decides how menu screens replace each other.
 */
#include "menu/main_menu.h"

#include "kit/ui_kit.h"
#include "menu/menu_screen.h"
#include "settings/device_settings.h"
#include "screens/ui_Screen3.h"
#include "screens/ui_peaks.h"
#include "screens/first_run_wizard.h"
#include "system/rdm_lv_async.h"
#include "layout/layout_manager.h"
#include "storage/config_store.h"
#include "storage/data_logger.h"
#include "data/channel_manager.h"
#include "can/can_manager.h"
#include "net/wifi_manager.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_return   = NULL;   /* where Close goes */
static bool      s_dirty    = false;

/* Launcher live parts — valid only while the launcher screen exists. */
static lv_obj_t   *s_launcher     = NULL;
static lv_obj_t   *s_rec_tile     = NULL;
static lv_obj_t   *s_screen_tile  = NULL;
static lv_obj_t   *s_can_lbl      = NULL;
static lv_obj_t   *s_can_dot      = NULL;
static lv_timer_t *s_tick         = NULL;
static uint32_t    s_rx_seen      = 0;
static uint32_t    s_rx_change_ms = 0;

lv_obj_t *main_menu_return_screen(void) { return s_return; }
void main_menu_mark_dirty(void) { s_dirty = true; }

void main_menu_swap_to(lv_obj_t *screen)
{
    lv_obj_t *old = lv_scr_act();
    lv_scr_load(screen);
    if (old && old != screen && old != s_return && lv_obj_is_valid(old))
        rdm_obj_del_async(old);
}

/* ── Close / Back ─────────────────────────────────────────────────────── */

void main_menu_close_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *ret = s_return;
    bool dirty = s_dirty;
    s_return = NULL;
    s_dirty = false;

    if (dirty || !ret || !lv_obj_is_valid(ret)) {
        /* Settings that shape the dashboard were open (CAN, channels, OBD2):
         * re-apply the CAN filter and rebuild the dashboard from its saved
         * layout. Not close_menu_event_cb: that also SAVES the widgets, which
         * menus never edit, and saving "default" forks it into
         * "default_modified" — a copy nobody asked for, every time. */
        reconfigure_can_filter();
        ui_Screen3_rebuild();
        return;
    }
    lv_obj_t *old = lv_scr_act();
    lv_scr_load(ret);
    if (old && old != ret && lv_obj_is_valid(old)) rdm_obj_del_async(old);
}

void main_menu_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    main_menu_open(s_return);
}

/* ── Launcher status ──────────────────────────────────────────────────── */

static const char *_bitrate_short(void)
{
    static const char *k[] = {"125k", "250k", "500k", "1M"};
    uint8_t i = can_get_bitrate_index();
    return k[i > 3 ? 2 : i];
}

static void _fmt_elapsed(char *buf, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(buf, n, "%u:%02u:%02u", (unsigned)(s / 3600),
                            (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(buf, n, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void _refresh(void)
{
    if (s_rec_tile && lv_obj_is_valid(s_rec_tile)) {
        char rate[12];
        uint16_t hz = data_logger_get_rate_hz();
        if (hz) snprintf(rate, sizeof(rate), "%u Hz", hz);
        else snprintf(rate, sizeof(rate), "Max rate");
        if (data_logger_is_active()) {
            char t[16];
            _fmt_elapsed(t, sizeof(t), data_logger_get_elapsed_ms());
            uk_tile_set_status(s_rec_tile, t, rate, UK_TONE_TEXT);
            uk_tile_set_badge(s_rec_tile, "Rec", UK_TONE_ACCENT, true);
        } else {
            uk_tile_set_status(s_rec_tile, "Off", rate, UK_TONE_TEXT);
            uk_tile_set_badge(s_rec_tile, NULL, UK_TONE_ACCENT, false);
        }
    }
    if (s_screen_tile && lv_obj_is_valid(s_screen_tile)) {
        char b[8];
        snprintf(b, sizeof(b), "%u%%", current_brightness);
        uk_tile_set_status(s_screen_tile, b,
                           display_is_dimmed() ? "dimmed" : "brightness", UK_TONE_TEXT);
    }
    if (s_can_lbl && lv_obj_is_valid(s_can_lbl)) {
        uint32_t now = lv_tick_get();
        uint32_t rx = can_get_rx_frame_count();
        if (rx != s_rx_seen) { s_rx_seen = rx; s_rx_change_ms = now; }
        bool live = s_rx_change_ms && (now - s_rx_change_ms) < 2500;
        char t[24];
        snprintf(t, sizeof(t), "CAN %s", _bitrate_short());
        lv_label_set_text(s_can_lbl, t);
        if (s_can_dot && lv_obj_is_valid(s_can_dot))
            lv_obj_set_style_bg_color(s_can_dot, live ? ui_pal->ok : ui_pal->text_hint, 0);
    }
}

static void _tick_cb(lv_timer_t *t) { (void)t; _refresh(); }

static void _launcher_deleted_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_launcher) return;
    if (s_tick) { lv_timer_del(s_tick); s_tick = NULL; }
    s_launcher = s_rec_tile = s_screen_tile = s_can_lbl = s_can_dot = NULL;
}

/* ── Tile actions ─────────────────────────────────────────────────────── */

static void _open_layouts_page(void);

static void _t_layouts(lv_event_t *e)   { (void)e; _open_layouts_page(); }
static void _t_screen(lv_event_t *e)    { (void)e; device_settings_open_popup(DS_POPUP_SCREEN); }
static void _t_recording(lv_event_t *e) { (void)e; device_settings_open_popup(DS_POPUP_RECORDING); }
static void _t_car(lv_event_t *e)       { (void)e; device_settings_open_page(DS_PAGE_CAR); }
static void _t_connect(lv_event_t *e)   { (void)e; device_settings_open_page(DS_PAGE_CONNECT); }
static void _t_dash(lv_event_t *e)      { (void)e; device_settings_open_page(DS_PAGE_DASH); }

static void _show_peaks_async(void *arg) { (void)arg; peaks_ui_show(); }
static void _t_live(lv_event_t *e)      { (void)e; lv_async_call(_show_peaks_async, NULL); }

static void _t_channels(lv_event_t *e)
{
    (void)e;
    main_menu_mark_dirty();
    first_run_wizard_open_channels();
}

void main_menu_open(lv_obj_t *return_screen)
{
    uk_init();
    if (return_screen) s_return = return_screen;

    lv_obj_t *scr = uk_screen();
    s_launcher = scr;
    lv_obj_add_event_cb(scr, _launcher_deleted_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *bar = uk_bar(scr, "Menu", UK_BAR_CLOSE, main_menu_close_cb, NULL);
    s_can_lbl = uk_bar_status(bar, true, UK_TONE_OK, "CAN");
    s_can_dot = lv_obj_get_child(lv_obj_get_parent(s_can_lbl), 0);
    s_rx_seen = can_get_rx_frame_count();
    s_rx_change_ms = 0;

    lv_obj_t *grid = uk_grid(uk_body(scr), 4, 2);

    /* Layouts — the hero: it is what most taps on Setup are for. */
    char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
    int n = layout_manager_list(names, LAYOUT_MAX_COUNT);
    int saved = 0;
    for (int i = 0; i < n; i++) if (names[i][0] != '_') saved++;
    char active[LAYOUT_MAX_NAME] = "";
    layout_manager_get_active(active, sizeof(active));
    char rest[16];
    snprintf(rest, sizeof(rest), "%d saved", saved);

    lv_obj_t *t;
    t = uk_tile(grid, UK_ICON_LAYOUTS, "Layouts", NULL, UK_TILE_HERO, _t_layouts, NULL);
    uk_grid_place(t, 0, 0, 1, 1);
    uk_tile_set_status(t, active[0] ? active : "None", rest, UK_TONE_TEXT);

    s_screen_tile = uk_tile(grid, UK_ICON_SUN, "Screen", NULL, UK_TILE_NORMAL, _t_screen, NULL);
    uk_grid_place(s_screen_tile, 1, 0, 1, 1);

    s_rec_tile = uk_tile(grid, UK_ICON_REC, "Recording", NULL, UK_TILE_NORMAL, _t_recording, NULL);
    uk_grid_place(s_rec_tile, 2, 0, 1, 1);

    t = uk_tile(grid, UK_ICON_CHART, "Live data", NULL, UK_TILE_NORMAL, _t_live, NULL);
    uk_grid_place(t, 3, 0, 1, 1);
    uk_tile_set_status(t, NULL, "Values and peaks", UK_TONE_MUTED);

    size_t ch_total = channel_manager_count(), ch_bound = 0;
    for (size_t i = 0; i < ch_total; i++) {
        const channel_t *c = channel_manager_at(i);
        if (c && c->signal_index >= 0) ch_bound++;
    }
    char chn[12];
    snprintf(chn, sizeof(chn), "%u", (unsigned)ch_bound);
    t = uk_tile(grid, UK_ICON_CHANNELS, "Channels", NULL, UK_TILE_NORMAL, _t_channels, NULL);
    uk_grid_place(t, 0, 1, 1, 1);
    uk_tile_set_status(t, chn, "mapped", UK_TONE_TEXT);

    char car[24];
    snprintf(car, sizeof(car), "CAN %s", _bitrate_short());
    t = uk_tile(grid, UK_ICON_CAR, "Your car", NULL, UK_TILE_NORMAL, _t_car, NULL);
    uk_grid_place(t, 1, 1, 1, 1);
    uk_tile_set_status(t, car, "OBD2, gear", UK_TONE_TEXT);

    t = uk_tile(grid, UK_ICON_WIFI, "Connect", NULL, UK_TILE_NORMAL, _t_connect, NULL);
    uk_grid_place(t, 2, 1, 1, 1);
    const char *ssid = wifi_manager_get_connected_ssid();
    if (ssid && ssid[0]) uk_tile_set_status(t, ssid, "web editor", UK_TONE_TEXT);
    else if (wifi_manager_is_started() && wifi_manager_is_ap_enabled())
        uk_tile_set_status(t, "Hotspot", "web editor", UK_TONE_TEXT);
    else uk_tile_set_status(t, NULL, "WiFi, web editor", UK_TONE_MUTED);

    char fw[16];
    snprintf(fw, sizeof(fw), "v%s", FIRMWARE_VERSION);
    t = uk_tile(grid, UK_ICON_INFO, "This dash", NULL, UK_TILE_NORMAL, _t_dash, NULL);
    uk_grid_place(t, 3, 1, 1, 1);
    uk_tile_set_status(t, fw, "updates, reset", UK_TONE_TEXT);

    device_settings_attach(scr);
    _refresh();
    if (s_tick) lv_timer_del(s_tick);
    s_tick = lv_timer_create(_tick_cb, 1000, NULL);

    main_menu_swap_to(scr);
}

/* ── Layouts page ─────────────────────────────────────────────────────── */

/* Row = one choice in a list. Checked rows get the accent edge and a tick. */
static lv_obj_t *_choice(lv_obj_t *parent, const char *text, bool checked,
                         lv_event_cb_t cb, const char *user_name)
{
    lv_obj_t *b = uk_btn(parent, checked ? UK_ICON_CHECK : UK_ICON_NONE, text,
                         checked ? UK_BTN_ON : UK_BTN_NEUTRAL, cb, NULL);
    lv_obj_set_size(b, lv_pct(100), 48);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(b, 14, 0);
    lv_obj_t *l = uk_btn_label(b);
    lv_obj_set_style_text_font(l, uk_font(UK_FONT_LABEL), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    /* Stash the name the row stands for on the row itself. */
    if (user_name) {
        char buf[LAYOUT_MAX_NAME + 1];
        snprintf(buf, sizeof(buf), "%s", user_name);
        for (char *c = buf; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        lv_label_set_text(l, buf);
        lv_obj_t *key = lv_label_create(b);          /* hidden: the real name */
        lv_label_set_text(key, user_name);
        lv_obj_add_flag(key, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    }
    return b;
}

static const char *_choice_name(lv_obj_t *row)
{
    lv_obj_t *key = lv_obj_get_child(row, 2);
    return key ? lv_label_get_text(key) : NULL;
}

static void _layout_pick_cb(lv_event_t *e)
{
    const char *name = _choice_name(lv_event_get_current_target(e));
    if (!name) return;
    char active[LAYOUT_MAX_NAME] = "";
    layout_manager_get_active(active, sizeof(active));
    if (strcmp(active, name) == 0) {          /* already driving it: just go back */
        main_menu_close_cb(e);
        return;
    }
    s_return = NULL;
    s_dirty = false;
    ui_Screen3_switch_layout(name);
}

static lv_obj_t *s_splash_list = NULL;
static void _build_splash_rows(void);
static void _rebuild_splash_async(void *arg) { (void)arg; _build_splash_rows(); }

static void _splash_pick_cb(lv_event_t *e)
{
    const char *name = _choice_name(lv_event_get_current_target(e));
    if (!name || strcmp(name, "\x01off") == 0) {
        config_store_save_splash_enabled(false);
        uk_toast("Start-up screen off", UK_TONE_TEXT);
    } else {
        config_store_save_splash_enabled(true);
        layout_manager_set_active_splash(name);
        uk_toast("Start-up screen changed", UK_TONE_TEXT);
    }
    /* The tapped row is inside the list being rebuilt: never clean it from
     * inside its own click event. */
    lv_async_call(_rebuild_splash_async, NULL);
}

static void _build_splash_rows(void)
{
    if (!s_splash_list || !lv_obj_is_valid(s_splash_list)) return;
    lv_obj_clean(s_splash_list);
    char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
    int n = layout_manager_list_splash(names, LAYOUT_MAX_COUNT);
    char active[LAYOUT_MAX_NAME] = "";
    layout_manager_get_active_splash(active, sizeof(active));
    bool on = true;
    config_store_load_splash_enabled(&on);
    bool matched = false;
    for (int i = 0; i < n; i++) {
        bool sel = on && strcmp(names[i], active) == 0;
        matched |= sel;
        _choice(s_splash_list, names[i], sel, _splash_pick_cb, names[i]);
    }
    /* Enabled but the saved name is gone: the first one is what boots. */
    if (on && !matched && n > 0) {
        lv_obj_t *first = lv_obj_get_child(s_splash_list, 0);
        uk_btn_set_kind(first, UK_BTN_ON);
    }
    lv_obj_t *off = _choice(s_splash_list, "None", !on, _splash_pick_cb, "\x01off");
    lv_label_set_text(uk_btn_label(off), "NONE - STRAIGHT TO THE DASH");
}

static void _layouts_deleted_cb(lv_event_t *e) { (void)e; s_splash_list = NULL; }

static void _open_layouts_page(void)
{
    lv_obj_t *scr = uk_screen();
    lv_obj_add_event_cb(scr, _layouts_deleted_cb, LV_EVENT_DELETE, NULL);
    uk_bar(scr, "Layouts", UK_BAR_BACK, main_menu_back_cb, NULL);
    lv_obj_t *grid = uk_grid(uk_body(scr), 2, 1);

    lv_obj_t *left = uk_card(grid);
    uk_grid_place(left, 0, 0, 1, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    uk_section(left, "Drive with");
    lv_obj_t *list = uk_scroll(left);
    lv_obj_set_height(list, 0);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_pad_row(list, 8, 0);

    char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
    int n = layout_manager_list(names, LAYOUT_MAX_COUNT);
    char active[LAYOUT_MAX_NAME] = "";
    layout_manager_get_active(active, sizeof(active));
    for (int i = 0; i < n; i++) {
        if (names[i][0] == '_') continue;
        _choice(list, names[i], strcmp(names[i], active) == 0, _layout_pick_cb, names[i]);
    }

    lv_obj_t *right = uk_card(grid);
    uk_grid_place(right, 1, 0, 1, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    uk_section(right, "Shown at start-up");
    s_splash_list = uk_scroll(right);
    lv_obj_set_height(s_splash_list, 0);
    lv_obj_set_flex_grow(s_splash_list, 1);
    lv_obj_set_style_pad_row(s_splash_list, 8, 0);
    _build_splash_rows();

    main_menu_swap_to(scr);
}
