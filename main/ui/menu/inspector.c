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
#include "ui/screens/ui_Screen3.h" /* ui_Screen3_refresh_overlays */
#include "ui/theme.h"
#include "ui/ui.h"                 /* ui_Screen3 extern */
#include "widgets/widget_fields.h" /* schema-driven STYLE tab */
#include "widgets/signal.h"        /* signal binding picker */
#include "layout/ecu_presets.h"    /* signal library: ECU preset sources */
#include "storage/user_signals.h"  /* signal library: user's saved signals */
#include "ui/callbacks/ui_callbacks.h" /* keyboard popovers for TEXT/NUMBER rows */
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>   /* atoi */
#include <string.h>

static const char *TAG = "inspector";

/* Tab definitions. LAYOUT was removed - the bottom toolbar's chip popover
 * owns position + size, the Inspector doesn't duplicate it. */
#define TAB_COUNT 3
enum { TAB_DATA = 0, TAB_STYLE, TAB_RULES };
static const char *const s_tab_names[TAB_COUNT] = {
    "DATA", "STYLE", "RULES"
};

/* Dock geometry. Width varies per tab - STYLE is narrow so the dashboard
 * stays maximally visible during colour / dimension tweaks; DATA and
 * RULES are wider because they pack signal pickers, alert configs and
 * other dense rows that don't fit cleanly in 320 px. */
#define DOCK_W_NARROW 320
#define DOCK_W_WIDE   480
#define DOCK_H        480

static int s_dock_w = DOCK_W_NARROW;

/* Target dock width per tab. Used by _show_tab to switch widths when
 * the user taps a different tab. */
static const int s_tab_dock_w[TAB_COUNT] = {
    [TAB_DATA]  = DOCK_W_WIDE,
    [TAB_STYLE] = DOCK_W_NARROW,
    [TAB_RULES] = DOCK_W_WIDE,
};

static inline int _dock_x(void)    { return 800 - s_dock_w; }
static inline int _tab_width(void) { return s_dock_w / TAB_COUNT; }

/* Module state */

static lv_obj_t *s_overlay         = NULL;   /* right-side dock root */
static lv_obj_t *s_left_eater      = NULL;   /* invisible click-eater over
                                              * the dashboard area so
                                              * edit_mode drag / selection
                                              * can't fire underneath */
static lv_obj_t *s_header          = NULL;   /* resized on dock-width change */
static lv_obj_t *s_tab_bar         = NULL;   /* resized on dock-width change */
static lv_obj_t *s_tab_buttons[TAB_COUNT] = {NULL};
static lv_obj_t *s_tab_indicator   = NULL;
static lv_obj_t *s_content         = NULL;
static widget_t *s_widget          = NULL;
static int       s_active_tab      = TAB_STYLE;

/* Dock side toggle. 0 = right (default, matches web editor), 1 = left.
 * Resets to right on each inspector_open - per-session, not persisted. */
static int       s_dock_side       = 0;
static lv_obj_t *s_side_left_btn   = NULL;
static lv_obj_t *s_side_right_btn  = NULL;

/* Colour-wheel popover state. Opened from the picker's "Custom..." button.
 * Live-previews on VALUE_CHANGED, commits on OK, reverts on Cancel. */
static lv_obj_t *s_wheel_popup     = NULL;
static lv_obj_t *s_wheel           = NULL;
static char      s_wheel_field[32] = "";
static lv_color_t s_wheel_initial;

/* Hidden textarea reused by show_text_input_dialog_ex - the dialog
 * requires a target lv_textarea_t to write into, but we read the typed
 * value through the on_confirm callback so the target is just a sink.
 * Lazy-allocated on first text edit, lives on lv_layer_top so it stays
 * around across screen changes. */
static lv_obj_t *s_hidden_text_ta  = NULL;
/* Schema name of the field currently being edited via the keyboard. */
static char      s_active_text_field[32] = "";

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
                   (lv_coord_t)(s_active_tab * _tab_width()), 38);
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

#define PRESET_COUNT 12
static lv_color_t s_presets[PRESET_COUNT];
static bool       s_presets_ready = false;

static void _ensure_presets(void) {
    if (s_presets_ready) return;
    /* Row 0: light tones */
    s_presets[0]  = lv_color_white();
    s_presets[1]  = lv_color_hex(0x848684);  /* light warm grey */
    s_presets[2]  = THEME_COLOR_RED;
    s_presets[3]  = THEME_COLOR_ORANGE;
    /* Row 1: rainbow mid */
    s_presets[4]  = THEME_COLOR_YELLOW;
    s_presets[5]  = THEME_COLOR_GREEN;
    s_presets[6]  = THEME_COLOR_CYAN;
    s_presets[7]  = THEME_COLOR_BLUE;
    /* Row 2: rainbow tail + dark tones */
    s_presets[8]  = THEME_COLOR_PURPLE;
    s_presets[9]  = THEME_COLOR_PINK;
    s_presets[10] = lv_color_hex(0x292C29);  /* dark warm grey (THEME_COLOR_SURFACE) */
    s_presets[11] = lv_color_black();
    s_presets_ready = true;
}

/* Per-row registry. Schema name -> preview swatch / value label. Refreshed
 * when the picker / wheel writes a new value so the row stays in sync with
 * the live widget. Capped well above the largest widget's appearance-field
 * count - resize if Meter's STYLE tab ever balloons. */
#define INSPECTOR_FIELD_NAME_LEN 32
#define INSPECTOR_MAX_ROWS       24

typedef struct {
    char       name[INSPECTOR_FIELD_NAME_LEN];
    lv_obj_t  *swatch;       /* COLOR rows */
    lv_obj_t  *value_lbl;    /* STEPPER / SLIDER value readout */
} inspector_row_t;

static inspector_row_t s_rows[INSPECTOR_MAX_ROWS];
static int             s_row_count = 0;

/* Picker popover state. Opened on tap of a colour row, dismissed on tap
 * outside the inner card or on swatch select. */
static lv_obj_t *s_picker                          = NULL;
static char      s_picker_field[INSPECTOR_FIELD_NAME_LEN] = "";
static char      s_picker_label[INSPECTOR_FIELD_NAME_LEN] = "";
static lv_obj_t *s_picker_swatches[PRESET_COUNT]   = {NULL};

static bool _color_eq(lv_color_t a, lv_color_t b) {
    return a.full == b.full;
}

/* Schema-driven field helpers.
 *
 * All field reads / writes go through the widget's inspector_get /
 * inspector_set vtable hooks. Per-widget code (e.g. widget_panel.c)
 * implements those by mapping schema names to type_data members and
 * calling the matching lv_obj_set_style_* on the live LVGL object.
 *
 * The inspector is otherwise widget-agnostic - adding new fields means
 * editing schema/widgets.schema.json + running tools/codegen_widget_inspector.py
 * + adding switch cases in the widget's inspector_set hook. */

static inspector_row_t *_find_row(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < s_row_count; i++) {
        if (strcmp(s_rows[i].name, name) == 0) return &s_rows[i];
    }
    return NULL;
}

static inspector_row_t *_register_row(const char *name) {
    if (!name || s_row_count >= INSPECTOR_MAX_ROWS) return NULL;
    inspector_row_t *r = &s_rows[s_row_count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, INSPECTOR_FIELD_NAME_LEN - 1);
    return r;
}

static lv_color_t _get_field_color(const char *name) {
    if (!s_widget || !s_widget->inspector_get) return lv_color_black();
    widget_field_value_t v = {0};
    if (!s_widget->inspector_get(s_widget, name, &v)) return lv_color_black();
    return lv_color_hex(v.color);
}

static int _get_field_int(const char *name, int fallback) {
    if (!s_widget || !s_widget->inspector_get) return fallback;
    widget_field_value_t v = {0};
    if (!s_widget->inspector_get(s_widget, name, &v)) return fallback;
    return v.i;
}

