/* test_channel_math_fold.c — the running-unit rules of a calculated channel.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * A calculated channel used to be exactly two operands, `a op b`, and its unit
 * handling was written as a pair of special cases: commensurate the two channel
 * operands, then convert the result to the channel's own unit if the operator
 * was + / - or either operand was a constant. That reasoning does not survive a
 * third term, and a third term is what the useful fuel figure needs — fuel flow
 * (L/h) divided by speed (km/h) is litres per KILOMETRE, and the number a
 * driver reads, L/100km, is that multiplied by 100.
 *
 * So the special cases became one rule applied per operand: carry a running
 * unit, and let each fold decide what happens to it.
 *
 *   + and -  keep the dimension. Bring the operand into the running unit when
 *            both are known (kPa - psi means what it should); adopt the
 *            operand's unit when the running value is a bare constant.
 *   x and /  by a CONSTANT keep the dimension: scaling by a number does not
 *            change what the number measures. This is the rule that makes the
 *            "x 100" in an L/100km expression harmless.
 *   x and /  by a CHANNEL drop it: L/h / km/h is neither L/h nor km/h, and a
 *            linear unit_convert applied to a dimension it was never given
 *            would silently mis-scale the answer.
 *
 * The `derived` flag is the part that is easy to get wrong. A NULL running unit
 * means two different things — "a bare constant, which may still adopt a unit"
 * versus "a dimension we deliberately dropped, which must never adopt one
 * again" — and conflating them makes `100 - MAP - BARO` end up labelled kPa
 * while `MAP / BARO x 100` also ends up labelled kPa. Only the first is right.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * Same "mirror the firmware logic verbatim" approach as
 * test_arc_display_units.c / test_meter_display_units.c: the real evaluator is
 * an LVGL timer callback that reads the live signal registry, which would drag
 * in the graphics stack and the channel store. _fold itself is pure, so it is
 * mirrored here character for character, and the CONVERSION is not mirrored —
 * this links main/data/unit_convert.c straight out of the firmware, so the
 * factors under test are the shipping ones.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/data/channel_math.c, _fold()
 *   main/data/channel_math.c, _math_timer_cb()  (operand order + output convert)
 */
#include "unity.h"
#include "unit_convert.h"

#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Mirror of channel_math.c ─────────────────────────────────────────── */

typedef enum {
	CH_MATH_ADD = 0,
	CH_MATH_SUB = 1,
	CH_MATH_MUL = 2,
	CH_MATH_DIV = 3,
} channel_math_op_t;

static bool _fold(float *v, const char **unit, bool *derived,
                  uint8_t op, float ov, const char *ounit) {
	switch (op) {
	case CH_MATH_ADD:
	case CH_MATH_SUB:
		if (*unit && ounit && strcmp(ounit, *unit) != 0)
			ov = unit_convert(ov, ounit, *unit);
		*v = (op == CH_MATH_ADD) ? (*v + ov) : (*v - ov);
		if (!*unit && ounit && !*derived) *unit = ounit;
		return true;
	case CH_MATH_MUL:
		*v *= ov;
		if (ounit) {
			if (!*unit && !*derived) *unit = ounit;   /* number x channel */
			else { *unit = NULL; *derived = true; }
		}
		return true;
	case CH_MATH_DIV:
		if (fabsf(ov) < 1e-9f) return false;
		*v /= ov;
		if (ounit) { *unit = NULL; *derived = true; }
		return true;
	default:
		return false;
	}
}

/* ── Harness: one whole expression, the way _math_timer_cb runs it ────── */

typedef struct {
	float       v;
	const char *unit;   /* NULL = a bare constant */
} operand_t;

#define CONST(x)      ((operand_t){ (x), NULL })
#define CHAN(x, u)    ((operand_t){ (x), (u) })
#define NO_THIRD_TERM ((operand_t){ 0.0f, "" })   /* sentinel, see _eval */

typedef struct {
	bool  ok;           /* false = evaluator skipped this tick */
	float value;
	const char *unit;   /* running unit at the end, NULL once dropped */
} result_t;

