/*
 * widget_meter.c — Analog sweeping gauge for a value slot.
 *
 * Binds to a value slot (0–12). Receives raw int32_t via:
 *     w->update(w, &raw_value);
 * No double-precision math; only int32_t and clamping.
 */
#include "widget_meter.h"
#include "widget_image.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "signal.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widget_types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_meter";

/* Apply the user-configured anchor curve. Returns the "virtual" value to
 * pass to lv_meter_set_indicator_value so the needle lands at the desired
 * non-linear position. Two linear segments split at (anchor_value,
 * anchor_position%): values in [min..anchor] map to [0..anchor_pos%] of the
 * sweep, values in [anchor..max] map to [anchor_pos%..100%]. Disabled
 * (anchor_enabled=false) = linear pass-through. */
static int32_t _meter_apply_curve(meter_data_t *md, int32_t v) {
	if (!md->anchor_enabled || md->max <= md->min) return v;
	int32_t a  = md->anchor_value;
	int32_t ap = md->anchor_position;
	if (ap < 0)   ap = 0;
	if (ap > 100) ap = 100;
	int32_t pct;
	if (v <= a) {
		pct = (a > md->min)
		    ? (int32_t)(((int64_t)(v - md->min) * ap) / (a - md->min))
		    : 0;
	} else {
		int32_t hp = 100 - ap;
		pct = (md->max > a)
		    ? ap + (int32_t)(((int64_t)(v - a) * hp) / (md->max - a))
		    : 100;
	}
	if (pct < 0)   pct = 0;
	if (pct > 100) pct = 100;
	return md->min + (int32_t)(((int64_t)pct * (md->max - md->min)) / 100);
}

#define METER_DEFAULT_W 140
#define METER_DEFAULT_H 140

static void _meter_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !w->root || !lv_obj_is_valid(w->root)) return;
	if (!md->meter || !md->needle) return;
	int32_t v;
	if (is_stale) {
		v = md->min;
	} else {
		v = (int32_t)value;
		if (v < md->min) v = md->min;
		if (v > md->max) v = md->max;
	}
	/* Apply non-linear scale curve (gamma) — affects only the needle
	 * angle, not the tick labels. See _meter_apply_curve docstring. */
	v = _meter_apply_curve(md, v);
	/* Only drive whichever meter is currently visible. Updating the hidden
	 * sibling costs an LVGL indicator recompute + invalidation-mark that
	 * nobody ever sees — with meters bound to fast-moving sim signals
	 * (RPM etc.) this is a measurable chunk of the per-frame budget.
	 * When night mode toggles, _meter_apply_night_mode immediately pushes
	 * the current value into the newly-visible meter so the swap stays
	 * continuous. */
	if (md->night_meter && md->night_needle && lv_obj_is_valid(md->night_meter) &&
	    !lv_obj_has_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN)) {
		lv_meter_set_indicator_value(md->night_meter, md->night_needle, v);
	} else if (md->meter && md->needle &&
	           !lv_obj_has_flag(md->meter, LV_OBJ_FLAG_HIDDEN)) {
		lv_meter_set_indicator_value(md->meter, md->needle, v);
	}
}

/* Forward declarations — used by _meter_create / _meter_destroy below. */
static void _meter_apply_night_mode(widget_t *w, bool active);
static void _meter_night_cb(bool active, void *user_data);

/* True when at least one night-mode override touches a property baked in at
 * creation time (LVGL v8 can't mutate these live). When true, _meter_create
 * builds a sibling "night meter" with the night values pre-baked, and
 * apply_night_mode toggles visibility instead of mutating styles. */
static inline bool _meter_needs_night_meter(const meter_data_t *md) {
#if NIGHT_MODE_DISABLED
	(void)md;
	return false;
#else
	return md->night.has_minor_tick_color ||
	       md->night.has_major_tick_color ||
	       md->night.has_needle_color     ||
	       md->night.has_needle_image_name ||
	       md->night.has_bg_image_name;
#endif
}

/* Point at fraction num/den along the needle from pivot (p1) toward tip, with
 * a perpendicular offset of `perp` pixels (positive = left-of-needle facing
 * from pivot toward tip, negative = right). Shared by every polygon style
 * below to keep the geometry readable. */
static inline lv_point_t _tip_pt(const lv_point_t *p1, int32_t dx, int32_t dy,
                                 int32_t len, int32_t num, int32_t den,
                                 int32_t perp) {
	lv_point_t p;
	p.x = p1->x + (dx * num) / den + (-dy * perp) / len;
	p.y = p1->y + (dy * num) / den + ( dx * perp) / len;
	return p;
}

/* Custom needle tip renderer. Hooks LV_EVENT_DRAW_PART_BEGIN / _END on the
 * meter; LVGL fires DRAW_PART_NEEDLE_LINE for each line-needle indicator with
 * the pivot (p1), tip (p2), and line_dsc already populated.
 *
 * Styles (all use the line's color and scale their proportions with the
 * configured needle width, so they look consistent on big and small meters):
 *   0 Flat    — plain line end, LVGL default (early return)
 *   1 Rounded — mutate line_dsc->round_start/round_end on BEGIN (soft caps)
 *   2 Lance   — uniform shaft for 90% of length, short tapered tip (classic
 *               sword-hand / premium aviation-watch look)
 *   3 Dagger  — full taper from wide pivot base to a sharp apex at the tip
 *   4 Spade   — tapered trapezoid: wide base narrows toward the tip but
 *               terminates in a short flat cap instead of a point (reads
 *               more "blunt" and refined than Dagger)
 *   5 Diamond — dauphine / rhombus shape, pointed at both pivot and tip,
 *               widest at mid-length (classy watchmaker feel)
 *
 * Polygon styles (2-5) hide the built-in line at BEGIN and draw custom
 * geometry via lv_draw_polygon at END. Integer-only math (lv_sqrt) — no
 * libm pull-in. Tip styles are ignored for image needles. */
