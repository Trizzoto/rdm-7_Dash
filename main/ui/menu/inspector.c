/*
 * inspector.c — see inspector.h.
 *
 * Screen layout (800×480):
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  ←  Panel · slot 0                                        ⋯  │  48
 *   ├──────────────────────────────────────────────────────────────┤
 *   │   DATA    STYLE    LAYOUT    RULES                           │  40
 *   ├──────────────────────────────────────────────────────────────┤
 *   │                                                              │
 *   │   <scrollable tab content>                                   │  392
 *   │                                                              │
 *   └──────────────────────────────────────────────────────────────┘
 */
#include "ui/menu/inspector.h"
#include "ui/menu/design_tokens.h"
#include "ui/menu/edit_mode.h"     /* edit_mode_commit_external_edit */
#include "ui/menu/menu_screen.h"   /* load_menu_screen_for_widget */
#include "ui/theme.h"
#include "ui/ui.h"                 /* ui_Screen3 extern */
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "inspector";

/* ── Module state ─────────────────────────────────────────────────────── */

#define TAB_COUNT 4
enum { TAB_DATA = 0, TAB_STYLE, TAB_LAYOUT, TAB_RULES };
static const char *const s_tab_names[TAB_COUNT] = {
    "DATA", "STYLE", "LAYOUT", "RULES"
};

static lv_obj_t *s_screen          = NULL;
static lv_obj_t *s_tab_buttons[TAB_COUNT] = {NULL};
static lv_obj_t *s_tab_indicator   = NULL;   /* underline under active tab */
static lv_obj_t *s_content         = NULL;
static widget_t *s_widget          = NULL;
static int       s_active_tab      = TAB_LAYOUT;   /* default-open */

/* LAYOUT-tab controls (recreated each time the tab is shown) */
static lv_obj_t *s_x_slider = NULL, *s_x_value = NULL;
static lv_obj_t *s_y_slider = NULL, *s_y_value = NULL;
static lv_obj_t *s_w_slider = NULL, *s_w_value = NULL;
static lv_obj_t *s_h_slider = NULL, *s_h_value = NULL;
static bool      s_layout_syncing = false;   /* re-entry guard */

/* ── Helpers ──────────────────────────────────────────────────────────── */

static const char *_widget_type_name(widget_type_t t) {
    switch (t) {
        case WIDGET_PANEL:       return "Panel";
        case WIDGET_RPM_BAR:     return "RPM Bar";
        case WIDGET_BAR:         return "Bar";
        case WIDGET_INDICATOR:   return "Indicator";
        case WIDGET_WARNING:     return "Alert";
        case WIDGET_TEXT:        return "Text";
        case WIDGET_METER:       return "Meter";
        case WIDGET_IMAGE:       return "Image";
        case WIDGET_SHAPE_PANEL: return "Shape";
        case WIDGET_ARC:         return "Arc";
        case WIDGET_TOGGLE:      return "Toggle";
        case WIDGET_BUTTON:      return "Button";
        case WIDGET_SHIFT_LIGHT: return "Shift Light";
        case WIDGET_LINE:        return "Line";
        default:                 return "Widget";
    }
}

/* Translucent-white button — same look as the dashboard toolbars'
 * _make_tbtn, copied locally so this module stays self-contained. */
static lv_obj_t *_make_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                              const char *text) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, 28, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, 70, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_opa(b, 55, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, DT_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, THEME_FONT_SMALL, 0);
    return b;
}

/* Card container — used for grouping related fields in a tab. */
static lv_obj_t *_make_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (title) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, DT_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(t, THEME_FONT_SMALL, 0);
    }
    return card;
}

/* ── Tab indicator placement ──────────────────────────────────────────── */

static void _position_tab_indicator(void) {
    if (!s_tab_indicator || !lv_obj_is_valid(s_tab_indicator)) return;
    if (s_active_tab < 0 || s_active_tab >= TAB_COUNT) return;
    /* Each tab button is 200 wide; the indicator hugs its bottom edge. */
    lv_obj_set_pos(s_tab_indicator, (lv_coord_t)(s_active_tab * 200), 38);
}

/* ── LAYOUT tab ───────────────────────────────────────────────────────── */

