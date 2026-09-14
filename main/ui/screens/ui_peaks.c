/*
 * ui_peaks.c — Live signal peak/min viewer ("Live values").
 *
 * Shows a scrollable table where every row is a registered signal and the
 * three numeric columns are:
 *   • Current  — sig->current_value (or - if stale)
 *   • Min      — sig->min_value     (or - if never sampled / FLT_MAX sentinel)
 *   • Max      — sig->peak_value    (or - if never sampled / -FLT_MAX sentinel)
 *
 * The signal layer always tracks peaks (since signal.h was set up that way);
 * this screen is the user-facing view of that data. The "Reset min / max"
 * button in the brand bar wipes all peaks via signal_reset_peaks().
 *
 * Built from ui_kit parts (ADR-0071): brand bar, one card holding the column
 * headers and the scrolling rows.
 *
 * Threading: the refresh callback is an lv_timer running on the LVGL task,
 * same constraints as ui_diagnostics.c.
 */
#include "ui_peaks.h"
#include "esp_attr.h"
#include "../theme.h"
#include "kit/ui_kit.h"
#include "screen_config.h"
#include "widgets/signal.h"
#include <float.h>
#include <stdio.h>
#include <string.h>

/* 100 ms refresh — fast enough to feel "live" like layout widgets,
 * still cheap (label set_text only repaints when text changes). */
#define REFRESH_PERIOD_MS  100
#define MAX_TRACKED        128   /* matches MAX_SIGNALS */
#define ROW_H              40

/* One LVGL row per signal. We cache the labels so the refresh callback
 * just rewrites text instead of rebuilding the row. The signal name,
 * three value labels, and the freshness dot all live here.
 *
 * The dot is a tiny circle on the left edge of the row that fills in
 * green when sig->is_stale == false (= a frame arrived inside
 * SIGNAL_TIMEOUT_MS), and grey when stale. last_dot_state caches the
 * last painted state so the 100 ms refresh skips redundant style
 * writes (LVGL invalidates on every style write regardless of value). */
typedef struct {
	int16_t   signal_index;
	lv_obj_t *dot;
	lv_obj_t *cur_lbl;
	lv_obj_t *min_lbl;
	lv_obj_t *max_lbl;
	int8_t    last_dot_fresh;  /* -1 = never painted, 0 = stale, 1 = fresh */
} peak_row_t;

static lv_obj_t   *s_screen          = NULL;
static lv_obj_t   *s_return_screen   = NULL;
static lv_obj_t   *s_list_container  = NULL;
static lv_timer_t *s_refresh_timer   = NULL;
static EXT_RAM_BSS_ATTR peak_row_t  s_rows[MAX_TRACKED];
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

/* A screen-wide action in the brand bar, left of Back. uk_bar's status strip
 * (its child 2) is the only slot there; the kit has no uk_bar_action() yet. */
static lv_obj_t *_bar_action(lv_obj_t *bar, uk_icon_t icon, const char *text,
                             lv_event_cb_t cb)
{
	lv_obj_t *strip = lv_obj_get_child(bar, 2);
	lv_obj_t *b = uk_btn(strip ? strip : bar, icon, text, UK_BTN_NEUTRAL, cb, NULL);
	lv_obj_set_height(b, 36);
	lv_obj_set_ext_click_area(b, 6);
	return b;
}

/* ── Row builder ─────────────────────────────────────────────────────────── */

/* Per-row reset button click — sig_idx is encoded in user_data as
 * (void *)(intptr_t)(idx + 1) so 0 vs NULL is distinguishable. */
static void _row_reset_btn_cb(lv_event_t *e)
{
	intptr_t enc = (intptr_t)lv_event_get_user_data(e);
	if (enc <= 0) return;
	int16_t idx = (int16_t)(enc - 1);
	signal_reset_peak(idx);
	/* No need to call _refresh — the 100ms timer will pick it up. The
	 * label sentinel check ("-" for FLT_MAX) renders the empty state. */
}

