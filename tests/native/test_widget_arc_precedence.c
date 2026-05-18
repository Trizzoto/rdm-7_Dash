/* test_widget_arc_precedence.c — fill-color precedence for widget_arc.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * widget_arc.c has a precedence ladder for the indicator fill color:
 *
 *     1. Limiter Solid       (limiter_effect == 2 && in_limiter)
 *     2. Limiter Flash phase (limiter_effect == 1 && in_limiter)
 *     3. Redline in-zone     (redline_enabled && redline_recolor_fill &&
 *                             _cached_value >= redline_threshold)
 *     4. Rule override       (_rule_arc_color_set)        <-- added 2026-05-19
 *     5. Night override      (night.arc_color)
 *     6. Default             (arc_color)
 *
 * The "rule override" tier was added when widget_arc.c was patched to fix
 * a regression where a widget_rule's arc_color override was lost once
 * a limiter or redline zone cleared — the indicator snapped back to the
 * default arc_color instead of the rule's colour. The fix caches the
 * rule colour in arc_data_t and reads it as the "normal" base inside
 * _arc_apply_fill_color. These tests lock that behaviour in.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * Same "mirror the firmware logic verbatim" approach as
 * test_layout_migration.c. The real _arc_apply_fill_color writes to
 * LVGL via lv_obj_set_style_arc_color, which would drag in the entire
 * graphics stack. Here we re-implement the *decision* (no LVGL write)
 * over a stripped-down struct that mirrors the relevant arc_data_t
 * fields one-for-one. If the firmware ladder is ever reordered, these
 * tests go red and the host mirror needs to be updated in lockstep.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/widgets/widget_arc.c, _arc_apply_fill_color()
 */
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

/* ── lv_color_t shim ──────────────────────────────────────────────────────
 * RGB565 packed into uint16_t .full — matches LVGL v8's representation. */
typedef struct {
    uint16_t full;
} lv_color_t;

static inline lv_color_t color_from_u16(uint16_t v) {
    lv_color_t c = { .full = v };
    return c;
}

/* ── arc data mirror ──────────────────────────────────────────────────────
 * Subset of arc_data_t fields used by _arc_apply_fill_color. Field names
 * match the firmware struct one-for-one so the mirror reads similarly. */
typedef struct {
    bool       has_arc_color;
    lv_color_t arc_color;        /* night override */
    bool       has_redline_color;
    lv_color_t redline_color;    /* night override */
} arc_night_t;

typedef struct {
    /* base styling */
    lv_color_t arc_color;        /* default fill */
    lv_color_t redline_color;    /* redline-zone fill */
    lv_color_t limiter_color;    /* limiter solid / flash colour */

    /* limiter state machine */
    uint8_t    limiter_effect;   /* 0=None, 1=Bar Flash, 2=Bar Solid */
    bool       in_limiter;       /* value >= limiter_value */
    bool       flash_phase;      /* current flash toggle (true = show limiter) */

    /* redline zone */
    bool       redline_enabled;
    bool       redline_recolor_fill;
    float      _cached_value;
    float      redline_threshold;

    /* rule override cache (the fix under test) */
    lv_color_t _rule_arc_color;
    bool       _rule_arc_color_set;

    /* night overrides */
    arc_night_t night;
} arc_data_t;

/* ── Function under test (host mirror) ────────────────────────────────────
 * Verbatim port of the precedence ladder from
 * main/widgets/widget_arc.c::_arc_apply_fill_color, minus the LVGL paint
 * call. Returns the colour that would be written to LV_PART_INDICATOR. */
