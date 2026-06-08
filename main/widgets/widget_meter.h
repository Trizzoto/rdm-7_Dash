#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Night-mode overrides for meter ────────────────────────────────────── */
typedef struct {
	NIGHT_FIELD_COLOR(minor_tick_color)
	NIGHT_FIELD_COLOR(major_tick_color)
	NIGHT_FIELD_COLOR(needle_color)
	NIGHT_FIELD_COLOR(needle_ball_color)
	NIGHT_FIELD_COLOR(border_color)
	NIGHT_FIELD_COLOR(meter_bg_color)
	NIGHT_FIELD_COLOR(tick_label_color)
	NIGHT_FIELD_IMAGE(needle_image_name, 32)
	NIGHT_FIELD_IMAGE(bg_image_name, 32)
} meter_night_overrides_t;

/* ── Per-instance state for meter widget ───────────────────────────────── */
typedef struct {
	uint8_t value_idx;
	/* Display range in float units. The LVGL meter scale is integer, so
	 * min/max/value are multiplied by value_scale (=10^value_decimals) only
	 * at the lv_meter_* boundary; tick labels divide back. value_scale
	 * defaults to 1 (integer behaviour) — decimals come from the bound
	 * channel via _meter_apply_channel. */
	float   min;
	float   max;
	int32_t value_scale;            /* 10^value_decimals; 1 = no decimals */
	uint8_t value_decimals;         /* 0..3 — drives tick-label precision */
	int16_t start_angle;
	int16_t end_angle;
	lv_obj_t *meter;
	lv_meter_scale_t *scale;
	lv_meter_indicator_t *needle;
	/* ── Appearance overrides ── */
	/* Ticks */
	uint8_t    minor_tick_count;     /* default: 21 */
	uint8_t    major_tick_every;     /* default: 5 */
	uint8_t    minor_tick_width;     /* default: 2 */
	uint8_t    minor_tick_length;    /* default: 10 */
	uint8_t    major_tick_width;     /* default: 4 */
	uint8_t    major_tick_length;    /* default: 15 */
	lv_color_t minor_tick_color;     /* default: LV_PALETTE_GREY */
	lv_color_t major_tick_color;     /* default: white (0xFFFFFF) */
	/* Needle */
	uint8_t    needle_width;         /* default: 4 */
	lv_color_t needle_color;         /* default: white (0xFFFFFF) */
	int16_t    needle_r_mod;         /* default: -10 */
	/* Rear extension behind the pivot, in pixels. Drawn in the same color and
	 * width as the needle line; works alongside any tip style. 0 = no rear. */
	uint8_t    needle_rear_length;   /* default: 0 */
	/* Tip style for line needles (ignored when needle_image_name is set):
	 *   0 = Flat    (plain line end, LVGL default)
	 *   1 = Rounded (soft round caps)
	 *   2 = Lance   (uniform shaft + short tapered tip)
	 *   3 = Dagger  (full taper from wide base to sharp point)
	 *   4 = Spade   (full taper with a blunt flat cap at the tip)
	 *   5 = Diamond (dauphine/rhombus — pointed at both ends, wide in the middle)
	 * Rendered via an LV_EVENT_DRAW_PART hook in widget_meter.c. */
	uint8_t    needle_tip_style;     /* default: 0 */
	/* Per-style tuning knobs. All default to 0 meaning "use the built-in
	 * default for the selected style" — so a plain style change needs no
	 * extra config, and dialing in uses familiar pixel / percent units:
	 *   needle_tip_base_w  — half-width at the wide end (px)
	 *   needle_tip_point_w — half-width at the pointed end / cap (px)
	 *   needle_tip_taper   — percent along the needle where narrowing begins
	 *                        (Lance shaft end, Spade cap position, Diamond
	 *                        widest-point location) */
	uint8_t    needle_tip_base_w;    /* default: 0 (auto) */
	uint8_t    needle_tip_point_w;   /* default: 0 (auto) */
	uint8_t    needle_tip_taper;     /* default: 0 (auto), range 1-100 */
	/* Needle drop shadow. Renders a colored, offset copy of the needle
	 * silhouette BEHIND the main needle. Works for all line-needle tip
	 * styles (Flat / Rounded / Lance / Dagger / Spade / Diamond) and
	 * picks up the rear-extension when set. Image needles do NOT receive
	 * a shadow (LVGL v8 can't easily tint a rotated img). The shadow is
	 * a real lv_meter indicator added before the main needle so it
	 * rotates and updates in lockstep. */
	bool       shadow_enabled;       /* default: false */
	/* When true, shadow offset magnitude scales with sin(angle_from_vertical)
	 * — invisible when needle is vertical (12 or 6 o'clock), full at 3 / 9.
	 * Mimics an overhead light source casting the needle's silhouette
	 * sideways as it tilts off-axis. When false, the offset is constant
	 * (classic drop shadow). Default true. */
	bool       shadow_dynamic;       /* default: true */
	int8_t     shadow_offset_x;      /* default: 3 (positive = right) */
	int8_t     shadow_offset_y;      /* default: 4 (positive = down) */
	uint8_t    shadow_opa;           /* default: 120 (0..255) */
	uint8_t    shadow_width_extra;   /* default: 2 (added to needle_width) */
	lv_color_t shadow_color;         /* default: 0x000000 */
	/* Visibility toggles. Both default true; independent of each other so a
	 * meter can show the ball with no needle, the needle with no ball, or
	 * neither. show_needle off skips the needle line/image (and its shadow)
	 * entirely; show_needle_ball off hides the center pivot ball regardless
	 * of needle_ball_size. */
	bool       show_needle;          /* default: true */
	bool       show_needle_ball;     /* default: true */
	/* Needle center ball (LV_PART_INDICATOR) */
	uint8_t    needle_ball_size;     /* default: 10 (diameter in px, 0 = hidden) */
	lv_color_t needle_ball_color;    /* default: white (0xFFFFFF) */
	/* Needle image (overrides line needle when set) */
	char           needle_image_name[32]; /* RDMIMG name, empty = use line needle */
	int16_t        needle_pivot_x;       /* pivot X in image pixels, default: 0 */
	int16_t        needle_pivot_y;       /* pivot Y in image pixels, default: 0 */
	int16_t        needle_angle_offset;  /* degrees, default: 0 — rotates needle via separate scale */
	lv_img_dsc_t  *needle_img_dsc;       /* runtime: loaded needle image */
	lv_meter_scale_t *needle_scale;      /* runtime: separate scale when angle offset != 0 */
	/* Background image (rendered behind meter content) */
	char           bg_image_name[32];    /* RDMIMG name, empty = solid color bg */
	lv_img_dsc_t  *bg_img_dsc;           /* runtime: loaded background image */
	/* Border */
	lv_color_t border_color;         /* default: 0x000000 */
	uint8_t    border_width;         /* default: 0 (no border) */
	uint8_t    border_opa;           /* default: 255 */
	/* Background */
	lv_color_t meter_bg_color;       /* default: 0x3D3D3D (LVGL meter default) */
	uint8_t    meter_bg_opa;         /* default: 255 */
	/* Scale layout */
	uint8_t    scale_padding;        /* default: 0 — pushes tick ring inward from border */
	int16_t    label_gap;            /* default: 10 — distance from major tick to label (range −150..+150) */
	char       tick_label_font[32];  /* default: "" — font for tick value labels */
	lv_color_t tick_label_color;    /* default: white (0xFFFFFF) */
	/* Tick label denomination: divides the displayed tick value by this
	 * factor. Useful for tachs (range 0..7000 with divisor=1000 reads
	 * "0..7") and similar high-magnitude scales where the user wants short
	 * labels and prints the unit elsewhere ("RPM x1000"). Default 1 = no
	 * change. Applies to all tick labels, regardless of anchor/reverse. */
	uint16_t   tick_label_divisor;  /* default: 1 */
	bool       show_ticks;          /* default: true — hide minor+major tick marks entirely */
	bool       show_tick_labels;    /* default: true — hide numeric labels at major ticks */
	/* Static-tick optimisation. When true, the meter snapshots its
	 * tick / label / background / redline-arc layer ONCE at create
	 * time, drops those parts off the live lv_meter (widths → 0,
	 * label opa → transparent, bg opa → 0, redline arc removed), and
	 * shows the snapshot as a sibling lv_img beneath the meter. The
	 * lv_meter then redraws only the needle on each signal tick
	 * instead of recomputing every tick that overlaps the needle
	 * bbox, which is the perf cliff people hit on tick-heavy
	 * dashboards.
	 *
	 * Trade-offs:
	 *   • ~125 KB PSRAM per meter for the snapshot (200×200×2 bytes
	 *     RGB565). Bounded — the firmware enforces a 4-meter cap.
	 *   • Edits to tick / label / redline fields rebuild the
	 *     snapshot inline (single-digit ms via lv_snapshot_take).
	 *     Needle / signal_name edits don't touch the snapshot.
	 *   • Night-mode meters get their own snapshot when built.
	 *   • Resize tears the snapshot down and rebuilds on the next
	 *     value tick.
	 *
	 * Default true because the perf win matters; flip off per-meter
	 * if a snapshot edge-case shows up (e.g. fonts not yet loaded
	 * when snapshot was taken). */
	bool       static_ticks;        /* default: true */
	/* Runtime snapshot state. The snapshot image data is heap-
	 * allocated by lv_snapshot_take and must be released with
	 * lv_snapshot_free. The snapshot is applied as the meter's own
	 * bg_img_src style — no sibling lv_img, no z-order problems. */
	lv_img_dsc_t *tick_snapshot_dsc;
	lv_img_dsc_t *night_tick_snapshot_dsc;
	/* Stored pointers to the redline-arc indicators on the day and
	 * night meters. Needed by the static-tick flatten path so we can
	 * collapse them to zero-length AFTER taking the snapshot (lv_meter
	 * v8 has no remove_indicator API, so leaving them alive would
	 * double-render against the redline already baked into the
	 * snapshot). NULL when no redline arc was added. */
	lv_meter_indicator_t *redline_arc;
	lv_meter_indicator_t *night_redline_arc;
	/* Redline zone — visual emphasis for "danger" range [threshold, max]. */
	bool       redline_enabled;     /* default: false — master toggle */
	float      redline_threshold;   /* default: 80 — value at which the zone starts */
	lv_color_t redline_color;       /* default: red (0xFF0000) */
	bool       redline_show_arc;    /* default: true — draw a colored arc segment */
	uint8_t    redline_arc_width;   /* default: 6 — arc thickness in px */
	int16_t    redline_arc_r_mod;   /* default: 0 — radius offset from scale */
	bool       redline_recolor_ticks; /* default: true — repaint ticks ≥ threshold */
	/* Anchor curve — piecewise-linear value→angle mapping. When enabled the
	 * needle treats `anchor_value` as landing at `anchor_position`% of the
	 * sweep, giving two linear segments instead of one. Tick label positions
	 * remain linear (LVGL v8 scale is single-segment). */
	bool       anchor_enabled;       /* default: false */
	float      anchor_value;         /* default: 50 */
	uint8_t    anchor_position;      /* default: 50 — percent of sweep */
	/* Reverse — flip the value axis. With reverse=true, max sits at the
	 * START of the sweep and min at the END. Combines cleanly with anchor
	 * (anchor curve is applied first, then the result is mirrored). */
	bool       reverse;              /* default: false */
	char     signal_name[32];
	int16_t  signal_index;
	/* ── v14 channel binding ───────────────────────────────────────
	 * When `channel_id` is set, this meter pulls its data semantics
	 * (signal_name, min, max, redline_threshold, redline_color) from
	 * the named channel instead of carrying them per-widget. Visual
	 * style (needle, shadow, fonts, etc.) stays widget-owned.
	 *
	 * Backwards compat: v13 layouts have channel_id == "" and use the
	 * legacy per-widget fields above. On v13 load, the widget self-
	 * reports its values to channel_manager_record_legacy_widget() so
	 * the channel registry auto-populates from existing layout data —
	 * users keep their custom redlines on first v14 boot.
	 *
	 * The `channel` pointer is cached at create-time so the render
	 * hot path never does string lookups. It's NULL when binding is
	 * legacy mode. */
	char     channel_id[32];
	void    *channel;             /* channel_t* — opaque to avoid
	                                  pulling channel_manager.h into
	                                  this header */
	/* Night-mode appearance overrides */
	meter_night_overrides_t night;
	/* Night-mode dual-meter pattern: when any "baked-in" night property is
	 * set (tick colors, line needle color, needle image, bg image), a second
	 * meter is created with those night values baked in. apply_night_mode()
	 * toggles visibility instead of mutating live. The signal callback
	 * updates both needles in lock-step. */
	lv_obj_t            *night_meter;        /* sibling night meter (or NULL) */
	lv_meter_scale_t    *night_scale;
	lv_meter_indicator_t *night_needle;
	lv_meter_scale_t    *night_needle_scale; /* for offset-rotated needle */
	lv_img_dsc_t        *night_needle_img_dsc;
	lv_img_dsc_t        *night_bg_img_dsc;
	/* Shadow indicators — created when shadow_enabled and the needle is a
	 * line (not image). Drawn under the main needle by linked-list order. */
	lv_meter_indicator_t *shadow_needle;
	lv_meter_indicator_t *night_shadow_needle;
} meter_data_t;

/**
 * Create an analog meter widget bound to a value slot.
 *
 * @param value_idx  Value slot 0–10 (panel0–7, RPM, BAR1, BAR2).
 * @return           Heap-allocated widget_t*, caller must call w->destroy(w).
 */
widget_t *widget_meter_create_instance(uint8_t value_idx);

/** Return value index for meter widget. */
uint8_t widget_meter_get_value_idx(const widget_t *w);

#ifdef __cplusplus
}
#endif
