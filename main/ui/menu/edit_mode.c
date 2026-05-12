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
#include "ui/callbacks/ui_callbacks.h"   /* show_numeric_input_dialog */
#include "ui/dashboard.h"         /* dashboard_persist_layout */
#include "ui/screens/ui_Screen3.h" /* ui_Screen3_refresh_overlays (badge) */
#include "ui/theme.h"
#include "ui/ui.h"                /* ui_Menu_Button, ui_Screen3 externs */
#include "layout/layout_manager.h"  /* build_json for undo snapshots */
#include "storage/config_store.h" /* edit step px persistence */
#include "esp_heap_caps.h"        /* PSRAM allocs for undo ring */
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>                /* snprintf */
#include <stdlib.h>               /* abs, atoi */
#include <stdint.h>               /* intptr_t */
#include <string.h>               /* strlen */

static const char *TAG = "edit_mode";

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void _destroy_toolbar(void);
static void _update_readout(void);
static void _refresh_step_styling(void);
static void _close_popover(void);
static void _update_popover(void);
static void _open_popover(char target);
static void _chip_clicked_cb(lv_event_t *e);
static void _build_handles(void);
static void _destroy_handles(void);
static void _update_handles(void);
static void _handle_pressed_cb(lv_event_t *e);
static void _handle_pressing_cb(lv_event_t *e);
static void _handle_released_cb(lv_event_t *e);
static void _schedule_save(void);
static void _update_ring(void);
static void _clear_selection(void);
static void _close_delete_modal(void);
static void _build_top_toolbar(lv_obj_t *parent);
static void _destroy_top_toolbar(void);
static void _refresh_undo_redo_styling(void);
static void _undo_snapshot(void);
static void _do_undo(void);
static void _do_redo(void);
static void _do_duplicate(void);
static lv_obj_t *_make_tbtn(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                            const char *text);

/* ── Module state ─────────────────────────────────────────────────────────── */

static bool       s_armed     = false;
static lv_obj_t  *s_pill      = NULL;
static lv_obj_t  *s_pill_lbl  = NULL;
static lv_obj_t  *s_banner    = NULL;

/* Selection + drag (active only while armed) */
static widget_t  *s_selected            = NULL;
static lv_obj_t  *s_ring                = NULL;
static lv_obj_t  *s_handles[8]          = {NULL};   /* NW N NE W E SW S SE */
static lv_point_t s_drag_start_pt       = {0, 0};
static int16_t    s_drag_start_widget_x = 0;
static int16_t    s_drag_start_widget_y = 0;
static bool       s_dragging            = false;

/* Resize state — separate from widget drag so a resize gesture never gets
 * confused with a move gesture (the user is pressing a handle, not the
 * widget body). */
static lv_point_t s_resize_start_pt     = {0, 0};
static int16_t    s_resize_start_x      = 0;
static int16_t    s_resize_start_y      = 0;
static uint16_t   s_resize_start_w      = 0;
static uint16_t   s_resize_start_h      = 0;
static int8_t     s_resize_dir          = -1;       /* index into s_handles */
static bool       s_resizing            = false;

/* Two-stage selection: the first press on an unselected widget only selects
 * it (drag suppressed). A subsequent press on the already-selected widget
 * arms drag for that press. Prevents accidental drag-on-tap when the user
 * really just wanted to inspect. */
static bool       s_drag_enabled_this_press = false;

/* Toolbar (shown when a widget is selected; replaces banner) */
static lv_obj_t  *s_toolbar       = NULL;
static lv_obj_t  *s_step_btns[3]  = {NULL};
static lv_obj_t  *s_chip_btns[4]  = {NULL};   /* x, y, w, h — clickable */

/* Adjustment popover — opens above the toolbar when a chip is tapped.
 * Lets the user drag a slider OR tap the value to open the numeric keypad.
 * Stays compact (280x80) so widgets remain visible behind it. */
static lv_obj_t  *s_chip_popover   = NULL;
static lv_obj_t  *s_popover_slider = NULL;
static lv_obj_t  *s_popover_value  = NULL;
static char       s_popover_target = 0;   /* 'x' / 'y' / 'w' / 'h', or 0 */
static bool       s_popover_syncing= false;  /* re-entry guard for slider sync */

/* Delete confirm modal — full-screen backdrop with a centered confirm panel.
 * Built lazily when the user taps the Delete button on the toolbar. */
static lv_obj_t  *s_delete_modal   = NULL;

/* Top toolbar — global editor actions: Exit / Undo / Redo / Duplicate /
 * Delete. Same dimensions as the bottom toolbar so the two read as a
 * pair. Independently draggable along Y. */
static lv_obj_t  *s_top_toolbar    = NULL;
static lv_obj_t  *s_undo_btn       = NULL;
static lv_obj_t  *s_redo_btn       = NULL;
static lv_obj_t  *s_dup_btn        = NULL;
static lv_obj_t  *s_del_btn        = NULL;
static int16_t    s_top_toolbar_y  = 10;     /* offset from TOP_MID anchor */
static lv_point_t s_tt_drag_pt     = {0, 0};
static int16_t    s_tt_drag_start  = 0;
static bool       s_tt_dragging    = false;

/* Undo ring — JSON-string snapshots of the layout. s_undo_pos points at the
 * snapshot matching the current state; -1 = no snapshots yet. New edits
 * truncate forward history before appending. PSRAM-backed so a 10-deep ring
 * doesn't pressure internal RAM. */
#define UNDO_MAX 10
static char  *s_undo[UNDO_MAX];
static int    s_undo_count        = 0;
static int    s_undo_pos          = -1;
static bool   s_suppress_snapshot = false;   /* set during undo/redo apply */

/* Toolbar vertical position — user-draggable via the grip strip on top.
 * `LV_ALIGN_BOTTOM_MID` is the alignment anchor; the offset is measured
 * upward from the parent's bottom edge. Default -10 leaves a 10 px gap.
 * Persists for the lifetime of the session; resets on reboot.
 * Clamp range covers full screen minus toolbar height (see _build_toolbar). */
static int16_t   s_toolbar_y_off  = -10;
static lv_point_t s_tb_drag_pt    = {0, 0};
static int16_t   s_tb_drag_start_y= 0;
static bool      s_tb_dragging    = false;

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
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_70, 0);   /* see-through */
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
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_70, 0);   /* see-through */
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_radius(s_banner, 0, 0);
    lv_obj_set_style_pad_all(s_banner, 0, 0);

    lv_obj_t *lbl = lv_label_create(s_banner);
    /* Plain ASCII only — Montserrat 12 ships without ·, em-dashes, etc.
     * A missing glyph renders as a hollow square, which looks broken. */
    lv_label_set_text(lbl,
        LV_SYMBOL_EDIT "  EDIT MODE  -  tap a widget to select");
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

/* Selection chrome = the ring + 8 resize handles. Built/destroyed/updated
 * as a unit so they always stay synchronised to the selected widget. */

/* Parent for the ring + handles: same as the toolbars (ui_Screen3) so we can
 * z-stack them with explicit foreground calls. Previously on lv_layer_top,
 * which draws above all screen content — that meant the ring sat on top
 * of the toolbars, which felt wrong. */
static lv_obj_t *_selection_parent(void) {
    if (s_pill && lv_obj_is_valid(s_pill)) return lv_obj_get_parent(s_pill);
    return lv_scr_act();
}

static void _build_ring(void) {
    if (s_ring && lv_obj_is_valid(s_ring)) return;
    lv_obj_t *parent = _selection_parent();
    if (!parent) return;
    s_ring = lv_obj_create(parent);
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

    _build_handles();
}

static void _destroy_ring(void) {
    _destroy_handles();
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
    _update_handles();
}

/* ── Resize handles (8: NW N NE W E SW S SE) ──────────────────────────────
 *
 * Each handle is a small accent-blue square sitting on lv_layer_top,
 * centered on a corner / edge midpoint of the selected widget. When the
 * user presses + drags one, the widget resizes from the opposite anchor:
 *
 *   E handle: right edge follows the finger; left edge stays put.
 *   N handle: top edge follows the finger; bottom stays put.
 *   NE corner: both right + top follow the finger; left + bottom anchor.
 *   ...etc.
 *
 * The math: under LV_ALIGN_CENTER, widget.x is the center offset from
 * screen center. To "anchor" one edge while moving the opposite, both
 * size AND center must shift. Concretely, dragging east by dx pixels
 * gives new_w = old_w + dx, new_x = old_x + dx/2 (center moves half the
 * size delta toward the moving edge). Same pattern for all directions. */

