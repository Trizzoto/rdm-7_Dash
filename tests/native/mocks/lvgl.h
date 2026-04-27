/* Minimal LVGL stub for native-host tests.
 *
 * widget_types.h pulls in <lvgl.h> for the lv_obj_t / lv_font_t /
 * lv_color_t types referenced in struct widget_t and a couple of helper
 * prototypes. None of those types are actually dereferenced from
 * widget_rules.c — they're forward-declared opaque pointers — so we
 * provide the minimum shape needed for the headers to compile. */
#ifndef RDM_TEST_MOCK_LVGL_H
#define RDM_TEST_MOCK_LVGL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct lv_obj_t  lv_obj_t;
typedef struct lv_font_t lv_font_t;

/* RGB565 bit-field shape — matches widget code's expectations of a
 * struct with `.full` being the underlying 16-bit value. Not used by
 * widget_rules.c directly, but defined so any test code that touches
 * a color field has a real type to bind against. */
typedef struct {
    uint16_t full;
} lv_color_t;

#endif /* RDM_TEST_MOCK_LVGL_H */
