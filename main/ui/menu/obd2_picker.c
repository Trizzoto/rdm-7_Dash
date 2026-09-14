/**
 * obd2_picker.c — OBD2 Signals modal (see obd2_picker.h).
 *
 * Builds a 640x420 kit popup on lv_layer_top() with a scrollable list of
 * OBD2 SIGNALS — one row per signal (not per PID). Single-value PIDs
 * produce one row each; packed PIDs (e.g. Toyota Mode 21 PID 0x80)
 * produce N rows, one per sub-field, all sharing one polled request.
 * Sub-field checkboxes are linked: ticking any one ticks all of them
 * because they ride a single PID poll.
 *
 * Each row shows the live signal value, a (M01:0x05)-style mode/PID
 * tag for protocol visibility, and a "supported" badge after a
 * vehicle scan. Scan also auto-checks every supported signal — the
 * dynamic-preset behaviour. The user un-checks anything they don't
 * want, then hits Save.
 *
 * On Save, the new list is written to the active layout's `obd2_pids`
 * array and obd2_start() is called to restart polling.
 *
 * Conflict handling: if a native ECU preset already registers a signal
 * with the same name as an OBD2 PID (e.g. RPM bound to a CAN broadcast),
 * the row is shown but disabled with an "in preset" badge. Saving never
 * lets a conflicting PID get into the list — the modal filters them out.
 */
#include "obd2_picker.h"
#include "esp_attr.h"

#include "obd2.h"
#include "can_manager.h"
#include "ecu_presets.h"
#include "layout_manager.h"
#include "theme.h"
#include "kit/ui_kit.h"
#include "signal.h"

#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "obd2_picker";

/* Sized small to keep redraw cost down — 46 PID rows + scrolling on top
 * of running OBD2 polling + dashboard widgets behind us. 640x420 (a kit
 * popup: 18 px padding, so 604x384 inside) → list area 224 px tall + 30 px
 * rows means ~7 rows on screen — LVGL still walks the whole tree, but a
 * modal smaller than the screen redraws faster. */
#define MODAL_W  640
#define MODAL_H  420
#define INNER_W  (MODAL_W - 36)
#define INNER_H  (MODAL_H - 36)
#define SCAN_Y   UK_POPUP_BODY_Y
#define LIST_Y   (SCAN_Y + UK_BTN_H + 12)
#define FOOTER_Y (INNER_H - UK_BTN_H)
#define PICKER_LIST_H   (FOOTER_Y - 12 - LIST_Y)
#define ROW_H     30
#define LIVE_REFRESH_MS 400

/* ── State ─────────────────────────────────────────────────────────────── */

/* One row = one signal. Single-value PIDs (Mode 01) produce one row each;
 * packed PIDs (Mode 21 Toyota engine block) produce N rows — one per
 * sub-field — that all share `parent_pid` and check/uncheck together
 * because they ride one polled request. */
typedef struct {
    uint16_t    parent_pid;          /* the PID that produces this signal (16-bit for Mode 22) */
    uint8_t     parent_service;      /* 0x01 = Mode 01, 0x21 = Mode 21 */
    const char *signal_name;         /* registry name (never NULL — packed
                                        sub-fields have their own names) */
    const char *display_label;       /* short human label */
    const char *unit;
    bool        checked;             /* mirrors parent_pid's enabled state */
    bool        provided_by_preset;  /* greyed out, uncheckable */
    bool        supported;           /* parent_pid responded to last scan */
    int16_t     signal_idx;          /* cached registry index or -1 */
    lv_obj_t   *cb;
    lv_obj_t   *row;
    lv_obj_t   *value_lbl;
    lv_obj_t   *badge;
} signal_row_t;

/* Sized for current entry count + reasonable headroom. With 69 today
 * (50 single Mode 01 + 4 Toyota Mode 21 + 9 diesel sub-fields + Toyota
 * 0x80 sub-fields) we have ~27 spare slots for custom PIDs. 96 keeps
 * BSS modest (~3.5 KB at 36 bytes/row) — earlier 128 cap was overkill
 * and contributed to memory pressure during preview-poll-all. */
#define PICKER_MAX_ROWS 96

