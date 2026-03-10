#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* --- Objects exposed externally ------------------------------------------*/
/* warning_circles and warning_labels are file-scope statics in
   widget_warning.c; access is via update functions below. */

/* --- API ------------------------------------------------------------------*/
/** Initialise warning_configs[] to safe defaults. Call before NVS load. */
void init_warning_configs(void);

/** Create all 8 warning circles, labels and transparent touch zones on parent.
 */
void widget_warning_create(lv_obj_t *parent);

/** Async (lv_async_call-compatible) UI update for a single warning. */
void update_warning_ui(void *param);

/** Immediate (same-task) UI update for a single warning. */
void update_warning_ui_immediate(uint8_t warning_idx);

/** Timer callback — currently a stub (kept for future use). */
void check_warning_timeouts(lv_timer_t *timer);

/** Create the full-screen warning configuration editor. */
void create_warning_config_menu(uint8_t warning_idx);

/* Warning-specific config callbacks exposed for config_controls.c ----------*/
void warning_high_threshold_event_cb(lv_event_t *e);
void warning_low_threshold_event_cb(lv_event_t *e);
void warning_high_color_event_cb(lv_event_t *e);
void warning_low_color_event_cb(lv_event_t *e);

/* Color-wheel popup creators -----------------------------------------------*/
void create_bar_low_color_wheel_popup(uint8_t value_id);
void create_bar_high_color_wheel_popup(uint8_t value_id);
void create_bar_in_range_color_wheel_popup(uint8_t value_id);

/**
 * Phase 2 — Factory function.
 * Allocates and returns a widget_t wired with the warning vtable.
 * @param slot  Warning circle index 0–7.
 * @return      Heap-allocated widget_t *, caller must eventually call
 * w->destroy(w).
 */
widget_t *widget_warning_create_instance(uint8_t slot);

/** Reset all warning circle LVGL pointers (call before re-creating layout). */
void widget_warning_reset(void);

/** Return the slot (0-7) from a warning widget's type_data. */
uint8_t widget_warning_get_slot(const widget_t *w);

/** Return true if this warning widget is bound to a signal. */
bool widget_warning_has_signal(const widget_t *w);

#ifdef __cplusplus
}
#endif
