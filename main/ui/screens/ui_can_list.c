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
 * Built from ui_kit parts (ADR-0071). The full screen is a brand bar over one
 * card holding the table; the embedded variant draws the same table straight
 * into the caller's container, so the two look alike.
 *
 * Threading: refresh runs on LVGL task. The tracker is also fed from the
 * LVGL task (from can_process_queued_frames), so no locking needed.
 */
#include "ui_can_list.h"
#include "esp_attr.h"
#include "../theme.h"
#include "kit/ui_kit.h"
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
#define ROW_H        32

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
static EXT_RAM_BSS_ATTR can_row_t   s_rows[MAX_TRACKED];
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

static lv_obj_t *_cell(lv_obj_t *row, const char *text, lv_coord_t pct_x,
                       lv_coord_t pct_w, lv_text_align_t align, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, uk_font(UK_FONT_BODY), 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_width(l, lv_pct(pct_w));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, lv_pct(pct_x), 0);
    return l;
}

/* One row per CAN ID. Layout: 18% ID | 14% Hz | 10% DLC | 56% bytes. */
static void _add_row(lv_obj_t *parent, const can_id_entry_t *e)
{
    if (s_row_count >= MAX_TRACKED) return;

    /* A table row: no fill of its own (it sits on a card or the host), a
     * hairline under it. */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_border_color(row, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char id_buf[16];
    _format_id(e->can_id, e->extended, id_buf, sizeof(id_buf));

    lv_obj_t *id_lbl    = _cell(row, id_buf, 0, 18, LV_TEXT_ALIGN_LEFT,
                                THEME_COLOR_TEXT_PRIMARY);
    lv_obj_t *hz_lbl    = _cell(row, "-", 18, 14, LV_TEXT_ALIGN_RIGHT,
                                THEME_COLOR_TEXT_PRIMARY);
    lv_obj_t *dlc_lbl   = _cell(row, "-", 32, 10, LV_TEXT_ALIGN_RIGHT,
                                THEME_COLOR_TEXT_MUTED);
    lv_obj_t *bytes_lbl = _cell(row, "(empty)", 44, 56, LV_TEXT_ALIGN_LEFT,
                                THEME_COLOR_TEXT_PRIMARY);
    lv_label_set_long_mode(bytes_lbl, LV_LABEL_LONG_DOT);

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

/* Build the table into @p parent: a caps column-header strip over a
 * scrolling list that flex-grows into the rest of the parent's height
 * (which must be definite). Shared by the full-screen and embedded
 * layouts, so both draw the same table. */
static void _build_table(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);

    lv_obj_t *col_hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(col_hdr);
    lv_obj_set_size(col_hdr, lv_pct(100), 28);
    lv_obj_set_style_pad_hor(col_hdr, 8, 0);
    lv_obj_set_style_border_color(col_hdr, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(col_hdr, 1, 0);
    lv_obj_set_style_border_side(col_hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Plain names for the four columns: ID, frames per second, DLC, data. */
    struct { const char *txt; lv_coord_t pct_offset; lv_coord_t pct_w;
             lv_text_align_t align; } cols[] = {
        { "ID",      0,  18, LV_TEXT_ALIGN_LEFT  },
        { "Per sec", 18, 14, LV_TEXT_ALIGN_RIGHT },
        { "Bytes",   32, 10, LV_TEXT_ALIGN_RIGHT },
        { "Data",    44, 56, LV_TEXT_ALIGN_LEFT  },
    };
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        lv_obj_t *l = uk_label(col_hdr, cols[i].txt, UK_FONT_LABEL, UK_TONE_MUTED);
        lv_obj_set_width(l, lv_pct(cols[i].pct_w));
        lv_obj_set_style_text_align(l, cols[i].align, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, lv_pct(cols[i].pct_offset), 0);
    }

    s_list_container = uk_scroll(parent);
    lv_obj_set_height(s_list_container, 0);
    lv_obj_set_flex_grow(s_list_container, 1);
    lv_obj_set_style_pad_row(s_list_container, 0, 0);
    lv_obj_set_scrollbar_mode(s_list_container, LV_SCROLLBAR_MODE_AUTO);
}

/* ── Refresh ─────────────────────────────────────────────────────────────── */

static void _refresh(lv_timer_t *t)
{
    (void)t;
    /* Guard on the list container only — the embedded variant has no
     * dedicated s_screen, but always sets s_list_container. */
    if (!s_list_container) return;

    /* Only recompute Hz on a slower cadence — the bytes still refresh at
     * the timer rate, but the Hz reading stabilises. */
    if (++s_hz_tick_counter >= HZ_RECOMPUTE_EVERY_TICKS) {
        can_id_tracker_recompute_hz();
        s_hz_tick_counter = 0;
    }

    uint16_t tracked = can_id_tracker_count();

    /* Empty-state placeholder: shown when no IDs have been seen yet so the
     * user understands the screen isn't broken — just no traffic. Hide it
     * the moment any frame arrives. The list is a flex column, so it is
     * centred by width and padding rather than lv_obj_align. */
    if (tracked == 0) {
        if (!s_empty_lbl) {
            s_empty_lbl = uk_label(s_list_container,
                "No CAN traffic yet.\n"
                "Check the wiring, that the ignition is on, and the bus speed.",
                UK_FONT_BODY, UK_TONE_MUTED);
            lv_obj_set_width(s_empty_lbl, lv_pct(100));
            lv_label_set_long_mode(s_empty_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(s_empty_lbl, 4, 0);
            lv_obj_set_style_pad_top(s_empty_lbl, 40, 0);
            lv_obj_set_style_pad_hor(s_empty_lbl, 16, 0);
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

    s_screen = uk_screen();

    /* Brand bar — title, Clear list, Back. The clear button sits in the
     * bar's status strip (child 2 of uk_bar), the only slot left of Back;
     * the kit has no uk_bar_action() yet. */
    lv_obj_t *bar = uk_bar(s_screen, "Live CAN IDs", UK_BAR_BACK, _back_btn_cb, NULL);
    lv_obj_t *strip = lv_obj_get_child(bar, 2);
    lv_obj_t *reset_btn = uk_btn(strip ? strip : bar, UK_ICON_RESET, "Clear list",
                                 UK_BTN_NEUTRAL, _reset_btn_cb, NULL);
    lv_obj_set_height(reset_btn, 36);
    lv_obj_set_ext_click_area(reset_btn, 6);

    /* The table on one card filling the body */
    lv_obj_t *card = uk_card(uk_body(s_screen));
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    _build_table(card);

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

/* ── Embedded variant ────────────────────────────────────────────────────── */

void can_list_ui_embed(lv_obj_t *parent)
{
    if (!parent) return;

    /* Reset the shared row cache. s_screen stays NULL — this isn't a screen,
     * so can_list_ui_is_active() correctly reports inactive while embedded. */
    s_row_count       = 0;
    s_hz_tick_counter = 0;
    s_empty_lbl       = NULL;

    /* The same table the full screen draws, laid out top-to-bottom inside
     * the caller's container (which owns the frame around it). */
    _build_table(parent);

    /* Initial paint + live refresh timer (same cadence as the full screen). */
    _refresh(NULL);
    if (s_refresh_timer) lv_timer_del(s_refresh_timer);
    s_refresh_timer = lv_timer_create(_refresh, REFRESH_PERIOD_MS, NULL);
}

void can_list_ui_embed_stop(void)
{
    if (s_refresh_timer) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    /* The col header, list container, and rows are children of the caller's
     * popup and are deleted with it — just drop our cached references so a
     * stale pointer isn't reused on the next open. */
    s_list_container = NULL;
    s_empty_lbl      = NULL;
    s_row_count      = 0;
}