/* The kit popup's card; its backdrop is deleted along with it. */
static lv_obj_t      *s_overlay    = NULL;
static lv_obj_t      *s_list       = NULL;
static lv_obj_t      *s_status     = NULL;     /* scan status label */
static lv_obj_t      *s_scan_btn   = NULL;
static lv_timer_t    *s_live_timer = NULL;
static EXT_RAM_BSS_ATTR signal_row_t   s_rows[PICKER_MAX_ROWS];
static int            s_row_count  = 0;

/* Snapshot of the enabled PID list at modal open. Encoded (service<<8|pid)
 * tuples. Used to:
 *  - restore polling on Cancel/Close-without-Save (so preview-poll-all
 *    doesn't leave stale wide polling running)
 *  - decide which rows start checked
 * `s_saved` is flipped true by _save_cb so the close handler knows
 * not to restore — Save already pushed the new set. */
static uint32_t s_snapshot[OBD2_MAX_ENABLED];
static uint8_t  s_snapshot_count = 0;
static bool     s_saved = false;

/* Forward decls */
static void  _close_cb(lv_event_t *e);
static void  _save_cb(lv_event_t *e);
static void  _scan_cb(lv_event_t *e);
static void  _checkbox_cb(lv_event_t *e);
static void  _scan_complete(const obd2_scan_result_t *r, void *user);
static void  _build_rows(void);
static bool  _signal_provided_by_preset(const char *signal_name);
static void  _set_status(const char *text);
static void  _refresh_count_status(void);
static void  _live_refresh_cb(lv_timer_t *t);

/* ── Public API ────────────────────────────────────────────────────────── */

bool obd2_picker_is_open(void) { return s_overlay != NULL; }

void obd2_picker_close(void)
{
    if (!s_overlay) return;
    if (s_live_timer) {
        lv_timer_del(s_live_timer);
        s_live_timer = NULL;
    }
    /* No revert needed — preview-poll-all was removed (caused OOM), so
     * polling stays on the user's saved set throughout the modal session.
     * Save (if pressed) is the only path that mutates the polled set,
     * and that path drives obd2_start() with the new list directly. */
    if (lv_obj_is_valid(s_overlay)) lv_obj_del(s_overlay);   /* takes the backdrop too */
    s_overlay = NULL;
    s_list    = NULL;
    s_status  = NULL;
    s_scan_btn = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    s_row_count = 0;
    s_saved = false;
}

