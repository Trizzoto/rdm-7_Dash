#include "preset_picker.h"
#include "theme.h"
#include <stdio.h>
#include "lvgl.h"
#include "screens/ui_Screen3.h"
#include "widgets/lv_dropdown.h"
#include <string.h>
#include <stdlib.h>
#include "storage/config_store.h"
#include "can/obd2.h"
#include "widgets/signal.h"
#include <ctype.h>


/* preconfig_item_t is defined in preset_picker.h */

/* preconfig_items[] itself lives in preset_picker_data.c — a data-only
 * translation unit with no LVGL in sight, so the host-side catalogue
 * emitter (tools/native/gen_channel_catalog.c) can compile the REAL
 * table instead of parsing this file or carrying a copy that drifts
 * (ADR-0033). Everything below is UI and stays put. */

/* ── OBD2 channels (synthesized from OBD2_PIDS at runtime) ──────────────────
 * Built lazily on first picker open. Each entry mirrors an OBD2 PID with
 * obd2_pid set so the apply path (in config_modal.c) knows to enable the
 * PID and bind the widget by signal name, rather than overwriting CAN
 * bit-decode params on the widget's existing signal. */

#define OBD2_LABEL_BUF_LEN 28
/* Sized for current built-in entry count + custom PID slack. 69 today
 * (50 Mode 01 + 4 Toyota + 9 diesel sub-fields + 7 Toyota 0x80 subs)
 * plus headroom for OBD2_MAX_CUSTOM_PIDS=32 puts us at ~100. 96 is a
 * compromise: covers all built-ins + ~27 custom, without bloating BSS
 * by 48 × sizeof(preconfig_item_t) ≈ 2 KB. */
#define OBD2_PICKER_MAX 96
static preconfig_item_t s_obd2_items[OBD2_PICKER_MAX];
static char             s_obd2_labels[OBD2_PICKER_MAX][OBD2_LABEL_BUF_LEN];
static int              s_obd2_count = 0;

/* Map an OBD2 PID definition to a (brand, version) pair for picker
 * grouping. PIDs with category set get their own brand bucket; bare
 * PIDs (Mode 01 standard) fall under OBD2 / Standard. */
static void _brand_version_for_def(const obd2_pid_def_t *def,
                                   const char **out_brand,
                                   const char **out_version)
{
    if (def->category && def->category[0]) {
        *out_brand = def->category;     /* e.g. "Toyota" */
        /* Map service byte to a human-recognisable version label.
         * Toyota uses Mode 21 (KWP-derived); future Mode 22 packs
         * would naturally land in "Mode 22". */
        uint8_t s = def->service ? def->service : 0x01;
        switch (s) {
            case 0x21: *out_version = "Mode 21"; break;
            case 0x22: *out_version = "Mode 22"; break;
            default:   *out_version = "Standard"; break;
        }
    } else {
        *out_brand   = "OBD2";
        *out_version = "Standard";
    }
}

/* Populate one preconfig_item_t for an OBD2 signal — used for both
 * single-value PIDs and packed-PID sub-fields. The label round-trips
 * through label_to_signal_name in config_modal so the apply path can
 * recover the signal name without storing it separately. */
