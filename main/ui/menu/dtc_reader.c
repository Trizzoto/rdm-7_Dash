/**
 * dtc_reader.c — Code Reader modal (see dtc_reader.h).
 *
 * Three buckets (Stored / Pending / Permanent) shown in three tabs. On
 * open we kick off Mode 03 immediately so the user sees results without
 * tapping Refresh. Tab switch lazily fires the matching mode if that
 * bucket hasn't been fetched yet (or use Refresh to re-poll all three).
 *
 * Clear Codes button is two-tap: first click flips to a "Tap again to
 * confirm" state with a different color; tapping any other control or
 * waiting 3 s cancels. On confirm, Mode 04 fires.
 *
 * UI structure (640 × 400 card):
 *   ┌──────────────────────────────────────┐
 *   │ Trouble Codes                  Close │ header
 *   ├──────────────────────────────────────┤
 *   │ [Stored] [Pending] [Permanent]       │ tab bar
 *   ├──────────────────────────────────────┤
 *   │  P0420  Catalyst Efficiency...       │ scrollable list
 *   │  P0301  Cylinder 1 Misfire           │
 *   ├──────────────────────────────────────┤
 *   │  status line                         │
 *   ├──────────────────────────────────────┤
 *   │  [Refresh]      [Clear Codes]        │ footer
 *   └──────────────────────────────────────┘
 */
#include "dtc_reader.h"

#include "obd2.h"
#include "obd2_dtc_db.h"
#include "dtc_monitor.h"
#include "theme.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MODAL_W   640
#define MODAL_H   400
#define HEADER_H   34
#define TABBAR_H   30
#define STATUS_H   22
#define FOOTER_H   42
#define ROW_H      26

#define CLEAR_CONFIRM_MS 3000   /* clear-button confirmation window */

typedef enum {
    TAB_STORED    = 0,
    TAB_PENDING   = 1,
    TAB_PERMANENT = 2,
    TAB_COUNT     = 3,
} dtc_tab_t;

static const uint8_t TAB_MODE[TAB_COUNT] = { 0x03, 0x07, 0x0A };
static const char   *TAB_NAME[TAB_COUNT] = { "Stored", "Pending", "Permanent" };

/* Per-bucket state — last fetch result. */
typedef struct {
    bool        loaded;          /* request completed (ok or empty) */
    bool        in_flight;       /* request issued, awaiting callback */
    bool        last_ok;         /* false => ECU rejected or timed out */
    uint8_t     count;
    obd2_dtc_t  codes[OBD2_MAX_DTCS];
} bucket_state_t;

static lv_obj_t       *s_overlay     = NULL;
static lv_obj_t       *s_card        = NULL;
static lv_obj_t       *s_tabs[TAB_COUNT] = {0};
static lv_obj_t       *s_list        = NULL;
static lv_obj_t       *s_status      = NULL;
static lv_obj_t       *s_refresh_btn = NULL;
static lv_obj_t       *s_clear_btn   = NULL;
static lv_obj_t       *s_clear_lbl   = NULL;
static lv_timer_t     *s_clear_timer = NULL;

static dtc_tab_t        s_current_tab = TAB_STORED;
static bucket_state_t   s_buckets[TAB_COUNT] = {0};
static bool             s_clear_armed = false;   /* second tap will fire Mode 04 */

/* Forward decls */
static void _close_btn_cb(lv_event_t *e);
static void _tab_cb(lv_event_t *e);
static void _refresh_btn_cb(lv_event_t *e);
static void _clear_btn_cb(lv_event_t *e);
static void _render_list(void);
static void _set_status(const char *text);
static void _set_status_fmt(const char *fmt, ...);
static void _fetch_tab(dtc_tab_t tab);
static void _on_dtc_response(bool ok, const obd2_dtc_t *codes, uint8_t count,
                              uint8_t mode, void *user);
static void _on_clear_response(bool ok, void *user);
static void _clear_disarm_cb(lv_timer_t *t);
static void _clear_button_set_state(bool armed);

/* ── Public API ────────────────────────────────────────────────────────── */

bool dtc_reader_is_open(void) { return s_overlay != NULL; }

void dtc_reader_close(void) {
    if (!s_overlay) return;
    if (s_clear_timer) {
        lv_timer_del(s_clear_timer);
        s_clear_timer = NULL;
    }
    lv_obj_del(s_overlay);
    s_overlay     = NULL;
    s_card        = NULL;
    s_list        = NULL;
    s_status      = NULL;
    s_refresh_btn = NULL;
    s_clear_btn   = NULL;
    s_clear_lbl   = NULL;
    for (int i = 0; i < TAB_COUNT; i++) s_tabs[i] = NULL;
    s_clear_armed = false;
    /* Bucket cache cleared so a re-open fetches fresh data — DTCs can
     * change between visits (codes set by drive cycle, cleared via the
     * Clear button, etc.). */
    memset(s_buckets, 0, sizeof(s_buckets));
}

