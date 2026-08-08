/* test_arc_display_units.c — widget_arc value axis vs. channel display unit.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * A channel stores its value, range and thresholds in a NATIVE unit (kPa for
 * manifold_pressure) and renders in a DISPLAY unit the user picks (psi).
 * widget_arc ignored that choice entirely — there was not one unit_convert call
 * in widget_arc.c — so "Display as: psi" on a kPa channel was a silent no-op:
 *
 *     _arc_on_signal          → fed the RAW native value to the fill
 *     _arc_on_channel_changed → signal_min/max, redline_threshold, arc_low/high
 *                               all copied RAW from the channel
 *     _arc_update_value_label → printed the RAW native number next to a
 *                               widget-owned free-text unit suffix
 *
 * Everything was consistently native, so the fill LOOKED right — until you sat
 * a psi-labelled arc next to a meter on the same channel and they disagreed, or
 * read the value text and got a kPa number with "psi" after it.
 *
 * The fix keeps each authored quantity in a native-unit base (range_min_base,
 * range_max_base, redline_base, limiter_base, anchor_base, tick_min/max_base)
 * and always derives the rendered field from it, so scale, fill, redline,
 * limiter, alerts, tick window and value text share one unit.
 *
 * A SECOND, pre-existing bug is covered here too: _arc_from_json tracked
 * sig_min_explicit / sig_max_explicit so an explicit layout signal_min/max
 * overrode the channel range — and then _arc_on_channel_changed overwrote
 * d->signal_min/max unconditionally on the first channel-changed notify, losing
 * the override at runtime. range_from_layout (persisted past from_json, same as
 * widget_meter) is what holds the pin.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * Same "mirror the firmware logic verbatim" approach as
 * test_meter_display_units.c — the real functions write to LVGL, which would
 * drag in the graphics stack. The CONVERSION itself is not mirrored: this links
 * main/data/unit_convert.c straight out of the firmware, so the factors under
 * test are the shipping ones.
 *
 * Source-of-truth references (must stay in lockstep):
 *
 *   main/widgets/widget_arc.c, _arc_to_display() / _arc_to_native()
 *   main/widgets/widget_arc.c, _arc_sync_units()
 *   main/widgets/widget_arc.c, _arc_sync_alert_thresholds()
 *   main/widgets/widget_arc.c, _arc_set_range_display()
 *   main/widgets/widget_arc.c, _arc_on_signal()          (value convert)
 *   main/widgets/widget_arc.c, _arc_on_channel_changed() (redline adopt + pin)
 *   main/widgets/widget_arc.c, _arc_to_json / _arc_from_json (base persistence)
 */
#include "unity.h"
#include "unit_convert.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Firmware sentinels (canonical_channels.h) — an unset threshold. */
#define CHANNEL_THRESHOLD_UNSET_LOW   (-1e9f)
#define CHANNEL_THRESHOLD_UNSET_HIGH  (1e9f)

/* Factory defaults (widget_arc.c). to_json is defaults-only, so these decide
 * what a save actually writes. */
#define ARC_DEFAULT_SIG_MIN      0.0f
#define ARC_DEFAULT_SIG_MAX    100.0f
#define ARC_DEFAULT_REDLINE     80.0f
#define ARC_DEFAULT_LIMITER_VAL 90.0f
#define ARC_DEFAULT_ANCHOR_VAL  50.0f

/* ── Mirrors ─────────────────────────────────────────────────────────────
 * Subsets of channel_t / arc_data_t. Field names match the firmware structs
 * one-for-one so the mirror reads like the original. */
typedef struct {
    const char *units_native;
    const char *units_display;
    float       min;
    float       max;
    float       low_warn;
    float       high_warn;
} chan_mirror_t;

typedef struct {
    const chan_mirror_t *channel;
    /* Rendered — DISPLAY units. */
    float signal_min, signal_max;
    float redline_threshold;
    float limiter_value;
    float anchor_value;
    float tick_min, tick_max;
    float arc_low, arc_high;
    uint16_t minor_tick_step, major_tick_step, mid_tick_step;
    /* Authored — NATIVE units. */
    bool  range_from_layout;
    float range_min_base, range_max_base;
    float redline_base;
    float limiter_base;
    float anchor_base;
    float tick_min_base, tick_max_base;
    float minor_tick_step_base, major_tick_step_base, mid_tick_step_base;
    bool  redline_enabled;
    /* Value text. */
    char  value_unit[16];
    bool  value_unit_from_layout;
} arc_mirror_t;

/* Verbatim port of _arc_to_display(). */
static float arc_to_display(const arc_mirror_t *d, float native) {
    const chan_mirror_t *c = d->channel;
    if (!c) return native;
    return unit_convert(native, c->units_native, c->units_display);
}

/* Verbatim port of _arc_to_native(). */
static float arc_to_native(const arc_mirror_t *d, float display) {
    const chan_mirror_t *c = d->channel;
    if (!c) return display;
    return unit_convert(display, c->units_display, c->units_native);
}

/* Verbatim port of _arc_delta_to_display() / _arc_delta_to_native(). */
static float arc_delta_to_display(const arc_mirror_t *d, float native_delta) {
    const chan_mirror_t *c = d->channel;
    if (!c) return native_delta;
    float zero = unit_convert(0.0f, c->units_native, c->units_display);
    return unit_convert(native_delta, c->units_native, c->units_display) - zero;
}

