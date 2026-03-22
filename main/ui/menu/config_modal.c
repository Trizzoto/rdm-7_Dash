/*
 * config_modal.c -- widget-based tabbed configuration modal
 *
 * Tabs (widget-type dependent):
 *   DATA         -- Label, signal info, collapsible CAN settings
 *   PRESETS      -- 3-column preset picker (Brand/Protocol/Channel)
 *   ALERTS       -- Thresholds + colours (panel / bar only)
 *   RPM SETTINGS -- Gauge max, redline, bar colour, limiter, background (rpm_bar only)
 *   FUEL SETUP   -- Fuel sender calibration (only when signal is FUEL_SENDER_V)
 *
 * All edits are written directly to the live signal_t and widget type_data.
 * Save/Cancel buttons in the footer delegate to menu_screen.c callbacks.
 */

#include "config_modal.h"
#include "../callbacks/ui_callbacks.h"
#include "../settings/settings_panel.h"
#include "../theme.h"
#include "menu_screen.h"
#include "widgets/signal.h"
#include "widgets/signal_internal.h"
#include "widgets/widget_panel.h"
#include "widgets/widget_bar.h"
#include "widgets/widget_rpm_bar.h"
#include "../settings/preset_picker.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Derive signal name from preset label ─────────────────────────────── */

/**
 * Convert a human-readable label like "Fuel Sender V" to a signal name
 * like "FUEL_SENDER_V".  Matches the web editor's convention so that
 * signal_internal.c can inject values by the expected name.
 */
static void label_to_signal_name(const char *label, char *out, size_t out_sz)
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
    /* Trim trailing underscore */
    if (j > 0 && out[j - 1] == '_') j--;
    out[j] = '\0';
}

/* ── Context passed to every callback via user_data ────────────────────── */

typedef struct {
    widget_t  *widget;
    int16_t    signal_index;   /* cached; updated if auto-created */
    /* DATA tab control references (for refresh after preset apply) */
    lv_obj_t  *label_ta;
    lv_obj_t  *signal_info_lbl;
    lv_obj_t  *can_id_ta;
    lv_obj_t  *endian_dd;
    lv_obj_t  *bit_start_dd;
    lv_obj_t  *bit_len_dd;
    lv_obj_t  *scale_ta;
    lv_obj_t  *offset_ta;
    lv_obj_t  *signed_dd;
} modal_ctx_t;

/* ── Layout constants ──────────────────────────────────────────────────── */

#define MODAL_W     778
#define MODAL_H     458
#define HDR_H        48
#define FOOTER_H     50
#define TABBAR_H     42

/* ── Bit-field option strings ──────────────────────────────────────────── */

static const char *BIT_START_OPTS =
    "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
    "16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
    "32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
    "48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63";

static const char *BIT_LEN_OPTS =
    "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n"
    "17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n"
    "33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n"
    "49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63\n64";

/* ── Helper: get or auto-create signal for a widget ────────────────────── */

static signal_t *ensure_signal(modal_ctx_t *ctx)
{
    if (ctx->signal_index >= 0) {
        return signal_get_by_index((uint16_t)ctx->signal_index);
    }

    /* No signal yet -- auto-create one named after the widget id */
    char name[32];
    snprintf(name, sizeof(name), "%s_sig", ctx->widget->id);

    int16_t idx = signal_register(name, 0, 0, 8, 1.0f, 0.0f, false, 1, "");
    if (idx < 0) return NULL;

    ctx->signal_index = idx;

    /* Write back into the widget's type_data */
    int16_t *idx_ptr = widget_get_signal_index_ptr(ctx->widget);
    if (idx_ptr) *idx_ptr = idx;

    char *nbuf = widget_get_signal_name_buf(ctx->widget);
    if (nbuf) {
        strncpy(nbuf, name, 31);
        nbuf[31] = '\0';
    }

    return signal_get_by_index((uint16_t)idx);
}

/* ── Style helpers ─────────────────────────────────────────────────────── */

static void style_tab(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tab, THEME_PAD_NORMAL, 0);
    lv_obj_set_style_pad_row(tab, 4, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
}

static void style_strip(lv_obj_t *obj, lv_coord_t w, lv_coord_t h)
{
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(obj, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
}

/* ── Context cleanup on modal delete ───────────────────────────────────── */

static void ctx_delete_cb(lv_event_t *e)
{
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (ctx) free(ctx);
}

/* =========================================================================
 * Signal tab callbacks
 * ========================================================================= */

static void can_id_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    lv_obj_t *ta = lv_event_get_target(e);
    const char *txt = lv_textarea_get_text(ta);
    if (!txt || txt[0] == '\0') {
        sig->can_id = 0;
        return;
    }

    char buf[32];
    if (txt[0] == '0' && (txt[1] == 'x' || txt[1] == 'X'))
        snprintf(buf, sizeof(buf), "%s", txt);
    else
        snprintf(buf, sizeof(buf), "0x%s", txt);

    sig->can_id = (uint32_t)strtoul(buf, NULL, 16);
}

static void endian_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    uint8_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    sig->endian = (sel == 0) ? 0 : 1;  /* 0=Motorola/big, 1=Intel/little */
}

static void bit_start_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    sig->bit_start = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
}

static void bit_length_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    sig->bit_length = (uint8_t)(lv_dropdown_get_selected(lv_event_get_target(e)) + 1);
}

static void scale_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    float val = (txt && txt[0]) ? (float)atof(txt) : 1.0f;
    if (val == 0.0f) val = 1.0f;
    sig->scale = val;
}

static void offset_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    sig->offset = (txt && txt[0]) ? (float)atof(txt) : 0.0f;
}

