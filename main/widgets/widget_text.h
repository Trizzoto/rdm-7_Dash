#pragma once
#include "lvgl.h"
#include "widget_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a text widget instance. Uses widget_t vtable with w->update
 * matching the CAN dispatch signature: w->update(w, text_update_t*).
 *
 * @param value_idx  Value slot 0-12 to bind to (panel 0-7, RPM, Speed, Gear,
 *                   BAR1, BAR2).
 * @return           Heap-allocated widget_t*, caller must call w->destroy(w).
 */
widget_t *widget_text_create_instance(uint8_t value_idx);

#ifdef __cplusplus
}
#endif