static void _apply_layout_value(char target, int v) {
    if (!s_widget || !s_widget->root || !lv_obj_is_valid(s_widget->root)) return;

    switch (target) {
        case 'x':
            s_widget->x = (int16_t)v;
            lv_obj_set_pos(s_widget->root, s_widget->x, s_widget->y);
            break;
        case 'y':
            s_widget->y = (int16_t)v;
            lv_obj_set_pos(s_widget->root, s_widget->x, s_widget->y);
            break;
        case 'w':
            if (v < 10)  v = 10;
            if (v > 800) v = 800;
            if (s_widget->resize) {
                s_widget->resize(s_widget, (uint16_t)v, s_widget->h);
            } else {
                s_widget->w = (uint16_t)v;
                lv_obj_set_size(s_widget->root, v, s_widget->h);
            }
            break;
        case 'h':
            if (v < 10)  v = 10;
            if (v > 480) v = 480;
            if (s_widget->resize) {
                s_widget->resize(s_widget, s_widget->w, (uint16_t)v);
            } else {
                s_widget->h = (uint16_t)v;
                lv_obj_set_size(s_widget->root, s_widget->w, v);
            }
            break;
        default:
            return;
    }
}

static void _slider_event_cb(lv_event_t *e) {
    if (s_layout_syncing) return;
    if (!s_widget) return;
    lv_obj_t *sl = lv_event_get_target(e);
    char target = (char)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(sl);
    _apply_layout_value(target, v);
    /* Update the matching value label */
    lv_obj_t *val_lbl = NULL;
    switch (target) {
        case 'x': val_lbl = s_x_value; break;
        case 'y': val_lbl = s_y_value; break;
        case 'w': val_lbl = s_w_value; break;
        case 'h': val_lbl = s_h_value; break;
    }
    if (val_lbl && lv_obj_is_valid(val_lbl))
        lv_label_set_text_fmt(val_lbl, "%d", v);
}

/* Single layout row: label + slider + value display */
static void _make_layout_row(lv_obj_t *parent, const char *name, char target,
                             int v, int vmin, int vmax,
                             lv_obj_t **out_slider, lv_obj_t **out_value) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 32);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_width(name_lbl, 48);
    lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(name_lbl, THEME_FONT_MEDIUM, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 480, 12);
    lv_slider_set_range(slider, vmin, vmax);
    lv_slider_set_value(slider, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, DT_BG_INSET, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, DT_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, DT_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, _slider_event_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)target);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text_fmt(value, "%d", v);
    lv_obj_set_width(value, 60);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    if (out_slider) *out_slider = slider;
    if (out_value)  *out_value  = value;
}

static void _build_layout_tab(void) {
    if (!s_widget) return;

    /* Position card */
    lv_obj_t *pos_card = _make_card(s_content, "POSITION");
    _make_layout_row(pos_card, "X", 'x', s_widget->x, -400, 400,
                     &s_x_slider, &s_x_value);
    _make_layout_row(pos_card, "Y", 'y', s_widget->y, -240, 240,
                     &s_y_slider, &s_y_value);

    /* Size card */
    lv_obj_t *sz_card = _make_card(s_content, "SIZE");
    _make_layout_row(sz_card, "W", 'w', s_widget->w,  10, 800,
                     &s_w_slider, &s_w_value);
    _make_layout_row(sz_card, "H", 'h', s_widget->h,  10, 480,
                     &s_h_slider, &s_h_value);
}

/* ── Placeholder tabs ─────────────────────────────────────────────────── */

static void _legacy_btn_cb(lv_event_t *e) {
    (void)e;
    widget_t *w = s_widget;
    inspector_close();
    if (w) load_menu_screen_for_widget(w);
}

static void _build_placeholder_tab(const char *tab_name) {
    lv_obj_t *card = _make_card(s_content, NULL);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 32, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "%s coming soon", tab_name);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg,
        "Use the legacy editor for the full settings list,\n"
        "or the web editor for the complete UI.");
    lv_obj_set_style_text_color(msg, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(msg, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *legacy_btn = _make_button(card, 220, 44, "Open legacy editor");
    lv_obj_add_event_cb(legacy_btn, _legacy_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Tab switching ────────────────────────────────────────────────────── */

static void _refresh_tab_button_styles(void) {
    for (int i = 0; i < TAB_COUNT; i++) {
        if (!s_tab_buttons[i] || !lv_obj_is_valid(s_tab_buttons[i])) continue;
        bool active = (i == s_active_tab);
        lv_obj_t *lbl = lv_obj_get_child(s_tab_buttons[i], 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                active ? lv_color_white() : DT_TEXT_MUTED, 0);
        }
    }
    _position_tab_indicator();
}

static void _show_tab(int idx) {
    if (!s_content || !lv_obj_is_valid(s_content)) return;
    s_active_tab = idx;
    /* Drop any existing tab content + the per-tab state pointers */
    lv_obj_clean(s_content);
    s_x_slider = s_x_value = s_y_slider = s_y_value = NULL;
    s_w_slider = s_w_value = s_h_slider = s_h_value = NULL;

    switch (idx) {
        case TAB_LAYOUT: _build_layout_tab();             break;
        case TAB_DATA:   _build_placeholder_tab("Data");  break;
        case TAB_STYLE:  _build_placeholder_tab("Style"); break;
        case TAB_RULES:  _build_placeholder_tab("Rules"); break;
        default:         _build_placeholder_tab("Tab");   break;
    }
    _refresh_tab_button_styles();
}

static void _tab_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _show_tab(idx);
}

/* ── Shell ────────────────────────────────────────────────────────────── */

static void _back_btn_cb(lv_event_t *e) {
    (void)e;
    inspector_close();
}

static void _build_header(void) {
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_set_size(header, 800, 48);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    /* Back chevron */
    lv_obj_t *back = lv_btn_create(header);
    lv_obj_set_size(back, 48, 48);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(back, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, 40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_radius(back, 0, 0);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);
    lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_lbl, THEME_FONT_LARGE, 0);
    lv_obj_add_event_cb(back, _back_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Title — widget type + slot. Plain ASCII separator only (Montserrat
     * doesn't ship the middle-dot glyph). */
    lv_obj_t *title = lv_label_create(header);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s  -  slot %u",
             _widget_type_name(s_widget ? s_widget->type : (widget_type_t)0),
             (unsigned)(s_widget ? s_widget->slot : 0));
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 60, 0);
}