static void signed_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    uint8_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    sig->is_signed = (sel == 1);
}

/* =========================================================================
 * Label changed callback
 * ========================================================================= */

static void label_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    char *lbl_buf = widget_get_label_buf(ctx->widget);
    if (!lbl_buf) return;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    size_t max_len = (ctx->widget->type == WIDGET_PANEL) ? 63 : 31;
    if (txt) {
        strncpy(lbl_buf, txt, max_len);
        lbl_buf[max_len] = '\0';
    }
}

/* =========================================================================
 * CAN settings toggle (click to expand/collapse)
 * ========================================================================= */

static void can_toggle_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *container = (lv_obj_t *)lv_event_get_user_data(e);
    if (!container) return;
    if (lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

/* =========================================================================
 * Preset applied callback (invoked by embedded preset picker)
 * ========================================================================= */

static void preset_applied_cb(const preconfig_item_t *item, void *user_data)
{
    modal_ctx_t *ctx = (modal_ctx_t *)user_data;
    if (!ctx || !item) return;

    signal_t *sig = ensure_signal(ctx);
    if (!sig) return;

    /* Derive proper signal name from the preset label so that
     * signal_internal.c injections (by name) find the right entry. */
    char new_name[32];
    label_to_signal_name(item->label, new_name, sizeof(new_name));

    /* If a different signal with this name already exists, switch to it
     * instead of renaming the current one (avoids duplicate names). */
    int16_t existing = signal_find_by_name(new_name);
    if (existing >= 0 && existing != ctx->signal_index) {
        sig = signal_get_by_index((uint16_t)existing);
        ctx->signal_index = existing;
        int16_t *idx_ptr = widget_get_signal_index_ptr(ctx->widget);
        if (idx_ptr) *idx_ptr = existing;
    } else if (new_name[0]) {
        /* Rename the current signal entry */
        strncpy(sig->name, new_name, sizeof(sig->name) - 1);
        sig->name[sizeof(sig->name) - 1] = '\0';
    }

    /* Update signal CAN fields */
    sig->can_id     = (uint32_t)strtol(item->can_id, NULL, 16);
    sig->endian     = item->endianess;
    sig->bit_start  = item->bit_start;
    sig->bit_length = item->bit_length;
    sig->scale      = item->scale;
    sig->offset     = item->value_offset;
    sig->is_signed  = item->is_signed;

    /* Update signal name in widget type_data to match the (possibly renamed) signal */
    char *sig_buf = widget_get_signal_name_buf(ctx->widget);
    if (sig_buf) {
        strncpy(sig_buf, sig->name, 31);
        sig_buf[31] = '\0';
    }

    /* Update widget label */
    char *lbl_buf = widget_get_label_buf(ctx->widget);
    if (lbl_buf) {
        size_t max = (ctx->widget->type == WIDGET_PANEL) ? 63 : 31;
        strncpy(lbl_buf, item->label, max);
        lbl_buf[max] = '\0';
    }

    /* Refresh DATA tab controls */
    if (ctx->label_ta) lv_textarea_set_text(ctx->label_ta, item->label);
    if (ctx->signal_info_lbl) lv_label_set_text(ctx->signal_info_lbl, item->label);
    if (ctx->can_id_ta) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lX", (unsigned long)sig->can_id);
        lv_textarea_set_text(ctx->can_id_ta, buf);
    }
    if (ctx->endian_dd)
        lv_dropdown_set_selected(ctx->endian_dd, sig->endian ? 1 : 0);
    if (ctx->bit_start_dd)
        lv_dropdown_set_selected(ctx->bit_start_dd, sig->bit_start);
    if (ctx->bit_len_dd)
        lv_dropdown_set_selected(ctx->bit_len_dd, sig->bit_length - 1);
    if (ctx->scale_ta) {
        char buf[16]; snprintf(buf, sizeof(buf), "%.6g", sig->scale);
        lv_textarea_set_text(ctx->scale_ta, buf);
    }
    if (ctx->offset_ta) {
        char buf[16]; snprintf(buf, sizeof(buf), "%.6g", sig->offset);
        lv_textarea_set_text(ctx->offset_ta, buf);
    }
    if (ctx->signed_dd)
        lv_dropdown_set_selected(ctx->signed_dd, sig->is_signed ? 1 : 0);
}

/* =========================================================================
 * Data tab builder (Label + Signal info + collapsible CAN settings)
 * ========================================================================= */

