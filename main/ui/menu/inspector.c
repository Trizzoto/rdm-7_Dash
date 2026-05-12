/*
 * inspector.c - see inspector.h.
 *
 * Right-side dock layout (320x480, the dashboard stays visible at full
 * native size in the left 480 px so live edits land in real time). Mirrors
 * the web editor's Properties-panel-on-right so users transfer skill from
 * the browser editor to on-device.
 *
 *   left 480 px              right 320 px
 *  +-----------------+ +-------------------+
 *  |                 | |  <-  Panel slot 0 |   48 header
 *  |                 | +-------------------+
 *  |   dashboard     | | DATA STYLE RULES  |   40 tab bar
 *  |   (live preview | +-------------------+
 *  |   - clicks      | |                   |
 *  |   absorbed by   | |  property list    |
 *  |   click-eater)  | |  (compact rows)   |
 *  |                 | |                   |
 *  +-----------------+ +-------------------+
 *
 * Property rows are one-line: label + preview swatch + chevron. Tap a row
 * to open a modal preset-picker popover floating above everything. Sliders
 * stay inline; they're cheap enough to render in the dock.
 *
 * Cards sit at ~70% opacity so the widget being edited stays partly
 * visible behind the dock too - the user sees colour / dim changes land
 * even on the area covered by the dock.
 */
#include "ui/menu/inspector.h"
#include "ui/menu/design_tokens.h"
#include "ui/menu/edit_mode.h"     /* edit_mode_commit_external_edit */
#include "ui/menu/menu_screen.h"   /* load_menu_screen_for_widget */
#include "ui/screens/ui_Screen3.h" /* ui_Screen3_refresh_overlays + ui_Label/ui_Value */
#include "ui/theme.h"
#include "ui/ui.h"                 /* ui_Screen3 extern */
#include "widgets/widget_panel.h"  /* panel_data_t - STYLE tab pilot */
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "inspector";

/* Tab definitions. LAYOUT was removed - the bottom toolbar's chip popover
 * owns position + size, the Inspector doesn't duplicate it. */
#define TAB_COUNT 3
enum { TAB_DATA = 0, TAB_STYLE, TAB_RULES };
static const char *const s_tab_names[TAB_COUNT] = {
    "DATA", "STYLE", "RULES"
};

/* Right-side dock geometry. Keeps the left 480 px untouched so live edits
 * show up at native size, matching the web editor's right-Properties
 * layout. */
#define DOCK_W    320
#define DOCK_X    (800 - DOCK_W)        /* 480 */
#define TAB_WIDTH (DOCK_W / TAB_COUNT)  /* even split across the bar */

/* Module state */

static lv_obj_t *s_overlay         = NULL;   /* right-side dock root */
static lv_obj_t *s_left_eater      = NULL;   /* invisible click-eater over
                                              * the dashboard area so
                                              * edit_mode drag / selection
                                              * can't fire underneath */
static lv_obj_t *s_tab_buttons[TAB_COUNT] = {NULL};
static lv_obj_t *s_tab_indicator   = NULL;
static lv_obj_t *s_content         = NULL;
static widget_t *s_widget          = NULL;
static int       s_active_tab      = TAB_STYLE;

/* Helpers */

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

/* Card container. Translucent so the dashboard widget being edited stays
 * partly visible behind the dock. */
static lv_obj_t *_make_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_border_color(card, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if (title) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, DT_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(t, THEME_FONT_SMALL, 0);
        lv_obj_set_style_pad_bottom(t, 4, 0);
    }
    return card;
}

static void _position_tab_indicator(void) {
    if (!s_tab_indicator || !lv_obj_is_valid(s_tab_indicator)) return;
    if (s_active_tab < 0 || s_active_tab >= TAB_COUNT) return;
    lv_obj_set_pos(s_tab_indicator,
                   (lv_coord_t)(s_active_tab * TAB_WIDTH), 38);
}

/* ════════════════════════════════════════════════════════════════════════
 *  STYLE tab - per-widget appearance fields
 * ════════════════════════════════════════════════════════════════════════
 *
 * Pilot widget: Panel. Other types fall through to the placeholder.
 *
 * Live preview: each change writes BOTH the widget's type_data field
 * (persisted on Inspector close via edit_mode_commit_external_edit) AND
 * the corresponding LVGL style attribute on the widget's existing object
 * (so the user sees it land in real time through the translucent dock).
 *
 * Row format mirrors the web editor's Properties panel:
 *
 *     Border               [#]  >
 *     Background           [#]  >
 *
 * Tap a row to open the preset picker. Picker floats over everything.
 */