static void _apply_field_color(const char *name, lv_color_t c) {
    if (!s_widget || !s_widget->inspector_set) return;
    widget_field_value_t v = { .color = lv_color_to32(c) & 0xFFFFFF };
    s_widget->inspector_set(s_widget, name, &v);

    /* Update the row's preview swatch + picker grid (if open on this field). */
    inspector_row_t *r = _find_row(name);
    if (r && r->swatch && lv_obj_is_valid(r->swatch)) {
        lv_obj_set_style_bg_color(r->swatch, c, 0);
    }
    if (strcmp(s_picker_field, name) == 0) {
        for (int i = 0; i < PRESET_COUNT; i++) {
            lv_obj_t *sw = s_picker_swatches[i];
            if (!sw || !lv_obj_is_valid(sw)) continue;
            bool active = _color_eq(s_presets[i], c);
            lv_obj_set_style_border_color(sw,
                active ? DT_ACCENT : lv_color_white(), 0);
            lv_obj_set_style_border_width(sw, active ? 3 : 1, 0);
            lv_obj_set_style_border_opa(sw, active ? LV_OPA_COVER : 60, 0);
        }
    }
}

static void _apply_field_int(const char *name, int v) {
    if (!s_widget || !s_widget->inspector_set) return;
    widget_field_value_t val = { .i = v };
    s_widget->inspector_set(s_widget, name, &val);

    /* Update the row's value readout. */
    inspector_row_t *r = _find_row(name);
    if (r && r->value_lbl && lv_obj_is_valid(r->value_lbl)) {
        lv_label_set_text_fmt(r->value_lbl, "%d", v);
    }
}

/* Picker popover */

static void _custom_btn_cb(lv_event_t *e);   /* defined alongside the wheel */

static void _close_picker(void) {
    if (s_picker && lv_obj_is_valid(s_picker)) {
        lv_obj_del(s_picker);
    }
    s_picker = NULL;
    s_picker_field[0] = '\0';
    s_picker_label[0] = '\0';
    for (int i = 0; i < PRESET_COUNT; i++) s_picker_swatches[i] = NULL;
}

static void _picker_swatch_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= PRESET_COUNT) return;
    if (s_picker_field[0] == '\0') return;
    char field_copy[INSPECTOR_FIELD_NAME_LEN];
    strncpy(field_copy, s_picker_field, sizeof(field_copy) - 1);
    field_copy[sizeof(field_copy) - 1] = '\0';
    _apply_field_color(field_copy, s_presets[idx]);
    _close_picker();
}

/* Tap on the backdrop (but not the inner card) closes the picker. */
static void _picker_backdrop_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    _close_picker();
}

static void _open_picker(const char *field_name, const char *display_label) {
    if (!s_widget || !field_name) return;
    _close_picker();
    strncpy(s_picker_field, field_name, sizeof(s_picker_field) - 1);
    s_picker_field[sizeof(s_picker_field) - 1] = '\0';
    strncpy(s_picker_label, display_label ? display_label : field_name,
            sizeof(s_picker_label) - 1);
    s_picker_label[sizeof(s_picker_label) - 1] = '\0';

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
    lv_obj_set_size(card, 300, 248);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "%s colour", s_picker_label);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_SMALL, 0);

    /* 4x3 swatch grid (12 presets), 40 px swatches + 8 px gaps. Centred
     * in the card so the layout breathes the same whether 10, 12, or
     * 16 presets ever live here. */
    lv_color_t current = _get_field_color(field_name);
    const int cell     = 48;
    const int cols     = 4;
    const int grid_w   = cols * 40 + (cols - 1) * 8;
    const int inner_w  = 300 - 16 * 2;     /* card width minus pad */
    const int grid_x0  = (inner_w - grid_w) / 2;
    const int grid_y0  = 28;
    for (int i = 0; i < PRESET_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
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

    /* Custom... button - opens the colour wheel for anything not in the
     * preset palette. */
    lv_obj_t *custom = _make_button(card, 268, 36, "Custom...");
    lv_obj_align(custom, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(custom, _custom_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* Custom colour wheel - opened from the picker's "Custom..." button.
 *
 * Same lv_colorwheel widget the legacy config_modal uses. Live preview
 * on every drag step so the user sees the colour land in real time on
 * the widget behind. OK commits, Cancel reverts to the colour the
 * picker was opened at. */

static void _close_wheel(void) {
    if (s_wheel_popup && lv_obj_is_valid(s_wheel_popup)) {
        lv_obj_del(s_wheel_popup);
    }
    s_wheel_popup = NULL;
    s_wheel       = NULL;
    s_wheel_field[0] = '\0';
}

static void _wheel_value_changed_cb(lv_event_t *e) {
    (void)e;
    if (s_wheel_field[0] == '\0' || !s_wheel || !lv_obj_is_valid(s_wheel)) return;
    lv_color_t c = lv_colorwheel_get_rgb(s_wheel);
    _apply_field_color(s_wheel_field, c);
}

static void _wheel_ok_cb(lv_event_t *e) {
    (void)e;
    _close_wheel();
}

static void _wheel_cancel_cb(lv_event_t *e) {
    (void)e;
    if (s_wheel_field[0] != '\0') {
        _apply_field_color(s_wheel_field, s_wheel_initial);
    }
    _close_wheel();
}

static void _open_wheel(const char *field_name, const char *display_label,
                        lv_color_t initial) {
    if (!s_widget || !field_name) return;
    _close_wheel();
    strncpy(s_wheel_field, field_name, sizeof(s_wheel_field) - 1);
    s_wheel_field[sizeof(s_wheel_field) - 1] = '\0';
    s_wheel_initial = initial;

    s_wheel_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_wheel_popup, 800, 480);
    lv_obj_set_pos(s_wheel_popup, 0, 0);
    lv_obj_set_style_bg_color(s_wheel_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wheel_popup, 110, 0);
    lv_obj_set_style_border_width(s_wheel_popup, 0, 0);
    lv_obj_set_style_radius(s_wheel_popup, 0, 0);
    lv_obj_set_style_pad_all(s_wheel_popup, 0, 0);
    lv_obj_clear_flag(s_wheel_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wheel_popup, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(s_wheel_popup);
    lv_obj_set_size(card, 340, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "%s - custom",
                          display_label ? display_label : field_name);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_BODY, 0);

    s_wheel = lv_colorwheel_create(card, true);
    lv_obj_set_size(s_wheel, 220, 220);
    lv_obj_align(s_wheel, LV_ALIGN_TOP_MID, 0, 28);
    lv_colorwheel_set_rgb(s_wheel, initial);
    lv_obj_add_event_cb(s_wheel, _wheel_value_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *ok = _make_button(card, 120, 40, "OK");
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_set_style_bg_color(ok, DT_ACCENT, 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ok, _wheel_ok_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = _make_button(card, 120, 40, "Cancel");
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_add_event_cb(cancel, _wheel_cancel_cb, LV_EVENT_CLICKED, NULL);
}

static void _custom_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_picker_field[0] == '\0' || !s_widget) return;
    char field_copy[INSPECTOR_FIELD_NAME_LEN];
    char label_copy[INSPECTOR_FIELD_NAME_LEN];
    strncpy(field_copy, s_picker_field, sizeof(field_copy) - 1);
    field_copy[sizeof(field_copy) - 1] = '\0';
    strncpy(label_copy, s_picker_label, sizeof(label_copy) - 1);
    label_copy[sizeof(label_copy) - 1] = '\0';
    lv_color_t current = _get_field_color(field_copy);
    _close_picker();
    _open_wheel(field_copy, label_copy, current);
}

/* Dock side switcher - move the dock between right and left edges. */

static void _refresh_side_buttons(void) {
    if (s_side_left_btn && lv_obj_is_valid(s_side_left_btn)) {
        bool active = (s_dock_side == 1);
        lv_obj_set_style_border_color(s_side_left_btn,
            active ? DT_ACCENT : lv_color_white(), 0);
        lv_obj_set_style_border_opa(s_side_left_btn,
            active ? LV_OPA_COVER : 60, 0);
        lv_obj_set_style_bg_opa(s_side_left_btn, active ? 70 : 28, 0);
    }
    if (s_side_right_btn && lv_obj_is_valid(s_side_right_btn)) {
        bool active = (s_dock_side == 0);
        lv_obj_set_style_border_color(s_side_right_btn,
            active ? DT_ACCENT : lv_color_white(), 0);
        lv_obj_set_style_border_opa(s_side_right_btn,
            active ? LV_OPA_COVER : 60, 0);
        lv_obj_set_style_bg_opa(s_side_right_btn, active ? 70 : 28, 0);
    }
}

/* Resize every dock element to match the current s_dock_w / s_dock_side.
 * Called on tab change (when width may flip narrow/wide) and on side
 * change. Snaps - no animation yet. */
static void _apply_dock_geometry(void) {
    if (!s_overlay || !lv_obj_is_valid(s_overlay)) return;

    int dock_x = (s_dock_side == 0) ? (800 - s_dock_w) : 0;
    lv_obj_set_size(s_overlay, s_dock_w, DOCK_H);
    lv_obj_set_pos(s_overlay, dock_x, 0);

    if (s_left_eater && lv_obj_is_valid(s_left_eater)) {
        if (s_dock_side == 0) {
            lv_obj_set_pos(s_left_eater, 0, 0);
        } else {
            lv_obj_set_pos(s_left_eater, s_dock_w, 0);
        }
        lv_obj_set_size(s_left_eater, 800 - s_dock_w, DOCK_H);
    }

    if (s_header  && lv_obj_is_valid(s_header))  lv_obj_set_width(s_header,  s_dock_w);
    if (s_tab_bar && lv_obj_is_valid(s_tab_bar)) lv_obj_set_width(s_tab_bar, s_dock_w);
    if (s_content && lv_obj_is_valid(s_content)) lv_obj_set_width(s_content, s_dock_w);

    int tw = _tab_width();
    for (int i = 0; i < TAB_COUNT; i++) {
        if (s_tab_buttons[i] && lv_obj_is_valid(s_tab_buttons[i])) {
            lv_obj_set_size(s_tab_buttons[i], tw, 40);
            lv_obj_set_pos(s_tab_buttons[i], i * tw, 0);
        }
    }
    if (s_tab_indicator && lv_obj_is_valid(s_tab_indicator)) {
        lv_obj_set_width(s_tab_indicator, tw);
        _position_tab_indicator();
    }
}

static void _apply_dock_side(void) {
    _apply_dock_geometry();
    _refresh_side_buttons();
}

static void _side_left_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_dock_side == 1) return;
    s_dock_side = 1;
    _apply_dock_side();
}

