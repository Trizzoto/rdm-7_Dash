#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include "gradient_stops.h"
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
    /* Optional N-stop value-interpolated colour. When grad_stops.count
     * is ≥2, the indicator stroke samples the stops at
     * t = (value - signal_min) / (signal_max - signal_min) — effectively
     * a gradient that walks with the value, lerping between whichever
     * two stops bracket t. LVGL v8 doesn't expose arc-stroke gradients
     * natively (angular sweep would need a custom draw), so this
     * value-walked colour change is the closest visual equivalent.
     *
     * Redline / limiter still override with their own colours so
     * danger visibility isn't lost. Stops are absolute colours — they
     * don't inherit from arc_color or its night override (legacy
     * 2-stop layouts get migrated to [{0, arc_color}, {100, end}] at
     * load time to preserve prior visuals). */
    gradient_stops_t grad_stops;
    uint8_t    bg_arc_width;    /* default: 10 */
    bool       rounded_ends;    /* default: false */
    lv_obj_t  *arc_obj;         /* runtime only */

    /* Signal binding (optional data source) */
    char       signal_name[32]; /* default: "" */
    int16_t    signal_index;    /* runtime: -1 = unbound */
    float      signal_min;      /* default: 0 */
    float      signal_max;      /* default: 100 */
    /* ── v14 channel binding ──────────────────────────────────────
     * See widget_meter.h for pattern. signal_min/max ← channel.min/max,
     * redline_threshold ← channel.high_warn. */
    char       channel_id[32];
    void      *channel;     /* channel_t* — opaque */

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

    /* Rule-override cached fg color. When a widget_rule overrides arc_color,
     * _arc_apply_fill_color uses this as the "normal" base instead of
     * d->arc_color. Cleared when the active rule set drops the arc_color
     * override (or when no rule is active). Lets limiter/redline flash
     * without clobbering the rule color permanently — the rule color is
     * restored when the zone clears. */
    lv_color_t _rule_arc_color;
    bool       _rule_arc_color_set;

    /* Paint memo — skip redundant per-tick style writes (LVGL v8
     * set_style/set_text always invalidate). _arc_apply_fill_color is the
     * sole writer of the indicator color and _arc_update_value_label the sole
     * writer of the label text, so comparing the FINAL fill / formatted
     * string keeps these coherent across all paths (signal/flash/night/
     * overrides/stale). Inspector arc_color writes the indicator directly, so
     * it must clear _last_fill_valid. Mirrors widget_rpm_bar.c s_paint_cache. */
    lv_color_t _last_fill;
    bool       _last_fill_valid;
    char       _last_label[32];
    bool       _last_label_valid;

    /* Night-mode appearance overrides (only applied when night_mode active) */
    arc_night_overrides_t night;
} arc_data_t;

widget_t *widget_arc_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
