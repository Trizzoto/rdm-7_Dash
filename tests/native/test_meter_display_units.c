/* test_meter_display_units.c — widget_meter scale vs. channel display unit.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * A channel stores its value, range and thresholds in a NATIVE unit (kPa for
 * manifold_pressure) and renders in a DISPLAY unit the user picks (psi). The
 * meter is the only gauge that converts, and it used to convert only PART of
 * itself:
 *
 *     _meter_on_signal      → value  converted  (always)
 *     _meter_apply_channel  → redline converted (always)
 *     _meter_apply_channel  → min/max converted ONLY when !range_from_layout
 *
 * range_from_layout is set for any layout whose meter min/max isn't the 0..100
 * factory default — i.e. every authored gauge. So switching a MAP gauge from
 * kPa to psi left a 0..250 kPa dial with a psi needle: 101 kPa read 14.7 and
 * sat on the bottom stop, dragging the redline down with it.
 *
 * The fix keeps the authored scale in a native-unit base (range_min_base /
 * range_max_base) and always derives md->min/max from it, so scale, needle and
 * redline share one unit no matter where the scale came from.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * Same "mirror the firmware logic verbatim" approach as
 * test_widget_arc_precedence.c — the real functions write to LVGL, which would
 * drag in the graphics stack. The CONVERSION itself is not mirrored: this
 * links main/data/unit_convert.c straight out of the firmware, so the factors
 * under test are the shipping ones.
 *
 * Source-of-truth references (must stay in lockstep):
 *
 *   main/widgets/widget_meter.c, _meter_sync_range()
 *   main/widgets/widget_meter.c, _meter_set_range_display()
 *   main/widgets/widget_meter.c, _meter_on_signal()      (value convert + clamp)
 *   main/widgets/widget_meter.c, _meter_apply_channel()  (redline convert)
 */
#include "unity.h"
#include "unit_convert.h"

#include <stdbool.h>
#include <stdint.h>

/* ── Mirrors ─────────────────────────────────────────────────────────────
 * Subsets of channel_t / meter_data_t. Field names match the firmware
 * structs one-for-one so the mirror reads like the original. */
typedef struct {
    const char *units_native;
    const char *units_display;
    float       min;
    float       max;
    float       high_warn;
} chan_mirror_t;

typedef struct {
    const chan_mirror_t *channel;
    float min;                  /* rendered scale — DISPLAY units */
    float max;
    bool  range_from_layout;
    float range_min_base;       /* authored scale — NATIVE units */
    float range_max_base;
    float redline_threshold;    /* DISPLAY units, like min/max */
    bool  redline_enabled;
} meter_mirror_t;

/* Verbatim port of _meter_sync_range(). */
static void meter_sync_range(meter_mirror_t *md) {
    const chan_mirror_t *c = md->channel;
    if (c && !md->range_from_layout) {
        md->range_min_base = c->min;
        md->range_max_base = c->max;
    }
    if (c) {
        md->min = unit_convert(md->range_min_base, c->units_native, c->units_display);
        md->max = unit_convert(md->range_max_base, c->units_native, c->units_display);
    } else {
        md->min = md->range_min_base;
        md->max = md->range_max_base;
    }
}

/* Verbatim port of _meter_set_range_display() — inspector write path. */
static void meter_set_range_display(meter_mirror_t *md, float dmin, float dmax) {
    const chan_mirror_t *c = md->channel;
    md->range_min_base = c ? unit_convert(dmin, c->units_display, c->units_native) : dmin;
    md->range_max_base = c ? unit_convert(dmax, c->units_display, c->units_native) : dmax;
    md->range_from_layout =
        (md->range_min_base != 0.0f || md->range_max_base != 100.0f);
    meter_sync_range(md);
}

/* The redline half of _meter_apply_channel(). */
static void meter_apply_redline(meter_mirror_t *md) {
    const chan_mirror_t *c = md->channel;
    if (!c) return;
    md->redline_enabled = true;
    md->redline_threshold = unit_convert(c->high_warn, c->units_native, c->units_display);
}

