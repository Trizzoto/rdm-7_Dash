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
    /* EFFECTIVE (rendered) values — the draw callback reads these. Rule
     * overrides and night mode overlay onto these; they are reset to the
     * base_* snapshot at the top of each apply so deactivation restores. */
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
    /* BASE (configured) snapshot — immutable except via config/from_json/
     * inspector. to_json serializes THESE so a save while a rule or night
     * override is active persists the configured line, not the override. */
    lv_color_t        base_line_color;
    lv_opa_t          base_line_opa;
    uint8_t           base_line_width;
    int16_t           base_curvature;
    line_night_overrides_t night;
} line_data_t;

widget_t *widget_line_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
