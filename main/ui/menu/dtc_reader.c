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
 * UI structure — a kit popup (640 x 400 card, 18 px padding, so the
 * content area is 604 x 364; y below is inside that padding):
 *   ┌──────────────────────────────────────┐
 *   │ TROUBLE CODES                    [X] │ uk_popup title + close
 *   │ [Stored] [Pending] [Permanent]       │ y 56, tabs (on = UK_BTN_ON)
 *   │  P0420  Catalyst Efficiency...       │ y 104, scrollable list of
 *   │  P0301  Cylinder 1 Misfire           │        neutral rows
 *   │  status line                         │ y 300
 *   │  [Read again]         [Clear codes]  │ y 324, footer buttons
 *   └──────────────────────────────────────┘
 */
#include "dtc_reader.h"

#include "obd2.h"
#include "obd2_dtc_db.h"
#include "dtc_monitor.h"
#include "theme.h"
#include "kit/ui_kit.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MODAL_W   640
#define MODAL_H   400
#define INNER_W   (MODAL_W - 36)    /* card padding is 18 each side */
#define INNER_H   (MODAL_H - 36)
#define TABS_Y    UK_POPUP_BODY_Y
#define TAB_W     120
#define TAB_H     36
#define LIST_Y    (TABS_Y + TAB_H + 12)
#define FOOTER_Y  (INNER_H - UK_BTN_H)
#define STATUS_Y  (FOOTER_Y - 24)
#define LIST_H    (STATUS_Y - 8 - LIST_Y)
#define ROW_H      34

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

/* The kit popup's card. uk_popup's backdrop is a sibling that goes with it
 * when this is deleted, so this one pointer is the whole modal. */
static lv_obj_t       *s_overlay     = NULL;
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
static void _row_click_cb(lv_event_t *e);

/* ── Freeze-frame sub-modal state ────────────────────────────────────
 *
 * Tap a DTC row → opens a small overlay that fires a sequence of
 * Mode 02 queries for the standard set of "snapshot" PIDs (RPM, speed,
 * coolant temp, load, throttle, MAP, MAF, fuel level, run time). Each
 * comes back individually; the panel updates row-by-row as data
 * arrives. Some ECUs reject many PIDs with NRC — those show "-". */

#define FF_MAX_ROWS 12

/* Standard set of freeze-frame PIDs we query. Order = render order. */
typedef struct {
    uint8_t     pid;
    const char *label;
    const char *unit;
    /* decode: same scale/offset/bytes as Mode 01 for that PID. */
    uint8_t     bytes;
    float       scale;
    float       offset;
    int         decimals;
} ff_spec_t;

static const ff_spec_t FF_PIDS[] = {
    { 0x04, "Engine load",     "%",     1, 0.392157f,  0.0f,    1 },
    { 0x05, "Coolant temp",    "degC",  1, 1.0f,       -40.0f,  0 },
    { 0x0B, "MAP",             "kPa",   1, 1.0f,        0.0f,   0 },
    { 0x0C, "RPM",             "rpm",   2, 0.25f,       0.0f,   0 },
    { 0x0D, "Vehicle speed",   "km/h",  1, 1.0f,        0.0f,   0 },
    { 0x0E, "Timing adv",      "deg",   1, 0.5f,       -64.0f,  1 },
    { 0x0F, "Intake air temp", "degC",  1, 1.0f,       -40.0f,  0 },
    { 0x10, "MAF",             "g/s",   2, 0.01f,       0.0f,   2 },
    { 0x11, "Throttle pos",    "%",     1, 0.392157f,   0.0f,   1 },
    { 0x1F, "Run time",        "s",     2, 1.0f,        0.0f,   0 },
    { 0x2F, "Fuel level",      "%",     1, 0.392157f,   0.0f,   1 },
};
#define FF_PID_COUNT (sizeof(FF_PIDS) / sizeof(FF_PIDS[0]))

static lv_obj_t *s_ff_overlay        = NULL;
static lv_obj_t *s_ff_value_lbls[FF_PID_COUNT] = {0};
static uint8_t   s_ff_dtc_for_modal[6] = {0};   /* "P0420" */
static uint8_t   s_ff_index            = 0;     /* next PID to query */
static bool      s_ff_running          = false;

static void _ff_open(const char *code);
static void _ff_close(void);
static void _ff_close_cb(lv_event_t *e);
static void _ff_kick_next(void);
static void _ff_on_response(bool ok, const uint8_t *raw, uint8_t raw_len,
                             void *user);

/* ── Public API ────────────────────────────────────────────────────────── */

bool dtc_reader_is_open(void) { return s_overlay != NULL; }

