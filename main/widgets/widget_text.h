#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdbool.h>

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

/** Return the value_idx (0-12) from a text widget's type_data. */
uint8_t widget_text_get_value_idx(const widget_t *w);

/** Return true if this text widget is bound to a signal. */
bool widget_text_has_signal(const widget_t *w);

#ifdef __cplusplus
}
#endif
