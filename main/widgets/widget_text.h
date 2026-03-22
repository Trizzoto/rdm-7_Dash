#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Per-instance state for text widget ────────────────────────────────── */
typedef struct {
	uint8_t    value_idx;
	uint8_t    decimals;
	char       font[32];
	char       static_text[64];
	char       signal_name[32];
	int16_t    signal_index;
	lv_color_t text_color;
	int16_t    rotation;       /* Rotation in degrees (0-359, default 0) */
} text_data_t;

/**
 * Create a text widget instance.
 *
 * @param value_idx  Value slot 0-10 to bind to (panel 0-7, RPM, BAR1, BAR2).
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
