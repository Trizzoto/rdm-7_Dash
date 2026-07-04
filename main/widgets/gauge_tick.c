#include "gauge_tick.h"

#include <math.h>

void gauge_tick_draw_outline(lv_draw_ctx_t *ctx, const lv_point_t *p1,
                             const lv_point_t *p2, lv_coord_t cx, lv_coord_t cy,
                             lv_coord_t tlen, lv_coord_t base_w,
                             lv_color_t color, uint8_t strength, uint8_t fade)
{
    if (!ctx || strength == 0 || !p1 || !p2) return;
    float d1 = (float)((p1->x-cx)*(p1->x-cx) + (p1->y-cy)*(p1->y-cy));
    float d2 = (float)((p2->x-cx)*(p2->x-cx) + (p2->y-cy)*(p2->y-cy));
    lv_point_t outer = d1 >= d2 ? *p1 : *p2;            /* rim end */
    float dx = (float)(cx - outer.x), dy = (float)(cy - outer.y);
    float dist = sqrtf(dx*dx + dy*dy);
    lv_point_t inner = outer;
    if (dist > 1.0f) {
        inner.x = (lv_coord_t)(outer.x + dx/dist * tlen);
        inner.y = (lv_coord_t)(outer.y + dy/dist * tlen);
    }
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = color;
    ld.round_start = 1;
    ld.round_end = 1;
    int f = fade > 20 ? 20 : fade;
    for (int j = f; j >= 1; j--) {                       /* wide+faint → narrow glow */
        ld.width = (lv_coord_t)(base_w + 2 + 2 * j);
        ld.opa   = (lv_opa_t)(((uint16_t)strength * (f - j + 1)) / (f + 1) / 2);
        lv_draw_line(ctx, &ld, &outer, &inner);
    }
    ld.width = (lv_coord_t)(base_w + 2);                 /* tight hard outline */
    ld.opa   = strength;
    lv_draw_line(ctx, &ld, &outer, &inner);
}

int16_t gauge_tick_outward_angle(float dx, float dy)
{
    float deg = atan2f(dy, dx) * 57.2957795f;
    int a = (int)lroundf(deg + 90.0f) * 10;
    a %= 3600;
    if (a < 0) a += 3600;
    return (int16_t)a;
}
