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
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_pathbar";

/* ── Defaults ───────────────────────────────────────────────────────────── */
#define DEF_MIN          0.0f
#define DEF_MAX          11000.0f
#define DEF_BAND_WIDTH   22
#define DEF_DIM_COLOR    0xC8CBCF
#define DEF_LIT_COLOR    0x202328
#define DEF_RED_COLOR    0xF26E10
#define DEF_DIM_OPA      70
#define DEF_W            560
#define DEF_H            320
#define PATHBAR_ANIM_MS  16

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

/* ── Draw one band between arc-length fractions [fa, fb] ─────────────────── */
static void _draw_band(lv_draw_ctx_t *ctx, pathbar_data_t *pd, float fa, float fb,
                       lv_color_t color, lv_opa_t opa, lv_opa_t master) {
    if (fb <= fa || pd->n_pts < 2 || pd->total_len <= 0.0f) return;
    float a = fa * pd->total_len;
    float b = fb * pd->total_len;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = pd->band_width;
    dsc.round_start = pd->rounded ? 1 : 0;
    dsc.round_end   = pd->rounded ? 1 : 0;
    dsc.opa = master < LV_OPA_MAX ? (lv_opa_t)(((uint32_t)opa * master) >> 8) : opa;
    if (dsc.opa <= LV_OPA_MIN) return;

    for (uint16_t i = 1; i < pd->n_pts; i++) {
        float c0 = pd->cum[i - 1], c1 = pd->cum[i];
        if (c1 <= a) continue;        /* segment entirely before the range */
        if (c0 >= b) break;           /* and the rest are after it */
        float seg = c1 - c0;
        if (seg <= 0.0f) continue;
        float sa = a > c0 ? (a - c0) / seg : 0.0f;
        float sb = b < c1 ? (b - c0) / seg : 1.0f;
        lv_point_t p0 = pd->pts[i - 1], p1 = pd->pts[i];
        lv_point_t q0 = { (lv_coord_t)(p0.x + (p1.x - p0.x) * sa),
                          (lv_coord_t)(p0.y + (p1.y - p0.y) * sa) };
        lv_point_t q1 = { (lv_coord_t)(p0.x + (p1.x - p0.x) * sb),
                          (lv_coord_t)(p0.y + (p1.y - p0.y) * sb) };
        lv_draw_line(ctx, &dsc, &q0, &q1);
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

    /* dim full-path track */
    _draw_band(ctx, pd, 0.0f, 1.0f, pd->dim_color, pd->dim_opa, master);

    float f = pd->cur_frac;
    if (f <= 0.0f) return;

    /* redline split */
    float rl = 1.0f;
    if (pd->redline < pd->val_max && pd->val_max > pd->val_min)
        rl = (pd->redline - pd->val_min) / (pd->val_max - pd->val_min);
    if (rl < 0.0f) rl = 0.0f;
    if (rl > 1.0f) rl = 1.0f;

    if (f <= rl) {
        _draw_band(ctx, pd, 0.0f, f, pd->lit_color, LV_OPA_COVER, master);
    } else {
        _draw_band(ctx, pd, 0.0f, rl, pd->lit_color, LV_OPA_COVER, master);
        _draw_band(ctx, pd, rl, f, pd->redline_color, LV_OPA_COVER, master);
    }
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
    if (!pd->rounded) cJSON_AddBoolToObject(cfg, "rounded", false);
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

    if (pd->pts && pd->n_pts > 0) {
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
    item = cJSON_GetObjectItemCaseSensitive(cfg, "rounded");
    if (cJSON_IsBool(item)) pd->rounded = cJSON_IsTrue(item);
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
    pd->dim_color     = _u32_to_color(DEF_DIM_COLOR);
    pd->lit_color     = _u32_to_color(DEF_LIT_COLOR);
    pd->redline_color = _u32_to_color(DEF_RED_COLOR);
    pd->dim_opa       = DEF_DIM_OPA;
    pd->smoothing_ms  = 0;

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