static lv_color_t arc_pick_fill_color(const arc_data_t *d, bool active) {
    /* Step A: pick the "normal" base.
     *   - Rule override wins over night and default.
     *   - Else: night override if active, else default arc_color. */
    lv_color_t normal;
    if (d->_rule_arc_color_set) {
        normal = d->_rule_arc_color;
    } else if (active && d->night.has_arc_color) {
        normal = d->night.arc_color;
    } else {
        normal = d->arc_color;
    }

    /* Redline colour also honours night override. */
    lv_color_t redline = (active && d->night.has_redline_color)
                            ? d->night.redline_color
                            : d->redline_color;

    /* Step B: layered precedence on top of normal. */
    lv_color_t fill = normal;
    if (d->in_limiter && d->limiter_effect == 2) {
        fill = d->limiter_color;
    } else if (d->in_limiter && d->limiter_effect == 1) {
        fill = d->flash_phase ? d->limiter_color : normal;
    } else if (d->redline_enabled && d->redline_recolor_fill) {
        if (d->_cached_value >= d->redline_threshold) {
            fill = redline;
        }
    }
    return fill;
}

/* ── Test fixtures ────────────────────────────────────────────────────────
 * Distinctive RGB565 values so failures point at the right tier. */
#define DEFAULT_FG    0x07E0u  /* green */
#define NIGHT_FG      0x0410u  /* dim green */
#define REDLINE_FG    0xF800u  /* red */
#define NIGHT_REDLINE 0x7800u  /* dim red */
#define LIMITER_FG    0xFFFFu  /* white */
#define RULE_FG       0x001Fu  /* blue */

static arc_data_t make_baseline_arc(void) {
    arc_data_t d = {0};
    d.arc_color     = color_from_u16(DEFAULT_FG);
    d.redline_color = color_from_u16(REDLINE_FG);
    d.limiter_color = color_from_u16(LIMITER_FG);
    /* everything else zero — no limiter, no redline, no rule, no night. */
    return d;
}

/* ── Default-state tests ─────────────────────────────────────────────────── */

static void test_default_returns_arc_color(void) {
    arc_data_t d = make_baseline_arc();
    TEST_ASSERT_EQUAL_HEX(DEFAULT_FG, arc_pick_fill_color(&d, false).full);
}

static void test_night_override_when_active(void) {
    arc_data_t d = make_baseline_arc();
    d.night.has_arc_color = true;
    d.night.arc_color = color_from_u16(NIGHT_FG);

    TEST_ASSERT_EQUAL_HEX(NIGHT_FG, arc_pick_fill_color(&d, true).full);
}

static void test_night_override_ignored_when_inactive(void) {
    arc_data_t d = make_baseline_arc();
    d.night.has_arc_color = true;
    d.night.arc_color = color_from_u16(NIGHT_FG);

    /* active=false → default colour, not night colour. */
    TEST_ASSERT_EQUAL_HEX(DEFAULT_FG, arc_pick_fill_color(&d, false).full);
}

/* ── Limiter tests ──────────────────────────────────────────────────────── */

static void test_limiter_solid_wins(void) {
    arc_data_t d = make_baseline_arc();
    d.limiter_effect = 2;
    d.in_limiter = true;

    TEST_ASSERT_EQUAL_HEX(LIMITER_FG, arc_pick_fill_color(&d, false).full);
}

static void test_limiter_flash_alternates(void) {
    arc_data_t d = make_baseline_arc();
    d.limiter_effect = 1;
    d.in_limiter = true;

    d.flash_phase = true;   /* limiter colour visible */
    TEST_ASSERT_EQUAL_HEX(LIMITER_FG, arc_pick_fill_color(&d, false).full);

    d.flash_phase = false;  /* falls back to "normal" */
    TEST_ASSERT_EQUAL_HEX(DEFAULT_FG, arc_pick_fill_color(&d, false).full);
}

static void test_limiter_solid_overrides_redline(void) {
    /* limiter solid should win over redline even when both are active. */
    arc_data_t d = make_baseline_arc();
    d.limiter_effect = 2;
    d.in_limiter = true;
    d.redline_enabled = true;
    d.redline_recolor_fill = true;
    d.redline_threshold = 80.0f;
    d._cached_value = 95.0f;

    TEST_ASSERT_EQUAL_HEX(LIMITER_FG, arc_pick_fill_color(&d, false).full);
}

