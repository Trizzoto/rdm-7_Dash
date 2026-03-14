#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Per-instance state for bar widgets ────────────────────────────────── */
typedef struct {
	uint8_t  slot;              /* 0=BAR1, 1=BAR2 */
	char     label[32];
	int32_t  bar_min;
	int32_t  bar_max;
	int32_t  bar_low;
	int32_t  bar_high;
	lv_color_t bar_low_color;
	lv_color_t bar_high_color;
	lv_color_t bar_in_range_color;
	bool     show_bar_value;
	bool     invert_bar_value;
	bool     fuel_sender;
	float    fuel_sender_empty_v;
	float    fuel_sender_full_v;
	uint8_t  fuel_sender_filter;
	uint8_t  decimals;
	char     label_font[32];
	char     value_font[32];
	/* ── Appearance overrides ── */
	lv_color_t bar_bg_color;         /* default: THEME_COLOR_PANEL */
	uint8_t    bar_radius;           /* default: 5 */
	uint8_t    bar_border_width;     /* default: 2 */
	lv_color_t bar_border_color;     /* default: THEME_COLOR_PANEL */
	uint8_t    indicator_radius;     /* default: 5 */
	lv_color_t label_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	lv_color_t value_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	char     signal_name[32];
	int16_t  signal_index;
	/* LVGL object pointers (runtime only, per-instance) */
	lv_obj_t *bar_obj;
	lv_obj_t *label_obj;
	lv_obj_t *value_obj;
} bar_data_t;

/* previous_bar_values is also used by widget_dispatcher; expose it */
extern float previous_bar_values[2];

/** Create BAR1 and BAR2 horizontal bar widgets and their labels on parent. */
void widget_bar_create(lv_obj_t *parent);

/** Immediate bar UI update. bar_index: 0=BAR1, 1=BAR2. */
void update_bar_ui_immediate(int bar_index, int32_t bar_value,
							 double final_value, int config_index);

/** Async bar UI update (lv_async_call compatible). */
void update_bar_ui(void *param);

/** Color-wheel popup creators for bar thresholds. */
void create_bar_low_color_wheel_popup(uint8_t value_id);
void create_bar_high_color_wheel_popup(uint8_t value_id);
void create_bar_in_range_color_wheel_popup(uint8_t value_id);

/** Config callbacks for bar range/threshold inputs (used by config_modal). */
void bar_range_input_event_cb(lv_event_t *e);
void bar_low_value_event_cb(lv_event_t *e);
void bar_high_value_event_cb(lv_event_t *e);
void bar_low_color_event_cb(lv_event_t *e);
void bar_high_color_event_cb(lv_event_t *e);
void bar_in_range_color_event_cb(lv_event_t *e);

/** Returns pointer to last_bar_can_received[bar_idx] for timeout tracking. */
uint64_t *widget_bar_get_last_can_time(uint8_t bar_idx);

/* Bar value labels are defined here; ui.h already declares ui_Bar_1_Value etc.
   but the actual definitions live in widget_bar.c */
extern lv_obj_t *ui_Bar_1_Value;
extern lv_obj_t *ui_Bar_2_Value;

/**
 * Phase 2 — Factory function.
 * Allocates and returns a widget_t wired with the bar vtable.
 * @param slot  0 = BAR1, 1 = BAR2.
 * @return      Heap-allocated widget_t *, caller must eventually call
 * w->destroy(w).
 */
widget_t *widget_bar_create_instance(uint8_t slot);

uint8_t widget_bar_get_slot(const widget_t *w);
bool widget_bar_has_signal(const widget_t *w);

#ifdef __cplusplus
}
#endif