/* The needle half of _meter_on_signal(): convert to display units, then clamp
 * into the scale. Returns what the needle would point at. */
static float meter_needle_value(const meter_mirror_t *md, float native) {
    float fv = native;
    if (md->channel)
        fv = unit_convert(fv, md->channel->units_native, md->channel->units_display);
    if (fv < md->min) fv = md->min;
    if (fv > md->max) fv = md->max;
    return fv;
}

/* Where the needle sits along the sweep, 0.0 = bottom stop, 1.0 = top. */
static float needle_fraction(const meter_mirror_t *md, float native) {
    float span = md->max - md->min;
    if (span <= 0.0f) return 0.0f;
    return (meter_needle_value(md, native) - md->min) / span;
}

/* The layout persists range_*_base, so a save/load cycle is just that pair
 * surviving a round trip through from_json's range_from_layout rule. */
static meter_mirror_t reload(const meter_mirror_t *saved, const chan_mirror_t *c) {
    meter_mirror_t md = {0};
    md.channel = c;
    md.range_min_base = saved->range_min_base;   /* cfg["min"] */
    md.range_max_base = saved->range_max_base;   /* cfg["max"] */
    md.min = md.range_min_base;
    md.max = md.range_max_base;
    if (md.range_min_base != 0.0f || md.range_max_base != 100.0f)
        md.range_from_layout = true;
    meter_sync_range(&md);
    return md;
}

/* ── Fixtures ────────────────────────────────────────────────────────────
 * manifold_pressure as canonical_channels.c defines it, with the display
 * unit switched to psi — exactly the edit that surfaced the bug. */
#define MAP_IDLE_KPA  30.0f
#define MAP_WOT_KPA  101.0f

static chan_mirror_t map_in_psi(void) {
    chan_mirror_t c = {
        .units_native = "kPa", .units_display = "psi",
        .min = 0.0f, .max = 250.0f, .high_warn = 200.0f,
    };
    return c;
}

static chan_mirror_t map_in_kpa(void) {
    chan_mirror_t c = {
        .units_native = "kPa", .units_display = "kPa",
        .min = 0.0f, .max = 250.0f, .high_warn = 200.0f,
    };
    return c;
}

/* An authored MAP gauge: layout pinned the dial to 0..250 (kPa). */
static meter_mirror_t pinned_meter(const chan_mirror_t *c) {
    meter_mirror_t md = {0};
    md.channel = c;
    md.range_min_base = 0.0f;
    md.range_max_base = 250.0f;
    md.range_from_layout = true;
    meter_sync_range(&md);
    return md;
}

/* ── The conversion factors themselves (real firmware table) ────────────── */

static void test_kpa_to_psi_factor_is_real(void) {
    /* One standard atmosphere. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 14.696f, unit_convert(101.325f, "kPa", "psi"));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 101.325f, unit_convert(14.696f, "psi", "kPa"));
}

/* ── The regression ──────────────────────────────────────────────────────── */

static void test_pinned_scale_follows_display_unit(void) {
    chan_mirror_t c = map_in_psi();
    meter_mirror_t md = pinned_meter(&c);

    /* 0..250 kPa is 0..36.26 psi — the dial re-expresses itself. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, md.min);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.26f, md.max);
}

static void test_pinned_needle_is_not_pegged_to_the_bottom(void) {
    /* The user-visible symptom: with the scale left in kPa, a psi needle at
     * 14.65 sat at 6% of a 0..250 sweep instead of 40%. */
    chan_mirror_t c = map_in_psi();
    meter_mirror_t md = pinned_meter(&c);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 14.65f, meter_needle_value(&md, MAP_WOT_KPA));
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.404f, needle_fraction(&md, MAP_WOT_KPA));

    /* Idle sits where it did in kPa — 30/250 and 4.35/36.26 are the same spot. */
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.12f, needle_fraction(&md, MAP_IDLE_KPA));
}

