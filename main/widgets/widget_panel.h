#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Night-mode overrides for panel ─────────────────────────────────────── */
typedef struct {
	NIGHT_FIELD_COLOR(border_color)
	NIGHT_FIELD_COLOR(bg_color)
	NIGHT_FIELD_COLOR(label_color)
	NIGHT_FIELD_COLOR(value_color)
} panel_night_overrides_t;

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
	/* Peak hold display: when non-zero, render a small line below the value.
	 *   0 = off (default)
	 *   1 = MAX  ("MAX 7184")
	 *   2 = MIN  ("MIN 0")
	 *   3 = both ("MIN 0 / MAX 7184")
	 * Reads SESSION peaks from the signal layer (signal_t.session_peak/min)
	 * — these reset on every boot. The Peaks screen shows the all-time
	 * persisted peaks instead. Reset is signal-wide via signal_reset_peak. */
	uint8_t    show_peak;
	char       signal_name[32];
	int16_t    signal_index;
	/* LVGL object pointers (runtime only) */
	lv_obj_t  *box;
	lv_obj_t  *header_label;
	lv_obj_t  *value_label;
	lv_obj_t  *custom_text_label;
	lv_obj_t  *peak_label;            /* runtime: small "MAX X" line under value */
	/* Display-state cache: used by _panel_on_signal to early-out when
	 * a new signal update would render the same thing as last time.
	 * Collapses a 60 Hz restyle storm to just the visible transitions. */
	char       last_display[32];
	uint8_t    last_warn_state;  /* bit0=label bit1=value bit2=panel bit7=stale */
	/* Night-mode appearance overrides (only applied when night_mode active) */
	panel_night_overrides_t night;
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
