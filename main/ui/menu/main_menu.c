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
#include "layout_thumbs.h"
#include "menu/menu_screen.h"
#include "settings/device_settings.h"
#include "screens/ui_Screen3.h"
#include "screens/ui_graphs.h"
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

static void _t_live(lv_event_t *e)      { (void)e; graphs_ui_show(); }

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
    uk_tile_set_status(t, NULL, "Graphs, min and max", UK_TONE_MUTED);

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

/* Names behind the cards, by card index. Valid while the page exists. */
static char s_card_names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
static int  s_card_count = 0;

static void _layout_pick_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_card_count) return;
    const char *name = s_card_names[idx];
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

/* Start-up screen: "None" first, then every splash layout. */
static char s_splash_names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];

static void _splash_dd_cb(lv_event_t *e)
{
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel == 0) {
        config_store_save_splash_enabled(false);
        uk_toast("Start-up screen off: straight to the dash", UK_TONE_TEXT);
    } else {
        config_store_save_splash_enabled(true);
        layout_manager_set_active_splash(s_splash_names[sel - 1]);
        uk_toast("Start-up screen changed", UK_TONE_TEXT);
    }
}

static lv_obj_t *_splash_dropdown(lv_obj_t *parent)
{
    int n = layout_manager_list_splash(s_splash_names, LAYOUT_MAX_COUNT);
    char active[LAYOUT_MAX_NAME] = "";
    layout_manager_get_active_splash(active, sizeof(active));
    bool on = true;
    config_store_load_splash_enabled(&on);

    char opts[640] = "None";
    size_t pos = strlen(opts);
    int sel = 0;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(s_splash_names[i]);
        if (pos + len + 2 > sizeof(opts)) break;
        opts[pos++] = '\n';
        memcpy(opts + pos, s_splash_names[i], len + 1);
        pos += len;
        if (on && strcmp(s_splash_names[i], active) == 0) sel = i + 1;
    }
    /* Enabled but the saved name is gone: the first one is what boots. */
    if (on && sel == 0 && n > 0) sel = 1;

    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, sel);
    uk_style_dropdown(dd);
    lv_obj_set_size(dd, 190, 36);
    lv_obj_add_event_cb(dd, _splash_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

/* One layout as a card: its picture, its name, and whether it is in use. */
static void _layout_card(lv_obj_t *parent, int idx, bool in_use)
{
    const char *name = s_card_names[idx];

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 252, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_color(card, THEME_COLOR_SECTION_BG, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UK_R_TILE, 0);
    lv_obj_set_style_border_width(card, in_use ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, in_use ? THEME_COLOR_ACCENT : THEME_COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(card, in_use ? 11 : 12, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, _layout_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    /* The picture sits in a black frame the size of a thumbnail, so a card
     * with no picture yet keeps the same shape as one with. */
    lv_obj_t *frame = lv_obj_create(card);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, LAYOUT_THUMB_W + 2, LAYOUT_THUMB_H + 2);
    lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(frame, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    const lv_img_dsc_t *thumb = layout_thumbs_get(name);
    if (thumb) {
        lv_obj_t *img = lv_img_create(frame);
        lv_img_set_src(img, thumb);
        lv_obj_center(img);
    } else {
        lv_obj_t *ic = uk_icon(frame, UK_ICON_LAYOUTS, UK_ICON_LG, UK_TONE_HINT);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, -14);
        lv_obj_t *t = uk_label(frame, "Picture appears once it has been on screen",
                               UK_FONT_SMALL, UK_TONE_HINT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, 180);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 26);
    }

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 26);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = uk_label(row, name, UK_FONT_HEAD, UK_TONE_TEXT);
    lv_obj_set_flex_grow(nm, 1);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);

    if (in_use) {
        lv_obj_t *pill = lv_obj_create(row);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, 22);
        lv_obj_set_style_bg_color(pill, THEME_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(pill, 9, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *pl = uk_label(pill, "In use", UK_FONT_LABEL, UK_TONE_TEXT);
        lv_obj_set_style_text_color(pl, THEME_COLOR_TEXT_ON_ACCENT, 0);
        lv_obj_center(pl);
    }
}

static void _open_layouts_page(void)
{
    lv_obj_t *scr = uk_screen();
    lv_obj_t *bar = uk_bar(scr, "Layouts", UK_BAR_BACK, main_menu_back_cb, NULL);

    /* Start-up screen lives in the bar, so the whole body is pictures. */
    lv_obj_t *status = lv_obj_get_child(bar, 2);
    lv_obj_set_style_pad_column(status, 10, 0);
    uk_label(status, "At start-up", UK_FONT_LABEL, UK_TONE_MUTED);
    _splash_dropdown(status);

    lv_obj_t *list = uk_scroll(uk_body(scr));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(list, UK_GAP, 0);
    lv_obj_set_style_pad_row(list, UK_GAP, 0);

    char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
    int n = layout_manager_list(names, LAYOUT_MAX_COUNT);
    char active[LAYOUT_MAX_NAME] = "";
    layout_manager_get_active(active, sizeof(active));

    /* The one in use first, then the rest in filesystem order. */
    s_card_count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            if (names[i][0] == '_') continue;
            bool in_use = strcmp(names[i], active) == 0;
            if ((pass == 0) != in_use) continue;
            snprintf(s_card_names[s_card_count], LAYOUT_MAX_NAME, "%s", names[i]);
            _layout_card(list, s_card_count, in_use);
            s_card_count++;
        }
    }

    /* Where more come from. */
    lv_obj_t *more = lv_obj_create(list);
    lv_obj_remove_style_all(more);
    lv_obj_set_size(more, 252, LAYOUT_THUMB_H + 2 + 10 + 26 + 24);
    lv_obj_set_style_border_color(more, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_border_width(more, 1, 0);
    lv_obj_set_style_radius(more, UK_R_TILE, 0);
    lv_obj_clear_flag(more, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mi = uk_icon(more, UK_ICON_WEB, UK_ICON_LG, UK_TONE_HINT);
    lv_obj_align(mi, LV_ALIGN_CENTER, 0, -22);
    lv_obj_t *mt = uk_label(more, "More layouts from RDM Studio or the web editor",
                            UK_FONT_SMALL, UK_TONE_MUTED);
    lv_label_set_long_mode(mt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(mt, 190);
    lv_obj_set_style_text_align(mt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mt, LV_ALIGN_CENTER, 0, 24);

    main_menu_swap_to(scr);
}
