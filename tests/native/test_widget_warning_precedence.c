/* test_widget_warning_precedence.c — effective-value resolution for the
 * Alert Light (widget_warning).
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * update_warning_ui_immediate is the ONLY function that paints the lamp, and
 * it re-runs on every signal change. Before 2026-07-16 two other functions
 * (_warning_apply_overrides and _warning_apply_night_mode) also painted
 * directly from the base fields. That worked only until the next signal tick,
 * which repainted from the base values and silently undid them:
 *
 *   - A rule "IF coolant > 100 THEN active_color = red" turned the lamp red at
 *     the threshold crossing, then reverted on the next temperature change.
 *     Invisible on a 0/1 CAN bit (notify_subscribers only fires on a value
 *     change, so a boolean lamp rarely ticks) — obvious on an analog channel.
 *   - Night-mode colours had the identical bug, hidden for the same reason.
 *
 * The fix routes every look-affecting input through warning_data_t and
 * resolves it in one place. These tests lock that ladder in.
 *
 * Precedence, per resolver:
 *
 *     rule override  >  night override  >  configured base
 *
 * A rule outranks night mode because it is a deliberate user-authored
 * condition. The critical property for already-deployed layouts: with no rules
 * and no night overrides, every resolver returns the base field — byte-identical
 * to the behaviour before any of this existed.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * Same "mirror the firmware logic verbatim" approach as
 * test_widget_arc_precedence.c: the real resolvers are static and their caller
 * paints via LVGL, which would drag in the whole graphics stack. Here we
 * re-implement the *decision* over a stripped struct whose field names match
 * warning_data_t one-for-one. If the firmware ladder changes, these go red and
 * the mirror must be updated in lockstep.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/widgets/widget_warning.c — _warn_eff_active_color,
 *   _warn_eff_inactive_color, _warn_eff_flash_mode, _warn_eff_flash_speed,
 *   _warn_eff_state
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

/* ── warning data mirror ──────────────────────────────────────────────────
 * Subset of warning_data_t used by the resolvers. Names match one-for-one.
 * The night_overrides_t members mirror the NIGHT_FIELD_COLOR macro expansion
 * (a has_* flag beside the value), which NIGHT_PICK_COLOR reads. */
typedef struct {
    bool       has_active_color;
    lv_color_t active_color;
    bool       has_inactive_color;
    lv_color_t inactive_color;
} warning_night_t;

typedef struct {
    /* configured base */
    lv_color_t active_color;
    lv_color_t inactive_color;
    uint8_t    flash_mode;        /* 0=Solid, 1=Flashing */
    uint16_t   flash_speed_ms;
    bool       current_state;     /* signal-driven: any non-zero (maybe inverted) */

    /* rule-override state — all zero/false means "no override" */
    bool       ov_active_color_set;
    lv_color_t ov_active_color;
    bool       ov_inactive_color_set;
    lv_color_t ov_inactive_color;
    bool       ov_flash_mode_set;
    uint8_t    ov_flash_mode;
    bool       ov_flash_speed_set;
    uint16_t   ov_flash_speed_ms;
    uint8_t    ov_lamp;           /* 0 = none, 1 = force off, 2 = force on */

    warning_night_t night;
} warning_data_t;

/* ── Functions under test (host mirrors) ──────────────────────────────────
 * `night` stands in for night_mode_is_active(), which the firmware queries
 * directly rather than taking as a parameter. */

static lv_color_t warn_eff_active_color(const warning_data_t *d, bool night) {
    if (d->ov_active_color_set) return d->ov_active_color;
    if (night && d->night.has_active_color) return d->night.active_color;
    return d->active_color;
}

static lv_color_t warn_eff_inactive_color(const warning_data_t *d, bool night) {
    if (d->ov_inactive_color_set) return d->ov_inactive_color;
    if (night && d->night.has_inactive_color) return d->night.inactive_color;
    return d->inactive_color;
}

static uint8_t warn_eff_flash_mode(const warning_data_t *d) {
    return d->ov_flash_mode_set ? d->ov_flash_mode : d->flash_mode;
}

static uint16_t warn_eff_flash_speed(const warning_data_t *d) {
    uint16_t ms = d->ov_flash_speed_set ? d->ov_flash_speed_ms : d->flash_speed_ms;
    return ms ? ms : 200;
}

static bool warn_eff_state(const warning_data_t *d) {
    if (d->ov_lamp != 0) return (d->ov_lamp == 2);
    return d->current_state;
}

