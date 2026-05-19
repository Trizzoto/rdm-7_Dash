/* ui_gear_setup.c — on-device Gear Setup overlay.
 *
 * Lets the user configure CALCULATED_GEAR without going to the web
 * editor:
 *   - Pick the RPM and VEHICLE_SPEED signals from the live registry
 *     (or fall back to text-named defaults if nothing's registered yet).
 *   - Set wheel circumference, final-drive ratio, gearbox ratios.
 *   - Enable / disable the calculator.
 *
 * Auto-opens after the user picks "RDM-7 / Internal" in the ECU picker
 * (see ui_ecu_picker.c::_apply_cb), and can also be opened directly
 * from Device Settings.
 *
 * UI shape: 600 × 460 card. Header row + scrollable body + bottom
 * Save/Cancel buttons. Body is a flex column with one row per field;
 * per-gear ratio rows show/hide based on the gear count selector.
 *
 * Numeric inputs use a self-contained [−] [value] [+] stepper pattern.
 * The codebase doesn't use lv_spinbox elsewhere, and a touchscreen
 * stepper is friendlier than the spinbox's digit-selection mode.
 */

#include "ui_gear_setup.h"

#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#include "../theme.h"
#include "../../storage/config_store.h"
#include "../../widgets/signal.h"

static const char *TAG = "gear_setup";

#define CARD_W 600
#define CARD_H 460
#define ROW_H   34

/* Max ratio rows shown — must equal GEAR_CAL_MAX_GEARS-1 (slot 0 is N). */
#define MAX_RATIO_ROWS 8

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *card;
    lv_obj_t *list;            /* scrollable body container */

    lv_obj_t *enabled_sw;
    lv_obj_t *rpm_dd;
    lv_obj_t *speed_dd;
    lv_obj_t *wheel_lbl;
    lv_obj_t *fd_lbl;
    lv_obj_t *count_lbl;
    lv_obj_t *ratio_row[MAX_RATIO_ROWS];
    lv_obj_t *ratio_lbl[MAX_RATIO_ROWS];

    /* Cached signal-registry options (dropdown text). NULL if no signals
     * are registered yet — UI falls back to a one-line note then. */
    char     dd_options[1024];

    /* Live working copy — pushed to NVS on Save. */
    gear_cal_config_t cfg;

    ui_gear_setup_done_cb_t cb;
    void *ctx;
} gear_setup_state_t;

static gear_setup_state_t s;

/* ── Forward decls ─────────────────────────────────────────────────────── */
static void _close(bool saved);
static void _save_cb(lv_event_t *e);
static void _cancel_cb(lv_event_t *e);
static void _enabled_sw_cb(lv_event_t *e);
static void _rpm_dd_cb(lv_event_t *e);
static void _speed_dd_cb(lv_event_t *e);
static void _wheel_step_cb(lv_event_t *e);
static void _fd_step_cb(lv_event_t *e);
static void _count_step_cb(lv_event_t *e);
static void _ratio_step_cb(lv_event_t *e);
static void _build_signal_options(char *buf, size_t n);
static void _select_dd_by_name(lv_obj_t *dd, const char *name);
static void _read_dd_into(char *dst, size_t dst_sz, lv_obj_t *dd);
static void _refresh_ratio_rows(void);
static void _refresh_value_labels(void);

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Format a float with 2 decimals into a label. Used by every numeric row. */
static void _fmt2(char *buf, size_t n, float v) {
    snprintf(buf, n, "%.2f", (double)v);
}

static void _set_label_float(lv_obj_t *lbl, float v) {
    if (!lbl) return;
    char buf[16];
    _fmt2(buf, sizeof(buf), v);
    lv_label_set_text(lbl, buf);
}

/* Build the newline-separated signal-name dropdown list. Includes a
 * leading "(custom)" hint when the current value isn't in the registry,
 * so the user can see what the stored config refers to even after the
 * registry changes. */
