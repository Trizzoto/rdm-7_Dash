#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
	char     signal_name[32];
	int16_t  signal_index;
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