static void _add_obd2_item(const obd2_pid_def_t *def,
                           const char *signal_name,
                           float scale, float offset, bool is_signed)
{
    if (!signal_name || s_obd2_count >= OBD2_PICKER_MAX) return;
    int idx = s_obd2_count;

    /* Build label by replacing underscores with spaces.
     * "COOLANT_TEMP" -> "COOLANT TEMP". label_to_signal_name reverses
     * it lossless so the apply path recovers the signal name. */
    size_t j = 0;
    for (size_t k = 0; signal_name[k] && j < OBD2_LABEL_BUF_LEN - 1; k++) {
        s_obd2_labels[idx][j++] = (signal_name[k] == '_') ? ' '
                                                          : signal_name[k];
    }
    s_obd2_labels[idx][j] = '\0';

    const char *brand = NULL;
    const char *version = NULL;
    _brand_version_for_def(def, &brand, &version);

    s_obd2_items[idx].ecu          = brand;
    s_obd2_items[idx].version      = version;
    s_obd2_items[idx].label        = s_obd2_labels[idx];
    s_obd2_items[idx].can_id       = "0";
    s_obd2_items[idx].endianess    = 0;
    s_obd2_items[idx].bit_start    = 0;
    s_obd2_items[idx].bit_length   = 0;
    s_obd2_items[idx].scale        = scale;
    s_obd2_items[idx].value_offset = offset;
    s_obd2_items[idx].decimals     = (scale >= 1.0f) ? 0
                                   : (scale >= 0.1f) ? 1 : 2;
    s_obd2_items[idx].is_signed    = is_signed;
    s_obd2_items[idx].obd2_pid     = def->pid;
    s_obd2_items[idx].obd2_service = def->service ? def->service : 0x01;
    s_obd2_count++;
}

static void _ensure_obd2_items_built(void)
{
    /* Always rebuild — custom PIDs may have been added/removed since the
     * last picker open. ~80 entries × small struct = trivial cost. */
    s_obd2_count = 0;
    uint8_t total = obd2_pid_total_count();
    for (uint8_t i = 0; i < total && s_obd2_count < OBD2_PICKER_MAX; i++) {
        const obd2_pid_def_t *p = obd2_pid_at(i);
        if (!p) continue;

        if (p->sub_fields && p->sub_field_count > 0) {
            /* Packed PID (e.g. Toyota Mode 21 engine block): one entry
             * per sub-field. Picking any will auto-enable polling for
             * the parent PID via _apply_obd2_preset, and the widget
             * binds to that sub-field's signal name. */
            for (uint8_t j = 0; j < p->sub_field_count && s_obd2_count < OBD2_PICKER_MAX; j++) {
                const obd2_subfield_t *sf = &p->sub_fields[j];
                _add_obd2_item(p, sf->signal_name,
                               sf->scale, sf->offset, sf->is_signed);
            }
            continue;
        }

        if (!p->signal_name) continue;
        _add_obd2_item(p, p->signal_name, p->scale, p->offset, false);
    }
}

/* Resolve a sel_sig index that can point at either preconfig_items[] or the
 * synthesized OBD2 entries. Indices < (preconfig_items_count - 1) are CAN
 * preset entries; indices >= it are offsets into s_obd2_items[]. -1 = none. */
static const preconfig_item_t *_resolve_item(int idx)
{
    if (idx < 0) return NULL;
    int base = preconfig_items_count - 1;   /* exclude trailing sentinel */
    if (idx < base) return &preconfig_items[idx];
    int oi = idx - base;
    _ensure_obd2_items_built();
    if (oi >= 0 && oi < s_obd2_count) return &s_obd2_items[oi];
    return NULL;
}

/* Encode an OBD2 entry's local index into the unified index space. */
static int _obd2_idx_to_unified(int oi)
{
    return (preconfig_items_count - 1) + oi;
}

/* =========================================================================
 * Full-screen 3-column preset browser
 *
 * Layout (780 × 456 centred panel):
 *  ┌─ SELECT PRESET ─────────────────────────────────────────── [CLOSE] ─┐ 48px
 *  ├──────────────┬────────────────┬──────────────────────────────────────┤ 356px
 *  │  BRAND       │  PROTOCOL      │  CHANNEL                             │
 *  │  MaxxECU ●  │  v1.2          │  THROTTLE %                          │
 *  │  Haltech     │  v1.3 ●       │  MAP                                 │
 *  │  Ford        │                │  LAMBDA ...  (scrollable)            │
 *  ├──────────────┴────────────────┴──────────────────────────────────────┤ 52px
 *  │  ○ No channel selected                           [✓  APPLY PRESET]  │
 *  └──────────────────────────────────────────────────────────────────────┘
 * ========================================================================= */

