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
#include <math.h>
#include "signal.h"
#include "esp_log.h"
#include "lvgl.h"
#include "extra/others/snapshot/lv_snapshot.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widget_types.h"
#include "data/channel_manager.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_meter";

#define METER_DEFAULT_W 140
#define METER_DEFAULT_H 140

/* Invalidate just the bounding box swept by the rear extension at one value.
 * Mirrors LVGL's built-in inv_line for the front needle, but for the segment
 * BEHIND the pivot (which LVGL doesn't know about). Called for both the old
 * and the new value so partial-refresh clears stale rear pixels and paints
 * the new ones — without invalidating the whole meter every signal tick. */
static void _meter_inv_rear(lv_obj_t *m, lv_meter_scale_t *scale,
                             int32_t value, uint8_t rear_len, uint8_t width) {
	if (!m || !scale || rear_len == 0) return;

	lv_area_t scale_area;
	lv_obj_get_content_coords(m, &scale_area);
	lv_coord_t r_edge = LV_MIN(lv_area_get_width(&scale_area),
	                            lv_area_get_height(&scale_area)) / 2;
	lv_point_t scale_center;
	scale_center.x = scale_area.x1 + r_edge;
	scale_center.y = scale_area.y1 + r_edge;

	int32_t angle = lv_map(value, scale->min, scale->max,
	                        scale->rotation, scale->rotation + scale->angle_range);

	/* Rear endpoint: opposite direction of the needle, distance = rear_len. */
	lv_point_t p_rear;
	p_rear.x = scale_center.x - (lv_trigo_cos(angle) * (int32_t)rear_len) / LV_TRIGO_SIN_MAX;
	p_rear.y = scale_center.y - (lv_trigo_sin(angle) * (int32_t)rear_len) / LV_TRIGO_SIN_MAX;

	int32_t pad = (int32_t)width / 2 + 2;
	lv_area_t a;
	a.x1 = LV_MIN(scale_center.x, p_rear.x) - pad;
	a.y1 = LV_MIN(scale_center.y, p_rear.y) - pad;
	a.x2 = LV_MAX(scale_center.x, p_rear.x) + pad;
	a.y2 = LV_MAX(scale_center.y, p_rear.y) + pad;
	lv_obj_invalidate_area(m, &a);
}

/* Invalidate the bbox the shadow needle covers at `value`. Mirrors what LVGL's
 * inv_line does for a needle_line indicator, but shifted by (offset_x,
 * offset_y) and padded for the extra shadow width + rear extension. Without
 * this, partial-refresh leaves shadow trails because LVGL's built-in
 * invalidation for the shadow indicator only covers the unshifted bbox. */
static void _meter_inv_shadow(lv_obj_t *m, lv_meter_scale_t *scale,
                               int32_t value, const meter_data_t *md) {
	if (!m || !scale || !md) return;

	lv_area_t scale_area;
	lv_obj_get_content_coords(m, &scale_area);
	lv_coord_t r_edge = LV_MIN(lv_area_get_width(&scale_area),
	                            lv_area_get_height(&scale_area)) / 2;
	lv_point_t scale_center;
	scale_center.x = scale_area.x1 + r_edge;
	scale_center.y = scale_area.y1 + r_edge;

	int32_t r_out = r_edge + scale->r_mod + md->needle_r_mod;
	int32_t angle = lv_map(value, scale->min, scale->max,
	                        scale->rotation, scale->rotation + scale->angle_range);
	lv_point_t p_end;
	p_end.x = (lv_trigo_cos(angle) * r_out) / LV_TRIGO_SIN_MAX + scale_center.x;
	p_end.y = (lv_trigo_sin(angle) * r_out) / LV_TRIGO_SIN_MAX + scale_center.y;

	/* Rear endpoint is bounded by scale_center ± rear_len in the bbox
	 * calc below (no need to compute the exact direction — over-
	 * invalidation by a few pixels is harmless). */

	int32_t width = (int32_t)md->needle_width + (int32_t)md->shadow_width_extra;
	int32_t pad   = width / 2 + 2;

	/* Apply the same dynamic-mode scaling the draw path uses, so the dirty
	 * rect tracks where the shadow is ACTUALLY drawn. Without this match,
	 * dynamic mode would over-invalidate (offset_x/y full magnitude) while
	 * the visible shadow has collapsed near the vertical — wasted redraw
	 * work, but harmless. Same scale math as _meter_draw_shadow_needle. */
	int32_t ox = md->shadow_offset_x;
	int32_t oy = md->shadow_offset_y;
	if (md->shadow_dynamic) {
		int32_t dx = p_end.x - scale_center.x;
		int32_t dy = p_end.y - scale_center.y;
		uint32_t lsq = (uint32_t)(dx * dx + dy * dy);
		if (lsq > 0) {
			lv_sqrt_res_t sres;
			lv_sqrt(lsq, &sres, 0x800);
			int32_t l = sres.i;
			if (l > 0) {
				int32_t abs_dx = dx < 0 ? -dx : dx;
				int32_t scale_q8 = (abs_dx * 256) / l;
				ox = (ox * scale_q8) / 256;
				oy = (oy * scale_q8) / 256;
			} else {
				ox = 0; oy = 0;
			}
		} else {
			ox = 0; oy = 0;
		}
	}

	/* Pivot-anchored bbox: pivot stays at scale_center; only the tip is
	 * displaced by (ox, oy). The rear extends BEHIND the pivot in the
	 * shadow direction by up to rear_len — bound it loosely by padding
	 * the pivot side by rear_len (over-invalidation is harmless; missed
	 * pixels would leave trails). */
	lv_coord_t tip_x = (lv_coord_t)(p_end.x + ox);
	lv_coord_t tip_y = (lv_coord_t)(p_end.y + oy);
	int32_t rear_pad = (int32_t)md->needle_rear_length;
	lv_area_t a;
	a.x1 = LV_MIN(scale_center.x - rear_pad, tip_x) - pad;
	a.y1 = LV_MIN(scale_center.y - rear_pad, tip_y) - pad;
	a.x2 = LV_MAX(scale_center.x + rear_pad, tip_x) + pad;
	a.y2 = LV_MAX(scale_center.y + rear_pad, tip_y) + pad;
	lv_obj_invalidate_area(m, &a);
}

/* Forward decl: _meter_apply_channel re-subscribes this callback when a
 * channel's source index changes; the definition lives further down. */
static void _meter_on_signal(float value, bool is_stale, void *user_data);

/* ── v14 Channel binding ─────────────────────────────────────────────
 * When md->channel_id is non-empty, the meter pulls signal_name, min,
 * max, redline_threshold, and redline_color from the bound channel.
 * The widget continues to use its own LIVE FIELDS at render time
 * (md->min, md->max, etc.) — _meter_apply_channel() just copies values
 * from channel into widget fields before signal subscription happens.
 *
 * Per-widget overrides for these fields are stored as md->X_override
 * (Phase 2 work; for v1 the channel value always wins when bound).
 *
 * On channel-changed events (user edits in Channels tab), we re-apply
 * and invalidate. */
static void _meter_apply_channel(widget_t *w) {
	if (!w) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md) return;
	channel_t *c = (channel_t *)md->channel;
	if (!c) return;

	/* Pull data semantics from the channel. */
	safe_strncpy(md->signal_name, c->signal_name, sizeof(md->signal_name));
	/* Re-point the live signal subscription when the channel's source index
	 * changes. The signal registry keys subscribers by the index they attached
	 * to, so a bare index swap would leave _meter_on_signal bound to the OLD
	 * signal and the needle would freeze when a channel source is re-bound at
	 * runtime. Mirror the inspector rebind path (unsubscribe old → subscribe
	 * new); safe here because channel-changed runs under the LVGL mutex.
	 * Guarded on w->root: pre-create (from_json) this just records the index
	 * and lets _meter_create do the single create-time subscribe — re-subscribe
	 * only matters once the widget is live, and signal_subscribe does not dedupe
	 * so subscribing here too would double-bind the callback. */
	int16_t new_idx = c->signal_index;
	if (w->root && new_idx != md->signal_index) {
		if (md->signal_index >= 0)
			signal_unsubscribe(md->signal_index, _meter_on_signal, w);
		md->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _meter_on_signal, w);
	} else {
		md->signal_index = new_idx;
	}
	md->min = c->min;
	md->max = c->max;
	/* Decimals drive the integer LVGL scale factor: a 0.0..2.0 boost channel
	 * with decimals=1 maps to an internal 0..20 meter scale, giving 21 needle
	 * steps instead of 3. Tick labels divide back by value_scale. */
	md->value_decimals = (c->decimals > 3) ? 3 : c->decimals;
	md->value_scale = 1;
	for (uint8_t k = 0; k < md->value_decimals; k++) md->value_scale *= 10;
	/* Channel high_warn → widget redline (the meter's "redline" concept
	 * maps to "danger zone above the high threshold"). */
	if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
		md->redline_enabled = true;
		md->redline_threshold = c->high_warn;
	} else {
		md->redline_enabled = false;
	}
	/* Redline colour is widget-owned (Widget settings) — never driven by the
	 * channel. Only threshold/range/signal sync from the channel. */

	/* Range / decimals / signal just changed, so the value→needle-integer
	 * mapping changed too — invalidate the paint memo so the next signal
	 * tick repaints instead of being gated against a now-stale memo. */
	md->_last_needle_valid = false;
}

static void _meter_on_channel_changed(channel_t *c, void *user_data) {
	(void)c;
	widget_t *w = (widget_t *)user_data;
	if (!w) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md) return;

	_meter_apply_channel(w);

	/* Threshold/range edits invalidate the entire meter (cheap relative
	 * to a user edit — happens at human speed, not per-frame). */
	if (md->meter && lv_obj_is_valid(md->meter)) lv_obj_invalidate(md->meter);
	if (md->night_meter && lv_obj_is_valid(md->night_meter))
		lv_obj_invalidate(md->night_meter);
}

/* Inverse of _meter_apply_anchor. Given an angular position 0..100% along
 * the sweep, return the data value the user should see at that point.
 * Used to relabel major tick texts so the numbers shown match the warped
 * needle position. (Linear pass-through when anchor is disabled.) */
static float _meter_value_for_angle_pct(const meter_data_t *md, int32_t pct) {
	if (md->max <= md->min) return md->min;
	if (pct <= 0)   return md->min;
	if (pct >= 100) return md->max;
	if (!md->anchor_enabled) {
		return md->min + (md->max - md->min) * (float)pct / 100.0f;
	}
	int32_t ap = md->anchor_position;
	if (ap < 0)   ap = 0;
	if (ap > 100) ap = 100;
	if (pct <= ap) {
		if (ap == 0) return md->min;
		return md->min + (md->anchor_value - md->min) * (float)pct / (float)ap;
	}
	int32_t hp = 100 - ap;
	if (hp == 0) return md->max;
	return md->anchor_value + (md->max - md->anchor_value)
	       * (float)(pct - ap) / (float)hp;
}

/* Apply the anchor curve to a clamped raw value, returning the value to feed
 * to lv_meter_set_indicator_value on a linear scale spanning [min, max].
 * Maps raw `v` so that `anchor_value` lands at `anchor_position`% of the
 * sweep — two linear segments. No-op when anchor_enabled is false. */