static float arc_delta_to_native(const arc_mirror_t *d, float display_delta) {
    const chan_mirror_t *c = d->channel;
    if (!c) return display_delta;
    float zero = unit_convert(0.0f, c->units_display, c->units_native);
    return unit_convert(display_delta, c->units_display, c->units_native) - zero;
}

/* Verbatim port of _arc_step_render(). */
static uint16_t arc_step_render(float display_step) {
    if (display_step <= 0.0f) return 0;
    long v = lroundf(display_step);
    if (v < 1)     v = 1;
    if (v > 65535) v = 65535;
    return (uint16_t)v;
}

/* Verbatim port of _arc_sync_units(). */
static void arc_sync_units(arc_mirror_t *d) {
    const chan_mirror_t *c = d->channel;
    if (c && !d->range_from_layout) {
        d->range_min_base = c->min;
        d->range_max_base = c->max;
    }
    d->signal_min        = arc_to_display(d, d->range_min_base);
    d->signal_max        = arc_to_display(d, d->range_max_base);
    d->redline_threshold = arc_to_display(d, d->redline_base);
    d->limiter_value     = arc_to_display(d, d->limiter_base);
    d->anchor_value      = arc_to_display(d, d->anchor_base);
    if (d->tick_max_base > d->tick_min_base) {
        d->tick_min = arc_to_display(d, d->tick_min_base);
        d->tick_max = arc_to_display(d, d->tick_max_base);
    } else {
        d->tick_min = d->tick_min_base;
        d->tick_max = d->tick_max_base;
    }
    d->minor_tick_step = arc_step_render(arc_delta_to_display(d, d->minor_tick_step_base));
    d->major_tick_step = arc_step_render(arc_delta_to_display(d, d->major_tick_step_base));
    d->mid_tick_step   = arc_step_render(arc_delta_to_display(d, d->mid_tick_step_base));
}

/* Verbatim port of _arc_sync_alert_thresholds(). */
static void arc_sync_alert_thresholds(arc_mirror_t *d) {
    const chan_mirror_t *c = d->channel;
    if (!c) return;
    d->arc_low  = (c->low_warn  != CHANNEL_THRESHOLD_UNSET_LOW)
                      ? arc_to_display(d, c->low_warn)  : d->signal_min;
    d->arc_high = (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
                      ? arc_to_display(d, c->high_warn) : d->signal_max;
}

/* Verbatim port of _arc_set_range_display() — Widget settings write path. */
static void arc_set_range_display(arc_mirror_t *d, float dmin, float dmax) {
    d->range_min_base = arc_to_native(d, dmin);
    d->range_max_base = arc_to_native(d, dmax);
    d->range_from_layout = (d->range_min_base != ARC_DEFAULT_SIG_MIN ||
                            d->range_max_base != ARC_DEFAULT_SIG_MAX);
    arc_sync_units(d);
}

/* The channel-adopting half of _arc_on_channel_changed(). */
static void arc_on_channel_changed(arc_mirror_t *d) {
    const chan_mirror_t *c = d->channel;
    if (!c) return;
    if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
        d->redline_enabled = true;
        d->redline_base = c->high_warn;
    } else {
        d->redline_enabled = false;
    }
    arc_sync_units(d);
    arc_sync_alert_thresholds(d);
    if (!d->value_unit_from_layout)
        snprintf(d->value_unit, sizeof(d->value_unit), "%s", c->units_display);
}

/* The conversion half of _arc_on_signal(): the registry hands over the native
 * reading, the arc converts once and everything downstream uses that. */
static float arc_signal_in(const arc_mirror_t *d, float native) {
    return arc_to_display(d, native);
}

/* Where the fill sits along the sweep, 0.0 = empty, 1.0 = full. Mirrors
 * _arc_fill_value + the pct math in _update_arc_value (anchor off). */
static float fill_fraction(const arc_mirror_t *d, float native) {
    float v = arc_signal_in(d, native);
    if (v < d->signal_min) v = d->signal_min;
    if (v > d->signal_max) v = d->signal_max;
    float span = d->signal_max - d->signal_min;
    if (span <= 0.0f) return 0.0f;
    return (v - d->signal_min) / span;
}

/* ── Save / reload ───────────────────────────────────────────────────────
 * to_json is defaults-only AND override-only: it emits the NATIVE bases, and
 * emits the scale only when the arc actually pinned one. from_json reads any
 * present signal_min/max back as an explicit pin. */
typedef struct {
    bool  has_signal_min, has_signal_max;
    float signal_min, signal_max;      /* native */
    bool  has_redline;
    float redline_threshold;           /* native */
    bool  has_limiter;
    float limiter_value;               /* native */
    bool  has_tick_window;
    float tick_min, tick_max;          /* native */
    bool  has_anchor;
    float anchor_value;                /* native */
    bool  has_steps;
    float minor_tick_step, major_tick_step;   /* native */
    bool  has_mid_step;
    float mid_tick_step;               /* native */
    bool  has_value_unit;
    char  value_unit[16];
} arc_json_t;