/* ── State ──────────────────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *ver_list;       /* inner flex container – version column  */
    lv_obj_t *sig_list;       /* inner flex container – channel column  */
    lv_obj_t *preview_lbl;
    lv_obj_t *apply_btn;
    char      sel_brand[32];
    char      sel_ver[32];
    int       sel_sig;        /* index into preconfig_items[], -1 = none */
    lv_obj_t *hi_brand;
    lv_obj_t *hi_ver;
    lv_obj_t *hi_sig;
    /* Live indicator timer: walks signal-column rows every ~500 ms and
     * shows a blue dot next to channels whose signal is currently fresh
     * in the registry. Lets the user see at a glance which channels
     * will actually work without trial-and-error binding. */
    lv_timer_t       *live_timer;
    preset_apply_cb_t  apply_cb;
    void              *apply_cb_ctx;
} picker_st_t;

typedef struct { picker_st_t *st; const char *name; } col_txt_ctx_t;
typedef struct { picker_st_t *st; int idx; }           col_sig_ctx_t;

/* Forward declarations of populate helpers (used by click callbacks) */
static void populate_ver_col(picker_st_t *st);
static void populate_sig_col(picker_st_t *st);
static void update_picker_preview(picker_st_t *st, int idx);
static void _live_indicator_refresh(lv_timer_t *t);

/* ── Memory free callbacks ───────────────────────────────────────────────── */
static void picker_st_free_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    picker_st_t *st = (picker_st_t *)lv_event_get_user_data(e);
    if (!st) return;
    if (st->live_timer) {
        lv_timer_del(st->live_timer);
        st->live_timer = NULL;
    }
    lv_mem_free(st);
}
static void col_txt_free_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    col_txt_ctx_t *c = (col_txt_ctx_t *)lv_event_get_user_data(e);
    if (c) lv_mem_free(c);
}
static void col_sig_free_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    col_sig_ctx_t *c = (col_sig_ctx_t *)lv_event_get_user_data(e);
    if (c) lv_mem_free(c);
}

/* ── Row highlight (accent left-bar + bright text) ───────────────────────── */
static void set_row_hi(lv_obj_t *row, bool on)
{
    if (!row || !lv_obj_is_valid(row)) return;
    lv_obj_set_style_bg_color(row, on ? THEME_COLOR_ACCENT_DIM : THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(row, on ? 3 : 0, 0);
    lv_obj_set_style_border_side(row,  LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, THEME_COLOR_ACCENT, 0);
    lv_obj_t *lbl = lv_obj_get_child(row, 0);
    if (lbl) lv_obj_set_style_text_color(lbl,
        on ? THEME_COLOR_TEXT_PRIMARY : THEME_COLOR_TEXT_MUTED, 0);
}

/* ── Click callbacks ─────────────────────────────────────────────────────── */
static void brand_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    col_txt_ctx_t *ctx = (col_txt_ctx_t *)lv_event_get_user_data(e);
    picker_st_t *st = ctx->st;
    set_row_hi(st->hi_brand, false);
    st->hi_brand = lv_event_get_target(e);
    set_row_hi(st->hi_brand, true);
    strncpy(st->sel_brand, ctx->name, sizeof(st->sel_brand) - 1);
    populate_ver_col(st);
}
static void ver_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    col_txt_ctx_t *ctx = (col_txt_ctx_t *)lv_event_get_user_data(e);
    picker_st_t *st = ctx->st;
    set_row_hi(st->hi_ver, false);
    st->hi_ver = lv_event_get_target(e);
    set_row_hi(st->hi_ver, true);
    strncpy(st->sel_ver, ctx->name, sizeof(st->sel_ver) - 1);
    populate_sig_col(st);
}
/* Core apply logic — shared by channel-click (embedded picker, auto-apply)
 * and the Apply button (standalone overlay). */
