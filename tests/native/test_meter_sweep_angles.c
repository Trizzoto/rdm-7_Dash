/* test_meter_sweep_angles.c — the dial's start/sweep, in both angle spaces.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * A dial is stored as two ABSOLUTE LVGL angles, start_angle and end_angle,
 * where 0° is 3 o'clock. Both inspectors present something different: a
 * START and a SWEEP, with 0° at 12 o'clock. So lvgl = user + 270, and the
 * sweep is end − start.
 *
 * The web editor did that conversion (see _meterDeriveLvglAngles in
 * main/web/index.html: `lvglStart = (userStart + 270) % 360`). The on-device
 * inspector did not — `_meter_inspector_get` handed back md->start_angle
 * RAW under the name "start_angle_user", and the setter wrote it back raw.
 * Two consequences, both silent:
 *
 *   1. The same dial read 225 in Studio and 135 on the glass.
 *   2. Typing a start angle on the glass moved the start but left end_angle
 *      alone, so the sweep silently resized — a 270° dial became a 315° one
 *      just by nudging where it began.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * Same "mirror the firmware logic verbatim" approach as
 * test_meter_display_units.c and test_widget_arc_precedence.c: the real
 * hooks live in static functions inside widget_meter.c, which writes to
 * LVGL and would drag in the graphics stack. The arithmetic below is copied
 * from _meter_inspector_get / _meter_inspector_set, and the web editor's
 * rule is stated independently so the two surfaces are asserted to AGREE
 * rather than merely to be self-consistent.
 */
#include "unity.h"

#include <stdint.h>

/* ── Mirror of the meter's stored angle pair ─────────────────────────── */
typedef struct {
	int16_t start_angle;   /* LVGL space: 0° = 3 o'clock */
	int16_t end_angle;
} meter_angles_t;

/* Mirror of _meter_inspector_get, "start_angle_user" + "sweep_degrees". */
static int fw_get_start_user(const meter_angles_t *md)
{
	return ((int)md->start_angle + 90) % 360;
}

static int fw_get_sweep(const meter_angles_t *md)
{
	int sweep = ((int)md->end_angle - (int)md->start_angle + 360) % 360;
	if (sweep == 0) sweep = 360;   /* start==end → full revolution */
	return sweep;
}

/* Mirror of _meter_inspector_set, "start_angle_user". */
static void fw_set_start_user(meter_angles_t *md, int v)
{
	int sweep = ((int)md->end_angle - (int)md->start_angle + 360) % 360;
	if (sweep == 0) sweep = 360;
	v %= 360; if (v < 0) v += 360;
	md->start_angle = (int16_t)((v + 270) % 360);
	md->end_angle   = (int16_t)(((int)md->start_angle + sweep) % 360);
}

/* Mirror of _meter_inspector_set, "sweep_degrees". */
static void fw_set_sweep(meter_angles_t *md, int v)
{
	if (v < 1) v = 1;
	if (v > 360) v = 360;
	md->end_angle = (int16_t)(((int)md->start_angle + v) % 360);
}

/* The WEB editor's rule, stated independently (index.html
 * _meterDeriveLvglAngles / _arcDeriveLvglAngles). If this and the firmware
 * ever disagree again, these tests are where it shows up. */
static int web_lvgl_start(int user_start) { return (((user_start % 360) + 360) % 360 + 270) % 360; }
static int web_lvgl_end(int user_start, int sweep) { return (web_lvgl_start(user_start) + sweep) % 360; }

/* ── The default dial ────────────────────────────────────────────────── */

static void test_default_dial_reads_back_as_authored(void)
{
	/* index.html: "Defaults start_angle_user=225, sweep=270 reproduce
	 * LVGL start=135/end=45." */
	meter_angles_t md = { .start_angle = 135, .end_angle = 45 };
	TEST_ASSERT_EQUAL_INT(225, fw_get_start_user(&md));
	TEST_ASSERT_EQUAL_INT(270, fw_get_sweep(&md));
}

static void test_raw_lvgl_angle_is_not_the_user_angle(void)
{
	/* The regression itself: returning the raw field would give 135. */
	meter_angles_t md = { .start_angle = 135, .end_angle = 45 };
	TEST_ASSERT_TRUE((int)md.start_angle != fw_get_start_user(&md));
	TEST_ASSERT_EQUAL_INT(135, (int)md.start_angle);
}

/* ── The two surfaces agree ──────────────────────────────────────────── */

static void test_firmware_matches_the_web_editor_for_every_start(void)
{
	for (int user = 0; user < 360; user++) {
		meter_angles_t md = { .start_angle = 0, .end_angle = 0 };
		fw_set_start_user(&md, user);
		TEST_ASSERT_EQUAL_INT(web_lvgl_start(user), md.start_angle);
		/* and it round-trips back to what was typed */
		TEST_ASSERT_EQUAL_INT(user, fw_get_start_user(&md));
	}
}

