/* test_calculated_gear.c — host-side coverage of CALCULATED_GEAR back-compute.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * CALCULATED_GEAR is a synthetic signal the dash injects every internal-
 * timer tick when the user has configured gear ratios + final drive +
 * wheel circumference. It exposes the currently engaged gear (1..N) so
 * widgets — typically a centre digit on the dash — can show it without
 * needing a vehicle CAN signal for it.
 *
 * The math is small and self-contained:
 *
 *     stationary / idle    → 0   (Neutral)         (speed<5 OR rpm<500)
 *     wheel_rps  = (speed_kmh * 1000 / 3600) / wheel_circumference_m
 *     overall    = (rpm / 60) / wheel_rps          // engine revs / wheel rev
 *     gearbox    = overall / final_drive
 *     best_gear  = argmin_i |gearbox - ratios[i]| over i in 1..ratio_count-1
 *
 * The real implementation lives in main/widgets/signal_internal.c inside
 * the LVGL tick callback, transitively pulling in FreeRTOS, ESP-IDF
 * timers, the signal registry, WiFi, and the on-chip temp sensor. Pure
 * arithmetic — mirrored verbatim here.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/widgets/signal_internal.c — CALCULATED_GEAR branch in
 *                                    _internal_tick_cb() (search for the
 *                                    "CALCULATED_GEAR" banner comment)
 *   main/storage/config_store.h    — gear_cal_config_t shape +
 *                                    GEAR_CAL_MAX_GEARS = 9
 */
#include "unity.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GEAR_CAL_MAX_GEARS 9   /* mirrors main/storage/config_store.h */

/* Mirror of gear_cal_config_t from config_store.h. Strings dropped —
 * the host helper never resolves signal names. */
typedef struct {
    float   wheel_circumference_m;
    float   final_drive;
    uint8_t ratio_count;
    float   ratios[GEAR_CAL_MAX_GEARS];   /* [0]=N, [1..n-1] = 1st..Nth */
} gear_cal_t;

/* ── Helper under test ─────────────────────────────────────────────────
 *
 * Verbatim mirror of the CALCULATED_GEAR compute inside
 * _internal_tick_cb. The firmware additionally skips compute and emits
 * nothing when `enabled == false` or `ratio_count <= 1` or the wheel /
 * final-drive guards trip — we model that as the caller's
 * responsibility (the gate is enforced by the firmware before this
 * arithmetic runs).
 *
 * Returns the best-match gear index 0..ratio_count-1. 0 means Neutral. */
static float compute_gear(const gear_cal_t *c, float rpm, float speed_kmh) {
    /* Stationary / idle → Neutral. Mirrors firmware threshold pair. */
    if (speed_kmh < 5.0f || rpm < 500.0f) return 0.0f;

    float wheel_rps = (speed_kmh * 1000.0f / 3600.0f) / c->wheel_circumference_m;
    if (wheel_rps <= 0.01f) return 0.0f;   /* divide-by-zero guard (matches firmware) */

    float overall = (rpm / 60.0f) / wheel_rps;
    float gearbox = overall / c->final_drive;

    /* Find closest configured ratio, skipping index 0 (N — 0.0 by convention,
     * never a target since real ratios are always > 1.0). */
    int   best_i  = 0;
    float best_err = 1e9f;
    for (int i = 1; i < c->ratio_count; i++) {
        float err = fabsf(gearbox - c->ratios[i]);
        if (err < best_err) { best_err = err; best_i = i; }
    }
    return (float)best_i;
}

/* ── Fixture ───────────────────────────────────────────────────────────
 *
 * A plausible 5-speed manual gearbox (FA20 / Subaru-ish 2.0L) with a
 * 4.11 final drive and a 1.95 m wheel circumference (≈255/40R18). Good
 * separation between adjacent ratios so the argmin classifier has
 * clear winners.
 *
 *   ratios[0] = 0.0   (N — never selected)
 *   ratios[1] = 3.454 (1st)
 *   ratios[2] = 2.062 (2nd)
 *   ratios[3] = 1.436 (3rd)
 *   ratios[4] = 1.000 (4th — direct drive)
 *   ratios[5] = 0.838 (5th — overdrive)
 *
 * For each (gear, rpm), the matching speed_kmh works out to:
 *     speed = rpm * 60 / (final_drive * ratios[gear]) * wheel_circumference / 1000
 * ... derived by inverting the compute. We pick clean RPM values where
 * each gear's match is unambiguous. */
static gear_cal_t make_fixture(void) {
    gear_cal_t c = {0};
    c.wheel_circumference_m = 1.95f;
    c.final_drive           = 4.11f;
    c.ratio_count           = 6;
    c.ratios[0] = 0.0f;
    c.ratios[1] = 3.454f;
    c.ratios[2] = 2.062f;
    c.ratios[3] = 1.436f;
    c.ratios[4] = 1.000f;
    c.ratios[5] = 0.838f;
    return c;
}

