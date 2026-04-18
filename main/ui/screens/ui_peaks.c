/*
 * ui_peaks.c — Live signal peak/min viewer.
 *
 * Shows a scrollable table where every row is a registered signal and the
 * three numeric columns are:
 *   • Current  — sig->current_value (or — if stale)
 *   • Min      — sig->min_value     (or — if never sampled / FLT_MAX sentinel)
 *   • Max      — sig->peak_value    (or — if never sampled / -FLT_MAX sentinel)
 *
 * The signal layer always tracks peaks (since signal.h was set up that way);
 * this screen is the user-facing view of that data. A "Reset All" button at
 * the top wipes all peaks via signal_reset_peaks().
 *
 * Threading: the refresh callback is an lv_timer running on the LVGL task,
 * same constraints as ui_diagnostics.c.
 */
#include "ui_peaks.h"
#include "../theme.h"
#include "screen_config.h"
#include "widgets/signal.h"
#include <float.h>
#include <stdio.h>
#include <string.h>

#define REFRESH_PERIOD_MS  500
#define MAX_TRACKED        128   /* matches MAX_SIGNALS */

/* One LVGL row per signal. We cache the labels so the refresh callback
 * just rewrites text instead of rebuilding the row. The signal name and
 * the three value labels live in this struct. */
typedef struct {
	int16_t   signal_index;
	lv_obj_t *cur_lbl;
	lv_obj_t *min_lbl;
	lv_obj_t *max_lbl;
} peak_row_t;

static lv_obj_t   *s_screen          = NULL;
static lv_obj_t   *s_return_screen   = NULL;
static lv_obj_t   *s_list_container  = NULL;
static lv_timer_t *s_refresh_timer   = NULL;
static peak_row_t  s_rows[MAX_TRACKED];
static uint16_t    s_row_count       = 0;
static uint16_t    s_seen_signal_count = 0;  /* last-seen signal_get_count() */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void _format_value(float v, char *out, size_t outsz)
{
	/* Trim trailing zeros for cleaner display: 12.0000 → 12, 12.5000 → 12.5 */
	snprintf(out, outsz, "%.4f", (double)v);
	char *dot = strchr(out, '.');
	if (dot) {
		char *end = out + strlen(out) - 1;
		while (end > dot && *end == '0') *end-- = '\0';
		if (end == dot) *end = '\0';  /* drop dangling decimal point */
	}
}

/* ── Row builder ─────────────────────────────────────────────────────────── */

