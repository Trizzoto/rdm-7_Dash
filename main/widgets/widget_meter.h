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
	lv_obj_t *value_label;
	lv_obj_t *id_label;
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
	/* Value label */
	bool       show_value;           /* default: true */
	int8_t     value_x_offset;       /* default: 0 */
	int8_t     value_y_offset;       /* default: 20 */
	lv_color_t value_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	/* ID label */
	bool       show_id_label;        /* default: true */
	int8_t     id_x_offset;          /* default: 0 */
	int8_t     id_y_offset;          /* default: 45 */
	lv_color_t id_label_color;       /* default: THEME_COLOR_TEXT_MUTED */
	/* Background */
	lv_color_t meter_bg_color;       /* default: 0x3D3D3D (LVGL meter default) */
	uint8_t    meter_bg_opa;         /* default: 255 */
	/* Arc color zones (up to 3) */
	bool       arc_zone1_enabled;    /* default: false */
	int32_t    arc_zone1_start;
	int32_t    arc_zone1_end;
	lv_color_t arc_zone1_color;      /* default: green */
	bool       arc_zone2_enabled;    /* default: false */
	int32_t    arc_zone2_start;
	int32_t    arc_zone2_end;
	lv_color_t arc_zone2_color;      /* default: yellow */
	bool       arc_zone3_enabled;    /* default: false */
	int32_t    arc_zone3_start;
	int32_t    arc_zone3_end;
	lv_color_t arc_zone3_color;      /* default: red */
	char     label_font[32];
	char     value_font[32];
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