static arc_json_t arc_to_json(const arc_mirror_t *d) {
    arc_json_t j = {0};
    bool pin_range = d->range_from_layout || !d->channel;
    if (pin_range && d->range_min_base != ARC_DEFAULT_SIG_MIN) {
        j.has_signal_min = true; j.signal_min = d->range_min_base;
    }
    if (pin_range && d->range_max_base != ARC_DEFAULT_SIG_MAX) {
        j.has_signal_max = true; j.signal_max = d->range_max_base;
    }
    if (d->redline_base != ARC_DEFAULT_REDLINE) {
        j.has_redline = true; j.redline_threshold = d->redline_base;
    }
    if (d->limiter_base != ARC_DEFAULT_LIMITER_VAL) {
        j.has_limiter = true; j.limiter_value = d->limiter_base;
    }
    if (d->tick_max_base > d->tick_min_base) {
        j.has_tick_window = true;
        j.tick_min = d->tick_min_base; j.tick_max = d->tick_max_base;
    }
    if (d->anchor_base != ARC_DEFAULT_ANCHOR_VAL) {
        j.has_anchor = true; j.anchor_value = d->anchor_base;
    }
    if (d->minor_tick_step > 0) {
        j.has_steps = true;
        j.minor_tick_step = d->minor_tick_step_base;
        j.major_tick_step = (d->major_tick_step > 0) ? d->major_tick_step_base : 0.0f;
    }
    if (d->mid_tick_step > 0) {
        j.has_mid_step = true; j.mid_tick_step = d->mid_tick_step_base;
    }
    if (d->value_unit[0] &&
        (!d->channel || strcmp(d->value_unit, d->channel->units_display) != 0)) {
        j.has_value_unit = true;
        snprintf(j.value_unit, sizeof(j.value_unit), "%s", d->value_unit);
    }
    return j;
}

/* Verbatim port of the range/threshold half of _arc_from_json(), including the
 * unconditional sync + alert adopt + unit adopt at the end. */
static arc_mirror_t arc_from_json(const arc_json_t *j, const chan_mirror_t *c) {
    arc_mirror_t d = {0};
    /* factory defaults */
    d.range_min_base = ARC_DEFAULT_SIG_MIN;
    d.range_max_base = ARC_DEFAULT_SIG_MAX;
    d.redline_base   = ARC_DEFAULT_REDLINE;
    d.limiter_base   = ARC_DEFAULT_LIMITER_VAL;
    d.anchor_base    = ARC_DEFAULT_ANCHOR_VAL;
    d.tick_min_base  = 0.0f;
    d.tick_max_base  = 0.0f;

    bool sig_min_explicit = false, sig_max_explicit = false;
    if (j->has_signal_min) { d.range_min_base = j->signal_min; sig_min_explicit = true; }
    if (j->has_signal_max) { d.range_max_base = j->signal_max; sig_max_explicit = true; }
    d.range_from_layout = sig_min_explicit || sig_max_explicit;
    if (j->has_redline)     d.redline_base = j->redline_threshold;
    if (j->has_limiter)     d.limiter_base = j->limiter_value;
    if (j->has_tick_window) { d.tick_min_base = j->tick_min; d.tick_max_base = j->tick_max; }
    if (j->has_anchor)      d.anchor_base = j->anchor_value;
    if (j->has_steps) {
        d.minor_tick_step_base = j->minor_tick_step;
        d.major_tick_step_base = j->major_tick_step;
    }
    if (j->has_mid_step)    d.mid_tick_step_base = j->mid_tick_step;
    if (j->has_value_unit) {
        snprintf(d.value_unit, sizeof(d.value_unit), "%s", j->value_unit);
        d.value_unit_from_layout = true;
    }

    d.channel = c;
    if (c) {
        if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
            d.redline_enabled = true;
            d.redline_base = c->high_warn;
        } else {
            d.redline_enabled = false;
        }
    }
    arc_sync_units(&d);
    arc_sync_alert_thresholds(&d);
    if (c && !d.value_unit_from_layout)
        snprintf(d.value_unit, sizeof(d.value_unit), "%s", c->units_display);
    return d;
}

/* ── Fixtures ────────────────────────────────────────────────────────────
 * manifold_pressure as canonical_channels.c defines it, with the display unit
 * switched to psi — exactly the edit that surfaced the bug. */
#define MAP_IDLE_KPA  30.0f
#define MAP_WOT_KPA  101.0f

static chan_mirror_t map_in(const char *display) {
    chan_mirror_t c = {
        .units_native = "kPa", .units_display = display,
        .min = 0.0f, .max = 250.0f,
        .low_warn = CHANNEL_THRESHOLD_UNSET_LOW, .high_warn = 200.0f,
    };
    return c;
}

/* An authored MAP arc: the layout pinned the dial to 0..250 (kPa). */
static arc_mirror_t pinned_arc(const chan_mirror_t *c) {
    arc_mirror_t d = {0};
    d.channel        = c;
    d.range_min_base = 0.0f;
    d.range_max_base = 250.0f;
    d.range_from_layout = true;
    d.redline_base   = ARC_DEFAULT_REDLINE;
    d.limiter_base   = ARC_DEFAULT_LIMITER_VAL;
    d.anchor_base    = ARC_DEFAULT_ANCHOR_VAL;
    arc_sync_units(&d);
    return d;
}

/* ── The regression: the scale follows the display unit ──────────────────── */

