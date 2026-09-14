/* test_fuel_stoich.c — what λ 1.00 is in AFR, for whichever fuel is in the tank.
 *
 * ── Why this exists ─────────────────────────────────────────────────────
 *
 * The dash converted λ -> AFR with a hardcoded 14.7. A Link customer asked how
 * to see AFR instead of lambda; the answer ("Display as AFR") was right for
 * petrol and quietly wrong for E85, where it showed petrol-equivalent AFR —
 * "14.7" at stoich while the tuning software said 9.8. The ratio is now a
 * setting, and for flex-fuel cars it tracks the ethanol sensor.
 *
 * Links the REAL unit_convert.c and includes the REAL fuel_stoich_calc.h, so
 * the ratios and the conversion under test are the shipping ones. The runtime
 * half (NVS, the flex timer, notifying widgets) needs LVGL and the channel
 * store, so it is covered on hardware rather than here.
 */
#include "unity.h"
#include "unit_convert.h"
#include "fuel_stoich_calc.h"

#include <math.h>

#define LAMBDA "\xce\xbb"

static void reset(void) { unit_convert_set_stoich(UNIT_STOICH_PETROL); }

/* ── The default must be exactly what shipped before ─────────────────── */

static void test_default_stoich_is_the_old_hardcoded_petrol_value(void) {
	/* Nobody who never opens the fuel setting may see a different number
	 * after updating. 14.7 is what the table always used. */
	reset();
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 14.7f, unit_convert_get_stoich());
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.7f, unit_convert(1.0f, LAMBDA, "AFR"));
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.495f, unit_convert(0.85f, LAMBDA, "AFR"));
}

static void test_petrol_mode_reproduces_the_default(void) {
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 14.7f, fuel_stoich_for_mode(FUEL_PETROL));
}

/* ── The fuels ──────────────────────────────────────────────────────── */

static void test_e85_is_the_figure_tuners_quote(void) {
	/* 9.8 is the number on every E85 tune sheet. Interpolating by VOLUME
	 * would give 9.86; AFR mixes by MASS, which gives 9.81. */
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 9.81f, fuel_stoich_for_mode(FUEL_E85));
}

static void test_e10_and_the_pure_fuels(void) {
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 14.10f, fuel_stoich_for_mode(FUEL_E10));
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.0f,   fuel_stoich_for_mode(FUEL_E100));
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.4f,   fuel_stoich_for_mode(FUEL_METHANOL));
}

static void test_flex_at_85_percent_equals_the_e85_setting(void) {
	/* The presets are derived from the same function the flex path uses, so
	 * a flex car reading 85% and a car set to "E85" cannot disagree. */
	TEST_ASSERT_FLOAT_WITHIN(0.00001f, fuel_stoich_for_mode(FUEL_E85),
	                         fuel_stoich_for_ethanol(85.0f));
	TEST_ASSERT_FLOAT_WITHIN(0.00001f, fuel_stoich_for_mode(FUEL_E10),
	                         fuel_stoich_for_ethanol(10.0f));
}

static void test_flex_falls_monotonically_from_petrol_to_ethanol(void) {
	float prev = fuel_stoich_for_ethanol(0.0f);
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, FUEL_AFR_PETROL, prev);
	for (int pct = 5; pct <= 100; pct += 5) {
		float s = fuel_stoich_for_ethanol((float)pct);
		TEST_ASSERT_TRUE(s < prev);
		prev = s;
	}
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, FUEL_AFR_ETHANOL, prev);
}

static void test_a_bad_ethanol_reading_clamps_to_a_real_fuel(void) {
	/* A sensor fault must not produce a ratio no fuel has. */
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, FUEL_AFR_PETROL,  fuel_stoich_for_ethanol(-12.0f));
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, FUEL_AFR_ETHANOL, fuel_stoich_for_ethanol(140.0f));
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, FUEL_AFR_PETROL,  fuel_stoich_for_ethanol(NAN));
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, FUEL_AFR_PETROL,  fuel_stoich_for_ethanol(INFINITY));
}

/* ── The conversion follows the setting, both ways ─────────────────── */

static void test_lambda_to_afr_uses_the_chosen_fuel(void) {
	reset();
	TEST_ASSERT_TRUE(unit_convert_set_stoich(fuel_stoich_for_mode(FUEL_E85)));
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 9.81f, unit_convert(1.0f, LAMBDA, "AFR"));
	/* Rich of stoich on E85 — the number the customer's tuner would show. */
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 7.85f, unit_convert(0.80f, LAMBDA, "AFR"));
	reset();
}