static void _side_right_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_dock_side == 0) return;
    s_dock_side = 0;
    _apply_dock_side();
}

/* Schema-driven row builders.
 *
 * Each renderer takes a const widget_field_t * (from the codegen'd
 * WIDGET_FIELDS array) and builds the appropriate row. Live preview is
 * routed through s_widget->inspector_set so the widget code is the only
 * place that knows how a schema field maps to type_data and LVGL.  */

typedef struct {
    char field_name[INSPECTOR_FIELD_NAME_LEN];
    char field_label[INSPECTOR_FIELD_NAME_LEN];
} color_row_event_ctx_t;

/* One ctx per colour row, kept alive for the row's lifetime. We store
 * them in s_color_row_ctxs and clear on tab teardown. */
#define INSPECTOR_MAX_COLOR_ROWS 16
static color_row_event_ctx_t s_color_row_ctxs[INSPECTOR_MAX_COLOR_ROWS];
static int                   s_color_row_ctx_count = 0;

static void _color_row_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    color_row_event_ctx_t *ctx = (color_row_event_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    _open_picker(ctx->field_name, ctx->field_label);
}

static void _make_color_row_schema(lv_obj_t *parent, const widget_field_t *f) {
    if (!f || !s_widget) return;

    /* Stash field name + label for the click callback. */
    if (s_color_row_ctx_count >= INSPECTOR_MAX_COLOR_ROWS) return;
    color_row_event_ctx_t *ctx = &s_color_row_ctxs[s_color_row_ctx_count++];
    strncpy(ctx->field_name,  f->name,  sizeof(ctx->field_name)  - 1);
    ctx->field_name[sizeof(ctx->field_name) - 1] = '\0';
    strncpy(ctx->field_label, f->label, sizeof(ctx->field_label) - 1);
    ctx->field_label[sizeof(ctx->field_label) - 1] = '\0';

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
    lv_obj_add_event_cb(row, _color_row_clicked_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, f->label);
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
    lv_obj_set_style_bg_color(sw, _get_field_color(f->name), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sw, lv_color_white(), 0);
    lv_obj_set_style_border_opa(sw, 80, 0);
    lv_obj_set_style_border_width(sw, 1, 0);
    lv_obj_set_style_radius(sw, 4, 0);
    lv_obj_set_style_pad_all(sw, 0, 0);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);

    inspector_row_t *r = _register_row(f->name);
    if (r) r->swatch = sw;
}

/* Stepper / slider row. */

typedef struct {
    char field_name[INSPECTOR_FIELD_NAME_LEN];
} int_row_event_ctx_t;

#define INSPECTOR_MAX_INT_ROWS 24
static int_row_event_ctx_t s_int_row_ctxs[INSPECTOR_MAX_INT_ROWS];
static int                 s_int_row_ctx_count = 0;

static void _int_slider_cb(lv_event_t *e) {
    int_row_event_ctx_t *ctx = (int_row_event_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    int v = lv_slider_get_value(lv_event_get_target(e));
    _apply_field_int(ctx->field_name, v);
}

static void _make_stepper_row_schema(lv_obj_t *parent, const widget_field_t *f) {
    if (!f || !s_widget) return;
    if (s_int_row_ctx_count >= INSPECTOR_MAX_INT_ROWS) return;

    int_row_event_ctx_t *ctx = &s_int_row_ctxs[s_int_row_ctx_count++];
    strncpy(ctx->field_name, f->name, sizeof(ctx->field_name) - 1);
    ctx->field_name[sizeof(ctx->field_name) - 1] = '\0';

    /* Read current value via the widget's get hook; falls back to schema
     * default if the widget hasn't implemented the field yet. */
    int current = _get_field_int(f->name, f->default_int);

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
    lv_label_set_text(lbl, f->label);
    lv_obj_set_width(lbl, 100);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 130, 8);
    lv_slider_set_range(slider, f->min_int, f->max_int);
    lv_slider_set_value(slider, current, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, DT_BG_INSET, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, DT_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, DT_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, _int_slider_cb, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text_fmt(value, "%d", current);
    lv_obj_set_width(value, 30);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    inspector_row_t *r = _register_row(f->name);
    if (r) r->value_lbl = value;
}

/* Checkbox row (lv_switch). */

typedef struct {
    char field_name[INSPECTOR_FIELD_NAME_LEN];
} bool_row_event_ctx_t;

#define INSPECTOR_MAX_BOOL_ROWS 16
static bool_row_event_ctx_t s_bool_row_ctxs[INSPECTOR_MAX_BOOL_ROWS];
static int                  s_bool_row_ctx_count = 0;

static void _bool_switch_cb(lv_event_t *e) {
    bool_row_event_ctx_t *ctx = (bool_row_event_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !s_widget || !s_widget->inspector_set) return;
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    widget_field_value_t v = { .b = on };
    s_widget->inspector_set(s_widget, ctx->field_name, &v);
}

static void _make_checkbox_row_schema(lv_obj_t *parent, const widget_field_t *f) {
    if (!f || !s_widget) return;
    if (s_bool_row_ctx_count >= INSPECTOR_MAX_BOOL_ROWS) return;

    bool_row_event_ctx_t *ctx = &s_bool_row_ctxs[s_bool_row_ctx_count++];
    strncpy(ctx->field_name, f->name, sizeof(ctx->field_name) - 1);
    ctx->field_name[sizeof(ctx->field_name) - 1] = '\0';

    bool current = false;
    if (s_widget->inspector_get) {
        widget_field_value_t v = {0};
        if (s_widget->inspector_get(s_widget, f->name, &v)) current = v.b;
        else                                                 current = (f->default_int != 0);
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, f->label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -2, 0);
    if (current) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, DT_BG_INSET, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, DT_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, _bool_switch_cb, LV_EVENT_VALUE_CHANGED, ctx);
}

