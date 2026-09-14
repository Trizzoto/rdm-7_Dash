/*
 * widget_pathbar.c -- progress bar that fills along an arbitrary polyline path.
 *
 * Renders with a custom DRAW_MAIN_END callback (like widget_line): a dim
 * full-path band underneath, then the lit band from path-start to the current
 * value's arc-length fraction, then (optionally) a redline-coloured band for
 * the portion past the redline threshold. Each band is a stroked polyline with
 * rounded caps/joins so the fill reads as one smooth continuous band and the
 * leading edge is a clean rounded cap. Works for any shape (the KTM L-bar:
 * vertical -> radius -> horizontal).
 */
#include "widget_pathbar.h"
#include "esp_attr.h"
#include "widget_rules.h"
#include "signal.h"
#include "screen_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"   /* lv_draw_sw_blend: the coverage-field renderer */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_pathbar";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Defaults ───────────────────────────────────────────────────────────── */
#define DEF_MIN          0.0f
#define DEF_MAX          11000.0f
#define DEF_BAND_WIDTH   22
#define DEF_DIM_COLOR    0xC8CBCF
#define DEF_LIT_COLOR    0x202328
#define DEF_RED_COLOR    0xF26E10
#define DEF_LEAD_COLOR   0xE6FAFF
#define DEF_LEAD_WIDTH   6
#define DEF_DIM_OPA      70
#define DEF_W            560
#define DEF_H            320
#define PATHBAR_ANIM_MS  16
#define PATHBAR_SNAP_FRAC 0.15f /* jump > this fraction of full fill snaps (see widget_smooth.c) */
/* Tick-scale defaults (RGB565 raw, matching how the layout stores colours). */
#define DEF_TICK_COLOR        0x9CF3   /* mid grey */
#define DEF_MAJ_TICK_COLOR    0xFFFF   /* white    */
#define DEF_LABEL_COLOR       0xFFFF   /* white    */

/* ── Color helpers (layout colours are raw RGB565) ──────────────────────── */
static inline uint32_t _color_to_u32(lv_color_t c) { return (uint32_t)c.full; }

static inline lv_color_t _u32_to_color(uint32_t v) {
    lv_color_t c;
    c.ch.red   = (v >> 11) & 0x1F;
    c.ch.green = (v >> 5)  & 0x3F;
    c.ch.blue  = v & 0x1F;
    return c;
}

static void _pb_field_free(pathbar_data_t *pd);

static void _pathbar_free_path(pathbar_data_t *pd) {
    _pb_field_free(pd);
    if (pd->pts) { free(pd->pts); pd->pts = NULL; }
    if (pd->cum) { free(pd->cum); pd->cum = NULL; }
    pd->n_pts = 0;
    pd->total_len = 0.0f;
}

static void _pathbar_build_cum(pathbar_data_t *pd) {
    pd->total_len = 0.0f;
    if (!pd->cum || pd->n_pts == 0) return;
    pd->cum[0] = 0.0f;
    for (uint16_t i = 1; i < pd->n_pts; i++) {
        float dx = (float)(pd->pts[i].x - pd->pts[i - 1].x);
        float dy = (float)(pd->pts[i].y - pd->pts[i - 1].y);
        pd->cum[i] = pd->cum[i - 1] + sqrtf(dx * dx + dy * dy);
    }
    pd->total_len = pd->cum[pd->n_pts - 1];
}

static void _pathbar_free_anchors(pathbar_data_t *pd) {
    if (pd->anchors) { free(pd->anchors); pd->anchors = NULL; }
    pd->n_anchors = 0;
}

typedef void (*pb_emit_fn)(void *ctx, float x, float y);

/* Points on the smooth curve through `anc`, `step` px of chord apart (at most
 * `max_steps` per anchor gap), in order, starting with the first anchor. */
static void _pb_catmull_rom(const lv_point_t *anc, int na, float step, int max_steps,
                            pb_emit_fn emit, void *ctx) {
    if (!anc || na < 2) return;
    emit(ctx, anc[0].x, anc[0].y);
    if (na == 2) {
        emit(ctx, anc[1].x, anc[1].y);                          /* a line */
    } else {
        /* CENTRIPETAL Catmull-Rom (alpha=0.5, Barry-Goldman form). Uniform CR
         * overshoots/cusps badly when anchor spacing is uneven (e.g. a long
         * straight run next to a tight curve) because a far neighbour blows up
         * the local tangent; centripetal parameterises by sqrt(distance) so it
         * never overshoots or self-intersects. End segments use a reflected
         * phantom point so the knots stay non-coincident. */
        for (int i = 0; i < na - 1; i++) {
            float p1x = anc[i].x,     p1y = anc[i].y;
            float p2x = anc[i+1].x,   p2y = anc[i+1].y;
            float p0x, p0y, p3x, p3y;
            if (i > 0)      { p0x = anc[i-1].x;         p0y = anc[i-1].y; }
            else            { p0x = 2.0f*p1x - p2x;     p0y = 2.0f*p1y - p2y; }
            if (i+2 < na)   { p3x = anc[i+2].x;         p3y = anc[i+2].y; }
            else            { p3x = 2.0f*p2x - p1x;     p3y = 2.0f*p2y - p1y; }
            float t0 = 0.0f;
            float t1 = t0 + sqrtf(sqrtf((p1x-p0x)*(p1x-p0x) + (p1y-p0y)*(p1y-p0y)));
            float t2 = t1 + sqrtf(sqrtf((p2x-p1x)*(p2x-p1x) + (p2y-p1y)*(p2y-p1y)));
            float t3 = t2 + sqrtf(sqrtf((p3x-p2x)*(p3x-p2x) + (p3y-p2y)*(p3y-p2y)));
            if (t1 <= t0) t1 = t0 + 1e-4f;
            if (t2 <= t1) t2 = t1 + 1e-4f;
            if (t3 <= t2) t3 = t2 + 1e-4f;
            float chord = sqrtf((p2x-p1x)*(p2x-p1x) + (p2y-p1y)*(p2y-p1y));
            int steps = (int)(chord / step);
            if (steps < 2)  steps = 2;
            if (steps > max_steps) steps = max_steps;
            for (int s = 1; s <= steps; s++) {
                float t  = t1 + (t2 - t1) * (float)s / (float)steps;
                float A1x = ((t1-t)*p0x + (t-t0)*p1x) / (t1-t0), A1y = ((t1-t)*p0y + (t-t0)*p1y) / (t1-t0);
                float A2x = ((t2-t)*p1x + (t-t1)*p2x) / (t2-t1), A2y = ((t2-t)*p1y + (t-t1)*p2y) / (t2-t1);
                float A3x = ((t3-t)*p2x + (t-t2)*p3x) / (t3-t2), A3y = ((t3-t)*p2y + (t-t2)*p3y) / (t3-t2);
                float B1x = ((t2-t)*A1x + (t-t0)*A2x) / (t2-t0), B1y = ((t2-t)*A1y + (t-t0)*A2y) / (t2-t0);
                float B2x = ((t3-t)*A2x + (t-t1)*A3x) / (t3-t1), B2y = ((t3-t)*A2y + (t-t1)*A3y) / (t3-t1);
                float Cx  = ((t2-t)*B1x + (t-t1)*B2x) / (t2-t1), Cy  = ((t2-t)*B1y + (t-t1)*B2y) / (t2-t1);
                emit(ctx, Cx, Cy);
            }
        }
    }
}

typedef struct { lv_point_t *buf; int n; } pb_ibuf_t;

static void _pb_emit_int(void *ctx, float x, float y) {
    pb_ibuf_t *b = (pb_ibuf_t *)ctx;
    lv_coord_t X = (lv_coord_t)lroundf(x), Y = (lv_coord_t)lroundf(y);
    if ((b->n == 0 || X != b->buf[b->n - 1].x || Y != b->buf[b->n - 1].y) &&
        b->n < PATHBAR_MAX_POINTS) {
        b->buf[b->n].x = X; b->buf[b->n].y = Y; b->n++;
    }
}

/* Smooth custom path: tessellate a Catmull-Rom spline THROUGH pd->anchors into
 * the dense pd->pts so a few authored points produce a clean curve (instead of
 * the user hand-placing dozens of polyline points). Catmull-Rom interpolates
 * every anchor; the two end segments reflect the end anchor as the phantom
 * neighbour so the curve doesn't pull in. Sub-steps scale with chord length
 * (~6px) and the whole thing is capped at PATHBAR_MAX_POINTS. Replaces pts/cum;
 * pd->anchors is left intact (to_json round-trips the few points). */
static void _pathbar_smooth_from_anchors(pathbar_data_t *pd) {
    if (!pd->anchors || pd->n_anchors < 2) return;
    static EXT_RAM_BSS_ATTR lv_point_t buf[PATHBAR_MAX_POINTS];
    pb_ibuf_t ib = { buf, 0 };
    _pb_catmull_rom(pd->anchors, pd->n_anchors, 6.0f, 48, _pb_emit_int, &ib);
    int n = ib.n;

    _pathbar_free_path(pd);
    if (n < 2) return;
    pd->pts = heap_caps_calloc(n, sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    pd->cum = heap_caps_calloc(n, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!pd->pts || !pd->cum) { _pathbar_free_path(pd); return; }
    for (int i = 0; i < n; i++) pd->pts[i] = buf[i];
    pd->n_pts = (uint16_t)n;
    _pathbar_build_cum(pd);
}

#define PB_HOOK_MIN_TAIL 24.0f   /* px of straight tail a J-hook always keeps */

/* ── Parametric shapes, described rather than sampled ───────────────────────
 * Each shape is emitted as a couple of primitives (straight runs and circular
 * arcs). The renderer draws each in ONE anti-aliased pass, which is the only
 * way to get a clean edge out of a scanline rasteriser: overlapping AA shapes
 * blend twice and show their seams even in a single colour.
 *
 * pts/cum are then sampled FROM the primitives, because the ticks, the labels,
 * the lead edge and the value hit-test all want a polyline. */

static void _pb_prim_line(pathbar_data_t *pd, float x0, float y0, float x1, float y1) {
    if (pd->n_prims >= PATHBAR_MAX_PRIMS) return;
    float dx = x1 - x0, dy = y1 - y0;
    if (dx * dx + dy * dy < 0.25f) return;            /* degenerate */
    pb_prim_t *p = &pd->prims[pd->n_prims++];
    p->kind = PB_PRIM_LINE;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
}

/* a0/a1 in degrees, LVGL convention (0 = 3 o'clock, increasing clockwise on a
 * y-down screen). r is the CENTRELINE radius; the band straddles it. */
static void _pb_prim_arc(pathbar_data_t *pd, float cx, float cy, float r,
                         float a0, float a1) {
    if (pd->n_prims >= PATHBAR_MAX_PRIMS) return;
    if (r < 1.0f || fabsf(a1 - a0) < 0.5f) return;
    pb_prim_t *p = &pd->prims[pd->n_prims++];
    p->kind = PB_PRIM_ARC;
    /* Quantise here, once, to what lv_draw_arc can actually express: an
     * integer centre and an integer outer radius. The centreline then follows
     * from the quantised outer radius rather than the other way round, so a
     * tangent run derived from `r` lands on the same band edge the arc draws. */
    p->cx = lroundf(cx);
    p->cy = lroundf(cy);
    p->outer_r = lroundf(r + pd->band_width / 2.0f);
    p->r = p->outer_r - pd->band_width / 2.0f;
    p->a0 = a0; p->a1 = a1;
}

static float _pb_prim_len(const pb_prim_t *p) {
    if (p->kind == PB_PRIM_LINE) {
        float dx = p->x1 - p->x0, dy = p->y1 - p->y0;
        return sqrtf(dx * dx + dy * dy);
    }
    return p->r * fabsf(p->a1 - p->a0) * (float)M_PI / 180.0f;
}

/* Point at fraction f (0..1) along one primitive. */
static void _pb_prim_at(const pb_prim_t *p, float f, float *x, float *y) {
    if (p->kind == PB_PRIM_LINE) {
        *x = p->x0 + (p->x1 - p->x0) * f;
        *y = p->y0 + (p->y1 - p->y0) * f;
    } else {
        float a = (p->a0 + (p->a1 - p->a0) * f) * (float)M_PI / 180.0f;
        *x = p->cx + p->r * cosf(a);
        *y = p->cy + p->r * sinf(a);
    }
}

/* Give every primitive its span along the whole path, then sample the lot into
 * pts/cum for the machinery that still wants a polyline. */
static void _pb_prims_finish(pathbar_data_t *pd) {
    lv_point_t buf[PATHBAR_MAX_POINTS];
    int n = 0;
    #define PB_EMIT(X, Y) do { \
        lv_coord_t _x = (lv_coord_t)lroundf(X), _y = (lv_coord_t)lroundf(Y); \
        if ((n == 0 || _x != buf[n - 1].x || _y != buf[n - 1].y) && n < PATHBAR_MAX_POINTS) { \
            buf[n].x = _x; buf[n].y = _y; n++; } \
    } while (0)

    float s = 0.0f;
    for (uint8_t k = 0; k < pd->n_prims; k++) {
        pb_prim_t *p = &pd->prims[k];
        float len = _pb_prim_len(p);
        p->s0 = s; p->s1 = s + len; s += len;

        /* Share the point budget out by length rather than spending a fixed
         * 6 px per piece: a fitted path can be forty pieces, and a budget
         * spent early truncates the polyline the ticks and the invalidate
         * window are read from. */
        float tot = 0.0f;
        for (uint8_t q = 0; q < pd->n_prims; q++) tot += _pb_prim_len(&pd->prims[q]);
        float gran = (tot > 0.0f) ? tot / (float)(PATHBAR_MAX_POINTS - 8) : 6.0f;
        if (gran < 6.0f) gran = 6.0f;
        int steps = (int)(len / gran) + 1;
        if (steps < 1) steps = 1;
        if (steps > 120) steps = 120;
        for (int q = (k == 0 ? 0 : 1); q <= steps; q++) {
            float x, y;
            _pb_prim_at(p, (float)q / (float)steps, &x, &y);
            PB_EMIT(x, y);
        }
    }
    #undef PB_EMIT

    _pathbar_free_path(pd);
    if (n < 2) { pd->n_prims = 0; return; }
    pd->pts = heap_caps_calloc(n, sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    pd->cum = heap_caps_calloc(n, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!pd->pts || !pd->cum) { _pathbar_free_path(pd); pd->n_prims = 0; return; }
    for (int q = 0; q < n; q++) pd->pts[q] = buf[q];
    pd->n_pts = (uint16_t)n;
    _pathbar_build_cum(pd);
    /* The sampled length differs from the exact one by rounding; the fill is
     * clipped against the primitives, so use THEIR total. */
    pd->total_len = s;
}

/* ── The band as a coverage field ───────────────────────────────────────────
 * Every way of drawing the band out of PIECES left the pieces visible.
 * Stroked segments overlap, and overlapping anti-aliased edges blend twice, so
 * every join prints a seam. Fitting a curve to arcs and lines swapped the
 * seams for kinks: nothing makes one fitted piece leave at the angle the last
 * one arrived. And even a true arc is not clean through lv_draw_arc — LVGL 8
 * snaps its centre and radius to whole pixels and its ends to whole degrees,
 * which shows as a faint wobble along the edge at high zoom.
 *
 * So no shape is drawn out of pieces. When the geometry is made, every pixel
 * the band can touch gets two numbers, measured against the exact float
 * centreline:
 *
 *   cov — how much of the pixel the band covers (the anti-aliased edge)
 *   s   — how far along the path the pixel sits
 *
 * Drawing is then per pixel: lit behind the fill front, dim ahead of it,
 * redline past the redline, and each of those boundaries is anti-aliased
 * against its true position rather than the pixel grid. One blend per strip,
 * no overlaps, nothing to seam or kink, at any zoom. A value change repaints
 * only the few pixels the invalidated area covers.
 *
 * The band is the union of one rectangle per centreline segment, cut square
 * at its ends, plus the mitre wedge on the outside of each join. On a curve
 * those wedges are slivers and the outline is just the curve (a 1 px chord on
 * a 30 px radius is off the true circle by 0.004 px); on an L-bend or a
 * hand-drawn stairstep they are the sharp corners it was drawn with. Where two
 * pieces both cover a pixel it takes the larger coverage — never the sum — so
 * no edge is ever blended twice. */

#ifndef PATHBAR_FADE_FULL
#define PATHBAR_FADE_FULL 0.45f
#endif
#define PB_FIELD_MITRE  1.5f   /* mitre reach, in half-widths, before it is clipped */
#define PB_FIELD_STEP   1.0f   /* px of chord on a curved centreline */
#define PB_FIELD_ROWS   16     /* rows blended per strip */

typedef struct { float *x, *y; int n, cap; } pb_fline_t;

static void _pb_emit_float(void *ctx, float x, float y) {
    pb_fline_t *f = (pb_fline_t *)ctx;
    if (f->n > 0) {
        float dx = x - f->x[f->n - 1], dy = y - f->y[f->n - 1];
        if (dx * dx + dy * dy < 0.04f) return;           /* under 0.2 px: same point */
    }
    if (f->n < f->cap) { f->x[f->n] = x; f->y[f->n] = y; f->n++; }
}

static void _pb_field_free(pathbar_data_t *pd) {
    pb_field_t *f = pd->field;
    if (!f) return;
    if (f->cov) free(f->cov);
    if (f->s8)  free(f->s8);
    if (f->cx)  free(f->cx);
    if (f->cy)  free(f->cy);
    if (f->cs)  free(f->cs);
    if (f->lut) free(f->lut);
    if (f->tiles) free(f->tiles);
    if (f->tick) free(f->tick);
    free(f);
    pd->field = NULL;
}

static inline float _pb_clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* How much of a pixel lies on the inside of a straight edge: `d` is the
 * distance from the pixel's centre to the edge (positive inside) and (nx,ny)
 * the edge's unit normal. This is the pixel's true covered AREA. The usual
 * shortcut, clamp(d + 0.5), is only exact for an edge that runs along the
 * pixel grid; on a diagonal it is off by up to 6%, and that error repeats
 * with every pixel the edge crosses — a faint ripple along a 45 degree
 * stretch of arc, which is the last thing between this and a clean curve. */
static inline float _pb_edge_cov(float d, float nx, float ny) {
    float a = fabsf(nx), b = fabsf(ny);
    if (b > a) { float t = a; a = b; b = t; }            /* a >= b, a >= 0.707 */
    const float w = 0.5f * (a + b);                      /* half the pixel's extent across the edge */
    if (d >=  w) return 1.0f;
    if (d <= -w) return 0.0f;
    const float e = 0.5f * (a - b);
    if (d >= -e && d <= e) return 0.5f + d / a;          /* the edge crosses two opposite sides */
    const float r = w - fabsf(d);                        /* it clips a corner: a triangle */
    const float tri = r * r / (2.0f * a * b);
    return (d > 0.0f) ? 1.0f - tri : tri;
}

/* Coverage of a disc of radius `r` at a pixel whose centre is (vx,vy) from it. */
static inline float _pb_disc_cov(float vx, float vy, float r) {
    float l = sqrtf(vx * vx + vy * vy);
    if (l < 1e-4f) return r >= 0.5f ? 1.0f : 0.0f;
    return _pb_edge_cov(r - l, vx / l, vy / l);
}

/* Offer one pixel a coverage from one piece of the band. The larger coverage
 * wins; between two full ones the piece whose centreline is nearer does, which
 * puts the s boundary on the corner's bisector. */
static inline void _pb_field_put(pb_field_t *f, uint8_t *perp, int x, int y,
                                 float c, float pe, float s) {
    if (c <= 0.0f) return;
    if (x < f->area.x1 || x > f->area.x2 || y < f->area.y1 || y > f->area.y2) return;
    size_t i = (size_t)(y - f->area.y1) * f->w + (size_t)(x - f->area.x1);
    uint8_t cv = (c >= 1.0f) ? 255 : (uint8_t)(c * 255.0f + 0.5f);
    float pq = pe * 4.0f;
    uint8_t p8 = (pq > 254.0f) ? 254 : (uint8_t)pq;
    if (cv < f->cov[i]) return;
    if (cv == f->cov[i] && p8 >= perp[i]) return;
    f->cov[i] = cv;
    perp[i] = p8;
    float s8 = s * 8.0f;
    f->s8[i] = (uint16_t)(s8 < 0.0f ? 0 : (s8 > 32767.0f ? 32767 : lroundf(s8)));  /* top bit: tick */
}

/* Point and unit left-normal at arc length s on the field's float centreline.
 * The tangent is blended between the two ends of the segment s falls in, so a
 * tick or a fill front moving along a curve turns smoothly instead of stepping
 * a chord at a time — except across a real corner (a turn of more than ten
 * degrees), where the local straight run's own direction is the right one. */
static bool _pb_field_sample(const pb_field_t *f, float s,
                             float *x, float *y, float *nx, float *ny) {
    if (!f || f->n < 2) return false;
    const uint32_t n = f->n;
    if (s < 0.0f) s = 0.0f;
    if (s > f->cs[n - 1]) s = f->cs[n - 1];
    uint32_t lo = 0, hi = n - 1;                        /* cs[lo] <= s <= cs[hi] */
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (f->cs[mid] <= s) lo = mid; else hi = mid;
    }
    float L = f->cs[hi] - f->cs[lo];
    float t = (L > 1e-6f) ? (s - f->cs[lo]) / L : 0.0f;
    *x = f->cx[lo] + (f->cx[hi] - f->cx[lo]) * t;
    *y = f->cy[lo] + (f->cy[hi] - f->cy[lo]) * t;

    float ux = f->cx[hi] - f->cx[lo], uy = f->cy[hi] - f->cy[lo];
    float ul = sqrtf(ux * ux + uy * uy);
    if (ul < 1e-6f) return false;
    ux /= ul; uy /= ul;
    float tx0 = ux, ty0 = uy, tx1 = ux, ty1 = uy;
    const float SMOOTH_COS = 0.985f;                    /* cos 10 deg */
    if (lo > 0) {                                        /* tangent at the start vertex */
        float px = f->cx[lo] - f->cx[lo - 1], py = f->cy[lo] - f->cy[lo - 1];
        float pl = sqrtf(px * px + py * py);
        if (pl > 1e-6f && (px * ux + py * uy) / pl > SMOOTH_COS) {
            tx0 = px / pl + ux; ty0 = py / pl + uy;
        }
    }
    if (hi + 1 < n) {                                    /* and at the end vertex */
        float qx = f->cx[hi + 1] - f->cx[hi], qy = f->cy[hi + 1] - f->cy[hi];
        float ql = sqrtf(qx * qx + qy * qy);
        if (ql > 1e-6f && (qx * ux + qy * uy) / ql > SMOOTH_COS) {
            tx1 = qx / ql + ux; ty1 = qy / ql + uy;
        }
    }
    float l0 = sqrtf(tx0 * tx0 + ty0 * ty0), l1 = sqrtf(tx1 * tx1 + ty1 * ty1);
    tx0 /= l0; ty0 /= l0; tx1 /= l1; ty1 /= l1;
    float tx = tx0 + (tx1 - tx0) * t, ty = ty0 + (ty1 - ty0) * t;
    float tl = sqrtf(tx * tx + ty * ty);
    if (tl < 1e-6f) { tx = ux; ty = uy; tl = 1.0f; }
    *nx = -ty / tl;
    *ny =  tx / tl;
    return true;
}

