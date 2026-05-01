/*
 * ui_can_list.c - Live CAN ID viewer.
 *
 * Lists every CAN ID currently being tracked by can_id_tracker, with the
 * last 8 data bytes, rolling Hz, and DLC. Designed for diagnosing what's
 * on the bus when you don't have a DBC yet.
 *
 * Performance pattern (matches ui_peaks.c): rows are built once and cached
 * in s_rows[]; the refresh callback only calls lv_label_set_text() on the
 * cached label refs. That keeps per-tick CPU low on the S3 even with
 * dozens of IDs at high frame rates. New IDs are appended to the table
 * without rebuilding existing rows.
 *
 * Threading: refresh runs on LVGL task. The tracker is also fed from the
 * LVGL task (from can_process_queued_frames), so no locking needed.
 */
#include "ui_can_list.h"
#include "../theme.h"
#include "screen_config.h"
#include "can/can_id_tracker.h"
#include <stdio.h>
#include <string.h>

/* 250 ms refresh — fast enough to feel live, but slow enough that the
 * S3 isn't churning labels at 30 Hz across 64 rows. */
#define REFRESH_PERIOD_MS  250

/* Recompute Hz at this cadence. Multiple of REFRESH_PERIOD_MS so we tick
 * cleanly. 1 Hz updates → stable readings even at low frame rates. */
#define HZ_RECOMPUTE_EVERY_TICKS  4   /* 250 ms × 4 = 1 s */

#define MAX_TRACKED  CAN_ID_TRACKER_MAX_IDS

typedef struct {
    uint32_t  can_id;
    bool      extended;
    lv_obj_t *id_lbl;
    lv_obj_t *hz_lbl;
    lv_obj_t *dlc_lbl;
    lv_obj_t *bytes_lbl;
} can_row_t;

static lv_obj_t   *s_screen          = NULL;
static lv_obj_t   *s_return_screen   = NULL;
static lv_obj_t   *s_list_container  = NULL;
static lv_obj_t   *s_empty_lbl       = NULL;
static lv_timer_t *s_refresh_timer   = NULL;
static can_row_t   s_rows[MAX_TRACKED];
static uint16_t    s_row_count       = 0;
static uint8_t     s_hz_tick_counter = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void _format_id(uint32_t id, bool extd, char *out, size_t outsz)
{
    if (extd) snprintf(out, outsz, "0x%08lX", (unsigned long)id);
    else      snprintf(out, outsz, "0x%03lX",  (unsigned long)id);
}

static void _format_bytes(const uint8_t *data, uint8_t dlc,
                          char *out, size_t outsz)
{
    if (dlc == 0) {
        snprintf(out, outsz, "(empty)");
        return;
    }
    /* "AB CD EF 12 34 56 78 9A" — 23 chars + null for 8 bytes */
    size_t pos = 0;
    for (uint8_t i = 0; i < dlc && pos + 4 < outsz; i++) {
        pos += snprintf(out + pos, outsz - pos, "%s%02X",
                        i == 0 ? "" : " ", data[i]);
    }
}

static void _format_hz(float hz, char *out, size_t outsz)
{
    if (hz <= 0.0f) {
        snprintf(out, outsz, "-");
    } else if (hz < 10.0f) {
        snprintf(out, outsz, "%.1f", (double)hz);
    } else {
        snprintf(out, outsz, "%.0f", (double)hz);
    }
}

/* ── Row builder ─────────────────────────────────────────────────────────── */