static float _meter_apply_anchor(const meter_data_t *md, float v) {
	if (!md->anchor_enabled) return v;
	float lo = md->min;
	float hi = md->max;
	if (hi <= lo) return v;
	float a = md->anchor_value;
	if (a <= lo || a >= hi) return v;
	int32_t pos = md->anchor_position;
	if (pos <= 0)   pos = 0;
	if (pos >= 100) pos = 100;
	float range  = hi - lo;
	float pivot  = lo + (range * (float)pos) / 100.0f;
	if (v <= a) {
		/* Map [lo, a] -> [lo, pivot] linearly. */
		float span = a - lo;
		if (span <= 0.0f) return lo;
		return lo + (v - lo) * (pivot - lo) / span;
	} else {
		/* Map [a, hi] -> [pivot, hi] linearly. */
		float span = hi - a;
		if (span <= 0.0f) return hi;
		return pivot + (v - a) * (hi - pivot) / span;
	}
}

static void _meter_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !w->root || !lv_obj_is_valid(w->root)) return;
	if (!md->meter || !md->needle) return;
	/* Work in float display units, then scale to the meter's integer LVGL
	 * domain (value_scale = 10^decimals) at the very end so fractional ranges
	 * keep a smooth needle. */
	float fv;
	if (is_stale) {
		fv = md->min;
	} else {
		fv = value;
		if (fv < md->min) fv = md->min;
		if (fv > md->max) fv = md->max;
	}
	fv = _meter_apply_anchor(md, fv);
	if (md->reverse) fv = md->max + md->min - fv;
	int32_t v = lroundf(fv * (float)md->value_scale);
	/* Value-gate: suppress redundant repaints when the integer needle value
	 * hasn't moved. This is the only ticking widget without a value-gate, so
	 * the sim's continuous triangle-wave sweep used to repaint the (large,
	 * centerpiece) meter every render window even when v was unchanged. Real
	 * CAN holds steady decoded values, so this also equalises sim vs CAN.
	 * The stale path forces fv=md->min, which changes v and flows through.
	 * The memo starts invalid (calloc) so the first post-create paint runs,
	 * and is reset to invalid on every rebuild / forced-repaint path. */
	if (md->_last_needle_valid && md->_last_needle_v == v) return;
	md->_last_needle_v = v;
	md->_last_needle_valid = true;
	/* Only drive whichever meter is currently visible. Updating the hidden
	 * sibling costs an LVGL indicator recompute + invalidation-mark that
	 * nobody ever sees — with meters bound to fast-moving sim signals
	 * (RPM etc.) this is a measurable chunk of the per-frame budget.
	 * When night mode toggles, _meter_apply_night_mode immediately pushes
	 * the current value into the newly-visible meter so the swap stays
	 * continuous. */
	/* Rear extension lives outside LVGL's inv_line bbox (center→tip), so on
	 * partial-refresh the rear pixels go stale. Snapshot the indicator's
	 * current end_value before set, then invalidate the rear bboxes for the
	 * old AND new value — same pattern LVGL uses for the front line. Tight
	 * bbox = doesn't tank framerate the way invalidating the whole meter
	 * does on fast signals. */
	if (md->night_meter && md->night_needle && lv_obj_is_valid(md->night_meter) &&
	    !lv_obj_has_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN)) {
		int32_t old_v = md->night_needle->end_value;
		lv_meter_set_indicator_value(md->night_meter, md->night_needle, v);
		if (md->needle_rear_length > 0) {
			_meter_inv_rear(md->night_meter, md->night_scale, old_v,
			                md->needle_rear_length, md->needle_width);
			_meter_inv_rear(md->night_meter, md->night_scale, v,
			                md->needle_rear_length, md->needle_width);
		}
		if (md->shadow_enabled && md->night_shadow_needle) {
			lv_meter_set_indicator_value(md->night_meter, md->night_shadow_needle, v);
			_meter_inv_shadow(md->night_meter, md->night_scale, old_v, md);
			_meter_inv_shadow(md->night_meter, md->night_scale, v,     md);
		}
	} else if (md->meter && md->needle &&
	           !lv_obj_has_flag(md->meter, LV_OBJ_FLAG_HIDDEN)) {
		int32_t old_v = md->needle->end_value;
		lv_meter_set_indicator_value(md->meter, md->needle, v);
		if (md->needle_rear_length > 0) {
			_meter_inv_rear(md->meter, md->scale, old_v,
			                md->needle_rear_length, md->needle_width);
			_meter_inv_rear(md->meter, md->scale, v,
			                md->needle_rear_length, md->needle_width);
		}
		if (md->shadow_enabled && md->shadow_needle) {
			lv_meter_set_indicator_value(md->meter, md->shadow_needle, v);
			_meter_inv_shadow(md->meter, md->scale, old_v, md);
			_meter_inv_shadow(md->meter, md->scale, v,     md);
		}
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

/* Drop-shadow renderer. Drawn from inside DRAW_PART_BEGIN of the shadow
 * indicator (before the main needle), so it lands UNDER the main needle in
 * z-order. Mirrors the same tip-style + rear-extension geometry the main
 * needle uses, just shifted by (shadow_offset_x, shadow_offset_y), in
 * shadow_color, with shadow_opa and a width of needle_width +
 * shadow_width_extra. After this returns the caller sets the LVGL line's
 * own opa to TRANSP so the original-position line draw is suppressed. */
static void _meter_draw_shadow_needle(lv_draw_ctx_t *ctx, meter_data_t *md,
                                       const lv_point_t *orig_p1,
                                       const lv_point_t *orig_p2,
                                       lv_opa_t master_opa) {
	if (!ctx || !md) return;
	if (master_opa <= LV_OPA_MIN) return;

	/* Compute needle direction vector & length up front. Used both for the
	 * dynamic-offset scaling and for the polygon/rear geometry below. */
	int32_t raw_dx = orig_p2->x - orig_p1->x;
	int32_t raw_dy = orig_p2->y - orig_p1->y;
	uint32_t raw_len_sq = (uint32_t)(raw_dx * raw_dx + raw_dy * raw_dy);
	int32_t raw_len = 0;
	if (raw_len_sq > 0) {
		lv_sqrt_res_t sres;
		lv_sqrt(raw_len_sq, &sres, 0x800);
		raw_len = sres.i;
	}

	/* Dynamic mode: scale the user's configured offsets by |dx|/len so the
	 * shadow fades out as the needle approaches vertical (12 or 6 o'clock).
	 * Integer math in Q8 — no float, no libm. At needle horizontal:
	 *   |dx|/len = 1.0 → full offset.
	 * At needle vertical:
	 *   |dx|/len = 0   → zero offset → shadow draws on top of the needle
	 *                    and disappears. */
	int32_t ox = md->shadow_offset_x;
	int32_t oy = md->shadow_offset_y;
	if (md->shadow_dynamic && raw_len > 0) {
		int32_t abs_dx = raw_dx < 0 ? -raw_dx : raw_dx;
		int32_t scale_q8 = (abs_dx * 256) / raw_len;   /* 0..256 */
		ox = (ox * scale_q8) / 256;
		oy = (oy * scale_q8) / 256;
	}

	/* Pivot-anchored geometry: shadow's BASE stays at the needle's base
	 * (scale_center) and only the TIP is offset. Visually the shadow fans
	 * out from the same pivot — same point of contact with the dial face,
	 * like a real cast shadow on a flat surface. Translating both p1 AND
	 * p2 the same way (older code) made the shadow appear to lift off the
	 * pivot, which read as "off" on horizontal needles. */
	lv_point_t p1 = *orig_p1;
	lv_point_t p2 = { (lv_coord_t)(orig_p2->x + ox), (lv_coord_t)(orig_p2->y + oy) };

	int32_t width  = (int32_t)md->needle_width + (int32_t)md->shadow_width_extra;
	if (width < 1) width = 1;
	uint8_t style    = md->needle_tip_style;
	uint8_t rear_len = md->needle_rear_length;
	lv_color_t color = md->shadow_color;
	/* master_opa carries the recursive parent opacity (boot fade-in etc.) —
	 * these draws bypass lv_obj_init_draw_*_dsc so it must be applied here. */
	lv_opa_t   opa   = master_opa < LV_OPA_MAX
	                   ? (lv_opa_t)(((uint32_t)md->shadow_opa * master_opa) >> 8)
	                   : md->shadow_opa;

	/* Recompute the shadow direction vector + length. With pivot anchored
	 * and only the tip offset, the shadow's axis is slightly skewed from
	 * the main needle (which is exactly the visual effect we want — a fan).
	 * The polygon-tip and rear-extension helpers below build geometry
	 * along this shadow vector. */
	int32_t dx = p2.x - p1.x;
	int32_t dy = p2.y - p1.y;
	int32_t len;
	if (dx == raw_dx && dy == raw_dy) {
		len = raw_len;   /* dynamic zeroed the offset — reuse precomputed length */
	} else {
		uint32_t lsq = (uint32_t)(dx * dx + dy * dy);
		if (lsq == 0) { len = 0; }
		else {
			lv_sqrt_res_t sres;
			lv_sqrt(lsq, &sres, 0x800);
			len = sres.i;
		}
	}

	lv_draw_line_dsc_t sline;
	lv_draw_line_dsc_init(&sline);
	sline.color = color;
	sline.opa   = opa;
	sline.width = (lv_coord_t)width;
	if (style == 1) {
		sline.round_start = 1;
		sline.round_end   = 1;
	}

	/* Flat / Rounded — single line draw, optionally extending behind pivot
	 * for rear. Matches the BEGIN-time p1-mutation path on the main needle. */
	if (style <= 1) {
		lv_point_t draw_p1 = p1;
		if (rear_len > 0 && len > 0) {
			draw_p1.x = (lv_coord_t)(p1.x - (dx * (int32_t)rear_len) / len);
			draw_p1.y = (lv_coord_t)(p1.y - (dy * (int32_t)rear_len) / len);
		}
		lv_draw_line(ctx, &sline, &draw_p1, &p2);
		return;
	}

	/* Polygon styles 2-5 — duplicate the same geometry the main END block
	 * builds, in shadow color/opa. */
	if (len == 0) return;
	lv_draw_rect_dsc_t rdsc;
	lv_draw_rect_dsc_init(&rdsc);
	rdsc.bg_color     = color;
	rdsc.bg_opa       = opa;
	rdsc.border_width = 0;

	lv_point_t pts[6];
	uint16_t   npts = 0;
	int32_t ov_base  = md->needle_tip_base_w;
	int32_t ov_point = md->needle_tip_point_w;
	int32_t ov_taper = md->needle_tip_taper;
	if (ov_taper > 100) ov_taper = 100;

	switch (style) {
	case 2: {
		int32_t half_w  = ov_base  > 0 ? ov_base  : (width / 2 + 1);
		int32_t shaft   = ov_taper > 0 ? ov_taper : 90;
		int32_t cap_w   = ov_point;
		pts[0] = _tip_pt(&p1, dx, dy, len, 0,     100,  half_w);
		pts[1] = _tip_pt(&p1, dx, dy, len, shaft, 100,  half_w);
		if (cap_w > 0) {
			pts[2] = _tip_pt(&p1, dx, dy, len, 100, 100,  cap_w);
			pts[3] = _tip_pt(&p1, dx, dy, len, 100, 100, -cap_w);
			pts[4] = _tip_pt(&p1, dx, dy, len, shaft, 100, -half_w);
			pts[5] = _tip_pt(&p1, dx, dy, len, 0,     100, -half_w);
			npts = 6;
		} else {
			pts[2] = p2;
			pts[3] = _tip_pt(&p1, dx, dy, len, shaft, 100, -half_w);
			pts[4] = _tip_pt(&p1, dx, dy, len, 0,     100, -half_w);
			npts = 5;
		}
		break;
	}
	case 3: {
		int32_t half_w = ov_base > 0 ? ov_base : (width / 2 + 2);
		if (ov_point > 0 || (ov_taper > 0 && ov_taper < 100)) {
			int32_t cap_pos = ov_taper > 0 ? ov_taper : 97;
			int32_t cap_w   = ov_point > 0 ? ov_point : 1;
			pts[0] = _tip_pt(&p1, dx, dy, len, 0,       100,  half_w);
			pts[1] = _tip_pt(&p1, dx, dy, len, cap_pos, 100,  cap_w);
			pts[2] = _tip_pt(&p1, dx, dy, len, cap_pos, 100, -cap_w);
			pts[3] = _tip_pt(&p1, dx, dy, len, 0,       100, -half_w);
			npts = 4;
		} else {
			pts[0] = _tip_pt(&p1, dx, dy, len, 0, 1,  half_w);
			pts[1] = p2;
			pts[2] = _tip_pt(&p1, dx, dy, len, 0, 1, -half_w);
			npts = 3;
		}
		break;
	}
	case 4: {
		int32_t half_w  = ov_base  > 0 ? ov_base  : (width / 2 + 2);
		int32_t cap_pos = ov_taper > 0 ? ov_taper : 97;
		int32_t cap_w   = ov_point > 0 ? ov_point : 1;
		pts[0] = _tip_pt(&p1, dx, dy, len, 0,       100,  half_w);
		pts[1] = _tip_pt(&p1, dx, dy, len, cap_pos, 100,  cap_w);
		pts[2] = _tip_pt(&p1, dx, dy, len, cap_pos, 100, -cap_w);
		pts[3] = _tip_pt(&p1, dx, dy, len, 0,       100, -half_w);
		npts = 4;
		break;
	}
	case 5: {
		int32_t half_w  = ov_base  > 0 ? ov_base  : (width / 2 + 3);
		int32_t mid_pos = ov_taper > 0 ? ov_taper : 50;
		pts[0] = p1;
		pts[1] = _tip_pt(&p1, dx, dy, len, mid_pos, 100,  half_w);
		pts[2] = p2;
		pts[3] = _tip_pt(&p1, dx, dy, len, mid_pos, 100, -half_w);
		npts = 4;
		break;
	}
	default: break;
	}
	if (npts > 0) lv_draw_polygon(ctx, &rdsc, pts, npts);

	/* Polygon rear — same opa/width as the polygon body. */
	if (rear_len > 0) {
		int32_t rx = p1.x - (dx * (int32_t)rear_len) / len;
		int32_t ry = p1.y - (dy * (int32_t)rear_len) / len;
		lv_point_t rear_start = { (lv_coord_t)rx, (lv_coord_t)ry };
		lv_draw_line(ctx, &sline, &rear_start, &p1);
	}
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

	meter_data_t *md = (meter_data_t *)lv_event_get_user_data(e);
	if (!md) return;

	/* Tell LVGL how much extra draw area we need outside the meter's rect.
	 * The needle rear extends backwards from the pivot; without this hook
	 * LVGL's clip_area is the meter's bounds, so the rear gets cut off at
	 * the edges. Reporting `rear_length + line width` here makes LVGL
	 * expand the clip region (up to the parent's bounds) so the rear can
	 * draw past the meter without being chopped. */
	if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
		lv_coord_t pad = 0;
		if (md->needle_rear_length > 0) {
			pad = (lv_coord_t)md->needle_rear_length +
			      (lv_coord_t)(md->needle_width / 2 + 2);
		}
		/* Shadow may render outside the meter rect by (offset, width_extra).
		 * Pad the ext_draw_size so lv_obj_invalidate_area can reach those
		 * pixels — otherwise the shifted dirty rect clips at the meter edge
		 * and the shadow lingers when the needle sweeps off the side. */
		if (md->shadow_enabled) {
			int32_t ax = md->shadow_offset_x < 0 ? -md->shadow_offset_x : md->shadow_offset_x;
			int32_t ay = md->shadow_offset_y < 0 ? -md->shadow_offset_y : md->shadow_offset_y;
			lv_coord_t shadow_pad = (lv_coord_t)(LV_MAX(ax, ay) +
			                                     md->needle_width / 2 +
			                                     md->shadow_width_extra + 2);
			if (shadow_pad > pad) pad = shadow_pad;
		}
		if (pad > 0) lv_event_set_ext_draw_size(e, pad);
		return;
	}

	if (code != LV_EVENT_DRAW_PART_BEGIN && code != LV_EVENT_DRAW_PART_END) return;

	lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
	if (!dsc) return;

	/* Tick part — two BEGIN-time mutations:
	 *   1. Anchor relabel: rewrite major-tick label text in place so the
	 *      numbers shown align with the warped needle. LVGL gives us a
	 *      small mutable buffer in dsc->text and re-measures/draws using
	 *      the new content.
	 *   2. Redline recolor: paint tick line + label with redline_color
	 *      when the tick's *displayed* value crosses redline_threshold.
	 *      Using the warped value (not dsc->value, which is linear) is
	 *      what makes the redline ticks visually match the threshold —
	 *      e.g. with anchor on, the tick whose label says "105" recolors
	 *      when threshold=105, not the tick whose linear value is 105
	 *      (which displays a different number under the curve).
	 * Only fires on BEGIN — END is post-draw, dsc fields are stale. */
	if (dsc->type == LV_METER_DRAW_PART_TICK) {
		if (code != LV_EVENT_DRAW_PART_BEGIN) return;

		/* Pick the value to display at this tick (and to test against the
		 * redline threshold). When neither anchor nor reverse is active we
		 * trust LVGL's linear dsc->value. With either flag on we recompute
		 * from the tick's angle position: convert id→pct, mirror pct under
		 * reverse, then run through _meter_value_for_angle_pct (which
		 * handles the anchor curve, falling back to linear pass-through
		 * when anchor is off). */
		/* shown_display is the value in REAL display units. LVGL's dsc->value
		 * lives in the scaled integer domain (scale range = min*value_scale),
		 * so divide it back; the anchor/reverse relabel path already returns
		 * display units. */
		bool needs_relabel = md->anchor_enabled || md->reverse;
		float shown_display;
		if (needs_relabel) {
			uint16_t total = (md->minor_tick_count > 1)
			                 ? (uint16_t)(md->minor_tick_count - 1) : 1;
			int32_t pct = (int32_t)(((int64_t)dsc->id * 100) / (int64_t)total);
			if (md->reverse) pct = 100 - pct;
			shown_display = _meter_value_for_angle_pct(md, pct);
		} else {
			shown_display = (float)dsc->value / (float)md->value_scale;
		}
		/* Compose the label text. Rewrite when:
		 *   - the scale carries decimals (value_scale > 1), OR
		 *   - anchor/reverse warped the linear LVGL value, OR
		 *   - tick_label_divisor scales the display (e.g. 1000 → "6"
		 *     instead of "6000" on a tach).
		 * Tick recolor below uses shown_display because redline_threshold is
		 * in real value space. */
		uint16_t div = md->tick_label_divisor > 0 ? md->tick_label_divisor : 1;
		if (dsc->label_dsc != NULL && dsc->text != NULL) {
			if (md->value_scale > 1) {
				/* Fractional scale → print the real value with its decimals. */
				lv_snprintf(dsc->text, 16, "%.*f", md->value_decimals,
				            shown_display / (float)div);
			} else if (needs_relabel || div > 1) {
				int32_t iv = (int32_t)lroundf(shown_display);
				int32_t display_v = (div > 1) ? (iv / (int32_t)div) : iv;
				lv_snprintf(dsc->text, 16, "%d", (int)display_v);
			}
		}
		if (md->redline_enabled && md->redline_recolor_ticks &&
		    shown_display >= md->redline_threshold) {
			if (dsc->line_dsc)  dsc->line_dsc->color  = md->redline_color;
			if (dsc->label_dsc) dsc->label_dsc->color = md->redline_color;
		}
		return;
	}

	if (dsc->type != LV_METER_DRAW_PART_NEEDLE_LINE) return;
	if (dsc->p1 == NULL || dsc->p2 == NULL || dsc->line_dsc == NULL) return;

	/* Recursive parent opacity (boot fade-in, future widget fades). LVGL
	 * already factored it into line_dsc, but the tip polygon / rear / shadow
	 * draws below build their own dscs from scratch and would otherwise pop
	 * in at full opacity while the rest of the meter fades. */
	lv_opa_t master_opa =
		lv_obj_get_style_opa_recursive(lv_event_get_target(e), LV_PART_MAIN);

	/* Shadow needle path. The shadow is a real lv_meter indicator added in
	 * front of the main needle in the linked list (drawn first by READ_BACK
	 * iteration), so LVGL fires DRAW_PART_NEEDLE_LINE for it before the main
	 * needle. We draw the offset shadow ourselves at BEGIN and then hide
	 * LVGL's own line-at-origin draw via line_dsc->opa. END is a no-op for
	 * the shadow — the main needle's BEGIN/END pair runs after this and
	 * uses its own statics, so no cross-talk. */
	bool is_shadow = md->shadow_enabled &&
	                 ((dsc->sub_part_ptr == md->shadow_needle && md->shadow_needle != NULL) ||
	                  (dsc->sub_part_ptr == md->night_shadow_needle && md->night_shadow_needle != NULL));
	if (is_shadow) {
		if (code == LV_EVENT_DRAW_PART_BEGIN) {
			_meter_draw_shadow_needle(dsc->draw_ctx, md, dsc->p1, dsc->p2,
			                          master_opa);
			dsc->line_dsc->opa = LV_OPA_TRANSP;
		}
		return;
	}

	uint8_t style    = md->needle_tip_style;
	uint8_t rear_len = md->needle_rear_length;
	/* Fast path: no tip polygon or rear extension needed. */
	if (style == 0 && rear_len == 0) return;

	lv_draw_line_dsc_t *line_dsc = dsc->line_dsc;

	/* Static across BEGIN/END so the pointer we hand back to LVGL stays valid
	 * during the lv_draw_line call inside the meter draw loop, AND so END can
	 * recover the original pivot for tip-polygon math after BEGIN mutated p1.
	 * Single-threaded LVGL — no races between needle instances because each
	 * needle draws BEGIN→line→END as a tight sequence. */
	static lv_point_t s_orig_pivot;
	static lv_point_t s_extended_p1;

	if (code == LV_EVENT_DRAW_PART_BEGIN) {
		s_orig_pivot = *dsc->p1;

		if (style == 1) {
			line_dsc->round_start = 1;
			line_dsc->round_end   = 1;
		} else if (style >= 2 && style <= 5) {
			/* Polygon tip styles replace the line — hide the original. */
			line_dsc->opa = LV_OPA_TRANSP;
		}

		/* For non-polygon styles + rear: extend dsc->p1 backward so LVGL
		 * draws ONE continuous line from rear endpoint through pivot to
		 * tip. Geometry is guaranteed coherent — it's literally the same
		 * lv_draw_line call that draws the front, so the rear can't be
		 * at a different angle. For polygon styles (2-5) the line is
		 * already hidden, so we leave p1 alone and draw the rear
		 * separately in END (alongside the tip polygon). */
		if (rear_len > 0 && style <= 1) {
			int32_t dx = dsc->p2->x - dsc->p1->x;
			int32_t dy = dsc->p2->y - dsc->p1->y;
			uint32_t len_sq = (uint32_t)(dx * dx + dy * dy);
			if (len_sq > 0) {
				lv_sqrt_res_t sres;
				lv_sqrt(len_sq, &sres, 0x800);
				int32_t len = sres.i;
				if (len > 0) {
					s_extended_p1.x = (lv_coord_t)(dsc->p1->x - (dx * (int32_t)rear_len) / len);
					s_extended_p1.y = (lv_coord_t)(dsc->p1->y - (dy * (int32_t)rear_len) / len);
					dsc->p1 = &s_extended_p1;
				}
			}
		}
		return;
	}

	/* DRAW_PART_END: tip polygon (styles 2-5), and rear for polygon styles.
	 * Use the SAVED original pivot — dsc->p1 may have been mutated in BEGIN
	 * to extend the front line backward. */
	const lv_point_t *p1 = &s_orig_pivot;
	const lv_point_t *p2 = dsc->p2;
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
	rdsc.bg_opa       = master_opa;   /* COVER normally; scaled during fades */
	rdsc.border_width = 0;

	/* Tip polygon (styles 2-5). Rear extension below runs independently for
	 * any style with rear_len > 0. */
	if (style >= 2 && style <= 5) {
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
		break;
	}

	if (npts > 0) lv_draw_polygon(dsc->draw_ctx, &rdsc, pts, npts);
	} /* end of tip-polygon block */

	/* Rear extension for POLYGON tip styles only. Flat (0) and Rounded (1) get
	 * the rear via dsc->p1 mutation in BEGIN — LVGL draws one continuous
	 * line that already includes the rear. Polygon styles (2-5) hide the
	 * line, so we draw the rear here as a separate line using the same
	 * line_dsc with opacity restored. */
	if (rear_len > 0 && style >= 2 && style <= 5) {
		int32_t rx = p1->x - (dx * (int32_t)rear_len) / len;
		int32_t ry = p1->y - (dy * (int32_t)rear_len) / len;
		lv_point_t rear_start = { (lv_coord_t)rx, (lv_coord_t)ry };
		lv_draw_line_dsc_t rear_dsc = *line_dsc;
		rear_dsc.opa = master_opa;   /* restore from the BEGIN-time hide, but
		                              * keep any recursive fade opacity */
		lv_draw_line(dsc->draw_ctx, &rear_dsc, &rear_start, p1);
	}
}