void dtc_reader_close(void) {
    if (!s_overlay) return;
    if (s_clear_timer) {
        lv_timer_del(s_clear_timer);
        s_clear_timer = NULL;
    }
    /* Tear down freeze-frame sub-modal if it's open — same safety pattern
     * as the existing dump overlay handling. */
    _ff_close();
    if (lv_obj_is_valid(s_overlay)) lv_obj_del(s_overlay);   /* takes the backdrop too */
    s_overlay     = NULL;
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

    /* Kit popup on lv_layer_top, above the dashboard and any settings
     * underneath; its X calls _close_btn_cb. */
    s_overlay = uk_popup(MODAL_W, MODAL_H, "Trouble codes", _close_btn_cb);

    /* ── Tabs ── the showing bucket reads as "on". */
    for (int i = 0; i < TAB_COUNT; i++) {
        s_tabs[i] = uk_btn(s_overlay, UK_ICON_NONE, TAB_NAME[i],
                           (i == s_current_tab) ? UK_BTN_ON : UK_BTN_NEUTRAL,
                           _tab_cb, (void *)(intptr_t)i);
        lv_obj_set_size(s_tabs[i], TAB_W, TAB_H);
        lv_obj_align(s_tabs[i], LV_ALIGN_TOP_LEFT, i * (TAB_W + 8), TABS_Y);
    }

    /* ── List body ── */
    s_list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, INNER_W, LIST_H);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, LIST_Y);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* ── Status line ── */
    s_status = uk_label(s_overlay, "Reading stored codes...", UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 0, STATUS_Y);

    /* ── Footer ── */
    s_refresh_btn = uk_btn(s_overlay, UK_ICON_RESET, "Read again", UK_BTN_NEUTRAL,
                           _refresh_btn_cb, NULL);
    lv_obj_align(s_refresh_btn, LV_ALIGN_TOP_LEFT, 0, FOOTER_Y);

    s_clear_btn = uk_btn(s_overlay, UK_ICON_TRASH, "Clear codes", UK_BTN_DANGER,
                         _clear_btn_cb, NULL);
    lv_obj_set_width(s_clear_btn, 200);
    lv_obj_align(s_clear_btn, LV_ALIGN_TOP_RIGHT, 0, FOOTER_Y);
    s_clear_lbl = uk_btn_label(s_clear_btn);

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
    /* Move the "on" look to the chosen tab. */
    for (int i = 0; i < TAB_COUNT; i++) {
        if (!s_tabs[i] || !lv_obj_is_valid(s_tabs[i])) continue;
        uk_btn_set_kind(s_tabs[i],
                        (i == s_current_tab) ? UK_BTN_ON : UK_BTN_NEUTRAL);
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
                          armed ? "Tap again to clear" : "Clear codes");
    }
    /* Armed = solid red fill, so the second tap is clearly the one that
     * does it; resting = the kit's soft danger look. */
    uk_btn_set_kind(s_clear_btn, armed ? UK_BTN_PRIMARY : UK_BTN_DANGER);
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
        uk_label(s_list, "Reading...", UK_FONT_SMALL, UK_TONE_MUTED);
        return;
    }
    if (!b->loaded) return;
    if (b->count == 0) {
        uk_label(s_list,
            b->last_ok
                ? "No trouble codes in this list."
                : "No answer from the car (engine off, or not supported).",
            UK_FONT_SMALL, UK_TONE_MUTED);
        return;
    }

    for (uint8_t i = 0; i < b->count; i++) {
        /* A neutral row: raised fill, button radius. */
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), ROW_H);
        lv_obj_set_style_bg_color(row, THEME_COLOR_CONTROL_BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UK_R_BTN, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Code column — fixed width, condensed caps so it reads as the key. */
        lv_obj_t *code_lbl = uk_label(row, b->codes[i].code, UK_FONT_LABEL, UK_TONE_TEXT);
        lv_obj_align(code_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* Description column — looked up from offline DB. */
        const char *desc = obd2_dtc_lookup(b->codes[i].code);
        lv_obj_t *desc_lbl = uk_label(row, desc ? desc : "(no description on file)",
                                      UK_FONT_SMALL,
                                      desc ? UK_TONE_TEXT : UK_TONE_MUTED);
        lv_obj_set_width(desc_lbl, INNER_W - 20 - 70);
        lv_label_set_long_mode(desc_lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(desc_lbl, LV_ALIGN_LEFT_MID, 70, 0);

        /* Tap-to-expand → freeze frame sub-modal. Stored DTCs only; the
         * ECU normally only retains a freeze frame for confirmed faults
         * (Mode 03), so wiring it on pending/permanent rows would just
         * mostly produce "no data" panels. user_data is a small heap
         * copy of the code so the event handler doesn't need to walk
         * back to s_buckets to find it. */
        if (s_current_tab == TAB_STORED) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            /* Pass the index as user_data (cheap) and look it up in the
             * handler — saves a per-row malloc. */
            lv_obj_add_event_cb(row, _row_click_cb, LV_EVENT_CLICKED,
                                 (void *)(intptr_t)i);
            /* Press feedback: the kit's second neutral step. */
            lv_obj_set_style_bg_color(row, THEME_COLOR_BTN_DIM_PRESSED,
                                      LV_STATE_PRESSED);
        }
    }
}

