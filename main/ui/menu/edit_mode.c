/*
 * edit_mode.c — see edit_mode.h.
 *
 * Pill positioning: 100×36 at LV_ALIGN_TOP_RIGHT, offset -120/+12. That sits
 * the right edge at x=680 — 8 px gap to the Menu button (which lives at
 * -12/+12 with right edge at x=788, left edge x=688). Both pills share the
 * same vertical baseline and rounded-rect aesthetic; only the colour signals
 * which is which.
 *
 * Banner: 28 px tall, anchored bottom of the parent screen, full width,
 * solid DT_DANGER with white text. Lives on the same parent as the pill so
 * a layout reload (which deletes ui_Screen3) automatically tears it down.
 */
#include "ui/menu/edit_mode.h"
#include "ui/menu/design_tokens.h"
#include "ui/menu/menu_screen.h"  /* load_menu_screen_for_widget */
#include "ui/dashboard.h"         /* dashboard_persist_layout */
#include "ui/theme.h"
#include "ui/ui.h"                /* ui_Menu_Button extern */
#include "storage/config_store.h" /* edit step px persistence */
#include "esp_log.h"
#include <stdlib.h>               /* abs() */
#include <stdint.h>               /* intptr_t */

static const char *TAG = "edit_mode";

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void _destroy_toolbar(void);
static void _update_readout(void);
static void _refresh_step_styling(void);

/* ── Module state ─────────────────────────────────────────────────────────── */

static bool       s_armed     = false;
static lv_obj_t  *s_pill      = NULL;
static lv_obj_t  *s_pill_lbl  = NULL;
static lv_obj_t  *s_banner    = NULL;

/* Selection + drag (active only while armed) */
static widget_t  *s_selected            = NULL;
static lv_obj_t  *s_ring                = NULL;
static lv_point_t s_drag_start_pt       = {0, 0};
static int16_t    s_drag_start_widget_x = 0;
static int16_t    s_drag_start_widget_y = 0;
static bool       s_dragging            = false;

/* Two-stage selection: the first press on an unselected widget only selects
 * it (drag suppressed). A subsequent press on the already-selected widget
 * arms drag for that press. Prevents accidental drag-on-tap when the user
 * really just wanted to inspect. */
static bool       s_drag_enabled_this_press = false;

/* Toolbar (shown when a widget is selected; replaces banner) */
static lv_obj_t  *s_toolbar       = NULL;
static lv_obj_t  *s_step_btns[3]  = {NULL};
static lv_obj_t  *s_readout_lbl   = NULL;

/* Nudge / drag-snap step (px). One of 1, 5, 10. Loaded from NVS on first
 * pill creation; written back when the user changes it. */
static int8_t     s_step          = 5;
static bool       s_step_loaded   = false;

/* Debounced save timer — coalesces a burst of nudges / drags into one
 * LittleFS write. */
static lv_timer_t *s_save_timer   = NULL;
#define SAVE_DEBOUNCE_MS 600

/* Movement above this threshold (px) promotes a press into a drag. Below it,
 * the press registers as a tap (selection only). Matches the 6 px gestural
 * dead-zone in the web editor. */
#define DRAG_THRESHOLD_PX 6

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static void _apply_pill_style_live(void) {
    if (!s_pill || !lv_obj_is_valid(s_pill)) return;
    lv_obj_set_style_bg_color(s_pill, THEME_COLOR_BTN_NEUTRAL, 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_60, 0);
    lv_obj_set_style_border_color(s_pill, DT_BORDER_LIGHT, 0);
    lv_obj_set_style_border_width(s_pill, 1, 0);
    if (s_pill_lbl && lv_obj_is_valid(s_pill_lbl)) {
        lv_label_set_text(s_pill_lbl, LV_SYMBOL_EDIT "  Edit Mode");
        lv_obj_set_style_text_color(s_pill_lbl, DT_TEXT_PRIMARY, 0);
    }
}

static void _apply_pill_style_armed(void) {
    if (!s_pill || !lv_obj_is_valid(s_pill)) return;
    lv_obj_set_style_bg_color(s_pill, DT_DANGER, 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_pill, 0, 0);
    if (s_pill_lbl && lv_obj_is_valid(s_pill_lbl)) {
        lv_label_set_text(s_pill_lbl, LV_SYMBOL_CLOSE "  Exit Edit Mode");
        lv_obj_set_style_text_color(s_pill_lbl, lv_color_white(), 0);
    }
}