static void build_data_tab(lv_obj_t *tab, modal_ctx_t *ctx)
{
    widget_t *w = ctx->widget;
    signal_t *sig = (ctx->signal_index >= 0)
                        ? signal_get_by_index((uint16_t)ctx->signal_index)
                        : NULL;

    /* ── Label section (only for widgets that have a label) ──────── */
    char *lbl_buf = widget_get_label_buf(w);
    if (lbl_buf) {
        settings_section_t *sec_disp =
            settings_add_section(tab, "DISPLAY", THEME_COLOR_ACCENT_TEAL);

        ctx->label_ta = settings_add_text_input(sec_disp, "Label:",
                                                 "widget label", lbl_buf);
        lv_obj_add_event_cb(ctx->label_ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(ctx->label_ta, label_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);
    }

    /* ── Data Source section (only for widgets that receive CAN data) ── */
    if (widget_needs_data_source(w)) {
        settings_section_t *sec_sig =
            settings_add_section(tab, "DATA SOURCE", THEME_COLOR_ACCENT_BLUE);

        char sig_info[64] = "No signal assigned";
        char *sig_name = widget_get_signal_name_buf(w);
        if (sig_name && sig_name[0])
            snprintf(sig_info, sizeof(sig_info), "%s", sig_name);
        ctx->signal_info_lbl = settings_add_info_row(sec_sig, "Signal:", sig_info);

        /* ── Collapsible CAN settings toggle button ─────────────────── */
        lv_obj_t *can_toggle = settings_add_button(
            sec_sig, LV_SYMBOL_DOWN "  CAN Bus Settings",
            THEME_COLOR_SURFACE, 0);

        /* Container for CAN fields, hidden by default */
        lv_obj_t *can_box = lv_obj_create(tab);
        lv_obj_set_size(can_box, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(can_box, THEME_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(can_box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(can_box, THEME_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(can_box, 1, 0);
        lv_obj_set_style_radius(can_box, THEME_RADIUS_NORMAL, 0);
        lv_obj_set_style_pad_all(can_box, 8, 0);
        lv_obj_set_style_pad_row(can_box, 4, 0);
        lv_obj_clear_flag(can_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(can_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(can_box, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_event_cb(can_toggle, can_toggle_btn_cb,
                            LV_EVENT_CLICKED, can_box);

        /* CAN ID */
        char can_id_str[16] = "0";
        if (sig) snprintf(can_id_str, sizeof(can_id_str), "%X", sig->can_id);
        ctx->can_id_ta = settings_add_text_input(can_box, "CAN ID (0x):",
                                                  "hex", can_id_str);
        lv_obj_add_event_cb(ctx->can_id_ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(ctx->can_id_ta, can_id_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);

        /* Endian */
        ctx->endian_dd = settings_add_dropdown(can_box, "Endian:",
                                                "Big Endian\nLittle Endian", 0);
        lv_dropdown_set_selected(ctx->endian_dd,
                                 (sig && sig->endian == 1) ? 1 : 0);
        lv_obj_add_event_cb(ctx->endian_dd, endian_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);

        /* Bit Start */
        ctx->bit_start_dd = settings_add_dropdown(can_box, "Bit Start:",
                                                   BIT_START_OPTS, 0);
        lv_dropdown_set_selected(ctx->bit_start_dd, sig ? sig->bit_start : 0);
        lv_obj_add_event_cb(ctx->bit_start_dd, bit_start_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);

        /* Bit Length */
        ctx->bit_len_dd = settings_add_dropdown(can_box, "Bit Length:",
                                                 BIT_LEN_OPTS, 0);
        lv_dropdown_set_selected(ctx->bit_len_dd,
                                 sig ? (sig->bit_length - 1) : 7);
        lv_obj_add_event_cb(ctx->bit_len_dd, bit_length_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);

        /* Scale */
        char scale_str[16] = "1";
        if (sig) snprintf(scale_str, sizeof(scale_str), "%.6g", sig->scale);
        ctx->scale_ta = settings_add_text_input(can_box, "Scale:",
                                                 "factor", scale_str);
        lv_obj_add_event_cb(ctx->scale_ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(ctx->scale_ta, scale_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);

        /* Offset */
        char offset_str[16] = "0";
        if (sig) snprintf(offset_str, sizeof(offset_str), "%.6g", sig->offset);
        ctx->offset_ta = settings_add_text_input(can_box, "Offset:",
                                                  "value offset", offset_str);
        lv_obj_add_event_cb(ctx->offset_ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(ctx->offset_ta, offset_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);

        /* Data Type (signed/unsigned) */
        ctx->signed_dd = settings_add_dropdown(can_box, "Data Type:",
                                                "Unsigned\nSigned", 0);
        lv_dropdown_set_selected(ctx->signed_dd,
                                 (sig && sig->is_signed) ? 1 : 0);
        lv_obj_add_event_cb(ctx->signed_dd, signed_changed_cb,
                            LV_EVENT_VALUE_CHANGED, ctx);
    }
}

/* =========================================================================
 * Alerts tab callbacks -- Panel
 * ========================================================================= */

static void panel_warn_high_thresh_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)ctx->widget->type_data;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    pd->warning_high_threshold = (txt && txt[0]) ? (float)atof(txt) : 0.0f;
}

static void panel_warn_low_thresh_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)ctx->widget->type_data;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    pd->warning_low_threshold = (txt && txt[0]) ? (float)atof(txt) : 0.0f;
}

static void panel_warn_high_color_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)ctx->widget->type_data;
    uint8_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    pd->warning_high_color = (sel == 0) ? THEME_COLOR_RED : THEME_COLOR_BLUE_PURE;
}

static void panel_warn_low_color_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)ctx->widget->type_data;
    uint8_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    pd->warning_low_color = (sel == 0) ? THEME_COLOR_RED : THEME_COLOR_BLUE_PURE;
}

static void panel_warn_high_enable_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)ctx->widget->type_data;
    pd->warning_high_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void panel_warn_low_enable_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_PANEL) return;
    panel_data_t *pd = (panel_data_t *)ctx->widget->type_data;
    pd->warning_low_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

/* =========================================================================
 * Alerts tab callbacks -- Bar
 * ========================================================================= */

static void bar_low_thresh_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_BAR) return;
    bar_data_t *bd = (bar_data_t *)ctx->widget->type_data;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    bd->bar_low = (txt && txt[0]) ? (int32_t)atoi(txt) : 0;
}

static void bar_high_thresh_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_BAR) return;
    bar_data_t *bd = (bar_data_t *)ctx->widget->type_data;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    bd->bar_high = (txt && txt[0]) ? (int32_t)atoi(txt) : 0;
}

static const char *BAR_COLOR_OPTS =
    "Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom";