#define PB_S8_TICK 0x8000
#define PB_S8_MASK 0x7FFF

static bool _pathbar_sample(pathbar_data_t *pd, float s, float *x, float *y, float *nx, float *ny);

/* Coverage of a straight mark of half-width hw from (x0,y0) along unit (ux,uy)
 * for length L, at the pixel whose centre is (px,py), with round ends. */
static inline float _pb_mark_cov_round(float px, float py, float x0, float y0,
                                       float ux, float uy, float L, float hw) {
    float vx = px - x0, vy = py - y0;
    float al = vx * ux + vy * uy;
    float t = al < 0.0f ? 0.0f : (al > L ? L : al);
    return _pb_disc_cov(vx - ux * t, vy - uy * t, hw);
}

/* The tick comb, baked once into f->tick: the low six bits are the mark's
 * coverage and the top two say whose colour it is (minor, major, redline).
 * Ticks never move, and rasterising each one on every redraw of the band was
 * as expensive as drawing the band itself. Where marks touch, the stronger
 * one keeps the pixel. */
static void _pb_bake_ticks(pathbar_data_t *pd) {
    pb_field_t *f = pd->field;
    if (!pd->show_ticks || pd->minor_tick_step <= 0.0f ||
        pd->val_max <= pd->val_min || pd->total_len <= 0.0f)
        return;
    float range = pd->val_max - pd->val_min;
    int N = (int)lroundf(range / pd->minor_tick_step);
    if (N < 1) N = 1;
    if (N > 500) N = 500;
    int majEvery = pd->major_tick_step > 0.0f
                 ? (int)lroundf(pd->major_tick_step / pd->minor_tick_step) : 1;
    if (majEvery < 1) majEvery = 1;
    for (int i = 0; i <= N; i++) {
        float v = pd->val_min + (float)i * pd->minor_tick_step;
        float fr = (v - pd->val_min) / range;
        if (fr < 0.0f) fr = 0.0f;
        if (fr > 1.0f) fr = 1.0f;
        bool isMaj = (i % majEvery) == 0;
        uint8_t len = isMaj ? pd->major_tick_len   : pd->tick_len;
        uint8_t wdt = isMaj ? pd->major_tick_width : pd->tick_width;
        if (len == 0 || wdt == 0) continue;
        float px, py, nx, ny;
        if (!_pathbar_sample(pd, fr * pd->total_len, &px, &py, &nx, &ny)) continue;
        uint8_t who = isMaj ? 1 : 0;
        if (pd->redline_recolor_ticks && pd->redline < pd->val_max && v >= pd->redline) who = 2;
        float tnx = nx, tny = ny;                       /* tick_slant: see _pathbar_draw_scale */
        if (pd->tick_slant) {
            float a  = (float)pd->tick_slant * 3.14159265f / 180.0f;
            float ca = cosf(a), sa = sinf(a);
            tnx = nx * ca - ny * sa;
            tny = nx * sa + ny * ca;
        }
        const float L = (float)len, hwm = (float)wdt * 0.5f, pad = hwm + 1.5f;
        float x0 = px - tnx * L / 2.0f, y0 = py - tny * L / 2.0f;
        float x1 = x0 + tnx * L, y1 = y0 + tny * L;
        int32_t bx0 = (int32_t)floorf((x0 < x1 ? x0 : x1) - pad), by0 = (int32_t)floorf((y0 < y1 ? y0 : y1) - pad);
        int32_t bx1 = (int32_t)ceilf((x0 > x1 ? x0 : x1) + pad),  by1 = (int32_t)ceilf((y0 > y1 ? y0 : y1) + pad);
        if (bx0 < f->area.x1) bx0 = f->area.x1;
        if (by0 < f->area.y1) by0 = f->area.y1;
        if (bx1 > f->area.x2) bx1 = f->area.x2;
        if (by1 > f->area.y2) by1 = f->area.y2;
        for (int32_t y = by0; y <= by1; y++) {
            for (int32_t x = bx0; x <= bx1; x++) {
                float c = _pb_mark_cov_round((float)x + 0.5f, (float)y + 0.5f, x0, y0, tnx, tny, L, hwm);
                uint8_t q = (uint8_t)(c * 63.0f + 0.5f);
                if (q == 0) continue;
                size_t k = (size_t)(y - f->area.y1) * f->w + (size_t)(x - f->area.x1);
                if (q > (f->tick[k] & 63)) f->tick[k] = (uint8_t)((who << 6) | q);
            }
        }
    }
}

static void _pathbar_build_field(pathbar_data_t *pd) {
    _pb_field_free(pd);
    if (!pd->pts || pd->n_pts < 2 || pd->total_len <= 0.0f) return;

    /* The exact centreline:
     *  - a parametric shape samples its own lines and arcs (a line needs only
     *    its ends; an arc gets a point every PB_FIELD_STEP px);
     *  - a smooth custom path re-evaluates its spline in float — pd->pts is
     *    the same curve snapped to whole pixels, and an edge measured against
     *    that wobbles by half a pixel;
     *  - a plain custom path is the points it was drawn with. */
    const bool from_prims  = pd->n_prims > 0;
    const bool from_spline = !from_prims && pd->smooth && pd->anchors && pd->n_anchors >= 2;
    pb_fline_t fl = { 0 };
    if (from_prims) {
        float len = 0.0f;
        for (uint8_t k = 0; k < pd->n_prims; k++) len += _pb_prim_len(&pd->prims[k]);
        fl.cap = (int)(len / PB_FIELD_STEP) + 4 * pd->n_prims + 4;
    } else if (from_spline) {
        int cap = 2;
        for (uint16_t i = 0; i + 1 < pd->n_anchors; i++) {
            float dx = (float)(pd->anchors[i + 1].x - pd->anchors[i].x);
            float dy = (float)(pd->anchors[i + 1].y - pd->anchors[i].y);
            int st = (int)(sqrtf(dx * dx + dy * dy) / PB_FIELD_STEP);
            cap += (st < 2 ? 2 : (st > 2048 ? 2048 : st));
        }
        fl.cap = cap;
    } else {
        fl.cap = pd->n_pts;
    }
    fl.x = heap_caps_malloc((size_t)fl.cap * sizeof(float), MALLOC_CAP_SPIRAM);
    fl.y = heap_caps_malloc((size_t)fl.cap * sizeof(float), MALLOC_CAP_SPIRAM);
    float *cum = heap_caps_malloc((size_t)fl.cap * sizeof(float), MALLOC_CAP_SPIRAM);
    uint8_t *perp = NULL;
    pb_field_t *f = NULL;
    if (!fl.x || !fl.y || !cum) goto out;

    if (from_prims) {
        for (uint8_t k = 0; k < pd->n_prims; k++) {
            const pb_prim_t *p = &pd->prims[k];
            /* a chord of length c on radius r is off the arc by c*c/(8r):
             * keep that under a hundredth of a pixel, and no longer than
             * 4 px so the join wedges stay slivers */
            float chord = (p->kind == PB_PRIM_ARC) ? sqrtf(0.08f * p->r) : 1.0f;
            if (chord < PB_FIELD_STEP) chord = PB_FIELD_STEP;
            if (chord > 4.0f) chord = 4.0f;
            int steps = (p->kind == PB_PRIM_LINE) ? 1
                      : (int)ceilf(_pb_prim_len(p) / chord) + 1;
            /* The first and last chord set the direction a square end is cut
             * across, so on an arc they are half a pixel long: a full-length
             * chord leans off the true tangent by chord/2R and turned the cut
             * by most of a degree. */
            const float len  = _pb_prim_len(p);
            const float edge = (p->kind == PB_PRIM_ARC && len > 2.0f) ? 0.5f / len : 0.0f;
            if (k == 0) {
                float x, y;
                _pb_prim_at(p, 0.0f, &x, &y);
                _pb_emit_float(&fl, x, y);
            }
            if (edge > 0.0f) {
                float x, y;
                _pb_prim_at(p, edge, &x, &y);
                _pb_emit_float(&fl, x, y);
            }
            for (int q = 1; q <= steps; q++) {
                float t = (float)q / (float)steps;
                float x, y;
                if (edge > 0.0f && q == steps) {        /* the half-pixel chord into the end */
                    _pb_prim_at(p, 1.0f - edge, &x, &y);
                    _pb_emit_float(&fl, x, y);
                }
                _pb_prim_at(p, t, &x, &y);
                _pb_emit_float(&fl, x, y);
            }
        }
    } else if (from_spline) {
        _pb_catmull_rom(pd->anchors, pd->n_anchors, PB_FIELD_STEP, 2048, _pb_emit_float, &fl);
    } else {
        for (uint16_t i = 0; i < pd->n_pts; i++) _pb_emit_float(&fl, pd->pts[i].x, pd->pts[i].y);
    }
    if (fl.n < 2) goto out;

    cum[0] = 0.0f;
    for (int i = 1; i < fl.n; i++) {
        float dx = fl.x[i] - fl.x[i - 1], dy = fl.y[i] - fl.y[i - 1];
        cum[i] = cum[i - 1] + sqrtf(dx * dx + dy * dy);
    }
    if (cum[fl.n - 1] <= 0.0f) goto out;
    /* Values map onto pd->total_len, which for a custom path is measured along
     * the pixel polyline. Stretch this line's lengths onto it so the fill
     * front, the ticks and the invalidate window all agree; the two differ by
     * well under a percent, spread evenly. */
    const float scale = pd->total_len / cum[fl.n - 1];

    const float hw    = (float)pd->band_width * 0.5f;
    /* The field also carries the ticks, which reach past the band. */
    float reach = hw * PB_FIELD_MITRE + 2.0f;
    if (pd->show_ticks) {
        float tl = (float)LV_MAX(pd->major_tick_len, pd->tick_len) * 0.5f
                 + (float)LV_MAX(pd->major_tick_width, pd->tick_width) + 2.0f;
        if (tl > reach) reach = tl;
    }
    float minx = fl.x[0], maxx = fl.x[0], miny = fl.y[0], maxy = fl.y[0];
    for (int i = 1; i < fl.n; i++) {
        if (fl.x[i] < minx) minx = fl.x[i];
        if (fl.x[i] > maxx) maxx = fl.x[i];
        if (fl.y[i] < miny) miny = fl.y[i];
        if (fl.y[i] > maxy) maxy = fl.y[i];
    }

    f = heap_caps_calloc(1, sizeof(pb_field_t), MALLOC_CAP_SPIRAM);
    if (!f) goto out;
    f->area.x1 = (lv_coord_t)floorf(minx - reach);
    f->area.y1 = (lv_coord_t)floorf(miny - reach);
    f->area.x2 = (lv_coord_t)ceilf(maxx + reach);
    f->area.y2 = (lv_coord_t)ceilf(maxy + reach);
    int32_t W = f->area.x2 - f->area.x1 + 1, H = f->area.y2 - f->area.y1 + 1;
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) goto out;
    f->w = (uint16_t)W; f->h = (uint16_t)H;
    size_t npx = (size_t)W * (size_t)H;
    f->cov = heap_caps_calloc(npx, 1, MALLOC_CAP_SPIRAM);
    f->s8  = heap_caps_calloc(npx, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    perp   = heap_caps_malloc(npx, MALLOC_CAP_SPIRAM);
    if (!f->cov || !f->s8 || !perp) goto out;
    memset(perp, 255, npx);
    f->tick = heap_caps_calloc(npx, 1, MALLOC_CAP_SPIRAM);
    if (!f->tick) goto out;

    const int nseg = fl.n - 1;
    /* The square ends' cut lines, from the first and last segments. */
    float e0x = fl.x[0], e0y = fl.y[0], e1x = fl.x[fl.n - 1], e1y = fl.y[fl.n - 1];
    float u0x = 1.0f, u0y = 0.0f, u1x = 1.0f, u1y = 0.0f;
    {
        float L0 = cum[1] - cum[0], L1 = cum[nseg] - cum[nseg - 1];
        if (L0 > 0.0f) { u0x = (fl.x[1] - fl.x[0]) / L0; u0y = (fl.y[1] - fl.y[0]) / L0; }
        if (L1 > 0.0f) { u1x = (fl.x[nseg] - fl.x[nseg - 1]) / L1; u1y = (fl.y[nseg] - fl.y[nseg - 1]) / L1; }
    }
    const float path_len = cum[nseg];
    for (int j = 0; j < nseg; j++) {
        float ax = fl.x[j], ay = fl.y[j];
        float dx = fl.x[j + 1] - ax, dy = fl.y[j + 1] - ay;
        float L = cum[j + 1] - cum[j];
        if (L <= 0.0f) continue;
        float ux = dx / L, uy = dy / L, nx = -uy, ny = ux;
        bool first = (j == 0), last = (j == nseg - 1);
        int x0 = (int)floorf((ax < fl.x[j + 1] ? ax : fl.x[j + 1]) - hw - 2.0f);
        int x1 = (int)ceilf ((ax > fl.x[j + 1] ? ax : fl.x[j + 1]) + hw + 2.0f);
        int y0 = (int)floorf((ay < fl.y[j + 1] ? ay : fl.y[j + 1]) - hw - 2.0f);
        int y1 = (int)ceilf ((ay > fl.y[j + 1] ? ay : fl.y[j + 1]) + hw + 2.0f);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float vx = (float)x + 0.5f - ax, vy = (float)y + 0.5f - ay;
                float al = vx * ux + vy * uy;
                float pe = fabsf(vx * nx + vy * ny);
                if (pe > hw + 1.0f) continue;
                float c, s;
                if (al < 0.0f) {                     /* before this segment */
                    if (!first) continue;            /* a join: the wedge's job */
                    if (pd->rounded) c = _pb_disc_cov(vx, vy, hw);
                    else             c = _pb_edge_cov(hw - pe, nx, ny) * _pb_edge_cov(al, ux, uy);
                    s = 0.0f;
                } else if (al > L) {                 /* past it */
                    if (!last) continue;
                    if (pd->rounded) {
                        float ex = vx - dx, ey = vy - dy;
                        c = _pb_disc_cov(ex, ey, hw);
                    } else {
                        c = _pb_edge_cov(hw - pe, nx, ny) * _pb_edge_cov(L - al, ux, uy);
                    }
                    s = L;
                } else {
                    c = _pb_edge_cov(hw - pe, nx, ny);
                    s = al;
                    /* Near a square end the cut crosses the footprints of
                     * pixels whose centres are inside it too — including
                     * pixels that belong to the second or third segment when
                     * the first is only half a pixel long. Measure against
                     * the path's own end line, not this segment's. */
                    if (!pd->rounded) {
                        if (cum[j] < 1.5f) {
                            float d0 = ((float)x + 0.5f - e0x) * u0x + ((float)y + 0.5f - e0y) * u0y;
                            if (d0 < 1.0f) c *= _pb_edge_cov(d0, u0x, u0y);
                        }
                        if (path_len - cum[j + 1] < 1.5f) {
                            float d1 = (e1x - ((float)x + 0.5f)) * u1x + (e1y - ((float)y + 0.5f)) * u1y;
                            if (d1 < 1.0f) c *= _pb_edge_cov(d1, u1x, u1y);
                        }
                    }
                }
                _pb_field_put(f, perp, x, y, c, pe, (cum[j] + s) * scale);
            }
        }
    }

    /* Mitre wedges. Past the end of the segment arriving at a join and before
     * the start of the one leaving it lies a wedge on the outside of the turn
     * that neither rectangle covers: the intersection of the two bands,
     * clipped at PB_FIELD_MITRE half-widths so a fold-back cannot throw a
     * spike. Skipping the sliver a gentle curve leaves would crack the band
     * radially, so every join gets one. */
    for (int k = 1; k < nseg; k++) {
        float cx = fl.x[k], cy = fl.y[k];
        float La = cum[k] - cum[k - 1], Lb = cum[k + 1] - cum[k];
        if (La <= 0.0f || Lb <= 0.0f) continue;
        float uax = (fl.x[k] - fl.x[k - 1]) / La, uay = (fl.y[k] - fl.y[k - 1]) / La;
        float ubx = (fl.x[k + 1] - fl.x[k]) / Lb, uby = (fl.y[k + 1] - fl.y[k]) / Lb;
        float ox = uax - ubx, oy = uay - uby;              /* points out of the turn */
        float ol = sqrtf(ox * ox + oy * oy);
        if (ol < 1e-5f) continue;                           /* dead straight */
        ox /= ol; oy /= ol;
        /* bound the wedge: the join, both outer band corners, the clipped apex */
        float nax = -uay, nay = uax, nbx = -uby, nby = ubx;
        if (nax * ox + nay * oy < 0.0f) { nax = -nax; nay = -nay; }
        if (nbx * ox + nby * oy < 0.0f) { nbx = -nbx; nby = -nby; }
        float lim = hw * PB_FIELD_MITRE;
        float qx[4] = { cx, cx + nax * hw, cx + nbx * hw, cx + ox * lim };
        float qy[4] = { cy, cy + nay * hw, cy + nby * hw, cy + oy * lim };
        float bx0 = qx[0], bx1 = qx[0], by0 = qy[0], by1 = qy[0];
        for (int q = 1; q < 4; q++) {
            if (qx[q] < bx0) bx0 = qx[q];
            if (qx[q] > bx1) bx1 = qx[q];
            if (qy[q] < by0) by0 = qy[q];
            if (qy[q] > by1) by1 = qy[q];
        }
        for (int y = (int)floorf(by0 - 1.5f); y <= (int)ceilf(by1 + 1.5f); y++) {
            for (int x = (int)floorf(bx0 - 1.5f); x <= (int)ceilf(bx1 + 1.5f); x++) {
                float vx = (float)x + 0.5f - cx, vy = (float)y + 0.5f - cy;
                if (vx * uax + vy * uay <= 0.0f) continue;   /* not past the arriving end */
                if (vx * ubx + vy * uby >= 0.0f) continue;   /* not before the leaving start */
                float pa = fabsf(vx * nax + vy * nay);
                float pb = fabsf(vx * nbx + vy * nby);
                float c  = _pb_edge_cov(hw - pa, nax, nay);
                float cb = _pb_edge_cov(hw - pb, nbx, nby);
                if (cb < c) c = cb;
                float cl = _pb_disc_cov(vx, vy, lim);
                if (cl < c) c = cl;
                _pb_field_put(f, perp, x, y, c, pa < pb ? pa : pb, cum[k] * scale);
            }
        }
    }

    /* keep the centreline: it is what ticks, the fill front and the lead edge
     * are positioned against */
    for (int i = 0; i < fl.n; i++) cum[i] *= scale;
    f->cx = fl.x; f->cy = fl.y; f->cs = cum; f->n = (uint32_t)fl.n;
    fl.x = fl.y = NULL; cum = NULL;
    {
        /* How fast s can run across the band. On a bend of centreline radius
         * R the inside edge is at R - hw, where one px of travel covers
         * R / (R - hw) px of path. A corner, or a bend tighter than one and a
         * half half-widths, has no useful bound (0). */
        float rate = 1.0f;
        f->n_corner = 0;
        const float *cx = f->cx, *cy = f->cy, *cs = f->cs;   /* lengths already in path px */
        for (int q = 1; q + 1 < (int)f->n; q++) {
            float La = cs[q] - cs[q - 1], Lb = cs[q + 1] - cs[q];
            if (La <= 0.0f || Lb <= 0.0f) continue;
            float d = ((cx[q] - cx[q - 1]) * (cx[q + 1] - cx[q]) +
                       (cy[q] - cy[q - 1]) * (cy[q + 1] - cy[q])) / (La * Lb);
            if (d > 1.0f) d = 1.0f;
            if (d < -1.0f) d = -1.0f;
            float turn = acosf(d);
            if (turn < 1e-5f) continue;
            float R = 0.5f * (La + Lb) / turn;
            if (R <= hw * 1.5f) {
                /* a corner: it only matters to a cut near it, so note where */
                if (f->n_corner > 0 && cs[q] - f->corner_s[f->n_corner - 1] < hw) continue;
                if (f->n_corner >= 16) { rate = 0.0f; break; }   /* a jagged path: no bound anywhere */
                f->corner_s[f->n_corner++] = cs[q];
                continue;
            }
            float r = R / (R - hw);
            if (r > rate) rate = r;
        }
        f->srate = rate;
    }
    f->lut_n = (uint32_t)ceilf(pd->total_len) + 2;
    f->lut   = heap_caps_malloc(f->lut_n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    f->tw    = (uint16_t)((W + 15) / 16);
    f->th    = (uint16_t)((H + 15) / 16);
    f->tiles = heap_caps_calloc((size_t)f->tw * f->th, 1, MALLOC_CAP_SPIRAM);
    if (!f->lut || !f->tiles) goto out;
    {
        const float total = pd->total_len;
        lv_color_t bright = pd->lit_color;
        lv_color_t fdim   = lv_color_mix(bright, lv_color_black(), 189);
        const bool red_on = (pd->redline < pd->val_max && pd->val_max > pd->val_min);
        const float rlpx  = red_on ? (pd->redline - pd->val_min) / (pd->val_max - pd->val_min) * total : total;
        const float fade_end = PATHBAR_FADE_FULL * total;
        for (uint32_t q = 0; q < f->lut_n; q++) {
            lv_color_t lc = bright;
            if (pd->fade_fill && fade_end > 0.0f) {
                float t = (float)q / fade_end; if (t > 1.0f) t = 1.0f;
                lc = lv_color_mix(bright, fdim, (uint8_t)(t * 255.0f + 0.5f));
            }
            if (red_on && (float)q >= rlpx) lc = pd->redline_color;
            f->lut[q] = lc;
        }
    }

    pd->field = f;
    _pb_bake_ticks(pd);                 /* after: a custom path samples the field */
    for (int32_t y = 0; y < H; y++) {
        size_t row = (size_t)y * (size_t)W;
        uint8_t *trow = f->tiles + (size_t)(y / 16) * f->tw;
        for (int32_t x = 0; x < W; x++) {
            size_t i = row + (size_t)x;
            if (f->cov[i] | f->tick[i]) trow[x / 16] = 1;
            if (f->cov[i] && f->tick[i]) f->s8[i] |= PB_S8_TICK;
        }
    }
    f = NULL;
    ESP_LOGD(TAG, "field: %d pts -> %dx%d px", fl.n, W, H);

out:
    if (f) {
        if (f->cov)   free(f->cov);
        if (f->s8)    free(f->s8);
        if (f->lut)   free(f->lut);
        if (f->tiles) free(f->tiles);
        if (f->tick)  free(f->tick);
        if (f->cx)    free(f->cx);
        if (f->cy)    free(f->cy);
        if (f->cs)    free(f->cs);
        free(f);
    }
    if (perp) free(perp);
    if (fl.x) free(fl.x);
    if (fl.y) free(fl.y);
    if (cum)  free(cum);
}