static void _apply_selection(picker_st_t *st)
{
    if (!st->apply_cb || st->sel_sig < 0) return;
    const preconfig_item_t *it = _resolve_item(st->sel_sig);
    if (!it) return;
    /* Capture the callback + ctx, then update the preview label BEFORE invoking
     * it. The apply callback may tear down the picker — the wizard bind-sheet
     * closes itself (lv_obj_del) on apply — which frees `st` and `preview_lbl`.
     * Touching them after the callback was a use-after-free (crash in
     * lv_label_set_text -> lv_obj_invalidate -> lv_obj_get_disp). So the
     * callback MUST be the last thing we do with `st`. `it` points into the
     * static preconfig tables, so it stays valid across the callback. */
    preset_apply_cb_t cb = st->apply_cb;
    void *cb_ctx = st->apply_cb_ctx;
    if (st->preview_lbl) {
        char buf[96];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Applied: %s", it->label);
        lv_label_set_text(st->preview_lbl, buf);
        lv_obj_set_style_text_color(st->preview_lbl, THEME_COLOR_GREEN, 0);
    }
    cb(it, cb_ctx);   /* may free st — do NOT touch st after this */
}

static void sig_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    col_sig_ctx_t *ctx = (col_sig_ctx_t *)lv_event_get_user_data(e);
    picker_st_t *st = ctx->st;
    set_row_hi(st->hi_sig, false);
    st->hi_sig = lv_event_get_target(e);
    set_row_hi(st->hi_sig, true);
    st->sel_sig = ctx->idx;
    update_picker_preview(st, ctx->idx);
    /* Auto-apply on channel selection — no separate Apply step needed.
     * The modal's SAVE button is the sole commit-to-disk action; pending
     * edits (including this preset binding) go out together on SAVE. */
    _apply_selection(st);
}

/* ── Preview footer update ───────────────────────────────────────────────── */
static void update_picker_preview(picker_st_t *st, int idx)
{
    if (!st->preview_lbl) return;
    /* Reset colour in case it was set to green by a previous Apply */
    lv_obj_set_style_text_color(st->preview_lbl, THEME_COLOR_TEXT_MUTED, 0);
    if (idx < 0) {
        lv_label_set_text(st->preview_lbl, "Select a brand, protocol, then channel");
        if (st->apply_btn) lv_obj_add_state(st->apply_btn, LV_STATE_DISABLED);
    } else {
        const preconfig_item_t *it = _resolve_item(idx);
        if (!it) {
            lv_label_set_text(st->preview_lbl, "");
            if (st->apply_btn) lv_obj_add_state(st->apply_btn, LV_STATE_DISABLED);
            return;
        }
        char buf[128];
        if (it->obd2_pid != 0) {
            uint8_t svc = it->obd2_service ? it->obd2_service : 0x01;
            const obd2_pid_def_t *d = obd2_pid_find_svc(svc, it->obd2_pid);
            const char *rate = (d && d->tier == OBD2_TIER_FAST) ? "10" : "1";
            if (svc == 0x22) {
                snprintf(buf, sizeof(buf),
                         "%s | M22 0x%04X | polled @ ~%s Hz",
                         it->label, (unsigned)it->obd2_pid, rate);
            } else {
                snprintf(buf, sizeof(buf),
                         "%s | M%02X 0x%02X | polled @ ~%s Hz",
                         it->label, svc,
                         (unsigned)(it->obd2_pid & 0xFF), rate);
            }
        } else {
            snprintf(buf, sizeof(buf), "%s | CAN 0x%s | %s | Bit %d  Len %d | x%.4g",
                it->label, it->can_id,
                it->endianess ? "LE" : "BE",
                it->bit_start, it->bit_length,
                (double)it->scale);
        }
        lv_label_set_text(st->preview_lbl, buf);
        if (st->apply_btn) lv_obj_clear_state(st->apply_btn, LV_STATE_DISABLED);
    }
}

/* ── Column builder helpers ──────────────────────────────────────────────── */