static void test_pinned_scale_follows_display_unit(void) {
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);

    /* 0..250 kPa is 0..36.26 psi — the dial re-expresses itself. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, d.signal_min);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.26f, d.signal_max);
}

static void test_fill_lands_in_the_same_place_in_either_unit(void) {
    /* Switching the display unit relabels the gauge; it must not move the fill.
     * Before the fix the value was native and the scale was native too, so this
     * held by accident — it now has to hold because BOTH converted. */
    chan_mirror_t kpa = map_in("kPa");
    chan_mirror_t psi = map_in("psi");
    arc_mirror_t in_kpa = pinned_arc(&kpa);
    arc_mirror_t in_psi = pinned_arc(&psi);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, fill_fraction(&in_kpa, MAP_WOT_KPA),
                                     fill_fraction(&in_psi, MAP_WOT_KPA));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, fill_fraction(&in_kpa, MAP_IDLE_KPA),
                                     fill_fraction(&in_psi, MAP_IDLE_KPA));
    /* And it's the physically right spot: 101/250 of the sweep. */
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.404f, fill_fraction(&in_psi, MAP_WOT_KPA));
}

static void test_value_text_number_matches_its_unit(void) {
    /* The user-visible symptom that a ratio-preserving fill hides: the value
     * text printed the native number and the suffix said psi. */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    arc_on_channel_changed(&d);

    TEST_ASSERT_EQUAL_STRING("psi", d.value_unit);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 14.65f, arc_signal_in(&d, MAP_WOT_KPA));
}

static void test_redline_shares_the_scale_unit(void) {
    /* 200 kPa high_warn is 29 psi — it has to land 80% up the dial, not near
     * the bottom stop the way it did against an unconverted kPa scale. */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    arc_on_channel_changed(&d);

    TEST_ASSERT_TRUE(d.redline_enabled);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.01f, d.redline_threshold);
    float frac = (d.redline_threshold - d.signal_min) / (d.signal_max - d.signal_min);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.8f, frac);
}

static void test_alert_thresholds_share_the_scale_unit(void) {
    chan_mirror_t c = map_in("psi");
    c.low_warn = 50.0f;                 /* 50 kPa = 7.25 psi */
    arc_mirror_t d = pinned_arc(&c);
    arc_on_channel_changed(&d);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.25f, d.arc_low);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.01f, d.arc_high);

    /* The buckets they define have to agree with the converted value: 30 kPa is
     * below the low warn, 101 kPa is between, 210 kPa is above the high. */
    TEST_ASSERT_TRUE(arc_signal_in(&d, MAP_IDLE_KPA) <= d.arc_low);
    TEST_ASSERT_TRUE(arc_signal_in(&d, MAP_WOT_KPA)  >  d.arc_low);
    TEST_ASSERT_TRUE(arc_signal_in(&d, MAP_WOT_KPA)  <  d.arc_high);
    TEST_ASSERT_TRUE(arc_signal_in(&d, 210.0f)       >= d.arc_high);
}

static void test_unset_alert_side_parks_at_the_converted_range_edge(void) {
    /* A side with no channel warn parks at the range edge so it can never fire.
     * That edge is now in display units — parking at the NATIVE min on a psi
     * scale would leave arc_low sitting inside the visible range. */
    chan_mirror_t c = map_in("psi");
    c.low_warn  = CHANNEL_THRESHOLD_UNSET_LOW;
    c.high_warn = CHANNEL_THRESHOLD_UNSET_HIGH;
    arc_mirror_t d = pinned_arc(&c);
    arc_on_channel_changed(&d);

    TEST_ASSERT_EQUAL_FLOAT(d.signal_min, d.arc_low);
    TEST_ASSERT_EQUAL_FLOAT(d.signal_max, d.arc_high);
    TEST_ASSERT_FALSE(d.redline_enabled);
}

static void test_limiter_and_anchor_share_the_scale_unit(void) {
    /* Both are compared against the value / laid out on the scale, so leaving
     * them native while the scale moved would put the limiter off the end of a
     * psi dial (90 psi on a 0..36 gauge = never fires). */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    d.limiter_base = 200.0f;            /* 200 kPa */
    d.anchor_base  = 100.0f;            /* 100 kPa */
    arc_sync_units(&d);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.01f, d.limiter_value);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 14.50f, d.anchor_value);
    /* Both inside the sweep, which is the point. */
    TEST_ASSERT_TRUE(d.limiter_value < d.signal_max);
    TEST_ASSERT_TRUE(d.anchor_value  > d.signal_min);
    TEST_ASSERT_TRUE(d.anchor_value  < d.signal_max);
}

static void test_tick_window_converts_when_live(void) {
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    d.tick_min_base = 50.0f;
    d.tick_max_base = 200.0f;
    arc_sync_units(&d);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.25f, d.tick_min);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.01f, d.tick_max);
    TEST_ASSERT_TRUE(d.tick_max > d.tick_min);   /* still a live window */
}

static void test_disabled_tick_window_is_not_converted_into_existence(void) {
    /* 0/0 means "window off". °C→°F would map it to 32/32 — still off, but only
     * by luck; a min/max pair either side of the offset could come out live. */
    chan_mirror_t c = { .units_native = "\xC2\xB0" "C", .units_display = "\xC2\xB0" "F",
                        .min = 0.0f, .max = 120.0f,
                        .low_warn = CHANNEL_THRESHOLD_UNSET_LOW, .high_warn = 110.0f };
    arc_mirror_t d = {0};
    d.channel = &c;
    d.range_min_base = 0.0f; d.range_max_base = 120.0f;
    d.range_from_layout = true;
    arc_sync_units(&d);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.tick_min);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.tick_max);
    TEST_ASSERT_FALSE(d.tick_max > d.tick_min);
}

/* ── Tick spacing is an INTERVAL, not a point ────────────────────────────── */