/* Generate the path for a parametric shape, fit to the widget box. box is
 * absolute screen px. Replaces prims + pts/cum. */
static void _pathbar_gen_shape(pathbar_data_t *pd, lv_coord_t bx, lv_coord_t by,
                               lv_coord_t bw, lv_coord_t bh) {
    pd->n_prims = 0;

    float inset = pd->band_width / 2.0f + 2.0f;
    float l = bx + inset, t = by + inset;
    float r = bx + bw - inset, b = by + bh - inset;

    if (pd->shape == 4) {                  /* J-hook (Ford digital-cluster tach) */
        /* A constant-radius hook that always leaves with a HORIZONTAL tangent
         * and flows into a straight tail, so the curve meets the bar with no
         * kink. Two independent controls:
         *
         *   hook_angle  — the arc sweep in degrees (90 = quarter circle, so the
         *                 bar starts pointing straight down; 180 = half circle,
         *                 starting pointing back along the tail).
         *   hook_radius — the hook's radius in px. 0 = auto-fit the box.
         *
         * The radius has to be its own control. It used to be DERIVED from the
         * sweep (R = height / (1 - cos S)) so the hook always filled the box,
         * which meant the one shape a J-hook is for — a tight hook with a long
         * straight tail — could not be drawn at any angle: every setting gave
         * one enormous arc and a stub of tail.
         *
         * Built in a canonical frame (hook bottom-left, tail running right
         * along the top) and mirrored into the other three orientations. Four
         * hand-written angle cases were four chances to get a sign wrong, and
         * a mirror cannot disagree with itself. */
        float S = (float)pd->hook_angle;
        if (S < 10.0f) S = 10.0f;
        float Srad = S * (float)M_PI / 180.0f;

        /* How far the arc reaches back from the point where it hands over to
         * the tail: sin S until the sweep passes the quarter turn, a full
         * radius after that (the arc has crossed its own leftmost point). */
        float kx = (S > 90.0f) ? 1.0f : sinf(Srad);
        if (kx < 0.05f) kx = 0.05f;
        float depth = 1.0f - cosf(Srad);
        if (depth < 0.02f) depth = 0.02f;

        float boxW = r - l, boxH = b - t;
        float rMaxH = boxH / depth;
        float rMaxW = (boxW - PB_HOOK_MIN_TAIL) / kx;
        float R = (pd->hook_radius > 0) ? (float)pd->hook_radius
                                        : (rMaxH < rMaxW ? rMaxH : rMaxW);
        if (R > rMaxH) R = rMaxH;
        if (R > rMaxW) R = rMaxW;
        if (R < 8.0f)  R = 8.0f;

        float hx = l + R * kx;             /* where the arc hands over to the tail */
        float hy = t + R;                  /* centre sits directly below that */

        bool mirX = (pd->orientation == 1 || pd->orientation == 3);
        bool mirY = (pd->orientation == 2 || pd->orientation == 3);
        /* Mirroring a circle keeps it a circle: the centre reflects and the
         * angles reflect with it (x-mirror: a -> 180-a, y-mirror: a -> -a). */
        float ccx = mirX ? (l + r - hx) : hx;
        float ccy = mirY ? (t + b - hy) : hy;
        float a0 = 270.0f - S, a1 = 270.0f;
        if (mirX) { a0 = 180.0f - a0; a1 = 180.0f - a1; }
        if (mirY) { a0 = -a0;         a1 = -a1;         }
        _pb_prim_arc(pd, ccx, ccy, R, a0, a1);
        /* Start the tail on the arc's OWN tangent point, read back after
         * quantisation, so the two pieces cannot disagree about where the band
         * is. The tangent is at 270 deg in the canonical frame — straight up
         * from the centre — which mirrors to straight down. */
        const pb_prim_t *ha = &pd->prims[pd->n_prims - 1];
        float ty0 = mirY ? (ha->cy + ha->r) : (ha->cy - ha->r);
        float tx0 = ha->cx;
        float tx1 = mirX ? l : r;
        _pb_prim_line(pd, tx0, ty0, tx1, ty0);
    } else if (pd->shape == 2) {           /* straight */
        if (pd->orientation == 1) {        /* vertical: bottom -> top */
            float cx = (l + r) / 2.0f;
            _pb_prim_line(pd, cx, b, cx, t);
        } else {                           /* horizontal: left -> right */
            float cy = (t + b) / 2.0f;
            _pb_prim_line(pd, l, cy, r, cy);
        }
    } else {                               /* L-bend: shape 1 (rounded) or 3 (45° bevel) */
        float ax, ay, cx, cy, ex, ey;      /* A (start) - C (corner) - B (end) */
        switch (pd->orientation) {
        case 1: ax = r; ay = b; cx = r; cy = t; ex = l; ey = t; break;  /* TR */
        case 2: ax = l; ay = t; cx = l; cy = b; ex = r; ey = b; break;  /* BL */
        case 3: ax = r; ay = t; cx = r; cy = b; ex = l; ey = b; break;  /* BR */
        default: ax = l; ay = b; cx = l; cy = t; ex = r; ey = t; break; /* TL (KTM) */
        }
        float leg1 = fabsf(ay - cy) + fabsf(ax - cx);   /* legs are axis-aligned */
        float leg2 = fabsf(ey - cy) + fabsf(ex - cx);
        float rad = (float)pd->corner_radius;
        float maxr = (leg1 < leg2 ? leg1 : leg2) - 2.0f;
        if (rad > maxr) rad = maxr;
        if (rad < 0.0f) rad = 0.0f;
        float a1x = (ax > cx) - (ax < cx), a1y = (ay > cy) - (ay < cy);
        float b1x = (ex > cx) - (ex < cx), b1y = (ey > cy) - (ey < cy);
        float t1x = cx + a1x * rad, t1y = cy + a1y * rad;
        float t2x = cx + b1x * rad, t2y = cy + b1y * rad;
        float cnx = cx + (a1x + b1x) * rad, cny = cy + (a1y + b1y) * rad;

        if (rad > 1.0f && pd->shape != 3) {
            float d0 = atan2f(t1y - cny, t1x - cnx) * 180.0f / (float)M_PI;
            float d2 = atan2f(t2y - cny, t2x - cnx) * 180.0f / (float)M_PI;
            float d = d2 - d0;
            while (d >  180.0f) d -= 360.0f;
            while (d < -180.0f) d += 360.0f;
            /* Emit the fillet first so the legs can be snapped onto where it
             * actually landed after quantisation, then put it back in order. */
            _pb_prim_arc(pd, cnx, cny, rad, d0, d0 + d);
            pb_prim_t fillet = pd->prims[--pd->n_prims];
            float rr = (float)M_PI / 180.0f;
            float q1x = fillet.cx + fillet.r * cosf(d0 * rr);
            float q1y = fillet.cy + fillet.r * sinf(d0 * rr);
            float q2x = fillet.cx + fillet.r * cosf((d0 + d) * rr);
            float q2y = fillet.cy + fillet.r * sinf((d0 + d) * rr);
            _pb_prim_line(pd, ax, ay, q1x, q1y);
            pd->prims[pd->n_prims++] = fillet;
            _pb_prim_line(pd, q2x, q2y, ex, ey);
        } else {
            _pb_prim_line(pd, ax, ay, t1x, t1y);
            /* shape 3 is a straight 45 deg chamfer, and a zero radius makes
             * the corner square — both are the t1 -> t2 run, which would be a
             * chord straight across the fillet if an arc had been emitted. */
            _pb_prim_line(pd, t1x, t1y, t2x, t2y);
            _pb_prim_line(pd, t2x, t2y, ex, ey);
        }
    }

    _pb_prims_finish(pd);
    _pathbar_build_field(pd);
}

/* Lit fill ramps from dim to full brightness by this fraction of the path
 * (matches the studio preview). Used by the quad-strip renderer below. */
#define PATHBAR_FADE_FULL 0.45f

/* ── Targeted invalidate ────────────────────────────────────────────────────
 * Only redraw the region the fill front actually moved through (the path arc
 * between fractions fa..fb, expanded by the band half-width). Without this, every
 * value change invalidates the whole L-shaped bbox -> the full band re-rasterises
 * (round-cap overdraw over every segment) AND the bg slice under it re-blits, so
 * one moving needle costs ~63% of the screen. Clipping to the delta turns that
 * into a small rect: LVGL clip-rejects the segments outside it for free. */
/* The s-rate bound for a cut anywhere in [s0, s1]: the field's own, unless a
 * sharp corner lies within reach of that stretch, where it is unbounded (0). */
static float _pb_rate_near(const pb_field_t *f, float s0, float s1, float hw) {
    if (!f || f->srate <= 0.0f) return 0.0f;
    const float guard = hw * PB_FIELD_MITRE + 3.0f;
    for (uint8_t i = 0; i < f->n_corner; i++)
        if (f->corner_s[i] > s0 - guard && f->corner_s[i] < s1 + guard) return 0.0f;
    return f->srate;
}

/* How far along the path, either side of a moving fill front, a pixel of the
 * field can change colour: the cut's anti-aliasing, the round cap ahead of
 * the front and the lead edge, stretched by how fast s runs across the band.
 * Near a sharp corner there is no bound on that, and it takes a band's worth. */
static float _pb_field_reach_at(const pathbar_data_t *pd, float s0, float s1) {
    const float hw = (float)pd->band_width * 0.5f;
    const float rate = _pb_rate_near(pd->field, s0, s1, hw);
    const float lw = (float)pd->lead_edge_width * 0.5f + 1.0f;
    if (rate <= 0.0f) return 2.0f * hw + lw + 6.0f;
    float r = 0.75f * rate + 1.5f;
    if (pd->rounded && hw * rate + 2.0f > r) r = hw * rate + 2.0f;
    if (lw * rate + 1.0f > r) r = lw * rate + 1.0f;
    return r + 2.0f;
}

