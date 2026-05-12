/*
 * inspector.c — see inspector.h.
 *
 * Overlay layout (800×480, sits on top of ui_Screen3):
 *
 *   +-----------------------------------------------------------+
 *   |  <-  Panel - slot 0                                       |   48 header (opaque)
 *   +-----------------------------------------------------------+
 *   |    DATA           STYLE           RULES                   |   40 tab bar (opaque)
 *   +-----------------------------------------------------------+
 *   |                                                           |
 *   |   <scrollable tab content>                                |   392 (transparent
 *   |   cards are opaque; gaps show the dashboard through       |        background)
 *   |   a dimmed backdrop so live edits are visible.            |
 *   |                                                           |
 *   +-----------------------------------------------------------+
 */
#include "ui/menu/inspector.h"
#include "ui/menu/design_tokens.h"
#include "ui/menu/edit_mode.h"     /* edit_mode_commit_external_edit */
#include "ui/menu/menu_screen.h"   /* load_menu_screen_for_widget */
#include "ui/screens/ui_Screen3.h" /* ui_Screen3_refresh_overlays */
#include "ui/theme.h"
#include "ui/ui.h"                 /* ui_Screen3 extern */
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "inspector";

/* ── Tab definitions ──────────────────────────────────────────────────── */
/* LAYOUT was removed — the bottom toolbar's chip popover owns position +
 * size, the Inspector doesn't duplicate it. */

#define TAB_COUNT 3
enum { TAB_DATA = 0, TAB_STYLE, TAB_RULES };
static const char *const s_tab_names[TAB_COUNT] = {
    "DATA", "STYLE", "RULES"
};
#define TAB_WIDTH (800 / TAB_COUNT)   /* even split across the bar */

/* ── Module state ─────────────────────────────────────────────────────── */

static lv_obj_t *s_overlay         = NULL;   /* root, full-screen translucent */
static lv_obj_t *s_tab_buttons[TAB_COUNT] = {NULL};
static lv_obj_t *s_tab_indicator   = NULL;   /* accent underline */
static lv_obj_t *s_content         = NULL;   /* scrollable, transparent bg */
static widget_t *s_widget          = NULL;
static int       s_active_tab      = TAB_STYLE;   /* most-used tab default */

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

/* Translucent-white button — mirrors the dashboard toolbars' _make_tbtn so
 * the visual language stays consistent between the toolbars and Inspector. */
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

/* Card container for grouping fields. Opaque so the dashboard underneath
 * doesn't fight with the field content for legibility. Gaps between cards
 * (and below them, since the content area is column-flex with pad_row) leak
 * the dashboard through. */
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
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (title) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, DT_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(t, THEME_FONT_SMALL, 0);
    }
    return card;
}

static void _position_tab_indicator(void) {
    if (!s_tab_indicator || !lv_obj_is_valid(s_tab_indicator)) return;
    if (s_active_tab < 0 || s_active_tab >= TAB_COUNT) return;
    lv_obj_set_pos(s_tab_indicator,
                   (lv_coord_t)(s_active_tab * TAB_WIDTH), 38);
}

/* ── Placeholder tab (DATA / STYLE / RULES, all same shape for now) ──── */

static void _legacy_btn_cb(lv_event_t *e) {
    (void)e;
    widget_t *w = s_widget;
    inspector_close();
    if (w) load_menu_screen_for_widget(w);
}