static lv_color_t bar_color_from_idx(uint8_t idx)
{
    lv_color_t p[] = {THEME_COLOR_BLUE_DARK,    THEME_COLOR_RED,
                      THEME_COLOR_GREEN_BRIGHT,  THEME_COLOR_YELLOW,
                      THEME_COLOR_ORANGE,        THEME_COLOR_PURPLE,
                      THEME_COLOR_CYAN,          THEME_COLOR_MAGENTA};
    return (idx < 8) ? p[idx] : THEME_COLOR_BLUE_DARK;
}

static uint8_t bar_color_idx(lv_color_t c)
{
    lv_color_t p[] = {THEME_COLOR_BLUE_DARK,    THEME_COLOR_RED,
                      THEME_COLOR_GREEN_BRIGHT,  THEME_COLOR_YELLOW,
                      THEME_COLOR_ORANGE,        THEME_COLOR_PURPLE,
                      THEME_COLOR_CYAN,          THEME_COLOR_MAGENTA};
    for (int i = 0; i < 8; i++)
        if (c.full == p[i].full) return i;
    return 8;
}

static void bar_low_color_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_BAR) return;
    bar_data_t *bd = (bar_data_t *)ctx->widget->type_data;
    bd->bar_low_color = bar_color_from_idx(lv_dropdown_get_selected(lv_event_get_target(e)));
}

static void bar_high_color_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    modal_ctx_t *ctx = (modal_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || ctx->widget->type != WIDGET_BAR) return;
    bar_data_t *bd = (bar_data_t *)ctx->widget->type_data;
    bd->bar_high_color = bar_color_from_idx(lv_dropdown_get_selected(lv_event_get_target(e)));
}

/* =========================================================================
 * Warnings toggle callback (show/hide sub-container)
 * ========================================================================= */

static void warnings_toggle_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *container = (lv_obj_t *)lv_event_get_user_data(e);
    if (!container) return;
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED))
        lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

/* =========================================================================
 * Alerts tab builder -- Panel
 * ========================================================================= */