/* Select row (lv_dropdown). */

typedef struct {
    char     field_name[INSPECTOR_FIELD_NAME_LEN];
    /* Pointer into the schema's options array - we don't own it. */
    const widget_field_option_t *options;
    uint8_t  option_count;
} select_row_event_ctx_t;

#define INSPECTOR_MAX_SELECT_ROWS 8
static select_row_event_ctx_t s_select_row_ctxs[INSPECTOR_MAX_SELECT_ROWS];
static int                    s_select_row_ctx_count = 0;

static void _select_dropdown_cb(lv_event_t *e) {
    select_row_event_ctx_t *ctx =
        (select_row_event_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !s_widget || !s_widget->inspector_set) return;
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t idx = lv_dropdown_get_selected(dd);
    if (idx >= ctx->option_count) return;
    widget_field_value_t v = { .i = ctx->options[idx].value };
    s_widget->inspector_set(s_widget, ctx->field_name, &v);
}

static void _make_select_row_schema(lv_obj_t *parent, const widget_field_t *f) {
    if (!f || !s_widget || !f->options || f->option_count == 0) return;
    if (s_select_row_ctx_count >= INSPECTOR_MAX_SELECT_ROWS) return;

    select_row_event_ctx_t *ctx = &s_select_row_ctxs[s_select_row_ctx_count++];
    strncpy(ctx->field_name, f->name, sizeof(ctx->field_name) - 1);
    ctx->field_name[sizeof(ctx->field_name) - 1] = '\0';
    ctx->options      = f->options;
    ctx->option_count = f->option_count;

    int current = _get_field_int(f->name, f->default_int);
    int sel_idx = 0;
    for (uint8_t i = 0; i < f->option_count; i++) {
        if (f->options[i].value == current) { sel_idx = i; break; }
    }

    /* Build newline-separated options string for lv_dropdown. */
    char opts_buf[256];
    size_t cursor = 0;
    for (uint8_t i = 0; i < f->option_count && cursor < sizeof(opts_buf) - 2; i++) {
        const char *lbl = f->options[i].label ? f->options[i].label : "";
        size_t lbl_len = strlen(lbl);
        if (i > 0 && cursor < sizeof(opts_buf) - 1) opts_buf[cursor++] = '\n';
        size_t copy = lbl_len;
        if (cursor + copy >= sizeof(opts_buf)) copy = sizeof(opts_buf) - 1 - cursor;
        memcpy(opts_buf + cursor, lbl, copy);
        cursor += copy;
    }
    opts_buf[cursor] = '\0';

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 40);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, f->label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, opts_buf);
    lv_dropdown_set_selected(dd, (uint16_t)sel_idx);
    lv_obj_set_size(dd, 150, 32);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(dd, DT_BG_INSET, 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dd, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_text_color(dd, lv_color_white(), 0);
    lv_obj_set_style_text_font(dd, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(dd, _select_dropdown_cb, LV_EVENT_VALUE_CHANGED, ctx);
}

/* Text / number row. Tap opens a keyboard popover; on confirm we write
 * the new value via inspector_set. */

static void _ensure_hidden_textarea(void) {
    if (s_hidden_text_ta && lv_obj_is_valid(s_hidden_text_ta)) return;
    s_hidden_text_ta = lv_textarea_create(lv_layer_top());
    lv_obj_add_flag(s_hidden_text_ta, LV_OBJ_FLAG_HIDDEN);
}

static void _numeric_confirm_cb(const char *text, void *user_data);  /* fwd */

static void _text_confirm_cb(const char *text, void *user_data) {
    (void)user_data;
    if (s_active_text_field[0] == '\0' || !s_widget || !s_widget->inspector_set) return;
    widget_field_value_t v = { .str = text };
    s_widget->inspector_set(s_widget, s_active_text_field, &v);

    /* Update the row's value preview. */
    inspector_row_t *r = _find_row(s_active_text_field);
    if (r && r->value_lbl && lv_obj_is_valid(r->value_lbl)) {
        lv_label_set_text(r->value_lbl, text ? text : "");
    }
    s_active_text_field[0] = '\0';
}

static void _text_cancel_cb(void *user_data) {
    (void)user_data;
    s_active_text_field[0] = '\0';
}

typedef struct {
    char field_name[INSPECTOR_FIELD_NAME_LEN];
    char label[INSPECTOR_FIELD_NAME_LEN];
    bool numeric;
} text_row_event_ctx_t;

#define INSPECTOR_MAX_TEXT_ROWS 12
static text_row_event_ctx_t s_text_row_ctxs[INSPECTOR_MAX_TEXT_ROWS];
static int                  s_text_row_ctx_count = 0;

static void _text_row_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_row_event_ctx_t *ctx = (text_row_event_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !s_widget) return;
    strncpy(s_active_text_field, ctx->field_name, sizeof(s_active_text_field) - 1);
    s_active_text_field[sizeof(s_active_text_field) - 1] = '\0';

    if (ctx->numeric) {
        char buf[32];
        int cur = _get_field_int(ctx->field_name, 0);
        snprintf(buf, sizeof(buf), "%d", cur);
        show_numeric_input_dialog(ctx->label, buf,
                                  _numeric_confirm_cb, _text_cancel_cb, NULL);
        return;
    }

    _ensure_hidden_textarea();
    if (!s_hidden_text_ta) return;
    /* Pre-fill the textarea with the current value so the dialog opens
     * with the existing text editable rather than blank. */
    const char *current = "";
    if (s_widget->inspector_get) {
        widget_field_value_t v = {0};
        if (s_widget->inspector_get(s_widget, ctx->field_name, &v) && v.str) current = v.str;
    }
    lv_textarea_set_text(s_hidden_text_ta, current);
    show_text_input_dialog_ex(s_hidden_text_ta, ctx->label, "", false,
                              _text_confirm_cb, _text_cancel_cb, NULL);
}

/* Map a numeric ASCII string to int when the numeric dialog confirms. */
static void _numeric_confirm_cb(const char *text, void *user_data) {
    (void)user_data;
    if (s_active_text_field[0] == '\0' || !s_widget || !s_widget->inspector_set) return;
    int v_int = text ? atoi(text) : 0;
    widget_field_value_t v = { .i = v_int };
    s_widget->inspector_set(s_widget, s_active_text_field, &v);

    inspector_row_t *r = _find_row(s_active_text_field);
    if (r && r->value_lbl && lv_obj_is_valid(r->value_lbl)) {
        lv_label_set_text_fmt(r->value_lbl, "%d", v_int);
    }
    s_active_text_field[0] = '\0';
}

