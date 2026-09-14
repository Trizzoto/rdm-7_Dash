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
/* A parametric shape is at most four pieces (an L-bend: leg, fillet, leg).
 * They describe the shape; the band itself is drawn from pb_field_t. */
#define PATHBAR_MAX_PRIMS   8

/* One piece of a parametric shape. A shape is a couple of these, and each is
 * drawn in a single anti-aliased pass (lv_draw_line / lv_draw_arc) — see the
 * renderer for why sampling the path into a stroked polyline could not be made
 * to look clean. `s0`/`s1` are the piece's span along the whole path, so the
 * fill can be clipped to an arc length without re-deriving anything. */
typedef enum { PB_PRIM_LINE = 0, PB_PRIM_ARC = 1 } pb_prim_kind_t;

typedef struct {
    uint8_t kind;
    float   s0, s1;              /* arc-length span within the path */
    float   x0, y0, x1, y1;      /* LINE: endpoints                 */
    float   cx, cy, r;           /* ARC: centre and CENTRELINE radius */
    float   outer_r;             /* ARC: outer radius, already quantised — the
                                  * draw must not re-round it or the band steps
                                  * where the arc meets its tangent run */
    float   a0, a1;              /* ARC: degrees, LVGL convention, a0 -> a1 */
} pb_prim_t;

/* The band, worked out once per pixel when its geometry is made:
 * how much of each pixel the band covers, and how far along the path that
 * pixel sits. Drawing colours pixels from these two numbers alone, so an
 * arbitrary curve has no pieces to seam or kink. */
typedef struct {
    lv_area_t   area;                /* absolute screen px the band can touch */
    uint16_t    w, h;
    uint8_t    *cov;                 /* w*h coverage, 0..255 */
    uint16_t   *s8;                  /* w*h arc length along the path, 1/8 px */
    float      *cx, *cy, *cs;        /* the float centreline and its arc length */
    uint32_t    n;
    lv_color_t *lut;                 /* lit colour at each whole px along the path */
    uint32_t    lut_n;
    float       srate;               /* max px of path per px across the band away from corners; 0 = unbounded */
    float       corner_s[16];        /* where along the path the sharp corners are */
    uint8_t     n_corner;
    uint8_t    *tiles;               /* tw*th: 1 where a 16x16 tile has any band or tick in it */
    uint8_t    *tick;                /* w*h baked tick comb: coverage 0..63, whose colour in the top 2 bits */
    uint16_t    tw, th;
} pb_field_t;

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
    uint16_t    hook_radius;          /* J-hook radius px; 0 = auto-fit the box height  */
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
    int8_t      tick_slant;           /* degrees the tick leans off the band normal */
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
    /* Parametric shapes also keep an exact description of themselves; pts/cum
     * below stay as the sampled form the ticks, labels and lead edge use. */
    pb_prim_t   prims[PATHBAR_MAX_PRIMS];
    uint8_t     n_prims;              /* 0 = custom path */
    pb_field_t *field;                /* the band's coverage; NULL = out of memory */

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
