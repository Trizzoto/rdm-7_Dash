/**
 * gauge_tick.h — shared tick-rendering math used by widget_arc and
 * widget_meter.
 *
 * Both widgets draw ticks around a circular scale and stamp per-tier tick
 * images rotated to point outward; the underlying geometry (glow/outline
 * behind a drawn tick line, and the outward rotation angle for a stamped
 * image) is identical between the two widgets — only the surrounding
 * per-tier styling (image source, scale, redline recolour, etc.) differs,
 * so that part stays in each widget's own file.
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Draw an outline/glow BEHIND a drawn tick line (p1..p2): a tight border
 * at @p strength, plus @p fade softer wider rings for a glow. LVGL's
 * meter reports p1..p2 as the full rim->centre radial and masks the
 * visible tick to @p tlen px from the rim, so this clips the outline to
 * that same span (else it draws a ray to the centre). @p cx,@p cy is the
 * scale centre, @p base_w the tick line's own width. No-op if
 * @p strength is 0 or either point is NULL.
 */
void gauge_tick_draw_outline(lv_draw_ctx_t *ctx, const lv_point_t *p1,
                             const lv_point_t *p2, lv_coord_t cx, lv_coord_t cy,
                             lv_coord_t tlen, lv_coord_t base_w,
                             lv_color_t color, uint8_t strength, uint8_t fade);

/**
 * Rotation angle (LVGL 0.1-degree units, 0..3599) to point an image
 * authored pointing UP (tip toward the rim) outward along the radial
 * direction @p dx,@p dy (typically tick-centre minus scale-centre).
 */
int16_t gauge_tick_outward_angle(float dx, float dy);

#ifdef __cplusplus
}
#endif