static void _meter_needle_draw_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code != LV_EVENT_DRAW_PART_BEGIN && code != LV_EVENT_DRAW_PART_END) return;

	lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
	if (!dsc) return;
	if (dsc->type != LV_METER_DRAW_PART_NEEDLE_LINE) return;
	if (dsc->p1 == NULL || dsc->p2 == NULL || dsc->line_dsc == NULL) return;

	meter_data_t *md = (meter_data_t *)lv_event_get_user_data(e);
	if (!md) return;
	uint8_t style = md->needle_tip_style;
	lv_draw_line_dsc_t *line_dsc = dsc->line_dsc;

	if (code == LV_EVENT_DRAW_PART_BEGIN) {
		if (style == 1) {
			line_dsc->round_start = 1;
			line_dsc->round_end   = 1;
		} else if (style >= 2 && style <= 5) {
			/* Polygon styles replace the line — hide the original. */
			line_dsc->opa = LV_OPA_TRANSP;
		}
		return;
	}

	/* === DRAW_PART_END === */

	/* Rear counterweight extension. Draws a short line in the opposite
	 * direction from the pivot, so a needle that points at "8 o'clock"
	 * also has a small "2 o'clock" tail. Length in pixels. */
	if (md->needle_rear_length > 0) {
		int32_t rdx = dsc->p2->x - dsc->p1->x;
		int32_t rdy = dsc->p2->y - dsc->p1->y;
		uint32_t rlen_sq = (uint32_t)(rdx * rdx + rdy * rdy);
		if (rlen_sq > 0) {
			lv_sqrt_res_t rsres;
			lv_sqrt(rlen_sq, &rsres, 0x800);
			int32_t rlen = rsres.i;
			if (rlen > 0) {
				int32_t back = md->needle_rear_length;
				lv_point_t rear_pt;
				rear_pt.x = dsc->p1->x - (rdx * back) / rlen;
				rear_pt.y = dsc->p1->y - (rdy * back) / rlen;
				lv_draw_line_dsc_t rear_dsc;
				lv_draw_line_dsc_init(&rear_dsc);
				rear_dsc.color       = line_dsc->color;
				rear_dsc.width       = line_dsc->width > 0 ? line_dsc->width
				                                          : md->needle_width;
				rear_dsc.opa         = LV_OPA_COVER;
				rear_dsc.round_start = line_dsc->round_start;
				rear_dsc.round_end   = line_dsc->round_end;
				lv_draw_line(dsc->draw_ctx, &rear_dsc, dsc->p1, &rear_pt);
			}
		}
	}

	/* Only polygon styles have custom geometry beyond this point. */
	if (style < 2 || style > 5) return;

	const lv_point_t *p1 = dsc->p1;   /* pivot */
	const lv_point_t *p2 = dsc->p2;   /* tip   */
	int32_t dx = p2->x - p1->x;
	int32_t dy = p2->y - p1->y;
	uint32_t len_sq = (uint32_t)(dx * dx + dy * dy);
	if (len_sq == 0) return;
	lv_sqrt_res_t sres;
	lv_sqrt(len_sq, &sres, 0x800);
	int32_t len = sres.i;
	if (len == 0) return;

	int32_t width = line_dsc->width > 0 ? line_dsc->width : md->needle_width;

	lv_draw_rect_dsc_t rdsc;
	lv_draw_rect_dsc_init(&rdsc);
	rdsc.bg_color     = line_dsc->color;
	rdsc.bg_opa       = LV_OPA_COVER;
	rdsc.border_width = 0;

	lv_point_t pts[6];
	uint16_t   npts = 0;

	/* Three user-exposed knobs. Each defaults to "auto" (0), which keeps the
	 * style's original built-in look; any non-zero value overrides it. */
	int32_t ov_base  = md->needle_tip_base_w;
	int32_t ov_point = md->needle_tip_point_w;
	int32_t ov_taper = md->needle_tip_taper;
	if (ov_taper > 100) ov_taper = 100;

	switch (style) {
	case 2: {
		/* Lance: uniform shaft for `shaft%` of length, then narrows to the
		 * tip. If point_w > 0 the tip is a flat cap instead of a sharp point
		 * (turns Lance into a miniature Spade-like hand). */
		int32_t half_w  = ov_base  > 0 ? ov_base  : (width / 2 + 1);
		int32_t shaft   = ov_taper > 0 ? ov_taper : 90;
		int32_t cap_w   = ov_point;
		pts[0] = _tip_pt(p1, dx, dy, len, 0,     100,  half_w);
		pts[1] = _tip_pt(p1, dx, dy, len, shaft, 100,  half_w);
		if (cap_w > 0) {
			pts[2] = _tip_pt(p1, dx, dy, len, 100, 100,  cap_w);
			pts[3] = _tip_pt(p1, dx, dy, len, 100, 100, -cap_w);
			pts[4] = _tip_pt(p1, dx, dy, len, shaft, 100, -half_w);
			pts[5] = _tip_pt(p1, dx, dy, len, 0,     100, -half_w);
			npts = 6;
		} else {
			pts[2] = *p2;
			pts[3] = _tip_pt(p1, dx, dy, len, shaft, 100, -half_w);
			pts[4] = _tip_pt(p1, dx, dy, len, 0,     100, -half_w);
			npts = 5;
		}
		break;
	}
	case 3: {
		/* Dagger: wide base, sharp apex. If the user sets point_w or taper
		 * it morphs into a Spade-style trapezoid — useful for dialing in
		 * "pointy but not a stabby point". */
		int32_t half_w = ov_base > 0 ? ov_base : (width / 2 + 2);
		if (ov_point > 0 || (ov_taper > 0 && ov_taper < 100)) {
			int32_t cap_pos = ov_taper > 0 ? ov_taper : 97;
			int32_t cap_w   = ov_point > 0 ? ov_point : 1;
			pts[0] = _tip_pt(p1, dx, dy, len, 0,       100,  half_w);
			pts[1] = _tip_pt(p1, dx, dy, len, cap_pos, 100,  cap_w);
			pts[2] = _tip_pt(p1, dx, dy, len, cap_pos, 100, -cap_w);
			pts[3] = _tip_pt(p1, dx, dy, len, 0,       100, -half_w);
			npts = 4;
		} else {
			pts[0] = _tip_pt(p1, dx, dy, len, 0, 1,  half_w);
			pts[1] = *p2;
			pts[2] = _tip_pt(p1, dx, dy, len, 0, 1, -half_w);
			npts = 3;
		}
		break;
	}
	case 4: {
		/* Spade: tapered trapezoid terminating in a flat cap. */
		int32_t half_w  = ov_base  > 0 ? ov_base  : (width / 2 + 2);
		int32_t cap_pos = ov_taper > 0 ? ov_taper : 97;
		int32_t cap_w   = ov_point > 0 ? ov_point : 1;
		pts[0] = _tip_pt(p1, dx, dy, len, 0,       100,  half_w);
		pts[1] = _tip_pt(p1, dx, dy, len, cap_pos, 100,  cap_w);
		pts[2] = _tip_pt(p1, dx, dy, len, cap_pos, 100, -cap_w);
		pts[3] = _tip_pt(p1, dx, dy, len, 0,       100, -half_w);
		npts = 4;
		break;
	}
	case 5: {
		/* Diamond / Dauphine: pointed at both ends, widest at `taper%`.
		 * For this style point_w is unused — the tip is always a sharp apex
		 * at p2; tuning the "pointedness" is done via base_w instead. */
		int32_t half_w  = ov_base  > 0 ? ov_base  : (width / 2 + 3);
		int32_t mid_pos = ov_taper > 0 ? ov_taper : 50;
		pts[0] = *p1;
		pts[1] = _tip_pt(p1, dx, dy, len, mid_pos, 100,  half_w);
		pts[2] = *p2;
		pts[3] = _tip_pt(p1, dx, dy, len, mid_pos, 100, -half_w);
		npts = 4;
		break;
	}
	default:
		return;
	}

	lv_draw_polygon(dsc->draw_ctx, &rdsc, pts, npts);
}