static void _pathbar_invalidate_range(widget_t *w, float fa, float fb) {
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (!w->root || !lv_obj_is_valid(w->root)) return;
    if (pd->n_pts < 2 || pd->total_len <= 0.0f) { lv_obj_invalidate(w->root); return; }

    /* With a field, exactly what can have changed: the stretch of centreline
     * between the two fronts plus the reach, and a half-width (a mitre's
     * worth at a corner) either side of it. The polyline estimate below is
     * sized for the old stroke renderer's spikes, and invalidating that much
     * made LVGL redraw a band-and-a-half square around every move. */
    if (pd->field && pd->field->n >= 2) {
        const pb_field_t *f = pd->field;
        const float sa = (fa < fb ? fa : fb) * pd->total_len, sb = (fa < fb ? fb : fa) * pd->total_len;
        const float reach = _pb_field_reach_at(pd, sa, sb);
        float s0 = sa - reach;
        float s1 = sb + reach;
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        /* Clip each segment to [s0, s1] rather than taking its ends: a straight
         * run is one segment however long it is, and taking both ends of a
         * J-hook's tail invalidated the whole tail on every move. */
        for (uint32_t i = 0; i + 1 < f->n; i++) {
            float a = f->cs[i], b = f->cs[i + 1];
            if (b < s0) continue;
            if (a > s1) break;
            float L = b - a;
            float ta = (a < s0 && L > 0.0f) ? (s0 - a) / L : 0.0f;
            float tb = (b > s1 && L > 0.0f) ? (s1 - a) / L : 1.0f;
            float dx = f->cx[i + 1] - f->cx[i], dy = f->cy[i + 1] - f->cy[i];
            for (int e = 0; e < 2; e++) {
                float t = e ? tb : ta;
                float x = f->cx[i] + dx * t, y = f->cy[i] + dy * t;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
        if (maxx >= minx) {
            const float hw = (float)pd->band_width * 0.5f;
            const float m = (_pb_rate_near(f, s0, s1, hw) > 0.0f ? hw : hw * PB_FIELD_MITRE) + 3.0f;
            lv_area_t area = { (lv_coord_t)floorf(minx - m), (lv_coord_t)floorf(miny - m),
                               (lv_coord_t)ceilf(maxx + m),  (lv_coord_t)ceilf(maxy + m) };
            lv_obj_invalidate_area(w->root, &area);
        }
        return;
    }

    float lo = fa < fb ? fa : fb;
    float hi = fa < fb ? fb : fa;
    float a = lo * pd->total_len;
    float b = hi * pd->total_len;

    lv_coord_t x0 = LV_COORD_MAX, y0 = LV_COORD_MAX, x1 = LV_COORD_MIN, y1 = LV_COORD_MIN;
    bool any = false;
    for (uint16_t i = 1; i < pd->n_pts; i++) {
        float c0 = pd->cum[i - 1], c1 = pd->cum[i];
        if (c1 < a) continue;
        if (c0 > b) break;
        float seg = c1 - c0;
        if (seg <= 0.0f) continue;
        float sa = a > c0 ? (a - c0) / seg : 0.0f;
        float sb = b < c1 ? (b - c0) / seg : 1.0f;
        lv_point_t p0 = pd->pts[i - 1], p1 = pd->pts[i];
        lv_coord_t ax = (lv_coord_t)(p0.x + (p1.x - p0.x) * sa);
        lv_coord_t ay = (lv_coord_t)(p0.y + (p1.y - p0.y) * sa);
        lv_coord_t bx = (lv_coord_t)(p0.x + (p1.x - p0.x) * sb);
        lv_coord_t by = (lv_coord_t)(p0.y + (p1.y - p0.y) * sb);
        x0 = LV_MIN(x0, LV_MIN(ax, bx));  y0 = LV_MIN(y0, LV_MIN(ay, by));
        x1 = LV_MAX(x1, LV_MAX(ax, bx));  y1 = LV_MAX(y1, LV_MAX(ay, by));
        any = true;
    }
    if (!any) { lv_obj_invalidate(w->root); return; }

    /* Margin must cover the widest thing the renderer paints off the centreline:
     * a mitered interior corner reaches up to PB_MITER_MAX*half-width, and round
     * end caps reach half-width. 3/4*band_width = 1.5*half-width covers the
     * miter clamp; +4 slack. Without this, a sharp corner's miter spike painted
     * during a full redraw isn't cleared by a targeted (recede) invalidate. */
    lv_coord_t m = (lv_coord_t)(pd->band_width * 3 / 4) + 4;
    lv_area_t area = { x0 - m, y0 - m, x1 + m, y1 + m };
    lv_obj_invalidate_area(w->root, &area);
}

/* Position (no normal) at arc length s along the path. */
static bool _pathbar_point_at(pathbar_data_t *pd, float s, float *x, float *y) {
    if (pd->n_pts < 2) return false;
    if (s < 0.0f) s = 0.0f;
    if (s > pd->total_len) s = pd->total_len;
    for (uint16_t i = 1; i < pd->n_pts; i++) {
        if (pd->cum[i] >= s) {
            float seg = pd->cum[i] - pd->cum[i - 1];
            float f = seg > 0.0f ? (s - pd->cum[i - 1]) / seg : 0.0f;
            *x = pd->pts[i - 1].x + (pd->pts[i].x - pd->pts[i - 1].x) * f;
            *y = pd->pts[i - 1].y + (pd->pts[i].y - pd->pts[i - 1].y) * f;
            return true;
        }
    }
    *x = pd->pts[pd->n_pts - 1].x;   /* s == total_len within float slop */
    *y = pd->pts[pd->n_pts - 1].y;
    return true;
}

/* Point + unit normal at arc length s along the path. Returns false if the
 * path is empty. The normal is the left-hand perpendicular of the local
 * tangent — used to lay ticks across the band and push labels off it.
 *
 * The tangent is a CENTERED finite difference over a multi-chord window, NOT
 * the single chord s falls in: shape tessellation snaps each ~6px arc point to
 * the integer pixel grid (PB_EMIT), which corrupts any one short chord's
 * direction by up to ±10°. Sampling the path a few chords either side of s
 * averages that rounding noise out, so ticks on a tight curve stay radial
 * instead of skewing randomly. */
/* Exact point and normal at arc length `s` on a PARAMETRIC shape.
 *
 * The polyline sampler below averages the tangent over a window of the path so
 * that pixel-snap noise on a tessellated curve does not make ticks wobble. On
 * an arc that window is the problem rather than the cure: it averages across
 * several degrees of turn, so ticks, numbers and the lead edge all came out
 * slanted instead of radial — visibly not square to the band. A parametric
 * shape knows its own geometry, so take the normal from that: radial on an
 * arc, perpendicular on a straight run, exact either way. */
static bool _pb_prim_sample(pathbar_data_t *pd, float s,
                            float *x, float *y, float *nx, float *ny) {
    if (pd->n_prims == 0) return false;
    if (s < 0.0f) s = 0.0f;
    if (s > pd->total_len) s = pd->total_len;

    for (uint8_t k = 0; k < pd->n_prims; k++) {
        const pb_prim_t *p = &pd->prims[k];
        if (s > p->s1 + 0.001f && k + 1 < pd->n_prims) continue;
        float len = p->s1 - p->s0;
        float f = (len > 0.001f) ? (s - p->s0) / len : 0.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        if (p->kind == PB_PRIM_LINE) {
            float dx = p->x1 - p->x0, dy = p->y1 - p->y0;
            float l = sqrtf(dx * dx + dy * dy);
            if (l < 0.001f) return false;
            *x = p->x0 + dx * f;
            *y = p->y0 + dy * f;
            *nx = -dy / l;                     /* tangent turned a quarter left */
            *ny =  dx / l;
        } else {
            float a  = (p->a0 + (p->a1 - p->a0) * f) * (float)M_PI / 180.0f;
            float ca = cosf(a), sa = sinf(a);
            *x = p->cx + p->r * ca;
            *y = p->cy + p->r * sa;
            /* d/da of (cos a, sin a) is (-sin a, cos a); turned a quarter left
             * that is -(cos a, sin a) — the radius itself. */
            float dir = (p->a1 >= p->a0) ? 1.0f : -1.0f;
            *nx = -ca * dir;
            *ny = -sa * dir;
        }
        return true;
    }
    return false;
}

static bool _pathbar_sample(pathbar_data_t *pd, float s,
                            float *x, float *y, float *nx, float *ny) {
    if (_pb_prim_sample(pd, s, x, y, nx, ny)) return true;
    if (_pb_field_sample(pd->field, s, x, y, nx, ny)) return true;
    if (!_pathbar_point_at(pd, s, x, y)) return false;
    float d = pd->total_len * 0.02f;       /* half-window, ~4 chords for a tach arc */
    if (d < 12.0f) d = 12.0f;
    if (d > pd->total_len * 0.45f) d = pd->total_len * 0.45f;
    /* On a SHARP custom polyline (shape 0, not smoothed) a window that straddles
     * a vertex averages the two segments' directions, so ticks/numbers/fill-edges
     * skew toward the corner's mitre bisector even on a straight run (the user's
     * "redline angled on a straight part" + "5 in a weird spot"). Clamp the window
     * to the segment s sits on → the tangent is the LOCAL segment direction.
     * Smoothed/parametric paths have no sharp corners and keep the wide window so
     * pixel-snap noise on the tessellated curve still averages out. */
    if (pd->shape == 0 && !pd->smooth && pd->cum && pd->n_pts >= 2) {
        for (uint16_t i = 1; i < pd->n_pts; i++) {
            if (pd->cum[i] >= s || i == pd->n_pts - 1) {
                float dl = s - pd->cum[i - 1], dr = pd->cum[i] - s;
                float dm = dl < dr ? dl : dr;
                if (dm < d) d = dm;
                break;
            }
        }
        if (d < 1.0f) d = 1.0f;   /* keep a non-zero chord for the tangent */
    }
    float ax, ay, bx, by;
    _pathbar_point_at(pd, s - d, &ax, &ay);
    _pathbar_point_at(pd, s + d, &bx, &by);
    float tx = bx - ax, ty = by - ay;
    float L = sqrtf(tx * tx + ty * ty); if (L <= 0.0f) L = 1.0f;
    *nx = -ty / L; *ny = tx / L;
    return true;
}

/* ── Coverage-field renderer ────────────────────────────────────────────────
 * See _pathbar_build_field. Colours each pixel of the redraw area from its
 * coverage and its distance along the path, and blends it in strips. */
static lv_color_t *s_strip_col;
static lv_opa_t   *s_strip_mask;
static size_t      s_strip_cap;

static bool _pb_strip_reserve(size_t px) {
    if (px <= s_strip_cap) return true;
    lv_color_t *c = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_opa_t   *m = heap_caps_malloc(px, MALLOC_CAP_SPIRAM);
    if (!c || !m) { if (c) free(c); if (m) free(m); return false; }
    if (s_strip_col)  free(s_strip_col);
    if (s_strip_mask) free(s_strip_mask);
    s_strip_col = c; s_strip_mask = m; s_strip_cap = px;
    return true;
}

/* A boundary across the band — the fill front, the redline — is the normal
 * line through its point on the centreline. Near it, a pixel's coverage comes
 * from its true distance to that line, so the cut is anti-aliased against
 * where it really is. Off the centreline s does not advance one px per px (on
 * the inside of a bend it runs faster), so s alone would give the cut a hard,
 * stepped edge there. `window` is how far in s the line is trusted. */
typedef struct { bool ok; float x, y, tx, ty; } pb_cut_t;

static pb_cut_t _pb_cut_at(const pb_field_t *f, float s) {
    pb_cut_t c = { false, 0, 0, 0, 0 };
    float nx, ny;
    if (_pb_field_sample(f, s, &c.x, &c.y, &nx, &ny)) {
        c.tx = ny; c.ty = -nx;                          /* tangent, pointing along the path */
        c.ok = true;
    }
    return c;
}

/* 0..1: how far a pixel at (px,py) with path position s lies BEHIND the cut.
 * The line is trusted only on the band's own cross-section through the cut
 * point (within `hw` of it across the band, `window` along s); anywhere else
 * — the far side of a fold, the other leg of a square corner — s decides. */
static inline float _pb_behind(const pb_cut_t *c, float cut_s, float px, float py,
                               float s, float hw, float window) {
    if (c->ok && s > cut_s - window && s < cut_s + window) {
        float vx = px - c->x, vy = py - c->y;
        float across = vx * -c->ty + vy * c->tx;
        if (fabsf(across) <= hw + 1.5f)
            return _pb_edge_cov(-(vx * c->tx + vy * c->ty), c->tx, c->ty);
    }
    return (s < cut_s) ? 1.0f : 0.0f;
}

static void _pathbar_draw_field(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                                lv_opa_t master, lv_color_t dim_solid) {
    const pb_field_t *f = pd->field;
    lv_area_t clip;
    if (!_lv_area_intersect(&clip, ctx->clip_area, &f->area)) return;

    const float total = pd->total_len;
    const float hw    = (float)pd->band_width * 0.5f;
    const float F     = pd->cur_frac * total;            /* fill front, px along */
    const bool  empty = pd->cur_frac <= 0.0005f;
    const bool  full  = pd->cur_frac >= 0.9995f;
    const bool red_on = (pd->redline < pd->val_max && pd->val_max > pd->val_min);
    const float rlpx  = red_on ? (pd->redline - pd->val_min) / (pd->val_max - pd->val_min) * total
                               : total;
    /* How far from a cut, in s, a pixel can still sit on it: its footprint is
     * under a px, and across a bend s runs up to f->srate px per px. A field
     * with a sharp corner has no such bound and keeps a band's worth. */
    const float rate     = _pb_rate_near(f, F, F, hw);
    const float window   = (rate > 0.0f) ? 0.75f * rate + 1.5f : hw + 3.0f;
    const float rate_r   = _pb_rate_near(f, rlpx, rlpx, hw);
    const float window_r = (rate_r > 0.0f) ? 0.75f * rate_r + 1.5f : hw + 3.0f;
    /* A rounded bar has a round fill front: a disc of the band's own radius
     * centred on the front, lit where it overlaps the band ahead of it. */
    const float cap_ahead = pd->rounded ? ((rate > 0.0f) ? hw * rate + 2.0f : hw + 2.0f) : 0.0f;

    pb_cut_t front = { false, 0, 0, 0, 0 }, red = { false, 0, 0, 0, 0 };
    if (!empty && !full) front = _pb_cut_at(f, F);
    if (red_on && rlpx > 0.0f) red = _pb_cut_at(f, rlpx);
    const bool round_front = pd->rounded && front.ok;
    const float cap_r2 = (hw + 1.0f) * (hw + 1.0f);

    /* Outside these stretches of s a pixel is plainly lit or plainly dim, and
     * an integer compare and a table lookup settle it (s is in eighths). */
    const bool settled = empty || full;
    const int32_t nlo8 = settled ? INT32_MAX : (int32_t)floorf((F - window - 1.0f) * 8.0f);
    const int32_t nhi8 = settled ? INT32_MIN : (int32_t)ceilf((F + window + cap_ahead + 1.0f) * 8.0f);
    const int32_t rlo8 = red_on ? (int32_t)floorf((rlpx - window_r) * 8.0f) : INT32_MAX;
    const int32_t rhi8 = red_on ? (int32_t)ceilf((rlpx + window_r) * 8.0f) : INT32_MIN;
    const int32_t F8   = empty ? -1 : (full ? INT32_MAX : (int32_t)(F * 8.0f));
    const uint32_t lut_last = f->lut_n - 1;
    const lv_color_t *lut = f->lut;
    const lv_color_t tick_col[3] = { pd->tick_color, pd->major_tick_color, pd->redline_color };

    const int32_t cw = lv_area_get_width(&clip);
    if (!_pb_strip_reserve((size_t)cw * PB_FIELD_ROWS)) return;

    for (int32_t ys = clip.y1; ys <= clip.y2; ys += PB_FIELD_ROWS) {
        int32_t ye = ys + PB_FIELD_ROWS - 1;
        if (ye > clip.y2) ye = clip.y2;
        const int32_t rows = ye - ys + 1;
        lv_memset_00(s_strip_mask, (size_t)cw * (size_t)rows);
        bool any = false;
        for (int32_t y = ys; y <= ye; y++) {
            const int32_t fy = y - f->area.y1;
            const uint8_t *cov = f->cov + (size_t)fy * f->w;
            const uint16_t *s8 = f->s8 + (size_t)fy * f->w;
            const uint8_t *tick = f->tick + (size_t)fy * f->w;
            const uint8_t *trow = f->tiles + (size_t)(fy / 16) * f->tw;
            lv_opa_t *mrow = s_strip_mask + (size_t)(y - ys) * (size_t)cw;
            lv_color_t *crow = s_strip_col + (size_t)(y - ys) * (size_t)cw;
            const float pyc = (float)y + 0.5f;
            int32_t x = clip.x1;
            while (x <= clip.x2) {
                const int32_t fx = x - f->area.x1;
                const int32_t tile_end = LV_MIN(clip.x2, f->area.x1 + ((fx / 16) + 1) * 16 - 1);
                if (!trow[fx / 16]) { x = tile_end + 1; continue; }   /* no band in this tile */
                for (; x <= tile_end; x++) {
                    const int32_t fxi = x - f->area.x1;
                    const lv_opa_t c = cov[fxi];
                    const int32_t v = s8[fxi];            /* top bit: a tick lies over this pixel */
                    const int32_t k = x - clip.x1;
                    if (c == 0) {
                        const uint8_t t = tick[fxi];
                        if (!t) continue;
                        any = true;                        /* a tick reaching off the band */
                        uint8_t who = (uint8_t)(t >> 6); if (who > 2) who = 2;
                        mrow[k] = (lv_opa_t)(((uint32_t)(t & 63) * 255u + 31u) / 63u);
                        crow[k] = tick_col[who];
                        continue;
                    }
                    any = true;
                    mrow[k] = c;
                    if ((v & PB_S8_TICK) == 0 && (v < nlo8 || v > nhi8) && (v < rlo8 || v > rhi8)) {
                        if (v <= F8) {
                            uint32_t q = (uint32_t)(v >> 3);
                            crow[k] = lut[q > lut_last ? lut_last : q];
                        } else {
                            crow[k] = dim_solid;
                        }
                        continue;
                    }

                    const int32_t vs = v & PB_S8_MASK;
                    lv_color_t out = dim_solid;
                    if ((vs < nlo8 || vs > nhi8) && (vs < rlo8 || vs > rhi8)) {
                        if (vs <= F8) {
                            uint32_t q = (uint32_t)(vs >> 3);
                            out = lut[q > lut_last ? lut_last : q];
                        }
                    } else {
                        const float s = (float)vs * 0.125f;
                        const float pxc = (float)x + 0.5f;
                        float a;                          /* how lit, 0..1 */
                        if (empty)                           a = 0.0f;
                        else if (full || s < F - window)     a = 1.0f;
                        else if (s > F + window + cap_ahead) a = 0.0f;
                        else {
                            a = (s < F) ? 1.0f : 0.0f;
                            if (front.ok && s > F - window && s < F + window) {
                                /* on the band's cross-section through the front,
                                 * the pixel's true distance to the cut decides;
                                 * a footprint is at most 0.71 px across */
                                float vx = pxc - front.x, vy = pyc - front.y;
                                float along = vx * front.tx + vy * front.ty;
                                float across = vx * -front.ty + vy * front.tx;
                                if (fabsf(across) <= hw + 1.5f) {
                                    if (along <= -0.75f)      a = 1.0f;
                                    else if (along >= 0.75f)  a = 0.0f;
                                    else                      a = _pb_edge_cov(-along, front.tx, front.ty);
                                }
                            }
                            if (round_front && a < 1.0f && s >= F - 1.0f) {
                                float dx = pxc - front.x, dy = pyc - front.y;
                                if (dx * dx + dy * dy < cap_r2) {
                                    float ac = _pb_disc_cov(dx, dy, hw);
                                    if (ac > a) a = ac;
                                }
                            }
                        }
                        if (a > 0.0f) {
                            float sc = (s < F || full) ? s : F;   /* the cap takes the front's colour */
                            uint32_t q = (uint32_t)sc;
                            lv_color_t lc = lut[q > lut_last ? lut_last : q];
                            if (red_on && sc > rlpx - window_r && sc < rlpx + window_r) {
                                /* the table cuts on a whole px; anti-alias against the true line */
                                uint32_t qb = (uint32_t)LV_MAX(0.0f, rlpx - window_r - 1.0f);
                                lv_color_t base = lut[qb > lut_last ? lut_last : qb];
                                float r = 1.0f - _pb_behind(&red, rlpx, pxc, pyc, sc, hw, window_r);
                                lc = (r <= 0.0f) ? base
                                   : lv_color_mix(pd->redline_color, base, (uint8_t)(r * 255.0f + 0.5f));
                            }
                            out = (a >= 1.0f) ? lc
                                : lv_color_mix(lc, dim_solid, (uint8_t)(a * 255.0f + 0.5f));
                        }
                    }
                    if (v & PB_S8_TICK) {
                        /* tick over band, straight alpha: the tick's share of
                         * the result is ta / (ta + band * (1 - ta)) */
                        const uint8_t t = tick[fxi];
                        uint8_t who = (uint8_t)(t >> 6); if (who > 2) who = 2;
                        const uint32_t ta = ((uint32_t)(t & 63) * 255u + 31u) / 63u;
                        const uint32_t oa = ta + ((255u - ta) * c + 127u) / 255u;
                        const uint32_t share = oa ? (ta * 255u + oa / 2u) / oa : 0u;
                        out = lv_color_mix(tick_col[who], out, (uint8_t)(share > 255u ? 255u : share));
                        mrow[k] = (lv_opa_t)(oa > 255u ? 255u : oa);
                    }
                    crow[k] = out;
                }
            }
        }
        if (!any) continue;

        lv_area_t strip = { clip.x1, (lv_coord_t)ys, clip.x2, (lv_coord_t)ye };
        lv_draw_sw_blend_dsc_t bd;
        lv_memset_00(&bd, sizeof(bd));
        bd.blend_area = &strip;
        bd.src_buf    = s_strip_col;
        bd.mask_buf   = s_strip_mask;
        bd.mask_res   = LV_DRAW_MASK_RES_CHANGED;
        bd.mask_area  = &strip;
        bd.opa        = master;
        bd.blend_mode = LV_BLEND_MODE_NORMAL;
        lv_draw_sw_blend(ctx, &bd);
    }
}

/* A straight mark of width w from (x0,y0) to (x1,y1), anti-aliased against its
 * true float position. lv_draw_line takes whole-pixel endpoints, so a comb of
 * ticks round a curve came out unevenly spaced and each one slightly off its
 * angle — visible the moment you zoom in. Round ends when `round`, else cut
 * square at the endpoints. */
static void _pb_draw_mark(lv_draw_ctx_t *ctx, float x0, float y0, float x1, float y1,
                          float w, bool round, lv_color_t color, lv_opa_t opa) {
    if (w <= 0.0f || opa <= LV_OPA_MIN) return;
    float dx = x1 - x0, dy = y1 - y0;
    float L = sqrtf(dx * dx + dy * dy);
    if (L < 1e-4f) return;
    float ux = dx / L, uy = dy / L;
    const float hw = w * 0.5f, pad = hw + 1.5f;

    lv_area_t box = {
        (lv_coord_t)floorf((x0 < x1 ? x0 : x1) - pad), (lv_coord_t)floorf((y0 < y1 ? y0 : y1) - pad),
        (lv_coord_t)ceilf ((x0 > x1 ? x0 : x1) + pad), (lv_coord_t)ceilf ((y0 > y1 ? y0 : y1) + pad),
    };
    lv_area_t clip;
    if (!_lv_area_intersect(&clip, ctx->clip_area, &box)) return;
    const int32_t cw = lv_area_get_width(&clip), ch = lv_area_get_height(&clip);
    if (!_pb_strip_reserve((size_t)cw * (size_t)ch)) return;

    size_t k = 0;
    for (int32_t y = clip.y1; y <= clip.y2; y++) {
        for (int32_t x = clip.x1; x <= clip.x2; x++, k++) {
            float vx = (float)x + 0.5f - x0, vy = (float)y + 0.5f - y0;
            float al = vx * ux + vy * uy;
            float pe = fabsf(vx * -uy + vy * ux);
            float c;
            if (round) {
                float t = al < 0.0f ? 0.0f : (al > L ? L : al);
                float ex = vx - ux * t, ey = vy - uy * t;
                c = _pb_disc_cov(ex, ey, hw);
            } else {
                c = _pb_edge_cov(hw - pe, -uy, ux) * _pb_edge_cov(al, ux, uy) * _pb_edge_cov(L - al, ux, uy);
            }
            s_strip_mask[k] = (lv_opa_t)(c * 255.0f + 0.5f);
        }
    }
    lv_draw_sw_blend_dsc_t bd;
    lv_memset_00(&bd, sizeof(bd));
    bd.blend_area = &clip;
    bd.color      = color;
    bd.mask_buf   = s_strip_mask;
    bd.mask_res   = LV_DRAW_MASK_RES_CHANGED;
    bd.mask_area  = &clip;
    bd.opa        = opa;
    bd.blend_mode = LV_BLEND_MODE_NORMAL;
    lv_draw_sw_blend(ctx, &bd);
}

/* ── Draw the optional tick + number scale along the path ────────────────────
 * Ticks are perpendicular marks centred on the band; major-tick numbers sit
 * just off the band toward the path centroid (so they read on the inside of a
 * curve, exactly like the Ford-style tach). All in absolute screen px, matching
 * the bands. Cheap because LVGL clip-rejects marks outside the redraw area. */
static void _pathbar_draw_scale(lv_draw_ctx_t *ctx, pathbar_data_t *pd) {
    if (!pd->show_ticks || pd->minor_tick_step <= 0.0f ||
        pd->val_max <= pd->val_min || pd->n_pts < 2 || pd->total_len <= 0.0f)
        return;

    float range = pd->val_max - pd->val_min;
    int N = (int)lroundf(range / pd->minor_tick_step);
    if (N < 1) N = 1;
    if (N > 500) N = 500;
    int majEvery = pd->major_tick_step > 0.0f
                 ? (int)lroundf(pd->major_tick_step / pd->minor_tick_step) : 1;
    if (majEvery < 1) majEvery = 1;
    if (pd->field && !pd->show_labels) return;          /* the ticks are baked into the field */

    /* path centroid → which way the numbers face */
    float cgx = 0, cgy = 0;
    for (uint16_t i = 0; i < pd->n_pts; i++) { cgx += pd->pts[i].x; cgy += pd->pts[i].y; }
    cgx /= pd->n_pts; cgy /= pd->n_pts;

    const lv_font_t *lf = pd->label_font[0] ? widget_resolve_font(pd->label_font) : NULL;
    if (!lf) lf = LV_FONT_DEFAULT;

    lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
    ld.font = lf; ld.color = pd->label_color;

    uint16_t div = pd->tick_label_divisor ? pd->tick_label_divisor : 1;

    for (int i = 0; i <= N; i++) {
        float v = pd->val_min + (float)i * pd->minor_tick_step;
        float fr = (v - pd->val_min) / range;
        if (fr < 0.0f) fr = 0.0f;
        if (fr > 1.0f) fr = 1.0f;
        float px, py, nx, ny;
        if (!_pathbar_sample(pd, fr * pd->total_len, &px, &py, &nx, &ny)) continue;

        bool isMaj = (i % majEvery) == 0;
        uint8_t len = isMaj ? pd->major_tick_len   : pd->tick_len;
        uint8_t wdt = isMaj ? pd->major_tick_width : pd->tick_width;
        lv_color_t col = isMaj ? pd->major_tick_color : pd->tick_color;
        if (pd->redline_recolor_ticks && pd->redline < pd->val_max && v >= pd->redline)
            col = pd->redline_color;

        if (len > 0 && wdt > 0 && !pd->field) {
            /* The tick's own direction. Normally the band normal; tick_slant
             * rotates it about the tick's midpoint so the comb leans, without
             * moving where along the path the tick sits. The numbers below
             * keep using the true normal, so they stay upright and outboard. */
            float tnx = nx, tny = ny;
            if (pd->tick_slant) {
                float a  = (float)pd->tick_slant * 3.14159265f / 180.0f;
                float ca = cosf(a), sa = sinf(a);
                tnx = nx * ca - ny * sa;
                tny = nx * sa + ny * ca;
            }
            _pb_draw_mark(ctx, px - tnx * len / 2.0f, py - tny * len / 2.0f,
                          px + tnx * len / 2.0f, py + tny * len / 2.0f,
                          (float)wdt, true, col, LV_OPA_COVER);
        }

        if (isMaj && pd->show_labels) {
            /* The number's anchor can be nudged ALONG the path (arc-length) so it
             * sits where the user wants instead of dead-on the tick. */
            float lpx = px, lpy = py, lnx = nx, lny = ny;
            if (pd->label_along_offset != 0) {
                float ls = fr * pd->total_len + (float)pd->label_along_offset;
                if (ls < 0.0f) ls = 0.0f;
                if (ls > pd->total_len) ls = pd->total_len;
                _pathbar_sample(pd, ls, &lpx, &lpy, &lnx, &lny);
            }
            /* Side: auto faces the centroid (smooth-arc default); label_side locks
             * every number to ONE fixed side so sharp multi-corner paths don't flip. */
            float sgn;
            if (pd->label_side == 1)      sgn =  1.0f;
            else if (pd->label_side == 2) sgn = -1.0f;
            else sgn = ((cgx - lpx) * lnx + (cgy - lpy) * lny) >= 0.0f ? 1.0f : -1.0f;
            /* Clamp off >= 0 so a negative label_gap can pull numbers toward the
             * band but never drag them across to the wrong side (the -40 bug). */
            float off = pd->band_width / 2.0f + (float)pd->label_gap;
            if (off < 0.0f) off = 0.0f;
            float lx = lpx + lnx * sgn * off, ly = lpy + lny * sgn * off;
            char buf[16];
            float dv = v / (float)div;
            if (fabsf(dv - lroundf(dv)) < 0.05f) snprintf(buf, sizeof buf, "%d", (int)lroundf(dv));
            else                                 snprintf(buf, sizeof buf, "%.1f", dv);
            lv_point_t ts;
            lv_txt_get_size(&ts, buf, lf, 0, 0, LV_COORD_MAX, 0);
            lv_coord_t lxi = (lv_coord_t)lroundf(lx) - ts.x / 2;
            lv_coord_t lyi = (lv_coord_t)lroundf(ly) - ts.y / 2;
            lv_area_t la = { lxi, lyi, lxi + ts.x, lyi + ts.y };
            lv_draw_label(ctx, &ld, &la, buf, NULL);
        }
    }
}

/* ── Band renderer (routed by path shape) ────────────────────────────────────
 * Two fill methods, chosen by `sharp` below, because a hand-drawn angular
 * polyline and a smooth/parametric curve want opposite things:
 *
 *  SHARP custom polyline (shape 0, not smoothed -- the VK stairstep tach): one
 *    PERPENDICULAR-ended rectangle per straight segment + a triangle-fan miter
 *    join at each interior vertex. Every colour edge (fill front, redline break,
 *    gradient step) is square to the LOCAL segment instead of skewed toward a
 *    corner's miter bisector, and a solid run is a SINGLE quad so a long diagonal
 *    gets one clean AA pass (no ~N stacked 4 px slivers = no fuzz). Few long
 *    segments, so the per-segment seams are negligible.
 *
 *  SMOOTH / parametric (J-hook, L-bend, straight, Catmull-Rom): the continuous
 *    mitered quad-strip, each quad poking PB_SEAM_OVERLAP px into the next so the
 *    later opaque quad buries the previous AA edge. On a ~50-segment tessellated
 *    curve that is what keeps the arc smooth -- perpendicular per-segment rects
 *    would show every facet/seam ("very liney"), and the miter interpolation is
 *    already ~perpendicular at those tiny angles so the cross-cut never skews. */
#define PB_SEAM_OVERLAP 2.0f    /* px each smooth-strip quad pokes into the next    */
#define PB_GRAD_STEP    4.0f    /* px per gradient sub-quad (fade zone/smooth strip)*/
#define PB_STROKE_FADE_BANDS 8  /* colour runs across a stroked fade zone */
#define PB_MITER_MAX    1.5f    /* cap miter at 1.5*half-width (clean to ~96°, then  */
                                /* a slight bevel) — bounds the off-centreline reach */
                                /* so _pathbar_invalidate_range can cover it.        */

/* Interpolate the band cross-edge (the two mitered offset points) at arc-length
 * s. Used by the SMOOTH/parametric strip; the SHARP renderer uses perpendicular
 * offsets instead (see _pb_seg_quad / _pb_join_fan). */
static void _spk_edge_at(const lv_point_t *L, const lv_point_t *R, const float *cum,
                         int n, float s, lv_point_t *oL, lv_point_t *oR) {
    if (s <= cum[0])     { *oL = L[0];   *oR = R[0];   return; }
    if (s >= cum[n - 1]) { *oL = L[n-1]; *oR = R[n-1]; return; }
    for (int i = 1; i < n; i++) {
        if (cum[i] >= s) {
            float seg = cum[i] - cum[i - 1];
            float fr  = seg > 0.0f ? (s - cum[i - 1]) / seg : 0.0f;
            oL->x = (lv_coord_t)lroundf(L[i-1].x + (L[i].x - L[i-1].x) * fr);
            oL->y = (lv_coord_t)lroundf(L[i-1].y + (L[i].y - L[i-1].y) * fr);
            oR->x = (lv_coord_t)lroundf(R[i-1].x + (R[i].x - R[i-1].x) * fr);
            oR->y = (lv_coord_t)lroundf(R[i-1].y + (R[i].y - R[i-1].y) * fr);
            return;
        }
    }
    *oL = L[n-1]; *oR = R[n-1];
}

/* One convex polygon fill. The LVGL 9 port (RDM-Lume-149) fans this out into
 * triangles because v9 dropped lv_draw_polygon; v8 still has it, and a single
 * polygon has no interior diagonal to seam. Kept as a wrapper so the two
 * copies of this file stay line-for-line comparable. */
static void _pb_draw_poly(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *rd,
                          const lv_point_t *pts, int cnt) {
    if (cnt < 3) return;
    lv_draw_polygon(ctx, rd, pts, (uint16_t)cnt);
}

/* Filled half-disc terminal cap for rounded ends: centred at (cx,cy), radius hw,
 * bulging in the outward tangent (tx,ty). Convex → one lv_draw_polygon. */
static void _spk_round_cap(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *rd,
                           float cx, float cy, float tx, float ty, float hw) {
    float l = sqrtf(tx*tx + ty*ty);
    if (l <= 0.001f || hw < 1.0f) return;
    tx /= l; ty /= l;
    float nx = -ty, ny = tx;                       /* perpendicular (band cross-axis) */
    enum { K = 10 };
    lv_point_t pts[K + 1];
    for (int j = 0; j <= K; j++) {
        float phi = (float)M_PI * (0.5f - (float)j / (float)K);   /* +90°..-90° */
        float s = sinf(phi), c = cosf(phi);
        pts[j].x = (lv_coord_t)lroundf(cx + hw * (s*nx + c*tx));
        pts[j].y = (lv_coord_t)lroundf(cy + hw * (s*ny + c*ty));
    }
    _pb_draw_poly(ctx, rd, pts, K + 1);
}

/* lv_draw_polygon (LVGL sw) ASSUMES a simple convex polygon — its scanline
 * edge-walk never terminates on a self-intersecting one, hanging the LVGL task
 * (watchdog → reboot). A band trapezoid can bowtie on a tight concave curve
 * (inner radius < half-width) or collapse to ~zero area at the clipped fill
 * front. So fill every quad as TWO triangles (a triangle can't self-intersect),
 * skipping any with sub-pixel area — LVGL then only ever sees a clean triangle. */
static inline bool _pb_tri_ok(lv_point_t a, lv_point_t b, lv_point_t c) {
    long cross = (long)(b.x - a.x) * (c.y - a.y) - (long)(b.y - a.y) * (c.x - a.x);
    return cross > 1 || cross < -1;            /* 2*area; skip near-collinear */
}
/* A simple convex quad can be filled as ONE polygon — no interior diagonal, so
 * no anti-aliased seam line through the band. Standard test: every consecutive
 * edge turns the same way (all cross products share a sign). A self-intersecting
 * "bowtie" (the tight-curve case _pb_fill_quad guards against) fails this and
 * falls back to the safe two-triangle split, so lv_draw_polygon never sees a
 * non-convex polygon. */
static inline bool _pb_quad_convex(lv_point_t a, lv_point_t b, lv_point_t c, lv_point_t d) {
    lv_point_t p[4] = { a, b, c, d };
    int sign = 0;
    for (int i = 0; i < 4; i++) {
        lv_point_t o = p[i], u = p[(i + 1) & 3], v = p[(i + 2) & 3];
        long cr = (long)(u.x - o.x) * (v.y - o.y) - (long)(u.y - o.y) * (v.x - o.x);
        if (cr == 0) continue;                  /* colinear vertex — skip */
        int s = cr > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        else if (s != sign) return false;        /* reflex/crossing turn → not convex */
    }
    return sign != 0;                            /* sign==0 → fully degenerate */
}
static void _pb_fill_quad(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *rd,
                          lv_point_t a, lv_point_t b, lv_point_t c, lv_point_t d) {
    if (_pb_quad_convex(a, b, c, d)) {
        lv_point_t q[4] = { a, b, c, d };        /* one fill = no diagonal seam */
        _pb_draw_poly(ctx, rd, q, 4);
        return;
    }
    if (_pb_tri_ok(a, b, c)) { lv_point_t t[3] = { a, b, c }; _pb_draw_poly(ctx, rd, t, 3); }
    if (_pb_tri_ok(a, c, d)) { lv_point_t t[3] = { a, c, d }; _pb_draw_poly(ctx, rd, t, 3); }
}

/* One perpendicular-ended band rectangle for a straight run (pax,pay)->(pbx,pby)
 * on a segment whose unit perpendicular is (nx,ny): square ends + clean long
 * edges as a single convex quad (falls back to two triangles if it pinches). */
static void _pb_seg_quad(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *rd,
                         float pax, float pay, float pbx, float pby,
                         float nx, float ny, float hw) {
    lv_point_t q0 = { (lv_coord_t)lroundf(pax + nx*hw), (lv_coord_t)lroundf(pay + ny*hw) };
    lv_point_t q1 = { (lv_coord_t)lroundf(pax - nx*hw), (lv_coord_t)lroundf(pay - ny*hw) };
    lv_point_t q2 = { (lv_coord_t)lroundf(pbx - nx*hw), (lv_coord_t)lroundf(pby - ny*hw) };
    lv_point_t q3 = { (lv_coord_t)lroundf(pbx + nx*hw), (lv_coord_t)lroundf(pby + ny*hw) };
    _pb_fill_quad(ctx, rd, q0, q1, q2, q3);
}

/* Miter join fan at interior vertex v: bridges the perpendicular-ended segment
 * rects on either side out to the mitered apexes L[v]/R[v], reconstructing the
 * clean corner. A triangle fan from the centreline point can't self-intersect,
 * so lv_draw_polygon stays safe; _pb_tri_ok drops the degenerate slivers a
 * near-straight (tessellated-curve) vertex produces. */
static void _pb_join_fan(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *rd,
                         pathbar_data_t *pd, const lv_point_t *L,
                         const lv_point_t *R, int v, float hw) {
    float idx = pd->pts[v].x - pd->pts[v-1].x, idy = pd->pts[v].y - pd->pts[v-1].y;
    float il = sqrtf(idx*idx + idy*idy); if (il <= 0.0f) il = 1.0f;
    float inx = -idy/il, iny = idx/il;
    float odx = pd->pts[v+1].x - pd->pts[v].x, ody = pd->pts[v+1].y - pd->pts[v].y;
    float ol = sqrtf(odx*odx + ody*ody); if (ol <= 0.0f) ol = 1.0f;
    float onx = -ody/ol, ony = odx/ol;
    float px = pd->pts[v].x, py = pd->pts[v].y;
    lv_point_t P = { (lv_coord_t)lroundf(px), (lv_coord_t)lroundf(py) };
    lv_point_t b[6] = {
        { (lv_coord_t)lroundf(px + inx*hw), (lv_coord_t)lroundf(py + iny*hw) },
        L[v],
        { (lv_coord_t)lroundf(px + onx*hw), (lv_coord_t)lroundf(py + ony*hw) },
        { (lv_coord_t)lroundf(px - onx*hw), (lv_coord_t)lroundf(py - ony*hw) },
        R[v],
        { (lv_coord_t)lroundf(px - inx*hw), (lv_coord_t)lroundf(py - iny*hw) },
    };
    for (int k = 0; k < 6; k++) {
        lv_point_t e0 = b[k], e1 = b[(k + 1) % 6];
        if (_pb_tri_ok(P, e0, e1)) {
            lv_point_t tri[3] = { P, e0, e1 };
            _pb_draw_poly(ctx, rd, tri, 3);
        }
    }
}

/* Lit-fill colour at path fraction pc: redline takes over past rl, else an
 * optional dim->bright fade across the first PATHBAR_FADE_FULL of the path. */
static lv_color_t _pb_lit_color(pathbar_data_t *pd, float pc, float rl,
                                lv_color_t bright, lv_color_t dim) {
    if (pc > rl) return pd->redline_color;
    if (pd->fade_fill) {
        float t = pc / PATHBAR_FADE_FULL; if (t > 1.0f) t = 1.0f;
        return lv_color_mix(bright, dim, (uint8_t)(t * 255.0f));
    }
    return bright;
}

/* ── Primitive renderer (parametric shapes) ─────────────────────────────────
 * One lv_draw_arc / lv_draw_line per piece. Both rasterise the whole piece in
 * a single anti-aliased pass, which is the only way to get a clean edge here:
 * two anti-aliased shapes that overlap blend twice — a 50% edge over a 50%
 * edge reads 75% — so any renderer built out of overlapping pieces shows its
 * seams even in one flat colour. That is what a stroked polyline is, and why
 * it scalloped on the outside of every bend however fine the step. */

/* Draw the [f0..f1] fraction of one primitive. */
static void _pb_draw_prim(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                          const pb_prim_t *p, float f0, float f1,
                          lv_color_t color, lv_opa_t opa, bool cap0, bool cap1,
                          bool open0, bool open1, float ext0, float ext1) {
    if (f1 - f0 < 0.0005f) return;
    if (p->kind == PB_PRIM_ARC) {
        if (p->outer_r <= (float)pd->band_width * 0.5f + 1.0f) return;
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.color    = color;
        ad.opa      = opa;
        ad.width    = pd->band_width;
        lv_point_t centre = { (lv_coord_t)p->cx, (lv_coord_t)p->cy };
        /* lv_draw_arc measures to the OUTER edge; our radius is the centreline */
        uint16_t radius = (uint16_t)p->outer_r;  /* quantised at generation */
        /* lv_draw_arc rounds both ends or neither, so an arc that starts the
         * path and hands over to a tangent line gets a round cap at the
         * handover too. That cap is a half-disc of the same colour sitting
         * inside the band the next piece draws over it, so it disappears —
         * whereas demanding BOTH ends want rounding loses the cap the path
         * actually has. */
        ad.rounded  = (cap0 || cap1) ? 1 : 0;
        float aa = p->a0 + (p->a1 - p->a0) * f0;
        float bb = p->a0 + (p->a1 - p->a0) * f1;
        bool o0 = open0, o1 = open1;
        if (bb < aa) {                                   /* lv_draw_arc is CW */
            float tmp = aa; aa = bb; bb = tmp;
            bool tb = o0;   o0 = o1;  o1 = tb;
        }
        /* Angles are whole degrees here (LV_USE_FLOAT is off), and rounding an
         * interior cut can round it AWAY from its neighbour — a hairline crack
         * across the band. An end that butts onto more of the same span is
         * expanded outwards instead, so consecutive pieces always overlap. */
        int32_t start_angle = o0 ? (int32_t)floorf(aa) - 1 : (int32_t)lroundf(aa);
        /* A real (not butted) end is the fill FRONT, and lv_draw_arc can only
         * put it on a whole degree — which on a big radius is several pixels,
         * so the bar advanced in visible jumps. Stop the arc on the degree
         * BELOW and draw the remainder as a short chord: at under a degree it
         * is a few pixels long and indistinguishable from the arc, but the
         * front now moves continuously. */
        float tail_from = o1 ? 0.0f : floorf(bb);
        int32_t end_angle = o1 ? (int32_t)ceilf(bb) + 1 : (int32_t)tail_from;
        if (end_angle <= start_angle) end_angle = start_angle + 1;
        /* v8 takes unsigned angles and wraps an end below its start across 0,
         * so fold both into 0..359 — a mirrored J-hook sweeps through negative
         * degrees. Exactly 360 apart is the one way to ask for the whole ring. */
        int32_t sweep = end_angle - start_angle;
        int32_t sa = ((start_angle % 360) + 360) % 360;
        int32_t ea = (sweep >= 360) ? sa + 360 : (((end_angle % 360) + 360) % 360);
        lv_draw_arc(ctx, &ad, &centre, radius, (uint16_t)sa, (uint16_t)ea);
        if (!o1 && bb - tail_from > 0.02f && tail_from >= aa) {
            float rr = (float)M_PI / 180.0f;
            lv_draw_line_dsc_t sd;
            lv_draw_line_dsc_init(&sd);
            sd.color = color;
            sd.opa   = opa;
            sd.width = pd->band_width;
            sd.round_start = false;
            sd.round_end   = cap1;
            lv_point_t q1 = { (lv_coord_t)lroundf(p->cx + p->r * cosf(tail_from * rr)),
                              (lv_coord_t)lroundf(p->cy + p->r * sinf(tail_from * rr)) };
            lv_point_t q2 = { (lv_coord_t)lroundf(p->cx + p->r * cosf(bb * rr)),
                              (lv_coord_t)lroundf(p->cy + p->r * sinf(bb * rr)) };
            if (q1.x != q2.x || q1.y != q2.y) lv_draw_line(ctx, &sd, &q1, &q2);
        }
    } else {
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = color;
        ld.opa   = opa;
        ld.width = pd->band_width;
        ld.round_start = cap0;
        ld.round_end   = cap1;
        float ax = p->x0 + (p->x1 - p->x0) * f0, ay = p->y0 + (p->y1 - p->y0) * f0;
        float bx = p->x0 + (p->x1 - p->x0) * f1, by = p->y0 + (p->y1 - p->y0) * f1;
        float dx = bx - ax, dy = by - ay;
        float ln = sqrtf(dx * dx + dy * dy);
        if (ln > 0.001f) {
            /* Push an interior cut out past its neighbour by however much the
             * caller says that neighbour needs — see _pb_paint_span. */
            dx /= ln; dy /= ln;
            if (open0) { ax -= dx * ext0; ay -= dy * ext0; }
            if (open1) { bx += dx * ext1; by += dy * ext1; }
        }
        lv_point_t q1 = { (lv_coord_t)lroundf(ax), (lv_coord_t)lroundf(ay) };
        lv_point_t q2 = { (lv_coord_t)lroundf(bx), (lv_coord_t)lroundf(by) };
        lv_draw_line(ctx, &ld, &q1, &q2);
    }
}

/* Unit tangent at one end of a primitive, pointing ALONG the path. */
static void _pb_prim_tangent(const pb_prim_t *p, bool at_end, float *tx, float *ty) {
    if (p->kind == PB_PRIM_LINE) {
        float dx = p->x1 - p->x0, dy = p->y1 - p->y0;
        float ln = sqrtf(dx * dx + dy * dy);
        if (ln < 0.001f) { *tx = 1.0f; *ty = 0.0f; return; }
        *tx = dx / ln; *ty = dy / ln;
    } else {
        float a = (at_end ? p->a1 : p->a0) * (float)M_PI / 180.0f;
        float sgn = (p->a1 >= p->a0) ? 1.0f : -1.0f;
        *tx = -sinf(a) * sgn; *ty = cosf(a) * sgn;
    }
}

/* How far a straight run must reach past a corner to fill the wedge on the
 * outside of it: half a band times tan(half the turn). A right angle wants the
 * full half-band; the four-degree kinks a fitted shallow curve is made of want
 * well under a pixel, and reaching half a band into one of those paints a
 * second anti-aliased edge along the first — the seam this is all about. */
static float _pb_mitre(const pathbar_data_t *pd, uint8_t a, uint8_t b) {
    const float slop = 1.5f, cap = (float)pd->band_width * 0.5f + slop;
    float ax, ay, bx, by;
    _pb_prim_tangent(&pd->prims[a], true,  &ax, &ay);
    _pb_prim_tangent(&pd->prims[b], false, &bx, &by);
    float d = ax * bx + ay * by;
    if (d >  1.0f) d =  1.0f;
    if (d < -1.0f) d = -1.0f;
    float half = acosf(d) * 0.5f;
    if (half > 1.45f) return cap;                    /* near a fold-back */
    float ext = (float)pd->band_width * 0.5f * tanf(half) + slop;
    return (ext > cap) ? cap : ext;
}

/* Paint the arc-length span [s_from, s_to] across however many primitives it
 * covers. Interior handovers are tangent, so the two pieces butt along the
 * same straight cut; they are overlapped by a pixel so the cut cannot show as
 * a gap. */
static void _pb_paint_span(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                           float s_from, float s_to, lv_color_t color,
                           lv_opa_t opa, bool cap_start, bool cap_end,
                           bool open_start, bool open_end) {
    if (s_to - s_from < 0.01f) return;
    for (uint8_t k = 0; k < pd->n_prims; k++) {
        const pb_prim_t *p = &pd->prims[k];
        float len = p->s1 - p->s0;
        if (len <= 0.01f) continue;
        float a = (s_from > p->s0) ? s_from : p->s0;
        float b = (s_to   < p->s1) ? s_to   : p->s1;
        if (b - a < 0.01f) continue;
        /* Only the piece carrying the span's start takes the start cap, and
         * only the piece carrying its end takes the end cap. Comparing against
         * the PIECE's own end instead of the span's made every piece but the
         * last one think it ended the span. */
        bool first = (a <= s_from + 0.01f);
        bool last  = (b >= s_to   - 0.01f);
        /* extend into the neighbour so a tangent handover cannot leave a gap */
        if (!first) a -= 1.0f;
        if (!last)  b += 1.0f;
        /* How far a straight run reaches into its neighbour depends on what
         * that neighbour IS. Two straight legs meeting at a corner leave a
         * wedge half a band deep on the outside, so the run has to reach that
         * far. An arc it is TANGENT to leaves no wedge at all — and reaching
         * half a band straight back over a curve paints a flat edge above the
         * arc's curved one, which was the step visible at the J-hook's
         * shoulder. A tangent handover needs only the rounding slop. */
        const float slop = 1.5f;
        float ext0 = (k > 0 && pd->prims[k - 1].kind == PB_PRIM_LINE)
                   ? _pb_mitre(pd, k - 1, k) : slop;
        float ext1 = (k + 1 < pd->n_prims && pd->prims[k + 1].kind == PB_PRIM_LINE)
                   ? _pb_mitre(pd, k, k + 1) : slop;
        _pb_draw_prim(ctx, pd, p, (a - p->s0) / len, (b - p->s0) / len, color, opa,
                      first ? cap_start : false, last ? cap_end : false,
                      first ? open_start : true, last ? open_end : true,
                      ext0, ext1);
    }
}

/* NOTE on fades. The panel is 16-bit, so a long ramp between two colours has
 * only about a dozen distinct values available to it — 6 bits of green, 5 of
 * red and blue. `fade_fill` therefore bands, and that banding belongs to the
 * FRAMEBUFFER, not to how finely the fill is sliced.
 *
 * Ordered dithering between the two nearest colours was tried and taken out
 * again: on a straight run it works, but on a curve the slices are radial
 * wedges and the dither reads as streaks out of the centre — worse than the
 * banding it removes. The look that actually reads clean on this panel is a
 * solid fill with a bright lead edge, which costs one draw and no gradient. */

static void _pathbar_paint_prims(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                                 lv_opa_t master, lv_color_t dim_solid) {
    const float total = pd->total_len;
    if (total <= 0.0f) return;

    _pb_paint_span(ctx, pd, 0.0f, total, dim_solid, master,
                   pd->rounded, pd->rounded, false, false);

    const float fill = pd->cur_frac * total;
    if (fill <= 0.0f) return;

    lv_color_t bright = pd->lit_color;
    lv_color_t dim    = lv_color_mix(bright, lv_color_black(), 189);
    float rl = (pd->redline < pd->val_max && pd->val_max > pd->val_min)
             ? (pd->redline - pd->val_min) / (pd->val_max - pd->val_min) : 1.0f;
    const float rlpx = rl * total;
    const float lit_end = (fill < rlpx) ? fill : rlpx;

    if (lit_end > 0.01f) {
        if (pd->fade_fill) {
            /* A single pass cannot express a ramp, so the fade is sliced — but
             * finely enough (about every 4 px) that no step is visible, rather
             * than into a handful of bands that read as facets. Each slice
             * overlaps its neighbour, so the cuts cannot crack either. The
             * slices are small and this is opt-in; the ordinary fill is still
             * one pass per piece. */
            int N = (int)(lit_end / 4.0f);
            if (N < 8)   N = 8;
            if (N > 200) N = 200;
            float fadeEnd = PATHBAR_FADE_FULL * total;
            for (int q = 0; q < N; q++) {
                float a = lit_end * (float)q / (float)N;
                float b = lit_end * (float)(q + 1) / (float)N;
                if (q > 0) a -= 1.5f;
                float t = ((a + b) * 0.5f) / fadeEnd;
                if (t > 1.0f) t = 1.0f;
                _pb_paint_span(ctx, pd, a, b,
                               lv_color_mix(bright, dim, (uint8_t)(t * 255.0f)),
                               master, (q == 0) ? pd->rounded : false,
                               (q == N - 1) ? ((fill <= rlpx + 0.01f) ? pd->rounded : false)
                                            : false,
                               q > 0, q < N - 1);
            }
        } else {
            _pb_paint_span(ctx, pd, 0.0f, lit_end, bright, master, pd->rounded,
                           (fill <= rlpx + 0.01f) ? pd->rounded : false,
                           false, fill > rlpx + 0.01f);
        }
    }

    if (fill > rlpx + 0.01f) {
        _pb_paint_span(ctx, pd, rlpx, fill, pd->redline_color, master,
                       (rlpx <= 0.01f) ? pd->rounded : false, pd->rounded,
                       rlpx > 0.01f, false);
    }
}

/* ── Anti-aliased stroke renderer ───────────────────────────────────────────
 * LVGL's triangle rasteriser takes INTEGER vertices (lv_point_from_precise
 * truncates, and LV_USE_FLOAT is 0 here anyway), so filling a curve as a quad
 * strip snaps every edge vertex to a whole pixel: the outline stair-steps, and
 * because neighbouring quads round independently their anti-aliased edges don't
 * line up. That is the "jagged angles" a curved bar shows.
 *
 * Stroking the CENTRELINE as a polyline of width band_width hands both edges to
 * LVGL's anti-aliased line mask instead, and a round cap at every vertex fills
 * the joins, so the curve reads smooth with no seams to bury. Chords are
 * decimated to about one band width: on the tightest arc this widget is asked
 * to draw that is a sagitta well under a tenth of a pixel.
 *
 * Only for ROUNDED bars. A flat-ended bar wants the mitered quad strip, which is
 * also the right renderer for the straight/L/45 shapes that have real corners to
 * keep square. */
static void _pb_stroke_run(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                           lv_color_t c0, lv_color_t c1, lv_opa_t opa,
                           float s0, float s1, bool cap0, bool cap1) {
    if (s1 - s0 <= 0.01f || pd->total_len <= 0.0f) return;

    /* Chord length along the path. The round join at each vertex bulges out to
     * the true offset curve while the chord between two vertices cuts inside
     * it, so too long a step scallops the outer edge: the error is
     * step^2/(8R). Tied to the band width it is invisible at any normal width
     * (26 px on a 300 px radius is 0.3 px) and only shows on a very fat band,
     * so it is capped rather than made proportional — the cap costs nothing on
     * a thin bar and is what keeps a fat one round. */
    float step = (float)pd->band_width;
    if (step < 8.0f)  step = 8.0f;
    if (step > 28.0f) step = 28.0f;
    int n = (int)((s1 - s0) / step) + 2;
    if (n < 2) n = 2;
    /* Geometry is happy with few segments on a short run; a COLOUR RAMP is
     * not — each segment is one flat shade, so a short faded bar came out as
     * half a dozen visible bands. Only subdivide further when there is
     * actually a ramp to draw. */
    if (_color_to_u32(c0) != _color_to_u32(c1) && n < 26) n = 26;
    if (n > PATHBAR_MAX_POINTS) n = PATHBAR_MAX_POINTS;

    static EXT_RAM_BSS_ATTR lv_point_t sbuf[PATHBAR_MAX_POINTS];
    for (int i = 0; i < n; i++) {
        float s = s0 + (s1 - s0) * (float)i / (float)(n - 1);
        float x, y;
        if (!_pathbar_point_at(pd, s, &x, &y)) return;
        sbuf[i].x = (lv_coord_t)lroundf(x);
        sbuf[i].y = (lv_coord_t)lroundf(y);
    }

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.opa   = opa;
    ld.width = pd->band_width;

    /* A segment at a time, not one polyline: the joins must stay round even on
     * a square-ended bar (the round cap IS the join), and a polyline call can
     * only say one thing about both of its ends. Same cost — lv_draw_line
     * walks a polyline segment by segment anyway — minus the malloc it does
     * to copy the point array on every draw. */
    for (int i = 0; i + 1 < n; i++) {
        /* The colour walks the ramp segment by segment, so neighbours differ
         * by a fraction of a step and the round joins between them are
         * invisible — which is the whole reason the fade is not drawn as
         * separate passes any more. */
        uint8_t m = (n <= 2) ? 255 : (uint8_t)((255 * i) / (n - 2));
        ld.color = lv_color_mix(c1, c0, m);
        ld.round_start = (i == 0)       ? cap0 : true;
        ld.round_end   = (i + 2 == n)   ? cap1 : true;
        lv_draw_line(ctx, &ld, &sbuf[i], &sbuf[i + 1]);
    }
}

/* Dim track in one stroke, then the lit fill in as few colour runs as the
 * redline break and the optional fade need. */
static void _pathbar_stroke_band(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                                 lv_opa_t master, lv_color_t dim_solid) {
    const float total = pd->total_len;
    if (total <= 0.0f) return;

    _pb_stroke_run(ctx, pd, dim_solid, dim_solid, master, 0.0f, total,
                   pd->rounded, pd->rounded);   /* the unlit track, end to end */

    const float fill = pd->cur_frac * total;
    if (fill <= 0.0f) return;

    lv_color_t bright = pd->lit_color;
    lv_color_t dim    = lv_color_mix(bright, lv_color_black(), 189);
    float rl = (pd->redline < pd->val_max && pd->val_max > pd->val_min)
             ? (pd->redline - pd->val_min) / (pd->val_max - pd->val_min) : 1.0f;
    const float rlpx    = rl * total;
    const float fadeEnd = PATHBAR_FADE_FULL * total;

    /* Where a pass has to hand over mid-curve it starts a little early, so the
     * pass drawn after it covers the wedge two butted rectangles leave on the
     * outside of a bend. */
    float ov = (float)pd->band_width * 0.35f;
    if (ov < 4.0f) ov = 4.0f;

    float lit_end = (fill < rlpx) ? fill : rlpx;   /* in-range part of the fill */

    if (lit_end > 0.01f) {
        float ramp_end = (pd->fade_fill && fadeEnd < lit_end) ? fadeEnd : lit_end;
        lv_color_t c0 = pd->fade_fill ? dim : bright;
        lv_color_t c1 = bright;
        if (pd->fade_fill && ramp_end < fadeEnd) {
            /* a short fill only travels part of the way up the ramp */
            float t = ramp_end / fadeEnd;
            c1 = lv_color_mix(bright, dim, (uint8_t)(t * 255.0f));
        }
        bool ramp_is_end = (ramp_end >= lit_end - 0.01f);
        _pb_stroke_run(ctx, pd, c0, c1, master, 0.0f, ramp_end,
                       pd->rounded,
                       ramp_is_end ? ((fill <= rlpx + 0.01f) ? pd->rounded : false)
                                   : false);
        if (!ramp_is_end) {
            /* past the fade: solid bright. Same colour both sides of the
             * handover, so the join cannot show. */
            _pb_stroke_run(ctx, pd, bright, bright, master,
                           ramp_end - ov, lit_end, false,
                           (fill <= rlpx + 0.01f) ? pd->rounded : false);
        }
    }

    if (fill > rlpx + 0.01f) {
        /* The redline's edge is a deliberate hard one — the only boundary in
         * the fill that is supposed to be visible. */
        _pb_stroke_run(ctx, pd, pd->redline_color, pd->redline_color, master,
                       (rlpx > ov ? rlpx - ov : 0.0f), fill,
                       (rlpx <= 0.01f) ? pd->rounded : false, pd->rounded);
    }
}

static void _pathbar_draw_band(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                               lv_opa_t master, lv_color_t dim_solid) {
    const int n = pd->n_pts;
    if (n < 2 || n > PATHBAR_MAX_POINTS) return;
    if (pd->field) {               /* every shape: per-pixel coverage */
        _pathbar_draw_field(ctx, pd, master, dim_solid);
        return;
    }
    /* Only reached if the field could not be allocated. */
    if (pd->n_prims > 0) {
        _pathbar_paint_prims(ctx, pd, master, dim_solid);
        return;
    }
    _pathbar_stroke_band(ctx, pd, master, dim_solid);
    return;
    const float hw = (float)pd->band_width * 0.5f;

    /* Per-vertex mitered offset points (left/right of the centreline). Endpoints
     * use the plain segment normal → the terminal edge is perpendicular to the
     * path = a flat butt cut. Interior vertices use the angle bisector with the
     * miter length that preserves perpendicular half-width, clamped so a sharp
     * bend can't throw a long spike. */
    static EXT_RAM_BSS_ATTR lv_point_t L[PATHBAR_MAX_POINTS], R[PATHBAR_MAX_POINTS];
    for (int i = 0; i < n; i++) {
        float inx = 0, iny = 0, onx = 0, ony = 0;       /* left-normals of in/out segs */
        if (i > 0) {
            float dx = pd->pts[i].x - pd->pts[i-1].x, dy = pd->pts[i].y - pd->pts[i-1].y;
            float l = sqrtf(dx*dx + dy*dy); if (l <= 0.0f) l = 1.0f;
            inx = -dy/l; iny = dx/l;
        }
        if (i < n-1) {
            float dx = pd->pts[i+1].x - pd->pts[i].x, dy = pd->pts[i+1].y - pd->pts[i].y;
            float l = sqrtf(dx*dx + dy*dy); if (l <= 0.0f) l = 1.0f;
            onx = -dy/l; ony = dx/l;
        }
        float mx, my, mlen;
        if (i == 0)            { mx = onx; my = ony; mlen = hw; }       /* flat start cut */
        else if (i == n-1)     { mx = inx; my = iny; mlen = hw; }       /* flat end cut   */
        else {
            mx = inx + onx; my = iny + ony;
            float ml = sqrtf(mx*mx + my*my);
            /* Near-exact 180° fold (a U-turn) has no defined bisector — fall back
             * to the incoming normal. A true fold makes the band overlap itself
             * (a degenerate shape for a gauge bar); custom paths shouldn't author
             * one. Real turns, even 90°+, take the bisector branch below. */
            if (ml < 0.001f) { mx = inx; my = iny; mlen = hw; }
            else {
                mx /= ml; my /= ml;
                float c = mx*onx + my*ony;                 /* cos(half-turn) */
                if (c < 0.05f) c = 0.05f;                  /* avoid blow-up before clamp */
                mlen = hw / c;
                if (mlen > hw * PB_MITER_MAX) mlen = hw * PB_MITER_MAX;
            }
        }
        L[i].x = (lv_coord_t)lroundf(pd->pts[i].x + mx*mlen);
        L[i].y = (lv_coord_t)lroundf(pd->pts[i].y + my*mlen);
        R[i].x = (lv_coord_t)lroundf(pd->pts[i].x - mx*mlen);
        R[i].y = (lv_coord_t)lroundf(pd->pts[i].y - my*mlen);
    }

    const float total = pd->total_len;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = master;
    /* Sharp hand-drawn polylines get the perpendicular-rect renderer; smooth and
     * parametric shapes keep the continuous mitered strip (see the header). */
    bool sharp = (pd->shape == 0 && !pd->smooth);
    float ov = (master < LV_OPA_COVER) ? 0.0f : PB_SEAM_OVERLAP;

    /* Dim full-path track. */
    rd.bg_color = dim_solid;
    if (sharp) {
        for (int i = 0; i + 1 < n; i++) {
            float dx = pd->pts[i+1].x - pd->pts[i].x, dy = pd->pts[i+1].y - pd->pts[i].y;
            float l = sqrtf(dx*dx + dy*dy); if (l <= 0.0f) l = 1.0f;
            _pb_seg_quad(ctx, &rd, pd->pts[i].x, pd->pts[i].y,
                         pd->pts[i+1].x, pd->pts[i+1].y, -dy/l, dx/l, hw);
        }
        for (int v = 1; v + 1 < n; v++) _pb_join_fan(ctx, &rd, pd, L, R, v, hw);
    } else {
        for (int i = 1; i < n; i++) {
            lv_point_t el = L[i], er = R[i];
            if (i < n - 1) _spk_edge_at(L, R, pd->cum, n, pd->cum[i] + ov, &el, &er);
            _pb_fill_quad(ctx, &rd, L[i-1], R[i-1], er, el);
        }
    }
    if (pd->rounded) {     /* round the track's two ends (base, far end) */
        _spk_round_cap(ctx, &rd, pd->pts[0].x, pd->pts[0].y,
                       (float)(pd->pts[0].x - pd->pts[1].x),
                       (float)(pd->pts[0].y - pd->pts[1].y), hw);
        _spk_round_cap(ctx, &rd, pd->pts[n-1].x, pd->pts[n-1].y,
                       (float)(pd->pts[n-1].x - pd->pts[n-2].x),
                       (float)(pd->pts[n-1].y - pd->pts[n-2].y), hw);
    }

    /* Lit fill: per-segment perpendicular rects + corner joins, subdivided only
     * at the redline break and (when fading) in steps inside the fade zone. */
    float f = pd->cur_frac;
    if (f <= 0.0f || total <= 0.0f) return;
    float fill = f * total;
    lv_color_t bright = pd->lit_color;
    /* ~74% of bright — subtle ramp; see widget_arc.c _arc_fade_draw_cb. */
    lv_color_t dim    = lv_color_mix(bright, lv_color_black(), 189);
    float rl = (pd->redline < pd->val_max && pd->val_max > pd->val_min)
             ? (pd->redline - pd->val_min) / (pd->val_max - pd->val_min) : 1.0f;
    float rlpx = rl * total;                          /* exact redline arc-length */
    float fadeEnd = PATHBAR_FADE_FULL * total;

    if (sharp) {
        for (int i = 0; i + 1 < n; i++) {
            float c0 = pd->cum[i], c1 = pd->cum[i+1];
            if (c0 >= fill) break;
            float hi = c1 < fill ? c1 : fill;             /* lit end on this segment */
            float seglen = c1 - c0; if (seglen <= 0.0f) seglen = 1.0f;
            float dx = pd->pts[i+1].x - pd->pts[i].x, dy = pd->pts[i+1].y - pd->pts[i].y;
            float l = sqrtf(dx*dx + dy*dy); if (l <= 0.0f) l = 1.0f;
            float nx = -dy/l, ny = dx/l;
            float a = c0;
            while (a < hi - 0.01f) {
                float b = hi;
                if (a < rlpx - 0.01f && b > rlpx) b = rlpx;          /* land on the redline */
                if (pd->fade_fill && a < fadeEnd && b - a > PB_GRAD_STEP)
                    b = a + PB_GRAD_STEP;                            /* gradient step in fade */
                float pc = ((a + b) * 0.5f) / total;
                rd.bg_color = _pb_lit_color(pd, pc, rl, bright, dim);
                float fa = (a - c0) / seglen, fb = (b - c0) / seglen;
                _pb_seg_quad(ctx, &rd,
                             pd->pts[i].x + dx*fa, pd->pts[i].y + dy*fa,
                             pd->pts[i].x + dx*fb, pd->pts[i].y + dy*fb, nx, ny, hw);
                a = b;
            }
            if (hi >= c1 - 0.01f && i + 2 < n) {    /* front cleared the vertex: light its join */
                rd.bg_color = _pb_lit_color(pd, c1 / total, rl, bright, dim);
                _pb_join_fan(ctx, &rd, pd, L, R, i+1, hw);
            }
        }
    } else {
        /* Smooth/parametric: continuous mitered strip in PB_GRAD_STEP steps, each
         * quad poking ov past the next so the opaque later quad buries the seam. */
        float s0 = 0.0f;
        lv_point_t e0l = L[0], e0r = R[0];
        while (s0 < fill - 0.01f) {
            float s1 = s0 + PB_GRAD_STEP;
            bool last = false;
            if (s1 >= fill) { s1 = fill; last = true; }
            if (s0 < rlpx - 0.01f && s1 > rlpx) { s1 = rlpx; last = false; }
            lv_point_t e1l, e1r;
            _spk_edge_at(L, R, pd->cum, n, s1, &e1l, &e1r);
            lv_point_t qel = e1l, qer = e1r;
            if (!last) _spk_edge_at(L, R, pd->cum, n, s1 + ov, &qel, &qer);
            float pc = ((s0 + s1) * 0.5f) / total;
            rd.bg_color = _pb_lit_color(pd, pc, rl, bright, dim);
            _pb_fill_quad(ctx, &rd, e0l, e0r, qer, qel);
            e0l = e1l; e0r = e1r;
            s0  = s1;
        }
    }

    if (pd->rounded) {
        /* lit base cap — gradient colour at fraction 0 */
        rd.bg_color = pd->fade_fill ? dim : bright;
        _spk_round_cap(ctx, &rd, pd->pts[0].x, pd->pts[0].y,
                       (float)(pd->pts[0].x - pd->pts[1].x),
                       (float)(pd->pts[0].y - pd->pts[1].y), hw);
        /* lit tip cap — colour at the fill front, bulging forward */
        float mx, my, mnx, mny;
        if (_pathbar_sample(pd, fill, &mx, &my, &mnx, &mny)) {
            float pcf = fill / total;
            lv_color_t tc;
            if (pcf > rl) tc = pd->redline_color;
            else if (pd->fade_fill) {
                float t = pcf / PATHBAR_FADE_FULL; if (t > 1.0f) t = 1.0f;
                tc = lv_color_mix(bright, dim, (uint8_t)(t * 255.0f));
            } else tc = bright;
            rd.bg_color = tc;
            _spk_round_cap(ctx, &rd, mx, my, mny, -mnx, hw);   /* forward tangent = (ny,-nx) */
        }
    }
}

/* ── Draw callback ──────────────────────────────────────────────────────── */
static void _pathbar_draw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_END) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    if (!pd || pd->n_pts < 2) return;

    lv_opa_t master = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    if (master <= LV_OPA_MIN) return;

    /* Dim track colour: dim_color pre-blended over black at dim_opa, drawn opaque
     * (dim_opa is the authoring knob for how dark the track reads). Opaque fills,
     * so the only overlap is the tiny corner-join wedge — invisible at full opa,
     * a faint corner brighten only under a partial master opa (brief boot fade). */
    lv_color_t dim_solid = lv_color_mix(pd->dim_color, lv_color_black(), pd->dim_opa);
    float f = pd->cur_frac;
    _pathbar_draw_band(ctx, pd, master, dim_solid);

    if (f > 0.0f) {
        /* Bright "current value" marker at the fill tip. Drawn as a FLAT line
         * straight across the band (perpendicular to the path) so it never
         * rounds on a bend the way a band slice did — and it's configurable. */
        if (pd->lead_edge_enabled && pd->lead_edge_width > 0) {
            float mx, my, mnx, mny;
            if (_pathbar_sample(pd, f * pd->total_len, &mx, &my, &mnx, &mny)) {
                float hw = pd->band_width * 0.5f;
                _pb_draw_mark(ctx, mx - mnx * hw, my - mny * hw, mx + mnx * hw, my + mny * hw,
                              (float)pd->lead_edge_width, false,   /* flat ends */
                              pd->lead_edge_color, master);
            }
        }
    }

    /* Tick + number scale on top (static — drawn regardless of fill level). */
    _pathbar_draw_scale(ctx, pd);
}

