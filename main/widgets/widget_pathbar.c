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
#include "signal.h"
#include "screen_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

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

static void _pathbar_free_path(pathbar_data_t *pd) {
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

/* Smooth custom path: tessellate a Catmull-Rom spline THROUGH pd->anchors into
 * the dense pd->pts so a few authored points produce a clean curve (instead of
 * the user hand-placing dozens of polyline points). Catmull-Rom interpolates
 * every anchor; the two end segments reflect the end anchor as the phantom
 * neighbour so the curve doesn't pull in. Sub-steps scale with chord length
 * (~6px) and the whole thing is capped at PATHBAR_MAX_POINTS. Replaces pts/cum;
 * pd->anchors is left intact (to_json round-trips the few points). */
static void _pathbar_smooth_from_anchors(pathbar_data_t *pd) {
    const int na = pd->n_anchors;
    if (!pd->anchors || na < 2) return;

    static lv_point_t buf[PATHBAR_MAX_POINTS];
    int n = 0;
    #define SM_EMIT(X, Y) do { \
        lv_coord_t _x = (lv_coord_t)lroundf(X), _y = (lv_coord_t)lroundf(Y); \
        if ((n == 0 || _x != buf[n-1].x || _y != buf[n-1].y) && n < PATHBAR_MAX_POINTS) { \
            buf[n].x = _x; buf[n].y = _y; n++; } } while (0)

    SM_EMIT(pd->anchors[0].x, pd->anchors[0].y);
    if (na == 2) {
        SM_EMIT(pd->anchors[1].x, pd->anchors[1].y);            /* a line */
    } else {
        /* CENTRIPETAL Catmull-Rom (alpha=0.5, Barry-Goldman form). Uniform CR
         * overshoots/cusps badly when anchor spacing is uneven (e.g. a long
         * straight run next to a tight curve) because a far neighbour blows up
         * the local tangent; centripetal parameterises by sqrt(distance) so it
         * never overshoots or self-intersects. End segments use a reflected
         * phantom point so the knots stay non-coincident. */
        for (int i = 0; i < na - 1; i++) {
            float p1x = pd->anchors[i].x,     p1y = pd->anchors[i].y;
            float p2x = pd->anchors[i+1].x,   p2y = pd->anchors[i+1].y;
            float p0x, p0y, p3x, p3y;
            if (i > 0)      { p0x = pd->anchors[i-1].x; p0y = pd->anchors[i-1].y; }
            else            { p0x = 2.0f*p1x - p2x;     p0y = 2.0f*p1y - p2y; }
            if (i+2 < na)   { p3x = pd->anchors[i+2].x; p3y = pd->anchors[i+2].y; }
            else            { p3x = 2.0f*p2x - p1x;     p3y = 2.0f*p2y - p1y; }
            float t0 = 0.0f;
            float t1 = t0 + sqrtf(sqrtf((p1x-p0x)*(p1x-p0x) + (p1y-p0y)*(p1y-p0y)));
            float t2 = t1 + sqrtf(sqrtf((p2x-p1x)*(p2x-p1x) + (p2y-p1y)*(p2y-p1y)));
            float t3 = t2 + sqrtf(sqrtf((p3x-p2x)*(p3x-p2x) + (p3y-p2y)*(p3y-p2y)));
            if (t1 <= t0) t1 = t0 + 1e-4f;
            if (t2 <= t1) t2 = t1 + 1e-4f;
            if (t3 <= t2) t3 = t2 + 1e-4f;
            float chord = sqrtf((p2x-p1x)*(p2x-p1x) + (p2y-p1y)*(p2y-p1y));
            int steps = (int)(chord / 6.0f);
            if (steps < 2)  steps = 2;
            if (steps > 48) steps = 48;
            for (int s = 1; s <= steps; s++) {
                float t  = t1 + (t2 - t1) * (float)s / (float)steps;
                float A1x = ((t1-t)*p0x + (t-t0)*p1x) / (t1-t0), A1y = ((t1-t)*p0y + (t-t0)*p1y) / (t1-t0);
                float A2x = ((t2-t)*p1x + (t-t1)*p2x) / (t2-t1), A2y = ((t2-t)*p1y + (t-t1)*p2y) / (t2-t1);
                float A3x = ((t3-t)*p2x + (t-t2)*p3x) / (t3-t2), A3y = ((t3-t)*p2y + (t-t2)*p3y) / (t3-t2);
                float B1x = ((t2-t)*A1x + (t-t0)*A2x) / (t2-t0), B1y = ((t2-t)*A1y + (t-t0)*A2y) / (t2-t0);
                float B2x = ((t3-t)*A2x + (t-t1)*A3x) / (t3-t1), B2y = ((t3-t)*A2y + (t-t1)*A3y) / (t3-t1);
                float Cx  = ((t2-t)*B1x + (t-t1)*B2x) / (t2-t1), Cy  = ((t2-t)*B1y + (t-t1)*B2y) / (t2-t1);
                SM_EMIT(Cx, Cy);
            }
        }
    }
    #undef SM_EMIT

    _pathbar_free_path(pd);
    if (n < 2) return;
    pd->pts = heap_caps_calloc(n, sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    pd->cum = heap_caps_calloc(n, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!pd->pts || !pd->cum) { _pathbar_free_path(pd); return; }
    for (int i = 0; i < n; i++) pd->pts[i] = buf[i];
    pd->n_pts = (uint16_t)n;
    _pathbar_build_cum(pd);
}

/* Generate the path points for a parametric shape, fit to the widget box (so
 * the editor only needs a shape + radius, no point array). box is absolute
 * screen px. Replaces pd->pts/cum. */
static void _pathbar_gen_shape(pathbar_data_t *pd, lv_coord_t bx, lv_coord_t by,
                               lv_coord_t bw, lv_coord_t bh) {
    lv_point_t buf[PATHBAR_MAX_POINTS];
    int n = 0;
    #define PB_EMIT(X, Y) do { \
        lv_coord_t _x = (lv_coord_t)(X), _y = (lv_coord_t)(Y); \
        if ((n == 0 || _x != buf[n - 1].x || _y != buf[n - 1].y) && n < PATHBAR_MAX_POINTS) { \
            buf[n].x = _x; buf[n].y = _y; n++; } \
    } while (0)

    float inset = pd->band_width / 2.0f + 2.0f;
    float l = bx + inset, t = by + inset;
    float r = bx + bw - inset, b = by + bh - inset;

    if (pd->shape == 4) {                  /* J-hook (Ford digital-cluster tach) */
        /* A TRUE CIRCULAR arc (constant radius — no skew) that ALWAYS ends with a
         * horizontal tangent, flowing into the straight horizontal tail, so the
         * curve meets the bar with a perfect radius and no kink. `hook_angle` is
         * the arc sweep in degrees (meter-style: 90 = quarter circle / vertical at
         * the start, 180 = half circle), and the radius auto-fits the box height
         * for that sweep so it's round and fits every time. orientation picks the
         * corner the hook curls into (0 = bottom-left / tail along the top = Ford). */
        float S = (float)pd->hook_angle * (float)M_PI / 180.0f;   /* sweep, rad */
        float denom = 1.0f - cosf(S);                /* height = R*(1-cos S) */
        if (denom < 0.05f) denom = 0.05f;
        float R = (b - t) / denom;                   /* circle radius that fills H */
        float maxR = (r - l) - 24.0f;                /* always leave room for tail */
        if (R > maxR) R = maxR;
        if (R < 8.0f) R = 8.0f;

        const float DEG = (float)M_PI / 180.0f;
        float cx, cy, a0, a1, tx, ty;      /* center, start/end angle, tail end */
        switch (pd->orientation) {
        case 1: cx = r - R; cy = t + R; a0 = (270.0f + (float)pd->hook_angle)*DEG; a1 = 270.0f*DEG; tx = l; ty = t; break; /* hook BR, tail top-left  */
        case 2: cx = l + R; cy = b - R; a0 = ( 90.0f + (float)pd->hook_angle)*DEG; a1 =  90.0f*DEG; tx = r; ty = b; break; /* hook TL, tail bottom-right */
        case 3: cx = r - R; cy = b - R; a0 = ( 90.0f - (float)pd->hook_angle)*DEG; a1 =  90.0f*DEG; tx = l; ty = b; break; /* hook TR, tail bottom-left  */
        default:cx = l + R; cy = t + R; a0 = (270.0f - (float)pd->hook_angle)*DEG; a1 = 270.0f*DEG; tx = r; ty = t; break; /* hook BL, tail top-right (Ford) */
        }
        int steps = (int)(R * fabsf(a1 - a0) / 6.0f) + 4;
        if (steps < 16)  steps = 16;
        if (steps > 120) steps = 120;
        for (int i = 0; i <= steps; i++) {
            float ang = a0 + (a1 - a0) * (float)i / (float)steps;
            PB_EMIT(cx + R * cosf(ang), cy + R * sinf(ang));
        }
        PB_EMIT(tx, ty);                   /* straight tangent tail */
    } else if (pd->shape == 2) {           /* straight */
        if (pd->orientation == 1) {        /* vertical: bottom -> top */
            float cx = (l + r) / 2.0f;
            PB_EMIT(cx, b); PB_EMIT(cx, t);
        } else {                           /* horizontal: left -> right */
            float cy = (t + b) / 2.0f;
            PB_EMIT(l, cy); PB_EMIT(r, cy);
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
        /* unit directions corner->endpoint (axis-aligned, so just signs) */
        float a1x = (ax > cx) - (ax < cx), a1y = (ay > cy) - (ay < cy);
        float b1x = (ex > cx) - (ex < cx), b1y = (ey > cy) - (ey < cy);
        float t1x = cx + a1x * rad, t1y = cy + a1y * rad;
        float t2x = cx + b1x * rad, t2y = cy + b1y * rad;
        float cnx = cx + (a1x + b1x) * rad, cny = cy + (a1y + b1y) * rad;
        PB_EMIT(ax, ay);
        PB_EMIT(t1x, t1y);
        /* shape 3 = a straight 45° chamfer (t1 -> t2 direct, no arc) */
        if (rad > 1.0f && pd->shape != 3) {
            float a0 = atan2f(t1y - cny, t1x - cnx);
            float a2 = atan2f(t2y - cny, t2x - cnx);
            float d = a2 - a0;
            while (d >  (float)M_PI) d -= 2.0f * (float)M_PI;
            while (d < -(float)M_PI) d += 2.0f * (float)M_PI;
            int steps = (int)(rad * 0.5f) + 8;   /* finer arc = smoother bend */
            if (steps > 72) steps = 72;
            for (int i = 1; i < steps; i++) {
                float ang = a0 + d * (float)i / (float)steps;
                PB_EMIT(cnx + rad * cosf(ang), cny + rad * sinf(ang));
            }
        }
        PB_EMIT(t2x, t2y);
        PB_EMIT(ex, ey);
    }
    #undef PB_EMIT

    _pathbar_free_path(pd);
    if (n < 2) return;
    pd->pts = heap_caps_calloc(n, sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    pd->cum = heap_caps_calloc(n, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!pd->pts || !pd->cum) { _pathbar_free_path(pd); return; }
    for (int i = 0; i < n; i++) pd->pts[i] = buf[i];
    pd->n_pts = (uint16_t)n;
    _pathbar_build_cum(pd);
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
static void _pathbar_invalidate_range(widget_t *w, float fa, float fb) {
    pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
    if (!w->root || !lv_obj_is_valid(w->root)) return;
    if (pd->n_pts < 2 || pd->total_len <= 0.0f) { lv_obj_invalidate(w->root); return; }

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

/* Point + unit normal at arc length s along the path. Returns false if the
 * path is empty. The normal is the left-hand perpendicular of the local
 * tangent — used to lay ticks across the band and push labels off it. */
static bool _pathbar_sample(pathbar_data_t *pd, float s,
                            float *x, float *y, float *nx, float *ny) {
    if (pd->n_pts < 2) return false;
    if (s < 0.0f) s = 0.0f;
    if (s > pd->total_len) s = pd->total_len;
    for (uint16_t i = 1; i < pd->n_pts; i++) {
        if (pd->cum[i] >= s) {
            float seg = pd->cum[i] - pd->cum[i - 1];
            float f = seg > 0.0f ? (s - pd->cum[i - 1]) / seg : 0.0f;
            float p0x = pd->pts[i - 1].x, p0y = pd->pts[i - 1].y;
            float p1x = pd->pts[i].x,     p1y = pd->pts[i].y;
            *x = p0x + (p1x - p0x) * f;   *y = p0y + (p1y - p0y) * f;
            float tx = p1x - p0x, ty = p1y - p0y;
            float L = sqrtf(tx * tx + ty * ty); if (L <= 0.0f) L = 1.0f;
            *nx = -ty / L; *ny = tx / L;
            return true;
        }
    }
    return false;
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

    /* path centroid → which way the numbers face */
    float cgx = 0, cgy = 0;
    for (uint16_t i = 0; i < pd->n_pts; i++) { cgx += pd->pts[i].x; cgy += pd->pts[i].y; }
    cgx /= pd->n_pts; cgy /= pd->n_pts;

    const lv_font_t *lf = pd->label_font[0] ? widget_resolve_font(pd->label_font) : NULL;
    if (!lf) lf = LV_FONT_DEFAULT;

    lv_draw_line_dsc_t td; lv_draw_line_dsc_init(&td);
    td.round_start = 1; td.round_end = 1;
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

        if (len > 0 && wdt > 0) {
            td.color = col; td.width = wdt;
            lv_point_t a = { (lv_coord_t)(px - nx * len / 2.0f), (lv_coord_t)(py - ny * len / 2.0f) };
            lv_point_t b = { (lv_coord_t)(px + nx * len / 2.0f), (lv_coord_t)(py + ny * len / 2.0f) };
            lv_draw_line(ctx, &td, &a, &b);
        }

        if (isMaj && pd->show_labels) {
            float sgn = ((cgx - px) * nx + (cgy - py) * ny) >= 0.0f ? 1.0f : -1.0f;
            float off = pd->band_width / 2.0f + (float)pd->label_gap;
            float lx = px + nx * sgn * off, ly = py + ny * sgn * off;
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

/* ── Band renderer (vertex-offset quad strip) ────────────────────────────────
 * The band is stroked as a strip of convex trapezoids filled with
 * lv_draw_polygon, NOT as overlapping round-capped lines. This is the proper
 * stroke-to-fill: the band outline is the path offset by ±half-width with
 * mitered joints, so flat ends and clean corners come for free — no round-cap
 * overshoot (the old "rounded base/tip" bugs are structurally impossible) and
 * no per-segment fuzz. Two details make it clean and cheap:
 *   - each quad pokes PB_SEAM_OVERLAP px into the next so the later (opaque)
 *     quad buries the previous quad's anti-aliased edge → no black seam lines,
 *   - the lit fill is walked in PB_GRAD_STEP arc-length steps for a smooth
 *     gradient even across one long path segment, last step clipped at the front.
 * Far less overdraw than the line-stroke approach → measurably faster too. */
#define PB_SEAM_OVERLAP 2.0f    /* px each quad pokes into the next to bury AA seams */
#define PB_GRAD_STEP    9.0f    /* px per gradient sub-quad along the lit fill       */
#define PB_MITER_MAX    1.5f    /* cap miter at 1.5*half-width (clean to ~96°, then  */
                                /* a slight bevel) — bounds the off-centreline reach */
                                /* so _pathbar_invalidate_range can cover it.        */

/* Interpolate the band cross-edge (the two offset points) at arc-length s. */
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
    lv_draw_polygon(ctx, rd, pts, K + 1);
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
        lv_draw_polygon(ctx, rd, q, 4);
        return;
    }
    if (_pb_tri_ok(a, b, c)) { lv_point_t t[3] = { a, b, c }; lv_draw_polygon(ctx, rd, t, 3); }
    if (_pb_tri_ok(a, c, d)) { lv_point_t t[3] = { a, c, d }; lv_draw_polygon(ctx, rd, t, 3); }
}

/* Render the band as a vertex-offset quad strip filled with lv_draw_polygon
 * (one convex trapezoid per step), instead of overlapping round-capped lines:
 *   - flat base/tip + clean joins BY CONSTRUCTION (offset points are mitered and
 *     shared between adjacent quads — no round-cap overshoot, no fray),
 *   - each quad pokes PB_SEAM_OVERLAP px into the next so the later (opaque) quad
 *     buries the previous one's anti-aliased edge — kills the black seam lines,
 *   - lit fill is walked in PB_GRAD_STEP arc-length steps for a smooth gradient
 *     even across one long path segment, last step clipped at the fill front.
 * Far less overdraw than the line-stroke path → also faster. */
static void _pathbar_draw_band(lv_draw_ctx_t *ctx, pathbar_data_t *pd,
                               lv_opa_t master, lv_color_t dim_solid) {
    const int n = pd->n_pts;
    if (n < 2 || n > PATHBAR_MAX_POINTS) return;
    const float hw = (float)pd->band_width * 0.5f;

    /* Per-vertex mitered offset points (left/right of the centreline). Endpoints
     * use the plain segment normal → the terminal edge is perpendicular to the
     * path = a flat butt cut. Interior vertices use the angle bisector with the
     * miter length that preserves perpendicular half-width, clamped so a sharp
     * bend can't throw a long spike. */
    static lv_point_t L[PATHBAR_MAX_POINTS], R[PATHBAR_MAX_POINTS];
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
    /* The seam-overlap trick assumes opaque-over-opaque (the later quad fully
     * buries the prior quad's AA edge). When the whole widget is translucent —
     * e.g. the boot fade-reveal animates the root's opa — overlapping would
     * alpha-blend the overlap region twice into a bright stripe, so drop it while
     * not fully opaque (the shared mitered edges still abut cleanly). */
    float ov = (master < LV_OPA_COVER) ? 0.0f : PB_SEAM_OVERLAP;

    /* Dim full-path track: one trapezoid per segment, each poking into the next. */
    rd.bg_color = dim_solid;
    for (int i = 1; i < n; i++) {
        lv_point_t el = L[i], er = R[i];
        if (i < n - 1) _spk_edge_at(L, R, pd->cum, n, pd->cum[i] + ov, &el, &er);
        _pb_fill_quad(ctx, &rd, L[i-1], R[i-1], er, el);
    }
    if (pd->rounded) {     /* round the track's two ends (base, far end) */
        _spk_round_cap(ctx, &rd, pd->pts[0].x, pd->pts[0].y,
                       (float)(pd->pts[0].x - pd->pts[1].x),
                       (float)(pd->pts[0].y - pd->pts[1].y), hw);
        _spk_round_cap(ctx, &rd, pd->pts[n-1].x, pd->pts[n-1].y,
                       (float)(pd->pts[n-1].x - pd->pts[n-2].x),
                       (float)(pd->pts[n-1].y - pd->pts[n-2].y), hw);
    }

    /* Lit fill walked in small steps for a smooth gradient; last step clipped. */
    float f = pd->cur_frac;
    if (f <= 0.0f || total <= 0.0f) return;
    float fill = f * total;
    lv_color_t bright = pd->lit_color;
    lv_color_t dim    = lv_color_mix(bright, lv_color_black(), 66);
    float rl = (pd->redline < pd->val_max && pd->val_max > pd->val_min)
             ? (pd->redline - pd->val_min) / (pd->val_max - pd->val_min) : 1.0f;
    float rlpx = rl * total;                          /* exact redline arc-length */

    float s0 = 0.0f;
    lv_point_t e0l = L[0], e0r = R[0];
    while (s0 < fill - 0.01f) {
        float s1 = s0 + PB_GRAD_STEP;
        bool last = false;
        if (s1 >= fill) { s1 = fill; last = true; }
        /* Land a quad boundary exactly on the redline so the lit/redline colour
         * break is at rl, not snapped to the nearest gradient step. */
        if (s0 < rlpx - 0.01f && s1 > rlpx) { s1 = rlpx; last = false; }
        lv_point_t e1l, e1r;                          /* true edge at s1 (the front if last) */
        _spk_edge_at(L, R, pd->cum, n, s1, &e1l, &e1r);
        lv_point_t qel = e1l, qer = e1r;              /* end edge actually drawn (overlapped) */
        if (!last) _spk_edge_at(L, R, pd->cum, n, s1 + ov, &qel, &qer);

        float pc = ((s0 + s1) * 0.5f) / total;
        lv_color_t col;
        if (pc > rl) col = pd->redline_color;
        else if (pd->fade_fill) {
            float t = pc / PATHBAR_FADE_FULL; if (t > 1.0f) t = 1.0f;
            col = lv_color_mix(bright, dim, (uint8_t)(t * 255.0f));
        } else col = bright;
        rd.bg_color = col;
        _pb_fill_quad(ctx, &rd, e0l, e0r, qer, qel);

        e0l = e1l; e0r = e1r;                         /* next quad starts at the true edge */
        s0  = s1;
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
     * (dim_opa is the authoring knob for how dark the track reads). The band
     * renderer fills opaque quads, so there's no cap/overlap compounding. */
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
                lv_point_t a = { (lv_coord_t)(mx - mnx * hw), (lv_coord_t)(my - mny * hw) };
                lv_point_t b = { (lv_coord_t)(mx + mnx * hw), (lv_coord_t)(my + mny * hw) };
                lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
                ld.color = pd->lead_edge_color;
                ld.width = pd->lead_edge_width;
                ld.opa = master;
                ld.round_start = ld.round_end = 0;   /* flat ends */
                lv_draw_line(ctx, &ld, &a, &b);
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
        if (pd->shape == 4) cJSON_AddNumberToObject(cfg, "hook_angle", pd->hook_angle);
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
    if (pd->smoothing_ms != 0)
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
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_recolor_ticks");
    if (cJSON_IsBool(item)) pd->redline_recolor_ticks = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_font");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(pd->label_font, item->valuestring, sizeof(pd->label_font));

    /* path: flat [x0,y0,x1,y1,...] of absolute screen-px points */
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
    pd->dim_color     = _u32_to_color(DEF_DIM_COLOR);
    pd->lit_color     = _u32_to_color(DEF_LIT_COLOR);
    pd->redline_color = _u32_to_color(DEF_RED_COLOR);
    pd->lead_edge_enabled = true;
    pd->lead_edge_color   = lv_color_hex(DEF_LEAD_COLOR);   /* 888->565 (NOT _u32_to_color, which expects raw 565) */
    pd->lead_edge_width   = DEF_LEAD_WIDTH;
    pd->dim_opa       = DEF_DIM_OPA;
    pd->smoothing_ms  = 0;
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
