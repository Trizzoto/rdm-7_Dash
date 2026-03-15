#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t    start_angle;     /* default: 135 */
    int16_t    end_angle;       /* default: 45 */
    uint8_t    arc_width;       /* default: 10 */
    lv_color_t arc_color;       /* default: 0x00FF00 */
    lv_color_t bg_arc_color;    /* default: 0x333333 */
    uint8_t    bg_arc_width;    /* default: 10 */
    bool       rounded_ends;    /* default: false */
    lv_obj_t  *arc_obj;         /* runtime only */
} arc_data_t;

widget_t *widget_arc_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
