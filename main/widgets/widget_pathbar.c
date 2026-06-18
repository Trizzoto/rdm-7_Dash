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

    if (pd->shape == 2) {                  /* straight */
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

/* Stroke every path segment overlapping arc-length [a, b] with the given dsc.
 * LVGL clips each segment to the draw clip area, so off-region segments cost
 * only a clip-reject (cheap with targeted invalidate).
 *
 * round_ends controls only the band's two OUTER caps (start at a, end at b).
 * Internal joints between segments are ALWAYS rounded: lv_draw_line has no join
 * support, so a smooth bend is just consecutive round caps overlapping. With
 * butt caps everywhere, each bend leaves a triangular gap on its outer side ->
 * a frayed/saw-tooth edge along curves. dsc is mutated per segment. */
static void _stroke_range(lv_draw_ctx_t *ctx, pathbar_data_t *pd, float a, float b,
                          lv_draw_line_dsc_t *dsc, bool round_ends) {
    bool first = true;
    for (uint16_t i = 1; i < pd->n_pts; i++) {
        float c0 = pd->cum[i - 1], c1 = pd->cum[i];
        if (c1 <= a) continue;
        if (c0 >= b) break;
        float seg = c1 - c0;
        if (seg <= 0.0f) continue;
        float sa = a > c0 ? (a - c0) / seg : 0.0f;
        float sb = b < c1 ? (b - c0) / seg : 1.0f;
        lv_point_t p0 = pd->pts[i - 1], p1 = pd->pts[i];
        lv_point_t q0 = { (lv_coord_t)(p0.x + (p1.x - p0.x) * sa),
                          (lv_coord_t)(p0.y + (p1.y - p0.y) * sa) };
        lv_point_t q1 = { (lv_coord_t)(p0.x + (p1.x - p0.x) * sb),
                          (lv_coord_t)(p0.y + (p1.y - p0.y) * sb) };
        bool is_last = (b <= c1);                 /* this segment holds the band end */
        dsc->round_start = first   ? (round_ends ? 1 : 0) : 1;
        dsc->round_end   = is_last ? (round_ends ? 1 : 0) : 1;
        lv_draw_line(ctx, dsc, &q0, &q1);
        first = false;
    }
}

/* ── Draw one band between arc-length fractions [fa, fb] ─────────────────────
 */
static void _draw_band(lv_draw_ctx_t *ctx, pathbar_data_t *pd, float fa, float fb,
                       lv_color_t color, lv_opa_t opa, lv_opa_t master) {
    if (fb <= fa || pd->n_pts < 2 || pd->total_len <= 0.0f) return;
    float a = fa * pd->total_len;
    float b = fb * pd->total_len;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = pd->band_width;
    dsc.opa = master < LV_OPA_MAX ? (lv_opa_t)(((uint32_t)opa * master) >> 8) : opa;
    if (dsc.opa > LV_OPA_MIN) _stroke_range(ctx, pd, a, b, &dsc, pd->rounded);
}

/* ── Positional fade fill ────────────────────────────────────────────────────
 * Draw [fa, fb] as a series of opaque sub-bands tinted dim->bright by their
 * ABSOLUTE position along the path (not by value), so it stays targeted-
 * invalidate cheap: each lv_draw_line is clipped to the dirty rect, and the
 * colour at a given path position never changes as the value moves. Brightness
 * ramps from `dim` at fraction 0 to full `bright` by PATHBAR_FADE_FULL of the
 * path — matching the studio preview. */
#define PATHBAR_FADE_FULL 0.45f
static void _draw_band_faded(lv_draw_ctx_t *ctx, pathbar_data_t *pd, float fa, float fb,
                             lv_color_t bright, lv_opa_t master) {
    if (fb <= fa || pd->n_pts < 2 || pd->total_len <= 0.0f) return;
    lv_color_t dim = lv_color_mix(bright, lv_color_black(), 66);   /* 26% of bright — matches studio _pvDarken(col,0.26) */
    /* ~6px sub-bands along the lit span: smooth blend (near the RGB565 step
     * floor). Targeted-invalidate keeps the redraw cheap (segments outside the
     * dirty rect clip away). Clamp for sanity. */
    float span_px = (fb - fa) * pd->total_len;
    int segs = (int)(span_px / 6.0f) + 1;
    if (segs < 2)  segs = 2;
    if (segs > 90) segs = 90;
    float dstep = (fb - fa) / (float)segs;
    for (int i = 0; i < segs; i++) {
        float p0 = fa + dstep * i;
        /* overlap into the next (brighter) sub-band so butt-cap joints can't
         * leave a thin dark gap on curves — the next band paints over it. */
        float p1 = fa + dstep * (i + 1) + (i < segs - 1 ? dstep * 0.5f : 0.0f);
        float pc = p0 + dstep * 0.5f;                       /* centre fraction */
        float t  = pc / PATHBAR_FADE_FULL;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        lv_color_t col = lv_color_mix(bright, dim, (uint8_t)(t * 255.0f));
        _draw_band(ctx, pd, p0, p1, col, LV_OPA_COVER, master);
    }
}

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

    lv_coord_t m = (lv_coord_t)(pd->band_width / 2) + 3;
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

    /* Dim full-path track. Drawn OPAQUE using dim_color pre-blended over black
     * at dim_opa, rather than as a translucent stroke. lv_draw_line has no join
     * support, so a curved bend is many short segments whose rounded caps
     * overlap; at <100% opacity that overlap COMPOUNDS (each cap re-blends the
     * same pixels) and the bends/joints render darker than the single-segment
     * straight runs — the bar looks blotchy at corners. Pre-blending to a solid
     * colour makes overlap a no-op (opaque over opaque = same colour), so the
     * whole track reads as one uniform run. The straight runs already showed
     * exactly this blended shade (dim_color over the dark cluster bg), so they
     * are unchanged — only the corners lighten to match. dim_opa stays the
     * authoring knob: it sets how dark the solid track is. */
    lv_color_t dim_solid = lv_color_mix(pd->dim_color, lv_color_black(), pd->dim_opa);
    _draw_band(ctx, pd, 0.0f, 1.0f, dim_solid, LV_OPA_COVER, master);

    float f = pd->cur_frac;
    if (f > 0.0f) {
        /* redline split */
        float rl = 1.0f;
        if (pd->redline < pd->val_max && pd->val_max > pd->val_min)
            rl = (pd->redline - pd->val_min) / (pd->val_max - pd->val_min);
        if (rl < 0.0f) rl = 0.0f;
        if (rl > 1.0f) rl = 1.0f;

        if (f <= rl) {
            if (pd->fade_fill) _draw_band_faded(ctx, pd, 0.0f, f, pd->lit_color, master);
            else               _draw_band(ctx, pd, 0.0f, f, pd->lit_color, LV_OPA_COVER, master);
        } else {
            if (pd->fade_fill) _draw_band_faded(ctx, pd, 0.0f, rl, pd->lit_color, master);
            else               _draw_band(ctx, pd, 0.0f, rl, pd->lit_color, LV_OPA_COVER, master);
            _draw_band(ctx, pd, rl, f, pd->redline_color, LV_OPA_COVER, master);
        }
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
        cJSON_AddNumberToObject(cfg, "corner_radius", pd->corner_radius);
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

    /* Only custom shapes carry an explicit path; parametric shapes regenerate. */
    if (pd->shape == 0 && pd->pts && pd->n_pts > 0) {
        cJSON *path = cJSON_AddArrayToObject(cfg, "path");
        for (uint16_t i = 0; i < pd->n_pts; i++) {
            cJSON_AddItemToArray(path, cJSON_CreateNumber(pd->pts[i].x));
            cJSON_AddItemToArray(path, cJSON_CreateNumber(pd->pts[i].y));
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
    if (cJSON_IsNumber(item)) pd->shape = (uint8_t)LV_CLAMP(0, item->valueint, 3);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "orientation");
    if (cJSON_IsNumber(item)) pd->orientation = (uint8_t)LV_CLAMP(0, item->valueint, 3);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "corner_radius");
    if (cJSON_IsNumber(item)) pd->corner_radius = (uint16_t)LV_CLAMP(0, item->valueint, 1000);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "rounded");
    if (cJSON_IsBool(item)) pd->rounded = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "fade_fill");
    if (cJSON_IsBool(item)) pd->fade_fill = cJSON_IsTrue(item);
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
                for (int i = 0; i < n; i++) {
                    cJSON *px = cJSON_GetArrayItem(path, i * 2);
                    cJSON *py = cJSON_GetArrayItem(path, i * 2 + 1);
                    pd->pts[i].x = (lv_coord_t)(cJSON_IsNumber(px) ? px->valuedouble : 0);
                    pd->pts[i].y = (lv_coord_t)(cJSON_IsNumber(py) ? py->valuedouble : 0);
                }
                pd->n_pts = (uint16_t)n;
                _pathbar_build_cum(pd);
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