static void build_alerts_tab_panel(lv_obj_t *tab, modal_ctx_t *ctx)
{
    panel_data_t *pd = (panel_data_t *)ctx->widget->type_data;

    bool has_warnings = pd->warning_high_enabled || pd->warning_low_enabled;

    settings_section_t *sec =
        settings_add_section(tab, "ALERT SETTINGS", THEME_COLOR_ACCENT_AMBER);

    lv_obj_t *en_sw = settings_add_switch(sec, "Enable Alerts:", has_warnings);

    /* Sub-container hidden when alerts are off */
    lv_obj_t *warn_box = lv_obj_create(tab);
    lv_obj_set_size(warn_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(warn_box, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(warn_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(warn_box, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(warn_box, 1, 0);
    lv_obj_set_style_radius(warn_box, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_pad_all(warn_box, 6, 0);
    lv_obj_set_style_pad_row(warn_box, 4, 0);
    lv_obj_clear_flag(warn_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(warn_box, LV_FLEX_FLOW_COLUMN);
    if (!has_warnings)
        lv_obj_add_flag(warn_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(en_sw, warnings_toggle_cb, LV_EVENT_VALUE_CHANGED, warn_box);

    /* High warning */
    lv_obj_t *he_sw = settings_add_switch(warn_box, "High Enabled:", pd->warning_high_enabled);
    lv_obj_add_event_cb(he_sw, panel_warn_high_enable_cb, LV_EVENT_VALUE_CHANGED, ctx);

    char buf[20];
    snprintf(buf, sizeof(buf), "%.2f", pd->warning_high_threshold);
    lv_obj_t *ht = settings_add_text_input(warn_box, "High Threshold:", "value", buf);
    lv_obj_add_event_cb(ht, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ht, panel_warn_high_thresh_cb, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t *hc = settings_add_dropdown(warn_box, "High Colour:", "Red\nBlue", 0);
    lv_dropdown_set_selected(hc, pd->warning_high_color.full == THEME_COLOR_RED.full ? 0 : 1);
    lv_obj_add_event_cb(hc, panel_warn_high_color_cb, LV_EVENT_VALUE_CHANGED, ctx);

    /* Low warning */
    lv_obj_t *le_sw = settings_add_switch(warn_box, "Low Enabled:", pd->warning_low_enabled);
    lv_obj_add_event_cb(le_sw, panel_warn_low_enable_cb, LV_EVENT_VALUE_CHANGED, ctx);

    snprintf(buf, sizeof(buf), "%.2f", pd->warning_low_threshold);
    lv_obj_t *lt = settings_add_text_input(warn_box, "Low Threshold:", "value", buf);
    lv_obj_add_event_cb(lt, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(lt, panel_warn_low_thresh_cb, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t *lc = settings_add_dropdown(warn_box, "Low Colour:", "Red\nBlue", 0);
    lv_dropdown_set_selected(lc, pd->warning_low_color.full == THEME_COLOR_RED.full ? 0 : 1);
    lv_obj_add_event_cb(lc, panel_warn_low_color_cb, LV_EVENT_VALUE_CHANGED, ctx);
}

/* =========================================================================
 * Alerts tab builder -- Bar
 * ========================================================================= */

static void build_alerts_tab_bar(lv_obj_t *tab, modal_ctx_t *ctx)
{
    bar_data_t *bd = (bar_data_t *)ctx->widget->type_data;

    bool has_warnings = (bd->bar_low != 0 || bd->bar_high != 0);

    settings_section_t *sec =
        settings_add_section(tab, "BAR THRESHOLDS", THEME_COLOR_ACCENT_AMBER);

    lv_obj_t *en_sw = settings_add_switch(sec, "Enable Alerts:", has_warnings);

    lv_obj_t *warn_box = lv_obj_create(tab);
    lv_obj_set_size(warn_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(warn_box, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(warn_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(warn_box, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(warn_box, 1, 0);
    lv_obj_set_style_radius(warn_box, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_pad_all(warn_box, 6, 0);
    lv_obj_set_style_pad_row(warn_box, 4, 0);
    lv_obj_clear_flag(warn_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(warn_box, LV_FLEX_FLOW_COLUMN);
    if (!has_warnings)
        lv_obj_add_flag(warn_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(en_sw, warnings_toggle_cb, LV_EVENT_VALUE_CHANGED, warn_box);

    char buf[16];

    snprintf(buf, sizeof(buf), "%d", (int)bd->bar_low);
    lv_obj_t *blow = settings_add_text_input(warn_box, "Low Threshold:", "value", buf);
    lv_obj_add_event_cb(blow, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(blow, bar_low_thresh_cb, LV_EVENT_VALUE_CHANGED, ctx);

    snprintf(buf, sizeof(buf), "%d", (int)bd->bar_high);
    lv_obj_t *bhigh = settings_add_text_input(warn_box, "High Threshold:", "value", buf);
    lv_obj_add_event_cb(bhigh, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(bhigh, bar_high_thresh_cb, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t *blc = settings_add_dropdown(warn_box, "Low Colour:", BAR_COLOR_OPTS, 0);
    lv_dropdown_set_selected(blc, bar_color_idx(bd->bar_low_color));
    lv_obj_add_event_cb(blc, bar_low_color_cb, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t *bhc = settings_add_dropdown(warn_box, "High Colour:", BAR_COLOR_OPTS, 0);
    lv_dropdown_set_selected(bhc, bar_color_idx(bd->bar_high_color));
    lv_obj_add_event_cb(bhc, bar_high_color_cb, LV_EVENT_VALUE_CHANGED, ctx);
}

/* =========================================================================
 * RPM Settings tab builder
 * ========================================================================= */

/* RPM roller options: 3000-12000 in 200-RPM steps */
static const char *RPM_STEP_OPTS =
    "3000\n3200\n3400\n3600\n3800\n4000\n4200\n4400\n4600\n4800\n"
    "5000\n5200\n5400\n5600\n5800\n6000\n6200\n6400\n6600\n6800\n"
    "7000\n7200\n7400\n7600\n7800\n8000\n8200\n8400\n8600\n8800\n"
    "9000\n9200\n9400\n9600\n9800\n10000\n10200\n10400\n10600\n10800\n"
    "11000\n11200\n11400\n11600\n11800\n12000";

static const char *COLOR_OPTS =
    "Green\nCyan\nYellow\nOrange\nRed\nBlue\nPurple\nMagenta\nPink\nCustom...";

static const char *LIMITER_EFFECT_OPTS =
    "None\nBar Flash\nBar+Circles Flash\nCircles Flash\n"
    "Bar Solid\nBar+Circles Solid\nCircles Solid";

/** Map an RPM value (3000-12000, step 200) to a dropdown index. */
static uint16_t _rpm_to_idx(int32_t rpm) {
    int idx = (rpm - 3000) / 200;
    if (idx < 0) idx = 0;
    if (idx > 45) idx = 45;
    return (uint16_t)idx;
}

/** Map a theme colour to the 10-entry colour dropdown index (9 = Custom). */
static uint16_t _theme_color_idx(lv_color_t c) {
    if (c.full == THEME_COLOR_GREEN.full)   return 0;
    if (c.full == THEME_COLOR_CYAN.full)    return 1;
    if (c.full == THEME_COLOR_YELLOW.full)  return 2;
    if (c.full == THEME_COLOR_ORANGE.full)  return 3;
    if (c.full == THEME_COLOR_RED.full)     return 4;
    if (c.full == THEME_COLOR_BLUE.full)    return 5;
    if (c.full == THEME_COLOR_PURPLE.full)  return 6;
    if (c.full == THEME_COLOR_MAGENTA.full) return 7;
    if (c.full == THEME_COLOR_PINK.full)    return 8;
    return 9; /* Custom */
}

/** Map limiter_effect enum to dropdown index. */
static uint16_t _limiter_effect_to_idx(uint8_t effect) {
    switch (effect) {
    case 0: return 0; /* None */
    case 2: return 1; /* Bar Flash */
    case 3: return 2; /* Bar+Circles Flash */
    case 4: return 3; /* Circles Flash */
    case 5: return 4; /* Bar Solid */
    case 6: return 5; /* Bar+Circles Solid */
    case 7: return 6; /* Circles Solid */
    default: return 0;
    }
}

static void build_rpm_settings_tab(lv_obj_t *tab, modal_ctx_t *ctx)
{
    rpm_bar_data_t *rd = (rpm_bar_data_t *)ctx->widget->type_data;
    if (!rd) return;

    /* ── Gauge section ─────────────────────────────────────────────── */
    settings_section_t *gauge_sec =
        settings_add_section(tab, "GAUGE", THEME_COLOR_ACCENT_BLUE);

    lv_obj_t *max_dd = settings_add_dropdown(gauge_sec, "Max RPM:",
                                              RPM_STEP_OPTS, 0);
    lv_dropdown_set_selected(max_dd, _rpm_to_idx(rd->gauge_max));
    lv_obj_add_event_cb(max_dd, rpm_gauge_roller_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *rl_dd = settings_add_dropdown(gauge_sec, "Redline:",
                                             RPM_STEP_OPTS, 0);
    lv_dropdown_set_selected(rl_dd, _rpm_to_idx(rd->redline));
    lv_obj_add_event_cb(rl_dd, rpm_redline_roller_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *col_dd = settings_add_dropdown(gauge_sec, "Bar Colour:",
                                              COLOR_OPTS, 0);
    lv_dropdown_set_selected(col_dd, _theme_color_idx(rd->bar_color));
    lv_obj_add_event_cb(col_dd, rpm_color_dropdown_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Limiter section ───────────────────────────────────────────── */
    settings_section_t *lim_sec =
        settings_add_section(tab, "LIMITER", THEME_COLOR_ACCENT_AMBER);

    lv_obj_t *eff_dd = settings_add_dropdown(lim_sec, "Effect:",
                                              LIMITER_EFFECT_OPTS, 0);
    lv_dropdown_set_selected(eff_dd, _limiter_effect_to_idx(rd->limiter_effect));
    lv_obj_add_event_cb(eff_dd, rpm_limiter_effect_dropdown_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lval_dd = settings_add_dropdown(lim_sec, "Trigger RPM:",
                                               RPM_STEP_OPTS, 0);
    lv_dropdown_set_selected(lval_dd, _rpm_to_idx(rd->limiter_value));
    lv_obj_add_event_cb(lval_dd, rpm_limiter_roller_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lcol_dd = settings_add_dropdown(lim_sec, "Limiter Colour:",
                                               COLOR_OPTS, 0);
    lv_dropdown_set_selected(lcol_dd, _theme_color_idx(rd->limiter_color));
    lv_obj_add_event_cb(lcol_dd, rpm_limiter_color_dropdown_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Background section ────────────────────────────────────────── */
    settings_section_t *bg_sec =
        settings_add_section(tab, "BACKGROUND", THEME_COLOR_ACCENT_TEAL);

    lv_obj_t *bg_sw = settings_add_switch(bg_sec, "Enable:",
                                           rd->background_enabled);
    lv_obj_add_event_cb(bg_sw, rpm_background_switch_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bg_val_dd = settings_add_dropdown(bg_sec, "Threshold RPM:",
                                                 RPM_STEP_OPTS, 0);
    lv_dropdown_set_selected(bg_val_dd, _rpm_to_idx(rd->background_value));
    lv_obj_add_event_cb(bg_val_dd, rpm_background_threshold_roller_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bg_col_dd = settings_add_dropdown(bg_sec, "Background Colour:",
                                                 COLOR_OPTS, 0);
    lv_dropdown_set_selected(bg_col_dd, _theme_color_idx(rd->background_color));
    lv_obj_add_event_cb(bg_col_dd, rpm_background_color_dropdown_event_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);
}

/* =========================================================================
 * Fuel Setup tab (shown only when signal is FUEL_SENDER_V)
 * ========================================================================= */

typedef struct {
    lv_timer_t *timer;
    lv_obj_t   *voltage_lbl;
    lv_obj_t   *empty_ta;
    lv_obj_t   *full_ta;
} fuel_tab_ctx_t;

static void fuel_timer_cb(lv_timer_t *t)
{
    fuel_tab_ctx_t *fc = (fuel_tab_ctx_t *)t->user_data;
    if (!fc || !fc->voltage_lbl) return;
    float v = signal_internal_get_fuel_voltage();
    lv_label_set_text_fmt(fc->voltage_lbl, "%.3f V", v);
}

static void fuel_tab_delete_cb(lv_event_t *e)
{
    fuel_tab_ctx_t *fc = (fuel_tab_ctx_t *)lv_event_get_user_data(e);
    if (!fc) return;
    if (fc->timer) lv_timer_del(fc->timer);
    free(fc);
}

static void fuel_empty_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    fuel_cal_config_t cal;
    signal_internal_get_fuel_cal(&cal);
    cal.empty_v = (txt && txt[0]) ? (float)atof(txt) : 0.0f;
    signal_internal_set_fuel_cal(cal.empty_v, cal.full_v, cal.full_value, cal.enabled);
}

static void fuel_full_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    fuel_cal_config_t cal;
    signal_internal_get_fuel_cal(&cal);
    cal.full_v = (txt && txt[0]) ? (float)atof(txt) : 0.0f;
    signal_internal_set_fuel_cal(cal.empty_v, cal.full_v, cal.full_value, cal.enabled);
}

static void fuel_fullval_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    fuel_cal_config_t cal;
    signal_internal_get_fuel_cal(&cal);
    cal.full_value = (txt && txt[0]) ? (float)atof(txt) : 100.0f;
    signal_internal_set_fuel_cal(cal.empty_v, cal.full_v, cal.full_value, cal.enabled);
}

static void fuel_enable_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    fuel_cal_config_t cal;
    signal_internal_get_fuel_cal(&cal);
    cal.enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    signal_internal_set_fuel_cal(cal.empty_v, cal.full_v, cal.full_value, cal.enabled);
}

static void fuel_set_empty_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    fuel_tab_ctx_t *fc = (fuel_tab_ctx_t *)lv_event_get_user_data(e);
    float v = signal_internal_get_fuel_voltage();

    fuel_cal_config_t cal;
    signal_internal_get_fuel_cal(&cal);
    cal.empty_v = v;
    signal_internal_set_fuel_cal(cal.empty_v, cal.full_v, cal.full_value, cal.enabled);

    if (fc && fc->empty_ta) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.3f", v);
        lv_textarea_set_text(fc->empty_ta, buf);
    }
}

static void fuel_set_full_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    fuel_tab_ctx_t *fc = (fuel_tab_ctx_t *)lv_event_get_user_data(e);
    float v = signal_internal_get_fuel_voltage();

    fuel_cal_config_t cal;
    signal_internal_get_fuel_cal(&cal);
    cal.full_v = v;
    signal_internal_set_fuel_cal(cal.empty_v, cal.full_v, cal.full_value, cal.enabled);

    if (fc && fc->full_ta) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.3f", v);
        lv_textarea_set_text(fc->full_ta, buf);
    }
}

static void build_fuel_setup_tab(lv_obj_t *tab, modal_ctx_t *ctx)
{
    (void)ctx;

    fuel_tab_ctx_t *fc = calloc(1, sizeof(fuel_tab_ctx_t));
    if (!fc) return;

    /* Clean up timer + struct when tab is deleted */
    lv_obj_add_event_cb(tab, fuel_tab_delete_cb, LV_EVENT_DELETE, fc);

    fuel_cal_config_t cal;
    signal_internal_get_fuel_cal(&cal);

    /* ── Live voltage section ─────────────────────────────────────── */
    settings_section_t *sec_live =
        settings_add_section(tab, "LIVE READING", THEME_COLOR_ACCENT_TEAL);

    float cur_v = signal_internal_get_fuel_voltage();
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%.3f V", cur_v);
    fc->voltage_lbl = settings_add_info_row(sec_live, "Voltage:", vbuf);

    /* Start 200 ms timer for live voltage updates */
    fc->timer = lv_timer_create(fuel_timer_cb, 200, fc);

    /* ── Calibration section ──────────────────────────────────────── */
    settings_section_t *sec_cal =
        settings_add_section(tab, "CALIBRATION", THEME_COLOR_ACCENT_AMBER);

    /* Empty voltage */
    char ebuf[16];
    snprintf(ebuf, sizeof(ebuf), "%.3f", cal.empty_v);
    fc->empty_ta = settings_add_text_input(sec_cal, "Empty Voltage:", "volts", ebuf);
    lv_obj_add_event_cb(fc->empty_ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(fc->empty_ta, fuel_empty_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *set_empty_btn = settings_add_button(sec_cal, "SET EMPTY (capture current)",
                                                    THEME_COLOR_ACCENT_DIM, 0);
    lv_obj_add_event_cb(set_empty_btn, fuel_set_empty_cb, LV_EVENT_CLICKED, fc);

    /* Full voltage */
    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%.3f", cal.full_v);
    fc->full_ta = settings_add_text_input(sec_cal, "Full Voltage:", "volts", fbuf);
    lv_obj_add_event_cb(fc->full_ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(fc->full_ta, fuel_full_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *set_full_btn = settings_add_button(sec_cal, "SET FULL (capture current)",
                                                   THEME_COLOR_ACCENT_DIM, 0);
    lv_obj_add_event_cb(set_full_btn, fuel_set_full_cb, LV_EVENT_CLICKED, fc);

    /* Full value (what 100% maps to) */
    char fvbuf[16];
    snprintf(fvbuf, sizeof(fvbuf), "%.1f", cal.full_value);
    lv_obj_t *fv_ta = settings_add_text_input(sec_cal, "Full Value:", "100 = %, or litres", fvbuf);
    lv_obj_add_event_cb(fv_ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(fv_ta, fuel_fullval_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Enable toggle */
    lv_obj_t *en_sw = settings_add_switch(sec_cal, "Enable Calibration:", cal.enabled);
    lv_obj_add_event_cb(en_sw, fuel_enable_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* =========================================================================
 * Public: config_modal_open_for_widget
 * ========================================================================= */

void config_modal_open_for_widget(lv_obj_t *screen, widget_t *w)
{
    if (!screen || !w) return;

    bool has_alerts = widget_has_alert_support(w);

    /* ── Allocate context ──────────────────────────────────────────────── */
    modal_ctx_t *ctx = calloc(1, sizeof(modal_ctx_t));
    if (!ctx) return;
    ctx->widget = w;

    int16_t *idx_ptr = widget_get_signal_index_ptr(w);
    ctx->signal_index = idx_ptr ? *idx_ptr : -1;

    /* ── Modal outer shell ─────────────────────────────────────────────── */
    lv_obj_t *modal = lv_obj_create(screen);
    lv_obj_set_size(modal, MODAL_W, MODAL_H);
    lv_obj_center(modal);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(modal, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(modal, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_color(modal, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(modal, 1, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_set_style_pad_row(modal, 0, 0);
    lv_obj_set_style_shadow_width(modal, 20, 0);
    lv_obj_set_style_shadow_ofs_y(modal, 4, 0);
    lv_obj_set_style_shadow_color(modal, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(modal, 140, 0);

    /* Free ctx when modal is deleted */
    lv_obj_add_event_cb(modal, ctx_delete_cb, LV_EVENT_DELETE, ctx);

    /* ── Header ────────────────────────────────────────────────────────── */
    lv_obj_t *hdr = lv_obj_create(modal);
    style_strip(hdr, MODAL_W, HDR_H);
    lv_obj_set_style_bg_color(hdr, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(hdr, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_pad_left(hdr, 14, 0);
    lv_obj_set_style_pad_right(hdr, 14, 0);

    /* Title: "PANEL_0 · panel" — ID bright, type muted */
    char id_upper[24];
    snprintf(id_upper, sizeof(id_upper), "%s", w->id);
    for (char *p = id_upper; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    }

    lv_obj_t *title_lbl = lv_label_create(hdr);
    lv_label_set_text(title_lbl, id_upper);
    lv_obj_set_style_text_color(title_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title_lbl, THEME_FONT_MEDIUM, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    /* Type suffix in muted text */
    char type_suffix[40];
    snprintf(type_suffix, sizeof(type_suffix), "  %s", widget_type_name(w->type));
    lv_obj_t *type_lbl = lv_label_create(hdr);
    lv_label_set_text(type_lbl, type_suffix);
    lv_obj_set_style_text_color(type_lbl, THEME_COLOR_TEXT_HINT, 0);
    lv_obj_set_style_text_font(type_lbl, THEME_FONT_SMALL, 0);
    lv_obj_align_to(type_lbl, title_lbl, LV_ALIGN_OUT_RIGHT_MID, 0, 1);

    /* Close (X) button — neutral secondary */
    lv_obj_t *close_btn = lv_btn_create(hdr);
    lv_obj_set_size(close_btn, 32, 28);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *x_lbl = lv_label_create(close_btn);
    lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(x_lbl, THEME_FONT_SMALL, 0);
    lv_obj_center(x_lbl);
    lv_obj_add_event_cb(close_btn, cancel_menu_event_cb, LV_EVENT_CLICKED, NULL);

    /* ── Tabview ───────────────────────────────────────────────────────── */
    lv_coord_t tab_total_h = MODAL_H - HDR_H - FOOTER_H;
    lv_obj_t *tv = lv_tabview_create(modal, LV_DIR_TOP, TABBAR_H);
    lv_obj_set_size(tv, MODAL_W, tab_total_h);
    lv_obj_set_style_bg_color(tv, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);

    /* Tab button styling — underline-only active, matching web UI */
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(tab_btns, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(tab_btns, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab_btns, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(tab_btns, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_letter_space(tab_btns, 1, 0);
    lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tab_btns, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(tab_btns, 1, 0);

    /* Inactive items: no bg fill, no border */
    lv_obj_set_style_bg_opa(tab_btns, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(tab_btns, 0, LV_PART_ITEMS);

    /* Active tab: bright text + accent underline only (no bg fill) */
    lv_obj_set_style_bg_opa(tab_btns, LV_OPA_TRANSP,
                            LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tab_btns, THEME_COLOR_TEXT_PRIMARY,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM,
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tab_btns, THEME_COLOR_ACCENT_BLUE,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tab_btns, 2,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);

    /* Create tabs */
    lv_obj_t *tab_data = lv_tabview_add_tab(tv, "  DATA  ");
    style_tab(tab_data);
    build_data_tab(tab_data, ctx);

    /* Presets tab (only for widgets that receive CAN data) */
    if (widget_needs_data_source(w) && widget_get_signal_name_buf(w)) {
        lv_obj_t *tab_presets = lv_tabview_add_tab(tv, "  PRESETS  ");
        lv_obj_set_style_bg_color(tab_presets, THEME_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(tab_presets, LV_OPA_COVER, 0);
        build_preset_picker_embedded(tab_presets, MODAL_W,
                                      tab_total_h - TABBAR_H,
                                      preset_applied_cb, ctx);
    }

    if (has_alerts) {
        lv_obj_t *tab_alerts = lv_tabview_add_tab(tv, "  ALERTS  ");
        style_tab(tab_alerts);

        if (w->type == WIDGET_PANEL)
            build_alerts_tab_panel(tab_alerts, ctx);
        else if (w->type == WIDGET_BAR)
            build_alerts_tab_bar(tab_alerts, ctx);
    }

    if (w->type == WIDGET_RPM_BAR) {
        lv_obj_t *tab_rpm = lv_tabview_add_tab(tv, "  RPM SETTINGS  ");
        style_tab(tab_rpm);
        build_rpm_settings_tab(tab_rpm, ctx);
    }

    /* Fuel setup tab — only when widget's signal is FUEL_SENDER_V */
    char *sig_name = widget_get_signal_name_buf(w);
    if (sig_name && strcmp(sig_name, "FUEL_SENDER_V") == 0) {
        lv_obj_t *tab_fuel = lv_tabview_add_tab(tv, "  FUEL SETUP  ");
        style_tab(tab_fuel);
        build_fuel_setup_tab(tab_fuel, ctx);
    }

    /* ── Footer ────────────────────────────────────────────────────────── */
    lv_obj_t *footer = lv_obj_create(modal);
    lv_obj_set_size(footer, MODAL_W, FOOTER_H);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(footer, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(footer, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_pad_left(footer, 8, 0);
    lv_obj_set_style_pad_right(footer, 8, 0);
    lv_obj_set_style_pad_top(footer, 7, 0);
    lv_obj_set_style_pad_bottom(footer, 7, 0);
    lv_obj_set_style_pad_column(footer, 10, 0);

    lv_coord_t btn_w = (MODAL_W - 26) / 2;

    /* Cancel — secondary style: surface bg, muted text */
    lv_obj_t *cancel_btn = lv_btn_create(footer);
    lv_obj_set_size(cancel_btn, btn_w, FOOTER_H - 14);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_SCROLLBAR, LV_STATE_PRESSED);
    lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_border_color(cancel_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_t *clbl = lv_label_create(cancel_btn);
    lv_label_set_text(clbl, "CANCEL");
    lv_obj_set_style_text_font(clbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(clbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_letter_space(clbl, 1, 0);
    lv_obj_center(clbl);
    lv_obj_add_event_cb(cancel_btn, cancel_menu_event_cb, LV_EVENT_CLICKED, NULL);

    /* Save — primary style: accent bg, white text */
    lv_obj_t *save_btn = lv_btn_create(footer);
    lv_obj_set_size(save_btn, btn_w, FOOTER_H - 14);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(save_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_t *slbl = lv_label_create(save_btn);
    lv_label_set_text(slbl, LV_SYMBOL_SAVE "  SAVE");
    lv_obj_set_style_text_font(slbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(slbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_set_style_text_letter_space(slbl, 1, 0);
    lv_obj_center(slbl);
    lv_obj_add_event_cb(save_btn, close_menu_event_cb, LV_EVENT_CLICKED, NULL);
}
