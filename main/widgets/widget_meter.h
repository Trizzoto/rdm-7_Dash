#pragma once
#include "lvgl.h"
#include "widget_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create an analog meter widget bound to a value slot.
 *
 * @param value_idx  Value slot 0–12 (panel0–7, RPM, Speed, Gear, BAR1, BAR2).
 * @return           Heap-allocated widget_t*, caller must call w->destroy(w).
 */
widget_t *widget_meter_create_instance(uint8_t value_idx);

/** Return value index for meter widget (for can_dispatch rebuild). */
uint8_t widget_meter_get_value_idx(const widget_t *w);

#ifdef __cplusplus
}
#endif