/* Scrollable flex-column container that fills remaining height of its parent */
static lv_obj_t *make_col_list(lv_obj_t *col)
{
    lv_obj_t *list = lv_obj_create(col);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_style_bg_color(list, THEME_COLOR_SCROLLBAR,
                               LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    return list;
}

/* One complete column (header strip + scrollable list), returns the list obj */
static lv_obj_t *make_col(lv_obj_t *body, const char *hdr_text, bool right_border,
                           lv_coord_t col_w, lv_coord_t col_h)
{
    lv_obj_t *col = lv_obj_create(body);
    lv_obj_set_size(col, col_w, col_h);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    if (right_border) {
        lv_obj_set_style_border_side(col, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(col, THEME_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(col, 1, 0);
    }

    /* Column header label strip */
    lv_obj_t *chdr = lv_obj_create(col);
    lv_obj_set_size(chdr, lv_pct(100), 28);
    lv_obj_clear_flag(chdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(chdr, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(chdr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chdr, 0, 0);
    lv_obj_set_style_border_width(chdr, 0, 0);
    lv_obj_set_style_border_side(chdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(chdr, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(chdr, 1, 0);
    lv_obj_set_style_pad_left(chdr, 12, 0);
    lv_obj_set_style_pad_right(chdr, 6, 0);
    lv_obj_set_style_pad_top(chdr, 0, 0);
    lv_obj_set_style_pad_bottom(chdr, 0, 0);

    lv_obj_t *clbl = lv_label_create(chdr);
    lv_label_set_text(clbl, hdr_text);
    lv_obj_set_style_text_color(clbl, THEME_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(clbl, THEME_FONT_SMALL, 0);
    lv_obj_align(clbl, LV_ALIGN_LEFT_MID, 0, 0);

    return make_col_list(col);
}

/* A single clickable row inside a column list */
static lv_obj_t *make_col_row(lv_obj_t *list, const char *text)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 6, 0);
    lv_obj_set_style_pad_top(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_set_style_bg_color(row, THEME_COLOR_INPUT_BG, LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, lv_pct(90));
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_BODY, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

/* ── Populate helpers ────────────────────────────────────────────────────── */
/* ── Auto-select the brand + version saved in NVS (config_store_load_ecu).
 * Called right after the brand list is populated. If the saved ECU maps
 * to a row in the list, highlight it and cascade to populate the version
 * column; if a matching version is present, highlight that too. No-op if
 * the NVS key is empty (Custom / None / first run). */
static void _preselect_ecu_from_nvs(picker_st_t *st, lv_obj_t *brand_list)
{
    if (!st || !brand_list) return;
    char ecu_make[32] = {0}, ecu_ver[32] = {0};
    if (config_store_load_ecu(ecu_make, sizeof(ecu_make),
                              ecu_ver, sizeof(ecu_ver)) != ESP_OK)
        return;
    if (ecu_make[0] == '\0') return;

    uint32_t nb = lv_obj_get_child_cnt(brand_list);
    for (uint32_t i = 0; i < nb; i++) {
        lv_obj_t *row = lv_obj_get_child(brand_list, i);
        if (!row) continue;
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (!lbl) continue;
        const char *txt = lv_label_get_text(lbl);
        if (!txt || strcmp(txt, ecu_make) != 0) continue;

        strncpy(st->sel_brand, ecu_make, sizeof(st->sel_brand) - 1);
        set_row_hi(row, true);
        st->hi_brand = row;
        populate_ver_col(st);
        if (ecu_ver[0] == '\0') return;

        uint32_t nv = lv_obj_get_child_cnt(st->ver_list);
        for (uint32_t j = 0; j < nv; j++) {
            lv_obj_t *vrow = lv_obj_get_child(st->ver_list, j);
            if (!vrow) continue;
            lv_obj_t *vlbl = lv_obj_get_child(vrow, 0);
            if (!vlbl) continue;
            const char *vtxt = lv_label_get_text(vlbl);
            if (!vtxt || strcmp(vtxt, ecu_ver) != 0) continue;
            strncpy(st->sel_ver, ecu_ver, sizeof(st->sel_ver) - 1);
            set_row_hi(vrow, true);
            st->hi_ver = vrow;
            populate_sig_col(st);
            return;
        }
        return;
    }
}

static void populate_ver_col(picker_st_t *st)
{
    lv_obj_clean(st->ver_list);
    lv_obj_clean(st->sig_list);
    st->hi_ver = st->hi_sig = NULL;
    st->sel_sig = -1;
    st->sel_ver[0] = '\0';
    update_picker_preview(st, -1);

    const char *vers[16]; int nv = 0;
    for (int i = 0; i < preconfig_items_count - 1 && preconfig_items[i].ecu; i++) {
        if (strcmp(preconfig_items[i].ecu, st->sel_brand) != 0) continue;
        bool dup = false;
        for (int j = 0; j < nv; j++)
            if (strcmp(vers[j], preconfig_items[i].version) == 0) { dup = true; break; }
        if (!dup && nv < 16) vers[nv++] = preconfig_items[i].version;
    }
    /* Add any OBD2-synthesised versions for this brand (e.g. OBD2 →
     * Standard, Toyota → Mode 21, future Ford → Mode 22). */
    _ensure_obd2_items_built();
    for (int oi = 0; oi < s_obd2_count && nv < 16; oi++) {
        const preconfig_item_t *it = &s_obd2_items[oi];
        if (!it->ecu || strcmp(it->ecu, st->sel_brand) != 0) continue;
        if (!it->version) continue;
        bool dup = false;
        for (int j = 0; j < nv; j++)
            if (strcmp(vers[j], it->version) == 0) { dup = true; break; }
        if (!dup) vers[nv++] = it->version;
    }
    /* Sort protocols alphabetically */
    for (int i = 0; i < nv - 1; i++)
        for (int j = i + 1; j < nv; j++)
            if (strcmp(vers[i], vers[j]) > 0) {
                const char *tmp = vers[i]; vers[i] = vers[j]; vers[j] = tmp;
            }
    for (int i = 0; i < nv; i++) {
        lv_obj_t *row = make_col_row(st->ver_list, vers[i]);
        col_txt_ctx_t *ctx = lv_mem_alloc(sizeof(col_txt_ctx_t));
        ctx->st = st; ctx->name = vers[i];
        lv_obj_add_event_cb(row, ver_click_cb,   LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, col_txt_free_cb, LV_EVENT_DELETE, ctx);
    }
}

/* Convert a picker row label (e.g. "COOLANT TEMP", "TY RPM") into the
 * signal-registry key (e.g. "COOLANT_TEMP", "TY_RPM"). Inlined here
 * because the same logic in config_modal.c is static. */
static void _label_to_sig_key(const char *label, char *out, size_t out_sz)
{
    if (!label || !out || out_sz == 0) return;
    size_t j = 0;
    for (size_t i = 0; label[i] && j < out_sz - 1; i++) {
        char c = label[i];
        if (isalnum((unsigned char)c))
            out[j++] = (char)toupper((unsigned char)c);
        else if (j > 0 && out[j - 1] != '_')
            out[j++] = '_';
    }
    if (j > 0 && out[j - 1] == '_') j--;
    out[j] = '\0';
}

/* Walk the picker's signal-column rows and show/hide the blue live-dot
 * stored on each row's user_data based on whether the matching registry
 * signal is currently fresh. Catches OBD2 signals being polled right
 * now, native preset CAN broadcasts producing live values, and internal
 * synthesised signals (e.g. CALCULATED_GEAR) all the same way. */
static void _live_indicator_refresh(lv_timer_t *t)
{
    picker_st_t *st = (picker_st_t *)t->user_data;
    if (!st || !st->sig_list || !lv_obj_is_valid(st->sig_list)) return;

    uint32_t n = lv_obj_get_child_cnt(st->sig_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(st->sig_list, i);
        if (!row || !lv_obj_is_valid(row)) continue;
        lv_obj_t *dot = (lv_obj_t *)lv_obj_get_user_data(row);
        if (!dot || !lv_obj_is_valid(dot)) continue;

        /* Label is always the first child of a row (made by make_col_row). */
        lv_obj_t *label = lv_obj_get_child(row, 0);
        if (!label) continue;
        const char *text = lv_label_get_text(label);
        if (!text) continue;

        char key[32];
        _label_to_sig_key(text, key, sizeof(key));
        if (!key[0]) continue;

        int16_t idx = signal_find_by_name(key);
        bool live = false;
        if (idx >= 0) {
            signal_t *sig = signal_get_by_index((uint16_t)idx);
            live = sig && !sig->is_stale && sig->last_update_ms > 0;
        }

        if (live) lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void populate_sig_col(picker_st_t *st)
{
    lv_obj_clean(st->sig_list);
    st->hi_sig = NULL;
    st->sel_sig = -1;
    update_picker_preview(st, -1);

    /* Collect matching unified indices (preconfig_items[] indices, or
     * encoded indices into the OBD2 synthetic table via
     * _obd2_idx_to_unified). _resolve_item dereferences either kind. */
    int idxs[256]; int nc = 0;
    for (int i = 0; i < preconfig_items_count - 1 && preconfig_items[i].ecu; i++) {
        if (strcmp(preconfig_items[i].ecu,     st->sel_brand) != 0) continue;
        if (strcmp(preconfig_items[i].version, st->sel_ver)   != 0) continue;
        if (nc < 256) idxs[nc++] = i;
    }
    /* OBD2 synthetic entries — include any whose ecu/version pair matches
     * the current picker selection. The brand can be "OBD2" (standard
     * Mode 01) or any category-derived brand like "Toyota". */
    _ensure_obd2_items_built();
    for (int oi = 0; oi < s_obd2_count && nc < 256; oi++) {
        const preconfig_item_t *it = &s_obd2_items[oi];
        if (!it->ecu || strcmp(it->ecu, st->sel_brand) != 0) continue;
        if (!it->version || strcmp(it->version, st->sel_ver) != 0) continue;
        idxs[nc++] = _obd2_idx_to_unified(oi);
    }
    /* Sort channels alphabetically by label (via _resolve_item). */
    for (int i = 0; i < nc - 1; i++)
        for (int j = i + 1; j < nc; j++) {
            const preconfig_item_t *a = _resolve_item(idxs[i]);
            const preconfig_item_t *b = _resolve_item(idxs[j]);
            if (a && b && strcmp(a->label, b->label) > 0) {
                int tmp = idxs[i]; idxs[i] = idxs[j]; idxs[j] = tmp;
            }
        }
    /* Create rows in sorted order, each with a hidden live-indicator dot
     * stashed via lv_obj_set_user_data so the live timer can reveal it
     * without re-iterating LVGL children. */
    for (int k = 0; k < nc; k++) {
        const preconfig_item_t *it = _resolve_item(idxs[k]);
        if (!it) continue;
        lv_obj_t *row = make_col_row(st->sig_list, it->label);
        col_sig_ctx_t *ctx = lv_mem_alloc(sizeof(col_sig_ctx_t));
        ctx->st = st; ctx->idx = idxs[k];
        lv_obj_add_event_cb(row, sig_click_cb,   LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, col_sig_free_cb, LV_EVENT_DELETE, ctx);

        /* Small accent-blue dot, right-aligned. Hidden by default; the
         * 500 ms refresh timer reveals it when the matching signal in
         * the registry has a fresh (non-stale) value. */
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 9, 9);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(row, dot);
    }

    /* (Re)start the live-indicator refresh timer for this column. */
    if (st->live_timer) {
        lv_timer_del(st->live_timer);
        st->live_timer = NULL;
    }
    st->live_timer = lv_timer_create(_live_indicator_refresh, 500, st);
    /* Prime immediately so dots appear without waiting for first tick. */
    _live_indicator_refresh(st->live_timer);
}

/* ── Embedded picker (for config modal PRESETS tab) ─────────────────────── */

static void _populate_brands(lv_obj_t *brand_list, picker_st_t *st)
{
    const char *brands[16]; int nb = 0;
    for (int i = 0; i < preconfig_items_count - 1 && preconfig_items[i].ecu; i++) {
        bool dup = false;
        for (int j = 0; j < nb; j++)
            if (strcmp(brands[j], preconfig_items[i].ecu) == 0) { dup = true; break; }
        if (!dup && nb < 16) brands[nb++] = preconfig_items[i].ecu;
    }
    /* Append any synthetic brands from the runtime OBD2 table — "OBD2"
     * for standard Mode 01 PIDs, "Toyota" for category="Toyota" PIDs,
     * and so on as future Mode 22 packs land. */
    _ensure_obd2_items_built();
    for (int oi = 0; oi < s_obd2_count && nb < 16; oi++) {
        const preconfig_item_t *it = &s_obd2_items[oi];
        if (!it->ecu) continue;
        bool dup = false;
        for (int j = 0; j < nb; j++)
            if (strcmp(brands[j], it->ecu) == 0) { dup = true; break; }
        if (!dup) brands[nb++] = it->ecu;
    }
    for (int i = 0; i < nb - 1; i++)
        for (int j = i + 1; j < nb; j++)
            if (strcmp(brands[i], brands[j]) > 0) {
                const char *tmp = brands[i]; brands[i] = brands[j]; brands[j] = tmp;
            }
    for (int i = 0; i < nb; i++) {
        lv_obj_t *row = make_col_row(brand_list, brands[i]);
        col_txt_ctx_t *ctx = lv_mem_alloc(sizeof(col_txt_ctx_t));
        ctx->st = st; ctx->name = brands[i];
        lv_obj_add_event_cb(row, brand_click_cb,  LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, col_txt_free_cb, LV_EVENT_DELETE, ctx);
    }

    _preselect_ecu_from_nvs(st, brand_list);
}

void build_preset_picker_embedded(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                   preset_apply_cb_t cb, void *ctx)
{
    picker_st_t *st = lv_mem_alloc(sizeof(picker_st_t));
    memset(st, 0, sizeof(*st));
    st->sel_sig      = -1;
    st->overlay      = NULL;
    st->apply_cb     = cb;
    st->apply_cb_ctx = ctx;

    lv_obj_add_event_cb(parent, picker_st_free_cb, LV_EVENT_DELETE, st);

    /* Configure parent as vertical flex, zero padding */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    lv_coord_t footer_h = 42;
    lv_coord_t body_h   = h - footer_h;
    lv_coord_t col_w    = w / 3;

    /* ── 3-column body (flex-row) ────────────────────────────────── */
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_set_size(body, w, body_h);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_column(body, 0, 0);

    lv_obj_t *brand_list = make_col(body, "BRAND",    true,  col_w, body_h);
    st->ver_list          = make_col(body, "PROTOCOL", true,  col_w, body_h);
    st->sig_list          = make_col(body, "CHANNEL",  false, col_w, body_h);

    /* ── Footer (preview + Apply) ────────────────────────────────── */
    lv_obj_t *footer = lv_obj_create(parent);
    lv_obj_set_size(footer, w, footer_h);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(footer, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(footer, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_pad_left(footer, 10, 0);
    lv_obj_set_style_pad_right(footer, 10, 0);
    lv_obj_set_style_pad_top(footer, 0, 0);
    lv_obj_set_style_pad_bottom(footer, 0, 0);

    lv_obj_t *prev_lbl = lv_label_create(footer);
    lv_label_set_text(prev_lbl, "Select a brand, protocol, then channel");
    lv_obj_set_style_text_color(prev_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(prev_lbl, THEME_FONT_SMALL, 0);
    lv_label_set_long_mode(prev_lbl, LV_LABEL_LONG_DOT);
    /* Apply button was removed (channel click auto-applies) — preview
     * label takes the full footer width and stays horizontally centered. */
    lv_obj_set_width(prev_lbl, w - 20);
    lv_obj_align(prev_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    st->preview_lbl = prev_lbl;
    st->apply_btn   = NULL;

    /* ── Populate brands ─────────────────────────────────────────── */
    _populate_brands(brand_list, st);
}

