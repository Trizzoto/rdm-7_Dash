/**
 * gradient_stops.c — see header.
 *
 * Sample/lerp uses lv_color_mix in 0..255 weight space. We expose RGB565
 * directly so callers don't need lv_color_t when they just want a 16-bit
 * value (the widgets store colours that way too).
 */
#include "gradient_stops.h"

#include <stdlib.h>
#include <string.h>

static int _cmp_pos(const void *a, const void *b) {
    const gradient_stop_t *sa = (const gradient_stop_t *)a;
    const gradient_stop_t *sb = (const gradient_stop_t *)b;
    return (int)sa->pos - (int)sb->pos;
}

bool gradient_stops_from_json(const cJSON *arr, gradient_stops_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsArray(arr)) return false;

    int n = cJSON_GetArraySize(arr);
    if (n < 2) return false;
    if (n > GRADIENT_MAX_STOPS) n = GRADIENT_MAX_STOPS;

    int count = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(obj)) continue;
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(obj, "pos");
        const cJSON *jc = cJSON_GetObjectItemCaseSensitive(obj, "color");
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jc)) continue;
        int pos = jp->valueint;
        if (pos < 0)   pos = 0;
        if (pos > 100) pos = 100;
        out->stops[count].pos   = (uint8_t)pos;
        out->stops[count].color = (uint16_t)(jc->valueint & 0xFFFF);
        count++;
    }
    if (count < 2) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    /* Stable sort by pos so subsequent sample() can walk linearly. The
     * editor is responsible for ordering during user edits but a
     * malformed save from an older client shouldn't break rendering. */
    qsort(out->stops, count, sizeof(gradient_stop_t), _cmp_pos);
    out->count = (uint8_t)count;
    return true;
}

bool gradient_stops_install_legacy_2stop(gradient_stops_t *out,
                                         uint16_t base, uint16_t legacy_end)
{
    if (!out) return false;
    if (out->count >= 2) return false;  /* a real grad_stops already wins */
    out->stops[0].pos   = 0;
    out->stops[0].color = base;
    out->stops[1].pos   = 100;
    out->stops[1].color = legacy_end;
    out->count          = 2;
    return true;
}

cJSON *gradient_stops_to_json(const gradient_stops_t *in)
{
    if (!in || in->count < 2) return NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (uint8_t i = 0; i < in->count; i++) {
        cJSON *obj = cJSON_CreateObject();
        if (!obj) continue;
        cJSON_AddNumberToObject(obj, "pos",   (double)in->stops[i].pos);
        cJSON_AddNumberToObject(obj, "color", (double)in->stops[i].color);
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

uint16_t gradient_stops_sample(const gradient_stops_t *g, float t)
{
    if (!g || g->count < 2) return 0;
    if (t <= 0.0f) return g->stops[0].color;
    if (t >= 1.0f) return g->stops[g->count - 1].color;
    /* Find the bracket containing t (stops are sorted by pos). */
    float t_pct = t * 100.0f;
    uint8_t i;
    for (i = 1; i < g->count; i++) {
        if (g->stops[i].pos >= (uint8_t)t_pct) break;
    }
    if (i >= g->count) i = g->count - 1;
    const gradient_stop_t *a = &g->stops[i - 1];
    const gradient_stop_t *b = &g->stops[i];
    float span = (float)(b->pos - a->pos);
    float local = (span > 0.0f) ? (t_pct - (float)a->pos) / span : 0.0f;
    if (local < 0.0f) local = 0.0f;
    if (local > 1.0f) local = 1.0f;
    /* lv_color_mix weights the FIRST argument with c1_w/255; so to lerp
     * a->b with local-fraction t, we pass (b, a, (1-local)*255). */
    lv_color_t mixed = lv_color_mix(
        (lv_color_t){.full = b->color},
        (lv_color_t){.full = a->color},
        (uint8_t)((1.0f - local) * 255.0f + 0.5f));
    return mixed.full;
}