static void _build_placeholder_tab(const char *tab_name) {
    lv_obj_t *card = _make_card(s_content, NULL);
    lv_obj_set_style_pad_all(card, 32, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "%s coming soon", tab_name);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg,
        "Position + size live in the bottom toolbar.\n"
        "For the full per-widget settings list, the legacy editor\n"
        "is still available below; the web editor remains the most\n"
        "complete option.");
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
    if (idx < 0 || idx >= TAB_COUNT) return;
    s_active_tab = idx;
    lv_obj_clean(s_content);
    switch (idx) {
        case TAB_DATA:  _build_placeholder_tab("Data");  break;
        case TAB_STYLE: _build_placeholder_tab("Style"); break;
        case TAB_RULES: _build_placeholder_tab("Rules"); break;
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
    lv_obj_t *header = lv_obj_create(s_overlay);
    lv_obj_set_size(header, 800, 48);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, DT_BG_PANEL, 0);
    /* Mostly opaque so the title + back button are crisp against whatever's
     * underneath. The dimmed backdrop handles the rest. */
    lv_obj_set_style_bg_opa(header, LV_OPA_90, 0);
    lv_obj_set_style_border_color(header, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

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
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_set_size(bar, 800, 40);
    lv_obj_set_pos(bar, 0, 48);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_border_color(bar, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *b = lv_btn_create(bar);
        lv_obj_set_size(b, TAB_WIDTH, 40);
        lv_obj_set_pos(b, i * TAB_WIDTH, 0);
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

    s_tab_indicator = lv_obj_create(bar);
    lv_obj_set_size(s_tab_indicator, TAB_WIDTH, 2);
    lv_obj_set_style_bg_color(s_tab_indicator, DT_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_tab_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tab_indicator, 0, 0);
    lv_obj_set_style_radius(s_tab_indicator, 0, 0);
    lv_obj_clear_flag(s_tab_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_tab_indicator, LV_OBJ_FLAG_SCROLLABLE);
    _position_tab_indicator();
}

static void _build_content_area(void) {
    s_content = lv_obj_create(s_overlay);
    lv_obj_set_size(s_content, 800, 480 - 48 - 40);
    lv_obj_set_pos(s_content, 0, 48 + 40);
    /* Transparent — the overlay's dimmed backdrop shows through, and through
     * THAT the dashboard widget being edited is visible. Cards are opaque
     * so the field content reads clearly. */
    lv_obj_set_style_bg_opa(s_content, 0, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 16, 0);
    lv_obj_set_style_pad_row(s_content, 12, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void inspector_open(widget_t *w) {
    if (!w) return;
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;

    s_widget     = w;
    s_active_tab = TAB_STYLE;

    /* Build on ui_Screen3, NOT a new screen — the dashboard stays alive
     * underneath so the user sees their edits land in real time through
     * the translucent backdrop. */
    lv_obj_t *parent = ui_Screen3;
    if (!parent || !lv_obj_is_valid(parent)) parent = lv_scr_act();
    if (!parent) return;

    s_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_overlay, 800, 480);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Dim the dashboard so the Inspector chrome reads clearly, without
     * fully hiding the widget being adjusted. */
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    /* Capture pointer events so taps on the dashboard widgets behind don't
     * trigger drags or selects while the Inspector is up. */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    _build_header();
    _build_tab_bar();
    _build_content_area();
    _show_tab(s_active_tab);

    /* On top of widgets + edit-mode chrome. The BUS SILENT badge gets
     * re-foregrounded so a CAN-silent warning is never buried under the
     * Inspector. */
    lv_obj_move_foreground(s_overlay);
    ui_Screen3_refresh_overlays();

    ESP_LOGI(TAG, "Opened for %s slot %u",
             _widget_type_name(w->type), (unsigned)w->slot);
}

void inspector_close(void) {
    if (!s_overlay) return;
    if (lv_obj_is_valid(s_overlay)) lv_obj_del(s_overlay);
    s_overlay         = NULL;
    s_widget          = NULL;
    s_content         = NULL;
    s_tab_indicator   = NULL;
    for (int i = 0; i < TAB_COUNT; i++) s_tab_buttons[i] = NULL;

    /* If anything was tweaked through the Inspector, capture it as an undo
     * step + schedule save. Same-state guard means a no-op when nothing
     * actually changed. */
    edit_mode_commit_external_edit();

    ESP_LOGI(TAG, "Closed");
}

bool inspector_is_open(void) {
    return s_overlay != NULL && lv_obj_is_valid(s_overlay);
}
