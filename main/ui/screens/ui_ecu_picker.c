/* ui_ecu_picker.c — shared ECU-selection overlay.
 *
 * Card-list picker: one row per Make·Version preset, each with its own
 * live blue dot that lights up when the bus is broadcasting that
 * preset's CAN IDs. Mirrors the on-device OBD2 picker style and the
 * web ECU Preset modal. Auto/Manual switch in the header filters out
 * non-detected presets when the user wants to declutter.
 *
 * Live data: ecu_preset_match_score() reads from can_id_tracker
 * (continuous per-ID tracking with last_seen_us timestamps) so a row
 * lights up within ~2s of the loom being attached, and dims within
 * ~2.5s of being disconnected. The refresh timer runs at 500 ms while
 * the picker is open — cheap (≤ 10 presets × ≤ 64 tracked IDs).
 *
 * Applies the selection to the named layout + persists in NVS. The
 * done callback receives an `applied` flag so the caller can trigger
 * a dashboard reload at the appropriate moment (reloading inline
 * would destroy any parent overlay — see first_run_wizard.c for the
 * wizard path).
 */

#include "ui_ecu_picker.h"

#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#include "../theme.h"
#include "../../layout/ecu_presets.h"
#include "../../layout/layout_manager.h"
#include "../../storage/config_store.h"
#include "../settings/ui_gear_setup.h"

static const char *TAG = "ecu_picker";

#define CARD_W 600
#define CARD_H 460
#define ROW_H   38

/* Compile-time cap on the list. The actual count is ECU_PRESETS_COUNT
 * (runtime constant) + 1 for the Custom row. 32 gives a generous ceiling
 * — the current table has 10 — and lets us size the rows[] struct field
 * without dragging in a heap allocation. If you ever need to exceed
 * this, switch rows[] to a heap-allocated array sized at picker_open
 * time. _rebuild_list asserts the limit. */
#define MAX_PICKER_ROWS 32

/* Index sentinel for the "Custom / None" pseudo-row at the bottom of the
 * list — it's not a real preset so it doesn't appear in ECU_PRESETS[]. */
#define CUSTOM_IDX (-1)

typedef struct {
    lv_obj_t *root;          /* the clickable row container */
    lv_obj_t *dot;           /* small filled circle, blue when detected */
    lv_obj_t *label;         /* "Make · Version" or "Custom / None" */
    lv_obj_t *score_lbl;     /* "NN% match" — only visible when detected */
    int       preset_idx;    /* ECU_PRESETS index, or CUSTOM_IDX */
    int       last_score;    /* cached so _refresh_dots can skip no-op repaints */
} picker_row_t;

typedef struct {
    lv_obj_t   *overlay;
    lv_obj_t   *card;
    lv_obj_t   *auto_sw;     /* Auto/Manual toggle in header */
    lv_obj_t   *list;        /* scrollable container holding picker rows */
    lv_obj_t   *empty_lbl;   /* "Nothing detected — toggle Auto on" hint */
    lv_obj_t   *apply_btn;
    lv_obj_t   *apply_label;
    lv_timer_t *refresh_timer;
    picker_row_t rows[MAX_PICKER_ROWS];
    int          row_count;
    int          selected_row; /* index into rows[], or -1 */
    char         layout_name[LAYOUT_MAX_NAME];
    bool         manual_mode;
    ecu_picker_done_cb_t cb;
    void        *ctx;
} ecu_picker_state_t;

static ecu_picker_state_t s;

/* Forward decls. */
static void _close(bool applied);
static void _row_click_cb(lv_event_t *e);
static void _apply_cb(lv_event_t *e);
static void _skip_cb(lv_event_t *e);
static void _auto_sw_event_cb(lv_event_t *e);
static void _refresh_timer_cb(lv_timer_t *t);
static void _rebuild_list(int preselect_preset_idx);
static void _refresh_dots(void);
static void _update_row_visual(picker_row_t *r, int score);
static void _set_selected(int row_index);

/* ── Helpers ───────────────────────────────────────────────────────────── */

static bool _preset_is_detected(const ecu_preset_t *p) {
    return p && ecu_preset_match_score(p) >= ECU_PRESET_MATCH_THRESHOLD;
}

/* Find the ECU_PRESETS index that matches the given make+version, or -1.
 * Used at open-time to preselect the saved ECU. */
