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
	char     signal_name[32];
	int16_t  signal_index;
} meter_data_t;

/**
 * Create an analog meter widget bound to a value slot.
 *
 * @param value_idx  Value slot 0–12 (panel0–7, RPM, Speed, Gear, BAR1, BAR2).
 * @return           Heap-allocated widget_t*, caller must call w->destroy(w).
 */
widget_t *widget_meter_create_instance(uint8_t value_idx);

/** Return value index for meter widget. */
uint8_t widget_meter_get_value_idx(const widget_t *w);

#ifdef __cplusplus
}
#endif