/* @out_native is the calculated channel's own units_native ("" for none). */
static result_t _eval(operand_t a, uint8_t op, operand_t b,
                      uint8_t op2, operand_t c, const char *out_native) {
	result_t r = { false, 0.0f, NULL };

	float v = a.v;
	const char *unit = a.unit;
	bool derived = false;

	if (!_fold(&v, &unit, &derived, op, b.v, b.unit)) return r;

	/* The "" unit marks the absent third term — a real operand is either a
	 * channel with a unit or a constant with NULL. */
	bool has_c = !(c.unit && c.unit[0] == '\0');
	if (has_c && !_fold(&v, &unit, &derived, op2, c.v, c.unit)) return r;

	if (unit && out_native && out_native[0] && strcmp(unit, out_native) != 0)
		v = unit_convert(v, unit, out_native);

	r.ok = true; r.value = v; r.unit = unit;
	return r;
}

/* ── The expression this whole feature exists for ─────────────────────── */

static void test_litres_per_100km_is_flow_over_speed_times_100(void) {
	/* 7.5 L/h at 90 km/h. Per kilometre that is 0.08333 L; the figure a dash
	 * shows is 8.33 L/100km. Two operands cannot say this at all — the second
	 * multiply is the entire point of the third term. */
	result_t r = _eval(CHAN(7.5f, "L/h"), CH_MATH_DIV, CHAN(90.0f, "km/h"),
	                   CH_MATH_MUL, CONST(100.0f), "L/100km");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.333f, r.value);
	/* Dropped by the channel-over-channel divide and never picked back up,
	 * so the "L/100km" label is left alone rather than being converted into. */
	TEST_ASSERT_NULL(r.unit);
}

static void test_scaling_by_a_number_does_not_mis_convert_the_output(void) {
	/* The trap the `derived` flag guards. If x100 by a constant were allowed
	 * to re-adopt km/h from the divisor, the final output convert would then
	 * treat 8.33 as a speed and "convert" it — a wrong number, silently. */
	result_t r = _eval(CHAN(7.5f, "L/h"), CH_MATH_DIV, CHAN(90.0f, "km/h"),
	                   CH_MATH_MUL, CONST(100.0f), "mph");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.333f, r.value);
}

static void test_stopped_car_skips_the_tick_rather_than_reading_infinity(void) {
	/* Divide by ~zero returns false, the evaluator skips the push, and the
	 * channel goes stale through the normal 2 s signal timeout. Stationary
	 * economy reads "--", not "inf". */
	result_t r = _eval(CHAN(7.5f, "L/h"), CH_MATH_DIV, CHAN(0.0f, "km/h"),
	                   CH_MATH_MUL, CONST(100.0f), "L/100km");
	TEST_ASSERT_FALSE(r.ok);
}

static void test_third_term_can_be_a_channel(void) {
	/* (a + b) / c, all channels. Nothing about the third term requires a
	 * constant — that is just the common case. */
	result_t r = _eval(CHAN(10.0f, "kPa"), CH_MATH_ADD, CHAN(20.0f, "kPa"),
	                   CH_MATH_DIV, CHAN(2.0f, "kPa"), "");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 15.0f, r.value);
	TEST_ASSERT_NULL(r.unit);
}

/* ── Two-operand behaviour must be exactly what it always was ─────────── */

static void test_boost_is_map_minus_baro(void) {
	result_t r = _eval(CHAN(180.0f, "kPa"), CH_MATH_SUB, CHAN(101.3f, "kPa"),
	                   0, NO_THIRD_TERM, "kPa");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 78.7f, r.value);
	TEST_ASSERT_EQUAL_STRING("kPa", r.unit);
}

static void test_mixed_units_commensurate_before_subtracting(void) {
	/* 14.5038 psi is one atmosphere. Subtracting it from 180 kPa must go
	 * through the conversion, not through the raw number. */
	result_t r = _eval(CHAN(180.0f, "kPa"), CH_MATH_SUB, CHAN(14.5038f, "psi"),
	                   0, NO_THIRD_TERM, "kPa");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.05f, 80.0f, r.value);
	TEST_ASSERT_EQUAL_STRING("kPa", r.unit);
}

