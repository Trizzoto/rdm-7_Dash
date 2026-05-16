/**
 * obd2_picker.c — OBD2 Signals modal (see obd2_picker.h).
 *
 * Builds a 640x360 overlay on lv_layer_top() with a scrollable list of
 * decodable OBD2 PIDs. The user toggles checkboxes to enable/disable
 * polling; each row shows the live signal value so the user can see at
 * a glance which PIDs are actually producing data. "Scan Vehicle"
 * probes the car for supported PIDs and badges the rows the ECU
 * responded to.
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

#include "obd2.h"
#include "ecu_presets.h"
#include "layout_manager.h"
#include "theme.h"
#include "signal.h"

#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "obd2_picker";

/* Sized small to keep redraw cost down — 46 PID rows + scrolling on top
 * of running OBD2 polling + dashboard widgets behind us. 640x360 → list
 * area ~248 px tall + 24 px rows means ~10 rows on screen, ~36 off — LVGL
 * still walks the whole tree, but a smaller modal redraws faster. */
#define MODAL_W  640
#define MODAL_H  360
#define HEADER_H  34
#define SCAN_H    32
#define FOOTER_H  40
#define ROW_H     24
#define LIVE_REFRESH_MS 400

/* ── State ─────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t   pid;
    bool      checked;
    bool      provided_by_preset;   /* greyed out, uncheckable */
    bool      discovered;           /* showed up in last scan */
    int16_t   signal_idx;           /* cached signal index or -1 */
    lv_obj_t *cb;                   /* checkbox object */
    lv_obj_t *row;                  /* row container */
    lv_obj_t *value_lbl;            /* live current-value readout */
    lv_obj_t *badge;                /* small label for status */
} pid_row_t;

static lv_obj_t  *s_overlay = NULL;
static lv_obj_t  *s_card    = NULL;
static lv_obj_t  *s_list    = NULL;
static lv_obj_t  *s_status  = NULL;     /* scan status label */
static lv_obj_t  *s_scan_btn = NULL;
static lv_timer_t *s_live_timer = NULL;
static pid_row_t  s_rows[64];
static int        s_row_count = 0;

/* Snapshot of the enabled PID list at modal open. Used to:
 *  - restore polling on Cancel/Close-without-Save (so preview-poll-all
 *    doesn't leave stale wide polling running)
 *  - decide which rows start checked
 * `s_saved` is flipped true by _save_cb so the close handler knows
 * not to restore — Save already pushed the new set. */
static uint8_t s_snapshot[OBD2_MAX_ENABLED];
static uint8_t s_snapshot_count = 0;
static bool    s_saved = false;