static void _make_text_row_schema(lv_obj_t *parent, const widget_field_t *f,
                                  bool numeric) {
    if (!f || !s_widget) return;
    if (s_text_row_ctx_count >= INSPECTOR_MAX_TEXT_ROWS) return;

    text_row_event_ctx_t *ctx = &s_text_row_ctxs[s_text_row_ctx_count++];
    strncpy(ctx->field_name, f->name, sizeof(ctx->field_name) - 1);
    ctx->field_name[sizeof(ctx->field_name) - 1] = '\0';
    strncpy(ctx->label, f->label, sizeof(ctx->label) - 1);
    ctx->label[sizeof(ctx->label) - 1] = '\0';
    ctx->numeric = numeric;

    /* Current value as a displayed string. */
    char preview[40] = "";
    if (numeric) {
        int v = _get_field_int(f->name, f->default_int);
        snprintf(preview, sizeof(preview), "%d", v);
    } else if (s_widget->inspector_get) {
        widget_field_value_t v = {0};
        if (s_widget->inspector_get(s_widget, f->name, &v) && v.str) {
            strncpy(preview, v.str, sizeof(preview) - 1);
            preview[sizeof(preview) - 1] = '\0';
        }
    }

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
    lv_obj_add_event_cb(row, _text_row_clicked_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, f->label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_color(chev, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(chev, THEME_FONT_SMALL, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, preview);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value, 130);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_color(value, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(value, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    inspector_row_t *r = _register_row(f->name);
    if (r) r->value_lbl = value;
}

/* Signal binding row + inline picker.
 *
 * The signal binding isn't a schema field per se - it's a layout-level
 * relationship between a widget and a signal in the registry. The DATA
 * tab renders it as a special first row that opens a fullscreen-within-
 * dock picker on tap.
 *
 * Widget support is detected by trying inspector_get("signal_name"):
 * if it returns true, we render the row; otherwise we skip it. */

static lv_obj_t *s_sig_row_value_lbl = NULL;   /* signal row's preview label */
static bool      s_in_signal_picker  = false;

static void _show_tab(int idx);              /* defined further down */
static void _close_signal_picker(void);
static void _open_signal_wizard(void);       /* manual-entry wizard, below */

static const char *_widget_signal_name(void) {
    if (!s_widget || !s_widget->inspector_get) return "";
    widget_field_value_t v = {0};
    if (!s_widget->inspector_get(s_widget, "signal_name", &v)) return "";
    return v.str ? v.str : "";
}

static bool _widget_supports_signal_binding(void) {
    if (!s_widget || !s_widget->inspector_get || !s_widget->inspector_set)
        return false;
    widget_field_value_t v = {0};
    return s_widget->inspector_get(s_widget, "signal_name", &v);
}

/* Tap on a signal row in the picker applies the binding + returns to
 * the DATA tab. user_data is a pointer to the signal_t->name string
 * which is stable as long as the registry isn't reset (it isn't while
 * the Inspector is open). */
static void _new_signal_row_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _open_signal_wizard();
}

static void _signal_picker_row_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name || !s_widget || !s_widget->inspector_set) return;
    widget_field_value_t v = { .str = name };
    s_widget->inspector_set(s_widget, "signal_name", &v);
    _close_signal_picker();
}

static void _signal_picker_back_cb(lv_event_t *e) {
    (void)e;
    _close_signal_picker();
}

static void _close_signal_picker(void) {
    s_in_signal_picker = false;
    /* Rebuild the DATA tab; cheaper than tracking saved state. */
    _show_tab(TAB_DATA);
}

