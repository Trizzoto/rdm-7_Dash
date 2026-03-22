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

    /* Signal binding (optional data source) */
    char       signal_name[32]; /* default: "" */
    int16_t    signal_index;    /* runtime: -1 = unbound */
    float      signal_min;      /* default: 0 */
    float      signal_max;      /* default: 100 */

    /* Image-based arc mode */
    char          arc_image[64];      /* track/empty image name, default: "" */
    char          arc_image_full[64]; /* fill image name, default: "" */
    lv_img_dsc_t *arc_img_dsc;        /* runtime: loaded track image */
    lv_img_dsc_t *arc_img_full_dsc;   /* runtime: loaded full image */
    lv_obj_t     *img_bg_obj;         /* runtime: background image object */
    lv_obj_t     *img_full_obj;       /* runtime: full image object */
    lv_obj_t     *img_clip_obj;       /* runtime: clipping container */
} arc_data_t;

widget_t *widget_arc_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