/* Build a single meter (day or night). When `use_night` is true, any field
 * with a corresponding night override picks the night value; otherwise the
 * day value is used. The output pointers (`*out_meter`, `*out_scale`,
 * `*out_needle`, `*out_needle_scale`) receive the created LVGL handles.
 * `*out_needle_img_dsc` and `*out_bg_img_dsc` receive any loaded image
 * descriptors so the caller can free them on destroy. */
static void _meter_build_one(meter_data_t *md, lv_obj_t *parent, bool use_night,
                             lv_obj_t **out_meter,
                             lv_meter_scale_t **out_scale,
                             lv_meter_indicator_t **out_needle,
                             lv_meter_scale_t **out_needle_scale,
                             lv_img_dsc_t **out_needle_img_dsc,
                             lv_img_dsc_t **out_bg_img_dsc) {
	/* Pick effective values based on day/night */
	lv_color_t bg_color    = use_night ? NIGHT_PICK_COLOR(true, md->night, meter_bg_color,    md->meter_bg_color)    : md->meter_bg_color;
	lv_color_t bdr_color   = use_night ? NIGHT_PICK_COLOR(true, md->night, border_color,      md->border_color)      : md->border_color;
	lv_color_t mintc       = use_night ? NIGHT_PICK_COLOR(true, md->night, minor_tick_color,  md->minor_tick_color)  : md->minor_tick_color;
	lv_color_t majtc       = use_night ? NIGHT_PICK_COLOR(true, md->night, major_tick_color,  md->major_tick_color)  : md->major_tick_color;
	lv_color_t needle_c    = use_night ? NIGHT_PICK_COLOR(true, md->night, needle_color,      md->needle_color)      : md->needle_color;
	lv_color_t ball_c      = use_night ? NIGHT_PICK_COLOR(true, md->night, needle_ball_color, md->needle_ball_color) : md->needle_ball_color;
	lv_color_t tlc         = use_night ? NIGHT_PICK_COLOR(true, md->night, tick_label_color,  md->tick_label_color)  : md->tick_label_color;
	const char *needle_img = (use_night && md->night.has_needle_image_name) ? md->night.needle_image_name : md->needle_image_name;
	const char *bg_img     = (use_night && md->night.has_bg_image_name)     ? md->night.bg_image_name     : md->bg_image_name;

	lv_obj_t *m = lv_meter_create(parent);
	if (!m) { *out_meter = NULL; return; }

	/* Placeholder size — caller sets the real w/h after we return. */
	lv_obj_set_size(m, METER_DEFAULT_W, METER_DEFAULT_H);
	lv_obj_set_style_bg_color(m, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(m, md->meter_bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(m, md->border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (md->border_width > 0) {
		lv_obj_set_style_border_color(m, bdr_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(m, md->border_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	lv_obj_set_style_pad_top(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Background image */
	if (bg_img && bg_img[0] != '\0') {
		lv_img_dsc_t *bgdsc = rdm_image_load(bg_img);
		if (bgdsc) {
			lv_obj_set_style_bg_img_src(m, bgdsc, LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		*out_bg_img_dsc = bgdsc;
	}

	lv_meter_scale_t *scale = lv_meter_add_scale(m);
	uint32_t angle_range = (360 + (md->end_angle % 360) - (md->start_angle % 360)) % 360;
	if (angle_range == 0 && md->start_angle != md->end_angle) angle_range = 360;
	lv_meter_set_scale_range(m, scale, md->min, md->max, angle_range, (int32_t)md->start_angle);
	uint8_t mtc = md->minor_tick_count < 2 ? 2 : md->minor_tick_count;
	uint8_t mte = md->major_tick_every < 1 ? 1 : md->major_tick_every;
	/* show_ticks=false zeroes the widths so LVGL draws no tick marks. Range
	 * + count still need to be set so the needle angle math stays correct —
	 * only the visible lines go away. */
	uint8_t minor_w = md->show_ticks ? md->minor_tick_width : 0;
	uint8_t minor_l = md->show_ticks ? md->minor_tick_length : 0;
	uint8_t major_w = md->show_ticks ? md->major_tick_width : 0;
	uint8_t major_l = md->show_ticks ? md->major_tick_length : 0;
	lv_meter_set_scale_ticks(m, scale, mtc, minor_w, minor_l, mintc);
	lv_meter_set_scale_major_ticks(m, scale, mte, major_w, major_l, majtc, md->label_gap);

	/* Tick label color/font */
	lv_obj_set_style_text_color(m, tlc, LV_PART_TICKS);
	if (md->tick_label_font[0] != '\0') {
		const lv_font_t *tfont = widget_resolve_font(md->tick_label_font);
		if (tfont) lv_obj_set_style_text_font(m, tfont, LV_PART_TICKS);
	}
	if (!md->show_tick_labels) {
		lv_obj_set_style_text_opa(m, LV_OPA_TRANSP, LV_PART_TICKS);
	}

	/* Needle */
	lv_meter_indicator_t *needle;
	lv_meter_scale_t *needle_target_scale = scale;
	*out_needle_scale = NULL;
	if (needle_img && needle_img[0] != '\0') {
		lv_img_dsc_t *ndsc = rdm_image_load(needle_img);
		if (ndsc) {
			if (md->needle_angle_offset != 0) {
				lv_meter_scale_t *ns = lv_meter_add_scale(m);
				lv_meter_set_scale_range(m, ns, md->min, md->max, angle_range,
				                         (int32_t)(md->start_angle + md->needle_angle_offset));
				lv_meter_set_scale_ticks(m, ns, 0, 0, 0, lv_color_black());
				*out_needle_scale = ns;
				needle_target_scale = ns;
			}
			needle = lv_meter_add_needle_img(m, needle_target_scale, ndsc,
			                                  md->needle_pivot_x, md->needle_pivot_y);
			*out_needle_img_dsc = ndsc;
		} else {
			needle = lv_meter_add_needle_line(m, scale, md->needle_width, needle_c, md->needle_r_mod);
		}
	} else {
		needle = lv_meter_add_needle_line(m, scale, md->needle_width, needle_c, md->needle_r_mod);
	}

	/* Needle center ball */
	if (md->needle_ball_size == 0) {
		lv_obj_set_style_size(m, 0, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_INDICATOR);
	} else {
		lv_obj_set_style_size(m, md->needle_ball_size, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(m, ball_c, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_INDICATOR);
	}

	lv_meter_set_indicator_value(m, needle, md->min);

	/* Custom needle-tip hook. Fires for every DRAW_PART — the callback gates
	 * on style==0 so the fast path is just a couple of pointer loads when
	 * the feature is off. Tip styles are ignored when drawing image needles
	 * (different part type); line needles handle all 4 styles. */
	lv_obj_add_event_cb(m, _meter_needle_draw_cb, LV_EVENT_DRAW_PART_BEGIN, md);
	lv_obj_add_event_cb(m, _meter_needle_draw_cb, LV_EVENT_DRAW_PART_END,   md);

	*out_meter = m;
	*out_scale = scale;
	*out_needle = needle;
}

static void _meter_create(widget_t *w, lv_obj_t *parent) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md) {
		ESP_LOGE(TAG, "_meter_create: missing meter_data");
		return;
	}

	bool needs_night = _meter_needs_night_meter(md);

	/* When a night meter is needed, wrap both meters in a transparent
	 * container so long-press / click events have a stable hit target
	 * regardless of which meter is currently visible. Otherwise the day
	 * meter itself is the root (preserves existing behavior for layouts
	 * with no night override or only color-mutable night overrides). */
	lv_obj_t *meter_parent;
	lv_obj_t *cont = NULL;
	if (needs_night) {
		cont = lv_obj_create(parent);
		lv_obj_set_size(cont, (lv_coord_t)w->w, (lv_coord_t)w->h);
		lv_obj_set_align(cont, LV_ALIGN_CENTER);
		lv_obj_set_pos(cont, w->x, w->y);
		lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		meter_parent = cont;
	} else {
		meter_parent = parent;
	}

	/* Build day meter */
	lv_obj_t *m = NULL;
	lv_meter_scale_t *scale = NULL;
	lv_meter_indicator_t *needle = NULL;
	lv_meter_scale_t *needle_scale = NULL;
	_meter_build_one(md, meter_parent, false, &m, &scale, &needle, &needle_scale,
	                 &md->needle_img_dsc, &md->bg_img_dsc);
	if (!m) {
		ESP_LOGE(TAG, "_meter_create: lv_meter_create failed");
		if (cont) lv_obj_del(cont);
		return;
	}
	lv_obj_set_size(m, (lv_coord_t)w->w, (lv_coord_t)w->h);
	if (!cont) {
		lv_obj_set_align(m, LV_ALIGN_CENTER);
		lv_obj_set_pos(m, w->x, w->y);
	} else {
		lv_obj_set_align(m, LV_ALIGN_CENTER);
	}
	md->meter = m;
	md->scale = scale;
	md->needle = needle;
	md->needle_scale = needle_scale;

	/* Night meter build is deferred to _meter_apply_night_mode — see the
	 * _meter_build_night_lazy helper below. Keeping two lv_meter objects
	 * in the tree (even with the night one hidden) was forcing LVGL to
	 * walk + skip the hidden meter every refresh; when adjacent widgets'
	 * dirty rects landed on the container, the extra bookkeeping dragged
	 * through the redraw pipeline. Deferring the build means layouts
	 * that never engage night mode pay zero overhead for the override
	 * config being present. */

	w->root = cont ? cont : m;

	/* Subscribe to signal if bound */
	if (md->signal_index >= 0)
		signal_subscribe(md->signal_index, _meter_on_signal, w);

	/* Subscribe rules (safe no-op if no rules defined) */
	widget_rules_subscribe(w);

	/* Subscribe to night-mode changes if any night override is set. */
	if (md->night.has_minor_tick_color  || md->night.has_major_tick_color ||
	    md->night.has_needle_color      || md->night.has_needle_ball_color ||
	    md->night.has_border_color      || md->night.has_meter_bg_color ||
	    md->night.has_tick_label_color  || md->night.has_needle_image_name ||
	    md->night.has_bg_image_name) {
		night_mode_subscribe(_meter_night_cb, w);
		_meter_apply_night_mode(w, night_mode_is_active());
	}

	ESP_LOGD(TAG, "_meter_create: DONE (night_meter=%p)", (void *)md->night_meter);
}

static void _meter_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, (lv_coord_t)nw, (lv_coord_t)nh);
	/* If root is the container, also resize the day meter (child of container).
	 * If root *is* the day meter, the size set above already covered it. */
	if (md && md->meter && lv_obj_is_valid(md->meter) && md->meter != w->root)
		lv_obj_set_size(md->meter, (lv_coord_t)nw, (lv_coord_t)nh);
	if (md && md->night_meter && lv_obj_is_valid(md->night_meter))
		lv_obj_set_size(md->night_meter, (lv_coord_t)nw, (lv_coord_t)nh);
	w->w = nw;
	w->h = nh;
}

static void _meter_open_settings(widget_t *w) { (void)w; }

static void _meter_to_json(widget_t *w, cJSON *out) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	widget_base_to_json(w, out);
	if (!md)
		return;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg)
		return;
	cJSON_AddNumberToObject(cfg, "slot", md->value_idx);
	cJSON_AddNumberToObject(cfg, "min", md->min);
	cJSON_AddNumberToObject(cfg, "max", md->max);
	cJSON_AddNumberToObject(cfg, "start_angle", md->start_angle);
	cJSON_AddNumberToObject(cfg, "end_angle", md->end_angle);
	if (md->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", md->signal_name);

	/* Appearance overrides — only serialize non-default values */
	if (md->minor_tick_count != 21)
		cJSON_AddNumberToObject(cfg, "minor_tick_count", md->minor_tick_count);
	if (md->major_tick_every != 5)
		cJSON_AddNumberToObject(cfg, "major_tick_every", md->major_tick_every);
	if (md->minor_tick_width != 2)
		cJSON_AddNumberToObject(cfg, "minor_tick_width", md->minor_tick_width);
	if (md->minor_tick_length != 10)
		cJSON_AddNumberToObject(cfg, "minor_tick_length", md->minor_tick_length);
	if (md->major_tick_width != 4)
		cJSON_AddNumberToObject(cfg, "major_tick_width", md->major_tick_width);
	if (md->major_tick_length != 15)
		cJSON_AddNumberToObject(cfg, "major_tick_length", md->major_tick_length);
	if (md->minor_tick_color.full != lv_palette_main(LV_PALETTE_GREY).full)
		cJSON_AddNumberToObject(cfg, "minor_tick_color", (int)md->minor_tick_color.full);
	if (md->major_tick_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "major_tick_color", (int)md->major_tick_color.full);
	if (md->needle_width != 4)
		cJSON_AddNumberToObject(cfg, "needle_width", md->needle_width);
	if (md->needle_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "needle_color", (int)md->needle_color.full);
	if (md->needle_r_mod != -10)
		cJSON_AddNumberToObject(cfg, "needle_r_mod", md->needle_r_mod);
	if (md->needle_rear_length != 0)
		cJSON_AddNumberToObject(cfg, "needle_rear_length", md->needle_rear_length);
	if (md->anchor_enabled)
		cJSON_AddBoolToObject(cfg, "anchor_enabled", true);
	if (md->anchor_enabled || md->anchor_value != (md->min + md->max) / 2)
		cJSON_AddNumberToObject(cfg, "anchor_value", md->anchor_value);
	if (md->anchor_enabled || md->anchor_position != 50)
		cJSON_AddNumberToObject(cfg, "anchor_position", md->anchor_position);
	if (md->needle_tip_style != 0)
		cJSON_AddNumberToObject(cfg, "needle_tip_style", md->needle_tip_style);
	if (md->needle_tip_base_w != 0)
		cJSON_AddNumberToObject(cfg, "needle_tip_base_w", md->needle_tip_base_w);
	if (md->needle_tip_point_w != 0)
		cJSON_AddNumberToObject(cfg, "needle_tip_point_w", md->needle_tip_point_w);
	if (md->needle_tip_taper != 0)
		cJSON_AddNumberToObject(cfg, "needle_tip_taper", md->needle_tip_taper);
	if (md->needle_ball_size != 10)
		cJSON_AddNumberToObject(cfg, "needle_ball_size", md->needle_ball_size);
	if (md->needle_ball_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "needle_ball_color", (int)md->needle_ball_color.full);
	if (md->needle_image_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "needle_image_name", md->needle_image_name);
	if (md->needle_pivot_x != 0)
		cJSON_AddNumberToObject(cfg, "needle_pivot_x", md->needle_pivot_x);
	if (md->needle_pivot_y != 0)
		cJSON_AddNumberToObject(cfg, "needle_pivot_y", md->needle_pivot_y);
	if (md->needle_angle_offset != 0)
		cJSON_AddNumberToObject(cfg, "needle_angle_offset", md->needle_angle_offset);
	if (md->bg_image_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "bg_image_name", md->bg_image_name);
	if (md->border_width != 0)
		cJSON_AddNumberToObject(cfg, "border_width", md->border_width);
	if (md->border_color.full != lv_color_black().full)
		cJSON_AddNumberToObject(cfg, "border_color", (int)md->border_color.full);
	if (md->border_opa != 255)
		cJSON_AddNumberToObject(cfg, "border_opa", md->border_opa);
	if (md->meter_bg_color.full != lv_color_hex(0x3D3D3D).full)
		cJSON_AddNumberToObject(cfg, "meter_bg_color", (int)md->meter_bg_color.full);
	if (md->meter_bg_opa != 255)
		cJSON_AddNumberToObject(cfg, "meter_bg_opa", md->meter_bg_opa);
	if (md->scale_padding != 0)
		cJSON_AddNumberToObject(cfg, "scale_padding", md->scale_padding);
	if (md->label_gap != 10)
		cJSON_AddNumberToObject(cfg, "label_gap", md->label_gap);
	if (md->tick_label_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "tick_label_font", md->tick_label_font);
	if (md->tick_label_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "tick_label_color", (int)md->tick_label_color.full);
	if (!md->show_ticks)
		cJSON_AddBoolToObject(cfg, "show_ticks", false);
	if (!md->show_tick_labels)
		cJSON_AddBoolToObject(cfg, "show_tick_labels", false);

	/* Rules */
	widget_rules_to_json(w, cfg);

	/* Night-mode overrides — emit only fields that have an override set */
	{
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, md->night, minor_tick_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, major_tick_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, needle_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, needle_ball_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, border_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, meter_bg_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, tick_label_color);
		NIGHT_SERIALIZE_IMAGE(n, md->night, needle_image_name);
		NIGHT_SERIALIZE_IMAGE(n, md->night, bg_image_name);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
	}
}

static void _meter_from_json(widget_t *w, cJSON *in) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!md)
		return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg)
		return;
	cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	cJSON *min_item = cJSON_GetObjectItemCaseSensitive(cfg, "min");
	cJSON *max_item = cJSON_GetObjectItemCaseSensitive(cfg, "max");
	cJSON *sa_item = cJSON_GetObjectItemCaseSensitive(cfg, "start_angle");
	cJSON *ea_item = cJSON_GetObjectItemCaseSensitive(cfg, "end_angle");
	if (cJSON_IsNumber(slot_item)) {
		uint8_t idx = (uint8_t)slot_item->valueint;
		if (idx < 13)
			md->value_idx = idx;
	}
	if (cJSON_IsNumber(min_item))
		md->min = (int32_t)min_item->valueint;
	if (cJSON_IsNumber(max_item))
		md->max = (int32_t)max_item->valueint;
	if (cJSON_IsNumber(sa_item))
		md->start_angle = (int16_t)sa_item->valueint;
	if (cJSON_IsNumber(ea_item))
		md->end_angle = (int16_t)ea_item->valueint;
	cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(sig_item) && sig_item->valuestring) {
		safe_strncpy(md->signal_name, sig_item->valuestring, sizeof(md->signal_name));
	}

	/* Resolve signal name → index */
	if (md->signal_name[0] != '\0')
		md->signal_index = signal_find_by_name(md->signal_name);

	/* Appearance overrides */
	cJSON *ap;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_count");
	if (cJSON_IsNumber(ap)) md->minor_tick_count = (uint8_t)ap->valueint;
	if (md->minor_tick_count < 2) md->minor_tick_count = 2;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_every");
	if (cJSON_IsNumber(ap)) md->major_tick_every = (uint8_t)ap->valueint;
	if (md->major_tick_every < 1) md->major_tick_every = 1;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_width");
	if (cJSON_IsNumber(ap)) md->minor_tick_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_length");
	if (cJSON_IsNumber(ap)) md->minor_tick_length = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_width");
	if (cJSON_IsNumber(ap)) md->major_tick_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_length");
	if (cJSON_IsNumber(ap)) md->major_tick_length = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_color");
	if (cJSON_IsNumber(ap)) md->minor_tick_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_color");
	if (cJSON_IsNumber(ap)) md->major_tick_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_width");
	if (cJSON_IsNumber(ap)) md->needle_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_color");
	if (cJSON_IsNumber(ap)) md->needle_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_r_mod");
	if (cJSON_IsNumber(ap)) md->needle_r_mod = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_rear_length");
	if (cJSON_IsNumber(ap)) md->needle_rear_length = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_enabled");
	if (cJSON_IsBool(ap)) md->anchor_enabled = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_value");
	if (cJSON_IsNumber(ap)) md->anchor_value = (int32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_position");
	if (cJSON_IsNumber(ap)) {
		int v = ap->valueint;
		if (v < 0)   v = 0;
		if (v > 100) v = 100;
		md->anchor_position = (uint8_t)v;
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_tip_style");
	if (cJSON_IsNumber(ap)) md->needle_tip_style = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_tip_base_w");
	if (cJSON_IsNumber(ap)) md->needle_tip_base_w = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_tip_point_w");
	if (cJSON_IsNumber(ap)) md->needle_tip_point_w = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_tip_taper");
	if (cJSON_IsNumber(ap)) md->needle_tip_taper = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_ball_size");
	if (cJSON_IsNumber(ap)) md->needle_ball_size = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_ball_color");
	if (cJSON_IsNumber(ap)) md->needle_ball_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_image_name");
	if (cJSON_IsString(ap) && ap->valuestring) {
		safe_strncpy(md->needle_image_name, ap->valuestring, sizeof(md->needle_image_name));
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_pivot_x");
	if (cJSON_IsNumber(ap)) md->needle_pivot_x = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_pivot_y");
	if (cJSON_IsNumber(ap)) md->needle_pivot_y = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_angle_offset");
	if (cJSON_IsNumber(ap)) md->needle_angle_offset = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "bg_image_name");
	if (cJSON_IsString(ap) && ap->valuestring) {
		safe_strncpy(md->bg_image_name, ap->valuestring, sizeof(md->bg_image_name));
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "border_width");
	if (cJSON_IsNumber(ap)) md->border_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "border_color");
	if (cJSON_IsNumber(ap)) md->border_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "border_opa");
	if (cJSON_IsNumber(ap)) md->border_opa = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "meter_bg_color");
	if (cJSON_IsNumber(ap)) md->meter_bg_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "meter_bg_opa");
	if (cJSON_IsNumber(ap)) md->meter_bg_opa = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "scale_padding");
	if (cJSON_IsNumber(ap)) md->scale_padding = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "label_gap");
	if (cJSON_IsNumber(ap)) md->label_gap = (int8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_font");
	if (cJSON_IsString(ap) && ap->valuestring) {
		safe_strncpy(md->tick_label_font, ap->valuestring, sizeof(md->tick_label_font));
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_color");
	if (cJSON_IsNumber(ap)) md->tick_label_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "show_ticks");
	if (cJSON_IsBool(ap)) md->show_ticks = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "show_tick_labels");
	if (cJSON_IsBool(ap)) md->show_tick_labels = cJSON_IsTrue(ap);

	/* Rules */
	widget_rules_from_json(w, cfg);

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, md->night, minor_tick_color);
		NIGHT_PARSE_COLOR(night, md->night, major_tick_color);
		NIGHT_PARSE_COLOR(night, md->night, needle_color);
		NIGHT_PARSE_COLOR(night, md->night, needle_ball_color);
		NIGHT_PARSE_COLOR(night, md->night, border_color);
		NIGHT_PARSE_COLOR(night, md->night, meter_bg_color);
		NIGHT_PARSE_COLOR(night, md->night, tick_label_color);
		NIGHT_PARSE_IMAGE(night, md->night, needle_image_name);
		NIGHT_PARSE_IMAGE(night, md->night, bg_image_name);
	}
}

