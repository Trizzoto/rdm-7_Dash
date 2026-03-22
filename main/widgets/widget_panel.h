#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Per-instance state for panel widgets ──────────────────────────────── */
typedef struct {
	uint8_t    slot;
	char       label[64];
	char       custom_text[32];
	uint8_t    decimals;
	bool       warning_high_enabled;
	float      warning_high_threshold;
	lv_color_t warning_high_color;
	bool       warning_high_apply_label;
	bool       warning_high_apply_value;
	bool       warning_high_apply_panel;
	bool       warning_low_enabled;
	float      warning_low_threshold;
	lv_color_t warning_low_color;
	bool       warning_low_apply_label;
	bool       warning_low_apply_value;
	bool       warning_low_apply_panel;
	char       label_font[32];
	char       value_font[32];
	/* ── Appearance overrides (defaults match legacy shared box_style) ── */
	uint8_t    border_radius;        /* default: 7 */
	uint8_t    border_width;         /* default: 3 */
	lv_color_t border_color;         /* default: THEME_COLOR_PANEL */
	lv_color_t bg_color;             /* default: THEME_COLOR_BG */
	uint8_t    bg_opa;               /* default: 255 */
	lv_color_t label_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	lv_color_t value_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	int8_t     label_y_offset;       /* default: -28 */
	int8_t     value_y_offset;       /* default: 9 */
	int8_t     custom_text_x_offset; /* default: 41 */
	int8_t     custom_text_y_offset; /* default: 32 */
	char       signal_name[32];
	int16_t    signal_index;
	/* LVGL object pointers (runtime only) */
	lv_obj_t  *box;
	lv_obj_t  *header_label;
	lv_obj_t  *value_label;
	lv_obj_t  *custom_text_label;
} panel_data_t;

/** Initialise shared LVGL styles (box_style, common_style). Call once at boot.
 */
void init_styles(void);

/** Initialise the "common" (input form) style. */
void init_common_style(void);

/** Return a pointer to the shared common_style. */
lv_style_t *get_common_style(void);

/** Return a pointer to the shared box_style. */
lv_style_t *get_box_style(void);

/** Apply consistent roller styles. */
void apply_common_roller_styles(lv_obj_t *roller);

/** Create all 8 panel boxes, labels, value labels and custom text labels. */
void widget_panel_create(lv_obj_t *parent);

/** Shared panel shape helper used by widget_rpm_bar to build the gauge
 * surround. */
lv_obj_t *create_panel(lv_obj_t *parent, int width, int height, int x, int y,
					   int radius, lv_color_t bg_color, int transform_angle);

/** Immediate panel UI update (called on LVGL thread). */
void update_panel_ui_immediate(uint8_t i, const char *value_str,
							   double final_value);

/** Returns pointer to last_panel_can_received[idx] for timeout tracking. */
uint64_t *widget_panel_get_last_can_time(uint8_t idx);

/**
 * Phase 2 — Factory function.
 * Allocates and returns a widget_t wired with the panel vtable.
 * @param slot  Panel index 0-7. Determines the default position/size and id.
 * @return      Heap-allocated widget_t *, caller must eventually call
 * w->destroy(w).
 */
widget_t *widget_panel_create_instance(uint8_t slot);

uint8_t widget_panel_get_slot(const widget_t *w);
bool widget_panel_has_signal(const widget_t *w);

/** Set panel warning thresholds from config callbacks. */
void widget_panel_set_warning_high(uint8_t slot, float threshold, bool enabled);
void widget_panel_set_warning_low(uint8_t slot, float threshold, bool enabled);
void widget_panel_set_warning_high_color(uint8_t slot, lv_color_t color);
void widget_panel_set_warning_low_color(uint8_t slot, lv_color_t color);

#ifdef __cplusplus
}
#endif
