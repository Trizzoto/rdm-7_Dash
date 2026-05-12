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
#include "ui/theme.h"
#include "ui/ui.h"                /* ui_Menu_Button extern */
#include "storage/config_store.h" /* edit step px persistence */
#include "esp_log.h"
#include <stdio.h>                /* snprintf */
#include <stdlib.h>               /* abs, atoi */
#include <stdint.h>               /* intptr_t */

static const char *TAG = "edit_mode";

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void _destroy_toolbar(void);
static void _update_readout(void);
static void _refresh_step_styling(void);
static void _close_popover(void);
static void _update_popover(void);
static void _open_popover(char target);
static void _chip_clicked_cb(lv_event_t *e);

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
static lv_obj_t  *s_chip_btns[4]  = {NULL};   /* x, y, w, h — clickable */

/* Adjustment popover — opens above the toolbar when a chip is tapped.
 * Lets the user drag a slider OR tap the value to open the numeric keypad.
 * Stays compact (280x80) so widgets remain visible behind it. */
static lv_obj_t  *s_chip_popover   = NULL;
static lv_obj_t  *s_popover_slider = NULL;
static lv_obj_t  *s_popover_value  = NULL;
static char       s_popover_target = 0;   /* 'x' / 'y' / 'w' / 'h', or 0 */
static bool       s_popover_syncing= false;  /* re-entry guard for slider sync */

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
     *   max (lowest): -TB_MIN_GAP        → 4 px above bottom edge
     *   min (highest): -(480-56-TB_MIN_GAP) = -420 → 4 px below top edge
     * Clamp keeps the toolbar fully visible regardless of drag distance. */
    int new_y = s_tb_drag_start_y + dy;
    if (new_y > -TB_MIN_GAP)      new_y = -TB_MIN_GAP;
    if (new_y < -(480 - 56 - TB_MIN_GAP))
                                  new_y = -(480 - 56 - TB_MIN_GAP);
    s_toolbar_y_off = (int16_t)new_y;
    lv_obj_align(s_toolbar, LV_ALIGN_BOTTOM_MID, 0, s_toolbar_y_off);
}

/* Helper: web-style button — translucent surface, subtle light border, no
 * shadow. Mirrors the `.wst-btn` look from main/web/index.html. */
static lv_obj_t *_make_tbtn(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                            const char *text) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    /* Resting: very translucent inset surface — rgba(255,255,255,~0.04). */
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, 10, 0);
    /* Pressed: brighter highlight — rgba(255,255,255,~0.12). */
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, 30, LV_STATE_PRESSED);
    /* Thin light border for definition. */
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_opa(b, 20, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, DT_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, DT_TEXT_PRIMARY, 0);
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

static void _popover_keypad_confirmed(const char *text, void *user_data) {
    (void)user_data;
    if (!text || s_popover_target == 0) return;
    int v = atoi(text);
    _set_widget_field(s_popover_target, v);
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
    show_numeric_input_dialog(title, initial, _popover_keypad_confirmed, NULL);
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
    lv_obj_set_size(s_chip_popover, 300, 84);
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
    int popover_top = tb_area.y1 - 84 - 8;
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

    /* Slider — full width at the bottom */
    s_popover_slider = lv_slider_create(s_chip_popover);
    lv_obj_set_size(s_popover_slider, 276, 12);
    lv_obj_align(s_popover_slider, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(s_popover_slider, min_v, max_v);
    lv_slider_set_value(s_popover_slider, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_popover_slider, DT_BG_INSET, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_popover_slider, DT_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_popover_slider, DT_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_popover_slider, _popover_slider_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

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
    lv_obj_set_size(s_toolbar, 720, 56);
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

    /* Step toggle — 3-segment chip group. Each chip is a small _make_tbtn
     * button with rounded corners; _refresh_step_styling colours the active
     * one accent. Web-style: small gap between segments rather than a hard
     * shared border. */
    lv_obj_t *steps = lv_obj_create(s_toolbar);
    lv_obj_set_size(steps, 138, 38);
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
        lv_obj_t *b = _make_tbtn(steps, 44, 36, step_labels[i]);
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
    lv_obj_set_size(chip_grp, 252, 36);
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
        lv_obj_t *c = _make_tbtn(chip_grp, 60, 32, "");
        lv_obj_add_event_cb(c, _chip_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)chip_targets[i]);
        s_chip_btns[i] = c;
    }
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
    _close_popover();   /* never have a popover without its toolbar */
    if (s_toolbar && lv_obj_is_valid(s_toolbar)) lv_obj_del(s_toolbar);
    s_toolbar      = NULL;
    s_step_btns[0] = s_step_btns[1] = s_step_btns[2] = NULL;
    s_chip_btns[0] = s_chip_btns[1] = s_chip_btns[2] = s_chip_btns[3] = NULL;
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