static void _build_signal_options(char *buf, size_t n) {
    buf[0] = '\0';
    uint16_t count = signal_get_count();
    bool first = true;
    for (uint16_t i = 0; i < count; i++) {
        signal_t *sig = signal_get_by_index(i);
        if (!sig || !sig->name[0]) continue;
        if (!first) strncat(buf, "\n", n - strlen(buf) - 1);
        strncat(buf, sig->name, n - strlen(buf) - 1);
        first = false;
    }
    /* If the registry is empty (no layout loaded yet), seed a sensible
     * default pair so the dropdown isn't blank. The user can re-open the
     * modal after a layout loads to pick the real signal names. */
    if (buf[0] == '\0') {
        strncat(buf, "RPM\nVEHICLE_SPEED", n - strlen(buf) - 1);
    }
}

/* Find an entry matching `name` in the dropdown and select it. Falls back
 * to index 0 (no warning) so a stale saved name doesn't break the picker. */
static void _select_dd_by_name(lv_obj_t *dd, const char *name) {
    if (!dd || !name || !name[0]) return;
    const char *opts = lv_dropdown_get_options(dd);
    if (!opts) return;
    /* Walk the newline-separated string looking for an exact match. */
    int idx = 0;
    const char *p = opts;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (strncmp(p, name, len) == 0 && name[len] == '\0') {
            lv_dropdown_set_selected(dd, idx);
            return;
        }
        idx++;
        if (!nl) break;
        p = nl + 1;
    }
    /* Not found — leave the dropdown at index 0. */
}

/* Read the currently-selected dropdown string into `dst`. */
static void _read_dd_into(char *dst, size_t dst_sz, lv_obj_t *dd) {
    if (!dst || dst_sz == 0 || !dd) return;
    lv_dropdown_get_selected_str(dd, dst, dst_sz);
}

