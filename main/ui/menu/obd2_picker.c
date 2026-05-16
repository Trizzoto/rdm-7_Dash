/**
 * obd2_picker.c — OBD2 Signals modal (see obd2_picker.h).
 *
 * Builds a 640x360 overlay on lv_layer_top() with a scrollable list of
 * OBD2 SIGNALS — one row per signal (not per PID). Single-value PIDs
 * produce one row each; packed PIDs (e.g. Toyota Mode 21 PID 0x80)
 * produce N rows, one per sub-field, all sharing one polled request.
 * Sub-field checkboxes are linked: ticking any one ticks all of them
 * because they ride a single PID poll.
 *
 * Each row shows the live signal value, a (M01·0x05)-style mode/PID
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

static lv_obj_t      *s_overlay    = NULL;
static lv_obj_t      *s_card       = NULL;
static lv_obj_t      *s_list       = NULL;
static lv_obj_t      *s_status     = NULL;     /* scan status label */
static lv_obj_t      *s_scan_btn   = NULL;
static lv_obj_t      *s_dump_btn   = NULL;     /* "Dump Unknowns" button */
static lv_obj_t      *s_trim_btn   = NULL;     /* "Trim to Supported" button */
static lv_timer_t    *s_live_timer = NULL;
static signal_row_t   s_rows[PICKER_MAX_ROWS];
static int            s_row_count  = 0;

/* ── "Dump Unknowns" capture state ──────────────────────────────────────
 *
 * After a scan, PIDs the vehicle reports as supported but we have no
 * decoder for are stashed here. The Dump button walks this list,
 * fires a Mode 01 test request for each, captures the raw response
 * bytes, and shows them in a sub-overlay (and ESP_LOGI's the whole
 * thing for serial-monitor capture). Helps a user (or me) author new
 * obd2_pids.c entries for cars that report PIDs we don't yet decode. */

/* Sized to fit the typical "unknown PID" set on a real vehicle. The user's
 * 2024 HiAce returned 18 unknowns; 32 is double that, plenty of headroom.
 * Shrinking from 64x80 to 32x64 also shrinks _dump_show_results' stack
 * buffer from 5 KB to 2 KB, easing pressure on the LVGL task stack. */
#define DUMP_MAX        32
#define DUMP_LINE_LEN   64       /* "0xAA: 41 AA BB CC DD EE FF" fits */

static uint8_t   s_unknown_pids[DUMP_MAX];
static uint8_t   s_unknown_count = 0;
static char      s_dump_lines[DUMP_MAX][DUMP_LINE_LEN];
static uint8_t   s_dump_idx      = 0;        /* next PID index to query */
static bool      s_dump_running  = false;
static lv_obj_t *s_dump_overlay  = NULL;     /* results sub-modal */

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
static void  _trim_btn_cb(lv_event_t *e);
static void  _dump_btn_cb(lv_event_t *e);
static void  _dump_next(void);
static void  _dump_test_cb(bool ok, const uint8_t *raw, uint8_t raw_len,
                            float decoded, uint32_t elapsed_ms, void *user);