static void _meter_destroy(widget_t *w) {
	if (!w)
		return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (md && md->signal_index >= 0)
		signal_unsubscribe(md->signal_index, _meter_on_signal, w);
	night_mode_unsubscribe(_meter_night_cb, w);
	widget_rules_free(w);
	/* Deleting w->root cascades to children (container case kills both day +
	 * night meters). If root is the day meter directly, no night meter exists
	 * (we only wrap when needs_night is true). */
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	if (md) {
		rdm_image_free(md->needle_img_dsc);
		rdm_image_free(md->bg_img_dsc);
		rdm_image_free(md->night_needle_img_dsc);
		rdm_image_free(md->night_bg_img_dsc);
		free(md);
	}
	free(w);
}

uint8_t widget_meter_get_value_idx(const widget_t *w) {
	if (!w || w->type != WIDGET_METER || !w->type_data)
		return 0;
	const meter_data_t *md = (const meter_data_t *)w->type_data;
	return md->value_idx < 13 ? md->value_idx : 0;
}

static void _meter_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !md->meter) return;

	lv_color_t bg = md->meter_bg_color;
	lv_color_t nc = md->needle_color;
	lv_color_t nbc = md->needle_ball_color;
	lv_color_t bc = md->border_color;

	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "meter_bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bg.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "needle_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			nc.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "needle_ball_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			nbc.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "border_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bc.full = (uint16_t)o->value.color;
		}
	}

	/* Apply to whichever meter is currently visible (day or night). When a
	 * night meter exists and is visible, conditional-rule overrides should
	 * affect it, not the hidden day meter. */
	lv_obj_t *target = md->meter;
	if (md->night_meter && lv_obj_is_valid(md->night_meter) &&
	    !lv_obj_has_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN)) {
		target = md->night_meter;
	}
	lv_obj_set_style_bg_color(target, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(target, nbc, LV_PART_INDICATOR);
	if (md->border_width > 0)
		lv_obj_set_style_border_color(target, bc, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Needle color can only be applied to line needles (not image needles).
	 * LVGL v8 doesn't expose a direct API for line needle color after creation,
	 * so we store it for potential future use. */
	(void)nc;
}

/* Lazily build the night meter the first time night mode actually engages.
 * Extracted from _meter_create so we only pay the cost of the second
 * lv_meter when the user toggles into night. The day meter's parent is the
 * shared container (when _meter_needs_night_meter is true) so we just add
 * the sibling and hide it so the caller's visibility swap works as before.
 * No-op if the meter doesn't need a dual-meter pattern, or if the lazy
 * build already happened. */
static void _meter_build_night_lazy(widget_t *w) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !md->meter) return;
	if (md->night_meter) return;                 /* already built */
	if (!_meter_needs_night_meter(md)) return;   /* no baked overrides */

	lv_obj_t *parent = lv_obj_get_parent(md->meter);
	if (!parent) return;

	lv_obj_t *nm = NULL;
	lv_meter_scale_t *nscale = NULL;
	lv_meter_indicator_t *nneedle = NULL;
	lv_meter_scale_t *nneedle_scale = NULL;
	_meter_build_one(md, parent, true, &nm, &nscale, &nneedle, &nneedle_scale,
	                 &md->night_needle_img_dsc, &md->night_bg_img_dsc);
	if (!nm) return;

	lv_coord_t mw = lv_obj_get_width(md->meter);
	lv_coord_t mh = lv_obj_get_height(md->meter);
	lv_obj_set_size(nm, mw, mh);
	lv_obj_set_align(nm, LV_ALIGN_CENTER);
	lv_obj_add_flag(nm, LV_OBJ_FLAG_HIDDEN);
	md->night_meter        = nm;
	md->night_scale        = nscale;
	md->night_needle       = nneedle;
	md->night_needle_scale = nneedle_scale;
}