/* Per-ratio + count change — show only the rows up to the active count. */
static void _refresh_ratio_rows(void) {
    uint8_t shown = s.cfg.ratio_count;
    if (shown > MAX_RATIO_ROWS + 1) shown = MAX_RATIO_ROWS + 1;
    /* ratio_count includes slot 0 (Neutral) — visible rows = ratio_count - 1 */
    uint8_t visible = (shown > 0) ? (uint8_t)(shown - 1) : 0;
    if (visible > MAX_RATIO_ROWS) visible = MAX_RATIO_ROWS;

    for (uint8_t i = 0; i < MAX_RATIO_ROWS; i++) {
        if (!s.ratio_row[i]) continue;
        if (i < visible) lv_obj_clear_flag(s.ratio_row[i], LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag  (s.ratio_row[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Push all live config values onto their label widgets. */
static void _refresh_value_labels(void) {
    if (s.wheel_lbl) _set_label_float(s.wheel_lbl, s.cfg.wheel_circumference_m);
    if (s.fd_lbl)    _set_label_float(s.fd_lbl,    s.cfg.final_drive);
    if (s.count_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u gears",
                 (unsigned)(s.cfg.ratio_count > 0 ? s.cfg.ratio_count - 1 : 0));
        lv_label_set_text(s.count_lbl, buf);
    }
    for (uint8_t i = 0; i < MAX_RATIO_ROWS; i++) {
        if (s.ratio_lbl[i]) {
            float v = (i + 1 < GEAR_CAL_MAX_GEARS) ? s.cfg.ratios[i + 1] : 0.0f;
            _set_label_float(s.ratio_lbl[i], v);
        }
    }
}

/* ── Event handlers ───────────────────────────────────────────────────── */

static void _enabled_sw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    s.cfg.enabled = lv_obj_has_state(s.enabled_sw, LV_STATE_CHECKED);
}

static void _rpm_dd_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    _read_dd_into(s.cfg.rpm_signal, sizeof(s.cfg.rpm_signal), s.rpm_dd);
}

static void _speed_dd_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    _read_dd_into(s.cfg.speed_signal, sizeof(s.cfg.speed_signal), s.speed_dd);
}

/* Stepper user_data layout: low bit (1) = "+" direction, 0 = "−".
 * High bytes encode which field (kind) is being stepped — see the
 * pack/unpack helpers below. Keeping it as a single intptr_t avoids a
 * per-button alloc just for two bytes of context. */
#define STEP_DOWN 0
#define STEP_UP   1
#define _pack_step(kind, dir, idx) \
    (void *)(((intptr_t)(idx) << 16) | ((intptr_t)(kind) << 8) | (intptr_t)(dir))
#define _unpack_dir(u)   ((int)((intptr_t)(u) & 0xFF))
#define _unpack_kind(u)  ((int)(((intptr_t)(u) >> 8) & 0xFF))
#define _unpack_idx(u)   ((int)(((intptr_t)(u) >> 16) & 0xFFFF))

static void _wheel_step_cb(lv_event_t *e) {
    void *u = lv_event_get_user_data(e);
    int dir = _unpack_dir(u);
    float step = 0.01f;
    s.cfg.wheel_circumference_m += (dir == STEP_UP ? step : -step);
    if (s.cfg.wheel_circumference_m < 0.5f) s.cfg.wheel_circumference_m = 0.5f;
    if (s.cfg.wheel_circumference_m > 3.5f) s.cfg.wheel_circumference_m = 3.5f;
    _refresh_value_labels();
}

static void _fd_step_cb(lv_event_t *e) {
    void *u = lv_event_get_user_data(e);
    int dir = _unpack_dir(u);
    float step = 0.01f;
    s.cfg.final_drive += (dir == STEP_UP ? step : -step);
    if (s.cfg.final_drive < 1.0f)  s.cfg.final_drive = 1.0f;
    if (s.cfg.final_drive > 10.0f) s.cfg.final_drive = 10.0f;
    _refresh_value_labels();
}

static void _count_step_cb(lv_event_t *e) {
    void *u = lv_event_get_user_data(e);
    int dir = _unpack_dir(u);
    /* ratio_count includes slot 0 (Neutral). Working range for the user:
     * 1..8 forward gears → ratio_count = 2..9. */
    int n = (int)s.cfg.ratio_count + (dir == STEP_UP ? 1 : -1);
    if (n < 2) n = 2;
    if (n > GEAR_CAL_MAX_GEARS) n = GEAR_CAL_MAX_GEARS;
    s.cfg.ratio_count = (uint8_t)n;
    _refresh_value_labels();
    _refresh_ratio_rows();
}

static void _ratio_step_cb(lv_event_t *e) {
    void *u = lv_event_get_user_data(e);
    int dir  = _unpack_dir(u);
    int idx  = _unpack_idx(u);            /* gear index, 0 = 1st gear */
    int slot = idx + 1;                   /* into cfg.ratios[] — slot 0 is N */
    if (slot < 1 || slot >= GEAR_CAL_MAX_GEARS) return;

    float step = 0.05f;
    s.cfg.ratios[slot] += (dir == STEP_UP ? step : -step);
    if (s.cfg.ratios[slot] < 0.20f) s.cfg.ratios[slot] = 0.20f;
    if (s.cfg.ratios[slot] > 6.0f)  s.cfg.ratios[slot] = 6.0f;
    _refresh_value_labels();
}

/* ── Close paths ──────────────────────────────────────────────────────── */

static void _save_cb(lv_event_t *e) {
    (void)e;
    /* Sync dropdown selections one last time in case the user changed
     * them but the VALUE_CHANGED didn't fire (e.g. dropdown was opened
     * then closed without a different pick). */
    if (s.rpm_dd)   _read_dd_into(s.cfg.rpm_signal,   sizeof(s.cfg.rpm_signal),   s.rpm_dd);
    if (s.speed_dd) _read_dd_into(s.cfg.speed_signal, sizeof(s.cfg.speed_signal), s.speed_dd);

    /* Slot 0 is always Neutral (0.0). Defensive — config_store doesn't
     * enforce this but signal_internal.c relies on it. */
    s.cfg.ratios[0] = 0.0f;

    esp_err_t err = config_store_save_gear_cal(&s.cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save_gear_cal failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Gear cal saved: %u gears, FD=%.2f, wheel=%.2f m, %s",
                 (unsigned)s.cfg.ratio_count,
                 (double)s.cfg.final_drive,
                 (double)s.cfg.wheel_circumference_m,
                 s.cfg.enabled ? "enabled" : "disabled");
    }
    _close(err == ESP_OK);
}

static void _cancel_cb(lv_event_t *e) {
    (void)e;
    _close(false);
}

static void _close(bool saved) {
    ui_gear_setup_done_cb_t cb = s.cb;
    void *ctx = s.ctx;
    if (s.overlay && lv_obj_is_valid(s.overlay)) lv_obj_del_async(s.overlay);
    memset(&s, 0, sizeof(s));
    if (cb) cb(saved, ctx);
}

/* ── Widget builders ─────────────────────────────────────────────────── */