static void  _dump_show_results(void);
static void  _dump_overlay_close_cb(lv_event_t *e);
static void  _dump_overlay_destroy(void);

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
    /* Tear down dump results sub-overlay if still open. Halt any in-flight
     * dump walk — the test callback checks s_dump_running before touching
     * UI, so an outstanding obd2_test_pid() in flight is safe to drop. */
    _dump_overlay_destroy();
    s_dump_running   = false;
    s_dump_idx       = 0;
    s_unknown_count  = 0;

    lv_obj_del(s_overlay);
    s_overlay = NULL;
    s_card    = NULL;
    s_list    = NULL;
    s_status  = NULL;
    s_scan_btn = NULL;
    s_dump_btn = NULL;
    s_trim_btn = NULL;
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

    /* "Trim to Supported" button — hidden until scan completes.
     * Quick post-scan cleanup: uncheck rows the car didn't confirm. */
    s_trim_btn = lv_btn_create(scan_row);
    lv_obj_set_size(s_trim_btn, 120, 26);
    lv_obj_align(s_trim_btn, LV_ALIGN_LEFT_MID, 160, 0);
    lv_obj_set_style_bg_color(s_trim_btn, THEME_COLOR_BTN_DIM, 0);
    lv_obj_set_style_radius(s_trim_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(s_trim_btn, 0, 0);
    lv_obj_t *trim_lbl = lv_label_create(s_trim_btn);
    lv_label_set_text(trim_lbl, "Trim Unsupported");
    lv_obj_center(trim_lbl);
    lv_obj_set_style_text_font(trim_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(trim_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(s_trim_btn, _trim_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_trim_btn, LV_OBJ_FLAG_HIDDEN);  /* hidden until scan */

    /* "Dump Unknowns" button — hidden until a scan finds undecodable PIDs.
     * Sized slightly narrower than Scan to keep the strip balanced. */
    s_dump_btn = lv_btn_create(scan_row);
    lv_obj_set_size(s_dump_btn, 130, 26);
    lv_obj_align(s_dump_btn, LV_ALIGN_LEFT_MID, 286, 0);
    lv_obj_set_style_bg_color(s_dump_btn, THEME_COLOR_BTN_DIM, 0);
    lv_obj_set_style_radius(s_dump_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(s_dump_btn, 0, 0);
    lv_obj_t *dump_lbl = lv_label_create(s_dump_btn);
    lv_label_set_text(dump_lbl, "Dump Unknowns");
    lv_obj_center(dump_lbl);
    lv_obj_set_style_text_font(dump_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(dump_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(s_dump_btn, _dump_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_dump_btn, LV_OBJ_FLAG_HIDDEN);  /* hidden until scan finds any */

    s_status = lv_label_create(scan_row);
    lv_label_set_text(s_status, "Tap Scan to detect supported PIDs.");
    lv_obj_set_style_text_font(s_status, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 426, 0);

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

    /* Label = signal name + mode/PID tag, e.g. "RPM  (M01·0x0C)" or
     * "TY_RPM  (M21·0x80)" or "ATF_TEMP_22  (M22·0x115C)" so the user
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

    if (r->checked) lv_obj_add_state(r->cb, LV_STATE_CHECKED);

    if (r->provided_by_preset) {
        lv_obj_add_state(r->cb, LV_STATE_DISABLED);
        lv_obj_clear_state(r->cb, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(r->cb, THEME_COLOR_TEXT_DISABLED, 0);
    } else {
        lv_obj_add_event_cb(r->cb, _checkbox_cb, LV_EVENT_VALUE_CHANGED, r);
    }

    /* Live value column. */
    r->value_lbl = lv_label_create(r->row);
    lv_obj_align(r->value_lbl, LV_ALIGN_RIGHT_MID, -72, 0);
    lv_obj_set_style_text_font(r->value_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(r->value_lbl, THEME_COLOR_TEXT_HINT, 0);
    lv_label_set_text(r->value_lbl, "-");

    /* Status badge. */
    r->badge = lv_label_create(r->row);
    lv_obj_align(r->badge, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(r->badge, THEME_FONT_TINY, 0);
    if (r->provided_by_preset) {
        lv_label_set_text(r->badge, "in preset");
        lv_obj_set_style_text_color(r->badge, THEME_COLOR_TEXT_HINT, 0);
    } else {
        lv_label_set_text(r->badge, "");
    }
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
        if (r != clicked && r->cb && lv_obj_is_valid(r->cb)) {
            if (new_state) lv_obj_add_state(r->cb, LV_STATE_CHECKED);
            else           lv_obj_clear_state(r->cb, LV_STATE_CHECKED);
        }
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
 * sees "X decoders · Y supported · Z enabled" update live. Lets the user
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
                 "%d sup, %d on (of %d)",
                 supported, enabled, total);
    } else {
        snprintf(buf, sizeof(buf),
                 "%d decoders, %d on, tap Scan",
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
        _set_status("No response from vehicle. Is ignition on?");
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

    /* Reset the unknown capture buffer — fresh scan, fresh list. */
    s_unknown_count = 0;

    for (uint8_t i = 0; i < r->count; i++) {
        uint8_t pid = r->pids[i];

        /* "Supported PIDs in range" bitmasks at 0x20/0x40/0x60/0x80/etc.
         * are metadata — the scan ITSELF walks them to discover the next
         * 32-PID block. They aren't decodable signals, so don't count
         * them as "unknown" and don't offer them in the Dump list (it'd
         * just dump the bitmask the user already saw via Scan). */
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
            /* Capture for the Dump button. Cap at DUMP_MAX; in practice
             * any real vehicle reports well under 64 supported PIDs. */
            if (s_unknown_count < DUMP_MAX) {
                s_unknown_pids[s_unknown_count++] = pid;
            }
            continue;
        }

        for (int j = 0; j < s_row_count; j++) {
            signal_row_t *row = &s_rows[j];
            if (row->parent_service != 0x01 || row->parent_pid != pid) continue;
            row->supported = true;

            if (row->badge && !row->provided_by_preset) {
                lv_label_set_text(row->badge, "supported");
                lv_obj_set_style_text_color(row->badge,
                                            THEME_COLOR_ACCENT_BLUE, 0);
            }
            /* Auto-check: dynamic-preset shortcut. Doesn't auto-uncheck
             * anything; users get to keep curated additions. */
            if (!row->provided_by_preset && !row->checked) {
                row->checked = true;
                if (row->cb && lv_obj_is_valid(row->cb)) {
                    lv_obj_add_state(row->cb, LV_STATE_CHECKED);
                }
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
                 "Scan: %d sup, %d on, %d unknown",
                 real_supported, decoder_signal_count, unknown);
    } else {
        snprintf(status, sizeof(status),
                 "Scan: %d sup, %d on",
                 real_supported, decoder_signal_count);
    }
    _set_status(status);

    /* Reveal Dump button only when there's something to dump. Hidden
     * otherwise to avoid implying there's work to do. */
    if (s_dump_btn && lv_obj_is_valid(s_dump_btn)) {
        if (s_unknown_count > 0) {
            lv_obj_clear_flag(s_dump_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_state(s_dump_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_flag(s_dump_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Reveal Trim button — lets user prune un-confirmed PIDs in one
     * click. Visible whenever a scan has run, even if 0 supported (in
     * which case Trim would uncheck everything — useful "reset" path). */
    if (s_trim_btn && lv_obj_is_valid(s_trim_btn)) {
        lv_obj_clear_flag(s_trim_btn, LV_OBJ_FLAG_HIDDEN);
    }
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

/* ── "Trim Unsupported" implementation ──────────────────────────────────
 *
 * Walks the visible rows and unchecks any whose parent PID didn't
 * respond to the last scan. Lets a user clean up after enabling too
 * many defaults: they scan, see "Scan: 34 sup", click Trim, and the
 * polled list shrinks to just what the car actually answers. Keeps
 * preset-owned rows untouched (those are already greyed/locked). */

static void _trim_btn_cb(lv_event_t *e)
{
    (void)e;
    int trimmed = 0;
    for (int i = 0; i < s_row_count; i++) {
        signal_row_t *r = &s_rows[i];
        if (r->provided_by_preset) continue;
        if (r->supported) continue;
        if (!r->checked) continue;
        r->checked = false;
        if (r->cb && lv_obj_is_valid(r->cb)) {
            lv_obj_clear_state(r->cb, LV_STATE_CHECKED);
        }
        trimmed++;
    }
    if (trimmed > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Trimmed %d unsupported.", trimmed);
        _set_status(buf);
    } else {
        _set_status("Nothing to trim - all enabled rows are supported.");
    }
}

/* ── "Dump Unknowns" implementation ─────────────────────────────────────
 *
 * Walks s_unknown_pids[] sequentially, firing one obd2_test_pid() per
 * step. The callback formats "0xPP: AA BB CC..." into s_dump_lines[],
 * advances s_dump_idx, and re-arms the next step. When all PIDs have
 * been queried (or timed out), we render the results overlay and emit
 * the same lines as ESP_LOGI("DUMP ...") so the user can also pull the
 * dump off the serial monitor.
 *
 * Each test has a 500 ms upper bound (obd2.c OBD2_TEST_TIMEOUT_MS), so
 * worst case for 18 unknowns is ~9 s — long enough to need a progress
 * indicator in the status line but fast enough not to need a cancel.
 */

static void _dump_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_dump_running) return;
    if (s_unknown_count == 0) return;

    s_dump_running = true;
    s_dump_idx     = 0;
    memset(s_dump_lines, 0, sizeof(s_dump_lines));

    if (s_dump_btn && lv_obj_is_valid(s_dump_btn)) {
        lv_obj_add_state(s_dump_btn, LV_STATE_DISABLED);
    }

    ESP_LOGI(TAG, "DUMP begin: %u unknown PID(s)", (unsigned)s_unknown_count);
    _dump_next();
}

static void _dump_next(void)
{
    if (!s_dump_running) return;

    /* Done? Show results and re-enable the button. */
    if (s_dump_idx >= s_unknown_count) {
        s_dump_running = false;
        if (s_dump_btn && lv_obj_is_valid(s_dump_btn)) {
            lv_obj_clear_state(s_dump_btn, LV_STATE_DISABLED);
        }
        ESP_LOGI(TAG, "DUMP end");
        _set_status("Dump complete. See overlay or serial 'DUMP' lines.");
        _dump_show_results();
        return;
    }

    /* Progress in the status line — small enough to fit alongside the
     * Dump button. */
    char status[64];
    snprintf(status, sizeof(status), "Dumping %u of %u (PID 0x%02X)...",
             (unsigned)(s_dump_idx + 1),
             (unsigned)s_unknown_count,
             s_unknown_pids[s_dump_idx]);
    _set_status(status);

    /* Generic Mode 01 request with 1-byte decode (we only care about the
     * raw bytes — the decoded float is discarded). request_id=0 means
     * broadcast 0x7DF, which is what the user's car already answered to
     * during the supported-PID scan, so we'll get a reply on the same path. */
    obd2_test_pid(0x01,
                  s_unknown_pids[s_dump_idx],
                  0,        /* request_id 0 = broadcast 0x7DF */
                  0,        /* data_offset */
                  1,        /* data_bytes (smallest valid) */
                  1.0f, 0.0f, false,
                  _dump_test_cb, NULL);
}

static void _dump_test_cb(bool ok, const uint8_t *raw, uint8_t raw_len,
                           float decoded, uint32_t elapsed_ms, void *user)
{
    (void)decoded; (void)user; (void)elapsed_ms;

    /* Modal closed mid-walk? Drop the result on the floor — _dump_overlay /
     * s_dump_btn are gone, and obd2_picker_close already cleared the
     * running flag. */
    if (!s_dump_running) return;
    if (s_dump_idx >= s_unknown_count) return;  /* belt and braces */

    uint8_t pid  = s_unknown_pids[s_dump_idx];
    char   *line = s_dump_lines[s_dump_idx];
    int     n    = 0;

    n += snprintf(line + n, DUMP_LINE_LEN - n, "0x%02X: ", pid);
    if (!ok || raw == NULL || raw_len == 0) {
        snprintf(line + n, DUMP_LINE_LEN - n, "(no response)");
    } else {
        /* Limit how many bytes we render so a long Mode 01 response can't
         * overflow the line buffer. 3 chars per byte ("AA "), reserve
         * ~4 chars trailing safety. */
        uint8_t cap = (uint8_t)((DUMP_LINE_LEN - n - 4) / 3);
        if (raw_len > cap) raw_len = cap;
        for (uint8_t i = 0; i < raw_len; i++) {
            n += snprintf(line + n, DUMP_LINE_LEN - n, "%02X ", raw[i]);
        }
    }

    ESP_LOGI(TAG, "DUMP %s", line);
    s_dump_idx++;
    _dump_next();
}

/* Standalone destroy so both close paths (Close button + parent modal
 * teardown) can call it. Idempotent. */
static void _dump_overlay_destroy(void)
{
    if (s_dump_overlay && lv_obj_is_valid(s_dump_overlay)) {
        lv_obj_del(s_dump_overlay);
    }
    s_dump_overlay = NULL;
}

static void _dump_overlay_close_cb(lv_event_t *e)
{
    (void)e;
    _dump_overlay_destroy();
}

static void _dump_show_results(void)
{
    _dump_overlay_destroy();  /* re-runs are allowed */

    /* Full-screen dimmer, slightly more opaque than the picker so the
     * results sit visually "on top" of the picker without ambiguity. */
    s_dump_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_dump_overlay);
    lv_obj_set_size(s_dump_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_dump_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dump_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(s_dump_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_dump_overlay);
    lv_obj_set_size(card, 560, 420);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Raw Response Dump");
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint,
        "Mode 01 reply bytes for each unsupported PID.\n"
        "First byte echoes the PID; remainder = data payload.\n"
        "Also copied to serial monitor as 'DUMP ...' lines.");
    lv_obj_set_style_text_font(hint, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 30);

    /* Scrollable container for the dump list. lv_obj_t scrolls by default;
     * we just need a tall content. */
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_set_size(box, 530, 290);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 88);
    lv_obj_set_style_bg_color(box, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, THEME_COLOR_BORDER_MED, 0);
    lv_obj_set_style_radius(box, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);

    lv_obj_t *text = lv_label_create(box);
    lv_obj_set_width(text, 510);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(text, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Concatenate all dumped lines into one label. With DUMP_MAX=64 and
     * DUMP_LINE_LEN=80, worst case is ~5 KB — well within LVGL label
     * limits, and the parent box scrolls. */
    char buf[DUMP_MAX * DUMP_LINE_LEN];
    int pos = 0;
    for (uint8_t i = 0; i < s_dump_idx && pos < (int)sizeof(buf) - 2; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\n", s_dump_lines[i]);
    }
    if (pos == 0) {
        snprintf(buf, sizeof(buf), "(no PIDs dumped)");
    }
    lv_label_set_text(text, buf);

    /* Close button bottom-right of card. */
    lv_obj_t *close = lv_btn_create(card);
    lv_obj_set_size(close, 90, 30);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close, THEME_COLOR_BTN_DIM, 0);
    lv_obj_set_style_radius(close, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_t *close_lbl = lv_label_create(close);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_font(close_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(close_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(close, _dump_overlay_close_cb, LV_EVENT_CLICKED, NULL);
}