/* ── Smoothing timer ────────────────────────────────────────────────────── */
static void _pathbar_anim_cb(lv_timer_t *t) {
    widget_t *w = (widget_t *)t->user_data;
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (!pd) { lv_timer_pause(t); return; }
    float old = pd->cur_frac;
    float d = pd->target_frac - pd->cur_frac;
    float k = pd->smoothing_ms ? (float)PATHBAR_ANIM_MS / (float)pd->smoothing_ms : 1.0f;
    if (k > 1.0f) k = 1.0f;
    pd->cur_frac += d * k;
    if (fabsf(pd->target_frac - pd->cur_frac) < 0.002f) {
        pd->cur_frac = pd->target_frac;
        lv_timer_pause(t);
    }
    _pathbar_invalidate_range(w, old, pd->cur_frac);
}

static void _pathbar_ensure_anim(widget_t *w) {
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (!pd->anim_timer) {
        pd->anim_timer = lv_timer_create(_pathbar_anim_cb, PATHBAR_ANIM_MS, w);
        if (pd->anim_timer) lv_timer_pause(pd->anim_timer);
    }
}

/* ── Signal callback ────────────────────────────────────────────────────── */
static void _pathbar_on_signal(float value, bool is_stale, void *user_data) {
    widget_t *w = (widget_t *)user_data;
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (!pd) return;
    pd->is_stale = is_stale;

    float f = 0.0f;
    if (!is_stale && pd->val_max > pd->val_min) {
        f = (value - pd->val_min) / (pd->val_max - pd->val_min);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
    }
    float old = pd->cur_frac;
    pd->target_frac = f;

    if (pd->smoothing_ms == 0 || is_stale) {
        pd->cur_frac = f;
        _pathbar_invalidate_range(w, old, f);
    } else if (fabsf(f - pd->cur_frac) > PATHBAR_SNAP_FRAC) {
        /* Big jump: snap instantly instead of easing (speed over smoothness). */
        pd->cur_frac = f;
        if (pd->anim_timer) lv_timer_pause(pd->anim_timer);
        _pathbar_invalidate_range(w, old, f);
    } else {
        _pathbar_ensure_anim(w);
        if (pd->anim_timer) lv_timer_resume(pd->anim_timer);
    }
}