/* Build one row in the scroll container for the given signal. */
static void _add_row(lv_obj_t *parent, int16_t sig_idx, signal_t *sig)
{
	if (s_row_count >= MAX_TRACKED) return;

	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, lv_pct(100), 28);
	lv_obj_set_style_bg_color(row, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(row, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_style_pad_all(row, 4, 0);
	lv_obj_set_style_pad_hor(row, 8, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

	/* Signal name on the left, takes ~40% of the width */
	lv_obj_t *name = lv_label_create(row);
	lv_label_set_text(name, sig->name);
	lv_obj_set_style_text_font(name, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(name, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_width(name, lv_pct(40));
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

	/* Three value columns, right-aligned, each ~20% wide. Order: cur, min, max */
	lv_obj_t *cur = lv_label_create(row);
	lv_label_set_text(cur, "—");
	lv_obj_set_style_text_font(cur, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(cur, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_align(cur, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_width(cur, lv_pct(20));
	lv_obj_align(cur, LV_ALIGN_LEFT_MID, lv_pct(40), 0);

	lv_obj_t *mn = lv_label_create(row);
	lv_label_set_text(mn, "—");
	lv_obj_set_style_text_font(mn, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(mn, THEME_COLOR_STATUS_CONNECTED, 0);
	lv_obj_set_style_text_align(mn, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_width(mn, lv_pct(20));
	lv_obj_align(mn, LV_ALIGN_LEFT_MID, lv_pct(60), 0);

	lv_obj_t *mx = lv_label_create(row);
	lv_label_set_text(mx, "—");
	lv_obj_set_style_text_font(mx, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(mx, THEME_COLOR_ACCENT_AMBER, 0);
	lv_obj_set_style_text_align(mx, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_width(mx, lv_pct(20));
	lv_obj_align(mx, LV_ALIGN_LEFT_MID, lv_pct(80), 0);

	s_rows[s_row_count].signal_index = sig_idx;
	s_rows[s_row_count].cur_lbl      = cur;
	s_rows[s_row_count].min_lbl      = mn;
	s_rows[s_row_count].max_lbl      = mx;
	s_row_count++;
}

/* Wipe all rows from the list — used when the signal registry changes
 * (layout reload) so we rebuild fresh on next refresh. */
static void _clear_rows(void)
{
	if (s_list_container && lv_obj_is_valid(s_list_container)) {
		lv_obj_clean(s_list_container);
	}
	s_row_count = 0;
}

/* ── Refresh ─────────────────────────────────────────────────────────────── */

static void _refresh(lv_timer_t *t)
{
	(void)t;
	if (!s_screen || !s_list_container) return;

	uint16_t now_count = signal_get_count();

	/* Detect signal-registry changes (layout reload). Rebuild from scratch
	 * because indices/names may have shifted. */
	if (now_count != s_seen_signal_count) {
		_clear_rows();
		for (uint16_t i = 0; i < now_count && i < MAX_TRACKED; i++) {
			signal_t *sig = signal_get_by_index(i);
			if (sig && sig->name[0]) _add_row(s_list_container, (int16_t)i, sig);
		}
		s_seen_signal_count = now_count;
	}

	/* Update each row's three value labels */
	char buf[24];
	for (uint16_t i = 0; i < s_row_count; i++) {
		signal_t *sig = signal_get_by_index((uint16_t)s_rows[i].signal_index);
		if (!sig) continue;

		if (sig->is_stale) {
			lv_label_set_text(s_rows[i].cur_lbl, "—");
			lv_obj_set_style_text_color(s_rows[i].cur_lbl,
			                             THEME_COLOR_TEXT_MUTED, 0);
		} else {
			_format_value(sig->current_value, buf, sizeof(buf));
			lv_label_set_text(s_rows[i].cur_lbl, buf);
			lv_obj_set_style_text_color(s_rows[i].cur_lbl,
			                             THEME_COLOR_TEXT_PRIMARY, 0);
		}

		if (sig->min_value == FLT_MAX) {
			lv_label_set_text(s_rows[i].min_lbl, "—");
		} else {
			_format_value(sig->min_value, buf, sizeof(buf));
			lv_label_set_text(s_rows[i].min_lbl, buf);
		}
		if (sig->peak_value == -FLT_MAX) {
			lv_label_set_text(s_rows[i].max_lbl, "—");
		} else {
			_format_value(sig->peak_value, buf, sizeof(buf));
			lv_label_set_text(s_rows[i].max_lbl, buf);
		}
	}
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void _back_btn_cb(lv_event_t *e)
{
	(void)e;
	peaks_ui_hide();
}

static void _reset_btn_cb(lv_event_t *e)
{
	(void)e;
	signal_reset_peaks();
	/* Force an immediate refresh so the user sees the reset land. */
	_refresh(NULL);
}

/* ── Build ───────────────────────────────────────────────────────────────── */

static void _create(void)
{
	s_row_count         = 0;
	s_seen_signal_count = 0;

	s_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s_screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

	/* Header — Back / Title / Reset All */
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
	lv_label_set_text(title, "Signal Peaks");
	lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

	lv_obj_t *reset_btn = lv_btn_create(header);
	lv_obj_set_size(reset_btn, 100, 30);
	lv_obj_align(reset_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_color(reset_btn, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_border_color(reset_btn, THEME_COLOR_STATUS_ERROR, 0);
	lv_obj_set_style_border_width(reset_btn, 1, 0);
	lv_obj_set_style_radius(reset_btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_shadow_width(reset_btn, 0, 0);
	lv_obj_add_event_cb(reset_btn, _reset_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *reset_lbl = lv_label_create(reset_btn);
	lv_label_set_text(reset_lbl, "Reset All");
	lv_obj_set_style_text_font(reset_lbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(reset_lbl, THEME_COLOR_STATUS_ERROR, 0);
	lv_obj_center(reset_lbl);

	/* Column headers — sticky at the top, right above the scroll list. */
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

	struct { const char *txt; lv_coord_t pct_offset; lv_color_t color; } cols[] = {
		{ "Signal",  0,    THEME_COLOR_TEXT_MUTED },
		{ "Current", 40,   THEME_COLOR_TEXT_MUTED },
		{ "Min",     60,   THEME_COLOR_STATUS_CONNECTED },
		{ "Max",     80,   THEME_COLOR_ACCENT_AMBER },
	};
	for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
		lv_obj_t *l = lv_label_create(col_hdr);
		lv_label_set_text(l, cols[i].txt);
		lv_obj_set_style_text_font(l, THEME_FONT_TINY, 0);
		lv_obj_set_style_text_color(l, cols[i].color, 0);
		lv_obj_set_style_text_letter_space(l, 1, 0);
		lv_obj_set_width(l, lv_pct(20));
		if (i == 0) {
			lv_obj_set_width(l, lv_pct(40));
			lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
		} else {
			lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
			lv_obj_align(l, LV_ALIGN_LEFT_MID, lv_pct(cols[i].pct_offset), 0);
		}
	}

	/* Scrollable list container fills the remaining space */
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
	lv_obj_set_style_bg_color(s_list_container, THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(s_list_container, LV_OPA_50, LV_PART_SCROLLBAR);
	lv_obj_set_style_radius(s_list_container, 2, LV_PART_SCROLLBAR);
	lv_obj_set_style_width(s_list_container, 4, LV_PART_SCROLLBAR);

	/* Initial population */
	uint16_t total = signal_get_count();
	if (total == 0) {
		lv_obj_t *empty = lv_label_create(s_list_container);
		lv_label_set_text(empty, "No signals registered.\n"
		                          "Load a layout with CAN signals to see peaks.");
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(empty, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(empty, THEME_FONT_SMALL, 0);
		lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
	} else {
		for (uint16_t i = 0; i < total && i < MAX_TRACKED; i++) {
			signal_t *sig = signal_get_by_index(i);
			if (sig && sig->name[0]) _add_row(s_list_container, (int16_t)i, sig);
		}
		s_seen_signal_count = total;
		_refresh(NULL);
	}

	if (s_refresh_timer) {
		lv_timer_del(s_refresh_timer);
	}
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
	s_row_count = 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void peaks_ui_show(void)
{
	if (s_screen) return;
	s_return_screen = lv_scr_act();
	_create();
	lv_scr_load(s_screen);
}

void peaks_ui_hide(void)
{
	if (!s_screen) return;
	lv_obj_t *ret = s_return_screen;
	s_return_screen = NULL;
	if (ret && lv_obj_is_valid(ret)) {
		lv_scr_load(ret);
	}
	_destroy();
}

bool peaks_ui_is_active(void)
{
	return s_screen != NULL;
}