static void _build_tab_bar(void) {
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_set_size(bar, 800, 40);
    lv_obj_set_pos(bar, 0, 48);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    /* Each tab is a transparent clickable area 200 wide */
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *b = lv_btn_create(bar);
        lv_obj_set_size(b, 200, 40);
        lv_obj_set_pos(b, i * 200, 0);
        lv_obj_set_style_bg_opa(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, 20, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, s_tab_names[i]);
        lv_obj_center(lbl);
        lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
        lv_obj_add_event_cb(b, _tab_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_tab_buttons[i] = b;
    }

    /* Active-tab underline indicator (2 px, accent) */
    s_tab_indicator = lv_obj_create(bar);
    lv_obj_set_size(s_tab_indicator, 200, 2);
    lv_obj_set_style_bg_color(s_tab_indicator, DT_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_tab_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tab_indicator, 0, 0);
    lv_obj_set_style_radius(s_tab_indicator, 0, 0);
    lv_obj_clear_flag(s_tab_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_tab_indicator, LV_OBJ_FLAG_SCROLLABLE);
    _position_tab_indicator();
}

static void _build_content_area(void) {
    s_content = lv_obj_create(s_screen);
    lv_obj_set_size(s_content, 800, 480 - 48 - 40);
    lv_obj_set_pos(s_content, 0, 48 + 40);
    lv_obj_set_style_bg_color(s_content, DT_BG_BASE, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 16, 0);
    lv_obj_set_style_pad_row(s_content, 12, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void inspector_open(widget_t *w) {
    if (!w) return;
    if (s_screen && lv_obj_is_valid(s_screen)) return;

    s_widget     = w;
    s_active_tab = TAB_LAYOUT;

    s_screen = lv_obj_create(NULL);   /* fresh screen */
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, DT_BG_BASE, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    _build_header();
    _build_tab_bar();
    _build_content_area();
    _show_tab(s_active_tab);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "Opened for %s slot %u",
             _widget_type_name(w->type), (unsigned)w->slot);
}

void inspector_close(void) {
    if (!s_screen) return;
    /* Switch back to the dashboard first, then delete the inspector screen
     * — lv_obj_del on the active screen is allowed but generates a brief
     * flash of nothing. */
    lv_obj_t *old = s_screen;
    s_screen          = NULL;
    s_widget          = NULL;
    s_content         = NULL;
    s_tab_indicator   = NULL;
    for (int i = 0; i < TAB_COUNT; i++) s_tab_buttons[i] = NULL;
    s_x_slider = s_x_value = s_y_slider = s_y_value = NULL;
    s_w_slider = s_w_value = s_h_slider = s_h_value = NULL;
    s_layout_syncing = false;

    if (ui_Screen3 && lv_obj_is_valid(ui_Screen3))
        lv_scr_load(ui_Screen3);
    if (lv_obj_is_valid(old))
        lv_obj_del(old);

    /* Tell edit_mode to repaint the selection ring at the widget's new
     * bounds, push an undo snapshot for the cumulative Inspector edit,
     * and schedule a save. Same-state check inside _undo_snapshot makes
     * close-without-edits a free no-op. */
    edit_mode_commit_external_edit();

    ESP_LOGI(TAG, "Closed");
}

bool inspector_is_open(void) {
    return s_screen != NULL && lv_obj_is_valid(s_screen);
}