static void test_tick_step_converts_as_an_interval(void) {
    /* Tick counts are derived as span/step and the span is now in display units,
     * so a native step wrecks the density: 50 kPa against a 0..36.26 psi dial
     * would leave a single tick. */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    d.minor_tick_step_base = 50.0f;      /* 50 kPa */
    d.major_tick_step_base = 100.0f;
    arc_sync_units(&d);

    TEST_ASSERT_EQUAL_UINT(7, d.minor_tick_step);    /* 7.25 psi */
    TEST_ASSERT_EQUAL_UINT(15, d.major_tick_step);   /* 14.50 psi */
    /* Still a handful of minor ticks across the sweep, as authored — not the
     * single tick a native 50 would have produced on a 36 psi dial. */
    int minors = (int)lroundf((d.signal_max - d.signal_min) / (float)d.minor_tick_step);
    TEST_ASSERT_TRUE(minors >= 4 && minors <= 6);
}

static void test_tick_step_takes_the_scale_not_the_offset(void) {
    /* THE reason steps need their own conversion: a 10 °C interval is 18 °F, not
     * 50 °F. Converting a step as if it were a point on the axis would add 32. */
    chan_mirror_t c = { .units_native = "\xC2\xB0" "C", .units_display = "\xC2\xB0" "F",
                        .min = 0.0f, .max = 120.0f,
                        .low_warn = CHANNEL_THRESHOLD_UNSET_LOW, .high_warn = 110.0f };
    arc_mirror_t d = {0};
    d.channel = &c;
    d.range_min_base = 0.0f; d.range_max_base = 120.0f;
    d.range_from_layout = true;
    d.minor_tick_step_base = 10.0f;
    arc_sync_units(&d);

    TEST_ASSERT_EQUAL_UINT(18, d.minor_tick_step);
    /* 0..120 °C in 10 °C steps is 12 intervals; 32..248 °F in 18 °F steps is
     * also 12 — the tick ring is unchanged, only relabelled. */
    int intervals = (int)lroundf((d.signal_max - d.signal_min) / (float)d.minor_tick_step);
    TEST_ASSERT_EQUAL_INT(12, intervals);
}

static void test_zero_tick_step_stays_off(void) {
    /* 0 is the "use the legacy count fields" / "tier off" sentinel — no
     * conversion may promote it to a live step. */
    chan_mirror_t c = { .units_native = "\xC2\xB0" "C", .units_display = "\xC2\xB0" "F",
                        .min = 0.0f, .max = 120.0f,
                        .low_warn = CHANNEL_THRESHOLD_UNSET_LOW, .high_warn = 110.0f };
    arc_mirror_t d = {0};
    d.channel = &c;
    d.range_min_base = 0.0f; d.range_max_base = 120.0f;
    arc_sync_units(&d);

    TEST_ASSERT_EQUAL_UINT(0, d.minor_tick_step);
    TEST_ASSERT_EQUAL_UINT(0, d.major_tick_step);
    TEST_ASSERT_EQUAL_UINT(0, d.mid_tick_step);
}

static void test_tick_step_survives_save_reload(void) {
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    d.minor_tick_step_base = 50.0f;
    d.major_tick_step_base = 100.0f;
    d.mid_tick_step_base   = 25.0f;
    arc_sync_units(&d);

    arc_json_t j = arc_to_json(&d);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, j.minor_tick_step);   /* native on disk */
    TEST_ASSERT_EQUAL_FLOAT(25.0f, j.mid_tick_step);

    arc_mirror_t back = arc_from_json(&j, &c);
    TEST_ASSERT_EQUAL_UINT(d.minor_tick_step, back.minor_tick_step);
    TEST_ASSERT_EQUAL_UINT(d.major_tick_step, back.major_tick_step);
    TEST_ASSERT_EQUAL_UINT(d.mid_tick_step,   back.mid_tick_step);
}

static void test_typed_tick_step_round_trips(void) {
    /* Typed in display units → native base → rendered back. The base stays float
     * precisely so the uint16 render rounds back to what was typed instead of
     * eroding on each reload. */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);

    d.minor_tick_step_base = arc_delta_to_native(&d, 8.0f);   /* "every 8 psi" */
    arc_sync_units(&d);
    TEST_ASSERT_EQUAL_UINT(8, d.minor_tick_step);

    for (int i = 0; i < 5; i++) {
        arc_json_t j = arc_to_json(&d);
        d = arc_from_json(&j, &c);
    }
    TEST_ASSERT_EQUAL_UINT(8, d.minor_tick_step);
}

/* ── The pin: an explicit layout scale survives the channel notify ───────── */

static void test_explicit_scale_survives_channel_changed(void) {
    /* THE pre-existing bug. from_json honoured the explicit signal_min/max, then
     * the first channel-changed notify overwrote both from c->min/c->max — so a
     * tacho authored to start at -500 snapped back to the channel's 0 the moment
     * anything touched the channel. */
    chan_mirror_t c = map_in("kPa");
    arc_json_t j = { .has_signal_min = true, .signal_min = -50.0f,
                     .has_signal_max = true, .signal_max = 300.0f };
    arc_mirror_t d = arc_from_json(&j, &c);

    TEST_ASSERT_TRUE(d.range_from_layout);
    TEST_ASSERT_EQUAL_FLOAT(-50.0f, d.signal_min);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, d.signal_max);

    arc_on_channel_changed(&d);          /* used to clobber both */

    TEST_ASSERT_EQUAL_FLOAT(-50.0f, d.signal_min);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, d.signal_max);
}

