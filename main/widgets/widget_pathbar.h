#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * widget_pathbar -- a signal-driven progress bar whose fill follows an
 * ARBITRARY polyline path (vertical, then a radius, then horizontal -- or any
 * shape). The fill is a smooth, continuous, anti-aliased band drawn from the
 * path start up to the current value's arc-length fraction, with a rounded
 * leading cap. A dim full-path track sits underneath; an optional redline
 * portion is drawn in a separate colour. Geometry comes from the layout as a
 * flat [x0,y0,x1,y1,...] array of ABSOLUTE screen-px points.
 */

#define PATHBAR_MAX_POINTS 320

typedef struct {
    char        signal_name[32];
    int16_t     signal_index;

    float       val_min, val_max;     /* value range mapped to path 0..1 */
    float       redline;              /* value; >= val_max disables the redline */

    uint8_t     band_width;           /* stroke thickness px (default 22) */
    bool        rounded;              /* rounded caps/joins (default true) */
    lv_color_t  dim_color;            /* empty track */
    lv_color_t  lit_color;            /* fill */
    lv_color_t  redline_color;        /* fill beyond redline */
    lv_opa_t    dim_opa;              /* track opacity (default ~70) */
    uint16_t    smoothing_ms;         /* 0 = snap; else ease toward target */

    /* path geometry (absolute screen px), heap-allocated in from_json */
    lv_point_t *pts;
    float      *cum;                  /* cumulative arc length per point */
    uint16_t    n_pts;
    float       total_len;

    /* runtime */
    float       cur_frac;             /* displayed fill fraction 0..1 */
    float       target_frac;
    bool        is_stale;
    lv_timer_t *anim_timer;
} pathbar_data_t;

widget_t *widget_pathbar_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
