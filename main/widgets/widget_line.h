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
    LINE_ORIENT_HORIZONTAL  = 0,
    LINE_ORIENT_VERTICAL    = 1,
    LINE_ORIENT_DIAG_FWD    = 2,  /* / — bottom-left to top-right */
    LINE_ORIENT_DIAG_BWD    = 3,  /* \ — top-left to bottom-right */
} line_orientation_t;

typedef struct {
    NIGHT_FIELD_COLOR(line_color)
} line_night_overrides_t;

typedef struct {
    lv_color_t        line_color;    /* default: 0xFFFFFF */
    lv_opa_t          line_opa;      /* default: 255 */
    uint8_t           line_width;    /* default: 4 */
    bool              rounded;       /* default: false */
    line_orientation_t orientation;  /* default: horizontal */
    uint8_t           dash_gap;      /* default: 0 (solid) */
    /* Quadratic-bezier curvature, in pixels. 0 = straight (existing
     * behaviour). Positive values bow the midpoint along the perpendicular
     * to the chord (LVGL +Y for horizontal, +X for vertical); negative
     * values bow the other way. Range −200..200 — bigger curves than the
     * bounding box add nothing visually since the line gets clipped. */
    int16_t           curvature;     /* default: 0 */
    line_night_overrides_t night;
} line_data_t;

widget_t *widget_line_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