/* One row per CAN ID. Layout: 18% ID | 14% Hz | 10% DLC | 58% bytes. */
static void _add_row(lv_obj_t *parent, const can_id_entry_t *e)
{
    if (s_row_count >= MAX_TRACKED) return;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 32);
    lv_obj_set_style_bg_color(row, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char id_buf[16];
    _format_id(e->can_id, e->extended, id_buf, sizeof(id_buf));

    lv_obj_t *id_lbl = lv_label_create(row);
    lv_label_set_text(id_lbl, id_buf);
    lv_obj_set_style_text_font(id_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(id_lbl, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_width(id_lbl, lv_pct(18));
    lv_obj_align(id_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *hz_lbl = lv_label_create(row);
    lv_label_set_text(hz_lbl, "-");
    lv_obj_set_style_text_font(hz_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(hz_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_align(hz_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(hz_lbl, lv_pct(14));
    lv_obj_align(hz_lbl, LV_ALIGN_LEFT_MID, lv_pct(18), 0);

    lv_obj_t *dlc_lbl = lv_label_create(row);
    lv_label_set_text(dlc_lbl, "-");
    lv_obj_set_style_text_font(dlc_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(dlc_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(dlc_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(dlc_lbl, lv_pct(10));
    lv_obj_align(dlc_lbl, LV_ALIGN_LEFT_MID, lv_pct(32), 0);

    lv_obj_t *bytes_lbl = lv_label_create(row);
    lv_label_set_text(bytes_lbl, "(empty)");
    lv_obj_set_style_text_font(bytes_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(bytes_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(bytes_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(bytes_lbl, lv_pct(56));
    lv_obj_align(bytes_lbl, LV_ALIGN_LEFT_MID, lv_pct(42), 0);

    s_rows[s_row_count].can_id    = e->can_id;
    s_rows[s_row_count].extended  = e->extended;
    s_rows[s_row_count].id_lbl    = id_lbl;
    s_rows[s_row_count].hz_lbl    = hz_lbl;
    s_rows[s_row_count].dlc_lbl   = dlc_lbl;
    s_rows[s_row_count].bytes_lbl = bytes_lbl;
    s_row_count++;
}

static void _clear_rows(void)
{
    if (s_list_container && lv_obj_is_valid(s_list_container)) {
        lv_obj_clean(s_list_container);
    }
    s_row_count = 0;
    /* lv_obj_clean deleted the empty-state placeholder too — drop the
     * stale pointer so the next refresh recreates it. */
    s_empty_lbl = NULL;
}

/* ── Refresh ─────────────────────────────────────────────────────────────── */

static void _refresh(lv_timer_t *t)
{
    (void)t;
    if (!s_screen || !s_list_container) return;

    /* Only recompute Hz on a slower cadence — the bytes still refresh at
     * the timer rate, but the Hz reading stabilises. */
    if (++s_hz_tick_counter >= HZ_RECOMPUTE_EVERY_TICKS) {
        can_id_tracker_recompute_hz();
        s_hz_tick_counter = 0;
    }

    uint16_t tracked = can_id_tracker_count();

    /* Empty-state placeholder: shown when no IDs have been seen yet so the
     * user understands the screen isn't broken — just no traffic. Hide it
     * the moment any frame arrives. */
    if (tracked == 0) {
        if (!s_empty_lbl) {
            s_empty_lbl = lv_label_create(s_list_container);
            lv_label_set_text(s_empty_lbl,
                "No CAN traffic yet.\n"
                "Confirm wiring, ignition, and bitrate (Device Settings).");
            lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(s_empty_lbl,
                                          THEME_COLOR_TEXT_MUTED, 0);
            lv_obj_set_style_text_font(s_empty_lbl, THEME_FONT_SMALL, 0);
            lv_obj_align(s_empty_lbl, LV_ALIGN_CENTER, 0, 0);
        }
        return;
    } else if (s_empty_lbl) {
        lv_obj_del(s_empty_lbl);
        s_empty_lbl = NULL;
    }

    /* Append new rows for IDs we haven't built yet. We never rebuild
     * existing rows — entries in the tracker are stable in their slot. */
    while (s_row_count < tracked && s_row_count < MAX_TRACKED) {
        const can_id_entry_t *e = can_id_tracker_get(s_row_count);
        if (!e) break;
        _add_row(s_list_container, e);
    }

    /* Update each row's three dynamic labels (Hz, DLC, bytes). The ID is
     * static so we skip it. */
    char buf[40];
    for (uint16_t i = 0; i < s_row_count; i++) {
        const can_id_entry_t *e = can_id_tracker_get(i);
        if (!e) continue;

        _format_hz(e->last_hz, buf, sizeof(buf));
        lv_label_set_text(s_rows[i].hz_lbl, buf);

        snprintf(buf, sizeof(buf), "%u", (unsigned)e->dlc);
        lv_label_set_text(s_rows[i].dlc_lbl, buf);

        _format_bytes(e->data, e->dlc, buf, sizeof(buf));
        lv_label_set_text(s_rows[i].bytes_lbl, buf);
    }
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void _back_btn_cb(lv_event_t *e)
{
    (void)e;
    can_list_ui_hide();
}

static void _reset_btn_cb(lv_event_t *e)
{
    (void)e;
    can_id_tracker_reset();
    _clear_rows();
    /* Tracker is now empty; next _refresh shows the placeholder. */
}

/* ── Build ───────────────────────────────────────────────────────────────── */

static void _create(void)
{
    s_row_count       = 0;
    s_hz_tick_counter = 0;
    s_empty_lbl       = NULL;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Header — Back / Title / Reset */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_set_size(header, SCREEN_W, 44);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 10, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 80, 30);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_color(back_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_radius(back_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, _back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(back_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "CAN ID Live");
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *reset_btn = lv_btn_create(header);
    lv_obj_set_size(reset_btn, 90, 30);
    lv_obj_align(reset_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(reset_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_color(reset_btn, THEME_COLOR_STATUS_ERROR, 0);
    lv_obj_set_style_border_width(reset_btn, 1, 0);
    lv_obj_set_style_radius(reset_btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(reset_btn, 0, 0);
    lv_obj_add_event_cb(reset_btn, _reset_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reset_lbl = lv_label_create(reset_btn);
    lv_label_set_text(reset_lbl, "Reset");
    lv_obj_set_style_text_font(reset_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(reset_lbl, THEME_COLOR_STATUS_ERROR, 0);
    lv_obj_center(reset_lbl);

    /* Column headers */
    lv_obj_t *col_hdr = lv_obj_create(s_screen);
    lv_obj_set_size(col_hdr, SCREEN_W, 28);
    lv_obj_align(col_hdr, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(col_hdr, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(col_hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(col_hdr, 0, 0);
    lv_obj_set_style_radius(col_hdr, 0, 0);
    lv_obj_set_style_pad_all(col_hdr, 4, 0);
    lv_obj_set_style_pad_hor(col_hdr, 8, 0);
    lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *txt; lv_coord_t pct_offset; lv_coord_t pct_w;
             lv_text_align_t align; } cols[] = {
        { "ID",    0,  18, LV_TEXT_ALIGN_LEFT  },
        { "Hz",    18, 14, LV_TEXT_ALIGN_RIGHT },
        { "DLC",   32, 10, LV_TEXT_ALIGN_RIGHT },
        { "Bytes", 42, 56, LV_TEXT_ALIGN_LEFT  },
    };
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        lv_obj_t *l = lv_label_create(col_hdr);
        lv_label_set_text(l, cols[i].txt);
        lv_obj_set_style_text_font(l, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(l, THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_obj_set_width(l, lv_pct(cols[i].pct_w));
        lv_obj_set_style_text_align(l, cols[i].align, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, lv_pct(cols[i].pct_offset), 0);
    }

    /* Scrollable list */
    s_list_container = lv_obj_create(s_screen);
    lv_obj_set_size(s_list_container, SCREEN_W, SCREEN_H - 44 - 28);
    lv_obj_align(s_list_container, LV_ALIGN_TOP_MID, 0, 44 + 28);
    lv_obj_set_style_bg_color(s_list_container, THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_list_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list_container, 0, 0);
    lv_obj_set_style_radius(s_list_container, 0, 0);
    lv_obj_set_style_pad_all(s_list_container, 0, 0);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_list_container,
                                THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list_container, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list_container, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_list_container, 4, LV_PART_SCROLLBAR);

    /* Initial paint */
    _refresh(NULL);

    if (s_refresh_timer) lv_timer_del(s_refresh_timer);
    s_refresh_timer = lv_timer_create(_refresh, REFRESH_PERIOD_MS, NULL);
}

static void _destroy(void)
{
    if (s_refresh_timer) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
    }
    s_list_container = NULL;
    s_empty_lbl      = NULL;
    s_row_count      = 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void can_list_ui_show(void)
{
    if (s_screen) return;
    s_return_screen = lv_scr_act();
    _create();
    lv_scr_load(s_screen);
}

void can_list_ui_hide(void)
{
    if (!s_screen) return;
    lv_obj_t *ret = s_return_screen;
    s_return_screen = NULL;
    if (ret && lv_obj_is_valid(ret)) {
        lv_scr_load(ret);
    }
    _destroy();
}

bool can_list_ui_is_active(void)
{
    return s_screen != NULL;
}
