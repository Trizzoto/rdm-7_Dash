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
    bool        fade_fill;            /* positional dim->bright fade along the lit band */
    bool        lead_edge_enabled;    /* bright "current value" marker at the fill tip */
    lv_color_t  lead_edge_color;      /* marker colour (default near-white) */
    uint8_t     lead_edge_width;      /* marker thickness px (0 = off) */

    /* Parametric shape: when shape != 0 the path is GENERATED to fit the widget
     * box (so the editor only needs a shape + radius, no point array). shape 0
     * = custom (use the explicit `path` below, e.g. tooling-authored). */
    uint8_t     shape;                /* 0=custom, 1=L-bend, 2=straight, 3=45° bend, 4=J-hook */
    uint8_t     orientation;          /* L/J: 0=TL 1=TR 2=BL 3=BR ; straight: 0=horiz 1=vert */
    uint16_t    corner_radius;        /* L-bend fillet radius px */
    uint16_t    hook_angle;           /* J-hook arc sweep degrees (90=quarter, default 120) */
    lv_color_t  dim_color;            /* empty track */
    lv_color_t  lit_color;            /* fill */
    lv_color_t  redline_color;        /* fill beyond redline */
    lv_opa_t    dim_opa;              /* track opacity (default ~70) */
    uint16_t    smoothing_ms;         /* 0 = snap; else ease toward target */

    /* Optional value scale drawn ALONG the path (Ford-tach style: ticks +
     * numbers that follow the band, spaced by arc-length so they line up with
     * the fill). All off by default — a bare pathbar is unchanged. */
    bool        show_ticks;
    bool        show_labels;
    float       minor_tick_step;      /* value spacing; <= 0 disables ticks   */
    float       major_tick_step;      /* value spacing for long/major ticks   */
    uint16_t    tick_label_divisor;   /* label = value / divisor (e.g. 1000)  */
    uint8_t     tick_len, major_tick_len;
    uint8_t     tick_width, major_tick_width;
    lv_color_t  tick_color, major_tick_color, label_color;
    int16_t     label_gap;            /* px from band edge out to the number  */
    int8_t      label_side;           /* number side: 0=auto(centroid) 1=side A 2=side B */
    int16_t     label_along_offset;   /* px shift of numbers ALONG the path (arc-length) */
    bool        redline_recolor_ticks;
    char        label_font[40];

    /* Smooth custom path: when set, the explicit `path` is treated as a few
     * ANCHOR points and a Catmull-Rom spline is tessellated through them into the
     * dense render polyline below. The anchors are kept verbatim so to_json round-
     * trips the few authored points (the editor edits those). Only meaningful for
     * a custom path (shape == 0). */
    bool        smooth;
    lv_point_t *anchors;              /* authored control points (smooth only) */
    uint16_t    n_anchors;

    /* path geometry (absolute screen px), heap-allocated in from_json. When
     * smooth, this is the tessellated dense curve; otherwise the path verbatim. */
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