static void test_pinned_needle_tracks_the_same_spot_in_either_unit(void) {
    /* Switching the display unit must not move the needle — only relabel it. */
    chan_mirror_t kpa = map_in_kpa();
    chan_mirror_t psi = map_in_psi();
    meter_mirror_t in_kpa = pinned_meter(&kpa);
    meter_mirror_t in_psi = pinned_meter(&psi);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, needle_fraction(&in_kpa, MAP_WOT_KPA),
                                     needle_fraction(&in_psi, MAP_WOT_KPA));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, needle_fraction(&in_kpa, MAP_IDLE_KPA),
                                     needle_fraction(&in_psi, MAP_IDLE_KPA));
}

static void test_redline_shares_the_scale_unit(void) {
    /* 200 kPa high_warn is 29 psi — it has to land 80% up the dial, not near
     * the bottom stop the way it did against an unconverted kPa scale. */
    chan_mirror_t c = map_in_psi();
    meter_mirror_t md = pinned_meter(&c);
    meter_apply_redline(&md);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.01f, md.redline_threshold);
    TEST_ASSERT_TRUE(md.redline_threshold > md.min);
    TEST_ASSERT_TRUE(md.redline_threshold < md.max);
    float frac = (md.redline_threshold - md.min) / (md.max - md.min);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.8f, frac);
}

/* ── Idempotence + round-trip ────────────────────────────────────────────── */

static void test_sync_is_idempotent(void) {
    /* Every channel edit re-notifies every bound widget. Converting in place
     * would compound: 250 → 36.26 → 5.26 → 0.76 … */
    chan_mirror_t c = map_in_psi();
    meter_mirror_t md = pinned_meter(&c);
    float first_min = md.min, first_max = md.max;

    for (int i = 0; i < 10; i++) meter_sync_range(&md);

    TEST_ASSERT_EQUAL_FLOAT(first_min, md.min);
    TEST_ASSERT_EQUAL_FLOAT(first_max, md.max);
}

static void test_save_reload_does_not_convert_twice(void) {
    /* to_json persists the native base, so the reloaded dial is identical.
     * Persisting the converted md->max instead would reload 36.26 as if the
     * user had authored it and convert it again to 5.26. */
    chan_mirror_t c = map_in_psi();
    meter_mirror_t md = pinned_meter(&c);

    meter_mirror_t back = reload(&md, &c);

    TEST_ASSERT_EQUAL_FLOAT(250.0f, back.range_max_base);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.26f, back.max);
    TEST_ASSERT_EQUAL_FLOAT(md.max, back.max);
}

static void test_reload_survives_switching_the_unit_back(void) {
    chan_mirror_t psi = map_in_psi();
    meter_mirror_t md = pinned_meter(&psi);
    meter_mirror_t saved = reload(&md, &psi);

    /* User changes their mind: display back to kPa. */
    chan_mirror_t kpa = map_in_kpa();
    meter_mirror_t back = reload(&saved, &kpa);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, back.min);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, back.max);
}

/* ── The other range sources ─────────────────────────────────────────────── */

static void test_unpinned_meter_still_adopts_the_channel_range(void) {
    chan_mirror_t c = map_in_psi();
    meter_mirror_t md = {0};
    md.channel = c.units_native ? &c : NULL;
    md.range_min_base = 0.0f;
    md.range_max_base = 100.0f;      /* factory default → not pinned */
    md.range_from_layout = false;
    meter_sync_range(&md);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.26f, md.max);
    /* Base follows the channel so to_json keeps emitting a native range. */
    TEST_ASSERT_EQUAL_FLOAT(250.0f, md.range_max_base);
}

static void test_unbound_meter_keeps_the_layout_numbers(void) {
    meter_mirror_t md = {0};
    md.channel = NULL;
    md.range_min_base = -50.0f;
    md.range_max_base = 300.0f;
    md.range_from_layout = true;
    meter_sync_range(&md);

    TEST_ASSERT_EQUAL_FLOAT(-50.0f, md.min);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, md.max);
}