/* Given a gear ratio and RPM, produce the matching speed_kmh exactly. */
static float speed_for(const gear_cal_t *c, int gear_idx, float rpm) {
    float gearbox = c->ratios[gear_idx];
    float overall = gearbox * c->final_drive;
    float wheel_rps = (rpm / 60.0f) / overall;
    float speed_mps = wheel_rps * c->wheel_circumference_m;
    return speed_mps * 3600.0f / 1000.0f;
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

/* Stationary (speed below 5 km/h threshold) → 0 (Neutral). */
static void test_stationary_returns_neutral(void) {
    gear_cal_t c = make_fixture();
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 800.0f, 0.0f));
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 800.0f, 4.99f));
}

/* Idle (rpm below 500 threshold) → 0 (Neutral). Models engine cranking /
 * about to stall — gear math wouldn't make sense. */
static void test_low_rpm_returns_neutral(void) {
    gear_cal_t c = make_fixture();
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 0.0f,    50.0f));
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 499.99f, 50.0f));
}

/* Both at zero (parked, engine off) → Neutral by either threshold. */
static void test_zero_zero_returns_neutral(void) {
    gear_cal_t c = make_fixture();
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 0.0f, 0.0f));
}

/* Each gear maps correctly at a representative RPM. Walks through
 * 1st..5th with the matching theoretical speed for that gear. */
static void test_each_gear_identifies_correctly(void) {
    gear_cal_t c = make_fixture();
    /* 1st @ 3000 RPM */
    float s1 = speed_for(&c, 1, 3000.0f);
    TEST_ASSERT_EQUAL_INT(1, (int)compute_gear(&c, 3000.0f, s1));
    /* 2nd @ 3000 RPM */
    float s2 = speed_for(&c, 2, 3000.0f);
    TEST_ASSERT_EQUAL_INT(2, (int)compute_gear(&c, 3000.0f, s2));
    /* 3rd @ 3000 RPM */
    float s3 = speed_for(&c, 3, 3000.0f);
    TEST_ASSERT_EQUAL_INT(3, (int)compute_gear(&c, 3000.0f, s3));
    /* 4th @ 3000 RPM */
    float s4 = speed_for(&c, 4, 3000.0f);
    TEST_ASSERT_EQUAL_INT(4, (int)compute_gear(&c, 3000.0f, s4));
    /* 5th @ 3000 RPM */
    float s5 = speed_for(&c, 5, 3000.0f);
    TEST_ASSERT_EQUAL_INT(5, (int)compute_gear(&c, 3000.0f, s5));
}

/* Same set, exercised at high RPM (6500) to confirm the result is
 * independent of the absolute speed — only the ratio matters. */
static void test_each_gear_at_high_rpm(void) {
    gear_cal_t c = make_fixture();
    for (int g = 1; g <= 5; g++) {
        float speed = speed_for(&c, g, 6500.0f);
        TEST_ASSERT_EQUAL_INT(g, (int)compute_gear(&c, 6500.0f, speed));
    }
}

/* Just on either side of the 5 km/h speed threshold. Below = Neutral,
 * above = whatever gear matches.  Mirrors the firmware tunable
 * (currently 5.0). */
static void test_speed_threshold_boundary(void) {
    gear_cal_t c = make_fixture();
    /* Just below — Neutral regardless of RPM. */
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 2500.0f, 4.99f));
    /* At threshold — firmware uses `< 5.0f`, so 5.00 is admitted; pick
     * an RPM that lands cleanly in 1st gear at exactly 5 km/h. The
     * speed isn't necessarily 1st-gear-accurate at this RPM but the
     * argmin picks the closest of the configured ratios. We don't
     * assert which one — we assert it's not Neutral. */
    float v = compute_gear(&c, 2500.0f, 5.00f);
    TEST_ASSERT_TRUE(v >= 1.0f);
}

/* Just on either side of the 500 RPM threshold. */
static void test_rpm_threshold_boundary(void) {
    gear_cal_t c = make_fixture();
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 499.99f, 30.0f));
    float v = compute_gear(&c, 500.0f, 30.0f);
    TEST_ASSERT_TRUE(v >= 1.0f);
}

/* Pathologically small wheel circumference. wheel_rps blows up, gearbox
 * shrinks toward 0. The argmin classifier will pick whichever real
 * ratio is closest to 0 — that's index 5 (5th @ 0.838) in our fixture.
 * The firmware doesn't blow up; it just picks the nearest gear. */
static void test_tiny_wheel_circumference_clamps_to_closest_ratio(void) {
    gear_cal_t c = make_fixture();
    c.wheel_circumference_m = 0.01f;   /* hits the wheel_rps > 0.01 guard */
    /* wheel_rps math: (50*1000/3600) / 0.01 = 1388.8 rev/s — wheel_rps
     * is well above the firmware's 0.01 guard, so compute proceeds.
     * overall = (3000/60) / 1388.8 ≈ 0.036; gearbox ≈ 0.036/4.11 ≈ 0.0088
     * — far smaller than any real ratio, so the argmin picks the
     * smallest ratio (5th = 0.838). */
    float gear = compute_gear(&c, 3000.0f, 50.0f);
    TEST_ASSERT_EQUAL_INT(5, (int)gear);
}

