#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Night-mode overrides for shape_panel ──────────────────────────────── */
typedef struct {
    NIGHT_FIELD_COLOR(bg_color)
    NIGHT_FIELD_COLOR(border_color)
    NIGHT_FIELD_COLOR(shadow_color)
} shape_panel_night_overrides_t;

typedef struct {
    lv_color_t bg_color;        /* default: 0x1A1A1A */
    uint8_t    bg_opa;          /* default: 255 */
    lv_color_t border_color;    /* default: 0x2E2F2E */
    uint8_t    border_width;    /* default: 0 */
    uint8_t    border_radius;   /* default: 10 */
    uint8_t    shadow_width;    /* default: 0 (disabled) */
    lv_color_t shadow_color;    /* default: 0x000000 */
    uint8_t    shadow_opa;      /* default: 128 */
    int8_t     shadow_ofs_x;    /* default: 0 */
    int8_t     shadow_ofs_y;    /* default: 0 */
    /* Night-mode appearance overrides */
    shape_panel_night_overrides_t night;
} shape_panel_data_t;

widget_t *widget_shape_panel_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