/* Order matches s_handles[] index */
enum { H_NW=0, H_N, H_NE, H_W, H_E, H_SW, H_S, H_SE };

#define HANDLE_VIS  12   /* visible square size */
#define HANDLE_HIT   8   /* invisible padding on each side -> 28px hit area */
#define RESIZE_THRESHOLD_PX 3

static void _build_handles(void) {
    lv_obj_t *parent = _selection_parent();
    if (!parent) return;
    for (int i = 0; i < 8; i++) {
        if (s_handles[i] && lv_obj_is_valid(s_handles[i])) continue;
        lv_obj_t *h = lv_obj_create(parent);
        lv_obj_set_size(h, HANDLE_VIS, HANDLE_VIS);
        lv_obj_set_ext_click_area(h, HANDLE_HIT);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(h, DT_ACCENT, 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(h, lv_color_white(), 0);
        lv_obj_set_style_border_width(h, 1, 0);
        lv_obj_set_style_border_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(h, 2, 0);
        lv_obj_set_style_pad_all(h, 0, 0);
        lv_obj_set_style_shadow_width(h, 0, 0);
        lv_obj_add_event_cb(h, _handle_pressed_cb,  LV_EVENT_PRESSED,
                            (void *)(intptr_t)i);
        lv_obj_add_event_cb(h, _handle_pressing_cb, LV_EVENT_PRESSING,
                            (void *)(intptr_t)i);
        lv_obj_add_event_cb(h, _handle_released_cb, LV_EVENT_RELEASED,
                            (void *)(intptr_t)i);
        s_handles[i] = h;
    }
}

static void _destroy_handles(void) {
    for (int i = 0; i < 8; i++) {
        if (s_handles[i] && lv_obj_is_valid(s_handles[i])) lv_obj_del(s_handles[i]);
        s_handles[i] = NULL;
    }
}

static void _update_handles(void) {
    if (!s_selected || !s_selected->root || !lv_obj_is_valid(s_selected->root)) return;
    lv_area_t a;
    lv_obj_get_coords(s_selected->root, &a);
    int cx = (a.x1 + a.x2) / 2;
    int cy = (a.y1 + a.y2) / 2;
    /* Anchor points = corner/edge of widget bounds */
    int px[8] = { a.x1, cx,   a.x2, a.x1, a.x2, a.x1, cx,   a.x2 };
    int py[8] = { a.y1, a.y1, a.y1, cy,   cy,   a.y2, a.y2, a.y2 };
    for (int i = 0; i < 8; i++) {
        if (!s_handles[i] || !lv_obj_is_valid(s_handles[i])) continue;
        /* Position so the handle is centered on the anchor point. */
        lv_obj_set_pos(s_handles[i],
                       (lv_coord_t)(px[i] - HANDLE_VIS / 2),
                       (lv_coord_t)(py[i] - HANDLE_VIS / 2));
    }
}

static void _handle_pressed_cb(lv_event_t *e) {
    if (!s_armed || !s_selected) return;
    s_resize_dir = (int8_t)(intptr_t)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_resize_start_pt);
    s_resize_start_x = s_selected->x;
    s_resize_start_y = s_selected->y;
    s_resize_start_w = s_selected->w;
    s_resize_start_h = s_selected->h;
    s_resizing       = false;
}

static void _handle_pressing_cb(lv_event_t *e) {
    if (!s_armed || !s_selected) return;
    if (!s_selected->root || !lv_obj_is_valid(s_selected->root)) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t cur;
    lv_indev_get_point(indev, &cur);
    int dx = (int)cur.x - (int)s_resize_start_pt.x;
    int dy = (int)cur.y - (int)s_resize_start_pt.y;
    if (!s_resizing) {
        if (abs(dx) < RESIZE_THRESHOLD_PX && abs(dy) < RESIZE_THRESHOLD_PX) return;
        s_resizing = true;
    }

    int new_w = s_resize_start_w;
    int new_h = s_resize_start_h;
    int new_x = s_resize_start_x;
    int new_y = s_resize_start_y;

    /* Decompose handle index into x/y "active" flags + sign. */
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    bool touches_w = (dir == H_NW || dir == H_W || dir == H_SW);
    bool touches_e = (dir == H_NE || dir == H_E || dir == H_SE);
    bool touches_n = (dir == H_NW || dir == H_N || dir == H_NE);
    bool touches_s = (dir == H_SW || dir == H_S || dir == H_SE);

    if (touches_e) { new_w += dx; new_x += dx / 2; }
    if (touches_w) { new_w -= dx; new_x += dx / 2; }
    if (touches_s) { new_h += dy; new_y += dy / 2; }
    if (touches_n) { new_h -= dy; new_y += dy / 2; }

    /* Live grid snap (size AND position, since position shifts when the
     * opposite edge stays anchored). Matches the widget-drag snap so the
     * whole editor feels consistent. Step = 1 disables. */
    int step = (s_step > 0) ? s_step : 5;
    if (step > 1) {
        new_w = (new_w / step) * step;
        new_h = (new_h / step) * step;
        new_x = (new_x / step) * step;
        new_y = (new_y / step) * step;
    }

    /* Clamp to widget size constraints (web editor uses identical bounds). */
    if (new_w < 10)  new_w = 10;
    if (new_w > 800) new_w = 800;
    if (new_h < 10)  new_h = 10;
    if (new_h > 480) new_h = 480;

    s_selected->x = (int16_t)new_x;
    s_selected->y = (int16_t)new_y;
    if (s_selected->resize) {
        s_selected->resize(s_selected, (uint16_t)new_w, (uint16_t)new_h);
    } else {
        s_selected->w = (uint16_t)new_w;
        s_selected->h = (uint16_t)new_h;
        lv_obj_set_size(s_selected->root, new_w, new_h);
    }
    lv_obj_set_pos(s_selected->root, new_x, new_y);
    _update_ring();      /* repositions ring + handles in lockstep */
    _update_readout();
}

static void _handle_released_cb(lv_event_t *e) {
    (void)e;
    if (!s_armed || !s_selected) return;
    bool was_resizing = s_resizing;
    s_resizing   = false;
    s_resize_dir = -1;
    if (!was_resizing) return;

    /* Snap final size + position to the current step grid (matches what
     * drag-to-move does on release). */
    int step = (s_step > 0) ? s_step : 5;
    int sx = (s_selected->x / step) * step;
    int sy = (s_selected->y / step) * step;
    int sw = ((int)s_selected->w / step) * step;
    int sh = ((int)s_selected->h / step) * step;
    if (sw < 10) sw = 10;
    if (sh < 10) sh = 10;
    s_selected->x = (int16_t)sx;
    s_selected->y = (int16_t)sy;
    if (s_selected->resize) {
        s_selected->resize(s_selected, (uint16_t)sw, (uint16_t)sh);
    } else {
        s_selected->w = (uint16_t)sw;
        s_selected->h = (uint16_t)sh;
        lv_obj_set_size(s_selected->root, sw, sh);
    }
    lv_obj_set_pos(s_selected->root, sx, sy);
    _update_ring();
    _update_readout();
    _schedule_save();
}