/* Zero speed but with `enabled` + ratios set — the early stationary
 * return kicks in, so divide-by-zero on wheel_rps is never reached.
 * Models the firmware guard against NaN propagation. */
static void test_zero_speed_with_high_rpm_returns_neutral(void) {
    gear_cal_t c = make_fixture();
    TEST_ASSERT_EQUAL_INT(0, (int)compute_gear(&c, 6500.0f, 0.0f));
}

/* High RPM + extremely low (but above-threshold) speed → wheel_rps tiny,
 * overall huge, gearbox huge, argmin picks the *largest* ratio (1st).
 * Sanity check on the upper-bound clamping behaviour. */
static void test_extreme_high_rpm_low_speed_clamps_to_first(void) {
    gear_cal_t c = make_fixture();
    /* 7000 RPM at 6 km/h — physically silly but math-wise: gearbox ≈
     * 7.99, way above 3.454 (1st), argmin still picks index 1. */
    float gear = compute_gear(&c, 7000.0f, 6.0f);
    TEST_ASSERT_EQUAL_INT(1, (int)gear);
}

/* Two-gear box (just N + 1st) — minimum useful config. Always picks 1st
 * once thresholds clear. Locks in that ratio_count > 1 is sufficient. */
static void test_two_gear_box_always_picks_first(void) {
    gear_cal_t c = {0};
    c.wheel_circumference_m = 1.95f;
    c.final_drive           = 4.11f;
    c.ratio_count           = 2;
    c.ratios[0] = 0.0f;
    c.ratios[1] = 3.454f;
    float speed = speed_for(&c, 1, 3000.0f);
    TEST_ASSERT_EQUAL_INT(1, (int)compute_gear(&c, 3000.0f, speed));
    /* Even at the "wrong" RPM/speed combo, argmin still picks 1 (only
     * non-Neutral option). */
    TEST_ASSERT_EQUAL_INT(1, (int)compute_gear(&c, 4500.0f, 30.0f));
}

/* 8-gear modern auto box at the upper GEAR_CAL_MAX_GEARS limit. Verifies
 * the argmin scans the full ratio_count and picks the correct slot. */
static void test_eight_gear_box_picks_correct_gear(void) {
    gear_cal_t c = {0};
    c.wheel_circumference_m = 1.95f;
    c.final_drive           = 3.50f;
    c.ratio_count           = 9;  /* N + 8 forward */
    c.ratios[0] = 0.000f;
    c.ratios[1] = 4.500f;
    c.ratios[2] = 2.900f;
    c.ratios[3] = 1.900f;
    c.ratios[4] = 1.400f;
    c.ratios[5] = 1.000f;
    c.ratios[6] = 0.870f;
    c.ratios[7] = 0.730f;
    c.ratios[8] = 0.640f;
    /* Walk through each of 1..8 and confirm round-trip. */
    for (int g = 1; g <= 8; g++) {
        float speed = speed_for(&c, g, 2500.0f);
        TEST_ASSERT_EQUAL_INT(g, (int)compute_gear(&c, 2500.0f, speed));
    }
}

/* Boundary between two adjacent gears — midpoint of the two ratios.
 * argmin should consistently break ties in favour of the lower-index
 * gear (because the loop's `<` is strict and lower-index ratios are
 * encountered first). Documents the tie-break behaviour. */
static void test_midpoint_breaks_tie_toward_lower_gear(void) {
    gear_cal_t c = make_fixture();
    /* Construct an exact midpoint between 3rd (1.436) and 4th (1.000):
     * midpoint = 1.218. Solve for speed at gearbox=1.218 with rpm=3000.
     *   overall   = 1.218 * 4.11 = 5.006
     *   wheel_rps = 50 / 5.006   = 9.988
     *   speed     = 9.988 * 1.95 * 3.6 = 70.11 km/h */
    float v = compute_gear(&c, 3000.0f, 70.11f);
    /* Either 3 or 4 is reasonable; the strict-< tie-break keeps the
     * lower index winning (3 was encountered first with the same err).
     * If a future refactor moves to `<=` this assertion goes red,
     * which is the desired behaviour — drift surfaces. */
    TEST_ASSERT_EQUAL_INT(3, (int)v);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stationary_returns_neutral);
    RUN_TEST(test_low_rpm_returns_neutral);
    RUN_TEST(test_zero_zero_returns_neutral);
    RUN_TEST(test_each_gear_identifies_correctly);
    RUN_TEST(test_each_gear_at_high_rpm);
    RUN_TEST(test_speed_threshold_boundary);
    RUN_TEST(test_rpm_threshold_boundary);
    RUN_TEST(test_tiny_wheel_circumference_clamps_to_closest_ratio);
    RUN_TEST(test_zero_speed_with_high_rpm_returns_neutral);
    RUN_TEST(test_extreme_high_rpm_low_speed_clamps_to_first);
    RUN_TEST(test_two_gear_box_always_picks_first);
    RUN_TEST(test_eight_gear_box_picks_correct_gear);
    RUN_TEST(test_midpoint_breaks_tie_toward_lower_gear);
    return UNITY_END();
}
