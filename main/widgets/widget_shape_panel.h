#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHAPE_TYPE_RECTANGLE = 0,
    SHAPE_TYPE_CIRCLE,
    SHAPE_TYPE_TRAPEZOID,
    SHAPE_TYPE_TRIANGLE,
    SHAPE_TYPE_DIAMOND,
    SHAPE_TYPE_ARROW_RIGHT,
    SHAPE_TYPE_ARROW_LEFT,
    SHAPE_TYPE_CHEVRON_RIGHT,
    SHAPE_TYPE_CHEVRON_LEFT,
} shape_panel_type_t;

/* ── Night-mode overrides for shape_panel ──────────────────────────────── */
typedef struct {
    NIGHT_FIELD_COLOR(bg_color)
    NIGHT_FIELD_COLOR(border_color)
    NIGHT_FIELD_COLOR(shadow_color)
} shape_panel_night_overrides_t;

typedef struct {
    shape_panel_type_t shape_type;   /* default: SHAPE_TYPE_RECTANGLE */
    uint8_t            taper;        /* 0-50; trapezoid inset %; default 20 */
    bool               taper_bottom; /* false=narrow top, true=narrow bottom */
    lv_color_t bg_color;             /* default: 0x1A1A1A */
    uint8_t    bg_opa;               /* default: 255 */
    lv_color_t border_color;         /* default: 0x2E2F2E */
    uint8_t    border_width;         /* default: 0 */
    uint8_t    border_radius;        /* default: 10 */
    uint8_t    shadow_width;         /* default: 0 (disabled) */
    lv_color_t shadow_color;         /* default: 0x000000 */
    uint8_t    shadow_opa;           /* default: 128 */
    int8_t     shadow_ofs_x;         /* default: 0 */
    int8_t     shadow_ofs_y;         /* default: 0 */
    /* Night-mode appearance overrides */
    shape_panel_night_overrides_t night;
} shape_panel_data_t;

widget_t *widget_shape_panel_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