#define PRESET_COUNT 10
static lv_color_t s_presets[PRESET_COUNT];
static bool       s_presets_ready = false;

static void _ensure_presets(void) {
    if (s_presets_ready) return;
    s_presets[0] = lv_color_white();
    s_presets[1] = THEME_COLOR_RED;
    s_presets[2] = THEME_COLOR_ORANGE;
    s_presets[3] = THEME_COLOR_YELLOW;
    s_presets[4] = THEME_COLOR_GREEN;
    s_presets[5] = THEME_COLOR_CYAN;
    s_presets[6] = THEME_COLOR_BLUE;
    s_presets[7] = THEME_COLOR_PURPLE;
    s_presets[8] = THEME_COLOR_PINK;
    s_presets[9] = lv_color_black();
    s_presets_ready = true;
}

enum panel_field {
    F_PANEL_BORDER = 0,
    F_PANEL_BG,
    F_PANEL_LABEL,
    F_PANEL_VALUE,
    F_PANEL_FIELD_COUNT
};
enum panel_dim {
    D_PANEL_BORDER_W = 0,
    D_PANEL_BORDER_R,
    D_PANEL_DIM_COUNT
};

static const char *const s_panel_field_names[F_PANEL_FIELD_COUNT] = {
    "Border", "Background", "Label", "Value"
};

/* One small preview swatch per colour row. */
static lv_obj_t *s_panel_swatch_preview[F_PANEL_FIELD_COUNT] = {NULL};
static lv_obj_t *s_panel_dim_value_lbls[D_PANEL_DIM_COUNT]    = {NULL};

/* Picker popover state. Opened on tap of a colour row, dismissed on tap
 * outside the inner card or on swatch select. */
static lv_obj_t *s_picker                            = NULL;
static int       s_picker_field                      = -1;
static lv_obj_t *s_picker_swatches[PRESET_COUNT]     = {NULL};

static bool _color_eq(lv_color_t a, lv_color_t b) {
    return a.full == b.full;
}

static lv_color_t _panel_get_color(panel_data_t *pd, int field) {
    switch (field) {
        case F_PANEL_BORDER: return pd->border_color;
        case F_PANEL_BG:     return pd->bg_color;
        case F_PANEL_LABEL:  return pd->label_color;
        case F_PANEL_VALUE:  return pd->value_color;
    }
    return lv_color_black();
}

/* Refresh the row's preview swatch AND, if the picker is up on the same
 * field, the active highlight in the picker grid. */
static void _refresh_swatch_active(int field) {
    if (!s_widget || s_widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)s_widget->type_data;
    if (!pd) return;
    lv_color_t current = _panel_get_color(pd, field);

    lv_obj_t *prev = s_panel_swatch_preview[field];
    if (prev && lv_obj_is_valid(prev)) {
        lv_obj_set_style_bg_color(prev, current, 0);
    }

    if (s_picker_field == field) {
        for (int i = 0; i < PRESET_COUNT; i++) {
            lv_obj_t *sw = s_picker_swatches[i];
            if (!sw || !lv_obj_is_valid(sw)) continue;
            bool active = _color_eq(s_presets[i], current);
            lv_obj_set_style_border_color(sw,
                active ? DT_ACCENT : lv_color_white(), 0);
            lv_obj_set_style_border_width(sw, active ? 3 : 1, 0);
            lv_obj_set_style_border_opa(sw, active ? LV_OPA_COVER : 60, 0);
        }
    }
}

/* Apply a colour to the live LVGL objects (so the change is visible
 * immediately) AND to the widget's type_data (so it persists on save). */