static void test_result_converts_into_the_channels_own_unit(void) {
	/* Same subtraction, but this channel is labelled psi: 78.7 kPa = 11.4 psi. */
	result_t r = _eval(CHAN(180.0f, "kPa"), CH_MATH_SUB, CHAN(101.3f, "kPa"),
	                   0, NO_THIRD_TERM, "psi");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 11.415f, r.value);
}

static void test_constant_adopts_the_channel_operands_unit(void) {
	/* A constant carries no unit of its own; "MAP - 50" is 50 kPa. */
	result_t r = _eval(CHAN(180.0f, "kPa"), CH_MATH_SUB, CONST(50.0f),
	                   0, NO_THIRD_TERM, "kPa");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 130.0f, r.value);
	TEST_ASSERT_EQUAL_STRING("kPa", r.unit);
}

static void test_constant_first_still_lands_in_the_channels_unit(void) {
	/* "100 - MAP": the running value starts as a bare constant with no unit,
	 * and the channel operand hands it one. Distinguishing that NULL from a
	 * dropped-dimension NULL is what `derived` is for. */
	result_t r = _eval(CONST(100.0f), CH_MATH_SUB, CHAN(30.0f, "kPa"),
	                   0, NO_THIRD_TERM, "kPa");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 70.0f, r.value);
	TEST_ASSERT_EQUAL_STRING("kPa", r.unit);
}

static void test_channel_times_constant_keeps_its_unit(void) {
	result_t r = _eval(CHAN(2.0f, "bar"), CH_MATH_MUL, CONST(1.6f),
	                   0, NO_THIRD_TERM, "bar");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, r.value);
	TEST_ASSERT_EQUAL_STRING("bar", r.unit);
}

static void test_channel_times_channel_drops_the_unit(void) {
	/* kPa x kPa is not kPa, so the output must NOT be converted into the
	 * channel's own unit — the number would be scaled by a factor that means
	 * nothing for this dimension. */
	result_t r = _eval(CHAN(4.0f, "kPa"), CH_MATH_MUL, CHAN(3.0f, "kPa"),
	                   0, NO_THIRD_TERM, "psi");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, r.value);
	TEST_ASSERT_NULL(r.unit);
}

static void test_unknown_unit_pairs_pass_through_unchanged(void) {
	/* unit_convert is the identity for pairs it does not know, so a relabel
	 * without a known conversion shows the same number rather than a scaled
	 * one. Nothing in the fold rules should defeat that. */
	result_t r = _eval(CHAN(10.0f, "widgets"), CH_MATH_ADD, CHAN(5.0f, "sprockets"),
	                   0, NO_THIRD_TERM, "gizmos");
	TEST_ASSERT_TRUE(r.ok);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 15.0f, r.value);
}

int main(void) {
	UNITY_BEGIN();
	RUN_TEST(test_litres_per_100km_is_flow_over_speed_times_100);
	RUN_TEST(test_scaling_by_a_number_does_not_mis_convert_the_output);
	RUN_TEST(test_stopped_car_skips_the_tick_rather_than_reading_infinity);
	RUN_TEST(test_third_term_can_be_a_channel);
	RUN_TEST(test_boost_is_map_minus_baro);
	RUN_TEST(test_mixed_units_commensurate_before_subtracting);
	RUN_TEST(test_result_converts_into_the_channels_own_unit);
	RUN_TEST(test_constant_adopts_the_channel_operands_unit);
	RUN_TEST(test_constant_first_still_lands_in_the_channels_unit);
	RUN_TEST(test_channel_times_constant_keeps_its_unit);
	RUN_TEST(test_channel_times_channel_drops_the_unit);
	RUN_TEST(test_unknown_unit_pairs_pass_through_unchanged);
	return UNITY_END();
}