/* Build a single meter (day or night). When `use_night` is true, any field
 * with a corresponding night override picks the night value; otherwise the
 * day value is used. The output pointers (`*out_meter`, `*out_scale`,
 * `*out_needle`, `*out_needle_scale`) receive the created LVGL handles.
 * `*out_needle_img_dsc` and `*out_bg_img_dsc` receive any loaded image
 * descriptors so the caller can free them on destroy. */
/* Extracted needle-add block. Called from _meter_build_one at the
 * end (the common path) OR from _meter_create after the static-tick
 * snapshot has been taken — the snapshot needs the meter built WITHOUT
 * the needle so the rendered image doesn't include a ghost needle line
 * stamped at init_v that would then sit visibly behind the live one.
 *
 * angle_range is duplicated rather than hoisted onto meter_data_t
 * because we only need it here and at scale-range setup, and the
 * normalisation rule (0→360 when start!=end) is shared by both. */
static uint32_t _meter_compute_angle_range(const meter_data_t *md) {
	uint32_t r = (360 + (md->end_angle % 360) - (md->start_angle % 360)) % 360;
	if (r == 0 && md->start_angle != md->end_angle) r = 360;
	return r;
}

static void _meter_add_needle_indicator(meter_data_t *md, lv_obj_t *m,
                                         lv_meter_scale_t *scale, bool use_night,
                                         uint32_t angle_range,
                                         lv_meter_indicator_t **out_needle,
                                         lv_meter_scale_t **out_needle_scale,
                                         lv_img_dsc_t **out_needle_img_dsc) {
	lv_color_t needle_c    = use_night ? NIGHT_PICK_COLOR(true, md->night, needle_color,      md->needle_color)      : md->needle_color;
	lv_color_t ball_c      = use_night ? NIGHT_PICK_COLOR(true, md->night, needle_ball_color, md->needle_ball_color) : md->needle_ball_color;
	const char *needle_img = (use_night && md->night.has_needle_image_name) ? md->night.needle_image_name : md->needle_image_name;

	bool needle_is_image = (needle_img && needle_img[0] != '\0');

	/* Needle (skipped entirely when show_needle is off — the meter then
	 * renders ticks / labels / ball only, with no needle line, image, or
	 * shadow). needle stays NULL so the signal callback's md->needle guard
	 * short-circuits and nothing tries to move a missing indicator. */
	lv_meter_indicator_t *needle = NULL;
	lv_meter_scale_t *needle_target_scale = scale;
	*out_needle_scale = NULL;
	if (md->show_needle) {
		/* Shadow indicator — only for line needles. Added BEFORE the main needle
		 * so READ_BACK iteration draws it first (under the real needle). Width
		 * + color set here serve no visual purpose because _meter_needle_draw_cb
		 * draws the shadow geometry itself, but we still need a real indicator
		 * for LVGL to emit DRAW_PART_NEEDLE_LINE events that we can intercept. */
		if (md->shadow_enabled && !needle_is_image) {
			lv_meter_indicator_t *sh = lv_meter_add_needle_line(m, scale, md->needle_width,
			                                                     md->shadow_color, md->needle_r_mod);
			if (use_night) md->night_shadow_needle = sh;
			else           md->shadow_needle       = sh;
		}

		if (needle_img && needle_img[0] != '\0') {
			lv_img_dsc_t *ndsc = rdm_image_load(needle_img);
			if (ndsc) {
				if (md->needle_angle_offset != 0) {
					lv_meter_scale_t *ns = lv_meter_add_scale(m);
					lv_meter_set_scale_range(m, ns,
					                         lroundf(md->min * (float)md->value_scale),
					                         lroundf(md->max * (float)md->value_scale), angle_range,
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
	}

	/* Needle center ball */
	if (!md->show_needle_ball || md->needle_ball_size == 0) {
		lv_obj_set_style_size(m, 0, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_INDICATOR);
	} else {
		lv_obj_set_style_size(m, md->needle_ball_size, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(m, ball_c, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_INDICATOR);
	}

	/* Initial needle position. Pull the current signal value if one is
	 * bound and fresh, otherwise fall back to md->min. Then apply the
	 * same anchor + invert transforms _meter_on_signal does, so toggling
	 * "Invert Direction" on a meter with no live signal still snaps the
	 * needle to the correct end of the sweep. */
	float init_f = md->min;
	if (md->signal_index >= 0) {
		signal_t *sig = signal_get_by_index((uint16_t)md->signal_index);
		if (sig && !sig->is_stale) {
			float fv = sig->current_value;
			if (fv < md->min) fv = md->min;
			if (fv > md->max) fv = md->max;
			init_f = fv;
		}
	}
	init_f = _meter_apply_anchor(md, init_f);
	if (md->reverse) init_f = md->max + md->min - init_f;
	int32_t init_v = lroundf(init_f * (float)md->value_scale);
	if (needle) lv_meter_set_indicator_value(m, needle, init_v);

	/* Snap the shadow indicator (if present) to the same initial value so
	 * frame-1 already has the shadow underneath the needle. */
	lv_meter_indicator_t *sh = use_night ? md->night_shadow_needle : md->shadow_needle;
	if (sh) lv_meter_set_indicator_value(m, sh, init_v);

	/* Refresh ext_draw_size now that the needle's rear extension (if
	 * any) needs to be reflected in the clip area. The DRAW_PART
	 * callbacks themselves were registered earlier inside
	 * _meter_build_one so they were already active for the
	 * static-tick snapshot pass — moving them here would have meant
	 * the snapshot ran with default LVGL tick rendering, losing
	 * tick_label_divisor / redline_recolor_ticks / anchor / reverse
	 * relabel customisations. */
	lv_obj_refresh_ext_draw_size(m);

	*out_needle = needle;
}

/* Snapshot the meter's tick / label / bg / redline-arc layer, apply
 * that snapshot as the meter's own bg_img_src, strip the now-redundant
 * static parts off the live meter, then add the needle. This is the
 * heart of the static-ticks optimisation: from here on, only the
 * needle redraws on signal updates — every other layer is a single
 * pre-rasterised bg image blit, which is dramatically cheaper than
 * recomputing each tick line per frame.
 *
 * The bg_img_src approach replaces an earlier sibling-lv_img design
 * which had z-order + sizing problems when the meter's parent was the
 * screen root. Using the meter's own bg_img keeps the snapshot
 * locked to the meter's coords by construction.
 *
 * Caller must have already created the meter with include_needle=false,
 * stored it on md->meter / md->scale, and laid out the parent so the
 * meter has its real (w,h). use_night picks which snapshot slot we're
 * writing — there's a separate pair for day and night so the night
 * meter can flatten independently when it gets built on demand. */
static void _meter_flatten_static_ticks(meter_data_t *md, lv_obj_t *parent, bool use_night) {
	(void)parent;   /* no longer needed — snapshot lives on the meter itself */
	if (!md) return;
	lv_obj_t             *m   = use_night ? md->night_meter        : md->meter;
	lv_meter_scale_t     *scale = use_night ? md->night_scale      : md->scale;
	lv_meter_indicator_t *arc = use_night ? md->night_redline_arc  : md->redline_arc;
	if (!m || !scale || !lv_obj_is_valid(m)) return;

	/* The meter has to have a realised position + size before snapshot
	 * — otherwise lv_snapshot_take captures an empty canvas. The size
	 * was set right after build, so update_layout flushes the layout
	 * pass synchronously. */
	lv_obj_update_layout(m);

	/* lv_meter draws the centre knob (LV_PART_INDICATOR) unconditionally in
	 * DRAW_MAIN — even with zero indicators. At this point the needle hasn't
	 * been added yet, so the part still carries the THEME's default knob
	 * style, and without this hide the snapshot bakes a ghost white knob
	 * into the face image (the lone static-vs-dynamic pixel diff found by
	 * tools/meter_visual_parity.py). _meter_add_needle_indicator re-styles
	 * the part right after the snapshot, so no restore is needed. */
	lv_obj_set_style_size(m, 0, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_INDICATOR);

	/* Use TRUE_COLOR_ALPHA so meter_bg_opa < 255 round-trips correctly.
	 * TRUE_COLOR would composite the translucent bg over a black
	 * (zeroed) buffer and lose the alpha, leaving the user with a
	 * darkish-red bg instead of the half-red they configured. The
	 * cost is one extra byte per pixel (RGB565 + 8-bit alpha) which
	 * works out to ~280 KB for a 240×240 meter — still bounded. */
	lv_img_dsc_t *snap = lv_snapshot_take(m, LV_IMG_CF_TRUE_COLOR_ALPHA);
	if (!snap) {
		/* Snapshot can fail under heap pressure. Fall back to the
		 * dynamic path: just add the needle, leave ticks live. */
		ESP_LOGW(TAG, "lv_snapshot_take returned NULL — static_ticks falls back to dynamic");
		uint32_t angle_range = _meter_compute_angle_range(md);
		lv_meter_indicator_t **out_needle = use_night ? &md->night_needle : &md->needle;
		lv_meter_scale_t     **out_ns     = use_night ? &md->night_needle_scale : &md->needle_scale;
		lv_img_dsc_t         **out_ndsc   = use_night ? &md->night_needle_img_dsc : &md->needle_img_dsc;
		_meter_add_needle_indicator(md, m, scale, use_night, angle_range,
		                             out_needle, out_ns, out_ndsc);
		return;
	}

	if (use_night) md->night_tick_snapshot_dsc = snap;
	else           md->tick_snapshot_dsc       = snap;

	/* Strip the static parts off the live meter — they all live in
	 * the snapshot now, redrawing them every frame would defeat the
	 * whole point:
	 *   - Tick widths to 0 (range / count preserved so the scale's
	 *     angle math keeps working for needle positioning).
	 *   - Tick label opacity transparent.
	 *   - Meter background opacity 0 (the snapshot has it baked in).
	 *   - Border width 0 (snapshot has it).
	 *   - Redline arc collapsed to zero-length (lv_meter v8 has no
	 *     remove_indicator API, so collapsing is the only way to
	 *     stop it double-rendering against the snapshot).
	 */
	uint8_t mtc = md->minor_tick_count < 2 ? 2 : md->minor_tick_count;
	uint8_t mte = md->major_tick_every < 1 ? 1 : md->major_tick_every;
	lv_meter_set_scale_ticks(m, scale, mtc, 0, 0, lv_color_black());
	lv_meter_set_scale_major_ticks(m, scale, mte, 0, 0, lv_color_black(), 0);
	lv_obj_set_style_text_opa(m, LV_OPA_TRANSP, LV_PART_TICKS);
	lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(m, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (arc) {
		lv_meter_set_indicator_start_value(m, arc, lroundf(md->min * (float)md->value_scale));
		lv_meter_set_indicator_end_value(m, arc, lroundf(md->min * (float)md->value_scale));
	}

	/* Snapshot becomes the meter's own bg image. LVGL anchors bg_img
	 * to the centre of the obj's content area and (with tiled=false)
	 * does not stretch — the snapshot is the same dimensions as the
	 * meter by construction, so they overlap perfectly. */
	lv_obj_set_style_bg_img_src(m, snap, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_img_opa(m, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_img_tiled(m, false, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Now add the needle. It draws on top of the bg image and is the
	 * only thing that redraws per frame. */
	uint32_t angle_range = _meter_compute_angle_range(md);
	lv_meter_indicator_t **out_needle = use_night ? &md->night_needle : &md->needle;
	lv_meter_scale_t     **out_ns     = use_night ? &md->night_needle_scale : &md->needle_scale;
	lv_img_dsc_t         **out_ndsc   = use_night ? &md->night_needle_img_dsc : &md->needle_img_dsc;
	_meter_add_needle_indicator(md, m, scale, use_night, angle_range,
	                             out_needle, out_ns, out_ndsc);
}

static void _meter_build_one(meter_data_t *md, lv_obj_t *parent, bool use_night,
                             bool include_needle,
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
	/* needle_color / needle_ball_color / needle_image_name moved into
	 * _meter_add_needle_indicator — only the static-layer colours are
	 * needed here. */
	lv_color_t tlc         = use_night ? NIGHT_PICK_COLOR(true, md->night, tick_label_color,  md->tick_label_color)  : md->tick_label_color;
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
	/* Counter-clockwise / reverse: scale's start_angle stays as configured —
	 * the value mirror (max+min-v in _meter_on_signal) is what makes max
	 * land at the start position and min land at the end of the sweep, so
	 * a Top-start meter with reverse=true ends up with 0 at the bottom and
	 * max at the top. Mirrors a Bottom-start meter to face it. */
	lv_meter_set_scale_range(m, scale,
	                         lroundf(md->min * (float)md->value_scale),
	                         lroundf(md->max * (float)md->value_scale),
	                         angle_range, (int32_t)md->start_angle);
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

	/* Redline arc — drawn before the needle so the needle paints on top.
	 * The arc spans [threshold, max] in real value space. We need the
	 * LVGL-linear values that produce the SAME angles as those real values,
	 * which means applying the same transforms (anchor → reverse) the
	 * needle goes through. With reverse on the high real values land at
	 * low LVGL values, so the arc covers [min, mirror_of(threshold)]. */
	if (md->redline_enabled && md->redline_show_arc) {
		float arc_thresh = _meter_apply_anchor(md, md->redline_threshold);
		if (arc_thresh < md->min) arc_thresh = md->min;
		if (arc_thresh > md->max) arc_thresh = md->max;
		float arc_start_f = arc_thresh;
		float arc_end_f   = md->max;
		if (md->reverse) {
			arc_start_f = md->min;
			arc_end_f   = md->max + md->min - arc_thresh;
		}
		lv_meter_indicator_t *arc = lv_meter_add_arc(m, scale,
			md->redline_arc_width > 0 ? md->redline_arc_width : 6,
			md->redline_color, md->redline_arc_r_mod);
		if (arc) {
			lv_meter_set_indicator_start_value(m, arc, lroundf(arc_start_f * (float)md->value_scale));
			lv_meter_set_indicator_end_value(m, arc, lroundf(arc_end_f * (float)md->value_scale));
		}
		/* Stash the arc pointer so the static-tick flatten path can
		 * collapse it to zero-length after snapshot. */
		if (use_night) md->night_redline_arc = arc;
		else           md->redline_arc       = arc;
	}

	/* Register the tick + needle draw callback NOW, before either the
	 * needle or the static-tick snapshot. The callback handles all of:
	 *   - tick_label_divisor    (e.g. 6000 → "6" on a tach with x1000)
	 *   - redline_recolor_ticks (paint ticks ≥ threshold red)
	 *   - anchor curve relabel  (non-linear value→angle map)
	 *   - reverse mode relabel  (max sits at start of sweep)
	 *   - needle tip styles + rear extension (only fires after needle exists)
	 * Registering it here means the static-tick snapshot pass captures
	 * the customised tick labels / redline-recoloured ticks correctly;
	 * the needle branch of the callback no-ops until a needle is added. */
	lv_obj_add_event_cb(m, _meter_needle_draw_cb, LV_EVENT_DRAW_PART_BEGIN, md);
	lv_obj_add_event_cb(m, _meter_needle_draw_cb, LV_EVENT_DRAW_PART_END,   md);
	lv_obj_add_event_cb(m, _meter_needle_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, md);

	/* Needle add — usually inline, but skipped when static_ticks mode
	 * wants to snapshot the meter without the needle in it. Caller is
	 * responsible for calling _meter_add_needle_indicator after the
	 * snapshot if include_needle was false. */
	if (include_needle) {
		_meter_add_needle_indicator(md, m, scale, use_night, angle_range,
		                             out_needle, out_needle_scale, out_needle_img_dsc);
	} else {
		*out_needle       = NULL;
		*out_needle_scale = NULL;
	}

	*out_meter = m;
	*out_scale = scale;
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

	/* Build day meter. include_needle=false defers needle setup so we
	 * can snapshot the meter without a ghost needle baked into the
	 * static-tick image. The actual needle-add happens after the
	 * snapshot in _meter_setup_static_ticks, or immediately below if
	 * the user opted out of the flat-tick path. */
	lv_obj_t *m = NULL;
	lv_meter_scale_t *scale = NULL;
	lv_meter_indicator_t *needle = NULL;
	lv_meter_scale_t *needle_scale = NULL;
	bool defer_needle_day = md->static_ticks;
	_meter_build_one(md, meter_parent, false, !defer_needle_day,
	                 &m, &scale, &needle, &needle_scale,
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

	/* Static-tick path: snapshot the bare meter (no needle yet), drop
	 * a sibling lv_img beneath it carrying that pixel data, and strip
	 * the now-static parts off the live meter so only the needle
	 * redraws on signal updates. _meter_flatten_static_ticks handles
	 * the whole dance; needle_scale gets re-evaluated when the helper
	 * adds the needle. */
	if (defer_needle_day) {
		_meter_flatten_static_ticks(md, meter_parent, false /*night*/);
	}

	/* Night meter build is deferred to _meter_apply_night_mode — see the
	 * _meter_build_night_lazy helper below. Keeping two lv_meter objects
	 * in the tree (even with the night one hidden) was forcing LVGL to
	 * walk + skip the hidden meter every refresh; when adjacent widgets'
	 * dirty rects landed on the container, the extra bookkeeping dragged
	 * through the redraw pipeline. Deferring the build means layouts
	 * that never engage night mode pay zero overhead for the override
	 * config being present. */

	w->root = cont ? cont : m;

	/* Fresh meter objects — force the first signal callback to paint. */
	md->_last_needle_valid = false;

	/* Subscribe to signal if bound */
	if (md->signal_index >= 0)
		signal_subscribe(md->signal_index, _meter_on_signal, w);

	/* Subscribe to channel-changed events when bound. Fires on user
	 * edits in the Channels tab (threshold tweaks, range changes,
	 * signal rebinding). Listener pattern works for both v14-bound
	 * widgets and legacy widgets that recorded themselves at load. */
	if (md->channel) {
		channel_manager_subscribe((channel_t *)md->channel,
		                           _meter_on_channel_changed, w);
	}

	/* NOTE: conditional rules are subscribed centrally by layout_manager
	 * (widget_rules_subscribe after create) for every widget — do NOT call it
	 * here too, or the rule signal gets a duplicate subscriber that survives
	 * destroy as a dangling pointer (use-after-free). Same for from_json /
	 * to_json below. */

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
	/* Geometry moved (scale center / radius) — force the next tick to repaint
	 * the needle even if the bound value hasn't changed. */
	if (md) md->_last_needle_valid = false;
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
	/* v14 channel binding — only emit when the widget loaded with an
	 * explicit `channel` field. Legacy widgets keep emitting just
	 * signal_name + min/max/redline so a downgrade to v13 firmware
	 * still reads them correctly. */
	if (md->channel_id[0] != '\0')
		cJSON_AddStringToObject(cfg, "channel", md->channel_id);

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
	if (!md->show_needle)
		cJSON_AddBoolToObject(cfg, "show_needle", false);
	if (!md->show_needle_ball)
		cJSON_AddBoolToObject(cfg, "show_needle_ball", false);
	if (md->needle_width != 4)
		cJSON_AddNumberToObject(cfg, "needle_width", md->needle_width);
	if (md->needle_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "needle_color", (int)md->needle_color.full);
	if (md->needle_r_mod != -10)
		cJSON_AddNumberToObject(cfg, "needle_r_mod", md->needle_r_mod);
	if (md->needle_rear_length != 0)
		cJSON_AddNumberToObject(cfg, "needle_rear_length", md->needle_rear_length);
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
	/* Shadow — defaults-only emit */
	if (md->shadow_enabled)
		cJSON_AddBoolToObject(cfg, "shadow_enabled", true);
	if (!md->shadow_dynamic)
		cJSON_AddBoolToObject(cfg, "shadow_dynamic", false);
	if (md->shadow_offset_x != 3)
		cJSON_AddNumberToObject(cfg, "shadow_offset_x", md->shadow_offset_x);
	if (md->shadow_offset_y != 4)
		cJSON_AddNumberToObject(cfg, "shadow_offset_y", md->shadow_offset_y);
	if (md->shadow_opa != 120)
		cJSON_AddNumberToObject(cfg, "shadow_opa", md->shadow_opa);
	if (md->shadow_width_extra != 2)
		cJSON_AddNumberToObject(cfg, "shadow_width_extra", md->shadow_width_extra);
	if (md->shadow_color.full != lv_color_black().full)
		cJSON_AddNumberToObject(cfg, "shadow_color", (int)md->shadow_color.full);
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
	if (md->tick_label_divisor != 1)
		cJSON_AddNumberToObject(cfg, "tick_label_divisor", md->tick_label_divisor);
	if (!md->show_ticks)
		cJSON_AddBoolToObject(cfg, "show_ticks", false);
	if (!md->show_tick_labels)
		cJSON_AddBoolToObject(cfg, "show_tick_labels", false);
	/* static_ticks defaults to TRUE — only emit when the user has opted
	 * out, keeping the JSON quiet for the common path. (Layouts saved by
	 * older firmware may carry a redundant `true`; from_json reads both.) */
	if (!md->static_ticks)
		cJSON_AddBoolToObject(cfg, "static_ticks", false);

	/* Redline */
	if (md->redline_enabled)
		cJSON_AddBoolToObject(cfg, "redline_enabled", true);
	if (md->redline_threshold != 80)
		cJSON_AddNumberToObject(cfg, "redline_threshold", md->redline_threshold);
	if (md->redline_color.full != lv_color_hex(0xFF0000).full)
		cJSON_AddNumberToObject(cfg, "redline_color", (int)md->redline_color.full);
	if (!md->redline_show_arc)
		cJSON_AddBoolToObject(cfg, "redline_show_arc", false);
	if (md->redline_arc_width != 6)
		cJSON_AddNumberToObject(cfg, "redline_arc_width", md->redline_arc_width);
	if (md->redline_arc_r_mod != 0)
		cJSON_AddNumberToObject(cfg, "redline_arc_r_mod", md->redline_arc_r_mod);
	if (!md->redline_recolor_ticks)
		cJSON_AddBoolToObject(cfg, "redline_recolor_ticks", false);

	/* Anchor curve */
	if (md->anchor_enabled)
		cJSON_AddBoolToObject(cfg, "anchor_enabled", true);
	if (md->anchor_value != 50)
		cJSON_AddNumberToObject(cfg, "anchor_value", md->anchor_value);
	if (md->anchor_position != 50)
		cJSON_AddNumberToObject(cfg, "anchor_position", md->anchor_position);

	/* Reverse */
	if (md->reverse)
		cJSON_AddBoolToObject(cfg, "reverse", true);

	/* Rules serialized centrally by layout_manager (see NOTE in _meter_create) */

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
		md->min = (float)min_item->valuedouble;
	if (cJSON_IsNumber(max_item))
		md->max = (float)max_item->valuedouble;
	if (cJSON_IsNumber(sa_item))
		md->start_angle = (int16_t)sa_item->valueint;
	if (cJSON_IsNumber(ea_item))
		md->end_angle = (int16_t)ea_item->valueint;
	cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(sig_item) && sig_item->valuestring) {
		safe_strncpy(md->signal_name, sig_item->valuestring, sizeof(md->signal_name));
	}

	/* ── v14 channel binding (deferred until after all other fields
	 * are parsed below, so the legacy-widget migrator sees the full
	 * widget config before populating the channel). The handler block
	 * is placed after _meter_from_json's redline parse so we can pull
	 * redline_threshold/redline_color into the channel correctly. */

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
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "show_needle");
	if (cJSON_IsBool(ap)) md->show_needle = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "show_needle_ball");
	if (cJSON_IsBool(ap)) md->show_needle_ball = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_width");
	if (cJSON_IsNumber(ap)) md->needle_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_color");
	if (cJSON_IsNumber(ap)) md->needle_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_r_mod");
	if (cJSON_IsNumber(ap)) md->needle_r_mod = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_rear_length");
	if (cJSON_IsNumber(ap)) md->needle_rear_length = (uint8_t)ap->valueint;
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
	/* Shadow */
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_enabled");
	if (cJSON_IsBool(ap)) md->shadow_enabled = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_dynamic");
	if (cJSON_IsBool(ap)) md->shadow_dynamic = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_offset_x");
	if (cJSON_IsNumber(ap)) md->shadow_offset_x = (int8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_offset_y");
	if (cJSON_IsNumber(ap)) md->shadow_offset_y = (int8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_opa");
	if (cJSON_IsNumber(ap)) md->shadow_opa = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_width_extra");
	if (cJSON_IsNumber(ap)) md->shadow_width_extra = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_color");
	if (cJSON_IsNumber(ap)) md->shadow_color.full = (uint32_t)ap->valueint;
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
	if (cJSON_IsNumber(ap)) md->label_gap = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_font");
	if (cJSON_IsString(ap) && ap->valuestring) {
		safe_strncpy(md->tick_label_font, ap->valuestring, sizeof(md->tick_label_font));
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_color");
	if (cJSON_IsNumber(ap)) md->tick_label_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_divisor");
	if (cJSON_IsNumber(ap)) {
		int32_t d = ap->valueint;
		if (d < 1)     d = 1;
		if (d > 65535) d = 65535;
		md->tick_label_divisor = (uint16_t)d;
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "show_ticks");
	if (cJSON_IsBool(ap)) md->show_ticks = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "show_tick_labels");
	if (cJSON_IsBool(ap)) md->show_tick_labels = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "static_ticks");
	if (cJSON_IsBool(ap)) md->static_ticks = cJSON_IsTrue(ap);

	/* Redline */
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "redline_enabled");
	if (cJSON_IsBool(ap)) md->redline_enabled = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "redline_threshold");
	if (cJSON_IsNumber(ap)) md->redline_threshold = (float)ap->valuedouble;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "redline_color");
	if (cJSON_IsNumber(ap)) md->redline_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "redline_show_arc");
	if (cJSON_IsBool(ap)) md->redline_show_arc = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "redline_arc_width");
	if (cJSON_IsNumber(ap)) md->redline_arc_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "redline_arc_r_mod");
	if (cJSON_IsNumber(ap)) md->redline_arc_r_mod = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "redline_recolor_ticks");
	if (cJSON_IsBool(ap)) md->redline_recolor_ticks = cJSON_IsTrue(ap);

	/* Anchor curve */
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_enabled");
	if (cJSON_IsBool(ap)) md->anchor_enabled = cJSON_IsTrue(ap);
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_value");
	if (cJSON_IsNumber(ap)) md->anchor_value = (float)ap->valuedouble;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_position");
	if (cJSON_IsNumber(ap)) md->anchor_position = (uint8_t)ap->valueint;

	/* Reverse */
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "reverse");
	if (cJSON_IsBool(ap)) md->reverse = cJSON_IsTrue(ap);

	/* Rules parsed centrally by layout_manager (see NOTE in _meter_create) */

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

	/* ── v14 channel binding ──────────────────────────────────────
	 * Two cases:
	 *   1. Layout has a `channel` field — v14 layout. Look up the
	 *      channel and snapshot its values into the widget's live
	 *      fields. Listener subscription happens later in _meter_create
	 *      once w->root exists.
	 *   2. No `channel` field but signal_name + min/max/redline are
	 *      present — v13 layout. Self-report the widget's data to the
	 *      channel registry via channel_manager_record_legacy_widget()
	 *      so the user's existing redlines and ranges auto-populate the
	 *      channels.json on first v14 boot. Widget continues running on
	 *      its legacy fields. */
	/* Empty-signal channel falls through to the legacy path so
	 * record_legacy_widget repopulates it from the widget's own signal
	 * field — handles the cross-layout-switch case where resolve_signals
	 * cleared a stale binding before widgets load. */
	cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(cfg, "channel");
	if (cJSON_IsString(ch_item) && ch_item->valuestring && ch_item->valuestring[0] != '\0')
		safe_strncpy(md->channel_id, ch_item->valuestring, sizeof(md->channel_id));
	channel_t *bound_c = md->channel_id[0] ? channel_manager_get(md->channel_id) : NULL;
	if (bound_c && bound_c->signal_index >= 0) {
		md->channel = bound_c;
		/* Apply channel values (signal, range, redline, decimal scale) to the
		 * widget's live fields via the shared helper so value_scale/decimals
		 * are snapshotted too. Redline colour stays widget-owned. */
		_meter_apply_channel(w);
	} else if (md->signal_name[0] != '\0') {
		/* v13 legacy path — self-report data to populate the channel
		 * registry. First-write-wins inside channel_manager so later
		 * widgets don't clobber earlier ones. */
		legacy_widget_data_t legacy = {
			.signal_name = md->signal_name,
			.min = md->min,
			.max = md->max,
			.high_warn = md->redline_enabled ? md->redline_threshold : INT32_MIN,
			.color_normal = CHANNEL_USE_DEFAULT_COLOR,
			.color_high_warn = md->redline_enabled ? lv_color_to32(md->redline_color) & 0xFFFFFF
			                                       : CHANNEL_USE_DEFAULT_COLOR,
		};
		channel_t *c = channel_manager_record_legacy_widget(&legacy);
		if (c) {
			/* Cache the pointer so user edits in Channels tab can
			 * propagate even though the widget loaded legacy. The
			 * channel_id field stays empty — to_json keeps emitting
			 * legacy format on save so a v13 dash can still read it. */
			md->channel = c;
		}
	}
}