static void test_identity_unit_changes_nothing(void) {
    /* The overwhelmingly common case: native == display. Nothing may move. */
    chan_mirror_t c = map_in_kpa();
    meter_mirror_t md = pinned_meter(&c);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, md.min);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, md.max);
    TEST_ASSERT_EQUAL_FLOAT(MAP_WOT_KPA, meter_needle_value(&md, MAP_WOT_KPA));
}

static void test_unknown_unit_pair_passes_through(void) {
    /* A free-text relabel ("Custom…" in the editor) has no conversion — it
     * must relabel only, never silently rescale the dial. */
    chan_mirror_t c = { .units_native = "kPa", .units_display = "boost units",
                        .min = 0.0f, .max = 250.0f, .high_warn = 200.0f };
    meter_mirror_t md = pinned_meter(&c);

    TEST_ASSERT_EQUAL_FLOAT(250.0f, md.max);
    TEST_ASSERT_EQUAL_FLOAT(MAP_WOT_KPA, meter_needle_value(&md, MAP_WOT_KPA));
}

/* ── Inspector edits ─────────────────────────────────────────────────────── */

static void test_inspector_edit_round_trips_through_native(void) {
    /* The Min/Max fields show what the dial shows, so a typed 40 psi has to
     * store 275.8 kPa and read back as 40. */
    chan_mirror_t c = map_in_psi();
    meter_mirror_t md = pinned_meter(&c);

    meter_set_range_display(&md, 0.0f, 40.0f);

    TEST_ASSERT_FLOAT_WITHIN(0.05f, 275.79f, md.range_max_base);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, md.max);
    TEST_ASSERT_TRUE(md.range_from_layout);

    /* And it survives the save/load cycle. */
    meter_mirror_t back = reload(&md, &c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, back.max);
}

static void test_inspector_edit_on_unbound_meter_is_literal(void) {
    meter_mirror_t md = {0};
    md.channel = NULL;
    meter_set_range_display(&md, 0.0f, 8000.0f);

    TEST_ASSERT_EQUAL_FLOAT(8000.0f, md.range_max_base);
    TEST_ASSERT_EQUAL_FLOAT(8000.0f, md.max);
}

/* ── Other unit families take the same path ─────────────────────────────── */

static void test_temperature_offset_conversion_pins_correctly(void) {
    /* °C→°F has a non-zero offset, so a base that isn't re-derived from the
     * native value would drift by 32 every time. */
    chan_mirror_t c = { .units_native = "\xC2\xB0" "C", .units_display = "\xC2\xB0" "F",
                        .min = 0.0f, .max = 120.0f, .high_warn = 110.0f };
    meter_mirror_t md = {0};
    md.channel = &c;
    md.range_min_base = 0.0f;
    md.range_max_base = 120.0f;
    md.range_from_layout = true;
    meter_sync_range(&md);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.0f, md.min);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 248.0f, md.max);

    for (int i = 0; i < 5; i++) meter_sync_range(&md);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.0f, md.min);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 248.0f, md.max);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_kpa_to_psi_factor_is_real);

    /* the regression */
    RUN_TEST(test_pinned_scale_follows_display_unit);
    RUN_TEST(test_pinned_needle_is_not_pegged_to_the_bottom);
    RUN_TEST(test_pinned_needle_tracks_the_same_spot_in_either_unit);
    RUN_TEST(test_redline_shares_the_scale_unit);

    /* idempotence + persistence */
    RUN_TEST(test_sync_is_idempotent);
    RUN_TEST(test_save_reload_does_not_convert_twice);
    RUN_TEST(test_reload_survives_switching_the_unit_back);

    /* other range sources */
    RUN_TEST(test_unpinned_meter_still_adopts_the_channel_range);
    RUN_TEST(test_unbound_meter_keeps_the_layout_numbers);
    RUN_TEST(test_identity_unit_changes_nothing);
    RUN_TEST(test_unknown_unit_pair_passes_through);

    /* inspector */
    RUN_TEST(test_inspector_edit_round_trips_through_native);
    RUN_TEST(test_inspector_edit_on_unbound_meter_is_literal);

    /* other unit families */
    RUN_TEST(test_temperature_offset_conversion_pins_correctly);

    return UNITY_END();
}