static lv_obj_t *_cell(lv_obj_t *row, lv_coord_t pct_x, lv_coord_t pct_w,
                       lv_color_t color)
{
	lv_obj_t *l = lv_label_create(row);
	lv_label_set_text(l, "-");
	lv_obj_set_style_text_font(l, uk_font(UK_FONT_BODY), 0);
	lv_obj_set_style_text_color(l, color, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_width(l, lv_pct(pct_w));
	lv_obj_align(l, LV_ALIGN_LEFT_MID, lv_pct(pct_x), 0);
	return l;
}

/* Build one row in the scroll container for the given signal. */
static void _add_row(lv_obj_t *parent, int16_t sig_idx, signal_t *sig)
{
	if (s_row_count >= MAX_TRACKED) return;

	/* A table row on the card: no fill of its own, a hairline under it. */
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), ROW_H);
	lv_obj_set_style_border_color(row, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_pad_hor(row, 8, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

	/* Layout: dot | 33% name | 20% cur | 18% min | 18% max | reset btn
	 * The dot sits at the row's left edge — name aligns at 16 px
	 * to leave clearance. */

	lv_obj_t *dot = lv_obj_create(row);
	lv_obj_remove_style_all(dot);
	lv_obj_set_size(dot, 8, 8);
	lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(dot, THEME_COLOR_TEXT_HINT, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

	lv_obj_t *name = lv_label_create(row);
	lv_label_set_text(name, sig->name);
	lv_obj_set_style_text_font(name, uk_font(UK_FONT_BODY), 0);
	lv_obj_set_style_text_color(name, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_width(name, lv_pct(33));
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);

	lv_obj_t *cur = _cell(row, 35, 20, THEME_COLOR_TEXT_PRIMARY);
	lv_obj_t *mn  = _cell(row, 55, 18, THEME_COLOR_STATUS_CONNECTED);
	lv_obj_t *mx  = _cell(row, 73, 18, THEME_COLOR_ACCENT_AMBER);

	/* Per-row reset button in the rightmost slot. Click clears just this
	 * signal's peak/min via signal_reset_peak(). */
	lv_obj_t *rst = uk_btn(row, UK_ICON_RESET, NULL, UK_BTN_GHOST, _row_reset_btn_cb,
	                       (void *)(intptr_t)(sig_idx + 1));
	lv_obj_set_size(rst, 44, 30);
	lv_obj_align(rst, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_ext_click_area(rst, 4);

	s_rows[s_row_count].signal_index   = sig_idx;
	s_rows[s_row_count].dot            = dot;
	s_rows[s_row_count].cur_lbl        = cur;
	s_rows[s_row_count].min_lbl        = mn;
	s_rows[s_row_count].max_lbl        = mx;
	s_rows[s_row_count].last_dot_fresh = -1;  /* force first refresh to paint */
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

	/* Update each row's freshness dot + value labels */
	char buf[24];
	for (uint16_t i = 0; i < s_row_count; i++) {
		signal_t *sig = signal_get_by_index((uint16_t)s_rows[i].signal_index);
		if (!sig) continue;

		/* Freshness dot — green when receiving a recent frame, grey when
		 * stale. The last_dot_fresh cache lets us skip lv_obj_set_style_*
		 * when nothing's changed (LVGL invalidates on every style write
		 * regardless of value). */
		int8_t want = sig->is_stale ? 0 : 1;
		if (want != s_rows[i].last_dot_fresh && s_rows[i].dot) {
			lv_color_t c = want ? THEME_COLOR_STATUS_CONNECTED
			                    : THEME_COLOR_TEXT_HINT;
			lv_obj_set_style_bg_color(s_rows[i].dot, c, LV_PART_MAIN);
			s_rows[i].last_dot_fresh = want;
		}

		if (sig->is_stale) {
			lv_label_set_text(s_rows[i].cur_lbl, "-");
			lv_obj_set_style_text_color(s_rows[i].cur_lbl,
			                             THEME_COLOR_TEXT_MUTED, 0);
		} else {
			_format_value(sig->current_value, buf, sizeof(buf));
			lv_label_set_text(s_rows[i].cur_lbl, buf);
			lv_obj_set_style_text_color(s_rows[i].cur_lbl,
			                             THEME_COLOR_TEXT_PRIMARY, 0);
		}

		if (sig->min_value == FLT_MAX) {
			lv_label_set_text(s_rows[i].min_lbl, "-");
		} else {
			_format_value(sig->min_value, buf, sizeof(buf));
			lv_label_set_text(s_rows[i].min_lbl, buf);
		}
		if (sig->peak_value == -FLT_MAX) {
			lv_label_set_text(s_rows[i].max_lbl, "-");
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

	s_screen = uk_screen();

	/* Brand bar — title, Reset min / max, Back */
	lv_obj_t *bar = uk_bar(s_screen, "Live values", UK_BAR_BACK, _back_btn_cb, NULL);
	_bar_action(bar, UK_ICON_RESET, "Reset min / max", _reset_btn_cb);

	/* One card under the bar: sticky column headers over the scrolling rows. */
	lv_obj_t *card = uk_card(uk_body(s_screen));
	lv_obj_set_size(card, lv_pct(100), lv_pct(100));
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(card, 0, 0);

	lv_obj_t *col_hdr = lv_obj_create(card);
	lv_obj_remove_style_all(col_hdr);
	lv_obj_set_size(col_hdr, lv_pct(100), 28);
	lv_obj_set_style_pad_hor(col_hdr, 8, 0);
	lv_obj_set_style_border_color(col_hdr, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(col_hdr, 1, 0);
	lv_obj_set_style_border_side(col_hdr, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

	struct { const char *txt; lv_coord_t pct_offset; lv_coord_t pct_w; uk_tone_t tone; } cols[] = {
		{ "Reading", 0,  35, UK_TONE_MUTED },
		{ "Now",     35, 20, UK_TONE_MUTED },
		{ "Min",     55, 18, UK_TONE_OK },
		{ "Max",     73, 18, UK_TONE_WARN },
	};
	for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
		lv_obj_t *l = uk_label(col_hdr, cols[i].txt, UK_FONT_LABEL, cols[i].tone);
		lv_obj_set_width(l, lv_pct(cols[i].pct_w));
		if (i == 0) {
			lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
		} else {
			lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
			lv_obj_align(l, LV_ALIGN_LEFT_MID, lv_pct(cols[i].pct_offset), 0);
		}
	}

	/* Scrollable list fills the rest of the card */
	s_list_container = uk_scroll(card);
	lv_obj_set_height(s_list_container, 0);
	lv_obj_set_flex_grow(s_list_container, 1);
	lv_obj_set_style_pad_row(s_list_container, 0, 0);
	lv_obj_set_scrollbar_mode(s_list_container, LV_SCROLLBAR_MODE_AUTO);

	/* Initial population */
	uint16_t total = signal_get_count();
	if (total == 0) {
		lv_obj_t *empty = uk_label(s_list_container,
		                           "Nothing to show yet.\n"
		                           "Load a layout that reads values from the car.",
		                           UK_FONT_BODY, UK_TONE_MUTED);
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_line_space(empty, 4, 0);
		lv_obj_set_style_pad_top(empty, 48, 0);
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