static void _build_banner(lv_obj_t *parent) {
    if (s_banner && lv_obj_is_valid(s_banner)) return;

    s_banner = lv_obj_create(parent);
    lv_obj_set_size(s_banner, LV_PCT(100), DT_BANNER_H);
    lv_obj_align(s_banner, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_banner, DT_DANGER, 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_radius(s_banner, 0, 0);
    lv_obj_set_style_pad_all(s_banner, 0, 0);

    lv_obj_t *lbl = lv_label_create(s_banner);
    lv_label_set_text(lbl,
        LV_SYMBOL_EDIT "  EDIT MODE  \xC2\xB7  long-press a widget to inspect");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_center(lbl);

    lv_obj_move_foreground(s_banner);
}

static void _destroy_banner(void) {
    if (s_banner && lv_obj_is_valid(s_banner)) {
        lv_obj_del(s_banner);
    }
    s_banner = NULL;
}

/* ── Selection ring ───────────────────────────────────────────────────────── */

static void _build_ring(void) {
    if (s_ring && lv_obj_is_valid(s_ring)) return;
    s_ring = lv_obj_create(lv_layer_top());
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);
    /* Transparent body, accent border only — the widget shows through. */
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_ring, DT_ACCENT, 0);
    lv_obj_set_style_border_width(s_ring, 2, 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ring, 0, 0);
    lv_obj_set_style_pad_all(s_ring, 0, 0);
    lv_obj_set_style_shadow_width(s_ring, 0, 0);
    lv_obj_set_style_outline_width(s_ring, 0, 0);
}

static void _destroy_ring(void) {
    if (s_ring && lv_obj_is_valid(s_ring)) {
        lv_obj_del(s_ring);
    }
    s_ring = NULL;
}

static void _update_ring(void) {
    if (!s_selected || !s_ring) return;
    if (!lv_obj_is_valid(s_selected->root) || !lv_obj_is_valid(s_ring)) return;
    lv_area_t area;
    lv_obj_get_coords(s_selected->root, &area);
    /* Ring sits 2 px outside the widget on every side so the border doesn't
     * obscure pixels inside the widget. Layer-top uses absolute screen
     * coordinates, matching what lv_obj_get_coords returns. */
    lv_obj_set_pos(s_ring,
                   (lv_coord_t)(area.x1 - 2),
                   (lv_coord_t)(area.y1 - 2));
    lv_obj_set_size(s_ring,
                    (lv_coord_t)(lv_area_get_width(&area) + 4),
                    (lv_coord_t)(lv_area_get_height(&area) + 4));
}

static void _clear_selection(void) {
    s_selected                = NULL;
    s_dragging                = false;
    s_drag_enabled_this_press = false;
    _destroy_ring();
    _destroy_toolbar();
    /* Re-show the no-selection banner so the user has feedback that they're
     * still in edit mode. Only rebuild while still armed — exit paths
     * destroy the banner explicitly afterwards. */
    if (s_armed && s_pill) {
        lv_obj_t *parent = lv_obj_get_parent(s_pill);
        if (parent) _build_banner(parent);
    }
}

/* ── Layout persistence — debounced ───────────────────────────────────────
 *
 * Rapid edits (nudge button hold-to-repeat, mid-drag releases) would write
 * to LittleFS several times a second otherwise. Debouncing collapses a
 * burst into a single write, sized to feel responsive (~600 ms after the
 * last edit) without thrashing the FS. */

static void _save_timer_cb(lv_timer_t *t) {
    (void)t;
    dashboard_persist_layout();
    if (s_save_timer) { lv_timer_del(s_save_timer); s_save_timer = NULL; }
}

static void _schedule_save(void) {
    if (s_save_timer) {
        lv_timer_reset(s_save_timer);
    } else {
        s_save_timer = lv_timer_create(_save_timer_cb, SAVE_DEBOUNCE_MS, NULL);
        if (s_save_timer) lv_timer_set_repeat_count(s_save_timer, 1);
    }
}

/* ── Decoration clickability ──────────────────────────────────────────────── */
/* Image / shape panel / line widgets are non-clickable in live mode so they
 * don't swallow taps meant for widgets underneath. While armed, flip them
 * clickable so they can be selected and dragged. */