static void _meter_destroy(widget_t *w) {
	if (!w)
		return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (md && md->signal_index >= 0)
		signal_unsubscribe(md->signal_index, _meter_on_signal, w);
	if (md && md->channel) {
		channel_manager_unsubscribe((channel_t *)md->channel,
		                             _meter_on_channel_changed, w);
		md->channel = NULL;
	}
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
		/* Tick-snapshot image data is heap-allocated by LVGL inside
		 * lv_snapshot_take — must be released with lv_snapshot_free
		 * (frees both data buffer + descriptor). The meter object
		 * that referenced it via bg_img_src was already deleted by
		 * the lv_obj_del cascade above, so this is safe to free now. */
		if (md->tick_snapshot_dsc)       lv_snapshot_free(md->tick_snapshot_dsc);
		if (md->night_tick_snapshot_dsc) lv_snapshot_free(md->night_tick_snapshot_dsc);
		md->tick_snapshot_dsc       = NULL;
		md->night_tick_snapshot_dsc = NULL;
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
	/* Same defer-needle-for-snapshot dance as the day meter — the
	 * static-tick flatten step runs after we size + position the
	 * night meter so the snapshot captures the correct dimensions. */
	bool defer_needle_night = md->static_ticks;
	_meter_build_one(md, parent, true, !defer_needle_night,
	                 &nm, &nscale, &nneedle, &nneedle_scale,
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

	if (defer_needle_night) {
		_meter_flatten_static_ticks(md, parent, true /*night*/);
		/* _meter_flatten_static_ticks writes md->night_needle and
		 * md->night_needle_scale itself, so no extra assignments needed. */
	}
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
		float fv = md->min;
		if (sig && !sig->is_stale) {
			fv = sig->current_value;
			if (fv < md->min) fv = md->min;
			if (fv > md->max) fv = md->max;
		}
		fv = _meter_apply_anchor(md, fv);
		if (md->reverse) fv = md->max + md->min - fv;
		int32_t v = lroundf(fv * (float)md->value_scale);

		/* The meter we're about to reveal hasn't been driven while hidden
		 * (the signal callback only touches the visible one). Reset the memo
		 * BEFORE catching it up + un-hiding so the next live signal tick is
		 * never suppressed against a stale memo from the other state. */
		md->_last_needle_valid = false;

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
	if (md->show_needle_ball && md->needle_ball_size > 0) {
		lv_obj_set_style_bg_color(md->meter, nbc, LV_PART_INDICATOR);
	}
	lv_obj_set_style_text_color(md->meter, tlc, LV_PART_TICKS);
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _meter_night_cb(bool active, void *user_data) {
	_meter_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Meter is the largest widget — most of its fields are baked into the
 * lv_meter scale / indicator at create time and aren't safely mutable live
 * (LVGL v8 limitation: no public API to repaint tick widths / colours after
 * lv_meter_add_scale_*). Strategy:
 *   - Always write the field into type_data so a save + dashboard reload
 *     picks it up.
 *   - For the small set of fields that map to lv_obj_set_style_* on the
 *     meter object (bg / border), apply live too.
 *   - Schema field deltas:
 *       "start_angle_user" -> md->start_angle (raw)
 *       "sweep_degrees"    -> recomputes md->end_angle from start_angle
 *       "minor_tick_step"  -> derives md->minor_tick_count from (max-min)
 *       "major_tick_step"  -> derives md->major_tick_every from minor_count */

static bool _meter_inspector_get(const widget_t *w, const char *name,
                                 widget_field_value_t *out) {
	if (!w || w->type != WIDGET_METER || !w->type_data || !name || !out) return false;
	const meter_data_t *md = (const meter_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0)        { out->str = md->signal_name;       return true; }
	if (strcmp(name, "tick_label_font") == 0)    { out->str = md->tick_label_font;   return true; }
	if (strcmp(name, "needle_image_name") == 0)  { out->str = md->needle_image_name; return true; }
	if (strcmp(name, "bg_image_name") == 0)      { out->str = md->bg_image_name;     return true; }
	if (strcmp(name, "min") == 0)                { out->i = md->min;                 return true; }
	if (strcmp(name, "max") == 0)                { out->i = md->max;                 return true; }
	if (strcmp(name, "start_angle_user") == 0)   { out->i = md->start_angle;         return true; }
	if (strcmp(name, "sweep_degrees") == 0) {
		int sweep = ((int)md->end_angle - (int)md->start_angle + 360) % 360;
		if (sweep == 0 && md->start_angle != md->end_angle) sweep = 360;
		out->i = sweep;
		return true;
	}
	if (strcmp(name, "reverse") == 0)            { out->b = md->reverse;             return true; }
	if (strcmp(name, "minor_tick_step") == 0) {
		int32_t range = md->max - md->min;
		int32_t denom = md->minor_tick_count > 1 ? md->minor_tick_count - 1 : 1;
		out->i = range / denom;
		return true;
	}
	if (strcmp(name, "major_tick_step") == 0) {
		int32_t range = md->max - md->min;
		int32_t denom = md->minor_tick_count > 1 ? md->minor_tick_count - 1 : 1;
		int32_t mstep = range / denom;
		out->i = mstep * md->major_tick_every;
		return true;
	}
	if (strcmp(name, "anchor_enabled") == 0)     { out->b = md->anchor_enabled;      return true; }
	if (strcmp(name, "anchor_value") == 0)       { out->i = md->anchor_value;        return true; }
	if (strcmp(name, "anchor_position") == 0)    { out->i = md->anchor_position;     return true; }
	if (strcmp(name, "redline_enabled") == 0)    { out->b = md->redline_enabled;     return true; }
	if (strcmp(name, "redline_threshold") == 0)  { out->i = md->redline_threshold;   return true; }
	if (strcmp(name, "redline_show_arc") == 0)   { out->b = md->redline_show_arc;    return true; }
	if (strcmp(name, "redline_arc_width") == 0)  { out->i = md->redline_arc_width;   return true; }
	if (strcmp(name, "redline_arc_r_mod") == 0)  { out->i = md->redline_arc_r_mod;   return true; }
	if (strcmp(name, "redline_recolor_ticks") == 0) { out->b = md->redline_recolor_ticks; return true; }
	if (strcmp(name, "redline_color") == 0)      { out->color = lv_color_to32(md->redline_color)    & 0xFFFFFF; return true; }
	if (strcmp(name, "meter_bg_opa") == 0)       { out->i = md->meter_bg_opa;        return true; }
	if (strcmp(name, "meter_bg_color") == 0)     { out->color = lv_color_to32(md->meter_bg_color)   & 0xFFFFFF; return true; }
	if (strcmp(name, "border_color") == 0)       { out->color = lv_color_to32(md->border_color)     & 0xFFFFFF; return true; }
	if (strcmp(name, "border_width") == 0)       { out->i = md->border_width;        return true; }
	if (strcmp(name, "border_opa") == 0)         { out->i = md->border_opa;          return true; }
	if (strcmp(name, "scale_padding") == 0)      { out->i = md->scale_padding;       return true; }
	if (strcmp(name, "show_ticks") == 0)         { out->b = md->show_ticks;          return true; }
	if (strcmp(name, "show_tick_labels") == 0)   { out->b = md->show_tick_labels;    return true; }
	if (strcmp(name, "static_ticks") == 0)       { out->b = md->static_ticks;        return true; }
	if (strcmp(name, "label_gap") == 0)          { out->i = md->label_gap;           return true; }
	if (strcmp(name, "tick_label_color") == 0)   { out->color = lv_color_to32(md->tick_label_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "minor_tick_width") == 0)   { out->i = md->minor_tick_width;    return true; }
	if (strcmp(name, "minor_tick_length") == 0)  { out->i = md->minor_tick_length;   return true; }
	if (strcmp(name, "major_tick_width") == 0)   { out->i = md->major_tick_width;    return true; }
	if (strcmp(name, "major_tick_length") == 0)  { out->i = md->major_tick_length;   return true; }
	if (strcmp(name, "minor_tick_color") == 0)   { out->color = lv_color_to32(md->minor_tick_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "major_tick_color") == 0)   { out->color = lv_color_to32(md->major_tick_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "show_needle") == 0)        { out->b = md->show_needle;         return true; }
	if (strcmp(name, "show_needle_ball") == 0)   { out->b = md->show_needle_ball;    return true; }
	if (strcmp(name, "needle_width") == 0)       { out->i = md->needle_width;        return true; }
	if (strcmp(name, "needle_color") == 0)       { out->color = lv_color_to32(md->needle_color)     & 0xFFFFFF; return true; }
	if (strcmp(name, "needle_r_mod") == 0)       { out->i = md->needle_r_mod;        return true; }
	if (strcmp(name, "needle_rear_length") == 0) { out->i = md->needle_rear_length;  return true; }
	if (strcmp(name, "needle_tip_style") == 0)   { out->i = md->needle_tip_style;    return true; }
	if (strcmp(name, "needle_tip_base_w") == 0)  { out->i = md->needle_tip_base_w;   return true; }
	if (strcmp(name, "needle_tip_point_w") == 0) { out->i = md->needle_tip_point_w;  return true; }
	if (strcmp(name, "needle_tip_taper") == 0)   { out->i = md->needle_tip_taper;    return true; }
	if (strcmp(name, "needle_ball_size") == 0)   { out->i = md->needle_ball_size;    return true; }
	if (strcmp(name, "needle_ball_color") == 0)  { out->color = lv_color_to32(md->needle_ball_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "needle_pivot_x") == 0)     { out->i = md->needle_pivot_x;      return true; }
	if (strcmp(name, "needle_pivot_y") == 0)     { out->i = md->needle_pivot_y;      return true; }
	if (strcmp(name, "needle_angle_offset") == 0){ out->i = md->needle_angle_offset; return true; }
	if (strcmp(name, "shadow_enabled") == 0)     { out->b = md->shadow_enabled;      return true; }
	if (strcmp(name, "shadow_dynamic") == 0)     { out->b = md->shadow_dynamic;      return true; }
	if (strcmp(name, "shadow_offset_x") == 0)    { out->i = md->shadow_offset_x;     return true; }
	if (strcmp(name, "shadow_offset_y") == 0)    { out->i = md->shadow_offset_y;     return true; }
	if (strcmp(name, "shadow_opa") == 0)         { out->i = md->shadow_opa;          return true; }
	if (strcmp(name, "shadow_width_extra") == 0) { out->i = md->shadow_width_extra;  return true; }
	if (strcmp(name, "shadow_color") == 0)       { out->color = lv_color_to32(md->shadow_color) & 0xFFFFFF; return true; }
	return false;
}

static bool _meter_inspector_set(widget_t *w, const char *name,
                                 const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_METER || !w->type_data || !name || !in) return false;
	meter_data_t *md = (meter_data_t *)w->type_data;
	lv_obj_t *m = md->meter;
	#define LV_VALID(o) ((o) && lv_obj_is_valid(o))

	if (strcmp(name, "signal_name") == 0 && in->str) {
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;

		if (md->signal_index >= 0)
			signal_unsubscribe(md->signal_index, _meter_on_signal, w);
		safe_strncpy(md->signal_name, in->str, sizeof(md->signal_name));
		md->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _meter_on_signal, w);
		return true;
	}
	if (strcmp(name, "tick_label_font") == 0 && in->str) {
		safe_strncpy(md->tick_label_font, in->str, sizeof(md->tick_label_font));
		return true;
	}
	if (strcmp(name, "needle_image_name") == 0 && in->str) {
		safe_strncpy(md->needle_image_name, in->str, sizeof(md->needle_image_name));
		return true;
	}
	if (strcmp(name, "bg_image_name") == 0 && in->str) {
		safe_strncpy(md->bg_image_name, in->str, sizeof(md->bg_image_name));
		return true;
	}
	if (strcmp(name, "min") == 0) { md->min = (int32_t)in->i; return true; }
	if (strcmp(name, "max") == 0) { md->max = (int32_t)in->i; return true; }
	if (strcmp(name, "start_angle_user") == 0) {
		int v = in->i; v %= 360; if (v < 0) v += 360;
		md->start_angle = (int16_t)v;
		return true;
	}
	if (strcmp(name, "sweep_degrees") == 0) {
		int v = in->i; if (v < 1) v = 1; if (v > 360) v = 360;
		md->end_angle = (int16_t)((md->start_angle + v) % 360);
		return true;
	}
	if (strcmp(name, "reverse") == 0) { md->reverse = in->b; return true; }
	if (strcmp(name, "minor_tick_step") == 0) {
		int step = in->i;
		if (step < 1) step = 1;
		int32_t range = md->max - md->min;
		int count = range / step + 1;
		if (count < 2)   count = 2;
		if (count > 200) count = 200;
		md->minor_tick_count = (uint8_t)count;
		return true;
	}
	if (strcmp(name, "major_tick_step") == 0) {
		int mstep = in->i;
		if (mstep < 1) mstep = 1;
		int32_t range = md->max - md->min;
		int32_t denom = md->minor_tick_count > 1 ? md->minor_tick_count - 1 : 1;
		int32_t minor_step = range / denom;
		if (minor_step < 1) minor_step = 1;
		int every = mstep / minor_step;
		if (every < 1)   every = 1;
		if (every > 200) every = 200;
		md->major_tick_every = (uint8_t)every;
		return true;
	}
	if (strcmp(name, "anchor_enabled") == 0) { md->anchor_enabled = in->b; return true; }
	if (strcmp(name, "anchor_value") == 0)   { md->anchor_value = (int32_t)in->i; return true; }
	if (strcmp(name, "anchor_position") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 100) v = 100;
		md->anchor_position = (uint8_t)v;
		return true;
	}
	if (strcmp(name, "redline_enabled") == 0)        { md->redline_enabled = in->b; return true; }
	if (strcmp(name, "redline_threshold") == 0)      { md->redline_threshold = (int32_t)in->i; return true; }
	if (strcmp(name, "redline_show_arc") == 0)       { md->redline_show_arc = in->b; return true; }
	if (strcmp(name, "redline_arc_width") == 0)      { md->redline_arc_width = (uint8_t)in->i; return true; }
	if (strcmp(name, "redline_arc_r_mod") == 0)      { md->redline_arc_r_mod = (int16_t)in->i; return true; }
	if (strcmp(name, "redline_recolor_ticks") == 0)  { md->redline_recolor_ticks = in->b; return true; }
	if (strcmp(name, "redline_color") == 0)          { md->redline_color = lv_color_hex(in->color); return true; }
	if (strcmp(name, "meter_bg_color") == 0) {
		md->meter_bg_color = lv_color_hex(in->color);
		if (LV_VALID(m)) lv_obj_set_style_bg_color(m, md->meter_bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "meter_bg_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		md->meter_bg_opa = (uint8_t)v;
		if (LV_VALID(m)) lv_obj_set_style_bg_opa(m, md->meter_bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "border_color") == 0) {
		md->border_color = lv_color_hex(in->color);
		if (LV_VALID(m)) lv_obj_set_style_border_color(m, md->border_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "border_width") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 20) v = 20;
		md->border_width = (uint8_t)v;
		if (LV_VALID(m)) lv_obj_set_style_border_width(m, md->border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "border_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		md->border_opa = (uint8_t)v;
		if (LV_VALID(m)) lv_obj_set_style_border_opa(m, md->border_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "scale_padding") == 0)    { md->scale_padding = (uint8_t)in->i; return true; }
	if (strcmp(name, "show_ticks") == 0)       { md->show_ticks = in->b;             return true; }
	if (strcmp(name, "show_tick_labels") == 0) { md->show_tick_labels = in->b;       return true; }
	if (strcmp(name, "static_ticks") == 0)     {
		/* Toggling static_ticks live requires rebuilding the meter
		 * (the snapshot path can't be retrofitted on a live needle).
		 * Mark the value; the dashboard reloads on layout save and
		 * the next create call honours the new setting. */
		md->static_ticks = in->b;
		return true;
	}
	if (strcmp(name, "label_gap") == 0)        { md->label_gap = (int16_t)in->i;     return true; }
	if (strcmp(name, "tick_label_color") == 0) { md->tick_label_color = lv_color_hex(in->color); return true; }
	if (strcmp(name, "minor_tick_width") == 0)  { md->minor_tick_width = (uint8_t)in->i;  return true; }
	if (strcmp(name, "minor_tick_length") == 0) { md->minor_tick_length = (uint8_t)in->i; return true; }
	if (strcmp(name, "major_tick_width") == 0)  { md->major_tick_width = (uint8_t)in->i;  return true; }
	if (strcmp(name, "major_tick_length") == 0) { md->major_tick_length = (uint8_t)in->i; return true; }
	if (strcmp(name, "minor_tick_color") == 0)  { md->minor_tick_color = lv_color_hex(in->color); return true; }
	if (strcmp(name, "major_tick_color") == 0)  { md->major_tick_color = lv_color_hex(in->color); return true; }
	/* show_needle / show_needle_ball create or skip indicators at meter-build
	 * time (LVGL v8 has no remove-indicator API), so toggling them needs a
	 * meter rebuild. Store the value; the dashboard reloads on layout save and
	 * the next create call honours it — same approach as static_ticks/shadow. */
	if (strcmp(name, "show_needle") == 0)         { md->show_needle = in->b;                return true; }
	if (strcmp(name, "show_needle_ball") == 0)    { md->show_needle_ball = in->b;           return true; }
	if (strcmp(name, "needle_width") == 0)        { md->needle_width = (uint8_t)in->i;      return true; }
	if (strcmp(name, "needle_color") == 0)        { md->needle_color = lv_color_hex(in->color); return true; }
	if (strcmp(name, "needle_r_mod") == 0)        { md->needle_r_mod = (int16_t)in->i;      return true; }
	if (strcmp(name, "needle_rear_length") == 0)  { md->needle_rear_length = (uint8_t)in->i; return true; }
	if (strcmp(name, "needle_tip_style") == 0)    { md->needle_tip_style = (uint8_t)in->i;  return true; }
	if (strcmp(name, "needle_tip_base_w") == 0)   { md->needle_tip_base_w = (uint8_t)in->i; return true; }
	if (strcmp(name, "needle_tip_point_w") == 0)  { md->needle_tip_point_w = (uint8_t)in->i; return true; }
	if (strcmp(name, "needle_tip_taper") == 0)    { md->needle_tip_taper = (uint8_t)in->i;  return true; }
	if (strcmp(name, "needle_ball_size") == 0)    { md->needle_ball_size = (uint8_t)in->i;  return true; }
	if (strcmp(name, "needle_ball_color") == 0) {
		md->needle_ball_color = lv_color_hex(in->color);
		if (LV_VALID(m))
			lv_obj_set_style_bg_color(m, md->needle_ball_color, LV_PART_INDICATOR);
		return true;
	}
	if (strcmp(name, "needle_pivot_x") == 0)      { md->needle_pivot_x = (int16_t)in->i;     return true; }
	if (strcmp(name, "needle_pivot_y") == 0)      { md->needle_pivot_y = (int16_t)in->i;     return true; }
	if (strcmp(name, "needle_angle_offset") == 0) { md->needle_angle_offset = (int16_t)in->i; return true; }
	/* Shadow — toggling shadow_enabled needs a meter rebuild because the
	 * shadow indicator is created at meter-build time. The other knobs
	 * (offset/opa/width/color) take effect on the next redraw because the
	 * draw hook reads md directly. Layouts reload on save, so the toggle
	 * reaches the live meter that way. */
	if (strcmp(name, "shadow_enabled") == 0)      { md->shadow_enabled = in->b;              return true; }
	if (strcmp(name, "shadow_dynamic") == 0) {
		md->shadow_dynamic = in->b;
		if (LV_VALID(m)) lv_obj_invalidate(m);
		return true;
	}
	if (strcmp(name, "shadow_offset_x") == 0) {
		int v = in->i; if (v < -32) v = -32; if (v > 32) v = 32;
		md->shadow_offset_x = (int8_t)v;
		if (LV_VALID(m)) lv_obj_refresh_ext_draw_size(m);
		return true;
	}
	if (strcmp(name, "shadow_offset_y") == 0) {
		int v = in->i; if (v < -32) v = -32; if (v > 32) v = 32;
		md->shadow_offset_y = (int8_t)v;
		if (LV_VALID(m)) lv_obj_refresh_ext_draw_size(m);
		return true;
	}
	if (strcmp(name, "shadow_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		md->shadow_opa = (uint8_t)v;
		if (LV_VALID(m)) lv_obj_invalidate(m);
		return true;
	}
	if (strcmp(name, "shadow_width_extra") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 32) v = 32;
		md->shadow_width_extra = (uint8_t)v;
		if (LV_VALID(m)) lv_obj_refresh_ext_draw_size(m);
		return true;
	}
	if (strcmp(name, "shadow_color") == 0) {
		md->shadow_color = lv_color_hex(in->color);
		if (LV_VALID(m)) lv_obj_invalidate(m);
		return true;
	}
	#undef LV_VALID
	return false;
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
	md->value_scale = 1;        /* 10^decimals; overridden when a channel binds */
	md->value_decimals = 0;

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
	md->needle_tip_style   = 0;
	md->needle_tip_base_w  = 0;
	md->needle_tip_point_w = 0;
	md->needle_tip_taper   = 0;
	md->show_needle = true;
	md->show_needle_ball = true;
	md->needle_ball_size = 10;
	md->needle_ball_color = lv_color_white();
	/* Drop shadow defaults — disabled, modest offset + half-transparency
	 * for a soft drop look without overpowering the needle once enabled. */
	md->shadow_enabled     = false;
	md->shadow_dynamic     = true;
	md->shadow_offset_x    = 3;
	md->shadow_offset_y    = 4;
	md->shadow_opa         = 120;
	md->shadow_width_extra = 2;
	md->shadow_color       = lv_color_black();
	/* Tick label defaults */
	md->tick_label_color = lv_color_white();
	md->tick_label_divisor = 1;
	md->show_ticks = true;
	md->show_tick_labels = true;
	/* Static-tick optimisation is ON by default: the face (ticks / labels /
	 * bg / redline arc) is rendered once into a PSRAM snapshot and only the
	 * needle stays live. The historical "sizing / z-order issues" turned out
	 * to be a single defect — the theme's default centre knob baked into the
	 * snapshot (fixed in _meter_flatten_static_ticks); pixel-diff parity at
	 * 140/240/450 px is verified by tools/meter_visual_parity.py. The flag
	 * remains as a per-meter opt-out for debugging. */
	md->static_ticks = true;
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
	/* Redline defaults — match web UI defs (0xFF0000 red, threshold 80,
	 * arc width 6 px, show_arc + recolor_ticks ON so toggling redline_enabled
	 * gives an immediate visible result). */
	md->redline_enabled       = false;
	md->redline_threshold     = 80;
	md->redline_color         = lv_color_hex(0xFF0000);
	md->redline_show_arc      = true;
	md->redline_arc_width     = 6;
	md->redline_arc_r_mod     = 0;
	md->redline_recolor_ticks = true;
	/* Anchor curve defaults — disabled, with a no-op midpoint mapping. */
	md->anchor_enabled  = false;
	md->anchor_value    = 50;
	md->anchor_position = 50;
	/* Reverse axis — off by default (low=start, high=end). */
	md->reverse         = false;

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
	w->inspector_get = _meter_inspector_get;
	w->inspector_set = _meter_inspector_set;

	return w;
}