static void test_explicit_scale_survives_repeated_notifies(void) {
    chan_mirror_t c = map_in("psi");
    arc_json_t j = { .has_signal_max = true, .signal_max = 300.0f };
    arc_mirror_t d = arc_from_json(&j, &c);

    for (int i = 0; i < 10; i++) arc_on_channel_changed(&d);

    TEST_ASSERT_EQUAL_FLOAT(300.0f, d.range_max_base);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 43.51f, d.signal_max);
    /* Only one side was explicit; the other keeps the factory default rather
     * than being adopted from the channel. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.range_min_base);
}

static void test_unpinned_arc_still_adopts_the_channel_range(void) {
    chan_mirror_t c = map_in("psi");
    arc_json_t j = {0};                  /* no signal_min/max in the layout */
    arc_mirror_t d = arc_from_json(&j, &c);

    TEST_ASSERT_FALSE(d.range_from_layout);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, d.range_max_base);   /* base follows channel */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.26f, d.signal_max);

    /* And it keeps tracking: widen the channel and the dial widens with it. */
    c.max = 300.0f;
    arc_on_channel_changed(&d);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 43.51f, d.signal_max);
}

/* ── Idempotence + persistence ───────────────────────────────────────────── */

static void test_sync_is_idempotent(void) {
    /* Every channel edit re-notifies every bound widget. Converting in place
     * would compound: 250 → 36.26 → 5.26 → 0.76 … */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    d.limiter_base = 200.0f;
    d.tick_min_base = 50.0f; d.tick_max_base = 200.0f;
    arc_sync_units(&d);
    float f_min = d.signal_min, f_max = d.signal_max;
    float f_lim = d.limiter_value, f_tmin = d.tick_min, f_tmax = d.tick_max;

    for (int i = 0; i < 10; i++) arc_sync_units(&d);

    TEST_ASSERT_EQUAL_FLOAT(f_min,  d.signal_min);
    TEST_ASSERT_EQUAL_FLOAT(f_max,  d.signal_max);
    TEST_ASSERT_EQUAL_FLOAT(f_lim,  d.limiter_value);
    TEST_ASSERT_EQUAL_FLOAT(f_tmin, d.tick_min);
    TEST_ASSERT_EQUAL_FLOAT(f_tmax, d.tick_max);
}

static void test_channel_changed_is_idempotent(void) {
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    arc_on_channel_changed(&d);
    float first_redline = d.redline_threshold, first_low = d.arc_low;

    for (int i = 0; i < 10; i++) arc_on_channel_changed(&d);

    TEST_ASSERT_EQUAL_FLOAT(first_redline, d.redline_threshold);
    TEST_ASSERT_EQUAL_FLOAT(first_low, d.arc_low);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.26f, d.signal_max);
}

static void test_save_reload_does_not_convert_twice(void) {
    /* to_json persists the native base, so the reloaded dial is identical.
     * Persisting the converted signal_max instead would reload 36.26 as if the
     * user had authored it and convert it again to 5.26. */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    d.limiter_base = 200.0f;
    d.tick_min_base = 50.0f; d.tick_max_base = 200.0f;
    d.anchor_base = 100.0f;
    arc_sync_units(&d);

    arc_json_t j = arc_to_json(&d);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, j.signal_max);      /* native on disk */
    TEST_ASSERT_EQUAL_FLOAT(200.0f, j.limiter_value);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, j.tick_max);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, j.anchor_value);

    arc_mirror_t back = arc_from_json(&j, &c);
    TEST_ASSERT_EQUAL_FLOAT(d.signal_max,    back.signal_max);
    TEST_ASSERT_EQUAL_FLOAT(d.limiter_value, back.limiter_value);
    TEST_ASSERT_EQUAL_FLOAT(d.tick_max,      back.tick_max);
    TEST_ASSERT_EQUAL_FLOAT(d.anchor_value,  back.anchor_value);
}

static void test_reload_survives_switching_the_unit_back(void) {
    chan_mirror_t psi = map_in("psi");
    arc_mirror_t d = pinned_arc(&psi);
    arc_json_t j = arc_to_json(&d);

    /* User changes their mind: display back to kPa. */
    chan_mirror_t kpa = map_in("kPa");
    arc_mirror_t back = arc_from_json(&j, &kpa);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, back.signal_min);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, back.signal_max);
}

static void test_repeated_save_reload_is_stable(void) {
    /* Five save/load cycles with a psi display — any leak of a converted value
     * into the persisted base would drift the scale a little each round. */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);
    for (int i = 0; i < 5; i++) {
        arc_json_t j = arc_to_json(&d);
        d = arc_from_json(&j, &c);
    }
    TEST_ASSERT_EQUAL_FLOAT(250.0f, d.range_max_base);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.26f, d.signal_max);
}

static void test_unpinned_arc_does_not_persist_the_channel_range(void) {
    /* An arc tracking the channel must not write that range out: from_json
     * reads any present signal_min/max as an explicit pin, so saving it would
     * freeze the gauge against later channel edits — the trap widget_text hit
     * persisting channel-sourced decimals. */
    chan_mirror_t c = map_in("psi");
    arc_json_t j0 = {0};
    arc_mirror_t d = arc_from_json(&j0, &c);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, d.range_max_base);   /* adopted */

    arc_json_t j = arc_to_json(&d);
    TEST_ASSERT_FALSE(j.has_signal_min);
    TEST_ASSERT_FALSE(j.has_signal_max);

    arc_mirror_t back = arc_from_json(&j, &c);
    TEST_ASSERT_FALSE(back.range_from_layout);
    c.max = 400.0f;
    arc_on_channel_changed(&back);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 58.02f, back.signal_max);
}