/* Forward decls */
static void  _close_cb(lv_event_t *e);
static void  _save_cb(lv_event_t *e);
static void  _scan_cb(lv_event_t *e);
static void  _checkbox_cb(lv_event_t *e);
static void  _scan_complete(const obd2_scan_result_t *r, void *user);
static void  _build_rows(void);
static bool  _signal_provided_by_preset(const char *signal_name);
static void  _set_status(const char *text);
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
    /* If the user dismissed without Save, revert OBD2 polling to the
     * pre-modal set — preview-poll-all should not persist past close. */
    if (!s_saved) {
        obd2_start(s_snapshot, s_snapshot_count);
    }
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    s_card    = NULL;
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

    /* Preview polling: kick off polling on every PID in the decode table.
     * Users see live values for the whole list and can tell at a glance
     * which PIDs the car responds to — vs. having to enable a row, save,
     * and watch the dashboard. obd2_start() filters PIDs whose signal is
     * already provided by the active native preset (conflict guard), so
     * supplemental-mode users don't accidentally double-bind RPM etc.
     * The adaptive scheduler drops unresponsive PIDs to a 5 sec probe
     * rate within ~3 sec so bus load stays reasonable even with 46 PIDs
     * enabled. */
    {
        uint8_t preview[OBD2_MAX_ENABLED];
        uint8_t pn = 0;
        for (int i = 0; i < OBD2_PIDS_COUNT && pn < OBD2_MAX_ENABLED; i++) {
            preview[pn++] = OBD2_PIDS[i].pid;
        }
        obd2_start(preview, pn);
    }

    /* Full-screen dimmer overlay. Doesn't dismiss on outside-tap — users
     * use the Close button (avoids LVGL event-bubbling gymnastics, and
     * matches the QR modal pattern elsewhere in Device Settings). */
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
    lv_label_set_text(title, "OBD2 Signals");
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
    lv_obj_add_event_cb(close_btn, _close_cb, LV_EVENT_CLICKED, NULL);

    /* ── Scan strip ── */
    lv_obj_t *scan_row = lv_obj_create(s_card);
    lv_obj_remove_style_all(scan_row);
    lv_obj_set_size(scan_row, MODAL_W, SCAN_H);
    lv_obj_align(scan_row, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_set_style_bg_color(scan_row, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(scan_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scan_row, 0, 0);
    lv_obj_set_style_border_side(scan_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(scan_row, LV_OBJ_FLAG_SCROLLABLE);

    s_scan_btn = lv_btn_create(scan_row);
    lv_obj_set_size(s_scan_btn, 140, 26);
    lv_obj_align(s_scan_btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(s_scan_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(s_scan_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(s_scan_btn, 0, 0);
    lv_obj_t *scan_lbl = lv_label_create(s_scan_btn);
    lv_label_set_text(scan_lbl, "Scan Vehicle");
    lv_obj_center(scan_lbl);
    lv_obj_set_style_text_font(scan_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(scan_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(s_scan_btn, _scan_cb, LV_EVENT_CLICKED, NULL);

    s_status = lv_label_create(scan_row);
    lv_label_set_text(s_status, "Tap Scan to detect supported PIDs.");
    lv_obj_set_style_text_font(s_status, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 168, 0);

    /* ── List body ──
     * Scrollbar mode OFF intentionally — every drawn scrollbar adds two
     * full-height drawn rects on each frame the user scrolls. The list
     * still scrolls via touch drag; users figure that out fast. */
    s_list = lv_obj_create(s_card);
    lv_obj_set_size(s_list, MODAL_W, MODAL_H - HEADER_H - SCAN_H - FOOTER_H);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, HEADER_H + SCAN_H);
    lv_obj_set_style_bg_color(s_list, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 2, 0);
    lv_obj_set_style_pad_row(s_list, 1, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* ── Footer ── */
    lv_obj_t *footer = lv_obj_create(s_card);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, MODAL_W, FOOTER_H);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(footer, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_btn_create(footer);
    lv_obj_set_size(cancel_btn, 96, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_RIGHT_MID, -120, 0);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_DIM, 0);
    lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_set_style_text_color(cancel_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(cancel_btn, _close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_btn = lv_btn_create(footer);
    lv_obj_set_size(save_btn, 96, 30);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_radius(save_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);
    lv_obj_set_style_text_color(save_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_set_style_text_font(save_lbl, THEME_FONT_SMALL, 0);
    lv_obj_add_event_cb(save_btn, _save_cb, LV_EVENT_CLICKED, NULL);

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

static void _build_rows(void)
{
    s_row_count = 0;

    /* Read currently-enabled PID list from the active layout. */
    char layout[LAYOUT_MAX_NAME];
    layout_manager_get_active(layout, sizeof(layout));

    uint8_t enabled[OBD2_MAX_ENABLED] = {0};
    uint8_t enabled_count = 0;
    ecu_preset_read_obd2_pids(layout, enabled, OBD2_MAX_ENABLED, &enabled_count);

    /* One row per decodable PID. */
    for (int i = 0; i < OBD2_PIDS_COUNT && s_row_count < 64; i++) {
        const obd2_pid_def_t *def = &OBD2_PIDS[i];

        bool checked = false;
        for (uint8_t k = 0; k < enabled_count; k++) {
            if (enabled[k] == def->pid) { checked = true; break; }
        }

        bool conflict = _signal_provided_by_preset(def->signal_name);

        pid_row_t *r = &s_rows[s_row_count++];
        r->pid = def->pid;
        r->checked = checked;
        r->provided_by_preset = conflict;
        r->discovered = false;
        r->signal_idx = signal_find_by_name(def->signal_name);

        r->row = lv_obj_create(s_list);
        lv_obj_set_size(r->row, lv_pct(100), ROW_H);
        lv_obj_set_style_bg_opa(r->row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r->row, 0, 0);
        lv_obj_set_style_pad_all(r->row, 0, 0);
        lv_obj_set_style_pad_left(r->row, 6, 0);
        lv_obj_set_style_pad_right(r->row, 6, 0);
        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);

        r->cb = lv_checkbox_create(r->row);
        lv_obj_align(r->cb, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_font(r->cb, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(r->cb, THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_color(r->cb, THEME_COLOR_INPUT_BG, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(r->cb, THEME_COLOR_ACCENT_BLUE,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(r->cb, THEME_COLOR_BORDER_MED,
                                      LV_PART_INDICATOR);
        lv_obj_set_style_border_width(r->cb, 1, LV_PART_INDICATOR);

        /* Compact label: "Engine RPM 0x0C" — unit moves to the live value
         * column so it's not duplicated. Tighter to fit 24 px rows. */
        char label[64];
        snprintf(label, sizeof(label), "%s  0x%02X", def->human_name, def->pid);
        lv_checkbox_set_text(r->cb, label);

        if (checked && !conflict) {
            lv_obj_add_state(r->cb, LV_STATE_CHECKED);
        }

        if (conflict) {
            /* Disabled: provided by the native preset. Force-uncheck. */
            lv_obj_add_state(r->cb, LV_STATE_DISABLED);
            lv_obj_clear_state(r->cb, LV_STATE_CHECKED);
            lv_obj_set_style_text_color(r->cb, THEME_COLOR_TEXT_DISABLED, 0);
            r->checked = false;
        } else {
            lv_obj_add_event_cb(r->cb, _checkbox_cb,
                                LV_EVENT_VALUE_CHANGED, r);
        }

        /* Live current-value readout: shows the signal's most recent value
         * + unit when the signal is registered, "—" when not, and is muted
         * when the signal is stale. Sits right of center. */
        r->value_lbl = lv_label_create(r->row);
        lv_obj_align(r->value_lbl, LV_ALIGN_RIGHT_MID, -72, 0);
        lv_obj_set_style_text_font(r->value_lbl, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(r->value_lbl, THEME_COLOR_TEXT_HINT, 0);
        lv_label_set_text(r->value_lbl, "—");

        /* Badge on the right. */
        r->badge = lv_label_create(r->row);
        lv_obj_align(r->badge, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_text_font(r->badge, THEME_FONT_TINY, 0);
        if (conflict) {
            lv_label_set_text(r->badge, "in preset");
            lv_obj_set_style_text_color(r->badge, THEME_COLOR_TEXT_HINT, 0);
        } else {
            lv_label_set_text(r->badge, "");
        }
    }

    /* Kick off the live-value refresh loop. */
    if (!s_live_timer) {
        s_live_timer = lv_timer_create(_live_refresh_cb, LIVE_REFRESH_MS, NULL);
        /* Prime once so values are visible immediately instead of after
         * the first 400 ms tick. */
        _live_refresh_cb(s_live_timer);
    }
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

    /* Only walk rows that have a registered signal. Unenabled PIDs keep
     * their initial "—" label — no LVGL work for ~40 of the 46 rows on
     * a typical setup. Single signal_find_by_name lookup per still-
     * unresolved row to catch PIDs the user just enabled. */
    for (int i = 0; i < s_row_count; i++) {
        pid_row_t *r = &s_rows[i];
        if (!r->value_lbl || !lv_obj_is_valid(r->value_lbl)) continue;

        if (r->signal_idx < 0) {
            const obd2_pid_def_t *def = obd2_pid_find(r->pid);
            if (def) {
                /* Packed PIDs have no top-level signal_name; use the
                 * first sub-field as the "headline" live value so the
                 * row at least shows that polling is working. */
                const char *name = def->signal_name;
                if (!name && def->sub_fields && def->sub_field_count > 0) {
                    name = def->sub_fields[0].signal_name;
                }
                if (name) r->signal_idx = signal_find_by_name(name);
            }
            if (r->signal_idx < 0) continue;   /* still unregistered */
        }

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
        /* Skip invalidation if the rendered text hasn't changed. lv_label
         * does its own strcmp on set_text, but doing it here also avoids
         * the per-tick color set. */
        const char *cur = lv_label_get_text(r->value_lbl);
        if (cur && strcmp(cur, buf) == 0) continue;
        lv_label_set_text(r->value_lbl, buf);
        lv_obj_set_style_text_color(r->value_lbl,
                                    (sig->is_stale || sig->last_update_ms == 0)
                                        ? THEME_COLOR_TEXT_HINT
                                        : THEME_COLOR_ACCENT_BLUE,
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
    pid_row_t *r = (pid_row_t *)lv_event_get_user_data(e);
    if (!r || !r->cb) return;
    r->checked = lv_obj_has_state(r->cb, LV_STATE_CHECKED);
}

static void _set_status(const char *text)
{
    if (s_status && lv_obj_is_valid(s_status)) {
        lv_label_set_text(s_status, text);
    }
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
        _set_status("No response from vehicle. Is ignition on?");
        return;
    }

    /* Mark each row that matches a discovered PID. */
    int decoder_hits = 0;
    int unknown = 0;
    for (uint8_t i = 0; i < r->count; i++) {
        uint8_t pid = r->pids[i];
        const obd2_pid_def_t *def = obd2_pid_find(pid);
        if (!def) { unknown++; continue; }
        for (int j = 0; j < s_row_count; j++) {
            if (s_rows[j].pid == pid) {
                s_rows[j].discovered = true;
                if (s_rows[j].badge && !s_rows[j].provided_by_preset) {
                    lv_label_set_text(s_rows[j].badge, "supported");
                    lv_obj_set_style_text_color(s_rows[j].badge,
                                                THEME_COLOR_ACCENT_BLUE, 0);
                }
                decoder_hits++;
                break;
            }
        }
    }

    char status[96];
    if (unknown > 0) {
        snprintf(status, sizeof(status),
                 "Found %u supported PIDs (%d decodable, %d unknown).",
                 r->count, decoder_hits, unknown);
    } else {
        snprintf(status, sizeof(status),
                 "Found %u supported PIDs.", r->count);
    }
    _set_status(status);
}

static void _save_cb(lv_event_t *e)
{
    (void)e;

    /* Collect checked PIDs (skipping conflicts which are uncheckable anyway). */
    uint8_t pids[OBD2_MAX_ENABLED];
    uint8_t count = 0;
    for (int i = 0; i < s_row_count && count < OBD2_MAX_ENABLED; i++) {
        if (s_rows[i].checked && !s_rows[i].provided_by_preset) {
            pids[count++] = s_rows[i].pid;
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