static void _set_decoration_clickable(bool clickable) {
    widget_t **widgets = dashboard_get_widgets();
    uint8_t    count   = dashboard_get_widget_count();
    for (uint8_t i = 0; i < count; i++) {
        widget_t *w = widgets[i];
        if (!w || !w->root || !lv_obj_is_valid(w->root)) continue;
        if (w->type != WIDGET_IMAGE &&
            w->type != WIDGET_SHAPE_PANEL &&
            w->type != WIDGET_LINE) continue;
        if (clickable) lv_obj_add_flag(w->root, LV_OBJ_FLAG_CLICKABLE);
        else           lv_obj_clear_flag(w->root, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ── Toolbar helpers (forward usage by edit_mode_select / _clear_selection) ─ */

static void _update_readout(void) {
    if (!s_readout_lbl || !lv_obj_is_valid(s_readout_lbl)) return;
    if (!s_selected) {
        lv_label_set_text(s_readout_lbl, "tap a widget to select");
        return;
    }
    lv_label_set_text_fmt(s_readout_lbl,
        "x:%4d  y:%4d  w:%4d  h:%4d  \xC2\xB7  tap again to drag",
        s_selected->x, s_selected->y, s_selected->w, s_selected->h);
}

static void _refresh_step_styling(void) {
    static const int8_t values[3] = {1, 5, 10};
    for (int i = 0; i < 3; i++) {
        if (!s_step_btns[i] || !lv_obj_is_valid(s_step_btns[i])) continue;
        bool sel = (values[i] == s_step);
        lv_obj_set_style_bg_color(s_step_btns[i],
                                  sel ? DT_ACCENT : DT_BG_INSET, 0);
        lv_obj_t *lbl = lv_obj_get_child(s_step_btns[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            sel ? lv_color_white() : DT_TEXT_PRIMARY, 0);
    }
}

typedef enum {
    NUDGE_LEFT  = 0,
    NUDGE_UP    = 1,
    NUDGE_DOWN  = 2,
    NUDGE_RIGHT = 3,
} nudge_dir_t;

static void _apply_nudge(nudge_dir_t dir) {
    if (!s_selected || !s_selected->root || !lv_obj_is_valid(s_selected->root)) return;
    int step = (s_step > 0) ? s_step : 5;
    int dx = 0, dy = 0;
    switch (dir) {
        case NUDGE_LEFT:  dx = -step; break;
        case NUDGE_UP:    dy = -step; break;
        case NUDGE_DOWN:  dy = +step; break;
        case NUDGE_RIGHT: dx = +step; break;
    }
    s_selected->x = (int16_t)(s_selected->x + dx);
    s_selected->y = (int16_t)(s_selected->y + dy);
    lv_obj_set_pos(s_selected->root, s_selected->x, s_selected->y);
    _update_ring();
    _update_readout();
    _schedule_save();
}

static void _nudge_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    /* Fire once on initial press + every 100 ms while held (LVGL's
     * LONG_PRESSED_REPEAT cadence). Single tap = single nudge. */
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    nudge_dir_t dir = (nudge_dir_t)(intptr_t)lv_event_get_user_data(e);
    _apply_nudge(dir);
}

static void _step_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v != 1 && v != 5 && v != 10) return;
    s_step = (int8_t)v;
    _refresh_step_styling();
    config_store_save_edit_step_px(s_step);
}

static void _configure_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_selected) return;
    /* Hand off to the existing per-widget config modal. It's the same path
     * the previous long-press gesture used; just driven from an explicit
     * button now so we don't fight LVGL's long-press timer mid-drag. */
    load_menu_screen_for_widget(s_selected);
}

/* ── Toolbar build / destroy ──────────────────────────────────────────────── */

/* Helper: small uniform button with a label centered inside it. */
static lv_obj_t *_make_tbtn(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                            const char *text) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, DT_BG_INSET, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, DT_RADIUS_SM, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, DT_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(l, THEME_FONT_SMALL, 0);
    return b;
}