/* ── Widget settings edits ───────────────────────────────────────────────── */

static void test_settings_edit_round_trips_through_native(void) {
    /* The Signal Min/Max fields show what the dial shows, so a typed 40 psi has
     * to store 275.8 kPa and read back as 40. */
    chan_mirror_t c = map_in("psi");
    arc_mirror_t d = pinned_arc(&c);

    arc_set_range_display(&d, 0.0f, 40.0f);

    TEST_ASSERT_FLOAT_WITHIN(0.05f, 275.79f, d.range_max_base);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, d.signal_max);
    TEST_ASSERT_TRUE(d.range_from_layout);

    /* And it survives the save/load cycle. */
    arc_json_t j = arc_to_json(&d);
    arc_mirror_t back = arc_from_json(&j, &c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, back.signal_max);
}

static void test_typing_the_factory_range_unpins_the_scale(void) {
    /* Mirrors _meter_set_range_display: 0..100 back in hands the scale to the
     * channel again rather than pinning the arc to the default forever. */
    chan_mirror_t c = map_in("kPa");
    arc_mirror_t d = pinned_arc(&c);
    TEST_ASSERT_TRUE(d.range_from_layout);

    arc_set_range_display(&d, 0.0f, 100.0f);

    TEST_ASSERT_FALSE(d.range_from_layout);
    arc_on_channel_changed(&d);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, d.signal_max);   /* channel range again */
}

static void test_settings_edit_on_unbound_arc_is_literal(void) {
    arc_mirror_t d = {0};
    d.channel = NULL;
    arc_set_range_display(&d, 0.0f, 8000.0f);

    TEST_ASSERT_EQUAL_FLOAT(8000.0f, d.range_max_base);
    TEST_ASSERT_EQUAL_FLOAT(8000.0f, d.signal_max);
}

/* ── Nothing moves when there's nothing to convert ───────────────────────── */

static void test_identity_unit_changes_nothing(void) {
    /* The overwhelmingly common case: native == display. */
    chan_mirror_t c = map_in("kPa");
    arc_mirror_t d = pinned_arc(&c);
    arc_on_channel_changed(&d);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.signal_min);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, d.signal_max);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, d.redline_threshold);
    TEST_ASSERT_EQUAL_FLOAT(MAP_WOT_KPA, arc_signal_in(&d, MAP_WOT_KPA));
}

static void test_unknown_unit_pair_passes_through(void) {
    /* A free-text relabel ("Custom…" in the editor) has no conversion — it must
     * relabel only, never silently rescale the dial. */
    chan_mirror_t c = map_in("boost units");
    arc_mirror_t d = pinned_arc(&c);
    arc_on_channel_changed(&d);

    TEST_ASSERT_EQUAL_FLOAT(250.0f, d.signal_max);
    TEST_ASSERT_EQUAL_FLOAT(MAP_WOT_KPA, arc_signal_in(&d, MAP_WOT_KPA));
    /* The label still adopts it, so the number and its caption agree. */
    TEST_ASSERT_EQUAL_STRING("boost units", d.value_unit);
}

static void test_unbound_arc_keeps_the_layout_numbers(void) {
    arc_json_t j = { .has_signal_min = true, .signal_min = -50.0f,
                     .has_signal_max = true, .signal_max = 300.0f };
    arc_mirror_t d = arc_from_json(&j, NULL);

    TEST_ASSERT_EQUAL_FLOAT(-50.0f, d.signal_min);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, d.signal_max);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, arc_signal_in(&d, 300.0f));
}

/* ── Value-unit adoption ─────────────────────────────────────────────────── */

static void test_bound_arc_adopts_the_channel_display_unit(void) {
    chan_mirror_t c = map_in("psi");
    arc_json_t j = {0};                  /* no value_unit authored */
    arc_mirror_t d = arc_from_json(&j, &c);

    TEST_ASSERT_EQUAL_STRING("psi", d.value_unit);
    TEST_ASSERT_FALSE(d.value_unit_from_layout);

    /* Follows a later change. */
    chan_mirror_t kpa = map_in("kPa");
    d.channel = &kpa;
    arc_on_channel_changed(&d);
    TEST_ASSERT_EQUAL_STRING("kPa", d.value_unit);
}

static void test_authored_value_unit_wins_and_sticks(void) {
    /* "x1000 RPM" style captions are the reason value_unit is free text — the
     * channel must never relabel one. */
    chan_mirror_t c = map_in("psi");
    arc_json_t j = { .has_value_unit = true, .value_unit = "BOOST" };
    arc_mirror_t d = arc_from_json(&j, &c);

    TEST_ASSERT_EQUAL_STRING("BOOST", d.value_unit);
    TEST_ASSERT_TRUE(d.value_unit_from_layout);

    arc_on_channel_changed(&d);
    TEST_ASSERT_EQUAL_STRING("BOOST", d.value_unit);

    /* And it round-trips: it differs from units_display, so to_json emits it. */
    arc_json_t saved = arc_to_json(&d);
    TEST_ASSERT_TRUE(saved.has_value_unit);
    arc_mirror_t back = arc_from_json(&saved, &c);
    TEST_ASSERT_EQUAL_STRING("BOOST", back.value_unit);
    TEST_ASSERT_TRUE(back.value_unit_from_layout);
}