static lv_obj_t *_make_stepper_btn(lv_obj_t *parent, const char *text,
                                   lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 28, 26);
    lv_obj_set_style_bg_color(btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

/* Build a single labeled row: [name label] ... [−] [value label] [+]
 * `kind` is opaque to the stepper but lets the user_data identify which
 * callback to fire on; `idx` is a sub-index (0 for non-array fields). */
static void _add_value_row(lv_obj_t *parent, const char *name,
                           lv_event_cb_t cb, int kind, int idx,
                           lv_obj_t **out_value_lbl,
                           lv_obj_t **out_row) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_left(row, 6, 0);
    lv_obj_set_style_pad_right(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(name_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(name_lbl, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *plus = _make_stepper_btn(row, "+", cb,
                                       _pack_step(kind, STEP_UP, idx));
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *value_lbl = lv_label_create(row);
    lv_label_set_text(value_lbl, "--");
    lv_obj_align(value_lbl, LV_ALIGN_RIGHT_MID, -36, 0);
    lv_obj_set_style_text_font(value_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(value_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_width(value_lbl, 70);
    lv_obj_set_style_text_align(value_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *minus = _make_stepper_btn(row, "-", cb,
                                        _pack_step(kind, STEP_DOWN, idx));
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -110, 0);

    if (out_value_lbl) *out_value_lbl = value_lbl;
    if (out_row)       *out_row       = row;
}

static void _style_dropdown(lv_obj_t *dd) {
    lv_obj_set_style_bg_color(dd, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(dd, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(dd, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(dd, 6, 0);
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool ui_gear_setup_is_open(void) { return s.overlay != NULL; }

void ui_gear_setup_open(ui_gear_setup_done_cb_t cb, void *ctx) {
    if (s.overlay && lv_obj_is_valid(s.overlay)) return;
    memset(&s, 0, sizeof(s));
    s.cb = cb;
    s.ctx = ctx;

    /* Load existing config; if read fails or first run, seed sensible
     * defaults (generic 5-speed, 4.11 final, 1.95 m tire circumference,
     * RPM + VEHICLE_SPEED signals, disabled). */
    if (config_store_load_gear_cal(&s.cfg) != ESP_OK) {
        memset(&s.cfg, 0, sizeof(s.cfg));
    }
    if (s.cfg.ratio_count == 0) {
        s.cfg.ratio_count = 6;  /* N + 5 forward */
        s.cfg.ratios[0]   = 0.0f;
        s.cfg.ratios[1]   = 3.32f;
        s.cfg.ratios[2]   = 1.92f;
        s.cfg.ratios[3]   = 1.30f;
        s.cfg.ratios[4]   = 1.00f;
        s.cfg.ratios[5]   = 0.83f;
    }
    if (s.cfg.wheel_circumference_m < 0.5f) s.cfg.wheel_circumference_m = 1.95f;
    if (s.cfg.final_drive < 1.0f)           s.cfg.final_drive = 4.11f;
    if (s.cfg.rpm_signal[0]   == '\0') strcpy(s.cfg.rpm_signal,   "RPM");
    if (s.cfg.speed_signal[0] == '\0') strcpy(s.cfg.speed_signal, "VEHICLE_SPEED");

    _build_signal_options(s.dd_options, sizeof(s.dd_options));

    /* Overlay */
    lv_obj_t *scr = lv_layer_top();
    s.overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s.overlay);
    lv_obj_set_size(s.overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(s.overlay);
    lv_obj_set_style_bg_color(s.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(s.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Card */
    s.card = lv_obj_create(s.overlay);
    lv_obj_set_size(s.card, CARD_W, CARD_H);
    lv_obj_center(s.card);
    lv_obj_set_style_bg_color(s.card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s.card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s.card, 1, 0);
    lv_obj_set_style_radius(s.card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(s.card, 16, 0);
    lv_obj_clear_flag(s.card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s.card);
    lv_label_set_text(title, "Calculated Gear Setup");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Enabled switch — top right of header */
    lv_obj_t *enabled_lbl = lv_label_create(s.card);
    lv_label_set_text(enabled_lbl, "Enabled");
    lv_obj_align(enabled_lbl, LV_ALIGN_TOP_RIGHT, -56, 4);
    lv_obj_set_style_text_font(enabled_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(enabled_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s.enabled_sw = lv_switch_create(s.card);
    lv_obj_set_size(s.enabled_sw, 44, 22);
    lv_obj_align(s.enabled_sw, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s.enabled_sw, THEME_COLOR_SECTION_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.enabled_sw, THEME_COLOR_ACCENT_BLUE,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s.cfg.enabled) lv_obj_add_state(s.enabled_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s.enabled_sw, _enabled_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Scrollable body — covers the middle of the card. Flex column so
     * each row stacks neatly. */
    s.list = lv_obj_create(s.card);
    lv_obj_set_size(s.list, CARD_W - 32, CARD_H - 60 - 64);
    lv_obj_align(s.list, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_opa(s.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.list, 0, 0);
    lv_obj_set_style_pad_all(s.list, 4, 0);
    lv_obj_set_flex_flow(s.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s.list, 4, 0);
    lv_obj_set_scroll_dir(s.list, LV_DIR_VER);

    /* Section: signal sources */
    lv_obj_t *sig_row = lv_obj_create(s.list);
    lv_obj_set_size(sig_row, lv_pct(100), 70);
    lv_obj_set_style_bg_opa(sig_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sig_row, 0, 0);
    lv_obj_set_style_pad_all(sig_row, 0, 0);
    lv_obj_clear_flag(sig_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *rpm_lbl = lv_label_create(sig_row);
    lv_label_set_text(rpm_lbl, "RPM signal");
    lv_obj_align(rpm_lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(rpm_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(rpm_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s.rpm_dd = lv_dropdown_create(sig_row);
    lv_obj_set_size(s.rpm_dd, 256, 30);
    lv_obj_align(s.rpm_dd, LV_ALIGN_TOP_LEFT, 0, 14);
    _style_dropdown(s.rpm_dd);
    lv_dropdown_set_options(s.rpm_dd, s.dd_options);
    _select_dd_by_name(s.rpm_dd, s.cfg.rpm_signal);
    lv_obj_add_event_cb(s.rpm_dd, _rpm_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *spd_lbl = lv_label_create(sig_row);
    lv_label_set_text(spd_lbl, "Vehicle speed signal");
    lv_obj_align(spd_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_text_font(spd_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(spd_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s.speed_dd = lv_dropdown_create(sig_row);
    lv_obj_set_size(s.speed_dd, 256, 30);
    lv_obj_align(s.speed_dd, LV_ALIGN_TOP_RIGHT, 0, 14);
    _style_dropdown(s.speed_dd);
    lv_dropdown_set_options(s.speed_dd, s.dd_options);
    _select_dd_by_name(s.speed_dd, s.cfg.speed_signal);
    lv_obj_add_event_cb(s.speed_dd, _speed_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Numeric rows — wheel circumference, final drive, gear count */
    _add_value_row(s.list, "Wheel circumference (m)",
                   _wheel_step_cb, 0, 0, &s.wheel_lbl, NULL);
    _add_value_row(s.list, "Final drive ratio",
                   _fd_step_cb, 0, 0, &s.fd_lbl, NULL);
    _add_value_row(s.list, "Forward gears",
                   _count_step_cb, 0, 0, &s.count_lbl, NULL);

    /* Per-gear ratio rows. Always create all 8; show/hide by ratio_count. */
    for (uint8_t i = 0; i < MAX_RATIO_ROWS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Gear %u ratio", (unsigned)(i + 1));
        _add_value_row(s.list, name, _ratio_step_cb, 1, i,
                       &s.ratio_lbl[i], &s.ratio_row[i]);
    }

    /* Footer buttons — Cancel left, Save right (mirror ECU picker). */
    lv_obj_t *save_btn = lv_btn_create(s.card);
    lv_obj_set_size(save_btn, 240, 40);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_MID, 130, -8);
    lv_obj_set_style_bg_color(save_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(save_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);
    lv_obj_set_style_text_font(save_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(save_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(save_btn, _save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_btn = lv_btn_create(s.card);
    lv_obj_set_size(cancel_btn, 240, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, -130, -8);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(cancel_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(cancel_btn, _cancel_cb, LV_EVENT_CLICKED, NULL);

    /* Initial paint of values + per-gear row visibility. */
    _refresh_value_labels();
    _refresh_ratio_rows();
}