void dtc_reader_open(void) {
    if (s_overlay) return;

    /* Full-screen dimmer overlay, sits on lv_layer_top so it appears
     * above the dashboard and any Device Settings underneath. */
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Card */
    s_card = lv_obj_create(s_overlay);
    lv_obj_set_size(s_card, MODAL_W, MODAL_H);
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_radius(s_card, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Header ── */
    lv_obj_t *header = lv_obj_create(s_card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, MODAL_W, HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Trouble Codes");
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 60, 28);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_BTN_DIM, 0);
    lv_obj_set_style_radius(close_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_font(close_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(close_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(close_btn, _close_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ── Tab bar ── */
    lv_obj_t *tabbar = lv_obj_create(s_card);
    lv_obj_remove_style_all(tabbar);
    lv_obj_set_size(tabbar, MODAL_W, TABBAR_H);
    lv_obj_align(tabbar, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_set_style_bg_color(tabbar, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(tabbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tabbar, LV_OBJ_FLAG_SCROLLABLE);

    int tab_w = 100;
    int tab_x = 12;
    for (int i = 0; i < TAB_COUNT; i++) {
        s_tabs[i] = lv_btn_create(tabbar);
        lv_obj_set_size(s_tabs[i], tab_w, 24);
        lv_obj_align(s_tabs[i], LV_ALIGN_LEFT_MID, tab_x, 0);
        lv_obj_set_style_bg_color(s_tabs[i],
            (i == s_current_tab) ? THEME_COLOR_ACCENT_BLUE
                                  : THEME_COLOR_BTN_DIM, 0);
        lv_obj_set_style_radius(s_tabs[i], THEME_RADIUS_SMALL, 0);
        lv_obj_set_style_shadow_width(s_tabs[i], 0, 0);
        lv_obj_t *tlbl = lv_label_create(s_tabs[i]);
        lv_label_set_text(tlbl, TAB_NAME[i]);
        lv_obj_center(tlbl);
        lv_obj_set_style_text_font(tlbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(tlbl,
            (i == s_current_tab) ? THEME_COLOR_TEXT_ON_ACCENT
                                  : THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_add_event_cb(s_tabs[i], _tab_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        tab_x += tab_w + 6;
    }

    /* ── List body ── */
    int list_y = HEADER_H + TABBAR_H;
    int list_h = MODAL_H - HEADER_H - TABBAR_H - STATUS_H - FOOTER_H;
    s_list = lv_obj_create(s_card);
    lv_obj_set_size(s_list, MODAL_W, list_h);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, list_y);
    lv_obj_set_style_bg_color(s_list, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 1, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* ── Status line ── */
    s_status = lv_label_create(s_card);
    lv_label_set_text(s_status, "Reading stored codes...");
    lv_obj_set_style_text_font(s_status, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 12, list_y + list_h + 4);

    /* ── Footer ── */
    lv_obj_t *footer = lv_obj_create(s_card);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, MODAL_W, FOOTER_H);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(footer, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    s_refresh_btn = lv_btn_create(footer);
    lv_obj_set_size(s_refresh_btn, 100, 30);
    lv_obj_align(s_refresh_btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(s_refresh_btn, THEME_COLOR_BTN_DIM, 0);
    lv_obj_set_style_radius(s_refresh_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(s_refresh_btn, 0, 0);
    lv_obj_t *rlbl = lv_label_create(s_refresh_btn);
    lv_label_set_text(rlbl, "Refresh");
    lv_obj_center(rlbl);
    lv_obj_set_style_text_font(rlbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(rlbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(s_refresh_btn, _refresh_btn_cb, LV_EVENT_CLICKED, NULL);

    s_clear_btn = lv_btn_create(footer);
    lv_obj_set_size(s_clear_btn, 130, 30);
    lv_obj_align(s_clear_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(s_clear_btn, THEME_COLOR_BTN_CANCEL, 0);
    lv_obj_set_style_radius(s_clear_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(s_clear_btn, 0, 0);
    s_clear_lbl = lv_label_create(s_clear_btn);
    lv_label_set_text(s_clear_lbl, "Clear Codes");
    lv_obj_center(s_clear_lbl);
    lv_obj_set_style_text_font(s_clear_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_clear_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(s_clear_btn, _clear_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Fire the initial fetch for the visible tab. */
    _fetch_tab(s_current_tab);
}

/* ── Event handlers ──────────────────────────────────────────────────── */

static void _close_btn_cb(lv_event_t *e) {
    (void)e;
    dtc_reader_close();
}

static void _tab_cb(lv_event_t *e) {
    dtc_tab_t tab = (dtc_tab_t)(intptr_t)lv_event_get_user_data(e);
    if (tab < 0 || tab >= TAB_COUNT) return;
    if (tab == s_current_tab) return;

    s_current_tab = tab;
    /* Re-color all tab buttons. */
    for (int i = 0; i < TAB_COUNT; i++) {
        if (!s_tabs[i] || !lv_obj_is_valid(s_tabs[i])) continue;
        bool sel = (i == s_current_tab);
        lv_obj_set_style_bg_color(s_tabs[i],
            sel ? THEME_COLOR_ACCENT_BLUE : THEME_COLOR_BTN_DIM, 0);
        lv_obj_t *lbl = lv_obj_get_child(s_tabs[i], 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                sel ? THEME_COLOR_TEXT_ON_ACCENT : THEME_COLOR_TEXT_PRIMARY, 0);
        }
    }

    /* Cancel any pending clear-confirm — switching tabs is "doing
     * something else", so it should reset the danger button. */
    _clear_button_set_state(false);

    /* If we haven't fetched this bucket yet, do it now. Otherwise just
     * re-render with cached data. */
    if (!s_buckets[s_current_tab].loaded &&
        !s_buckets[s_current_tab].in_flight) {
        _fetch_tab(s_current_tab);
    } else {
        _render_list();
    }
}

static void _refresh_btn_cb(lv_event_t *e) {
    (void)e;
    _clear_button_set_state(false);
    /* Re-fetch all three buckets so user gets a complete refresh, not
     * just the visible tab. Sequence them so we don't blow up the
     * "single in-flight DTC request" gate in obd2.c — each callback
     * fires the next mode. We kick this off by clearing 'loaded' for
     * all buckets and re-fetching the current tab; the response handler
     * will chain to the next un-loaded bucket. */
    for (int i = 0; i < TAB_COUNT; i++) {
        s_buckets[i].loaded    = false;
        s_buckets[i].in_flight = false;
        s_buckets[i].count     = 0;
    }
    _render_list();
    _fetch_tab(s_current_tab);
}

static void _clear_btn_cb(lv_event_t *e) {
    (void)e;
    if (!s_clear_armed) {
        /* First tap — arm and wait for confirmation. */
        _clear_button_set_state(true);
        if (s_clear_timer) lv_timer_del(s_clear_timer);
        s_clear_timer = lv_timer_create(_clear_disarm_cb,
                                         CLEAR_CONFIRM_MS, NULL);
        lv_timer_set_repeat_count(s_clear_timer, 1);
        return;
    }
    /* Second tap — actually clear. */
    _clear_button_set_state(false);
    _set_status("Clearing codes...");
    obd2_clear_dtcs(_on_clear_response, NULL);
}

static void _clear_disarm_cb(lv_timer_t *t) {
    (void)t;
    s_clear_timer = NULL;
    _clear_button_set_state(false);
}

static void _clear_button_set_state(bool armed) {
    s_clear_armed = armed;
    if (!s_clear_btn || !lv_obj_is_valid(s_clear_btn)) return;
    if (s_clear_lbl && lv_obj_is_valid(s_clear_lbl)) {
        lv_label_set_text(s_clear_lbl,
                          armed ? "Confirm Clear?" : "Clear Codes");
    }
    /* Color: armed = even more red to nudge the user to think. */
    lv_obj_set_style_bg_color(s_clear_btn,
        armed ? THEME_COLOR_BTN_CLOSE : THEME_COLOR_BTN_CANCEL, 0);
    if (!armed && s_clear_timer) {
        lv_timer_del(s_clear_timer);
        s_clear_timer = NULL;
    }
}

/* ── Fetch + render ──────────────────────────────────────────────────── */

static void _fetch_tab(dtc_tab_t tab) {
    if (tab < 0 || tab >= TAB_COUNT) return;
    if (s_buckets[tab].in_flight) return;
    s_buckets[tab].in_flight = true;
    s_buckets[tab].loaded    = false;
    _set_status_fmt("Reading %s codes...", TAB_NAME[tab]);

    switch (TAB_MODE[tab]) {
        case 0x03: obd2_read_stored_dtcs(_on_dtc_response, NULL);   break;
        case 0x07: obd2_read_pending_dtcs(_on_dtc_response, NULL);  break;
        case 0x0A: obd2_read_permanent_dtcs(_on_dtc_response, NULL); break;
    }
}

static dtc_tab_t _tab_for_mode(uint8_t mode) {
    for (int i = 0; i < TAB_COUNT; i++) {
        if (TAB_MODE[i] == mode) return (dtc_tab_t)i;
    }
    return TAB_STORED;
}

static void _on_dtc_response(bool ok, const obd2_dtc_t *codes, uint8_t count,
                              uint8_t mode, void *user) {
    (void)user;
    if (!s_overlay) return;   /* modal closed before callback fired */

    dtc_tab_t tab = _tab_for_mode(mode);
    bucket_state_t *b = &s_buckets[tab];
    b->in_flight = false;
    b->loaded    = true;
    b->last_ok   = ok;
    b->count     = (count > OBD2_MAX_DTCS) ? OBD2_MAX_DTCS : count;
    if (ok && codes && b->count > 0) {
        memcpy(b->codes, codes, b->count * sizeof(obd2_dtc_t));
    } else {
        b->count = 0;
    }

    if (tab == s_current_tab) _render_list();

    /* After the visible tab finishes, chain through the other buckets
     * so all three populate quietly in the background. Saves the user
     * having to tap each tab to discover whether anything's there. */
    for (int i = 0; i < TAB_COUNT; i++) {
        if (!s_buckets[i].loaded && !s_buckets[i].in_flight) {
            _fetch_tab((dtc_tab_t)i);
            return;
        }
    }

    /* All buckets done — update status with a summary if user is on
     * a bucket without codes (otherwise the row count speaks for itself). */
    if (s_current_tab < TAB_COUNT) {
        bucket_state_t *cur = &s_buckets[s_current_tab];
        if (!cur->last_ok) {
            _set_status_fmt("%s: no response (engine off, or not supported)",
                            TAB_NAME[s_current_tab]);
        } else if (cur->count == 0) {
            _set_status_fmt("%s: 0 codes", TAB_NAME[s_current_tab]);
        } else {
            _set_status_fmt("%s: %u code(s)", TAB_NAME[s_current_tab], cur->count);
        }
    }
}

static void _on_clear_response(bool ok, void *user) {
    (void)user;
    if (!s_overlay) return;
    if (ok) {
        _set_status("Codes cleared. Re-reading...");
        /* Kick the background DTC monitor so any warning widgets bound
         * to DTC_COUNT drop to 0 right away (vs waiting up to 30 s for
         * the next scheduled poll). The Mode 04 clear path can't reuse
         * this same response — it explicitly only acks the clear. */
        dtc_monitor_refresh_now();

        /* Re-fetch to confirm — many ECUs report ok then keep the codes
         * if conditions weren't right (e.g. engine running). */
        for (int i = 0; i < TAB_COUNT; i++) {
            s_buckets[i].loaded    = false;
            s_buckets[i].in_flight = false;
            s_buckets[i].count     = 0;
        }
        _render_list();
        _fetch_tab(s_current_tab);
    } else {
        _set_status("Clear failed. Try with engine off + ignition on.");
    }
}

/* ── List rendering ──────────────────────────────────────────────────── */

static void _render_list(void) {
    if (!s_list || !lv_obj_is_valid(s_list)) return;

    /* Tear down existing children — simpler than per-row diffing for a
     * list this small (max ~16 DTCs). */
    lv_obj_clean(s_list);

    bucket_state_t *b = &s_buckets[s_current_tab];

    if (b->in_flight) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "...");
        lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
        return;
    }
    if (!b->loaded) return;
    if (b->count == 0) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl,
            b->last_ok
                ? "No trouble codes in this bucket."
                : "No response from ECU (engine off, or not supported).");
        lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, 8);
        return;
    }

    for (uint8_t i = 0; i < b->count; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, lv_pct(100), ROW_H);
        lv_obj_set_style_bg_color(row, (i & 1) ? THEME_COLOR_INPUT_BG
                                                : THEME_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Code column — fixed width, accent color so it pops. */
        lv_obj_t *code_lbl = lv_label_create(row);
        lv_label_set_text(code_lbl, b->codes[i].code);
        lv_obj_set_style_text_font(code_lbl, THEME_FONT_BODY, 0);
        lv_obj_set_style_text_color(code_lbl, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_align(code_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* Description column — looked up from offline DB. */
        const char *desc = obd2_dtc_lookup(b->codes[i].code);
        lv_obj_t *desc_lbl = lv_label_create(row);
        lv_label_set_text(desc_lbl, desc ? desc : "(no description in DB)");
        lv_obj_set_style_text_font(desc_lbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(desc_lbl,
            desc ? THEME_COLOR_TEXT_PRIMARY : THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_set_width(desc_lbl, MODAL_W - 90);
        lv_label_set_long_mode(desc_lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(desc_lbl, LV_ALIGN_LEFT_MID, 70, 0);
    }
}

static void _set_status(const char *text) {
    if (s_status && lv_obj_is_valid(s_status)) {
        lv_label_set_text(s_status, text);
    }
}

static void _set_status_fmt(const char *fmt, ...) {
    if (!s_status || !lv_obj_is_valid(s_status)) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(s_status, buf);
}