static void _build_toolbar(lv_obj_t *parent) {
    if (s_toolbar && lv_obj_is_valid(s_toolbar)) return;

    s_toolbar = lv_obj_create(parent);
    lv_obj_set_size(s_toolbar, 720, 56);
    lv_obj_align(s_toolbar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_clear_flag(s_toolbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_toolbar, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(s_toolbar, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_toolbar, DT_BORDER_DARK, 0);
    lv_obj_set_style_border_width(s_toolbar, 1, 0);
    lv_obj_set_style_radius(s_toolbar, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(s_toolbar, 8, 0);
    lv_obj_set_style_pad_column(s_toolbar, 8, 0);
    lv_obj_set_style_shadow_width(s_toolbar, 10, 0);
    lv_obj_set_style_shadow_color(s_toolbar, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_toolbar, LV_OPA_40, 0);
    lv_obj_set_style_shadow_ofs_y(s_toolbar, 2, 0);

    lv_obj_set_flex_flow(s_toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_toolbar,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Nudge cluster — 4 buttons, tight 2px gaps */
    lv_obj_t *nudges = lv_obj_create(s_toolbar);
    lv_obj_set_size(nudges, 156, 40);
    lv_obj_set_style_bg_opa(nudges, 0, 0);
    lv_obj_set_style_border_width(nudges, 0, 0);
    lv_obj_set_style_pad_all(nudges, 0, 0);
    lv_obj_set_style_pad_column(nudges, 2, 0);
    lv_obj_clear_flag(nudges, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nudges, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nudges, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const nudge_dir_t dirs[4]  = {NUDGE_LEFT, NUDGE_UP, NUDGE_DOWN, NUDGE_RIGHT};
    static const char *const icons[4] = {LV_SYMBOL_LEFT, LV_SYMBOL_UP,
                                         LV_SYMBOL_DOWN, LV_SYMBOL_RIGHT};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = _make_tbtn(nudges, 36, 36, icons[i]);
        lv_obj_add_event_cb(b, _nudge_btn_cb, LV_EVENT_PRESSED,
                            (void *)(intptr_t)dirs[i]);
        lv_obj_add_event_cb(b, _nudge_btn_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                            (void *)(intptr_t)dirs[i]);
    }

    /* Step toggle — 3-segment, shares an outer border */
    lv_obj_t *steps = lv_obj_create(s_toolbar);
    lv_obj_set_size(steps, 132, 36);
    lv_obj_set_style_bg_opa(steps, 0, 0);
    lv_obj_set_style_border_color(steps, DT_BORDER_LIGHT, 0);
    lv_obj_set_style_border_width(steps, 1, 0);
    lv_obj_set_style_radius(steps, DT_RADIUS_SM, 0);
    lv_obj_set_style_pad_all(steps, 0, 0);
    lv_obj_set_style_pad_column(steps, 0, 0);
    lv_obj_set_style_clip_corner(steps, true, 0);
    lv_obj_clear_flag(steps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(steps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(steps, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const int8_t step_values[3] = {1, 5, 10};
    static const char *const step_labels[3] = {"1 px", "5 px", "10 px"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = _make_tbtn(steps, 44, 34, step_labels[i]);
        lv_obj_set_style_radius(b, 0, 0);   /* flush corners inside segment */
        lv_obj_add_event_cb(b, _step_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)step_values[i]);
        s_step_btns[i] = b;
    }
    _refresh_step_styling();

    /* Live readout — fills the middle */
    s_readout_lbl = lv_label_create(s_toolbar);
    lv_obj_set_style_text_color(s_readout_lbl, DT_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(s_readout_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_flex_grow(s_readout_lbl, 1);
    _update_readout();

    /* Configure button — accent-blue, opens the per-widget config modal */
    lv_obj_t *cfg = lv_btn_create(s_toolbar);
    lv_obj_set_size(cfg, 130, 36);
    lv_obj_set_style_bg_color(cfg, DT_ACCENT, 0);
    lv_obj_set_style_bg_color(cfg, DT_ACCENT_HOVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cfg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cfg, DT_RADIUS_SM, 0);
    lv_obj_set_style_border_width(cfg, 0, 0);
    lv_obj_set_style_shadow_width(cfg, 0, 0);
    lv_obj_set_style_pad_all(cfg, 0, 0);
    lv_obj_t *cfg_lbl = lv_label_create(cfg);
    lv_label_set_text(cfg_lbl, LV_SYMBOL_SETTINGS "  Configure");
    lv_obj_set_style_text_color(cfg_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cfg_lbl, THEME_FONT_SMALL, 0);
    lv_obj_center(cfg_lbl);
    lv_obj_add_event_cb(cfg, _configure_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_move_foreground(s_toolbar);
}

static void _destroy_toolbar(void) {
    if (s_toolbar && lv_obj_is_valid(s_toolbar)) lv_obj_del(s_toolbar);
    s_toolbar      = NULL;
    s_readout_lbl  = NULL;
    s_step_btns[0] = s_step_btns[1] = s_step_btns[2] = NULL;
}

/* ── Pill click handler — toggles armed state ─────────────────────────────── */

static void _pill_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_armed) edit_mode_exit();
    else         edit_mode_enter();
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool edit_mode_is_armed(void) { return s_armed; }

void edit_mode_enter(void) {
    if (s_armed) return;
    s_armed = true;

    /* Hide the Menu button — only one toolbar action visible while armed. */
    if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button))
        lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);

    /* Promote decorations (image/shape_panel/line) so they can be selected
     * and dragged just like any other widget. */
    _set_decoration_clickable(true);

    _apply_pill_style_armed();
    if (s_pill && lv_obj_is_valid(s_pill)) {
        lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
        /* Pin to foreground so it stays above the banner / widgets. */
        lv_obj_move_foreground(s_pill);

        /* Build banner on the pill's parent — when the screen reloads, the
         * parent goes with it and we don't leak a dangling banner. */
        lv_obj_t *parent = lv_obj_get_parent(s_pill);
        if (parent) _build_banner(parent);
    }

    ESP_LOGI(TAG, "Edit Mode armed");
}

void edit_mode_exit(void) {
    if (!s_armed && !s_banner && !s_ring && !s_toolbar)
        return;   /* idempotent fast path */
    s_armed = false;
    _clear_selection();   /* destroys ring + toolbar (won't rebuild banner) */
    _set_decoration_clickable(false);
    _apply_pill_style_live();
    if (s_pill && lv_obj_is_valid(s_pill))
        lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    _destroy_banner();
    /* Cancel any pending debounced save — the layout's current state will
     * be re-saved next time the user edits, no rush to flush now. */
    if (s_save_timer) { lv_timer_del(s_save_timer); s_save_timer = NULL; }
    ESP_LOGI(TAG, "Edit Mode exited");
}

lv_obj_t *edit_mode_create_pill(lv_obj_t *parent) {
    if (!parent) return NULL;

    /* If the previous screen was deleted, our cached pointers are stale —
     * forget them and rebuild from scratch on this parent. The ring lives
     * on lv_layer_top (which is NOT deleted with the screen) so we explicitly
     * tear it down here in case a previous session left it dangling. The
     * toolbar lived on the old screen and is already gone with it. */
    if (s_ring && lv_obj_is_valid(s_ring)) lv_obj_del(s_ring);
    s_pill                    = NULL;
    s_pill_lbl                = NULL;
    s_banner                  = NULL;
    s_ring                    = NULL;
    s_toolbar                 = NULL;
    s_readout_lbl             = NULL;
    s_step_btns[0]            = NULL;
    s_step_btns[1]            = NULL;
    s_step_btns[2]            = NULL;
    s_selected                = NULL;
    s_dragging                = false;
    s_drag_enabled_this_press = false;
    s_armed                   = false;   /* exit-on-reload semantics */
    if (s_save_timer) { lv_timer_del(s_save_timer); s_save_timer = NULL; }

    /* Load the user's preferred step from NVS the first time we initialise.
     * Subsequent screen reloads reuse the in-memory value. */
    if (!s_step_loaded) {
        config_store_load_edit_step_px(&s_step);
        s_step_loaded = true;
    }

    s_pill = lv_btn_create(parent);
    lv_obj_set_size(s_pill, DT_PILL_W, DT_PILL_H);
    lv_obj_align(s_pill, LV_ALIGN_TOP_RIGHT,
                 -(DT_PILL_W + DT_PILL_GAP + 12), 12);
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(s_pill, DT_RADIUS_MD, 0);
    lv_obj_set_style_shadow_width(s_pill, 8, 0);
    lv_obj_set_style_shadow_color(s_pill, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_pill, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(s_pill, 2, 0);

    s_pill_lbl = lv_label_create(s_pill);
    lv_obj_set_style_text_font(s_pill_lbl, THEME_FONT_SMALL, 0);
    lv_obj_center(s_pill_lbl);

    _apply_pill_style_live();

    lv_obj_add_event_cb(s_pill, _pill_clicked_cb, LV_EVENT_CLICKED, NULL);

    return s_pill;
}

void edit_mode_show_pill(void) {
    if (!s_pill || !lv_obj_is_valid(s_pill)) return;
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
}

void edit_mode_hide_pill(void) {
    if (s_armed) return;   /* pinned while armed */
    if (!s_pill || !lv_obj_is_valid(s_pill)) return;
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
}

/* ── Selection API ────────────────────────────────────────────────────────── */

widget_t *edit_mode_get_selected(void) { return s_selected; }

void edit_mode_select(widget_t *w) {
    if (!s_armed) return;
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    s_selected = w;
    _build_ring();
    _update_ring();

    /* Swap the no-selection banner for the selection toolbar (nudges, step
     * toggle, live readout, Configure). They share the same bottom anchor,
     * so the swap is visually clean. */
    _destroy_banner();
    if (s_pill) {
        lv_obj_t *parent = lv_obj_get_parent(s_pill);
        if (parent) _build_toolbar(parent);
    }
    _update_readout();
}

void edit_mode_refresh_selection(void) {
    _update_ring();
    _update_readout();
}

/* ── Widget event handlers (attached per-widget by dashboard.c) ──────────── */
/*
 * Three callbacks implement tap-to-select + drag-to-move:
 *
 *  PRESSED   record start touch + widget position, tentatively select.
 *  PRESSING  if moved past DRAG_THRESHOLD_PX, enter drag state and follow
 *            the finger. The widget reposes live so the user sees what
 *            they're doing.
 *  RELEASED  on a drag: snap to s_step grid, schedule debounced save.
 *            on a tap (no drag): selection is already done in PRESSED, no
 *            save needed.
 *
 * All three short-circuit when not armed, so registering them on every
 * widget is cheap in live mode.
 */

void edit_mode_widget_pressed_cb(lv_event_t *e) {
    if (!s_armed) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;

    /* Two-stage selection: drag is only allowed when pressing an
     * already-selected widget. First tap on an unselected widget just
     * selects (drag suppressed) so the user can deliberately commit to a
     * move on the next press. */
    s_drag_enabled_this_press = (s_selected == w);

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_drag_start_pt);
    s_drag_start_widget_x = w->x;
    s_drag_start_widget_y = w->y;
    s_dragging = false;

    edit_mode_select(w);
}

void edit_mode_widget_pressing_cb(lv_event_t *e) {
    if (!s_armed) return;
    if (!s_drag_enabled_this_press) return;   /* first-touch is select-only */
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    if (s_selected != w) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t cur;
    lv_indev_get_point(indev, &cur);
    int dx = (int)cur.x - (int)s_drag_start_pt.x;
    int dy = (int)cur.y - (int)s_drag_start_pt.y;
    if (!s_dragging) {
        if (abs(dx) < DRAG_THRESHOLD_PX && abs(dy) < DRAG_THRESHOLD_PX) return;
        s_dragging = true;
    }

    w->x = (int16_t)(s_drag_start_widget_x + dx);
    w->y = (int16_t)(s_drag_start_widget_y + dy);
    lv_obj_set_pos(w->root, w->x, w->y);
    _update_ring();
    _update_readout();
}

void edit_mode_widget_released_cb(lv_event_t *e) {
    if (!s_armed) return;
    bool was_dragging = s_dragging;
    s_dragging                = false;
    s_drag_enabled_this_press = false;   /* next press starts fresh */

    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    if (!was_dragging) return;   /* tap only — selection already done in PRESSED */

    /* Snap to the user-selected step grid. Integer division truncates toward
     * zero — small bias for negative coordinates that's imperceptible. */
    int step = (s_step > 0) ? s_step : 5;
    w->x = (int16_t)((w->x / step) * step);
    w->y = (int16_t)((w->y / step) * step);
    lv_obj_set_pos(w->root, w->x, w->y);
    _update_ring();
    _update_readout();
    _schedule_save();
}

void edit_mode_screen_pressed_cb(lv_event_t *e) {
    (void)e;
    if (!s_armed) return;
    if (s_selected) _clear_selection();
}