static void _row_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bucket_state_t *b = &s_buckets[s_current_tab];
    if (idx < 0 || idx >= b->count) return;
    _ff_open(b->codes[idx].code);
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

/* ── Freeze-frame sub-modal ──────────────────────────────────────────
 *
 * A 420 x 390 kit popup shown above the DTC reader card.
 * Fires the FF_PIDS sequence one at a time and updates each row as
 * data arrives. Each PID slot starts as "..." and resolves to either
 * a decoded value or "-" (the ECU didn't have a freeze frame for it).
 *
 * obd2_read_freeze_pid is single-in-flight; we chain them via the
 * callback. Total wall clock: ~11 PIDs × <100 ms = ~1 s typical, up
 * to ~6.6 s worst case if every PID hits the 600 ms timeout. */

static int32_t _ff_decode(const ff_spec_t *s,
                          const uint8_t *raw, uint8_t raw_len) {
    if (raw_len < s->bytes) return INT32_MIN;
    int32_t v = 0;
    for (uint8_t i = 0; i < s->bytes; i++) v = (v << 8) | raw[i];
    return v;
}

static void _ff_on_response(bool ok, const uint8_t *raw, uint8_t raw_len,
                             void *user) {
    (void)user;
    if (!s_ff_running || !s_ff_overlay) return;
    if (s_ff_index >= FF_PID_COUNT) return;

    const ff_spec_t *s = &FF_PIDS[s_ff_index];
    lv_obj_t *lbl = s_ff_value_lbls[s_ff_index];

    if (ok && raw && raw_len >= s->bytes && lbl && lv_obj_is_valid(lbl)) {
        int32_t raw_v = _ff_decode(s, raw, raw_len);
        float value  = (float)raw_v * s->scale + s->offset;
        char buf[24];
        snprintf(buf, sizeof(buf), "%.*f %s",
                 s->decimals, (double)value, s->unit);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    } else if (lbl && lv_obj_is_valid(lbl)) {
        lv_label_set_text(lbl, "-");
        lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_HINT, 0);
    }

    s_ff_index++;
    _ff_kick_next();
}

static void _ff_kick_next(void) {
    if (!s_ff_running) return;
    if (s_ff_index >= FF_PID_COUNT) {
        s_ff_running = false;
        return;
    }
    /* frame_no = 0 — most ECUs only store the first/only freeze frame. */
    obd2_read_freeze_pid(FF_PIDS[s_ff_index].pid, 0,
                         _ff_on_response, NULL);
}

static void _ff_close(void) {
    s_ff_running = false;
    s_ff_index   = 0;
    if (s_ff_overlay && lv_obj_is_valid(s_ff_overlay)) {
        lv_obj_del(s_ff_overlay);
    }
    s_ff_overlay = NULL;
    for (size_t i = 0; i < FF_PID_COUNT; i++) s_ff_value_lbls[i] = NULL;
}

static void _ff_close_cb(lv_event_t *e) {
    (void)e;
    _ff_close();
}

static void _ff_open(const char *code) {
    /* Tear down any previous sub-modal — fire-twice resilience. */
    _ff_close();

    strncpy((char *)s_ff_dtc_for_modal, code,
            sizeof(s_ff_dtc_for_modal) - 1);

    /* A second kit popup. Created after the reader's, so its backdrop sits
     * over the reader card and it reads as "on top of" it; the X calls
     * _ff_close_cb. s_ff_overlay is the card (backdrop goes with it). */
    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), "Freeze frame %s", code);
    s_ff_overlay = uk_popup(420, 390, title_buf, _ff_close_cb);

    /* Hint */
    lv_obj_t *hint = uk_label(s_ff_overlay,
        "Readings from the moment this code was set. "
        "A dash means the car kept no reading for it.",
        UK_FONT_SMALL, UK_TONE_MUTED);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, 420 - 36);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    /* Two-column layout: labels on left, values on right. */
    int row_y0 = UK_POPUP_BODY_Y + 44;
    int row_h  = 22;
    for (size_t i = 0; i < FF_PID_COUNT; i++) {
        lv_obj_t *lbl = uk_label(s_ff_overlay, FF_PIDS[i].label,
                                 UK_FONT_SMALL, UK_TONE_MUTED);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, row_y0 + (int)i * row_h);

        s_ff_value_lbls[i] = uk_label(s_ff_overlay, "...", UK_FONT_SMALL, UK_TONE_HINT);
        lv_obj_align(s_ff_value_lbls[i], LV_ALIGN_TOP_LEFT,
                     180, row_y0 + (int)i * row_h);
    }

    /* Kick off the sequence. */
    s_ff_running = true;
    s_ff_index   = 0;
    _ff_kick_next();
}