static void test_firmware_matches_the_web_editor_for_start_and_sweep(void)
{
	const int sweeps[] = { 1, 90, 180, 270, 359, 360 };
	for (int s = 0; s < (int)(sizeof(sweeps) / sizeof(sweeps[0])); s++) {
		for (int user = 0; user < 360; user += 15) {
			meter_angles_t md = { .start_angle = 0, .end_angle = 0 };
			fw_set_start_user(&md, user);
			fw_set_sweep(&md, sweeps[s]);
			TEST_ASSERT_EQUAL_INT(web_lvgl_start(user), md.start_angle);
			TEST_ASSERT_EQUAL_INT(web_lvgl_end(user, sweeps[s]), md.end_angle);
			TEST_ASSERT_EQUAL_INT(sweeps[s], fw_get_sweep(&md));
		}
	}
}

/* ── Moving the start rotates the dial, it does not resize it ────────── */

static void test_changing_start_preserves_sweep(void)
{
	meter_angles_t md = { .start_angle = 135, .end_angle = 45 };   /* 270° */
	for (int user = 0; user < 360; user += 7) {
		fw_set_start_user(&md, user);
		TEST_ASSERT_EQUAL_INT(270, fw_get_sweep(&md));
	}
}

static void test_changing_start_preserves_a_full_circle(void)
{
	/* start==end is a full revolution, not a 0° dial — it must survive
	 * being rotated (the naive sweep calc would collapse it to 0). */
	meter_angles_t md = { .start_angle = 135, .end_angle = 135 };
	TEST_ASSERT_EQUAL_INT(360, fw_get_sweep(&md));
	fw_set_start_user(&md, 90);
	TEST_ASSERT_EQUAL_INT(360, fw_get_sweep(&md));
	TEST_ASSERT_EQUAL_INT(md.start_angle, md.end_angle);
}

/* ── Sweep edges ─────────────────────────────────────────────────────── */

static void test_sweep_360_stores_start_equal_end(void)
{
	meter_angles_t md = { .start_angle = 135, .end_angle = 45 };
	fw_set_sweep(&md, 360);
	TEST_ASSERT_EQUAL_INT(md.start_angle, md.end_angle);
	TEST_ASSERT_EQUAL_INT(360, fw_get_sweep(&md));   /* not 0 */
}

static void test_sweep_is_clamped_to_the_drawable_range(void)
{
	meter_angles_t md = { .start_angle = 135, .end_angle = 45 };
	fw_set_sweep(&md, 0);
	TEST_ASSERT_EQUAL_INT(1, fw_get_sweep(&md));
	fw_set_sweep(&md, 5000);
	TEST_ASSERT_EQUAL_INT(360, fw_get_sweep(&md));
}

static void test_negative_and_oversized_start_wrap(void)
{
	meter_angles_t md = { .start_angle = 135, .end_angle = 45 };
	fw_set_start_user(&md, -45);
	TEST_ASSERT_EQUAL_INT(315, fw_get_start_user(&md));
	fw_set_start_user(&md, 585);            /* 585 - 360 = 225 */
	TEST_ASSERT_EQUAL_INT(225, fw_get_start_user(&md));
	TEST_ASSERT_EQUAL_INT(135, md.start_angle);
}

/* ── The presets the editor offers ───────────────────────────────────── */

static void test_centred_start_rule_reproduces_the_classic_dial(void)
{
	/* index.html _sweepCenteredStart: start = (360 - sweep/2) % 360, which
	 * centres the sweep on 12 o'clock. The ¾ preset must land exactly on
	 * the shipped default rather than near it. */
	const int sweeps[]  = { 360, 270, 180,  90 };
	const int expected[] = { 180, 225, 270, 315 };
	for (int i = 0; i < 4; i++) {
		int start = ((360 - sweeps[i] / 2) % 360 + 360) % 360;
		TEST_ASSERT_EQUAL_INT(expected[i], start);
	}
}

static void test_stops_at_readout_is_the_sum(void)
{
	/* The "Stops at" field is start + sweep, wrapped — the arithmetic the
	 * user had to do in their head before. */
	meter_angles_t md = { .start_angle = 0, .end_angle = 0 };
	fw_set_start_user(&md, 225);
	fw_set_sweep(&md, 270);
	int stop = (225 + 270) % 360;
	TEST_ASSERT_EQUAL_INT(135, stop);
	/* Compute first, assert second: unity.h stringifies the actual-expression
	 * into its printf format string, so a '%' inside the macro argument
	 * becomes a bogus conversion spec — a warning, and run.ps1 treats any
	 * compiler stderr as a build failure. */
	int derived = (fw_get_start_user(&md) + fw_get_sweep(&md)) % 360;
	TEST_ASSERT_EQUAL_INT(stop, derived);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_default_dial_reads_back_as_authored);
	RUN_TEST(test_raw_lvgl_angle_is_not_the_user_angle);
	RUN_TEST(test_firmware_matches_the_web_editor_for_every_start);
	RUN_TEST(test_firmware_matches_the_web_editor_for_start_and_sweep);
	RUN_TEST(test_changing_start_preserves_sweep);
	RUN_TEST(test_changing_start_preserves_a_full_circle);
	RUN_TEST(test_sweep_360_stores_start_equal_end);
	RUN_TEST(test_sweep_is_clamped_to_the_drawable_range);
	RUN_TEST(test_negative_and_oversized_start_wrap);
	RUN_TEST(test_centred_start_rule_reproduces_the_classic_dial);
	RUN_TEST(test_stops_at_readout_is_the_sum);
	return UNITY_END();
}
