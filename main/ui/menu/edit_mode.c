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
#include "ui/theme.h"
#include "ui/ui.h"             /* ui_Menu_Button extern */
#include "esp_log.h"

static const char *TAG = "edit_mode";

/* ── Module state ─────────────────────────────────────────────────────────── */

static bool       s_armed     = false;
static lv_obj_t  *s_pill      = NULL;
static lv_obj_t  *s_pill_lbl  = NULL;
static lv_obj_t  *s_banner    = NULL;

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
    if (!s_armed && !s_banner) return;   /* idempotent fast path */
    s_armed = false;
    _apply_pill_style_live();
    if (s_pill && lv_obj_is_valid(s_pill))
        lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    _destroy_banner();
    ESP_LOGI(TAG, "Edit Mode exited");
}

lv_obj_t *edit_mode_create_pill(lv_obj_t *parent) {
    if (!parent) return NULL;

    /* If the previous screen was deleted, our cached pointers are stale —
     * forget them and rebuild from scratch on this parent. */
    s_pill     = NULL;
    s_pill_lbl = NULL;
    s_banner   = NULL;
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
