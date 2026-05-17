#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Night-mode overrides for arc ───────────────────────────────────────── */
typedef struct {
    NIGHT_FIELD_COLOR(arc_color)
    NIGHT_FIELD_COLOR(bg_arc_color)
    NIGHT_FIELD_COLOR(value_color)
    NIGHT_FIELD_COLOR(redline_color)
    NIGHT_FIELD_IMAGE(arc_image, 64)
    NIGHT_FIELD_IMAGE(arc_image_full, 64)
} arc_night_overrides_t;

typedef struct {
    int16_t    start_angle;     /* default: 135 */
    int16_t    end_angle;       /* default: 45 */
    uint8_t    arc_width;       /* default: 10 */
    lv_color_t arc_color;       /* default: 0x00FF00 — fill color (used when not in redline/limiter) */
    lv_color_t bg_arc_color;    /* default: 0x333333 */
    uint8_t    bg_arc_width;    /* default: 10 */
    bool       rounded_ends;    /* default: false */
    lv_obj_t  *arc_obj;         /* runtime only */

    /* Signal binding (optional data source) */
    char       signal_name[32]; /* default: "" */
    int16_t    signal_index;    /* runtime: -1 = unbound */
    float      signal_min;      /* default: 0 */
    float      signal_max;      /* default: 100 */

    /* Image-based arc mode */
    char          arc_image[64];      /* track/empty image name, default: "" */
    char          arc_image_full[64]; /* fill image name, default: "" */
    lv_img_dsc_t *arc_img_dsc;        /* runtime: loaded track image */
    lv_img_dsc_t *arc_img_full_dsc;   /* runtime: loaded full image */
    lv_obj_t     *img_bg_obj;         /* runtime: background image object */
    lv_obj_t     *img_full_obj;       /* runtime: full image object */
    lv_obj_t     *img_clip_obj;       /* runtime: clipping container */

    /* ── Redline zone — visual emphasis for the "danger" range.
     *   - A separate static arc is drawn from `redline_threshold` to
     *     `signal_max` on top of the track in `redline_color`. This stays
     *     visible at all times so the driver knows where the danger zone is.
     *   - When `redline_recolor_fill` is true, the moving indicator arc also
     *     switches to `redline_color` while value >= threshold. */
    bool       redline_enabled;         /* default: false — master toggle */
    float      redline_threshold;       /* default: 80 — value where zone starts */
    lv_color_t redline_color;           /* default: 0xFF0000 */
    uint8_t    redline_arc_width;       /* default: 0 (= use arc_width) */
    bool       redline_recolor_fill;    /* default: true — also recolor the moving indicator when in zone */
    lv_obj_t  *redline_arc_obj;         /* runtime: the red zone marker arc */

    /* ── Limiter effect — applied when value >= limiter_value.
     *   0 = None       — no visual change (redline still applies if enabled)
     *   1 = Bar Flash  — indicator arc toggles between arc_color and limiter_color
     *                    every flash_speed_ms milliseconds
     *   2 = Bar Solid  — indicator goes solid limiter_color (no flashing) */
    uint8_t    limiter_effect;          /* default: 0 */
    float      limiter_value;           /* default: 90 */
    lv_color_t limiter_color;           /* default: 0xFF0000 */
    uint16_t   flash_speed_ms;          /* default: 200 (range 50..1000) */
    lv_timer_t *flash_timer;            /* runtime: timer for flash effect */
    bool       flash_phase;             /* runtime: current flash phase (true = limiter color shown) */
    bool       in_limiter;              /* runtime: cached "value >= limiter_value" */

    /* ── Value text overlay — optional centered label that shows the
     * current value with formatting. Sits on top of the arc; positioned by
     * value_y_offset relative to widget center. */
    bool       show_value;              /* default: false */
    char       value_font[32];          /* default: "" — empty = LVGL default */
    lv_color_t value_color;             /* default: 0xFFFFFF */
    int16_t    value_y_offset;          /* default: 0 — vertical offset from center, px */
    uint8_t    value_decimals;          /* default: 0 — decimals shown */
    char       value_unit[16];          /* default: "" — suffix string (e.g. "RPM") */
    lv_obj_t  *value_label;             /* runtime: the center label */

    /* Cached current value — used by redraws (resize, night swap) so they
     * don't have to wait for the next signal tick to look right. */
    float      _cached_value;

    /* Night-mode appearance overrides (only applied when night_mode active) */
    arc_night_overrides_t night;
} arc_data_t;

widget_t *widget_arc_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
