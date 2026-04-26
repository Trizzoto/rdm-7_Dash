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
	int32_t min;
	int32_t max;
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
	int8_t     label_gap;            /* default: 10 — distance from major tick to label */
	char       tick_label_font[32];  /* default: "" — font for tick value labels */
	lv_color_t tick_label_color;    /* default: white (0xFFFFFF) */
	bool       show_ticks;          /* default: true — hide minor+major tick marks entirely */
	bool       show_tick_labels;    /* default: true — hide numeric labels at major ticks */
	/* Anchor-based non-linear scale. Maps a single DATA value to a specific
	 * position along the angular sweep, splitting the range into two linear
	 * segments. e.g. "anchor coolant 90°C at 50% of the sweep" makes the
	 * over-90 zone visually expanded. Default off = linear pass-through.
	 * Tick labels stay linear — only the needle is warped. */
	int32_t    anchor_value;
	uint8_t    anchor_position;     /* 0..100 along sweep angle, default 50 */
	bool       anchor_enabled;      /* false = linear pass-through */
	/* Redline zone: visual cues drawn between [redline_threshold..max]. Each
	 * effect maps to one LVGL meter indicator added in _meter_create:
	 *   - redline_show_arc      → lv_meter_add_arc (colored arc segment)
	 *   - redline_recolor_ticks → lv_meter_add_scale_lines (tick recolor)
	 * Both are static segments — no per-frame work after creation. */
	bool       redline_enabled;
	int32_t    redline_threshold;
	lv_color_t redline_color;
	bool       redline_show_arc;
	bool       redline_recolor_ticks;
	uint8_t    redline_arc_width;   /* default: 6 */
	int8_t     redline_arc_r_mod;   /* default: 0, range: -50..50 */
	lv_meter_indicator_t *redline_arc_indic;   /* runtime LVGL ptr */
	lv_meter_indicator_t *redline_tick_indic;  /* runtime LVGL ptr */
	/* Optional rear extension of a line needle: draws a second short line
	 * pointing 180° away from the needle (counterweight tail). 0 = disabled. */
	uint8_t    needle_rear_length;  /* default: 0, range 0..100 px */
	char     signal_name[32];
	int16_t  signal_index;
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