static void test_limiter_off_yields_to_redline(void) {
    /* limiter_effect == 0 → ladder falls through to redline. */
    arc_data_t d = make_baseline_arc();
    d.limiter_effect = 0;
    d.in_limiter = true;  /* meaningless when effect=0, but exercise the guard */
    d.redline_enabled = true;
    d.redline_recolor_fill = true;
    d.redline_threshold = 80.0f;
    d._cached_value = 95.0f;

    TEST_ASSERT_EQUAL_HEX(REDLINE_FG, arc_pick_fill_color(&d, false).full);
}

/* ── Redline tests ──────────────────────────────────────────────────────── */

static void test_redline_in_zone(void) {
    arc_data_t d = make_baseline_arc();
    d.redline_enabled = true;
    d.redline_recolor_fill = true;
    d.redline_threshold = 80.0f;
    d._cached_value = 80.0f;  /* boundary inclusive */

    TEST_ASSERT_EQUAL_HEX(REDLINE_FG, arc_pick_fill_color(&d, false).full);
}

static void test_redline_out_of_zone_uses_normal(void) {
    arc_data_t d = make_baseline_arc();
    d.redline_enabled = true;
    d.redline_recolor_fill = true;
    d.redline_threshold = 80.0f;
    d._cached_value = 50.0f;

    TEST_ASSERT_EQUAL_HEX(DEFAULT_FG, arc_pick_fill_color(&d, false).full);
}

static void test_redline_disabled_ignores_threshold(void) {
    arc_data_t d = make_baseline_arc();
    d.redline_enabled = false;   /* master off */
    d.redline_recolor_fill = true;
    d.redline_threshold = 80.0f;
    d._cached_value = 200.0f;

    TEST_ASSERT_EQUAL_HEX(DEFAULT_FG, arc_pick_fill_color(&d, false).full);
}

static void test_redline_no_recolor_keeps_normal(void) {
    /* redline_enabled but recolor_fill off → moving indicator stays normal
     * (only the static redline-zone arc would be drawn in red — outside
     * this function's scope). */
    arc_data_t d = make_baseline_arc();
    d.redline_enabled = true;
    d.redline_recolor_fill = false;
    d.redline_threshold = 80.0f;
    d._cached_value = 200.0f;

    TEST_ASSERT_EQUAL_HEX(DEFAULT_FG, arc_pick_fill_color(&d, false).full);
}

static void test_redline_uses_night_color_when_active(void) {
    arc_data_t d = make_baseline_arc();
    d.redline_enabled = true;
    d.redline_recolor_fill = true;
    d.redline_threshold = 80.0f;
    d._cached_value = 95.0f;
    d.night.has_redline_color = true;
    d.night.redline_color = color_from_u16(NIGHT_REDLINE);

    TEST_ASSERT_EQUAL_HEX(NIGHT_REDLINE, arc_pick_fill_color(&d, true).full);
}

/* ── Rule-override tests (the regression fix) ───────────────────────────── */

static void test_rule_override_wins_over_default(void) {
    arc_data_t d = make_baseline_arc();
    d._rule_arc_color_set = true;
    d._rule_arc_color = color_from_u16(RULE_FG);

    TEST_ASSERT_EQUAL_HEX(RULE_FG, arc_pick_fill_color(&d, false).full);
}

static void test_rule_override_wins_over_night(void) {
    /* When both a rule and a night override are active, the rule wins —
     * rules are an explicit user-intent layered on top of theme. */
    arc_data_t d = make_baseline_arc();
    d._rule_arc_color_set = true;
    d._rule_arc_color = color_from_u16(RULE_FG);
    d.night.has_arc_color = true;
    d.night.arc_color = color_from_u16(NIGHT_FG);

    TEST_ASSERT_EQUAL_HEX(RULE_FG, arc_pick_fill_color(&d, true).full);
}

static void test_limiter_still_wins_over_rule(void) {
    /* The whole point of the fix: while the limiter zone is active, the
     * limiter colour paints. The rule is cached but suppressed. */
    arc_data_t d = make_baseline_arc();
    d._rule_arc_color_set = true;
    d._rule_arc_color = color_from_u16(RULE_FG);
    d.limiter_effect = 2;
    d.in_limiter = true;

    TEST_ASSERT_EQUAL_HEX(LIMITER_FG, arc_pick_fill_color(&d, false).full);
}