/* The flash timer period update_warning_ui_immediate hands to
 * _warn_ensure_flash_timer: 0 tears the timer down, non-zero (re)creates it.
 * Flashing only runs while the lamp is lit, so a solid or dark lamp is free. */
static uint16_t warn_flash_period(const warning_data_t *d) {
    bool state = warn_eff_state(d);
    return (warn_eff_flash_mode(d) == 1 && state) ? warn_eff_flash_speed(d) : 0;
}

/* ── Test fixtures ────────────────────────────────────────────────────────
 * Distinctive RGB565 values so a failure points at the right tier. */
#define BASE_ACTIVE     0xF800u  /* red */
#define BASE_INACTIVE   0x2124u  /* dark grey */
#define NIGHT_ACTIVE    0x7800u  /* dim red */
#define NIGHT_INACTIVE  0x1082u  /* dimmer grey */
#define RULE_ACTIVE     0xFFE0u  /* yellow */
#define RULE_INACTIVE   0x001Fu  /* blue */

/* A lamp as it exists in an already-deployed layout: base config only, no
 * rules, no night overrides. Every test that asserts back-compat starts here. */
static warning_data_t make_baseline_warning(void) {
    warning_data_t d = {0};
    d.active_color   = color_from_u16(BASE_ACTIVE);
    d.inactive_color = color_from_u16(BASE_INACTIVE);
    d.flash_mode     = 0;
    d.flash_speed_ms = 200;
    d.current_state  = true;
    return d;
}

static void add_night_overrides(warning_data_t *d) {
    d->night.has_active_color   = true;
    d->night.active_color       = color_from_u16(NIGHT_ACTIVE);
    d->night.has_inactive_color = true;
    d->night.inactive_color     = color_from_u16(NIGHT_INACTIVE);
}

/* ── Back-compat: no rules => base values, unchanged ──────────────────────
 * This is the property that protects layouts already running on customer
 * dashes. warning_data_t is heap_caps_calloc'd, so "no rules" really is the
 * all-zero override state these tests assert against. */

static void test_no_overrides_resolves_to_base(void) {
    warning_data_t d = make_baseline_warning();
    TEST_ASSERT_EQUAL_HEX(BASE_ACTIVE,   warn_eff_active_color(&d, false).full);
    TEST_ASSERT_EQUAL_HEX(BASE_INACTIVE, warn_eff_inactive_color(&d, false).full);
    TEST_ASSERT_EQUAL_UINT(0,   warn_eff_flash_mode(&d));
    TEST_ASSERT_EQUAL_UINT(200, warn_eff_flash_speed(&d));
    TEST_ASSERT_TRUE(warn_eff_state(&d));
}

static void test_zeroed_struct_is_no_override(void) {
    /* calloc'd instance: ov_lamp 0 must mean "no override", NOT "force off",
     * or every existing lamp would go dark. */
    warning_data_t d = {0};
    d.current_state = true;
    TEST_ASSERT_TRUE(warn_eff_state(&d));
    d.current_state = false;
    TEST_ASSERT_FALSE(warn_eff_state(&d));
}

static void test_no_overrides_solid_lamp_has_no_flash_timer(void) {
    warning_data_t d = make_baseline_warning();
    TEST_ASSERT_EQUAL_UINT(0, warn_flash_period(&d));
}

static void test_base_flash_mode_still_flashes_without_rules(void) {
    /* A layout that configured Flashing keeps flashing once rules can also
     * drive flash — the regression risk of resolving through ov_* fields. */
    warning_data_t d = make_baseline_warning();
    d.flash_mode     = 1;
    d.flash_speed_ms = 350;
    TEST_ASSERT_EQUAL_UINT(350, warn_flash_period(&d));
}

/* ── Rule overrides ───────────────────────────────────────────────────────── */

static void test_rule_active_color_wins_over_base(void) {
    warning_data_t d = make_baseline_warning();
    d.ov_active_color_set = true;
    d.ov_active_color     = color_from_u16(RULE_ACTIVE);
    TEST_ASSERT_EQUAL_HEX(RULE_ACTIVE, warn_eff_active_color(&d, false).full);
}

static void test_rule_inactive_color_wins_over_base(void) {
    warning_data_t d = make_baseline_warning();
    d.ov_inactive_color_set = true;
    d.ov_inactive_color     = color_from_u16(RULE_INACTIVE);
    TEST_ASSERT_EQUAL_HEX(RULE_INACTIVE, warn_eff_inactive_color(&d, false).full);
}