/* ── vtable: create ─────────────────────────────────────────────────────── */
static void _pathbar_create(widget_t *w, lv_obj_t *parent) {
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (!pd) return;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w->w, w->h);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_pos(obj, w->x, w->y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(obj, _pathbar_draw_cb, LV_EVENT_DRAW_MAIN_END, w);
    w->root = obj;

    if (pd->signal_index >= 0)
        signal_subscribe(pd->signal_index, _pathbar_on_signal, w);
}

/* ── vtable: resize ─────────────────────────────────────────────────────── */
static void _pathbar_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    w->w = nw;
    w->h = nh;
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (pd && pd->shape != 0) {       /* refit a parametric shape to the new box */
        lv_coord_t bx = SCREEN_ORIGIN_X + w->x - (lv_coord_t)(nw / 2);
        lv_coord_t by = SCREEN_ORIGIN_Y + w->y - (lv_coord_t)(nh / 2);
        _pathbar_gen_shape(pd, bx, by, nw, nh);
        if (w->root && lv_obj_is_valid(w->root)) lv_obj_invalidate(w->root);
    }
}

static void _pathbar_open_settings(widget_t *w) { (void)w; }

/* ── vtable: to_json (defaults-only for scalars; path always emitted) ───── */
static void _pathbar_to_json(widget_t *w, cJSON *out) {
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    widget_base_to_json(w, out);
    if (!pd) return;
    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;

    if (pd->signal_name[0])
        cJSON_AddStringToObject(cfg, "signal_name", pd->signal_name);
    if (pd->val_min != DEF_MIN) cJSON_AddNumberToObject(cfg, "min", pd->val_min);
    if (pd->val_max != DEF_MAX) cJSON_AddNumberToObject(cfg, "max", pd->val_max);
    if (pd->redline < pd->val_max)
        cJSON_AddNumberToObject(cfg, "redline", pd->redline);
    if (pd->band_width != DEF_BAND_WIDTH)
        cJSON_AddNumberToObject(cfg, "band_width", pd->band_width);
    if (pd->shape != 0) {
        cJSON_AddNumberToObject(cfg, "shape", pd->shape);
        if (pd->orientation != 0) cJSON_AddNumberToObject(cfg, "orientation", pd->orientation);
        if (pd->shape == 4) {
            cJSON_AddNumberToObject(cfg, "hook_angle", pd->hook_angle);
            if (pd->hook_radius > 0)
                cJSON_AddNumberToObject(cfg, "hook_radius", pd->hook_radius);
        }
        else                cJSON_AddNumberToObject(cfg, "corner_radius", pd->corner_radius);
    }
    if (!pd->rounded) cJSON_AddBoolToObject(cfg, "rounded", false);
    if (pd->fade_fill) cJSON_AddBoolToObject(cfg, "fade_fill", true);
    if (!pd->lead_edge_enabled) cJSON_AddBoolToObject(cfg, "lead_edge_enabled", false);
    if (pd->lead_edge_width != DEF_LEAD_WIDTH) cJSON_AddNumberToObject(cfg, "lead_edge_width", pd->lead_edge_width);
    if (pd->lead_edge_color.full != lv_color_hex(DEF_LEAD_COLOR).full)
        cJSON_AddNumberToObject(cfg, "lead_edge_color", _color_to_u32(pd->lead_edge_color));
    if (_color_to_u32(pd->dim_color) != _color_to_u32(_u32_to_color(DEF_DIM_COLOR)))
        cJSON_AddNumberToObject(cfg, "dim_color", _color_to_u32(pd->dim_color));
    if (_color_to_u32(pd->lit_color) != _color_to_u32(_u32_to_color(DEF_LIT_COLOR)))
        cJSON_AddNumberToObject(cfg, "lit_color", _color_to_u32(pd->lit_color));
    if (_color_to_u32(pd->redline_color) != _color_to_u32(_u32_to_color(DEF_RED_COLOR)))
        cJSON_AddNumberToObject(cfg, "redline_color", _color_to_u32(pd->redline_color));
    if (pd->dim_opa != DEF_DIM_OPA)
        cJSON_AddNumberToObject(cfg, "dim_opa", pd->dim_opa);
    if (pd->smoothing_ms != 20)
        cJSON_AddNumberToObject(cfg, "smoothing_ms", pd->smoothing_ms);
    if (pd->smooth) cJSON_AddBoolToObject(cfg, "smooth", true);

    /* Value scale (defaults-only). Only meaningful when show_ticks is on. */
    if (pd->show_ticks) {
        cJSON_AddBoolToObject(cfg, "show_ticks", true);
        if (pd->minor_tick_step != 0.0f) cJSON_AddNumberToObject(cfg, "minor_tick_step", pd->minor_tick_step);
        if (pd->major_tick_step != 0.0f) cJSON_AddNumberToObject(cfg, "major_tick_step", pd->major_tick_step);
        if (pd->show_labels) cJSON_AddBoolToObject(cfg, "show_labels", true);
        if (pd->tick_label_divisor != 1) cJSON_AddNumberToObject(cfg, "tick_label_divisor", pd->tick_label_divisor);
        if (pd->tick_len != 10) cJSON_AddNumberToObject(cfg, "tick_len", pd->tick_len);
        if (pd->major_tick_len != 16) cJSON_AddNumberToObject(cfg, "major_tick_len", pd->major_tick_len);
        if (pd->tick_width != 2) cJSON_AddNumberToObject(cfg, "tick_width", pd->tick_width);
        if (pd->major_tick_width != 3) cJSON_AddNumberToObject(cfg, "major_tick_width", pd->major_tick_width);
        if (_color_to_u32(pd->tick_color) != DEF_TICK_COLOR)
            cJSON_AddNumberToObject(cfg, "tick_color", _color_to_u32(pd->tick_color));
        if (_color_to_u32(pd->major_tick_color) != DEF_MAJ_TICK_COLOR)
            cJSON_AddNumberToObject(cfg, "major_tick_color", _color_to_u32(pd->major_tick_color));
        if (_color_to_u32(pd->label_color) != DEF_LABEL_COLOR)
            cJSON_AddNumberToObject(cfg, "label_color", _color_to_u32(pd->label_color));
        if (pd->label_gap != 14) cJSON_AddNumberToObject(cfg, "label_gap", pd->label_gap);
        if (pd->label_side != 0) cJSON_AddNumberToObject(cfg, "label_side", pd->label_side);
        if (pd->label_along_offset != 0) cJSON_AddNumberToObject(cfg, "label_along_offset", pd->label_along_offset);
        if (pd->tick_slant) cJSON_AddNumberToObject(cfg, "tick_slant", pd->tick_slant);
        if (!pd->redline_recolor_ticks) cJSON_AddBoolToObject(cfg, "redline_recolor_ticks", false);
        if (pd->label_font[0]) cJSON_AddStringToObject(cfg, "label_font", pd->label_font);
    }

    /* Only custom shapes carry an explicit path; parametric shapes regenerate.
     * When smooth, write the FEW authored anchors (not the dense tessellation) so
     * editing round-trips to the same handful of dots. */
    if (pd->shape == 0) {
        const lv_point_t *src = (pd->smooth && pd->anchors && pd->n_anchors > 0)
                              ? pd->anchors : pd->pts;
        uint16_t cnt = (pd->smooth && pd->anchors && pd->n_anchors > 0)
                     ? pd->n_anchors : pd->n_pts;
        if (src && cnt > 0) {
            cJSON *path = cJSON_AddArrayToObject(cfg, "path");
            for (uint16_t i = 0; i < cnt; i++) {
                cJSON_AddItemToArray(path, cJSON_CreateNumber(src[i].x));
                cJSON_AddItemToArray(path, cJSON_CreateNumber(src[i].y));
            }
        }
    }
}

