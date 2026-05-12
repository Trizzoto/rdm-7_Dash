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
#include "ui/dashboard.h"      /* dashboard_persist_layout */
#include "ui/theme.h"
#include "ui/ui.h"             /* ui_Menu_Button extern */
#include "esp_log.h"
#include <stdlib.h>            /* abs() */

static const char *TAG = "edit_mode";

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

/* Movement above this threshold (px) promotes a press into a drag. Below it,
 * the press registers as a tap (selection only). Matches the 6 px gestural
 * dead-zone in the web editor. */
#define DRAG_THRESHOLD_PX 6
/* Drag commit snaps to this grid in pixels. Will become user-selectable
 * (1 / 10) in Phase 2B; hardcoded as a sensible mid-point for now. */
#define DRAG_SNAP_PX      5

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
    s_selected = NULL;
    s_dragging = false;
    _destroy_ring();
}

/* ── Layout persistence shim (LVGL async callback signature) ─────────────── */

static void _persist_async(void *unused) {
    (void)unused;
    dashboard_persist_layout();
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
    if (!s_armed && !s_banner && !s_ring) return;   /* idempotent fast path */
    s_armed = false;
    _clear_selection();
    _set_decoration_clickable(false);
    _apply_pill_style_live();
    if (s_pill && lv_obj_is_valid(s_pill))
        lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    _destroy_banner();
    ESP_LOGI(TAG, "Edit Mode exited");
}

lv_obj_t *edit_mode_create_pill(lv_obj_t *parent) {
    if (!parent) return NULL;

    /* If the previous screen was deleted, our cached pointers are stale —
     * forget them and rebuild from scratch on this parent. The ring lives
     * on lv_layer_top (which is NOT deleted with the screen) so we explicitly
     * tear it down here in case a previous session left it dangling. */
    if (s_ring && lv_obj_is_valid(s_ring)) lv_obj_del(s_ring);
    s_pill     = NULL;
    s_pill_lbl = NULL;
    s_banner   = NULL;
    s_ring     = NULL;
    s_selected = NULL;
    s_dragging = false;
    s_armed    = false;   /* exit-on-reload semantics */

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
}

void edit_mode_refresh_selection(void) { _update_ring(); }

/* ── Widget event handlers (attached per-widget by dashboard.c) ──────────── */
/*
 * Three callbacks implement tap-to-select + drag-to-move:
 *
 *  PRESSED   record start touch + widget position, tentatively select.
 *  PRESSING  if moved past DRAG_THRESHOLD_PX, enter drag state and follow
 *            the finger. The widget reposes live so the user sees what
 *            they're doing.
 *  RELEASED  on a drag: snap to DRAG_SNAP_PX grid, schedule async save.
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
}

void edit_mode_widget_released_cb(lv_event_t *e) {
    if (!s_armed) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    if (!s_dragging) return;   /* tap only — selection already done in PRESSED */
    s_dragging = false;

    /* Snap to grid. Integer division truncates toward zero — small bias for
     * negative coordinates that's imperceptible at 5 px granularity. */
    w->x = (int16_t)((w->x / DRAG_SNAP_PX) * DRAG_SNAP_PX);
    w->y = (int16_t)((w->y / DRAG_SNAP_PX) * DRAG_SNAP_PX);
    lv_obj_set_pos(w->root, w->x, w->y);
    _update_ring();

    /* Defer the save so it runs after the current touch event finishes and
     * LVGL's invalidation has settled. dashboard_persist_layout writes JSON
     * to LittleFS (~tens of ms) — running it inline would visibly stutter
     * the release animation. */
    lv_async_call(_persist_async, NULL);
}

void edit_mode_screen_pressed_cb(lv_event_t *e) {
    (void)e;
    if (!s_armed) return;
    if (s_selected) _clear_selection();
}