static void test_rule_can_start_flashing(void) {
    /* The headline gap: flash_mode was a static field the rules engine could
     * not reach, so "THEN Alert Type: Flashing" was a control that did nothing. */
    warning_data_t d = make_baseline_warning();
    TEST_ASSERT_EQUAL_UINT(0, warn_flash_period(&d));   /* solid to begin with */
    d.ov_flash_mode_set = true;
    d.ov_flash_mode     = 1;
    TEST_ASSERT_EQUAL_UINT(200, warn_flash_period(&d)); /* base speed */
}

static void test_rule_flash_speed_alone_applies_to_base_flash_mode(void) {
    /* Overriding speed without mode must not imply mode — hence separate
     * ov_flash_mode_set / ov_flash_speed_set flags rather than one shared. */
    warning_data_t d = make_baseline_warning();
    d.flash_mode           = 1;
    d.ov_flash_speed_set   = true;
    d.ov_flash_speed_ms    = 80;
    TEST_ASSERT_EQUAL_UINT(1, warn_eff_flash_mode(&d));
    TEST_ASSERT_EQUAL_UINT(80, warn_flash_period(&d));
}

static void test_rule_flash_speed_alone_does_not_start_flashing(void) {
    warning_data_t d = make_baseline_warning();  /* flash_mode 0 */
    d.ov_flash_speed_set = true;
    d.ov_flash_speed_ms  = 80;
    TEST_ASSERT_EQUAL_UINT(0, warn_flash_period(&d));
}

static void test_rule_can_stop_flashing(void) {
    warning_data_t d = make_baseline_warning();
    d.flash_mode        = 1;
    d.ov_flash_mode_set = true;
    d.ov_flash_mode     = 0;
    TEST_ASSERT_EQUAL_UINT(0, warn_flash_period(&d));
}

/* ── lamp_on override ─────────────────────────────────────────────────────
 * The lamp is lit on ANY non-zero signal value, so bound to coolant temp it is
 * lit at 20 °C. Forcing it off is what makes a threshold lamp possible. */

static void test_rule_can_force_lamp_off_while_signal_nonzero(void) {
    warning_data_t d = make_baseline_warning();
    d.current_state = true;      /* signal says lit */
    d.ov_lamp       = 1;         /* rule says dark */
    TEST_ASSERT_FALSE(warn_eff_state(&d));
}

static void test_rule_can_force_lamp_on_while_signal_zero(void) {
    warning_data_t d = make_baseline_warning();
    d.current_state = false;
    d.ov_lamp       = 2;
    TEST_ASSERT_TRUE(warn_eff_state(&d));
}

static void test_forced_off_lamp_does_not_flash(void) {
    /* A dark lamp must not hold a live LVGL timer. */
    warning_data_t d = make_baseline_warning();
    d.flash_mode    = 1;
    d.current_state = true;
    d.ov_lamp       = 1;
    TEST_ASSERT_EQUAL_UINT(0, warn_flash_period(&d));
}

/* ── Night mode ───────────────────────────────────────────────────────────── */

static void test_night_override_applies_when_night_active(void) {
    warning_data_t d = make_baseline_warning();
    add_night_overrides(&d);
    TEST_ASSERT_EQUAL_HEX(NIGHT_ACTIVE,   warn_eff_active_color(&d, true).full);
    TEST_ASSERT_EQUAL_HEX(NIGHT_INACTIVE, warn_eff_inactive_color(&d, true).full);
}

static void test_night_override_ignored_when_night_inactive(void) {
    warning_data_t d = make_baseline_warning();
    add_night_overrides(&d);
    TEST_ASSERT_EQUAL_HEX(BASE_ACTIVE, warn_eff_active_color(&d, false).full);
}

static void test_rule_outranks_night(void) {
    warning_data_t d = make_baseline_warning();
    add_night_overrides(&d);
    d.ov_active_color_set = true;
    d.ov_active_color     = color_from_u16(RULE_ACTIVE);
    TEST_ASSERT_EQUAL_HEX(RULE_ACTIVE, warn_eff_active_color(&d, true).full);
}

static void test_night_applies_to_field_rule_does_not_override(void) {
    /* Rule overrides active_color only; inactive_color still follows night. */
    warning_data_t d = make_baseline_warning();
    add_night_overrides(&d);
    d.ov_active_color_set = true;
    d.ov_active_color     = color_from_u16(RULE_ACTIVE);
    TEST_ASSERT_EQUAL_HEX(RULE_ACTIVE,    warn_eff_active_color(&d, true).full);
    TEST_ASSERT_EQUAL_HEX(NIGHT_INACTIVE, warn_eff_inactive_color(&d, true).full);
}