/* ── vtable: from_json ──────────────────────────────────────────────────── */
static void _pathbar_from_json(widget_t *w, cJSON *in) {
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!pd) return;
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;
    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(pd->signal_name, item->valuestring, sizeof(pd->signal_name));
    item = cJSON_GetObjectItemCaseSensitive(cfg, "min");
    if (cJSON_IsNumber(item)) pd->val_min = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "max");
    if (cJSON_IsNumber(item)) pd->val_max = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline");
    if (cJSON_IsNumber(item)) pd->redline = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "band_width");
    if (cJSON_IsNumber(item)) pd->band_width = (uint8_t)LV_CLAMP(1, item->valueint, 80);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "shape");
    if (cJSON_IsNumber(item)) pd->shape = (uint8_t)LV_CLAMP(0, item->valueint, 4);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "orientation");
    if (cJSON_IsNumber(item)) pd->orientation = (uint8_t)LV_CLAMP(0, item->valueint, 3);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "corner_radius");
    if (cJSON_IsNumber(item)) pd->corner_radius = (uint16_t)LV_CLAMP(0, item->valueint, 1000);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "hook_angle");
    if (cJSON_IsNumber(item)) pd->hook_angle = (uint16_t)LV_CLAMP(30, item->valueint, 200);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "hook_radius");
    if (cJSON_IsNumber(item)) pd->hook_radius = (uint16_t)LV_CLAMP(0, item->valueint, 400);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "rounded");
    if (cJSON_IsBool(item)) pd->rounded = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "fade_fill");
    if (cJSON_IsBool(item)) pd->fade_fill = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "smooth");
    if (cJSON_IsBool(item)) pd->smooth = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "lead_edge_enabled");
    if (cJSON_IsBool(item)) pd->lead_edge_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "lead_edge_width");
    if (cJSON_IsNumber(item)) pd->lead_edge_width = (uint8_t)LV_CLAMP(0, item->valueint, 40);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "lead_edge_color");
    if (cJSON_IsNumber(item)) pd->lead_edge_color = _u32_to_color((uint32_t)item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "dim_color");
    if (cJSON_IsNumber(item)) pd->dim_color = _u32_to_color((uint32_t)item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "lit_color");
    if (cJSON_IsNumber(item)) pd->lit_color = _u32_to_color((uint32_t)item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_color");
    if (cJSON_IsNumber(item)) pd->redline_color = _u32_to_color((uint32_t)item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "dim_opa");
    if (cJSON_IsNumber(item)) pd->dim_opa = (lv_opa_t)LV_CLAMP(0, item->valueint, 255);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "smoothing_ms");
    if (cJSON_IsNumber(item)) pd->smoothing_ms = (uint16_t)item->valueint;

    /* optional value scale (ticks + numbers along the path) */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "show_ticks");
    if (cJSON_IsBool(item)) pd->show_ticks = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "show_labels");
    if (cJSON_IsBool(item)) pd->show_labels = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_step");
    if (cJSON_IsNumber(item)) pd->minor_tick_step = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_step");
    if (cJSON_IsNumber(item)) pd->major_tick_step = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_divisor");
    if (cJSON_IsNumber(item)) pd->tick_label_divisor = (uint16_t)LV_CLAMP(1, item->valueint, 100000);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_len");
    if (cJSON_IsNumber(item)) pd->tick_len = (uint8_t)LV_CLAMP(0, item->valueint, 200);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_len");
    if (cJSON_IsNumber(item)) pd->major_tick_len = (uint8_t)LV_CLAMP(0, item->valueint, 200);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_width");
    if (cJSON_IsNumber(item)) pd->tick_width = (uint8_t)LV_CLAMP(0, item->valueint, 40);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_width");
    if (cJSON_IsNumber(item)) pd->major_tick_width = (uint8_t)LV_CLAMP(0, item->valueint, 40);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_color");
    if (cJSON_IsNumber(item)) pd->tick_color = _u32_to_color((uint32_t)item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_color");
    if (cJSON_IsNumber(item)) pd->major_tick_color = _u32_to_color((uint32_t)item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_color");
    if (cJSON_IsNumber(item)) pd->label_color = _u32_to_color((uint32_t)item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_gap");
    if (cJSON_IsNumber(item)) pd->label_gap = (int16_t)LV_CLAMP(-200, item->valueint, 200);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_slant");
    if (cJSON_IsNumber(item)) pd->tick_slant = (int8_t)LV_CLAMP(-80, item->valueint, 80);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_recolor_ticks");
    if (cJSON_IsBool(item)) pd->redline_recolor_ticks = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_font");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(pd->label_font, item->valuestring, sizeof(pd->label_font));
    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_side");
    if (cJSON_IsNumber(item)) pd->label_side = (int8_t)LV_CLAMP(0, item->valueint, 2);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_along_offset");
    if (cJSON_IsNumber(item)) pd->label_along_offset = (int16_t)LV_CLAMP(-400, item->valueint, 400);

    /* path: flat [x0,y0,x1,y1,...] of absolute screen-px points */
    pd->n_prims = 0;                /* an explicit path is not parametric */
    cJSON *path = cJSON_GetObjectItemCaseSensitive(cfg, "path");
    if (cJSON_IsArray(path)) {
        int len = cJSON_GetArraySize(path);
        int n = len / 2;
        if (n > PATHBAR_MAX_POINTS) n = PATHBAR_MAX_POINTS;
        if (n >= 2) {
            _pathbar_free_path(pd);
            pd->pts = heap_caps_calloc(n, sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
            pd->cum = heap_caps_calloc(n, sizeof(float), MALLOC_CAP_SPIRAM);
            if (!pd->pts || !pd->cum) { _pathbar_free_path(pd); }
            else {
                /* Drop consecutive duplicate points (zero-length segments): the
                 * band offset/miter math treats a zero-length leg as a degenerate
                 * normal and would throw an offset spike. The parametric generator
                 * already de-dups (PB_EMIT); the explicit loader must too. */
                int k = 0;
                for (int i = 0; i < n; i++) {
                    cJSON *px = cJSON_GetArrayItem(path, i * 2);
                    cJSON *py = cJSON_GetArrayItem(path, i * 2 + 1);
                    lv_coord_t X = (lv_coord_t)(cJSON_IsNumber(px) ? px->valuedouble : 0);
                    lv_coord_t Y = (lv_coord_t)(cJSON_IsNumber(py) ? py->valuedouble : 0);
                    if (k == 0 || X != pd->pts[k-1].x || Y != pd->pts[k-1].y) {
                        pd->pts[k].x = X; pd->pts[k].y = Y; k++;
                    }
                }
                if (k >= 2) { pd->n_pts = (uint16_t)k; _pathbar_build_cum(pd); }
                else        { _pathbar_free_path(pd); }
            }
        }
    }

    /* Parametric shape: generate the path to fit the widget box (overrides any
     * explicit `path`). widget_base_from_json already set w->x/y/w/h above. */
    if (pd->shape != 0) {
        lv_coord_t bx = SCREEN_ORIGIN_X + w->x - (lv_coord_t)(w->w / 2);
        lv_coord_t by = SCREEN_ORIGIN_Y + w->y - (lv_coord_t)(w->h / 2);
        _pathbar_gen_shape(pd, bx, by, (lv_coord_t)w->w, (lv_coord_t)w->h);
    }

    /* Smooth custom path: the points just loaded are the AUTHORED anchors — keep
     * them verbatim (for to_json round-trip) and replace pts with the Catmull-Rom
     * curve through them. Only for custom paths; parametric shapes are already
     * smooth. */
    _pathbar_free_anchors(pd);
    if (pd->shape == 0 && pd->smooth && pd->n_pts >= 2) {
        pd->anchors = heap_caps_calloc(pd->n_pts, sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
        if (pd->anchors) {
            pd->n_anchors = pd->n_pts;
            for (uint16_t i = 0; i < pd->n_pts; i++) pd->anchors[i] = pd->pts[i];
            _pathbar_smooth_from_anchors(pd);     /* anchors -> dense pts */
        }
    }

    /* A custom path draws from a per-pixel coverage field (see
     * _pathbar_build_field); a parametric shape built its own in
     * _pathbar_gen_shape. */
    if (pd->shape == 0) _pathbar_build_field(pd);

    if (pd->signal_name[0])
        pd->signal_index = signal_find_by_name(pd->signal_name);
}

/* ── vtable: destroy ────────────────────────────────────────────────────── */
static void _pathbar_destroy(widget_t *w) {
    if (!w) return;
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (pd) {
        if (pd->signal_index >= 0)
            signal_unsubscribe(pd->signal_index, _pathbar_on_signal, w);
        if (pd->anim_timer) { lv_timer_del(pd->anim_timer); pd->anim_timer = NULL; }
        _pathbar_free_path(pd);
        _pathbar_free_anchors(pd);
    }
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    /* Unsubscribe rule signal callbacks + free w->rules. pathbar has no
     * apply_overrides (rules are a no-op for it — the editor excludes it from
     * the Rules UI), but a legacy/hand-authored layout can still carry a
     * "rules" array that widget_rules_from_json allocated + subscribed; without
     * this the subscription dangles into freed memory and w->rules leaks. */
    widget_rules_free(w);
    if (w->type_data) free(w->type_data);
    free(w);
}

/* ── Factory ────────────────────────────────────────────────────────────── */
widget_t *widget_pathbar_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) { ESP_LOGE(TAG, "alloc widget_t failed"); return NULL; }

    pathbar_data_t *pd = heap_caps_calloc(1, sizeof(pathbar_data_t), MALLOC_CAP_SPIRAM);
    if (!pd) pd = calloc(1, sizeof(pathbar_data_t));
    if (!pd) { free(w); return NULL; }

    pd->signal_index  = -1;
    pd->val_min       = DEF_MIN;
    pd->val_max       = DEF_MAX;
    pd->redline       = DEF_MAX;     /* off by default */
    pd->band_width    = DEF_BAND_WIDTH;
    pd->rounded       = true;
    pd->shape         = 0;            /* custom (explicit path) */
    pd->orientation   = 0;
    pd->corner_radius = 40;
    pd->hook_angle    = 120;          /* J-hook arc sweep degrees */
    pd->hook_radius   = 0;            /* 0 = auto-fit the box height */
    pd->dim_color     = _u32_to_color(DEF_DIM_COLOR);
    pd->lit_color     = _u32_to_color(DEF_LIT_COLOR);
    pd->redline_color = _u32_to_color(DEF_RED_COLOR);
    pd->lead_edge_enabled = true;
    pd->lead_edge_color   = lv_color_hex(DEF_LEAD_COLOR);   /* 888->565 (NOT _u32_to_color, which expects raw 565) */
    pd->lead_edge_width   = DEF_LEAD_WIDTH;
    pd->dim_opa       = DEF_DIM_OPA;
    pd->smoothing_ms  = 20;   /* default: gentle 20 ms glide (snappy, no lag) */
    pd->show_ticks          = false;
    pd->show_labels         = false;
    pd->minor_tick_step     = 0.0f;
    pd->major_tick_step     = 0.0f;
    pd->tick_label_divisor  = 1;
    pd->tick_len            = 10;
    pd->major_tick_len      = 16;
    pd->tick_width          = 2;
    pd->major_tick_width    = 3;
    pd->tick_color          = _u32_to_color(DEF_TICK_COLOR);
    pd->major_tick_color    = _u32_to_color(DEF_MAJ_TICK_COLOR);
    pd->label_color         = _u32_to_color(DEF_LABEL_COLOR);
    pd->label_gap           = 14;
    pd->label_side          = 0;   /* auto: face the path centroid (legacy behaviour) */
    pd->label_along_offset  = 0;
    pd->tick_slant          = 0;
    pd->redline_recolor_ticks = true;
    pd->label_font[0]       = '\0';

    w->type      = WIDGET_PATHBAR;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = DEF_W;
    w->h         = DEF_H;
    w->type_data = pd;
    snprintf(w->id, sizeof(w->id), "pathbar_%u", slot);

    w->create        = _pathbar_create;
    w->resize        = _pathbar_resize;
    w->open_settings = _pathbar_open_settings;
    w->to_json       = _pathbar_to_json;
    w->from_json     = _pathbar_from_json;
    w->destroy       = _pathbar_destroy;

    ESP_LOGI(TAG, "Created pathbar instance slot=%u", slot);
    return w;
}
