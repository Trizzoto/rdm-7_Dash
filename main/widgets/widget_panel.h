#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#ifdef __cplusplus
extern "C" {
#endif

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

/** Create a transparent click zone that opens the config menu on long-press. */
void create_transparent_click_zone(lv_obj_t *parent, lv_obj_t *target_label,
								   uint8_t value_id);

/** Immediate panel UI update (called on LVGL thread). */
void update_panel_ui_immediate(uint8_t i, const char *value_str,
							   double final_value);

/** Async panel UI update (lv_async_call compatible). */
void update_panel_ui(void *param);

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