static int _find_preset_index(const char *make, const char *version) {
    if (!make || !version || !make[0]) return -1;
    for (int i = 0; i < ECU_PRESETS_COUNT; i++) {
        if (strcmp(ECU_PRESETS[i].make, make) == 0 &&
            strcmp(ECU_PRESETS[i].version, version) == 0) {
            return i;
        }
    }
    return -1;
}

/* Apply enable state: a row must be selected to apply. Custom row
 * (CUSTOM_IDX) is always applicable (clears NVS). */
static void _update_apply_state(void) {
    if (!s.apply_btn || !s.apply_label) return;
    bool enable = (s.selected_row >= 0);
    if (enable) {
        lv_obj_clear_state(s.apply_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s.apply_btn, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_text_color(s.apply_label, THEME_COLOR_TEXT_ON_ACCENT, 0);
    } else {
        lv_obj_add_state(s.apply_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s.apply_btn, THEME_COLOR_SECTION_BG, 0);
        lv_obj_set_style_text_color(s.apply_label, THEME_COLOR_TEXT_MUTED, 0);
    }
}

/* Set highlight on the chosen row (or none if -1). Updates the Apply
 * button's enabled state at the same time. */
static void _set_selected(int row_index) {
    s.selected_row = row_index;
    for (int i = 0; i < s.row_count; i++) {
        if (!s.rows[i].root || !lv_obj_is_valid(s.rows[i].root)) continue;
        bool sel = (i == row_index);
        lv_obj_set_style_bg_color(s.rows[i].root,
            sel ? THEME_COLOR_INPUT_BG : THEME_COLOR_PANEL,
            LV_PART_MAIN);
        lv_obj_set_style_border_color(s.rows[i].root,
            sel ? THEME_COLOR_ACCENT_BLUE : THEME_COLOR_BORDER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s.rows[i].root, sel ? 2 : 1, LV_PART_MAIN);
    }
    _update_apply_state();
}

/* Paint a row's blue dot + score badge based on the score. The
 * `last_score` cache lets the periodic refresh skip the LVGL style
 * writes (which always invalidate, even when the value doesn't change)
 * — a deliberate optimisation given the timer ticks at 2 Hz. */
static void _update_row_visual(picker_row_t *r, int score) {
    if (!r || !r->root || !lv_obj_is_valid(r->root)) return;
    bool detected = (score >= ECU_PRESET_MATCH_THRESHOLD);
    bool was_detected = (r->last_score >= ECU_PRESET_MATCH_THRESHOLD);

    if (detected != was_detected) {
        lv_color_t c = detected ? THEME_COLOR_ACCENT_BLUE : THEME_COLOR_BORDER_MED;
        lv_obj_set_style_bg_color(r->dot, c, LV_PART_MAIN);
    }

    if (detected) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", score);
        lv_label_set_text(r->score_lbl, buf);
        if (!was_detected) {
            lv_obj_clear_flag(r->score_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (was_detected) {
        lv_obj_add_flag(r->score_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    r->last_score = score;
}

/* Visit every row, recompute its score, repaint the dot + badge. Called
 * by the refresh timer. Cheap: row_count is ≤ 11 and each match-score
 * call walks ≤ 64 tracker entries. */
static void _refresh_dots(void) {
    for (int i = 0; i < s.row_count; i++) {
        if (s.rows[i].preset_idx == CUSTOM_IDX) continue;  /* no score for Custom */
        const ecu_preset_t *p = &ECU_PRESETS[s.rows[i].preset_idx];
        _update_row_visual(&s.rows[i], ecu_preset_match_score(p));
    }
}

/* Builds one row (clickable container, blue dot, label, optional score)
 * inside the scrollable list. The dot starts grey; _refresh_dots paints
 * the live state immediately after. */
static lv_obj_t *_make_row(lv_obj_t *parent, int row_idx, int preset_idx,
                           const char *display_text) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_pad_top(row, 6, 0);
    lv_obj_set_style_pad_bottom(row, 6, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_bg_color(row, THEME_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, THEME_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, THEME_RADIUS_NORMAL, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, _row_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)row_idx);

    /* Blue dot — small filled circle on the left. */
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, THEME_COLOR_BORDER_MED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    /* Main label. */
    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
    lv_label_set_text(lbl, display_text ? display_text : "?");
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    /* Score badge on the right — hidden until detected. */
    lv_obj_t *score = lv_label_create(row);
    lv_obj_align(score, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(score, "");
    lv_obj_set_style_text_font(score, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(score, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_add_flag(score, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(score, LV_OBJ_FLAG_CLICKABLE);

    s.rows[row_idx].root      = row;
    s.rows[row_idx].dot       = dot;
    s.rows[row_idx].label     = lbl;
    s.rows[row_idx].score_lbl = score;
    s.rows[row_idx].preset_idx = preset_idx;
    s.rows[row_idx].last_score = 0;

    return row;
}

/* Tears down all existing rows and rebuilds the list from ECU_PRESETS.
 * Manual mode hides non-detected presets (but always keeps Custom).
 * If Manual ends up empty (no preset detected yet), shows a small
 * hint label instead of leaving the list blank. */
static void _rebuild_list(int preselect_preset_idx) {
    if (!s.list || !lv_obj_is_valid(s.list)) return;

    /* Wipe old rows. lv_obj_clean removes children + their children. */
    lv_obj_clean(s.list);
    memset(s.rows, 0, sizeof(s.rows));
    s.row_count = 0;

    /* Collect candidate presets, optionally filtered, sorted detected-first
     * by score descending. ECU_PRESETS_COUNT is small (~10), so simple
     * insertion-sort suffices. */
    int order[ECU_PRESETS_COUNT];
    int scores[ECU_PRESETS_COUNT];
    int n_kept = 0;
    for (int i = 0; i < ECU_PRESETS_COUNT; i++) {
        int score = ecu_preset_match_score(&ECU_PRESETS[i]);
        bool detected = (score >= ECU_PRESET_MATCH_THRESHOLD);
        if (s.manual_mode && !detected) continue;
        order[n_kept]  = i;
        scores[n_kept] = score;
        n_kept++;
    }

    /* If Manual filtered everything away, fall back to showing all so
     * the user is never stranded. */
    bool fallback = (s.manual_mode && n_kept == 0);
    if (fallback) {
        for (int i = 0; i < ECU_PRESETS_COUNT; i++) {
            order[i]  = i;
            scores[i] = ecu_preset_match_score(&ECU_PRESETS[i]);
        }
        n_kept = ECU_PRESETS_COUNT;
    }

    /* Insertion sort: detected (score >= threshold) ahead of undetected;
     * within each group, higher score first; otherwise alphabetical by
     * make+version. */
    for (int i = 1; i < n_kept; i++) {
        int j = i;
        while (j > 0) {
            int a = order[j - 1], b = order[j];
            int sa = scores[j - 1], sb = scores[j];
            bool a_det = (sa >= ECU_PRESET_MATCH_THRESHOLD);
            bool b_det = (sb >= ECU_PRESET_MATCH_THRESHOLD);
            bool swap = false;
            if (a_det != b_det) swap = b_det && !a_det;
            else if (sa != sb)  swap = sb > sa;
            else {
                char la[80], lb[80];
                snprintf(la, sizeof(la), "%s %s", ECU_PRESETS[a].make, ECU_PRESETS[a].version);
                snprintf(lb, sizeof(lb), "%s %s", ECU_PRESETS[b].make, ECU_PRESETS[b].version);
                swap = (strcmp(lb, la) < 0);
            }
            if (!swap) break;
            order[j - 1]  = b; order[j]  = a;
            scores[j - 1] = sb; scores[j] = sa;
            j--;
        }
    }

    /* Emit one row per kept preset. Bounds-check against rows[] capacity —
     * silently truncates if the preset table ever exceeds MAX_PICKER_ROWS.
     * In practice today this is 10 + 1 vs. 32, lots of headroom. */
    int new_selected = -1;
    for (int k = 0; k < n_kept; k++) {
        if (s.row_count >= MAX_PICKER_ROWS - 1) break;  /* leave a slot for Custom */
        int row_idx = s.row_count;
        const ecu_preset_t *p = &ECU_PRESETS[order[k]];
        const char *display = p->display ? p->display : p->make;
        _make_row(s.list, row_idx, order[k], display);
        s.row_count++;
        if (order[k] == preselect_preset_idx) new_selected = row_idx;
    }

    /* Custom / None always present. */
    int custom_row = s.row_count;
    _make_row(s.list, custom_row, CUSTOM_IDX, "Custom / None (configure signals manually)");
    s.row_count++;
    if (preselect_preset_idx == CUSTOM_IDX && new_selected < 0) {
        new_selected = custom_row;
    }

    /* Empty-state hint — only visible when Manual filter would have
     * left zero detected presets and we fell back to showing all. */
    if (s.empty_lbl) {
        if (fallback) {
            lv_label_set_text(s.empty_lbl,
                "No presets detected on the bus yet — showing all.");
            lv_obj_clear_flag(s.empty_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s.empty_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    _refresh_dots();
    _set_selected(new_selected);
}

/* ── Event handlers ───────────────────────────────────────────────────── */

static void _row_click_cb(lv_event_t *e) {
    int row_idx = (int)(intptr_t)lv_event_get_user_data(e);
    _set_selected(row_idx);
}

static void _auto_sw_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool checked = lv_obj_has_state(s.auto_sw, LV_STATE_CHECKED);
    bool auto_mode = checked;
    s.manual_mode = !auto_mode;
    config_store_save_ecu_picker_auto(auto_mode);

    /* Preserve selection by preset_idx across the rebuild. */
    int sel_preset = -2;
    if (s.selected_row >= 0 && s.selected_row < s.row_count) {
        sel_preset = s.rows[s.selected_row].preset_idx;
    }
    _rebuild_list(sel_preset);
}

static void _refresh_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s.list || !lv_obj_is_valid(s.list)) return;

    if (s.manual_mode) {
        /* Manual: if the visible-set has changed (preset just started
         * broadcasting or went silent), rebuild rows. Otherwise just
         * refresh the dots. */
        int detected_now[ECU_PRESETS_COUNT];
        int n_now = 0;
        for (int i = 0; i < ECU_PRESETS_COUNT; i++) {
            if (_preset_is_detected(&ECU_PRESETS[i])) detected_now[n_now++] = i;
        }
        /* Compare against rows currently rendering ECU_PRESETS entries. */
        int n_rendered = 0;
        for (int i = 0; i < s.row_count; i++) {
            if (s.rows[i].preset_idx >= 0) n_rendered++;
        }
        bool changed = (n_rendered != n_now);
        if (!changed) {
            /* Same count — check the identity matches too. */
            int found = 0;
            for (int k = 0; k < n_now; k++) {
                for (int i = 0; i < s.row_count; i++) {
                    if (s.rows[i].preset_idx == detected_now[k]) { found++; break; }
                }
            }
            changed = (found != n_now);
        }
        if (changed) {
            int sel_preset = -2;
            if (s.selected_row >= 0 && s.selected_row < s.row_count) {
                sel_preset = s.rows[s.selected_row].preset_idx;
            }
            _rebuild_list(sel_preset);
            return;
        }
    }
    _refresh_dots();
}

/* lv_async_call shim — opens the Gear Setup overlay on the next LVGL tick.
 * Used after the ECU picker closes itself when the user applies the
 * RDM-7 / Internal marker preset. Async because we need the picker's
 * destroy to finish before pushing a new overlay onto lv_layer_top. */
static void _open_gear_setup_async(void *unused) {
    (void)unused;
    ui_gear_setup_open(NULL, NULL);
}

static void _apply_cb(lv_event_t *e) {
    (void)e;
    if (s.selected_row < 0 || s.selected_row >= s.row_count) {
        _close(false);
        return;
    }
    int preset_idx = s.rows[s.selected_row].preset_idx;

    bool applied = false;
    bool is_rdm_internal = false;
    if (preset_idx == CUSTOM_IDX) {
        config_store_save_ecu("", "");
    } else {
        const ecu_preset_t *p = &ECU_PRESETS[preset_idx];
        if (ecu_preset_apply_to_layout(s.layout_name, p) != ESP_OK) {
            ESP_LOGE(TAG, "apply failed for %s / %s", p->make, p->version);
        } else {
            config_store_save_ecu(p->make, p->version);
            applied = true;
            /* RDM-7 / Internal is the marker preset that drives the
             * CALCULATED_GEAR synthetic signal. After applying it the
             * user still needs to configure RPM/Speed sources + gear
             * ratios — auto-pop the Gear Setup modal so the dash-only
             * user gets a working setup without a trip to the web
             * editor. Same UX as the web side's openGearSetup() hook. */
            if (p->make && p->version &&
                strcmp(p->make, "RDM-7") == 0 &&
                strcmp(p->version, "Internal") == 0) {
                is_rdm_internal = true;
            }
        }
    }
    _close(applied);
    if (is_rdm_internal) lv_async_call(_open_gear_setup_async, NULL);
}

static void _skip_cb(lv_event_t *e) {
    (void)e;
    _close(false);
}

static void _close(bool applied) {
    ecu_picker_done_cb_t cb = s.cb;
    void *ctx = s.ctx;
    if (s.refresh_timer) {
        lv_timer_del(s.refresh_timer);
        s.refresh_timer = NULL;
    }
    if (s.overlay && lv_obj_is_valid(s.overlay)) lv_obj_del_async(s.overlay);
    memset(&s, 0, sizeof(s));
    s.selected_row = -1;
    if (cb) cb(applied, ctx);
}

/* ── Button helper (unchanged from before) ────────────────────────────── */

static lv_obj_t *_make_btn(lv_obj_t *parent, const char *text,
                           lv_color_t bg, lv_color_t fg, lv_coord_t x_ofs,
                           lv_event_cb_t cb, lv_obj_t **out_label) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 240, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x_ofs, -10);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    if (out_label) *out_label = lbl;
    return btn;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void ecu_picker_open(const char *layout_name, bool allow_skip,
                     ecu_picker_done_cb_t cb, void *ctx) {
    if (s.overlay && lv_obj_is_valid(s.overlay)) return;

    memset(&s, 0, sizeof(s));
    s.cb = cb;
    s.ctx = ctx;
    s.selected_row = -1;
    strncpy(s.layout_name, layout_name ? layout_name : "default",
            sizeof(s.layout_name) - 1);
    s.manual_mode = !config_store_load_ecu_picker_auto();

    char cur_make[32] = {0}, cur_ver[32] = {0};
    config_store_load_ecu(cur_make, sizeof(cur_make), cur_ver, sizeof(cur_ver));
    int preselect = _find_preset_index(cur_make, cur_ver);
    if (preselect < 0 && cur_make[0] == '\0') preselect = CUSTOM_IDX;

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
    lv_obj_set_style_pad_all(s.card, 20, 0);
    lv_obj_clear_flag(s.card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s.card);
    lv_label_set_text(title, "Select Your ECU");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *sub = lv_label_create(s.card);
    lv_label_set_text(sub, "Blue dot = receiving signal from this preset right now.");
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Auto switch — top-right. */
    lv_obj_t *auto_lbl = lv_label_create(s.card);
    lv_label_set_text(auto_lbl, "Auto");
    lv_obj_align(auto_lbl, LV_ALIGN_TOP_RIGHT, -60, 4);
    lv_obj_set_style_text_font(auto_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(auto_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s.auto_sw = lv_switch_create(s.card);
    lv_obj_set_size(s.auto_sw, 40, 22);
    lv_obj_align(s.auto_sw, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s.auto_sw, THEME_COLOR_SECTION_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.auto_sw, THEME_COLOR_ACCENT_BLUE,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (!s.manual_mode) lv_obj_add_state(s.auto_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s.auto_sw, _auto_sw_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Empty-state hint label — shown only when Manual filtered to zero
     * and we fell back to showing all. _rebuild_list toggles visibility. */
    s.empty_lbl = lv_label_create(s.card);
    lv_label_set_text(s.empty_lbl, "");
    lv_obj_align(s.empty_lbl, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_set_style_text_font(s.empty_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s.empty_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_flag(s.empty_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Scrollable list. Flex-column layout auto-spaces rows. */
    s.list = lv_obj_create(s.card);
    lv_obj_set_size(s.list, CARD_W - 40, CARD_H - 60 - 70);  /* card minus header + button row */
    lv_obj_align(s.list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(s.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.list, 0, 0);
    lv_obj_set_style_pad_all(s.list, 0, 0);
    lv_obj_set_flex_flow(s.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s.list, 4, 0);
    lv_obj_set_scroll_dir(s.list, LV_DIR_VER);

    _rebuild_list(preselect);

    /* Buttons */
    s.apply_btn = _make_btn(s.card, "Apply",
                            THEME_COLOR_ACCENT_BLUE, THEME_COLOR_TEXT_ON_ACCENT,
                            allow_skip ? 130 : 0, _apply_cb, &s.apply_label);
    if (allow_skip) {
        _make_btn(s.card, "Skip",
                  THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_MUTED,
                  -130, _skip_cb, NULL);
    }
    _update_apply_state();

    /* Kick off the live-refresh timer last so it can't fire before the
     * widgets exist. Cleaned up in _close. */
    s.refresh_timer = lv_timer_create(_refresh_timer_cb, 500, NULL);
}