static void _open_signal_picker(void) {
    if (!s_content || !lv_obj_is_valid(s_content)) return;
    if (!s_widget) return;
    s_in_signal_picker = true;
    lv_obj_clean(s_content);

    /* Header strip with Back and title. */
    lv_obj_t *hdr = lv_obj_create(s_content);
    lv_obj_set_size(hdr, LV_PCT(100), 40);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);

    lv_obj_t *back = _make_button(hdr, 60, 32, "Back");
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(back, _signal_picker_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Choose Signal");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_BODY, 0);

    /* Signal list card - fills the rest of the dock. */
    lv_obj_t *list_card = _make_card(s_content, NULL);
    lv_obj_set_flex_grow(list_card, 1);
    lv_obj_set_style_pad_row(list_card, 2, 0);
    lv_obj_add_flag(list_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_card, LV_SCROLLBAR_MODE_ACTIVE);

    uint16_t count = signal_get_count();
    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list_card);
        lv_label_set_text(empty,
            "No signals in this layout.\n"
            "Create one to bind a widget.");
        lv_obj_set_style_text_color(empty, DT_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(empty, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    const char *current = _widget_signal_name();
    for (uint16_t i = 0; i < count; i++) {
        signal_t *s = signal_get_by_index(i);
        if (!s) continue;

        lv_obj_t *row = lv_btn_create(list_card);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 44);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row, DT_BG_INSET, 0);
        lv_obj_set_style_bg_opa(row, 60, 0);
        lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, 30, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_radius(row, 4, 0);

        bool is_current = (strcmp(s->name, current) == 0);
        if (is_current) {
            lv_obj_set_style_border_color(row, DT_ACCENT, 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        }

        /* Name on the left. */
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, s->name);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, 220);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(name_lbl, THEME_FONT_SMALL, 0);

        /* Current value + units on the right. Stale shows "--". */
        char buf[32];
        if (s->is_stale) {
            snprintf(buf, sizeof(buf), "--  %s", s->unit);
        } else if (s->unit[0]) {
            snprintf(buf, sizeof(buf), "%.2f %s", s->current_value, s->unit);
        } else {
            snprintf(buf, sizeof(buf), "%.2f", s->current_value);
        }
        lv_obj_t *val_lbl = lv_label_create(row);
        lv_label_set_text(val_lbl, buf);
        lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_text_color(val_lbl, DT_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(val_lbl, THEME_FONT_SMALL, 0);

        /* user_data points at s->name in the registry. Stable for the
         * picker's lifetime - the registry isn't reset while the
         * Inspector is open. */
        lv_obj_add_event_cb(row, _signal_picker_row_cb, LV_EVENT_CLICKED,
                            (void *)s->name);
    }

    /* "+ Create new signal" row at the bottom of the list. Opens the
     * manual-entry wizard. ECU Preset / From DBC entry points are added
     * in later steps. */
    lv_obj_t *new_row = lv_btn_create(list_card);
    lv_obj_set_width(new_row, LV_PCT(100));
    lv_obj_set_height(new_row, 44);
    lv_obj_clear_flag(new_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(new_row, DT_ACCENT, 0);
    lv_obj_set_style_bg_opa(new_row, 60, 0);
    lv_obj_set_style_bg_color(new_row, DT_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(new_row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(new_row, DT_ACCENT, 0);
    lv_obj_set_style_border_opa(new_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(new_row, 1, 0);
    lv_obj_set_style_shadow_width(new_row, 0, 0);
    lv_obj_set_style_pad_all(new_row, 6, 0);
    lv_obj_set_style_radius(new_row, 4, 0);

    lv_obj_t *new_lbl = lv_label_create(new_row);
    lv_label_set_text(new_lbl, LV_SYMBOL_PLUS "  Create new signal");
    lv_obj_center(new_lbl);
    lv_obj_set_style_text_color(new_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(new_lbl, THEME_FONT_SMALL, 0);

    lv_obj_add_event_cb(new_row, _new_signal_row_cb, LV_EVENT_CLICKED, NULL);
}

static void _signal_row_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _open_signal_picker();
}

static void _build_signal_row(lv_obj_t *parent) {
    if (!s_widget) return;
    const char *current = _widget_signal_name();

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
    lv_obj_add_event_cb(row, _signal_row_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Signal");
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_color(chev, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(chev, THEME_FONT_SMALL, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, (current && current[0]) ? current : "(none)");
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value, 280);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_color(value, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(value, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    s_sig_row_value_lbl = value;
}

/* New-signal wizard (Manual Entry path).
 *
 * Fullscreen modal popover on lv_layer_top. Form fields tap into the
 * existing keyboard popovers (TEXT for name / units / hex CAN ID /
 * float scale+offset; NUMBER for start_bit + length). Save calls
 * signal_register and binds the currently-selected widget to the new
 * signal.
 *
 * Endian + signedness aren't exposed here yet - they default to Intel
 * + unsigned (the common case). Refining them is a follow-up; the web
 * editor can edit them today on the saved layout.
 *
 * Persistence: signal_register only adds to the in-memory registry.
 * The layout save path (debounced by edit_mode) will pick the new
 * signal up the next time it serialises the layout. Step 6 will also
 * write the signal to /lfs/dbc/user.dbc for cross-layout reuse. */

typedef enum {
    WIZ_F_NAME = 0,
    WIZ_F_CAN_ID,
    WIZ_F_START_BIT,
    WIZ_F_LENGTH,
    WIZ_F_SCALE,
    WIZ_F_OFFSET,
    WIZ_F_UNIT,
    WIZ_F_COUNT,
} wizard_field_t;

typedef struct {
    char     name[32];
    uint32_t can_id;
    uint8_t  start_bit;
    uint8_t  length;
    float    scale;
    float    offset;
    char     unit[8];
} wizard_form_t;

/* Single-page manual-entry wizard. ECU presets + user.json signals are
 * browsable through the signal library (separate flow) so the wizard
 * stays focused on "I want to define a brand-new signal from scratch". */

static wizard_form_t s_wiz_form;
static lv_obj_t     *s_wiz_popup    = NULL;
static lv_obj_t     *s_wiz_card     = NULL;
static lv_obj_t     *s_wiz_title    = NULL;
static lv_obj_t     *s_wiz_subtitle = NULL;
static lv_obj_t     *s_wiz_body     = NULL;
static lv_obj_t     *s_wiz_btn_row  = NULL;
static lv_obj_t     *s_wiz_status_lbl = NULL;
static lv_obj_t     *s_wiz_field_lbls[WIZ_F_COUNT] = {NULL};


static const char *_wiz_field_label(int f) {
    switch (f) {
        case WIZ_F_NAME:      return "Name";
        case WIZ_F_CAN_ID:    return "CAN ID";
        case WIZ_F_START_BIT: return "Start Bit";
        case WIZ_F_LENGTH:    return "Length";
        case WIZ_F_SCALE:     return "Scale";
        case WIZ_F_OFFSET:    return "Offset";
        case WIZ_F_UNIT:      return "Unit";
    }
    return "?";
}

static void _wiz_format_preview(int f, char *buf, size_t bufsz) {
    switch (f) {
        case WIZ_F_NAME:
            snprintf(buf, bufsz, "%s",
                     s_wiz_form.name[0] ? s_wiz_form.name : "(empty)");
            break;
        case WIZ_F_CAN_ID:
            snprintf(buf, bufsz, "0x%X", (unsigned)s_wiz_form.can_id);
            break;
        case WIZ_F_START_BIT:
            snprintf(buf, bufsz, "%u", (unsigned)s_wiz_form.start_bit);
            break;
        case WIZ_F_LENGTH:
            snprintf(buf, bufsz, "%u", (unsigned)s_wiz_form.length);
            break;
        case WIZ_F_SCALE:
            snprintf(buf, bufsz, "%g", (double)s_wiz_form.scale);
            break;
        case WIZ_F_OFFSET:
            snprintf(buf, bufsz, "%g", (double)s_wiz_form.offset);
            break;
        case WIZ_F_UNIT:
            snprintf(buf, bufsz, "%s",
                     s_wiz_form.unit[0] ? s_wiz_form.unit : "(none)");
            break;
        default:
            if (bufsz) buf[0] = '\0';
    }
}

static void _wiz_refresh_field(int f) {
    if (f < 0 || f >= WIZ_F_COUNT) return;
    if (!s_wiz_field_lbls[f] || !lv_obj_is_valid(s_wiz_field_lbls[f])) return;
    char buf[32];
    _wiz_format_preview(f, buf, sizeof(buf));
    lv_label_set_text(s_wiz_field_lbls[f], buf);
}

static void _wiz_set_status(const char *msg, bool error) {
    if (!s_wiz_status_lbl || !lv_obj_is_valid(s_wiz_status_lbl)) return;
    lv_label_set_text(s_wiz_status_lbl, msg ? msg : "");
    lv_obj_set_style_text_color(s_wiz_status_lbl,
        error ? DT_DANGER : DT_TEXT_MUTED, 0);
}

static void _wiz_close(void) {
    if (s_wiz_popup && lv_obj_is_valid(s_wiz_popup)) lv_obj_del(s_wiz_popup);
    s_wiz_popup       = NULL;
    s_wiz_card        = NULL;
    s_wiz_title       = NULL;
    s_wiz_subtitle    = NULL;
    s_wiz_body        = NULL;
    s_wiz_btn_row     = NULL;
    s_wiz_status_lbl  = NULL;
    for (int i = 0; i < WIZ_F_COUNT; i++) s_wiz_field_lbls[i] = NULL;
}

static void _wiz_set_defaults(void) {
    memset(&s_wiz_form, 0, sizeof(s_wiz_form));
    s_wiz_form.scale  = 1.0f;
    s_wiz_form.offset = 0.0f;
    s_wiz_form.length = 8;
}

static void _wiz_text_confirm_cb(const char *text, void *user_data) {
    int f = (int)(intptr_t)user_data;
    if (!text) return;
    switch (f) {
        case WIZ_F_NAME:
            strncpy(s_wiz_form.name, text, sizeof(s_wiz_form.name) - 1);
            s_wiz_form.name[sizeof(s_wiz_form.name) - 1] = '\0';
            break;
        case WIZ_F_CAN_ID:
            s_wiz_form.can_id = (uint32_t)strtoul(text, NULL, 0);
            break;
        case WIZ_F_SCALE:
            s_wiz_form.scale = (float)atof(text);
            break;
        case WIZ_F_OFFSET:
            s_wiz_form.offset = (float)atof(text);
            break;
        case WIZ_F_UNIT:
            strncpy(s_wiz_form.unit, text, sizeof(s_wiz_form.unit) - 1);
            s_wiz_form.unit[sizeof(s_wiz_form.unit) - 1] = '\0';
            break;
        default:
            return;
    }
    _wiz_refresh_field(f);
}

static void _wiz_numeric_confirm_cb(const char *text, void *user_data) {
    int f = (int)(intptr_t)user_data;
    if (!text) return;
    int v = atoi(text);
    switch (f) {
        case WIZ_F_START_BIT:
            if (v < 0)  v = 0;
            if (v > 63) v = 63;
            s_wiz_form.start_bit = (uint8_t)v;
            break;
        case WIZ_F_LENGTH:
            if (v < 1)  v = 1;
            if (v > 32) v = 32;
            s_wiz_form.length = (uint8_t)v;
            break;
        default:
            return;
    }
    _wiz_refresh_field(f);
}

static void _wiz_field_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    char buf[32];
    _wiz_format_preview(f, buf, sizeof(buf));

    if (f == WIZ_F_START_BIT || f == WIZ_F_LENGTH) {
        show_numeric_input_dialog(_wiz_field_label(f), buf,
                                  _wiz_numeric_confirm_cb, NULL,
                                  (void *)(intptr_t)f);
    } else {
        _ensure_hidden_textarea();
        if (!s_hidden_text_ta) return;
        lv_textarea_set_text(s_hidden_text_ta, buf);
        show_text_input_dialog_ex(s_hidden_text_ta, _wiz_field_label(f),
                                  "", false,
                                  _wiz_text_confirm_cb, NULL,
                                  (void *)(intptr_t)f);
    }
}

static void _wiz_cancel_cb(lv_event_t *e) {
    (void)e;
    _wiz_close();
}

static void _wiz_save_cb(lv_event_t *e) {
    (void)e;
    if (s_wiz_form.name[0] == '\0') {
        _wiz_set_status("Name is required.", true);
        return;
    }
    if (s_wiz_form.length == 0) {
        _wiz_set_status("Length must be at least 1.", true);
        return;
    }

    /* Defaults: Intel endian, unsigned. User can refine via web editor. */
    int16_t idx = signal_register(s_wiz_form.name, s_wiz_form.can_id,
                                   s_wiz_form.start_bit, s_wiz_form.length,
                                   s_wiz_form.scale, s_wiz_form.offset,
                                   false, 1,
                                   s_wiz_form.unit);
    if (idx < 0) {
        _wiz_set_status("Could not register (duplicate name?)", true);
        return;
    }

    /* Append (or replace) in the persistent library at /lfs/dbc/user.json
     * so this signal is reusable in future layouts. */
    user_signal_t persistable = {
        .can_id    = s_wiz_form.can_id,
        .start_bit = s_wiz_form.start_bit,
        .length    = s_wiz_form.length,
        .scale     = s_wiz_form.scale,
        .offset    = s_wiz_form.offset,
        .is_signed = false,
        .endian    = 1,
    };
    strncpy(persistable.name, s_wiz_form.name, sizeof(persistable.name) - 1);
    strncpy(persistable.unit, s_wiz_form.unit, sizeof(persistable.unit) - 1);
    user_signals_append(&persistable);   /* best-effort */

    /* Auto-bind the currently-selected widget. */
    if (s_widget && s_widget->inspector_set) {
        widget_field_value_t v = { .str = s_wiz_form.name };
        s_widget->inspector_set(s_widget, "signal_name", &v);
    }

    _wiz_close();
    _close_signal_picker();   /* back to DATA tab with new binding */
}

static void _make_wiz_row(lv_obj_t *parent, int field) {
    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 38);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, DT_BG_INSET, 0);
    lv_obj_set_style_bg_opa(row, 60, 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, 30, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_add_event_cb(row, _wiz_field_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)field);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, _wiz_field_label(field));
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_color(chev, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(chev, THEME_FONT_SMALL, 0);

    lv_obj_t *value = lv_label_create(row);
    char buf[32];
    _wiz_format_preview(field, buf, sizeof(buf));
    lv_label_set_text(value, buf);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value, 320);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_color(value, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(value, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    s_wiz_field_lbls[field] = value;
}

static void _open_signal_wizard(void) {
    _wiz_close();
    _wiz_set_defaults();

    s_wiz_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_wiz_popup, 800, 480);
    lv_obj_set_pos(s_wiz_popup, 0, 0);
    lv_obj_set_style_bg_color(s_wiz_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wiz_popup, 110, 0);
    lv_obj_set_style_border_width(s_wiz_popup, 0, 0);
    lv_obj_set_style_radius(s_wiz_popup, 0, 0);
    lv_obj_set_style_pad_all(s_wiz_popup, 0, 0);
    lv_obj_clear_flag(s_wiz_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wiz_popup, LV_OBJ_FLAG_CLICKABLE);

    s_wiz_card = lv_obj_create(s_wiz_popup);
    lv_obj_set_size(s_wiz_card, 640, 440);
    lv_obj_center(s_wiz_card);
    lv_obj_set_style_bg_color(s_wiz_card, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(s_wiz_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_wiz_card, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(s_wiz_card, 1, 0);
    lv_obj_set_style_radius(s_wiz_card, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(s_wiz_card, 16, 0);
    lv_obj_set_style_pad_row(s_wiz_card, 8, 0);
    lv_obj_clear_flag(s_wiz_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_wiz_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wiz_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_wiz_title = lv_label_create(s_wiz_card);
    lv_label_set_text(s_wiz_title, "Create signal manually");
    lv_obj_set_style_text_color(s_wiz_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_wiz_title, THEME_FONT_LARGE, 0);

    s_wiz_subtitle = lv_label_create(s_wiz_card);
    lv_label_set_long_mode(s_wiz_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wiz_subtitle, 600);
    lv_label_set_text(s_wiz_subtitle,
        "Defines a new CAN signal from scratch. Saved to your signal "
        "library at /lfs/dbc/user.json so future layouts can pick it.");
    lv_obj_set_style_text_color(s_wiz_subtitle, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_wiz_subtitle, THEME_FONT_SMALL, 0);

    /* Scrollable body container holding the form rows. */
    s_wiz_body = lv_obj_create(s_wiz_card);
    lv_obj_set_width(s_wiz_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_wiz_body, 1);
    lv_obj_set_style_bg_opa(s_wiz_body, 0, 0);
    lv_obj_set_style_border_width(s_wiz_body, 0, 0);
    lv_obj_set_style_pad_all(s_wiz_body, 0, 0);
    lv_obj_set_style_pad_row(s_wiz_body, 4, 0);
    lv_obj_add_flag(s_wiz_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_wiz_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wiz_body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(s_wiz_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wiz_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (int f = 0; f < WIZ_F_COUNT; f++) _make_wiz_row(s_wiz_body, f);

    s_wiz_status_lbl = lv_label_create(s_wiz_card);
    lv_label_set_text(s_wiz_status_lbl, "");
    lv_obj_set_style_text_color(s_wiz_status_lbl, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_wiz_status_lbl, THEME_FONT_SMALL, 0);

    s_wiz_btn_row = lv_obj_create(s_wiz_card);
    lv_obj_set_size(s_wiz_btn_row, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(s_wiz_btn_row, 0, 0);
    lv_obj_set_style_border_width(s_wiz_btn_row, 0, 0);
    lv_obj_set_style_pad_all(s_wiz_btn_row, 0, 0);
    lv_obj_clear_flag(s_wiz_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = _make_button(s_wiz_btn_row, 120, 40, "Cancel");
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(cancel, _wiz_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save = _make_button(s_wiz_btn_row, 120, 40, "Save");
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save, DT_ACCENT, 0);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(save, _wiz_save_cb, LV_EVENT_CLICKED, NULL);
}

/* Generic tab builder - iterates the schema and dispatches per field
 * type. Categories filter what shows up where (STYLE = appearance,
 * DATA = data, RULES = alerts). Fields whose widget doesn't implement
 * inspector_get yet still appear, but their initial value comes from
 * the schema default rather than the live widget. */

static void _build_schema_tab(widget_t *w, widget_field_category_t cat) {
    if (!w || !w->inspector_get || !w->inspector_set) return;
    const widget_fields_def_t *def = widget_fields_for_type(w->type);
    if (!def) return;

    _ensure_presets();
    s_row_count = 0;
    s_color_row_ctx_count = 0;
    s_int_row_ctx_count = 0;
    s_bool_row_ctx_count = 0;
    s_select_row_ctx_count = 0;
    s_text_row_ctx_count = 0;
    s_sig_row_value_lbl = NULL;

    /* DATA tab leads with the per-widget signal binding row, then the
     * data-category schema fields below it. A layout is just a flat
     * signals list - we don't surface a "layout's ECU" concept here. */
    if (cat == WF_CAT_DATA && _widget_supports_signal_binding()) {
        lv_obj_t *sig_card = _make_card(s_content, "SIGNAL");
        _build_signal_row(sig_card);
    }

    lv_obj_t *colours = NULL;
    lv_obj_t *dims    = NULL;
    lv_obj_t *generic = NULL;

    /* Pick a default-card title based on category so the DATA tab card
     * doesn't read "DIMENSIONS" or "COLOURS" when it isn't either. */
    const char *generic_title =
        (cat == WF_CAT_DATA)        ? "DATA" :
        (cat == WF_CAT_ALERTS)      ? "ALERTS" :
        (cat == WF_CAT_THRESHOLDS)  ? "THRESHOLDS" : "FIELDS";

    for (uint16_t i = 0; i < def->field_count; i++) {
        const widget_field_t *f = &def->fields[i];
        if (f->category != cat) continue;

        switch (f->type) {
            case WF_TYPE_COLOR: {
                if (!colours) colours = _make_card(s_content, "COLOURS");
                _make_color_row_schema(colours, f);
                break;
            }
            case WF_TYPE_STEPPER:
            case WF_TYPE_STEPPER_AUTO:
            case WF_TYPE_SLIDER: {
                /* In STYLE the slider rows are "DIMENSIONS"; elsewhere they
                 * sit in the same card as the rest of the category. */
                if (cat == WF_CAT_APPEARANCE) {
                    if (!dims) dims = _make_card(s_content, "DIMENSIONS");
                    _make_stepper_row_schema(dims, f);
                } else {
                    if (!generic) generic = _make_card(s_content, generic_title);
                    _make_stepper_row_schema(generic, f);
                }
                break;
            }
            case WF_TYPE_CHECKBOX: {
                if (!generic) generic = _make_card(s_content, generic_title);
                _make_checkbox_row_schema(generic, f);
                break;
            }
            case WF_TYPE_SELECT: {
                if (!generic) generic = _make_card(s_content, generic_title);
                _make_select_row_schema(generic, f);
                break;
            }
            case WF_TYPE_TEXT:
            case WF_TYPE_TEXTAREA:
            case WF_TYPE_FONT: {
                if (!generic) generic = _make_card(s_content, generic_title);
                _make_text_row_schema(generic, f, false);
                break;
            }
            case WF_TYPE_NUMBER: {
                if (!generic) generic = _make_card(s_content, generic_title);
                _make_text_row_schema(generic, f, true);
                break;
            }
            default:
                /* IMAGE_PICKER / CAN_ID rendered in a later phase. */
                break;
        }
    }
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
    lv_obj_set_width(msg, s_dock_w - 80);
    lv_label_set_text(msg,
        "Position + size live in the bottom toolbar. "
        "For the full per-widget settings list, use the legacy "
        "editor or the web editor.");
    lv_obj_set_style_text_color(msg, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(msg, THEME_FONT_SMALL, 0);

    lv_obj_t *legacy_btn = _make_button(card, s_dock_w - 60, 40,
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

    /* Switch dock width to this tab's preference. Snap (no animation
     * yet) so the content area is at its final width before we rebuild. */
    int target_w = s_tab_dock_w[idx];
    if (target_w != s_dock_w) {
        s_dock_w = target_w;
        _apply_dock_geometry();
    }

    lv_obj_clean(s_content);
    switch (idx) {
        case TAB_DATA:
            if (s_widget && s_widget->inspector_get && s_widget->inspector_set &&
                widget_fields_for_type(s_widget->type))
                _build_schema_tab(s_widget, WF_CAT_DATA);
            else
                _build_placeholder_tab("Data");
            break;
        case TAB_STYLE:
            /* Schema-driven path - any widget that implements inspector_get
             * + inspector_set gets a fully-rendered STYLE tab from
             * schema/widgets.schema.json. Widgets without the hooks fall
             * through to the legacy editor placeholder. */
            if (s_widget && s_widget->inspector_get && s_widget->inspector_set &&
                widget_fields_for_type(s_widget->type))
                _build_schema_tab(s_widget, WF_CAT_APPEARANCE);
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
    s_header = header;
    lv_obj_set_size(header, s_dock_w, 48);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_90, 0);
    lv_obj_set_style_border_color(header, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    /* "Back" text instead of an arrow - clearer label, same affordance. */
    lv_obj_t *back = _make_button(header, 64, 36, "Back");
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, _back_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Side switcher - two small arrow buttons on the far right of the
     * header. The currently-docked side is highlighted; tapping the
     * other side moves the dock to that edge. Useful when the widget
     * being edited is on the same side as the dock. */
    s_side_right_btn = _make_button(header, 32, 32, LV_SYMBOL_RIGHT);
    lv_obj_align(s_side_right_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(s_side_right_btn, _side_right_clicked_cb,
                        LV_EVENT_CLICKED, NULL);

    s_side_left_btn = _make_button(header, 32, 32, LV_SYMBOL_LEFT);
    lv_obj_align(s_side_left_btn, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_obj_add_event_cb(s_side_left_btn, _side_left_clicked_cb,
                        LV_EVENT_CLICKED, NULL);

    /* Title between Back and the side switcher. THEME_FONT_BODY (14 px)
     * fits even the longest widget-type name ("Shift Light") in the
     * available width. */
    lv_obj_t *title = lv_label_create(header);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %u",
             _widget_type_name(s_widget ? s_widget->type : (widget_type_t)0),
             (unsigned)(s_widget ? s_widget->slot : 0));
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, THEME_FONT_BODY, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 80, 0);
}

static void _build_tab_bar(void) {
    lv_obj_t *bar = lv_obj_create(s_overlay);
    s_tab_bar = bar;
    lv_obj_set_size(bar, s_dock_w, 40);
    lv_obj_set_pos(bar, 0, 48);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_border_color(bar, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    int tw = _tab_width();
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *b = lv_btn_create(bar);
        lv_obj_set_size(b, tw, 40);
        lv_obj_set_pos(b, i * tw, 0);
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
    lv_obj_set_size(s_tab_indicator, tw, 2);
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
    lv_obj_set_size(s_content, s_dock_w, DOCK_H - 48 - 40);
    lv_obj_set_pos(s_content, 0, 48 + 40);
    /* Transparent - gaps between cards let the dashboard show through. */
    lv_obj_set_style_bg_opa(s_content, 0, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 8, 0);
    lv_obj_set_style_pad_row(s_content, 10, 0);
    /* Vertical scroll - now that the STYLE tab grows with every schema
     * field, the content reliably overflows the dock height. Cards stay
     * non-scrollable so scrolling happens at the content level rather
     * than per-card (smoother flicks, single scroll surface).  */
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_AUTO);
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
    s_dock_side  = 0;   /* always reopen on the right side */
    s_dock_w     = s_tab_dock_w[s_active_tab];   /* width for the default tab */

    lv_obj_t *parent = ui_Screen3;
    if (!parent || !lv_obj_is_valid(parent)) parent = lv_scr_act();
    if (!parent) return;

    /* Click-eater over the dashboard area. Invisible (the dash stays at
     * full brightness) but absorbs taps so edit_mode's selection / drag
     * gesture handlers don't fire while the Inspector is up. */
    s_left_eater = lv_obj_create(parent);
    lv_obj_set_size(s_left_eater, _dock_x(), DOCK_H);
    lv_obj_set_pos(s_left_eater, 0, 0);
    lv_obj_set_style_bg_opa(s_left_eater, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_left_eater, 0, 0);
    lv_obj_set_style_radius(s_left_eater, 0, 0);
    lv_obj_set_style_pad_all(s_left_eater, 0, 0);
    lv_obj_clear_flag(s_left_eater, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_left_eater, LV_OBJ_FLAG_CLICKABLE);

    /* Right-side dock - the visible Inspector. */
    s_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_overlay, s_dock_w, DOCK_H);
    lv_obj_set_pos(s_overlay, _dock_x(), 0);
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
    _apply_dock_side();

    lv_obj_move_foreground(s_left_eater);
    lv_obj_move_foreground(s_overlay);
    ui_Screen3_refresh_overlays();

    ESP_LOGI(TAG, "Opened for %s slot %u",
             _widget_type_name(w->type), (unsigned)w->slot);
}

void inspector_close(void) {
    _close_picker();
    _close_wheel();
    _wiz_close();
    if (s_overlay) {
        if (lv_obj_is_valid(s_overlay)) lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    if (s_left_eater) {
        if (lv_obj_is_valid(s_left_eater)) lv_obj_del(s_left_eater);
        s_left_eater = NULL;
    }
    s_widget             = NULL;
    s_content            = NULL;
    s_header             = NULL;
    s_tab_bar            = NULL;
    s_tab_indicator      = NULL;
    s_side_left_btn      = NULL;
    s_side_right_btn     = NULL;
    s_sig_row_value_lbl  = NULL;
    s_in_signal_picker   = false;
    for (int i = 0; i < TAB_COUNT; i++) s_tab_buttons[i] = NULL;

    edit_mode_commit_external_edit();

    ESP_LOGI(TAG, "Closed");
}

bool inspector_is_open(void) {
    return s_overlay != NULL && lv_obj_is_valid(s_overlay);
}