static void _clear_selection(void) {
    s_selected                = NULL;
    s_dragging                = false;
    s_drag_enabled_this_press = false;
    s_resizing                = false;
    s_resize_dir              = -1;
    _destroy_ring();
    _destroy_toolbar();
    /* Top-bar Duplicate + Delete grey out when nothing's selected. */
    _refresh_undo_redo_styling();
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

/* Undo snapshot is debounced like save, but with a much longer window so
 * a flurry of small edits (e.g. ten 1-px nudges) coalesces into a single
 * undo step. The timer resets on every edit, so it only fires once the
 * user pauses for SNAP_DEBOUNCE_MS without touching anything. Explicit
 * actions (Undo, Redo, Duplicate, Delete) flush a pending timer first so
 * any in-progress session is captured before the next discrete event. */
static lv_timer_t *s_snap_timer = NULL;
#define SNAP_DEBOUNCE_MS 2500

static void _snap_timer_cb(lv_timer_t *t) {
    (void)t;
    _undo_snapshot();
    if (s_snap_timer) { lv_timer_del(s_snap_timer); s_snap_timer = NULL; }
}

static void _schedule_snapshot(void) {
    if (s_suppress_snapshot) return;
    if (s_snap_timer) {
        lv_timer_reset(s_snap_timer);
        return;
    }
    s_snap_timer = lv_timer_create(_snap_timer_cb, SNAP_DEBOUNCE_MS, NULL);
    if (s_snap_timer) lv_timer_set_repeat_count(s_snap_timer, 1);
}

/* Capture the current state immediately and cancel any pending debounce.
 * Called by explicit actions so the user's most recent gesture doesn't
 * get lost when they hit Undo / Delete / Duplicate before the timer
 * fires on its own. */
static void _flush_pending_snapshot(void) {
    if (!s_snap_timer) return;
    lv_timer_del(s_snap_timer);
    s_snap_timer = NULL;
    _undo_snapshot();
}

static void _schedule_save(void) {
    _schedule_snapshot();
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

/* Update the 4 live-readout chips (x / y / w / h). Each chip is a small
 * button containing a single label; we just rewrite the label text. Also
 * syncs the open popover (if any) so slider position tracks live drags. */
static void _update_readout(void) {
    static const char prefixes[4] = {'X', 'Y', 'W', 'H'};
    int values[4] = {0, 0, 0, 0};
    if (s_selected) {
        values[0] = s_selected->x;
        values[1] = s_selected->y;
        values[2] = (int)s_selected->w;
        values[3] = (int)s_selected->h;
    }
    for (int i = 0; i < 4; i++) {
        if (!s_chip_btns[i] || !lv_obj_is_valid(s_chip_btns[i])) continue;
        lv_obj_t *lbl = lv_obj_get_child(s_chip_btns[i], 0);
        if (!lbl) continue;
        if (s_selected) {
            lv_label_set_text_fmt(lbl, "%c %d", prefixes[i], values[i]);
        } else {
            lv_label_set_text_fmt(lbl, "%c -", prefixes[i]);
        }
    }
    _update_popover();
}

static void _refresh_step_styling(void) {
    static const int8_t values[3] = {1, 5, 10};
    for (int i = 0; i < 3; i++) {
        if (!s_step_btns[i] || !lv_obj_is_valid(s_step_btns[i])) continue;
        bool sel = (values[i] == s_step);
        /* Selected: solid accent, no border. Unselected: faint white surface
         * with the subtle light border from _make_tbtn left intact. */
        lv_obj_set_style_bg_color(s_step_btns[i],
                                  sel ? DT_ACCENT : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_step_btns[i],
                                sel ? LV_OPA_COVER : 10, 0);
        lv_obj_set_style_border_opa(s_step_btns[i], sel ? 0 : 20, 0);
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

/* ── Delete: confirm modal + dashboard call ─────────────────────────────── */

static void _close_delete_modal(void) {
    if (s_delete_modal && lv_obj_is_valid(s_delete_modal)) {
        lv_obj_del(s_delete_modal);
    }
    s_delete_modal = NULL;
}

static void _delete_cancel_cb(lv_event_t *e) {
    (void)e;
    _close_delete_modal();
}

static void _delete_confirm_cb(lv_event_t *e) {
    (void)e;
    /* Capture target BEFORE tearing down chrome (which nulls s_selected). */
    widget_t *target = s_selected;
    _close_delete_modal();
    if (!target) return;
    /* Pre-delete state goes on the ring as a discrete undo step; without
     * the flush, the deletion would coalesce with any in-progress edit. */
    _flush_pending_snapshot();
    _clear_selection();              /* destroys ring/handles/toolbar/popover */
    dashboard_delete_widget(target); /* frees widget + drops from registry */
    _undo_snapshot();                /* explicit post-delete state */
    _schedule_save();
}

static void _delete_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_selected) return;
    if (s_delete_modal && lv_obj_is_valid(s_delete_modal)) return;  /* already open */

    /* Modal on lv_layer_top so it covers everything including the toolbar
     * and any open popover. Backdrop blocks clicks elsewhere — only the
     * Cancel / Delete buttons can dismiss it. */
    s_delete_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_delete_modal, 800, 480);
    lv_obj_set_pos(s_delete_modal, 0, 0);
    lv_obj_clear_flag(s_delete_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_delete_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_delete_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_delete_modal, 0, 0);
    lv_obj_set_style_radius(s_delete_modal, 0, 0);
    lv_obj_set_style_pad_all(s_delete_modal, 0, 0);

    /* Centered confirm panel — web-style glassmorphic dialog. */
    lv_obj_t *panel = lv_obj_create(s_delete_modal);
    lv_obj_set_size(panel, 380, 170);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_set_style_shadow_width(panel, 24, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_70, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 6, 0);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Delete widget?");
    lv_obj_set_style_text_color(title, DT_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *msg = lv_label_create(panel);
    lv_label_set_text(msg, "You can Undo to bring it back.");
    lv_obj_set_style_text_color(msg, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(msg, THEME_FONT_SMALL, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_LEFT, 0, 30);

    /* Cancel — translucent secondary */
    lv_obj_t *cancel = lv_btn_create(panel);
    lv_obj_set_size(cancel, 160, 44);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cancel, 12, 0);
    lv_obj_set_style_border_color(cancel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(cancel, 30, 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_radius(cancel, DT_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
    lv_obj_set_style_text_color(cl, DT_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(cl, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(cancel, _delete_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* Delete — solid danger */
    lv_obj_t *del = lv_btn_create(panel);
    lv_obj_set_size(del, 160, 44);
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(del, DT_DANGER, 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(del, 0, 0);
    lv_obj_set_style_radius(del, DT_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    lv_obj_t *dl = lv_label_create(del);
    lv_label_set_text(dl, LV_SYMBOL_TRASH "  Delete");
    lv_obj_center(dl);
    lv_obj_set_style_text_color(dl, lv_color_white(), 0);
    lv_obj_set_style_text_font(dl, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(del, _delete_confirm_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Toolbar build / destroy ──────────────────────────────────────────────── */

/* ── Toolbar drag (grip strip on top edge) ────────────────────────────────
 *
 * Pressing on the toolbar background (anywhere not on an inner button)
 * starts a vertical drag. The drag only follows the Y axis; X is locked
 * to bottom-mid alignment so the toolbar stays centered horizontally.
 *
 * 4 px movement threshold mirrors the widget drag handler. Position is
 * clamped to keep the toolbar fully on-screen (top edge between 0 and
 * "screen height − toolbar height − minimum gap"). */

#define TB_DRAG_THRESHOLD 4
#define TB_MIN_GAP        4    /* won't snap completely flush to screen edge */
#define TB_WIDTH         760
#define TB_HEIGHT         76   /* roomier than the original 56 — easier targets */

static void _toolbar_pressed_cb(lv_event_t *e) {
    (void)e;
    if (!s_toolbar) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_tb_drag_pt);
    s_tb_drag_start_y = s_toolbar_y_off;
    s_tb_dragging     = false;
}

static void _toolbar_pressing_cb(lv_event_t *e) {
    (void)e;
    if (!s_toolbar || !lv_obj_is_valid(s_toolbar)) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t cur;
    lv_indev_get_point(indev, &cur);
    int dy = (int)cur.y - (int)s_tb_drag_pt.y;
    if (!s_tb_dragging) {
        if (abs(dy) < TB_DRAG_THRESHOLD) return;
        s_tb_dragging = true;
    }

    /* Toolbar height is fixed at 56 (see _build_toolbar). LV_ALIGN_BOTTOM_MID
     * with offset y measures from the parent's bottom edge upward. Range:
     *   max (lowest): -TB_MIN_GAP                       → just above bottom
     *   min (highest): -(480 - TB_HEIGHT - TB_MIN_GAP)  → just below top
     * Clamp keeps the toolbar fully visible regardless of drag distance. */
    int new_y = s_tb_drag_start_y + dy;
    if (new_y > -TB_MIN_GAP)      new_y = -TB_MIN_GAP;
    if (new_y < -(480 - TB_HEIGHT - TB_MIN_GAP))
                                  new_y = -(480 - TB_HEIGHT - TB_MIN_GAP);
    s_toolbar_y_off = (int16_t)new_y;
    lv_obj_align(s_toolbar, LV_ALIGN_BOTTOM_MID, 0, s_toolbar_y_off);
}

/* Helper: web-style button — translucent surface, defined border, white text.
 * Tuned brighter than the literal .wst-btn values from main/web/index.html
 * because the dashboard's bg is much busier than the web editor's flat
 * panel, so the same alpha values read too faint on-device. */
static lv_obj_t *_make_tbtn(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                            const char *text) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    /* Resting: ~10 % white fill — clearly visible against the panel. */
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, 28, 0);
    /* Pressed: brighter — ~27 % white. */
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, 70, LV_STATE_PRESSED);
    /* 1 px white border at ~20 % — pulls the button out of the panel. */
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

/* ════════════════════════════════════════════════════════════════════════════
 *  Adjustment popover — tap an x/y/w/h chip to open
 * ════════════════════════════════════════════════════════════════════════════
 *
 * Compact 300×84 panel that floats just above the toolbar. Contents:
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │  X     120                            [⌨]   [✕]      │
 *   │  ───────────●──────────────────────────────────────  │
 *   └──────────────────────────────────────────────────────┘
 *
 *   - Label (X / Y / W / H) on the left.
 *   - Big numeric value (tap → opens the on-device numeric keypad).
 *   - ⌨ button (same action as tapping the number).
 *   - ✕ button to close.
 *   - Slider across the bottom for coarse drag-to-adjust; live previews
 *     the widget as the slider moves.
 *
 * Stays open until ✕ is tapped, the widget is deselected, or Edit Mode
 * exits. Tapping a different chip switches the popover's target. */

static int _get_widget_field(char target) {
    if (!s_selected) return 0;
    switch (target) {
        case 'x': return s_selected->x;
        case 'y': return s_selected->y;
        case 'w': return (int)s_selected->w;
        case 'h': return (int)s_selected->h;
    }
    return 0;
}

/* Resize / reposition the selected widget. For w/h we prefer the widget's
 * resize vtable (some widgets rebuild internal LVGL objects on resize); for
 * widgets without it, fall back to lv_obj_set_size and manual struct update. */
static void _set_widget_field(char target, int value) {
    if (!s_selected || !s_selected->root || !lv_obj_is_valid(s_selected->root)) return;

    switch (target) {
        case 'x':
            s_selected->x = (int16_t)value;
            lv_obj_set_pos(s_selected->root, s_selected->x, s_selected->y);
            break;
        case 'y':
            s_selected->y = (int16_t)value;
            lv_obj_set_pos(s_selected->root, s_selected->x, s_selected->y);
            break;
        case 'w':
            if (value < 10)  value = 10;
            if (value > 800) value = 800;
            if (s_selected->resize) {
                s_selected->resize(s_selected, (uint16_t)value, s_selected->h);
            } else {
                s_selected->w = (uint16_t)value;
                lv_obj_set_size(s_selected->root, value, s_selected->h);
            }
            break;
        case 'h':
            if (value < 10)  value = 10;
            if (value > 480) value = 480;
            if (s_selected->resize) {
                s_selected->resize(s_selected, s_selected->w, (uint16_t)value);
            } else {
                s_selected->h = (uint16_t)value;
                lv_obj_set_size(s_selected->root, s_selected->w, value);
            }
            break;
        default:
            return;
    }
    _update_ring();
    _update_readout();
    _schedule_save();
}

/* Reasonable slider ranges. Widths/heights cap at screen dimensions; x/y
 * use a generous over-range so widgets can be parked off-edge if needed
 * (rare but possible). */
static void _get_chip_range(char target, int *min, int *max) {
    switch (target) {
        case 'x': *min = -400; *max =  400; break;
        case 'y': *min = -240; *max =  240; break;
        case 'w': *min =   10; *max =  800; break;
        case 'h': *min =   10; *max =  480; break;
        default:  *min =    0; *max =    0; break;
    }
}

static void _close_popover(void) {
    if (s_chip_popover && lv_obj_is_valid(s_chip_popover)) {
        lv_obj_del(s_chip_popover);
    }
    s_chip_popover    = NULL;
    s_popover_slider  = NULL;
    s_popover_value   = NULL;
    s_popover_target  = 0;
    s_popover_syncing = false;
}

/* Sync the popover's slider + numeric display to the widget's current value.
 * Called from _update_readout whenever the widget moves (drag, nudge, slider
 * itself, keypad confirm). The syncing flag stops the slider value-change
 * handler from re-triggering on programmatic set_value. */
static void _update_popover(void) {
    if (!s_chip_popover || !lv_obj_is_valid(s_chip_popover)) return;
    if (!s_selected || s_popover_target == 0) return;
    int v = _get_widget_field(s_popover_target);
    s_popover_syncing = true;
    if (s_popover_slider && lv_obj_is_valid(s_popover_slider)) {
        lv_slider_set_value(s_popover_slider, v, LV_ANIM_OFF);
    }
    if (s_popover_value && lv_obj_is_valid(s_popover_value)) {
        lv_label_set_text_fmt(s_popover_value, "%d", v);
    }
    s_popover_syncing = false;
}

static void _popover_slider_cb(lv_event_t *e) {
    if (s_popover_syncing) return;
    if (!s_selected || s_popover_target == 0) return;
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    _set_widget_field(s_popover_target, v);
}

/* Selection ring + 8 handles live on lv_layer_top, which draws above every
 * screen including modal dialogs. Without hiding them, the blue ring sits
 * on top of the numeric keypad — looks broken. Hide on keypad open, show
 * again on close (confirm OR cancel). */
static void _set_selection_chrome_hidden(bool hidden) {
    if (s_ring && lv_obj_is_valid(s_ring)) {
        if (hidden) lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 8; i++) {
        if (!s_handles[i] || !lv_obj_is_valid(s_handles[i])) continue;
        if (hidden) lv_obj_add_flag(s_handles[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(s_handles[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void _popover_keypad_confirmed(const char *text, void *user_data) {
    (void)user_data;
    _set_selection_chrome_hidden(false);
    if (!text || s_popover_target == 0) return;
    int v = atoi(text);
    _set_widget_field(s_popover_target, v);
}

static void _popover_keypad_cancelled(void *user_data) {
    (void)user_data;
    _set_selection_chrome_hidden(false);
}

/* Fine-step buttons flanking the slider: each press nudges the active
 * field by 1 px (irrespective of the global step toggle — these are the
 * "escape hatch" for sub-grid precision). Hold-to-repeat at 100 ms via
 * LV_EVENT_LONG_PRESSED_REPEAT after the 400 ms long-press threshold. */
static void _popover_step_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    if (!s_selected || s_popover_target == 0) return;
    int delta = (int)(intptr_t)lv_event_get_user_data(e);   /* -1 or +1 */
    int current = _get_widget_field(s_popover_target);
    _set_widget_field(s_popover_target, current + delta);
}

static void _popover_keypad_cb(lv_event_t *e) {
    (void)e;
    if (!s_selected || s_popover_target == 0) return;
    char initial[12];
    snprintf(initial, sizeof(initial), "%d", _get_widget_field(s_popover_target));
    const char *title = "Value";
    switch (s_popover_target) {
        case 'x': title = "X position"; break;
        case 'y': title = "Y position"; break;
        case 'w': title = "Width";      break;
        case 'h': title = "Height";     break;
    }
    /* Stash the selection chrome so it doesn't sit on top of the keypad. */
    _set_selection_chrome_hidden(true);
    show_numeric_input_dialog(title, initial,
                              _popover_keypad_confirmed,
                              _popover_keypad_cancelled,
                              NULL);
}

static void _close_popover_cb(lv_event_t *e) {
    (void)e;
    _close_popover();
}

static void _open_popover(char target) {
    if (!s_selected || !s_toolbar) return;

    /* Re-open path: rebuild for the new target. Simpler than mutating an
     * existing popover, and the slider range may change between targets. */
    if (s_chip_popover && lv_obj_is_valid(s_chip_popover)) {
        lv_obj_del(s_chip_popover);
    }
    s_chip_popover    = NULL;
    s_popover_slider  = NULL;
    s_popover_value   = NULL;
    s_popover_target  = target;

    lv_obj_t *parent = lv_obj_get_parent(s_toolbar);
    if (!parent) return;

    int v = _get_widget_field(target);
    int min_v, max_v;
    _get_chip_range(target, &min_v, &max_v);

    s_chip_popover = lv_obj_create(parent);
    /* Slightly taller now to host the ± buttons alongside the slider. */
    lv_obj_set_size(s_chip_popover, 300, 92);
    lv_obj_clear_flag(s_chip_popover, LV_OBJ_FLAG_SCROLLABLE);
    /* Same glassmorphic style as the toolbar so they read as a pair. */
    lv_obj_set_style_bg_color(s_chip_popover, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(s_chip_popover, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_chip_popover, 0, 0);
    lv_obj_set_style_radius(s_chip_popover, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(s_chip_popover, 10, 0);
    lv_obj_set_style_shadow_width(s_chip_popover, 16, 0);
    lv_obj_set_style_shadow_color(s_chip_popover, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_chip_popover, LV_OPA_60, 0);
    lv_obj_set_style_shadow_ofs_y(s_chip_popover, 4, 0);

    /* Anchor just above the toolbar. The toolbar may have been dragged up
     * by the user, so read its current screen position rather than assuming
     * the default. Fall back to 8 px from top if it would clip. */
    lv_obj_update_layout(s_toolbar);
    lv_area_t tb_area;
    lv_obj_get_coords(s_toolbar, &tb_area);
    int popover_top = tb_area.y1 - 92 - 8;
    if (popover_top < 8) popover_top = 8;
    lv_obj_set_pos(s_chip_popover, (800 - 300) / 2, popover_top);

    /* Field label (X / Y / W / H) — muted, left side */
    lv_obj_t *fl = lv_label_create(s_chip_popover);
    char tag[2] = {(char)(target - 32), '\0'};   /* lowercase → uppercase */
    lv_label_set_text(fl, tag);
    lv_obj_align(fl, LV_ALIGN_TOP_LEFT, 0, 6);
    lv_obj_set_style_text_color(fl, DT_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(fl, THEME_FONT_SMALL, 0);

    /* Big numeric value — tappable to open the keypad */
    s_popover_value = lv_label_create(s_chip_popover);
    lv_obj_align(s_popover_value, LV_ALIGN_TOP_LEFT, 22, 0);
    lv_obj_set_style_text_color(s_popover_value, DT_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(s_popover_value, THEME_FONT_LARGE, 0);
    lv_obj_add_flag(s_popover_value, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_popover_value, _popover_keypad_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_label_set_text_fmt(s_popover_value, "%d", v);

    /* Keypad button (right-of-X) */
    lv_obj_t *kpd = _make_tbtn(s_chip_popover, 36, 28, LV_SYMBOL_KEYBOARD);
    lv_obj_align(kpd, LV_ALIGN_TOP_RIGHT, -36, 2);
    lv_obj_add_event_cb(kpd, _popover_keypad_cb, LV_EVENT_CLICKED, NULL);

    /* Close X (far right) */
    lv_obj_t *cls = _make_tbtn(s_chip_popover, 28, 28, LV_SYMBOL_CLOSE);
    lv_obj_align(cls, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_add_event_cb(cls, _close_popover_cb, LV_EVENT_CLICKED, NULL);

    /* Bottom row — ± buttons flank the slider. Flex container so the three
     * items stay vertically centered relative to each other (slider 12 tall,
     * buttons 32 tall). */
    lv_obj_t *bottom = lv_obj_create(s_chip_popover);
    lv_obj_set_size(bottom, 280, 36);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bottom, 0, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_pad_all(bottom, 0, 0);
    lv_obj_set_style_pad_column(bottom, 6, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Minus — 1 px decrement (irrespective of grid step) */
    lv_obj_t *minus_btn = _make_tbtn(bottom, 32, 32, LV_SYMBOL_MINUS);
    lv_obj_add_event_cb(minus_btn, _popover_step_btn_cb,
                        LV_EVENT_PRESSED, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(minus_btn, _popover_step_btn_cb,
                        LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);

    /* Slider — narrower now to leave room for the ± buttons */
    s_popover_slider = lv_slider_create(bottom);
    lv_obj_set_size(s_popover_slider, 200, 12);
    lv_slider_set_range(s_popover_slider, min_v, max_v);
    lv_slider_set_value(s_popover_slider, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_popover_slider, DT_BG_INSET, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_popover_slider, DT_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_popover_slider, DT_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_popover_slider, _popover_slider_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Plus — 1 px increment */
    lv_obj_t *plus_btn = _make_tbtn(bottom, 32, 32, LV_SYMBOL_PLUS);
    lv_obj_add_event_cb(plus_btn, _popover_step_btn_cb,
                        LV_EVENT_PRESSED, (void *)(intptr_t)+1);
    lv_obj_add_event_cb(plus_btn, _popover_step_btn_cb,
                        LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)+1);

    lv_obj_move_foreground(s_chip_popover);
}

static void _chip_clicked_cb(lv_event_t *e) {
    if (!s_selected) return;
    char target = (char)(intptr_t)lv_event_get_user_data(e);
    if (target != 'x' && target != 'y' && target != 'w' && target != 'h') return;
    _open_popover(target);
}

static void _build_toolbar(lv_obj_t *parent) {
    if (s_toolbar && lv_obj_is_valid(s_toolbar)) return;

    s_toolbar = lv_obj_create(parent);
    lv_obj_set_size(s_toolbar, TB_WIDTH, TB_HEIGHT);
    lv_obj_align(s_toolbar, LV_ALIGN_BOTTOM_MID, 0, s_toolbar_y_off);
    lv_obj_clear_flag(s_toolbar, LV_OBJ_FLAG_SCROLLABLE);

    /* Web editor look: dark glassmorphic panel — translucent so the live
     * widgets below remain partially visible, no hard border, soft floating
     * shadow. Tracks main/web/index.html `.modal` / `.toolbar` patterns. */
    lv_obj_set_style_bg_color(s_toolbar, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(s_toolbar, LV_OPA_70, 0);   /* see-through */
    lv_obj_set_style_border_width(s_toolbar, 0, 0);
    lv_obj_set_style_radius(s_toolbar, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(s_toolbar, 10, 0);
    /* Min gap between flex items; SPACE_BETWEEN distributes extra evenly. */
    lv_obj_set_style_pad_column(s_toolbar, 6, 0);
    lv_obj_set_style_shadow_width(s_toolbar, 16, 0);
    lv_obj_set_style_shadow_color(s_toolbar, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_toolbar, LV_OPA_60, 0);
    lv_obj_set_style_shadow_ofs_y(s_toolbar, 4, 0);

    /* Grip strip — small white pill at top-center hints "you can drag me". */
    lv_obj_t *grip = lv_obj_create(s_toolbar);
    lv_obj_set_size(grip, 36, 4);
    lv_obj_align(grip, LV_ALIGN_TOP_MID, 0, -4);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(grip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(grip, 60, 0);
    lv_obj_set_style_radius(grip, 2, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_shadow_width(grip, 0, 0);

    /* Toolbar bg captures drag events. Inner buttons get hit-tested first
     * (LVGL dispatches to topmost CLICKABLE child), so dragging only fires
     * when pressing on empty space — the grip is the obvious target. */
    lv_obj_add_event_cb(s_toolbar, _toolbar_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_toolbar, _toolbar_pressing_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_set_flex_flow(s_toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_toolbar,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* Grip is positioned absolutely (TOP_MID) — flex layout below treats
     * remaining children as the row content. Tell flex to ignore it. */
    lv_obj_add_flag(grip, LV_OBJ_FLAG_IGNORE_LAYOUT);

    /* Nudge cluster — 4 buttons, tight 2px gaps */
    lv_obj_t *nudges = lv_obj_create(s_toolbar);
    lv_obj_set_size(nudges, 156, 48);
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
        lv_obj_t *b = _make_tbtn(nudges, 36, 44, icons[i]);
        lv_obj_add_event_cb(b, _nudge_btn_cb, LV_EVENT_PRESSED,
                            (void *)(intptr_t)dirs[i]);
        lv_obj_add_event_cb(b, _nudge_btn_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                            (void *)(intptr_t)dirs[i]);
    }

    /* Step toggle — 3-segment chip group. Each chip is a small _make_tbtn
     * button with rounded corners; _refresh_step_styling colours the active
     * one accent. Web-style: small gap between segments rather than a hard
     * shared border. */
    lv_obj_t *steps = lv_obj_create(s_toolbar);
    lv_obj_set_size(steps, 138, 48);
    lv_obj_set_style_bg_opa(steps, 0, 0);
    lv_obj_set_style_border_width(steps, 0, 0);
    lv_obj_set_style_pad_all(steps, 0, 0);
    lv_obj_set_style_pad_column(steps, 3, 0);
    lv_obj_clear_flag(steps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(steps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(steps, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const int8_t step_values[3] = {1, 5, 10};
    static const char *const step_labels[3] = {"1", "5", "10"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = _make_tbtn(steps, 44, 44, step_labels[i]);
        lv_obj_add_event_cb(b, _step_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)step_values[i]);
        s_step_btns[i] = b;
    }
    _refresh_step_styling();

    /* Live x/y/w/h chips — clickable, each opens an adjustment popover.
     * Wrapped in a flex container so the chips stay together as a unit
     * while the outer flex layout positions them between the step toggle
     * and the Configure button. */
    lv_obj_t *chip_grp = lv_obj_create(s_toolbar);
    lv_obj_set_size(chip_grp, 252, 48);
    lv_obj_set_style_bg_opa(chip_grp, 0, 0);
    lv_obj_set_style_border_width(chip_grp, 0, 0);
    lv_obj_set_style_pad_all(chip_grp, 0, 0);
    lv_obj_set_style_pad_column(chip_grp, 4, 0);
    lv_obj_clear_flag(chip_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(chip_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip_grp, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const char chip_targets[4] = {'x', 'y', 'w', 'h'};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = _make_tbtn(chip_grp, 56, 40, "");
        lv_obj_add_event_cb(c, _chip_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)chip_targets[i]);
        s_chip_btns[i] = c;
    }
    _update_readout();

    /* Configure: solid accent — primary action, sits at the right edge.
     * Delete moved to the top toolbar; bottom toolbar is widget-tuning only. */
    lv_obj_t *cfg = lv_btn_create(s_toolbar);
    lv_obj_set_size(cfg, 130, 44);
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
    _close_popover();   /* never have a popover without its toolbar */
    if (s_toolbar && lv_obj_is_valid(s_toolbar)) lv_obj_del(s_toolbar);
    s_toolbar      = NULL;
    s_step_btns[0] = s_step_btns[1] = s_step_btns[2] = NULL;
    s_chip_btns[0] = s_chip_btns[1] = s_chip_btns[2] = s_chip_btns[3] = NULL;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Undo / Redo ring
 * ════════════════════════════════════════════════════════════════════════════
 *
 * Each snapshot is the current layout serialised to JSON (via
 * layout_manager_build_json + cJSON_PrintUnformatted). Snapshots are stored
 * in PSRAM-backed allocations to keep internal RAM free for LVGL.
 *
 * On every action commit, the post-action state is pushed. Undoing applies
 * the previous snapshot (and selection is cleared because old widget
 * pointers are about to be freed by widget_registry_reset). Re-edits after
 * undo truncate the forward history. */

/* s_suppress_snapshot defined at file scope above — set during undo/redo
 * apply so the resulting _schedule_save doesn't push a redundant snapshot. */

static char *_serialize_layout(void) {
    char layout_name[LAYOUT_MAX_NAME] = "default";
    layout_manager_get_active(layout_name, sizeof(layout_name));
    widget_t **widgets = dashboard_get_widgets();
    uint8_t   count    = dashboard_get_widget_count();
    cJSON *root = layout_manager_build_json(layout_name, widgets, count);
    if (!root) return NULL;
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return NULL;
    /* Copy into PSRAM so we don't squat on internal RAM. cJSON's allocator
     * may have used either pool — duplicate explicitly to guarantee. */
    size_t len = strlen(s);
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!copy) copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    cJSON_free(s);
    return copy;
}

static void _undo_clear_ring(void) {
    for (int i = 0; i < UNDO_MAX; i++) {
        if (s_undo[i]) { free(s_undo[i]); s_undo[i] = NULL; }
    }
    s_undo_count = 0;
    s_undo_pos   = -1;
}

static void _undo_snapshot(void) {
    if (s_suppress_snapshot) return;

    char *snap = _serialize_layout();
    if (!snap) return;

    /* Skip if identical to what we already pushed. Stops debounced timer
     * firings from creating duplicate ring entries when an explicit action
     * (Duplicate / Delete) already snapshotted moments earlier and nothing
     * changed in the meantime. */
    if (s_undo_pos >= 0 && s_undo[s_undo_pos] &&
        strcmp(snap, s_undo[s_undo_pos]) == 0) {
        free(snap);
        return;
    }

    /* Truncate redo branch — any forward history is invalidated by this new edit. */
    for (int i = s_undo_pos + 1; i < s_undo_count; i++) {
        if (s_undo[i]) { free(s_undo[i]); s_undo[i] = NULL; }
    }
    s_undo_count = s_undo_pos + 1;

    /* Drop oldest if ring is full. */
    if (s_undo_count >= UNDO_MAX) {
        free(s_undo[0]);
        for (int i = 0; i < UNDO_MAX - 1; i++) s_undo[i] = s_undo[i + 1];
        s_undo[UNDO_MAX - 1] = NULL;
        s_undo_count--;
        s_undo_pos--;
    }

    s_undo[s_undo_count] = snap;
    s_undo_count++;
    s_undo_pos = s_undo_count - 1;

    _refresh_undo_redo_styling();
}

static void _apply_snapshot(int idx) {
    if (idx < 0 || idx >= s_undo_count || !s_undo[idx]) return;
    cJSON *root = cJSON_Parse(s_undo[idx]);
    if (!root) {
        ESP_LOGE(TAG, "undo: failed to parse snapshot %d", idx);
        return;
    }
    /* Clear selection BEFORE the widget tree is torn down — selection's
     * widget pointer is about to be freed. */
    _clear_selection();
    s_suppress_snapshot = true;
    dashboard_reapply_layout_keep_edit_mode(ui_Screen3, root);
    s_suppress_snapshot = false;
    cJSON_Delete(root);
}

static void _do_undo(void) {
    /* Commit any in-progress edit session before stepping back, otherwise
     * the user's most recent gesture would be silently lost (the snapshot
     * timer hadn't fired yet). After flush, the current state IS the top
     * of the ring — undo then takes us one step back from there. */
    _flush_pending_snapshot();
    if (s_undo_pos <= 0) return;
    s_undo_pos--;
    _apply_snapshot(s_undo_pos);
    _refresh_undo_redo_styling();
}

static void _do_redo(void) {
    _flush_pending_snapshot();
    if (s_undo_pos >= s_undo_count - 1) return;
    s_undo_pos++;
    _apply_snapshot(s_undo_pos);
    _refresh_undo_redo_styling();
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Duplicate
 * ════════════════════════════════════════════════════════════════════════════
 *
 * JSON-round-trip approach: serialise the layout, find the source widget's
 * entry in the widgets array, deep-clone it, assign an unused slot of the
 * same widget type, offset the position so the clone is visible, append,
 * and reapply.
 *
 * This is heavier than a surgical "add one widget" path but stays simple
 * (reuses dashboard_reapply_layout_keep_edit_mode) and produces a clean
 * snapshot for undo.  */

static int _find_unused_slot(widget_type_t type) {
    widget_t **widgets = dashboard_get_widgets();
    uint8_t   count    = dashboard_get_widget_count();
    /* Try slots 0..31 — covers every widget type's constraint with room. */
    for (int s = 0; s < 32; s++) {
        bool taken = false;
        for (uint8_t i = 0; i < count; i++) {
            if (widgets[i] && widgets[i]->type == type && widgets[i]->slot == s) {
                taken = true;
                break;
            }
        }
        if (!taken) return s;
    }
    return -1;
}

static void _do_duplicate(void) {
    if (!s_selected || !s_selected->to_json) return;
    /* Commit any in-progress session so the pre-duplicate state is the
     * top of the ring; the explicit snapshot below then captures the
     * post-duplicate state as a separate undo step. */
    _flush_pending_snapshot();
    widget_type_t type = s_selected->type;
    uint8_t       src_slot = s_selected->slot;

    int new_slot = _find_unused_slot(type);
    if (new_slot < 0) {
        ESP_LOGW(TAG, "duplicate: no free slot for type %d", (int)type);
        return;
    }

    char layout_name[LAYOUT_MAX_NAME] = "default";
    layout_manager_get_active(layout_name, sizeof(layout_name));
    widget_t **widgets = dashboard_get_widgets();
    uint8_t    count   = dashboard_get_widget_count();
    cJSON *root = layout_manager_build_json(layout_name, widgets, count);
    if (!root) return;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "widgets");
    if (!arr) { cJSON_Delete(root); return; }

    /* Locate the source widget's JSON entry. build_json walked widgets[]
     * in the same order, so we can index directly. */
    int src_idx = -1;
    for (uint8_t i = 0; i < count; i++) {
        if (widgets[i] == s_selected) { src_idx = (int)i; break; }
    }
    if (src_idx < 0) { cJSON_Delete(root); return; }

    cJSON *src = cJSON_GetArrayItem(arr, src_idx);
    if (!src) { cJSON_Delete(root); return; }

    cJSON *dup = cJSON_Duplicate(src, true);
    if (!dup) { cJSON_Delete(root); return; }

    /* Reassign slot inside the clone's config object. */
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(dup, "config");
    if (cfg) {
        cJSON_DeleteItemFromObject(cfg, "slot");
        cJSON_AddNumberToObject(cfg, "slot", new_slot);
    }

    /* Update the top-level id so the registry can find both copies. */
    cJSON_DeleteItemFromObject(dup, "id");
    char new_id[16];
    snprintf(new_id, sizeof(new_id), "%d_%u", (int)type, (unsigned)new_slot);
    cJSON_AddStringToObject(dup, "id", new_id);

    /* Offset position by 20 px so the clone sits where the user can see it
     * instead of stacking dead-on the original. */
    cJSON *x = cJSON_GetObjectItemCaseSensitive(dup, "x");
    cJSON *y = cJSON_GetObjectItemCaseSensitive(dup, "y");
    if (cJSON_IsNumber(x)) cJSON_SetNumberValue(x, x->valueint + 20);
    if (cJSON_IsNumber(y)) cJSON_SetNumberValue(y, y->valueint + 20);

    cJSON_AddItemToArray(arr, dup);

    /* Push pre-action snapshot so undo can return to the un-duplicated state. */
    /* (The post-state will be captured by _schedule_save below.) */

    /* Selection chrome refers to the about-to-be-freed widget; tear down. */
    _clear_selection();
    s_suppress_snapshot = true;
    dashboard_reapply_layout_keep_edit_mode(ui_Screen3, root);
    s_suppress_snapshot = false;
    cJSON_Delete(root);

    /* Snapshot the post-duplicate state. */
    _undo_snapshot();
    _schedule_save();

    (void)src_slot;   /* placeholder for future "select clone" UX */
}

/* ────────────────────────────────────────────────────────────────────────── */

/* Updates resting opacity + text opacity to reflect which top-bar actions
 * are currently valid. "Enabled" = base _make_tbtn brightness; "disabled" =
 * markedly dimmer but still legible so the user knows the button exists.
 * Delete uses red tints (DT_DANGER set inline in _build_top_toolbar) — only
 * its OPAs are toggled here, not its colour. */
static void _refresh_undo_redo_styling(void) {
    bool can_undo       = (s_undo_pos > 0);
    bool can_redo       = (s_undo_pos >= 0 && s_undo_pos < s_undo_count - 1);
    bool has_selection  = (s_selected != NULL);

#define APPLY_STATE(BTN, ON, ON_BG, ON_BORDER, OFF_BG, OFF_BORDER)            \
    do {                                                                       \
        if ((BTN) && lv_obj_is_valid(BTN)) {                                   \
            lv_obj_set_style_bg_opa((BTN), (ON) ? (ON_BG) : (OFF_BG), 0);      \
            lv_obj_set_style_border_opa((BTN), (ON) ? (ON_BORDER)              \
                                                    : (OFF_BORDER), 0);        \
            lv_obj_t *_lbl = lv_obj_get_child((BTN), 0);                       \
            if (_lbl) lv_obj_set_style_text_opa((_lbl),                        \
                        (ON) ? LV_OPA_COVER : LV_OPA_40, 0);                   \
        }                                                                      \
    } while (0)

    APPLY_STATE(s_undo_btn, can_undo,      28, 55, 10, 20);
    APPLY_STATE(s_redo_btn, can_redo,      28, 55, 10, 20);
    APPLY_STATE(s_dup_btn,  has_selection, 28, 55, 10, 20);
    /* Delete: red tint, slightly stronger so it reads as destructive. */
    APPLY_STATE(s_del_btn,  has_selection, 38, 70, 12, 30);

#undef APPLY_STATE
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Top toolbar — Exit / Undo / Redo / Duplicate + selection status
 * ════════════════════════════════════════════════════════════════════════════ */

#define TT_DRAG_THRESHOLD 4

static void _exit_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    edit_mode_exit();
}

static void _undo_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _do_undo();
}

static void _redo_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _do_redo();
}

static void _dup_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _do_duplicate();
}

static void _top_toolbar_pressed_cb(lv_event_t *e) {
    (void)e;
    if (!s_top_toolbar) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_tt_drag_pt);
    s_tt_drag_start = s_top_toolbar_y;
    s_tt_dragging   = false;
}

static void _top_toolbar_pressing_cb(lv_event_t *e) {
    (void)e;
    if (!s_top_toolbar || !lv_obj_is_valid(s_top_toolbar)) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t cur;
    lv_indev_get_point(indev, &cur);
    int dy = (int)cur.y - (int)s_tt_drag_pt.y;
    if (!s_tt_dragging) {
        if (abs(dy) < TT_DRAG_THRESHOLD) return;
        s_tt_dragging = true;
    }
    /* Range: 4 px below top edge (offset 4) down to 4 px above bottom edge.
     * TB_HEIGHT / TB_MIN_GAP carry over from the bottom toolbar's geometry. */
    int new_y = s_tt_drag_start + dy;
    if (new_y < TB_MIN_GAP) new_y = TB_MIN_GAP;
    if (new_y > 480 - TB_HEIGHT - TB_MIN_GAP)
        new_y = 480 - TB_HEIGHT - TB_MIN_GAP;
    s_top_toolbar_y = (int16_t)new_y;
    lv_obj_align(s_top_toolbar, LV_ALIGN_TOP_MID, 0, s_top_toolbar_y);
}

static void _build_top_toolbar(lv_obj_t *parent) {
    if (s_top_toolbar && lv_obj_is_valid(s_top_toolbar)) return;

    s_top_toolbar = lv_obj_create(parent);
    lv_obj_set_size(s_top_toolbar, TB_WIDTH, TB_HEIGHT);
    lv_obj_align(s_top_toolbar, LV_ALIGN_TOP_MID, 0, s_top_toolbar_y);
    lv_obj_clear_flag(s_top_toolbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_top_toolbar, DT_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(s_top_toolbar, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_top_toolbar, 0, 0);
    lv_obj_set_style_radius(s_top_toolbar, DT_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(s_top_toolbar, 10, 0);
    lv_obj_set_style_pad_column(s_top_toolbar, 6, 0);
    lv_obj_set_style_shadow_width(s_top_toolbar, 16, 0);
    lv_obj_set_style_shadow_color(s_top_toolbar, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_top_toolbar, LV_OPA_60, 0);
    lv_obj_set_style_shadow_ofs_y(s_top_toolbar, 4, 0);

    /* Grip — symmetric with the bottom toolbar (at bottom edge here so it
     * reads as "drag me toward the screen middle"). */
    lv_obj_t *grip = lv_obj_create(s_top_toolbar);
    lv_obj_set_size(grip, 36, 4);
    lv_obj_align(grip, LV_ALIGN_BOTTOM_MID, 0, 4);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(grip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(grip, 60, 0);
    lv_obj_set_style_radius(grip, 2, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_shadow_width(grip, 0, 0);
    lv_obj_add_flag(grip, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_add_event_cb(s_top_toolbar, _top_toolbar_pressed_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_top_toolbar, _top_toolbar_pressing_cb,
                        LV_EVENT_PRESSING, NULL);

    lv_obj_set_flex_flow(s_top_toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_toolbar,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Exit button — solid danger, leftmost, prominent */
    lv_obj_t *exit_btn = lv_btn_create(s_top_toolbar);
    lv_obj_set_size(exit_btn, 150, 44);
    lv_obj_set_style_bg_color(exit_btn, DT_DANGER, 0);
    lv_obj_set_style_bg_opa(exit_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(exit_btn, DT_RADIUS_SM, 0);
    lv_obj_set_style_border_width(exit_btn, 0, 0);
    lv_obj_set_style_shadow_width(exit_btn, 0, 0);
    lv_obj_set_style_pad_all(exit_btn, 0, 0);
    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_label_set_text(exit_lbl, LV_SYMBOL_CLOSE "  Exit Edit Mode");
    lv_obj_set_style_text_color(exit_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(exit_lbl, THEME_FONT_SMALL, 0);
    lv_obj_center(exit_lbl);
    lv_obj_add_event_cb(exit_btn, _exit_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Undo / Redo cluster */
    lv_obj_t *ur_grp = lv_obj_create(s_top_toolbar);
    lv_obj_set_size(ur_grp, 152, 48);
    lv_obj_set_style_bg_opa(ur_grp, 0, 0);
    lv_obj_set_style_border_width(ur_grp, 0, 0);
    lv_obj_set_style_pad_all(ur_grp, 0, 0);
    lv_obj_set_style_pad_column(ur_grp, 4, 0);
    lv_obj_clear_flag(ur_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ur_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ur_grp, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Undo / Redo as text labels — LVGL Montserrat doesn't ship arc-rotate
     * symbols, and plain LV_SYMBOL_LEFT/RIGHT arrows read as navigation, not
     * history. Text removes the ambiguity. */
    s_undo_btn = _make_tbtn(ur_grp, 72, 44, "Undo");
    lv_obj_add_event_cb(s_undo_btn, _undo_btn_cb, LV_EVENT_CLICKED, NULL);
    s_redo_btn = _make_tbtn(ur_grp, 72, 44, "Redo");
    lv_obj_add_event_cb(s_redo_btn, _redo_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Duplicate button */
    s_dup_btn = _make_tbtn(s_top_toolbar, 124, 44, LV_SYMBOL_COPY "  Duplicate");
    lv_obj_add_event_cb(s_dup_btn, _dup_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Delete button — danger-tinted, right-most. Reuses the existing
     * _delete_btn_cb (confirm modal flow). The red colours are baked in
     * here; _refresh_undo_redo_styling only toggles their opacity. */
    s_del_btn = _make_tbtn(s_top_toolbar, 104, 44, LV_SYMBOL_TRASH "  Delete");
    lv_obj_set_style_bg_color(s_del_btn, DT_DANGER, 0);
    lv_obj_set_style_bg_color(s_del_btn, DT_DANGER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_del_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_del_btn, DT_DANGER, 0);
    lv_obj_add_event_cb(s_del_btn, _delete_btn_cb, LV_EVENT_CLICKED, NULL);

    _refresh_undo_redo_styling();
    lv_obj_move_foreground(s_top_toolbar);
}

static void _destroy_top_toolbar(void) {
    if (s_top_toolbar && lv_obj_is_valid(s_top_toolbar)) lv_obj_del(s_top_toolbar);
    s_top_toolbar = NULL;
    s_undo_btn    = NULL;
    s_redo_btn    = NULL;
    s_dup_btn     = NULL;
    s_del_btn     = NULL;
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

    /* Hide both pills — top toolbar takes over the Exit + status role,
     * and the Menu button is irrelevant while editing. */
    if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button))
        lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
    if (s_pill && lv_obj_is_valid(s_pill))
        lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);

    /* Promote decorations (image/shape_panel/line) so they can be selected
     * and dragged just like any other widget. */
    _set_decoration_clickable(true);

    /* Build the top toolbar on the same parent the pill lives in (ui_Screen3). */
    if (s_pill) {
        lv_obj_t *parent = lv_obj_get_parent(s_pill);
        if (parent) _build_top_toolbar(parent);
    }

    /* Initial snapshot (immediate, no debounce) — undo can always return
     * the user to the state they were in when Edit Mode opened. */
    _undo_snapshot();

    ESP_LOGI(TAG, "Edit Mode armed");
}

void edit_mode_exit(void) {
    if (!s_armed && !s_banner && !s_ring && !s_toolbar &&
        !s_delete_modal && !s_top_toolbar)
        return;   /* idempotent fast path */
    s_armed = false;
    _close_delete_modal();
    _clear_selection();   /* destroys ring + bottom toolbar + popover */
    _set_decoration_clickable(false);
    _destroy_top_toolbar();
    _destroy_banner();
    _undo_clear_ring();
    _apply_pill_style_live();
    if (s_pill && lv_obj_is_valid(s_pill))
        lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    /* Cancel any pending debounced save — the layout's current state will
     * be re-saved next time the user edits, no rush to flush now. */
    if (s_save_timer) { lv_timer_del(s_save_timer); s_save_timer = NULL; }
    if (s_snap_timer) { lv_timer_del(s_snap_timer); s_snap_timer = NULL; }
    ESP_LOGI(TAG, "Edit Mode exited");
}

lv_obj_t *edit_mode_create_pill(lv_obj_t *parent) {
    if (!parent) return NULL;

    /* If the previous screen was deleted, our cached pointers are stale —
     * forget them and rebuild from scratch on this parent. Ring + handles
     * now live on ui_Screen3 (not lv_layer_top, so the ring can sit behind
     * the toolbars), so LVGL cascades the delete when the screen goes —
     * the is_valid checks here are belt-and-braces. */
    if (s_ring && lv_obj_is_valid(s_ring)) lv_obj_del(s_ring);
    for (int i = 0; i < 8; i++) {
        if (s_handles[i] && lv_obj_is_valid(s_handles[i])) lv_obj_del(s_handles[i]);
        s_handles[i] = NULL;
    }
    if (s_delete_modal && lv_obj_is_valid(s_delete_modal)) lv_obj_del(s_delete_modal);
    s_delete_modal            = NULL;
    /* Top toolbar lived on the old screen — already gone. Forget the
     * cached pointers; undo ring is layout-scoped, so reset it too. */
    s_top_toolbar             = NULL;
    s_undo_btn                = NULL;
    s_redo_btn                = NULL;
    s_dup_btn                 = NULL;
    s_del_btn                 = NULL;
    s_tt_dragging             = false;
    _undo_clear_ring();
    if (s_snap_timer) { lv_timer_del(s_snap_timer); s_snap_timer = NULL; }
    s_pill                    = NULL;
    s_pill_lbl                = NULL;
    s_banner                  = NULL;
    s_ring                    = NULL;
    s_resizing                = false;
    s_resize_dir              = -1;
    s_toolbar                 = NULL;
    s_step_btns[0]            = NULL;
    s_step_btns[1]            = NULL;
    s_step_btns[2]            = NULL;
    s_chip_btns[0]            = NULL;
    s_chip_btns[1]            = NULL;
    s_chip_btns[2]            = NULL;
    s_chip_btns[3]            = NULL;
    s_chip_popover            = NULL;
    s_popover_slider          = NULL;
    s_popover_value           = NULL;
    s_popover_target          = 0;
    s_popover_syncing         = false;
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
    /* Use the wider edit-pill size so "Exit Edit Mode" doesn't truncate. */
    lv_obj_set_size(s_pill, DT_PILL_EDIT_W, DT_PILL_H);
    /* Position the right edge so the Menu pill (DT_PILL_W wide, 12 px from
     * the screen's right edge) sits flush to our right with DT_PILL_GAP between. */
    lv_obj_align(s_pill, LV_ALIGN_TOP_RIGHT,
                 -(DT_PILL_EDIT_W + DT_PILL_GAP + 12), 12);
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
    /* Suppressed while armed — the top toolbar hosts Exit now, and showing
     * the pill again on every dashboard tap creates a stale duplicate that
     * users (rightly) read as broken. */
    if (s_armed) return;
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

    /* Show the selection-toolbar at the bottom. Top toolbar (status + undo)
     * is already up. */
    if (s_pill) {
        lv_obj_t *parent = lv_obj_get_parent(s_pill);
        if (parent) _build_toolbar(parent);
    }
    _update_readout();
    _refresh_undo_redo_styling();

    /* Newly-built ring + handles + bottom toolbar were appended to ui_Screen3
     * children in arbitrary order — restore the canonical stack so the ring
     * sits behind the toolbars (per user spec) and the BUS SILENT badge
     * stays on top of everything. */
    edit_mode_refresh_zorder();
    ui_Screen3_refresh_overlays();
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

    int new_x = s_drag_start_widget_x + dx;
    int new_y = s_drag_start_widget_y + dy;

    /* Live grid snap during drag — same step the user picked in the toolbar
     * (1 / 5 / 10 px). The widget visibly clicks into grid positions as
     * the finger moves, which makes aligning multiple widgets predictable
     * and stops the small "settle" jump that would otherwise happen only
     * on release. Step = 1 disables the snap (pixel-precise drag). */
    int step = (s_step > 0) ? s_step : 5;
    if (step > 1) {
        new_x = (new_x / step) * step;
        new_y = (new_y / step) * step;
    }

    w->x = (int16_t)new_x;
    w->y = (int16_t)new_y;
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

void edit_mode_refresh_zorder(void) {
    /* Order matters — lv_obj_move_foreground sends to the END of the parent's
     * child list, so the LAST call ends up on top. Layering (bottom→top):
     *   widgets  <  ring  <  handles  <  top toolbar  <  bottom toolbar  <  badge
     * Badge is handled by ui_Screen3_refresh_overlays (different module). */
    if (s_ring && lv_obj_is_valid(s_ring))
        lv_obj_move_foreground(s_ring);
    for (int i = 0; i < 8; i++) {
        if (s_handles[i] && lv_obj_is_valid(s_handles[i]))
            lv_obj_move_foreground(s_handles[i]);
    }
    if (s_top_toolbar && lv_obj_is_valid(s_top_toolbar))
        lv_obj_move_foreground(s_top_toolbar);
    if (s_toolbar && lv_obj_is_valid(s_toolbar))
        lv_obj_move_foreground(s_toolbar);
}