static void _panel_apply_color(int field, lv_color_t c) {
    if (!s_widget || s_widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)s_widget->type_data;
    if (!pd) return;

    lv_obj_t *lbl_obj = NULL, *val_obj = NULL;
    if (s_widget->slot < 13) {
        lbl_obj = ui_Label[s_widget->slot];
        val_obj = ui_Value[s_widget->slot];
    }

    switch (field) {
        case F_PANEL_BORDER:
            pd->border_color = c;
            if (s_widget->root && lv_obj_is_valid(s_widget->root))
                lv_obj_set_style_border_color(s_widget->root, c, 0);
            break;
        case F_PANEL_BG:
            pd->bg_color = c;
            if (s_widget->root && lv_obj_is_valid(s_widget->root))
                lv_obj_set_style_bg_color(s_widget->root, c, 0);
            break;
        case F_PANEL_LABEL:
            pd->label_color = c;
            if (lbl_obj && lv_obj_is_valid(lbl_obj))
                lv_obj_set_style_text_color(lbl_obj, c, 0);
            break;
        case F_PANEL_VALUE:
            pd->value_color = c;
            if (val_obj && lv_obj_is_valid(val_obj))
                lv_obj_set_style_text_color(val_obj, c, 0);
            break;
    }
    _refresh_swatch_active(field);
}

/* Picker popover */

static void _close_picker(void) {
    if (s_picker && lv_obj_is_valid(s_picker)) {
        lv_obj_del(s_picker);
    }
    s_picker = NULL;
    s_picker_field = -1;
    for (int i = 0; i < PRESET_COUNT; i++) s_picker_swatches[i] = NULL;
}

static void _picker_swatch_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= PRESET_COUNT) return;
    if (s_picker_field < 0) return;
    _panel_apply_color(s_picker_field, s_presets[idx]);
    _close_picker();
}

/* Tap on the backdrop (but not the inner card) closes the picker. */
static void _picker_backdrop_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    _close_picker();
}

static void _open_picker(int field) {
    if (!s_widget || s_widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)s_widget->type_data;
    if (!pd) return;
    _close_picker();
    s_picker_field = field;

    /* lv_layer_top sits above the inspector dock and the dashboard.
     * Backdrop is semi-dim so the picker reads as modal. */
    s_picker = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_picker, 800, 480);
    lv_obj_set_pos(s_picker, 0, 0);
    lv_obj_set_style_bg_color(s_picker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_picker, 110, 0);
    lv_obj_set_style_border_width(s_picker, 0, 0);
    lv_obj_set_style_radius(s_picker, 0, 0);
    lv_obj_set_style_pad_all(s_picker, 0, 0);
    lv_obj_clear_flag(s_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_picker, _picker_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_picker);
    lv_obj_set_size(card, 300, 200);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    int safe_field = (field >= 0 && field < F_PANEL_FIELD_COUNT) ? field : 0;
    lv_label_set_text_fmt(title, "%s colour", s_panel_field_names[safe_field]);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_SMALL, 0);

    /* 5x2 swatch grid, 40 px swatches + 8 px gaps */
    lv_color_t current = _panel_get_color(pd, field);
    const int grid_x0 = 8;
    const int grid_y0 = 36;
    const int cell    = 48;
    for (int i = 0; i < PRESET_COUNT; i++) {
        int col = i % 5;
        int row = i / 5;
        lv_obj_t *sw = lv_btn_create(card);
        lv_obj_set_size(sw, 40, 40);
        lv_obj_set_pos(sw, grid_x0 + col * cell, grid_y0 + row * cell);
        lv_obj_set_style_bg_color(sw, s_presets[i], 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 4, 0);
        bool active = _color_eq(s_presets[i], current);
        lv_obj_set_style_border_color(sw,
            active ? DT_ACCENT : lv_color_white(), 0);
        lv_obj_set_style_border_width(sw, active ? 3 : 1, 0);
        lv_obj_set_style_border_opa(sw, active ? LV_OPA_COVER : 60, 0);
        lv_obj_set_style_shadow_width(sw, 0, 0);
        lv_obj_set_style_pad_all(sw, 0, 0);
        lv_obj_add_event_cb(sw, _picker_swatch_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_picker_swatches[i] = sw;
    }
}

/* Compact colour row: label + preview swatch + chevron. */

static void _color_row_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int field = (int)(intptr_t)lv_event_get_user_data(e);
    _open_picker(field);
}