/* ── Deactivation restores base ───────────────────────────────────────────
 * The rules engine calls apply_overrides with count==0 when the last active
 * rule goes false; that handler clears every ov_* flag. Simulate it. */

static void test_cleared_overrides_restore_base(void) {
    warning_data_t d = make_baseline_warning();
    d.ov_active_color_set = true;
    d.ov_active_color     = color_from_u16(RULE_ACTIVE);
    d.ov_flash_mode_set   = true;
    d.ov_flash_mode       = 1;
    d.ov_lamp             = 1;
    TEST_ASSERT_EQUAL_HEX(RULE_ACTIVE, warn_eff_active_color(&d, false).full);

    /* count==0 path */
    d.ov_active_color_set   = false;
    d.ov_inactive_color_set = false;
    d.ov_flash_mode_set     = false;
    d.ov_flash_speed_set    = false;
    d.ov_lamp               = 0;

    TEST_ASSERT_EQUAL_HEX(BASE_ACTIVE, warn_eff_active_color(&d, false).full);
    TEST_ASSERT_EQUAL_UINT(0, warn_flash_period(&d));
    TEST_ASSERT_TRUE(warn_eff_state(&d));
}

/* ── The customer scenario ────────────────────────────────────────────────
 * "Yellow flashing over 90, red flashing faster over 100" on a lamp bound to
 * coolant temp, authored as three rules. Walks the ladder end-to-end. */

static void test_three_rule_threshold_scenario(void) {
    warning_data_t d = make_baseline_warning();
    d.current_state = true;    /* temp is non-zero, so the lamp wants to be lit */

    /* temp = 60 -> rule 1 active: lamp_on = false */
    d.ov_lamp = 1;
    TEST_ASSERT_FALSE(warn_eff_state(&d));
    TEST_ASSERT_EQUAL_UINT(0, warn_flash_period(&d));

    /* temp = 95 -> rule 2: lamp_on = true, active_color = yellow, flash */
    d.ov_lamp             = 2;
    d.ov_active_color_set = true;
    d.ov_active_color     = color_from_u16(RULE_ACTIVE);
    d.ov_flash_mode_set   = true;
    d.ov_flash_mode       = 1;
    TEST_ASSERT_TRUE(warn_eff_state(&d));
    TEST_ASSERT_EQUAL_HEX(RULE_ACTIVE, warn_eff_active_color(&d, false).full);
    TEST_ASSERT_EQUAL_UINT(200, warn_flash_period(&d));

    /* temp = 105 -> rule 3: red, flashing faster */
    d.ov_active_color    = color_from_u16(BASE_ACTIVE);  /* red */
    d.ov_flash_speed_set = true;
    d.ov_flash_speed_ms  = 100;
    TEST_ASSERT_EQUAL_HEX(BASE_ACTIVE, warn_eff_active_color(&d, false).full);
    TEST_ASSERT_EQUAL_UINT(100, warn_flash_period(&d));
}

int main(void) {
    UNITY_BEGIN();

    /* back-compat — the tests that matter most for deployed layouts */
    RUN_TEST(test_no_overrides_resolves_to_base);
    RUN_TEST(test_zeroed_struct_is_no_override);
    RUN_TEST(test_no_overrides_solid_lamp_has_no_flash_timer);
    RUN_TEST(test_base_flash_mode_still_flashes_without_rules);

    /* rule overrides */
    RUN_TEST(test_rule_active_color_wins_over_base);
    RUN_TEST(test_rule_inactive_color_wins_over_base);
    RUN_TEST(test_rule_can_start_flashing);
    RUN_TEST(test_rule_flash_speed_alone_applies_to_base_flash_mode);
    RUN_TEST(test_rule_flash_speed_alone_does_not_start_flashing);
    RUN_TEST(test_rule_can_stop_flashing);

    /* lamp_on */
    RUN_TEST(test_rule_can_force_lamp_off_while_signal_nonzero);
    RUN_TEST(test_rule_can_force_lamp_on_while_signal_zero);
    RUN_TEST(test_forced_off_lamp_does_not_flash);

    /* night mode */
    RUN_TEST(test_night_override_applies_when_night_active);
    RUN_TEST(test_night_override_ignored_when_night_inactive);
    RUN_TEST(test_rule_outranks_night);
    RUN_TEST(test_night_applies_to_field_rule_does_not_override);

    /* restore + end-to-end */
    RUN_TEST(test_cleared_overrides_restore_base);
    RUN_TEST(test_three_rule_threshold_scenario);

    return UNITY_END();
}