static void test_afr_back_to_lambda_uses_the_same_ratio(void) {
	/* Ranges and thresholds are typed in the display unit and converted back
	 * to native before they are stored, so the reverse must invert exactly. */
	reset();
	unit_convert_set_stoich(fuel_stoich_for_mode(FUEL_E85));
	float afr = unit_convert(0.92f, LAMBDA, "AFR");
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.92f, unit_convert(afr, "AFR", LAMBDA));
	reset();
}

static void test_changing_fuel_does_not_touch_other_conversions(void) {
	reset();
	unit_convert_set_stoich(FUEL_AFR_METHANOL);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 14.5038f, unit_convert(100.0f, "kPa", "psi"));
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 212.0f,   unit_convert(100.0f, "\xc2\xb0" "C", "\xc2\xb0" "F"));
	reset();
}

static void test_a_nonsense_ratio_is_refused_and_the_old_one_kept(void) {
	/* A bad value would silently mis-scale every AFR readout on the dash, so
	 * the setter refuses it rather than trusting the caller. */
	reset();
	unit_convert_set_stoich(9.81f);
	TEST_ASSERT_FALSE(unit_convert_set_stoich(0.0f));
	TEST_ASSERT_FALSE(unit_convert_set_stoich(-3.0f));
	TEST_ASSERT_FALSE(unit_convert_set_stoich(147.0f));   /* a slipped decimal */
	TEST_ASSERT_FALSE(unit_convert_set_stoich(NAN));
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 9.81f, unit_convert_get_stoich());
	TEST_ASSERT_TRUE(unit_convert_set_stoich(UNIT_STOICH_MIN));
	TEST_ASSERT_TRUE(unit_convert_set_stoich(UNIT_STOICH_MAX));
	reset();
}

static void test_afr_is_still_offered_for_a_lambda_channel(void) {
	/* "Display as" builds its list from the table; the dynamic ratio must not
	 * have hidden the pair. */
	TEST_ASSERT_TRUE(unit_convert_supported(LAMBDA, "AFR"));
	TEST_ASSERT_TRUE(unit_convert_supported("AFR", LAMBDA));
	const char *out[8];
	size_t n = unit_convert_targets(LAMBDA, out, 8);
	TEST_ASSERT_EQUAL_INT(1, (int)n);
	TEST_ASSERT_EQUAL_STRING("AFR", out[0]);
}

/* ── The API key is the contract, not the enum value ───────────────── */

static void test_every_mode_round_trips_through_its_key(void) {
	for (int i = 0; i < FUEL__COUNT; ++i) {
		fuel_mode_t m;
		TEST_ASSERT_TRUE(fuel_mode_from_key(fuel_mode_key((fuel_mode_t)i), &m));
		TEST_ASSERT_EQUAL_INT(i, (int)m);
	}
}

static void test_an_unknown_key_is_rejected(void) {
	fuel_mode_t m = FUEL_E85;
	TEST_ASSERT_FALSE(fuel_mode_from_key("diesel", &m));
	TEST_ASSERT_FALSE(fuel_mode_from_key("", &m));
	TEST_ASSERT_FALSE(fuel_mode_from_key(NULL, &m));
	TEST_ASSERT_EQUAL_INT(FUEL_E85, (int)m);   /* untouched on failure */
}

int main(void) {
	UNITY_BEGIN();
	RUN_TEST(test_default_stoich_is_the_old_hardcoded_petrol_value);
	RUN_TEST(test_petrol_mode_reproduces_the_default);
	RUN_TEST(test_e85_is_the_figure_tuners_quote);
	RUN_TEST(test_e10_and_the_pure_fuels);
	RUN_TEST(test_flex_at_85_percent_equals_the_e85_setting);
	RUN_TEST(test_flex_falls_monotonically_from_petrol_to_ethanol);
	RUN_TEST(test_a_bad_ethanol_reading_clamps_to_a_real_fuel);
	RUN_TEST(test_lambda_to_afr_uses_the_chosen_fuel);
	RUN_TEST(test_afr_back_to_lambda_uses_the_same_ratio);
	RUN_TEST(test_changing_fuel_does_not_touch_other_conversions);
	RUN_TEST(test_a_nonsense_ratio_is_refused_and_the_old_one_kept);
	RUN_TEST(test_afr_is_still_offered_for_a_lambda_channel);
	RUN_TEST(test_every_mode_round_trips_through_its_key);
	RUN_TEST(test_an_unknown_key_is_rejected);
	return UNITY_END();
}
