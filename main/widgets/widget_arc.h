#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include "widget_smooth.h"
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
    /* Low/high alert fill colours — applied DYNAMICALLY on LV_PART_INDICATOR in
     * _arc_apply_fill_color (not baked), so a night override just triggers a
     * re-apply of the fill, no overlay rebuild. */
    NIGHT_FIELD_COLOR(arc_low_color)
    NIGHT_FIELD_COLOR(arc_high_color)
    /* Tick + value-line colours are baked into the overlay lv_meter at
     * create time (LVGL v8 has no live tick/needle recolor API), so the
     * overlay is rebuilt on night apply when one of these is set. See
     * _arc_apply_night_mode. */
    NIGHT_FIELD_COLOR(minor_tick_color)
    NIGHT_FIELD_COLOR(major_tick_color)
    NIGHT_FIELD_COLOR(tick_label_color)
    NIGHT_FIELD_COLOR(value_line_color)
    NIGHT_FIELD_IMAGE(arc_image, 64)
    NIGHT_FIELD_IMAGE(arc_image_full, 64)
} arc_night_overrides_t;

typedef struct {
    int16_t    start_angle;     /* default: 135 */
    int16_t    end_angle;       /* default: 45 */
    uint8_t    arc_width;       /* default: 10 */
    uint8_t    arc_offset;      /* default: 0 — radial inset (px) pushing the
                                 * arc track inward from the widget edge.
                                 * See _arc_track_inset. */
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
    bool       fade_fill;       /* default: false — positional dim->bright fade overlay on the moving fill */
    bool       lead_edge_enabled;  /* default: true — bright "current value" cap at the fill tip */
    lv_color_t lead_edge_color;    /* default: near-white */
    uint8_t    lead_edge_width;    /* default: 6 — cap length px along the arc (0 = off) */
    lv_obj_t  *arc_obj;         /* runtime only */
    lv_obj_t  *sector_clip;     /* runtime: clipping container sized to the
                                 * swept-sector bbox; the arc + redline arc live
                                 * inside it so dirty rects in the dead part of
                                 * the circle's bounding square never wake the
                                 * arc draw at all (standard mode only) */

    /* Signal binding (optional data source) */
    char       signal_name[32]; /* default: "" */
    int16_t    signal_index;    /* runtime: -1 = unbound */
    float      signal_min;      /* default: 0 */
    float      signal_max;      /* default: 100 */
    /* ── Value axis: NATIVE bases vs. rendered DISPLAY values ─────────────
     * A channel decodes in a NATIVE unit (kPa) and renders in whatever
     * DISPLAY unit the user picked (psi). Everything the arc measures along
     * its value axis — the scale, the redline, the limiter trip point, the
     * anchor pivot, the tick window — has to share ONE unit with the value
     * itself, which _arc_on_signal converts to display units on the way in.
     *
     * So each of those quantities exists twice: a *_base field holding what
     * was AUTHORED, in the channel's native unit, and the live field above
     * (signal_min/max, redline_threshold, limiter_value, anchor_value,
     * tick_min/tick_max) holding that base re-expressed in the display unit.
     * The base is what the layout persists and what the channel stores its
     * range/thresholds in; the live field is what renders.
     *
     * Keeping the base separate is what makes the conversion idempotent:
     * _arc_sync_units() always derives the live field FROM the base, never by
     * re-converting an already-converted value, so the channel-changed notify
     * can fire any number of times without compounding. Persisting the base
     * (not the converted value) is what stops a save/reload converting twice.
     * Same pattern as range_min_base/range_max_base in widget_meter.h. */
    bool       range_from_layout;  /* layout (or Widget settings) carried an explicit
                                    * signal_min/max → that display scale is the user's
                                    * own; don't let the bound channel's range replace
                                    * it in _arc_sync_units */
    float      range_min_base;     /* native twin of signal_min */
    float      range_max_base;     /* native twin of signal_max */
    float      redline_base;       /* native twin of redline_threshold */
    float      limiter_base;       /* native twin of limiter_value */
    float      anchor_base;        /* native twin of anchor_value */
    float      tick_min_base;      /* native twin of tick_min */
    float      tick_max_base;      /* native twin of tick_max */
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
    /* Per-image styling (track + fill): opacity, recolor tint, blend/overlay mode.
     * blend: 0 normal, 1 additive, 2 subtractive, 3 multiply (maps to LV_BLEND_MODE_*) */
    uint8_t       arc_image_opa;           /* default 255 */
    lv_color_t    arc_image_recolor;       /* tint colour */
    uint8_t       arc_image_recolor_opa;   /* default 0 = off */
    uint8_t       arc_image_blend;         /* default 0 */
    uint8_t       arc_image_full_opa;          /* default 255 */
    lv_color_t    arc_image_full_recolor;
    uint8_t       arc_image_full_recolor_opa;  /* default 0 = off */
    uint8_t       arc_image_full_blend;        /* default 0 */
    /* Radial fill reveal: uncover the fill image AROUND the ring (angle mask)
     * instead of the default horizontal left-to-right clip. For circular gauges. */
    bool          arc_image_radial;            /* default false */
    lv_draw_mask_angle_param_t reveal_mask;    /* runtime: radial reveal mask */
    int16_t       reveal_mask_id;              /* runtime */
    int16_t       reveal_angle;                /* runtime: current fill end angle (mod 360) */
    int16_t       reveal_raw;                  /* runtime: prev fill angle, sweep order, for delta invalidation */

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

    /* ── Color alerts — recolor the moving fill below arc_low (low_color) or
     *   above arc_high (high_color). Mirrors the bar widget's alert system.
     *   Thresholds are owned by the bound CHANNEL (low_warn/high_warn) and are
     *   NOT persisted on the widget (omitted from to_json); colours ARE widget-
     *   owned styling and round-trip. Applied in _arc_apply_fill_color with
     *   precedence rule > limiter > alerts > redline > normal. */
    bool       arc_alerts_enabled;      /* default: false — master toggle */
    float      arc_low;                 /* default: 0   — low alert threshold (from channel.low_warn) */
    float      arc_high;                /* default: 100 — high alert threshold (from channel.high_warn) */
    lv_color_t arc_low_color;           /* default: 0x0000FF */
    lv_color_t arc_high_color;          /* default: 0xFF0000 */

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
    /* A channel-bound arc with no authored suffix labels itself with the
     * channel's DISPLAY unit, so the unit printed next to the number is the
     * unit the number is actually in. A suffix the layout or the user supplied
     * is a per-widget override and wins (that's what this flag records) — an
     * arc reading "7.2" with "x1000 RPM" underneath keeps saying that. */
    bool       value_unit_from_layout;
    lv_obj_t  *value_label;             /* runtime: the center label */

    /* ── Ticks (meter-parity) — rendered via an OVERLAY lv_meter sibling.
     * lv_arc has no tick API in LVGL v8, but lv_meter does, so when
     * show_ticks (or show_value_line) is set we drop a transparent
     * lv_meter behind the arc fill that draws the tick ring. The meter's
     * scale spans signal_min..signal_max over the SAME angle span the arc
     * fill uses (start_angle/end_angle), so ticks line up with the fill.
     * Default OFF so existing layouts pay no overhead. STANDARD mode only —
     * the overlay is never created in image modes. */
    bool       show_ticks;              /* default: false */
    /* Tick spacing source of truth = STEP in value units (range-independent, so
     * it survives signal_min/max changes). When *_step > 0 the overlay derives
     * tick counts from the step over the active range (tick window if set, else
     * signal range), which keeps minor/medium/major aligned and anchors majors
     * to round values. *_step == 0 falls back to the legacy count fields below
     * (older layouts). minor_tick_count/major_tick_every/mid_tick_count are
     * runtime-derived when steps are used. */
    uint16_t   minor_tick_step;         /* value interval, 0 = use count */
    uint16_t   major_tick_step;         /* value interval for major ticks + labels */
    uint16_t   mid_tick_step;           /* value interval for medium ticks, 0 = off */
    /* Native-unit twins of the three steps, on the same base/render split as the
     * value-axis fields above — the counts are derived as span/step and the span
     * is in display units, so a native step would blow the tick density apart
     * (a 50 kPa step on a 0..36 psi dial leaves one tick). Held as float because
     * the rendered uint16 rounds: keeping the base exact is what lets a typed
     * step round-trip instead of eroding a little on every reload. Steps are
     * INTERVALS, so they convert with _arc_delta_to_display (multiplicative part
     * only — a 10 °C step is 18 °F, not 50). 0 stays 0 = tier off. */
    float      minor_tick_step_base;
    float      major_tick_step_base;
    float      mid_tick_step_base;
    uint8_t    minor_tick_count;        /* default: 21 (legacy / runtime) */
    uint8_t    major_tick_every;        /* default: 5  (legacy / runtime) */
    uint8_t    minor_tick_length;       /* default: 10 */
    uint8_t    minor_tick_width;        /* default: 2  */
    uint8_t    major_tick_length;       /* default: 15 */
    uint8_t    major_tick_width;        /* default: 4  */
    lv_color_t minor_tick_color;        /* default: 0x9E9E9E */
    lv_color_t major_tick_color;        /* default: 0xFFFFFF */
    /* ── Optional 3rd (medium) tick tier — e.g. a mark every 500 between the
     * minor (250) and major (1000) ticks. Drawn as a SECOND lv_meter scale on
     * the overlay so it composes with the minor/major scale. Disabled when
     * mid_tick_count < 2 (default). */
    uint8_t    mid_tick_count;          /* default: 0 (disabled) — total medium ticks across the range */
    uint8_t    mid_tick_length;         /* default: 13 */
    uint8_t    mid_tick_width;          /* default: 2  */
    lv_color_t mid_tick_color;          /* default: 0xBDBDBD */
    /* ── Per-tick IMAGES — stamp a rotated (+scaled) image at each tick instead
     * of the drawn line, one design per tier. Empty = drawn line for that tier.
     * Authored pointing OUTWARD (tip toward the rim); rotated to each tick's
     * angle, centred on it. Stamped in _arc_tick_draw_cb → baked into the
     * static-tick overlay snapshot, so runtime cost is zero. */
    char       major_tick_image_name[32];
    char       mid_tick_image_name[32];
    char       minor_tick_image_name[32];
    /* Per-tier image modifiers (scale %, opacity, recolour tint + its strength,
     * radial offset px +=outward). Default = identity (100/255/—/0/0). The old
     * single `tick_image_scale` seeds all three scales on load (back-compat). */
    uint16_t   major_tick_image_scale, mid_tick_image_scale, minor_tick_image_scale;
    uint8_t    major_tick_image_opa, mid_tick_image_opa, minor_tick_image_opa;
    lv_color_t major_tick_image_recolor, mid_tick_image_recolor, minor_tick_image_recolor;
    uint8_t    major_tick_image_recolor_opa, mid_tick_image_recolor_opa, minor_tick_image_recolor_opa;
    int16_t    major_tick_image_offset, mid_tick_image_offset, minor_tick_image_offset;
    /* DRAWN-tick outline/glow (procedural "pop" — no baked image needed). Drawn
     * BEHIND the tick line: a tight border that softens into a glow as `fade`
     * grows. strength 0 = off. Applies to tiers with no tick image. */
    lv_color_t tick_outline_color;      /* default black */
    uint8_t    tick_outline_strength;   /* 0..255, 0 = off */
    uint8_t    tick_outline_fade;       /* 0..20 glow spread px (0 = hard outline) */
    lv_img_dsc_t      *major_tick_img_dsc;   /* runtime */
    lv_img_dsc_t      *mid_tick_img_dsc;     /* runtime */
    lv_img_dsc_t      *minor_tick_img_dsc;   /* runtime */
    lv_meter_scale_t  *tick_mid_scale;       /* runtime: overlay medium scale (or NULL) */
    uint8_t            tick_major_nth;       /* runtime: every-Nth-is-major on the overlay */

    /* ── Numeric tick labels (meter-parity). Drawn by the OVERLAY lv_meter at
     * its major ticks. lv_meter bakes the label colour/font into LV_PART_TICKS
     * at create time, so a label-colour night override rebuilds the overlay
     * (same as the tick-mark colours). label_gap is the major-tick→label
     * distance passed to lv_meter_set_scale_major_ticks. The relabel hook
     * (_arc_tick_draw_cb) rewrites the major-tick text using tick_label_divisor
     * + value_decimals (REUSED from the value-text overlay) and the same
     * anchor/reverse warp the fill uses. Default ON (matches the meter), so
     * an arc that enables ticks gets labels unless they're explicitly hidden. */
    bool       show_tick_labels;        /* default: true */
    /* Tick window: when tick_max > tick_min, ticks + labels only draw for
     * values within [tick_min, tick_max]; the ring + fill still span the full
     * signal_min..signal_max sweep. Lets a scale extend past the meaningful
     * range (e.g. a negative signal_min that raises where 0 sits) without
     * showing ticks below the start or above the end. Default 0/0 = off. */
    float      tick_min;                /* default: 0 */
    float      tick_max;                /* default: 0 (<= tick_min → window off) */
    int16_t    label_gap;               /* default: 10  (range −150..+150) */
    char       tick_label_font[32];     /* default: "" — empty = LVGL default */
    lv_color_t tick_label_color;        /* default: 0xFFFFFF */
    uint16_t   tick_label_divisor;      /* default: 1 — divides displayed value */
    /* Draw the tick ring + labels (and value-line) ON TOP of the arc track and
     * fill instead of behind them. Default TRUE so the fill never paints over
     * the marks/numbers (the conventional gauge look). Set false to restore the
     * historical behind-the-fill z-order, e.g. when the fill should dominate a
     * decorative tick ring. */
    bool       ticks_on_top;            /* default: true */

    /* ── Value line (needle) — reuses the SAME overlay lv_meter. When set
     * (even if show_ticks is off) the overlay meter is created and an
     * lv_meter needle-line indicator is added; it's driven wherever the
     * arc recomputes its value (anchor + reverse applied first, same as
     * the meter). Default OFF. */
    bool       show_value_line;         /* default: false */
    uint8_t    value_line_width;        /* default: 4 */
    lv_color_t value_line_color;        /* default: 0xFFFFFF */
    int16_t    value_line_r_mod;        /* default: -10 (radius offset, like meter needle_r_mod) */

    /* Overlay lv_meter runtime handles (STANDARD mode only). The meter is a
     * child of the arc container, drawn UNDER the arc fill so the fill sits
     * on top. Freed by the w->root delete cascade. */
    lv_obj_t             *tick_meter;   /* runtime: overlay lv_meter, or NULL */
    lv_meter_scale_t     *tick_scale;   /* runtime: its scale */
    lv_meter_indicator_t *value_needle; /* runtime: the value-line needle, or NULL */
    /* Static-tick snapshot (STANDARD mode, show_ticks on). The tick ring +
     * labels render ONCE into a tight sector-bbox image (NOT the full widget
     * bbox — a 600 px arc's ring sector is ~100 KB vs ~1 MB, two of which
     * would not even fit in PSRAM) and the live overlay's tick rendering is
     * stripped, so redraws crossing the ring blit cached pixels instead of
     * re-running tick math + glyph rendering. tick_img is a child of the
     * container (freed by the delete cascade); the dsc + pixel buffer are
     * lv_mem allocations freed by _arc_free_tick_snapshot. */
    lv_obj_t             *tick_img;     /* runtime: baked tick ring, or NULL */
    lv_img_dsc_t         *tick_img_dsc; /* runtime: its image descriptor */

    /* ── Anchor curve (data) — piecewise-linear value mapping, ported from
     * widget_meter. Applied to the value BEFORE pct is computed (fill,
     * image-clip, and value-needle drive all go through it). */
    bool       anchor_enabled;          /* default: false */
    float      anchor_value;            /* default: 50 */
    uint8_t    anchor_position;         /* default: 50 — percent of sweep */

    /* ── Reverse (data) — flip the value axis. Applied AFTER anchor (same
     * order as the meter): v = signal_min + signal_max - v. Also folded into
     * _value_to_angle so the redline marker lands at the right end. */
    bool       reverse;                 /* default: false */

    /* Cached current value (DISPLAY units) — used by redraws (resize, night
     * swap) so they don't have to wait for the next signal tick to look right. */
    float      _cached_value;
    /* The same reading in NATIVE units, as it arrived from the signal registry.
     * Held so a display-unit change can re-render from the true reading (and so
     * the value→label map, which is keyed on raw signal values, still resolves)
     * instead of re-converting _cached_value out of a unit we no longer know. */
    float      _cached_native;

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
    /* Optional value smoothing (eases the fill/value-line at refresh rate). */
    widget_smooth_t smooth;
} arc_data_t;

widget_t *widget_arc_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