void obd2_picker_open(void)
{
    if (s_overlay) return;

    /* Snapshot the currently-enabled set BEFORE we start preview polling,
     * so a Cancel/Close-without-Save can restore it. */
    s_snapshot_count = obd2_get_enabled(s_snapshot, OBD2_MAX_ENABLED);
    s_saved = false;

    /* Polling stays on the user's CURRENTLY-SAVED set while the modal is
     * open. Earlier versions called obd2_start() with all 48 PIDs as a
     * "preview" so users could see which respond — but that spiked the
     * signal registry near MAX_SIGNALS (128) and stacked ISO-TP buffers
     * for packed PIDs, causing OOM crashes on memory-tight builds.
     *
     * The modal still gives clear feedback without preview-polling:
     *   - Live values appear for any row already in the user's saved set
     *   - Scan reveals which PIDs the car SUPPORTS (binary indicator)
     *   - Save commits new selections and starts polling them
     *
     * If a future "test these N rows now" feature is wanted, it should
     * temporarily add JUST those rows to the polling list, not all 48. */

    /* Kit popup. Doesn't dismiss on outside-tap (the backdrop only swallows
     * taps) — users use the X or Cancel (avoids LVGL event-bubbling
     * gymnastics, and matches the other popups in Device Settings). */
    s_overlay = uk_popup(MODAL_W, MODAL_H, "OBD2 signals", _close_cb);

    /* ── Scan strip ── */
    s_scan_btn = uk_btn(s_overlay, UK_ICON_OBD, "Scan the car", UK_BTN_NEUTRAL,
                        _scan_cb, NULL);
    lv_obj_set_width(s_scan_btn, 170);
    lv_obj_align(s_scan_btn, LV_ALIGN_TOP_LEFT, 0, SCAN_Y);

    /* Status sits in a fixed box beside the button, centred vertically by
     * flex so a one-line count and a two-line scan failure both line up. */
    lv_obj_t *status_box = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(status_box);
    lv_obj_set_size(status_box, INNER_W - 170 - 14, UK_BTN_H);
    lv_obj_align(status_box, LV_ALIGN_TOP_LEFT, 170 + 14, SCAN_Y);
    lv_obj_set_flex_flow(status_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(status_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_status = uk_label(status_box, "Scan to find what the car supports.",
                        UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_set_width(s_status, lv_pct(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);

    /* ── List body ──
     * Scrollbar mode OFF intentionally — every drawn scrollbar adds two
     * full-height drawn rects on each frame the user scrolls. The list
     * still scrolls via touch drag; users figure that out fast. */
    s_list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, INNER_W, PICKER_LIST_H);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, LIST_Y);
    lv_obj_set_style_pad_row(s_list, 3, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* ── Footer ── Save is the one thing this popup is for. */
    lv_obj_t *save_btn = uk_btn(s_overlay, UK_ICON_CHECK, "Save", UK_BTN_PRIMARY,
                                _save_cb, NULL);
    lv_obj_set_width(save_btn, 120);
    lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, FOOTER_Y);

    lv_obj_t *cancel_btn = uk_btn(s_overlay, UK_ICON_NONE, "Cancel", UK_BTN_GHOST,
                                  _close_cb, NULL);
    lv_obj_set_width(cancel_btn, 120);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, -132, FOOTER_Y);

    _build_rows();
}

/* ── Row construction ─────────────────────────────────────────────────── */

static bool _signal_provided_by_preset(const char *signal_name)
{
    /* "Provided by preset" = a signal with this name is registered AND has
     * a non-zero can_id (i.e. a real broadcast decode, not an external
     * source like OBD2). Walk the registry, comparing names. */
    int16_t idx = signal_find_by_name(signal_name);
    if (idx < 0) return false;
    signal_t *sig = signal_get_by_index((uint16_t)idx);
    return sig && sig->can_id != 0;
}

/* Show a row as ticked or not: the checkbox's CHECKED state, mirrored onto
 * the row so its soft-accent selected fill follows. */
static void _row_show_checked(signal_row_t *r, bool on)
{
    if (r->cb && lv_obj_is_valid(r->cb)) {
        if (on) lv_obj_add_state(r->cb, LV_STATE_CHECKED);
        else    lv_obj_clear_state(r->cb, LV_STATE_CHECKED);
    }
    if (r->row && lv_obj_is_valid(r->row)) {
        if (on) lv_obj_add_state(r->row, LV_STATE_CHECKED);
        else    lv_obj_clear_state(r->row, LV_STATE_CHECKED);
    }
}

/* Build one row for a given (parent_pid, signal_name, display_label). The
 * caller passes the parent PID (so checkbox state can link across packed
 * sub-fields), the signal name in the registry, and the display label. */
static void _add_row(const obd2_pid_def_t *def,
                     const char *signal_name,
                     const char *display_label,
                     const char *unit,
                     bool checked)
{
    if (s_row_count >= PICKER_MAX_ROWS) return;
    signal_row_t *r = &s_rows[s_row_count++];

    r->parent_pid         = def->pid;
    r->parent_service     = def->service ? def->service : 0x01;
    r->signal_name        = signal_name;
    r->display_label      = display_label;
    r->unit               = unit ? unit : "";
    r->provided_by_preset = _signal_provided_by_preset(signal_name);
    r->checked            = checked && !r->provided_by_preset;
    r->supported          = false;
    r->signal_idx         = signal_find_by_name(signal_name);

    /* A neutral row (raised fill, button radius); a ticked row takes the
     * soft accent fill with accent ink, like every other picker's
     * selection. The row mirrors its checkbox's CHECKED state
     * (_row_show_checked) so the look follows from style selectors. */
    r->row = lv_obj_create(s_list);
    lv_obj_remove_style_all(r->row);
    lv_obj_set_size(r->row, lv_pct(100), ROW_H);
    lv_obj_set_style_bg_color(r->row, THEME_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_color(r->row, THEME_COLOR_ACCENT_DIM, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(r->row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->row, UK_R_BTN, 0);
    lv_obj_set_style_pad_hor(r->row, 8, 0);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* No kit restyle exists for checkboxes: theme tokens directly. */
    r->cb = lv_checkbox_create(r->row);
    lv_obj_align(r->cb, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(r->cb, uk_font(UK_FONT_SMALL), 0);
    lv_obj_set_style_text_color(r->cb, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_color(r->cb, ui_pal->accent_ink, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(r->cb, THEME_COLOR_INPUT_BG, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(r->cb, THEME_COLOR_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(r->cb, THEME_COLOR_BORDER_MED,
                                  LV_PART_INDICATOR);
    lv_obj_set_style_border_color(r->cb, THEME_COLOR_ACCENT,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(r->cb, 1, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r->cb, 4, LV_PART_INDICATOR);

    /* Label = signal name + mode/PID tag, e.g. "RPM  (M01:0x0C)" or
     * "TY_RPM  (M21:0x80)" or "ATF_TEMP_22  (M22:0x115C)" so the user
     * can see at a glance which protocol and PID each signal comes
     * from. Mode 22 shows 4-digit PID. */
    char label[80];
    if (r->parent_service == 0x22) {
        snprintf(label, sizeof(label), "%s  (M22:0x%04X)",
                 display_label, r->parent_pid);
    } else {
        snprintf(label, sizeof(label), "%s  (M%02X:0x%02X)",
                 display_label, r->parent_service,
                 (unsigned)(r->parent_pid & 0xFF));
    }
    lv_checkbox_set_text(r->cb, label);

    if (r->checked) _row_show_checked(r, true);

    if (r->provided_by_preset) {
        lv_obj_add_state(r->cb, LV_STATE_DISABLED);
        _row_show_checked(r, false);
        lv_obj_set_style_text_color(r->cb, THEME_COLOR_TEXT_DISABLED, 0);
    } else {
        lv_obj_add_event_cb(r->cb, _checkbox_cb, LV_EVENT_VALUE_CHANGED, r);
    }

    /* Live value column. Preset-shadowed rows show a dash and never
     * paint — the matching signal_idx points at the broadcast CAN
     * signal, not the OBD2 poll response, so showing its value here
     * would read as "OBD2 is polling and got 6800" when in fact the
     * value came from the ECU broadcast (the "in preset" badge is the
     * truth). _live_refresh_cb also skips these rows. */
    r->value_lbl = uk_label(r->row, "-", UK_FONT_SMALL, UK_TONE_HINT);
    lv_obj_align(r->value_lbl, LV_ALIGN_RIGHT_MID, -80, 0);

    /* Status badge. */
    r->badge = uk_label(r->row, r->provided_by_preset ? "in preset" : "",
                        UK_FONT_TINY, UK_TONE_HINT);
    lv_obj_align(r->badge, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void _build_rows(void)
{
    s_row_count = 0;

    /* Read currently-enabled PID list from the active layout. */
    char layout[LAYOUT_MAX_NAME];
    layout_manager_get_active(layout, sizeof(layout));

    uint32_t enabled[OBD2_MAX_ENABLED] = {0};
    uint8_t enabled_count = 0;
    ecu_preset_read_obd2_pids(layout, enabled, OBD2_MAX_ENABLED, &enabled_count);

    /* For each PID definition (built-in + custom), expand into one row
     * per emitted signal — single-value PIDs produce one row; packed
     * PIDs produce one row per sub-field, all sharing the parent PID.
     * Match enabled state on the encoded (service, pid) tuple so
     * Toyota Mode 21 PID 0x80 doesn't accidentally match Mode 01 PID
     * 0x80 (or any other cross-service collision). */
    uint8_t total_defs = obd2_pid_total_count();
    for (uint8_t i = 0; i < total_defs; i++) {
        const obd2_pid_def_t *def = obd2_pid_at(i);
        if (!def) continue;
        uint32_t def_encoded = obd2_encode_pid(def->service, def->pid);

        bool pid_enabled = false;
        for (uint8_t k = 0; k < enabled_count; k++) {
            if (enabled[k] == def_encoded) { pid_enabled = true; break; }
        }

        if (def->sub_fields && def->sub_field_count > 0) {
            /* Packed PID: one row per sub-field. */
            for (uint8_t j = 0; j < def->sub_field_count; j++) {
                const obd2_subfield_t *sf = &def->sub_fields[j];
                if (!sf->signal_name) continue;
                _add_row(def, sf->signal_name, sf->signal_name, sf->unit,
                         pid_enabled);
            }
        } else if (def->signal_name) {
            /* Single-value PID. */
            _add_row(def, def->signal_name, def->human_name, def->unit,
                     pid_enabled);
        }
    }

    /* Kick off the live-value refresh loop. */
    if (!s_live_timer) {
        s_live_timer = lv_timer_create(_live_refresh_cb, LIVE_REFRESH_MS, NULL);
        _live_refresh_cb(s_live_timer);    /* prime first paint */
    }

    /* Show the live counts in the status line right away so the user can
     * see "X decoders / Y enabled" before they tap Scan. Visible feedback
     * that new firmware adds more PIDs without needing a scan first. */
    _refresh_count_status();
}

/* Pick a sensible decimals count for display based on the magnitude of the
 * value. Keeps the column compact while still readable. */
static int _decimals_for(float v, const char *unit)
{
    /* lambda etc. benefit from 2 decimals always. */
    if (unit && (strcmp(unit, "lambda") == 0)) return 3;
    float a = v < 0 ? -v : v;
    if (a >= 1000.0f) return 0;
    if (a >= 100.0f)  return 0;
    if (a >= 10.0f)   return 1;
    return 2;
}

static void _live_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_overlay) return;

    /* Walk every row and update its live value column from the signal
     * registry. Each row owns its own signal_name now (sub-field rows
     * point at their TY_* names), so no special-case for packed PIDs. */
    for (int i = 0; i < s_row_count; i++) {
        signal_row_t *r = &s_rows[i];
        if (!r->value_lbl || !lv_obj_is_valid(r->value_lbl)) continue;
        /* Preset-shadowed rows: signal_find_by_name resolves to the
         * broadcast CAN signal, not the OBD2 poll response. Painting it
         * here would mis-represent the source. The "in preset" badge
         * already tells the user where the value comes from. */
        if (r->provided_by_preset) continue;

        if (r->signal_idx < 0 && r->signal_name) {
            r->signal_idx = signal_find_by_name(r->signal_name);
            if (r->signal_idx < 0) continue;   /* not registered yet */
        }
        if (r->signal_idx < 0) continue;

        signal_t *sig = signal_get_by_index((uint16_t)r->signal_idx);
        if (!sig) continue;

        char buf[24];
        if (sig->is_stale || sig->last_update_ms == 0) {
            snprintf(buf, sizeof(buf), "%s", "...");
        } else {
            int d = _decimals_for(sig->current_value, sig->unit);
            snprintf(buf, sizeof(buf), "%.*f %s",
                     d, (double)sig->current_value,
                     sig->unit[0] ? sig->unit : "");
        }
        const char *cur = lv_label_get_text(r->value_lbl);
        if (cur && strcmp(cur, buf) == 0) continue;
        lv_label_set_text(r->value_lbl, buf);
        lv_obj_set_style_text_color(r->value_lbl,
                                    (sig->is_stale || sig->last_update_ms == 0)
                                        ? THEME_COLOR_TEXT_HINT
                                        : THEME_COLOR_TEXT_PRIMARY,
                                    0);
    }
}

/* ── Event handlers ────────────────────────────────────────────────────── */

static void _close_cb(lv_event_t *e)
{
    (void)e;
    obd2_picker_close();
}

static void _checkbox_cb(lv_event_t *e)
{
    signal_row_t *clicked = (signal_row_t *)lv_event_get_user_data(e);
    if (!clicked || !clicked->cb) return;
    bool new_state = lv_obj_has_state(clicked->cb, LV_STATE_CHECKED);

    /* Mirror the state across every row sharing the same parent PID.
     * Packed PIDs (Toyota engine block) bundle multiple signals into one
     * polled request — checking TY_RPM implicitly enables polling that
     * also delivers TY_THROTTLE, TY_COOLANT_TEMP, etc. Keep the visual
     * checkboxes in lockstep so the user isn't confused. */
    for (int i = 0; i < s_row_count; i++) {
        signal_row_t *r = &s_rows[i];
        if (r->parent_pid != clicked->parent_pid) continue;
        if (r->provided_by_preset) continue;
        if (r->checked == new_state) continue;
        r->checked = new_state;
        /* The clicked checkbox already toggled itself; this also moves its
         * row's selected fill, and ticks the siblings. */
        _row_show_checked(r, new_state);
    }
    /* Live-update the "X enabled" tally in the status line. */
    _refresh_count_status();
}

static void _set_status(const char *text)
{
    if (s_status && lv_obj_is_valid(s_status)) {
        lv_label_set_text(s_status, text);
    }
}

/* Recompute count breakdown and post to the status line. Called any time
 * row state changes (build, scan complete, checkbox toggle) so the user
 * sees "X signals, Y supported, Z on" update live. Lets the user
 * actually see additions take effect when new PIDs ship in firmware. */
static void _refresh_count_status(void)
{
    if (!s_status || !lv_obj_is_valid(s_status)) return;
    int total = s_row_count;
    int supported = 0;
    int enabled = 0;
    for (int i = 0; i < s_row_count; i++) {
        if (s_rows[i].supported) supported++;
        if (s_rows[i].checked)   enabled++;
    }
    char buf[96];
    if (supported > 0) {
        snprintf(buf, sizeof(buf),
                 "%d supported by the car, %d on (of %d)",
                 supported, enabled, total);
    } else {
        snprintf(buf, sizeof(buf),
                 "%d signals, %d on. Scan to see what the car supports.",
                 total, enabled);
    }
    lv_label_set_text(s_status, buf);
}

static void _scan_cb(lv_event_t *e)
{
    (void)e;
    if (obd2_discovery_in_progress()) return;
    _set_status("Scanning vehicle...");
    if (s_scan_btn) lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
    obd2_discovery_start(_scan_complete, NULL);
}

static void _scan_complete(const obd2_scan_result_t *r, void *user)
{
    (void)user;
    if (s_scan_btn) lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
    if (!s_overlay) return;  /* modal closed mid-scan */

    if (!r->completed || r->count == 0) {
        /* The scan auto-tried 500k+250k, 11-bit functional/physical AND
         * 29-bit extended addressing, so a failure here is genuine.
         * Distinguish "no CAN traffic at all" (wiring / K-line car) from
         * "CAN alive but the ECU never answered OBD" (gateway) so the
         * message is actionable. */
        uint32_t bus_err = 0;
        can_get_diagnostics(NULL, NULL, NULL, NULL, NULL, &bus_err, NULL);
        bool can_alive = (can_get_last_rx_id() != 0);
        if (can_alive) {
            _set_status("CAN alive but no OBD reply (tried 11+29-bit, "
                        "500k/250k). Gateway may block OBD.");
        } else if (bus_err > 1000) {
            _set_status("No CAN frames - check wiring: OBD-II pin 6 = CAN-H, "
                        "pin 14 = CAN-L (not swapped).");
        } else {
            _set_status("No CAN signal. Check pins 6/14 + ignition ON. "
                        "Pre-~2008 cars use K-line (unsupported).");
        }
        return;
    }

    /* For every supported PID, mark and auto-check ALL rows that share
     * that parent PID (a packed PID like Toyota Mode 21 PID 0x80 has
     * 7 rows — they all become "supported" together). This is the
     * dynamic-preset behaviour: scan tells us what the car responds to,
     * and the picker tracks the answer with no manual fiddling. */
    int decoder_signal_count = 0;     /* # of signals (rows) auto-enabled */
    int unknown = 0;                  /* # of PIDs with no decoder available */
    int meta_count = 0;               /* # of 0x20/0x40/... bitmask PIDs (skipped) */

    for (uint8_t i = 0; i < r->count; i++) {
        uint8_t pid = r->pids[i];

        /* "Supported PIDs in range" bitmasks at 0x20/0x40/0x60/0x80/etc.
         * are metadata — the scan ITSELF walks them to discover the next
         * 32-PID block. They aren't decodable signals. */
        if (pid == 0x20 || pid == 0x40 || pid == 0x60 || pid == 0x80 ||
            pid == 0xA0 || pid == 0xC0 || pid == 0xE0) {
            meta_count++;
            continue;
        }

        /* Scan only probes Mode 01 supported-PID bitmasks; match
         * accordingly so Toyota Mode 21 PID 0x80 doesn't accidentally
         * light up just because Mode 01's bitmask query 0x80 was sent. */
        const obd2_pid_def_t *def = obd2_pid_find_svc(0x01, pid);
        if (!def || def->service > 0x01) {
            unknown++;
            continue;
        }

        for (int j = 0; j < s_row_count; j++) {
            signal_row_t *row = &s_rows[j];
            if (row->parent_service != 0x01 || row->parent_pid != pid) continue;
            row->supported = true;

            if (row->badge && !row->provided_by_preset) {
                lv_label_set_text(row->badge, "supported");
                lv_obj_set_style_text_color(row->badge,
                                            THEME_COLOR_STATUS_CONNECTED, 0);
            }
            /* Auto-check: dynamic-preset shortcut. Doesn't auto-uncheck
             * anything; users get to keep curated additions. */
            if (!row->provided_by_preset && !row->checked) {
                row->checked = true;
                _row_show_checked(row, true);
                decoder_signal_count++;
            }
        }
    }

    /* "Supported" total excludes the discovery bitmask PIDs themselves
     * (0x20/0x40/etc.) — they're scaffolding the scan walks, not
     * decodable signals the user can put on a widget. */
    int real_supported = (int)r->count - meta_count;
    if (real_supported < 0) real_supported = 0;

    char status[96];
    if (unknown > 0) {
        snprintf(status, sizeof(status),
                 "Scan: %d supported, %d turned on, %d not known here",
                 real_supported, decoder_signal_count, unknown);
    } else {
        snprintf(status, sizeof(status),
                 "Scan: %d supported, %d turned on",
                 real_supported, decoder_signal_count);
    }
    _set_status(status);
    /* Scan summary stays in the status line until the user does
     * something — keeps the "X supported" number visible after scan. */
}

static void _save_cb(lv_event_t *e)
{
    (void)e;

    /* Collect distinct (service, parent_pid) tuples from checked rows.
     * Sub-fields of the same packed PID dedupe to one encoded entry —
     * one enable per PID is what the polling backend wants. */
    uint32_t pids[OBD2_MAX_ENABLED];
    uint8_t count = 0;
    for (int i = 0; i < s_row_count; i++) {
        if (!s_rows[i].checked || s_rows[i].provided_by_preset) continue;
        uint32_t enc = obd2_encode_pid(s_rows[i].parent_service,
                                       s_rows[i].parent_pid);
        bool dup = false;
        for (uint8_t k = 0; k < count; k++) {
            if (pids[k] == enc) { dup = true; break; }
        }
        if (!dup && count < OBD2_MAX_ENABLED) {
            pids[count++] = enc;
        }
    }

    char layout[LAYOUT_MAX_NAME];
    layout_manager_get_active(layout, sizeof(layout));
    esp_err_t err = ecu_preset_save_obd2_pids(layout, pids, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        _set_status("Save failed.");
        return;
    }
    ESP_LOGI(TAG, "Saved %u OBD2 PIDs", count);

    /* Apply immediately: restart polling with the new list. New PIDs'
     * signals get registered as external signals here. Any previously-
     * enabled PIDs that the user just disabled stay registered in the
     * signal registry (they'll go stale after 2s with no responses) —
     * full cleanup happens on the next layout reload, which is fine for
     * v1. Keeps the user on Device Settings without a jarring screen jump.
     *
     * s_saved = true tells obd2_picker_close not to revert to the
     * snapshot — the saved set IS the new truth. */
    obd2_start(pids, count);
    s_saved = true;

    obd2_picker_close();
}