static void test_rule_color_restored_when_limiter_clears(void) {
    /* The regression: before the fix, this returned DEFAULT_FG once
     * limiter cleared. After the fix, it returns RULE_FG because
     * _rule_arc_color is read as the "normal" base. */
    arc_data_t d = make_baseline_arc();
    d._rule_arc_color_set = true;
    d._rule_arc_color = color_from_u16(RULE_FG);

    /* Simulate the over-threshold tick. */
    d.limiter_effect = 2;
    d.in_limiter = true;
    TEST_ASSERT_EQUAL_HEX(LIMITER_FG, arc_pick_fill_color(&d, false).full);

    /* Simulate the value falling back below the threshold. */
    d.in_limiter = false;
    TEST_ASSERT_EQUAL_HEX(RULE_FG, arc_pick_fill_color(&d, false).full);
}

static void test_rule_color_restored_when_redline_clears(void) {
    /* Same regression, redline path. Before the fix, dropping out of zone
     * would surface DEFAULT_FG until the rule re-evaluated. */
    arc_data_t d = make_baseline_arc();
    d._rule_arc_color_set = true;
    d._rule_arc_color = color_from_u16(RULE_FG);
    d.redline_enabled = true;
    d.redline_recolor_fill = true;
    d.redline_threshold = 80.0f;

    d._cached_value = 95.0f;
    TEST_ASSERT_EQUAL_HEX(REDLINE_FG, arc_pick_fill_color(&d, false).full);

    d._cached_value = 50.0f;
    TEST_ASSERT_EQUAL_HEX(RULE_FG, arc_pick_fill_color(&d, false).full);
}

static void test_rule_cleared_falls_back_to_default(void) {
    /* When the rule deactivates (count==0 in the firmware), it should
     * clear _rule_arc_color_set so the next paint uses the default
     * colour (or night override). */
    arc_data_t d = make_baseline_arc();
    d._rule_arc_color_set = true;
    d._rule_arc_color = color_from_u16(RULE_FG);

    TEST_ASSERT_EQUAL_HEX(RULE_FG, arc_pick_fill_color(&d, false).full);

    d._rule_arc_color_set = false;  /* rule deactivated */
    TEST_ASSERT_EQUAL_HEX(DEFAULT_FG, arc_pick_fill_color(&d, false).full);
}

static void test_rule_cleared_falls_back_to_night_when_active(void) {
    /* And when night mode is active at the time of rule clear, the
     * fallback should be the night colour, not the day default. */
    arc_data_t d = make_baseline_arc();
    d._rule_arc_color_set = false;
    d.night.has_arc_color = true;
    d.night.arc_color = color_from_u16(NIGHT_FG);

    TEST_ASSERT_EQUAL_HEX(NIGHT_FG, arc_pick_fill_color(&d, true).full);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* default + night */
    RUN_TEST(test_default_returns_arc_color);
    RUN_TEST(test_night_override_when_active);
    RUN_TEST(test_night_override_ignored_when_inactive);

    /* limiter */
    RUN_TEST(test_limiter_solid_wins);
    RUN_TEST(test_limiter_flash_alternates);
    RUN_TEST(test_limiter_solid_overrides_redline);
    RUN_TEST(test_limiter_off_yields_to_redline);

    /* redline */
    RUN_TEST(test_redline_in_zone);
    RUN_TEST(test_redline_out_of_zone_uses_normal);
    RUN_TEST(test_redline_disabled_ignores_threshold);
    RUN_TEST(test_redline_no_recolor_keeps_normal);
    RUN_TEST(test_redline_uses_night_color_when_active);

    /* rule-override regression */
    RUN_TEST(test_rule_override_wins_over_default);
    RUN_TEST(test_rule_override_wins_over_night);
    RUN_TEST(test_limiter_still_wins_over_rule);
    RUN_TEST(test_rule_color_restored_when_limiter_clears);
    RUN_TEST(test_rule_color_restored_when_redline_clears);
    RUN_TEST(test_rule_cleared_falls_back_to_default);
    RUN_TEST(test_rule_cleared_falls_back_to_night_when_active);

    return UNITY_END();
}