static void test_adopted_value_unit_is_not_persisted_as_an_override(void) {
    /* Saving the adopted unit back would reload as an authored caption and stop
     * tracking the channel — same rule as the scale. */
    chan_mirror_t c = map_in("psi");
    arc_json_t j0 = {0};
    arc_mirror_t d = arc_from_json(&j0, &c);
    TEST_ASSERT_EQUAL_STRING("psi", d.value_unit);

    arc_json_t j = arc_to_json(&d);
    TEST_ASSERT_FALSE(j.has_value_unit);

    arc_mirror_t back = arc_from_json(&j, &c);
    TEST_ASSERT_FALSE(back.value_unit_from_layout);
    chan_mirror_t kpa = map_in("kPa");
    back.channel = &kpa;
    arc_on_channel_changed(&back);
    TEST_ASSERT_EQUAL_STRING("kPa", back.value_unit);
}

/* ── Other unit families take the same path ─────────────────────────────── */

static void test_temperature_offset_conversion_pins_correctly(void) {
    /* °C→°F has a non-zero offset, so a base that isn't re-derived from the
     * native value would drift by 32 every time. */
    chan_mirror_t c = { .units_native = "\xC2\xB0" "C", .units_display = "\xC2\xB0" "F",
                        .min = 0.0f, .max = 120.0f,
                        .low_warn = CHANNEL_THRESHOLD_UNSET_LOW, .high_warn = 110.0f };
    arc_mirror_t d = {0};
    d.channel = &c;
    d.range_min_base = 0.0f; d.range_max_base = 120.0f;
    d.range_from_layout = true;
    d.redline_base = ARC_DEFAULT_REDLINE;
    arc_on_channel_changed(&d);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.0f, d.signal_min);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 248.0f, d.signal_max);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 230.0f, d.redline_threshold);   /* 110 °C */

    for (int i = 0; i < 5; i++) arc_on_channel_changed(&d);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.0f, d.signal_min);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 248.0f, d.signal_max);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 230.0f, d.redline_threshold);

    /* 90 °C coolant sits at the same fraction of the sweep either way. */
    chan_mirror_t in_c = c; in_c.units_display = in_c.units_native;
    arc_mirror_t d_c = {0};
    d_c.channel = &in_c;
    d_c.range_min_base = 0.0f; d_c.range_max_base = 120.0f;
    d_c.range_from_layout = true;
    arc_sync_units(&d_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, fill_fraction(&d_c, 90.0f),
                                     fill_fraction(&d, 90.0f));
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* the regression */
    RUN_TEST(test_pinned_scale_follows_display_unit);
    RUN_TEST(test_fill_lands_in_the_same_place_in_either_unit);
    RUN_TEST(test_value_text_number_matches_its_unit);
    RUN_TEST(test_redline_shares_the_scale_unit);
    RUN_TEST(test_alert_thresholds_share_the_scale_unit);
    RUN_TEST(test_unset_alert_side_parks_at_the_converted_range_edge);
    RUN_TEST(test_limiter_and_anchor_share_the_scale_unit);
    RUN_TEST(test_tick_window_converts_when_live);
    RUN_TEST(test_disabled_tick_window_is_not_converted_into_existence);

    /* tick spacing is an interval */
    RUN_TEST(test_tick_step_converts_as_an_interval);
    RUN_TEST(test_tick_step_takes_the_scale_not_the_offset);
    RUN_TEST(test_zero_tick_step_stays_off);
    RUN_TEST(test_tick_step_survives_save_reload);
    RUN_TEST(test_typed_tick_step_round_trips);

    /* the explicit-scale pin */
    RUN_TEST(test_explicit_scale_survives_channel_changed);
    RUN_TEST(test_explicit_scale_survives_repeated_notifies);
    RUN_TEST(test_unpinned_arc_still_adopts_the_channel_range);

    /* idempotence + persistence */
    RUN_TEST(test_sync_is_idempotent);
    RUN_TEST(test_channel_changed_is_idempotent);
    RUN_TEST(test_save_reload_does_not_convert_twice);
    RUN_TEST(test_reload_survives_switching_the_unit_back);
    RUN_TEST(test_repeated_save_reload_is_stable);
    RUN_TEST(test_unpinned_arc_does_not_persist_the_channel_range);

    /* widget settings */
    RUN_TEST(test_settings_edit_round_trips_through_native);
    RUN_TEST(test_typing_the_factory_range_unpins_the_scale);
    RUN_TEST(test_settings_edit_on_unbound_arc_is_literal);

    /* nothing to convert */
    RUN_TEST(test_identity_unit_changes_nothing);
    RUN_TEST(test_unknown_unit_pair_passes_through);
    RUN_TEST(test_unbound_arc_keeps_the_layout_numbers);

    /* value-unit adoption */
    RUN_TEST(test_bound_arc_adopts_the_channel_display_unit);
    RUN_TEST(test_authored_value_unit_wins_and_sticks);
    RUN_TEST(test_adopted_value_unit_is_not_persisted_as_an_override);

    /* other unit families */
    RUN_TEST(test_temperature_offset_conversion_pins_correctly);

    return UNITY_END();
}