static void _make_color_row(lv_obj_t *parent, const char *name, int field) {
    if (!s_widget || s_widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)s_widget->type_data;
    if (!pd) return;

    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 40);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, 20, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_add_event_cb(row, _color_row_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)field);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_color(chev, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(chev, THEME_FONT_SMALL, 0);

    lv_obj_t *sw = lv_obj_create(row);
    lv_obj_set_size(sw, 28, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -22, 0);
    lv_obj_set_style_bg_color(sw, _panel_get_color(pd, field), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sw, lv_color_white(), 0);
    lv_obj_set_style_border_opa(sw, 80, 0);
    lv_obj_set_style_border_width(sw, 1, 0);
    lv_obj_set_style_radius(sw, 4, 0);
    lv_obj_set_style_pad_all(sw, 0, 0);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    s_panel_swatch_preview[field] = sw;
}

/* Dimension sliders (border width, border radius) - narrow column variant */

static void _panel_apply_dim(int dim, int v) {
    if (!s_widget || s_widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)s_widget->type_data;
    if (!pd) return;
    if (!s_widget->root || !lv_obj_is_valid(s_widget->root)) return;

    switch (dim) {
        case D_PANEL_BORDER_W:
            if (v < 0)  v = 0;
            if (v > 10) v = 10;
            pd->border_width = (uint8_t)v;
            lv_obj_set_style_border_width(s_widget->root, v, 0);
            break;
        case D_PANEL_BORDER_R:
            if (v < 0)  v = 0;
            if (v > 30) v = 30;
            pd->border_radius = (uint8_t)v;
            lv_obj_set_style_radius(s_widget->root, v, 0);
            break;
    }
    if (s_panel_dim_value_lbls[dim] && lv_obj_is_valid(s_panel_dim_value_lbls[dim]))
        lv_label_set_text_fmt(s_panel_dim_value_lbls[dim], "%d", v);
}

static void _dim_slider_cb(lv_event_t *e) {
    int dim = (int)(intptr_t)lv_event_get_user_data(e);
    int v   = lv_slider_get_value(lv_event_get_target(e));
    _panel_apply_dim(dim, v);
}

static void _make_dim_row(lv_obj_t *parent, const char *name, int dim,
                          int initial, int vmin, int vmax) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_width(lbl, 100);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 130, 8);
    lv_slider_set_range(slider, vmin, vmax);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, DT_BG_INSET, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, DT_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, DT_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, _dim_slider_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)dim);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text_fmt(value, "%d", initial);
    lv_obj_set_width(value, 30);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    s_panel_dim_value_lbls[dim] = value;
}

/* Panel STYLE tab builder */

static void _build_style_tab_panel(void) {
    if (!s_widget || s_widget->type != WIDGET_PANEL) return;

    _ensure_presets();
    for (int f = 0; f < F_PANEL_FIELD_COUNT; f++)
        s_panel_swatch_preview[f] = NULL;
    for (int d = 0; d < D_PANEL_DIM_COUNT; d++)
        s_panel_dim_value_lbls[d] = NULL;

    panel_data_t *pd = (panel_data_t *)s_widget->type_data;
    if (!pd) return;

    lv_obj_t *colours = _make_card(s_content, "COLOURS");
    _make_color_row(colours, "Border",     F_PANEL_BORDER);
    _make_color_row(colours, "Background", F_PANEL_BG);
    _make_color_row(colours, "Label",      F_PANEL_LABEL);
    _make_color_row(colours, "Value",      F_PANEL_VALUE);

    lv_obj_t *dims = _make_card(s_content, "DIMENSIONS");
    _make_dim_row(dims, "Border width",  D_PANEL_BORDER_W, pd->border_width,  0, 10);
    _make_dim_row(dims, "Border radius", D_PANEL_BORDER_R, pd->border_radius, 0, 30);
}

/* Placeholder tab */

static void _legacy_btn_cb(lv_event_t *e) {
    (void)e;
    widget_t *w = s_widget;
    inspector_close();
    if (w) load_menu_screen_for_widget(w);
}

