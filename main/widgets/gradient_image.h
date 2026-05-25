/**
 * gradient_image.h — bake a multi-stop gradient into an RGB565 image.
 *
 * Used by widget_bar and widget_rpm_bar to render N-stop gradients
 * (LVGL v8 only natively supports 2-stop bg_grad_color). The baked
 * image is set as bg_img_src on the bar's PART_INDICATOR; LVGL clips
 * to the indicator's current fill width, so as the bar value rises
 * the user sees progressively more of the gradient.
 *
 * Memory: each gradient consumes width × height × 2 bytes in PSRAM
 * (e.g. a 300×30 bar ~= 18 KB). Caller manages lifecycle — regenerate
 * on widget resize or stop edit, free on destroy.
 */
#pragma once

#include "gradient_stops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Heap-allocates an lv_img_dsc_t + pixel buffer in PSRAM and bakes
 * the gradient at the given dimensions. Returns NULL on alloc failure
 * or invalid args (w/h < 1, stops count < 2). Caller owns both — free
 * via gradient_image_free(). The gradient runs horizontally
 * (x=0 → stop[0], x=w-1 → stop[last]); height pixels are uniform per
 * column. */
lv_img_dsc_t *gradient_image_create(uint16_t w, uint16_t h,
                                    const gradient_stops_t *stops);

/* Frees the descriptor and the pixel buffer. Safe to call with NULL. */
void gradient_image_free(lv_img_dsc_t *dsc);

#ifdef __cplusplus
}
#endif