/* Apply night-mode state. Two paths:
 *   A) night_meter exists (or gets built lazily): a sibling meter was built
 *      with all night values baked in (tick colors, needle color, needle/bg
 *      image). We just toggle visibility — instant swap, no live mutation.
 *   B) no night_meter needed: only runtime-mutable color overrides are
 *      configured (bg/border/ball/tick label). Mutate them on the day meter
 *      directly. */
static void _meter_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !md->meter) return;

	/* Build the sibling meter now if it hasn't been created yet and the
	 * widget needs one. Pays the create cost on the first night activation
	 * (or first call with active=false+has_overrides, which is a no-op
	 * elsewhere), then never again. */
	if (active) _meter_build_night_lazy(w);

	bool has_night_obj = md->night_meter && lv_obj_is_valid(md->night_meter);

	if (has_night_obj) {
		/* Path A: visibility swap. Both meters already have correct colors
		 * baked in for their respective state, so no live mutation needed.
		 *
		 * Since _meter_on_signal only drives the visible meter for perf,
		 * the meter we're about to reveal has a stale needle position.
		 * Catch it up to the current signal value before un-hiding so the
		 * swap is visually seamless. */
		signal_t *sig = (md->signal_index >= 0)
			? signal_get_by_index((uint16_t)md->signal_index) : NULL;
		int32_t v = md->min;
		if (sig && !sig->is_stale) {
			float fv = sig->current_value;
			if (fv < (float)md->min) fv = (float)md->min;
			if (fv > (float)md->max) fv = (float)md->max;
			v = (int32_t)fv;
		}
		v = _meter_apply_curve(md, v);

		if (active) {
			if (md->night_needle)
				lv_meter_set_indicator_value(md->night_meter, md->night_needle, v);
			lv_obj_add_flag(md->meter, LV_OBJ_FLAG_HIDDEN);
			lv_obj_clear_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN);
		} else {
			if (md->needle)
				lv_meter_set_indicator_value(md->meter, md->needle, v);
			lv_obj_clear_flag(md->meter, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN);
		}
		return;
	}

	/* Path B: live mutation on the day meter (no baked night overrides). */
	lv_color_t bg   = NIGHT_PICK_COLOR(active, md->night, meter_bg_color,   md->meter_bg_color);
	lv_color_t bdr  = NIGHT_PICK_COLOR(active, md->night, border_color,     md->border_color);
	lv_color_t nbc  = NIGHT_PICK_COLOR(active, md->night, needle_ball_color, md->needle_ball_color);
	lv_color_t tlc  = NIGHT_PICK_COLOR(active, md->night, tick_label_color, md->tick_label_color);

	lv_obj_set_style_bg_color(md->meter, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (md->border_width > 0) {
		lv_obj_set_style_border_color(md->meter, bdr,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (md->needle_ball_size > 0) {
		lv_obj_set_style_bg_color(md->meter, nbc, LV_PART_INDICATOR);
	}
	lv_obj_set_style_text_color(md->meter, tlc, LV_PART_TICKS);
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _meter_night_cb(bool active, void *user_data) {
	_meter_apply_night_mode((widget_t *)user_data, active);
}

widget_t *widget_meter_create_instance(uint8_t value_idx) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	meter_data_t *md = heap_caps_calloc(1, sizeof(meter_data_t), MALLOC_CAP_SPIRAM);
	if (!md) md = calloc(1, sizeof(meter_data_t));
	if (!md) {
		free(w);
		return NULL;
	}

	md->value_idx = (value_idx < 13) ? value_idx : 0;
	md->min = 0;
	md->max = 100;

	md->start_angle = 135;
	md->end_angle = 45;
	md->meter = NULL;
	md->scale = NULL;
	md->needle = NULL;
	md->signal_index = -1;

	/* Tick defaults */
	md->minor_tick_count = 21;
	md->major_tick_every = 5;
	md->minor_tick_width = 2;
	md->minor_tick_length = 10;
	md->major_tick_width = 4;
	md->major_tick_length = 15;
	md->minor_tick_color = lv_palette_main(LV_PALETTE_GREY);
	md->major_tick_color = lv_color_white();
	/* Needle defaults */
	md->needle_width = 4;
	md->needle_color = lv_color_white();
	md->needle_r_mod = -10;
	md->needle_rear_length = 0;
	md->anchor_value    = 50;     /* midpoint of default 0..100 range */
	md->anchor_position = 50;
	md->anchor_enabled  = false;  /* off by default — pure linear */
	md->needle_tip_style   = 0;
	md->needle_tip_base_w  = 0;
	md->needle_tip_point_w = 0;
	md->needle_tip_taper   = 0;
	md->needle_ball_size = 10;
	md->needle_ball_color = lv_color_white();
	/* Tick label defaults */
	md->tick_label_color = lv_color_white();
	md->show_ticks = true;
	md->show_tick_labels = true;
	/* Border defaults */
	md->border_color = lv_color_black();
	md->border_width = 0;
	md->border_opa = 255;
	/* Background defaults */
	md->meter_bg_color = lv_color_hex(0x3D3D3D);
	md->meter_bg_opa = 255;
	/* Scale layout defaults */
	md->scale_padding = 0;
	md->label_gap = 10;

	w->type = WIDGET_METER;
	w->slot = md->value_idx;
	w->x = 0;
	w->y = 0;
	w->w = METER_DEFAULT_W;
	w->h = METER_DEFAULT_H;
	w->type_data = md;
	snprintf(w->id, sizeof(w->id), "meter_%u", md->value_idx);

	w->create = _meter_create;
	w->resize = _meter_resize;
	w->open_settings = _meter_open_settings;
	w->to_json = _meter_to_json;
	w->from_json = _meter_from_json;
	w->destroy = _meter_destroy;
	w->apply_overrides = _meter_apply_overrides;
	w->apply_night_mode = _meter_apply_night_mode;

	return w;
}