static void _build_placeholder_tab(const char *tab_name) {
    lv_obj_t *card = _make_card(s_content, NULL);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "%s coming soon", tab_name);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, DOCK_W - 80);
    lv_label_set_text(msg,
        "Position + size live in the bottom toolbar. "
        "For the full per-widget settings list, use the legacy "
        "editor or the web editor.");
    lv_obj_set_style_text_color(msg, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(msg, THEME_FONT_SMALL, 0);

    lv_obj_t *legacy_btn = _make_button(card, DOCK_W - 60, 40,
                                        "Open legacy editor");
    lv_obj_add_event_cb(legacy_btn, _legacy_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* Tab switching */

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
    _close_picker();
    s_active_tab = idx;
    lv_obj_clean(s_content);
    switch (idx) {
        case TAB_DATA:
            _build_placeholder_tab("Data");
            break;
        case TAB_STYLE:
            if (s_widget && s_widget->type == WIDGET_PANEL)
                _build_style_tab_panel();
            else
                _build_placeholder_tab("Style");
            break;
        case TAB_RULES:
            _build_placeholder_tab("Rules");
            break;
    }
    _refresh_tab_button_styles();
}

static void _tab_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _show_tab(idx);
}

/* Shell */

static void _back_btn_cb(lv_event_t *e) {
    (void)e;
    inspector_close();
}

static void _build_header(void) {
    lv_obj_t *header = lv_obj_create(s_overlay);
    lv_obj_set_size(header, DOCK_W, 48);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, DT_BG_PANEL, 0);
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
    snprintf(buf, sizeof(buf), "%s slot %u",
             _widget_type_name(s_widget ? s_widget->type : (widget_type_t)0),
             (unsigned)(s_widget ? s_widget->slot : 0));
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 52, 0);
}

static void _build_tab_bar(void) {
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_set_size(bar, DOCK_W, 40);
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
    lv_obj_set_size(s_content, DOCK_W, 480 - 48 - 40);
    lv_obj_set_pos(s_content, 0, 48 + 40);
    /* Transparent - gaps between cards let the dashboard show through. */
    lv_obj_set_style_bg_opa(s_content, 0, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 8, 0);
    lv_obj_set_style_pad_row(s_content, 10, 0);
    /* Content area itself is NOT scrollable - the dock is narrow so cards
     * stack tightly; if a future widget needs more space we'll re-enable
     * scrolling on individual cards. */
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
}

/* Public API */

void inspector_open(widget_t *w) {
    if (!w) return;
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;

    s_widget     = w;
    s_active_tab = TAB_STYLE;

    lv_obj_t *parent = ui_Screen3;
    if (!parent || !lv_obj_is_valid(parent)) parent = lv_scr_act();
    if (!parent) return;

    /* Click-eater over the dashboard area. Invisible (the dash stays at
     * full brightness) but absorbs taps so edit_mode's selection / drag
     * gesture handlers don't fire while the Inspector is up. */
    s_left_eater = lv_obj_create(parent);
    lv_obj_set_size(s_left_eater, DOCK_X, 480);
    lv_obj_set_pos(s_left_eater, 0, 0);
    lv_obj_set_style_bg_opa(s_left_eater, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_left_eater, 0, 0);
    lv_obj_set_style_radius(s_left_eater, 0, 0);
    lv_obj_set_style_pad_all(s_left_eater, 0, 0);
    lv_obj_clear_flag(s_left_eater, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_left_eater, LV_OBJ_FLAG_CLICKABLE);

    /* Right-side dock - the visible Inspector. */
    s_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_overlay, DOCK_W, 480);
    lv_obj_set_pos(s_overlay, DOCK_X, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Dock root transparent; the header / tab bar / cards each define
     * their own opacity so the live preview shows through gaps. */
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    _build_header();
    _build_tab_bar();
    _build_content_area();
    _show_tab(s_active_tab);

    lv_obj_move_foreground(s_left_eater);
    lv_obj_move_foreground(s_overlay);
    ui_Screen3_refresh_overlays();

    ESP_LOGI(TAG, "Opened for %s slot %u",
             _widget_type_name(w->type), (unsigned)w->slot);
}

void inspector_close(void) {
    _close_picker();
    if (s_overlay) {
        if (lv_obj_is_valid(s_overlay)) lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    if (s_left_eater) {
        if (lv_obj_is_valid(s_left_eater)) lv_obj_del(s_left_eater);
        s_left_eater = NULL;
    }
    s_widget          = NULL;
    s_content         = NULL;
    s_tab_indicator   = NULL;
    for (int i = 0; i < TAB_COUNT; i++) s_tab_buttons[i] = NULL;

    edit_mode_commit_external_edit();

    ESP_LOGI(TAG, "Closed");
}

bool inspector_is_open(void) {
    return s_overlay != NULL && lv_obj_is_valid(s_overlay);
}
