/*
 * Tests for the GPS lap timing engine.
 *
 * Unlike test_calculated_gear.c and test_ecu_preset_match.c, this test
 * COMPILES THE FIRMWARE SOURCE DIRECTLY (main/lap/lap_core.c) rather than
 * mirroring its arithmetic here. That is possible because lap_core.c has no
 * LVGL/FreeRTOS/ESP-IDF dependency by design, and it is strictly better: a
 * mirrored test can drift out of lockstep with the code it claims to cover
 * without anything failing. Prefer this pattern for new modules.
 *
 * The circuit simulator below drives a car round a circle at a chosen speed,
 * emitting fixes at a chosen rate, which is close enough to the real thing to
 * exercise interpolation, distance integration and the predictive delta
 * together.
 */

#include <math.h>
#include <string.h>

#include "unity.h"

#include "lap/lap_core.h"

/* Not ISO C — see the matching note in lap_core.c. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_R 6371008.8
#define R2D (180.0 / M_PI)

/* ── Local-metres helpers, independent of the implementation ─────────────
 * Origin is (0, 0) so cos(lat0) == 1 and the conversion is a clean scale. */
static double north_to_lat(double north_m) {
	return north_m / EARTH_R * R2D;
}

static double east_to_lon(double east_m) {
	return east_m / EARTH_R * R2D;
}

/* A fix as a pointer, for the many one-liner calls below. A compound literal
 * is an lvalue in C, so taking its address is legal — unlike `&fix_at(...)`,
 * which is not. Lifetime is the enclosing block, which is all we need. */
#define FIX(east_m_, north_m_, t_ms_, kph_)                                                        \
	(&(lap_fix_t){.lat_deg = north_to_lat(north_m_),                                               \
	              .lon_deg = east_to_lon(east_m_),                                                 \
	              .speed_kph = (kph_),                                                             \
	              .t_ms = (t_ms_),                                                                 \
	              .usable = true})

static lap_fix_t fix_at(double east_m, double north_m, uint32_t t_ms, float kph) {
	lap_fix_t f;
	f.lat_deg = north_to_lat(north_m);
	f.lon_deg = east_to_lon(east_m);
	f.speed_kph = kph;
	f.t_ms = t_ms;
	f.usable = true;
	return f;
}

/* A start/finish line at the origin, crossed heading due north. */
static lap_line_t line_north(void) {
	lap_line_t l;
	l.lat_deg = 0.0;
	l.lon_deg = 0.0;
	l.heading_deg = 0.0f;
	l.half_width_m = 10.0f;
	return l;
}

static lap_track_t track_simple(void) {
	lap_track_t t;
	memset(&t, 0, sizeof(t));
	strcpy(t.name, "test");
	t.start_finish = line_north();
	t.sector_count = 0;
	t.min_lap_time_s = 10.0f;
	return t;
}

/* ══════════════════════════════════════════════════════════════════════
 * Geometry
 * ════════════════════════════════════════════════════════════════════ */

static void test_projection_scales_correctly(void) {
	double e, n;
	/* 0.001 deg of latitude is about 111.2 m anywhere. */
	lap_project_local(0.0, 0.0, 0.001, 0.0, &e, &n);
	TEST_ASSERT_TRUE(fabs(n - 111.19) < 0.1);
	TEST_ASSERT_TRUE(fabs(e) < 0.001);

	/* At the equator a degree of longitude is the same size as one of
	 * latitude; the cos(lat0) factor is what makes that stop being true
	 * further north, so check it actually shrinks. */
	lap_project_local(60.0, 0.0, 60.0, 0.001, &e, &n);
	TEST_ASSERT_TRUE(fabs(e - 55.6) < 0.5); /* cos(60) = 0.5 */
}

static void test_signed_distance_sign_convention(void) {
	lap_line_t l = line_north();

	/* South of the line, heading north: not there yet, so negative. */
	float d = lap_line_signed_distance(&l, north_to_lat(-25.0), 0.0, NULL);
	TEST_ASSERT_TRUE(fabsf(d + 25.0f) < 0.05f);

	/* North of it: past, so positive. */
	d = lap_line_signed_distance(&l, north_to_lat(40.0), 0.0, NULL);
	TEST_ASSERT_TRUE(fabsf(d - 40.0f) < 0.05f);

	/* On it. */
	d = lap_line_signed_distance(&l, 0.0, 0.0, NULL);
	TEST_ASSERT_TRUE(fabsf(d) < 0.05f);
}

static void test_lateral_offset(void) {
	lap_line_t l = line_north();
	float lateral = 0.0f;

	/* 30 m east of the line centre, on the line itself. */
	lap_line_signed_distance(&l, 0.0, east_to_lon(30.0), &lateral);
	TEST_ASSERT_TRUE(fabsf(lateral - 30.0f) < 0.05f);

	/* West is the other sign. */
	lap_line_signed_distance(&l, 0.0, east_to_lon(-30.0), &lateral);
	TEST_ASSERT_TRUE(fabsf(lateral + 30.0f) < 0.05f);
}

static void test_heading_rotates_the_line(void) {
	/* Same line rotated to "crossed heading east": now the signed distance
	 * runs east-west and the lateral offset runs north-south. */
	lap_line_t l = line_north();
	l.heading_deg = 90.0f;

	float lateral = 0.0f;
	float d = lap_line_signed_distance(&l, 0.0, east_to_lon(20.0), &lateral);
	TEST_ASSERT_TRUE(fabsf(d - 20.0f) < 0.05f);
	TEST_ASSERT_TRUE(fabsf(lateral) < 0.05f);

	d = lap_line_signed_distance(&l, north_to_lat(20.0), 0.0, &lateral);
	TEST_ASSERT_TRUE(fabsf(d) < 0.05f);
	TEST_ASSERT_TRUE(fabsf(lateral + 20.0f) < 0.05f);
}

/* ══════════════════════════════════════════════════════════════════════
 * Crossing detection
 * ════════════════════════════════════════════════════════════════════ */

static void test_first_crossing_arms_but_does_not_time_a_lap(void) {
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	lap_core_set_track(&e, &t);

	TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, &(lap_fix_t){
	    .lat_deg = north_to_lat(-2), .lon_deg = 0, .speed_kph = 100, .t_ms = 1000, .usable = true}));
	TEST_ASSERT_EQUAL_INT(LAP_EVT_ARMED, lap_core_update(&e, &(lap_fix_t){
	    .lat_deg = north_to_lat(2), .lon_deg = 0, .speed_kph = 100, .t_ms = 1040, .usable = true}));

	/* Lap 1 is now running, but no lap TIME exists yet — reporting one here
	 * would mean timing from ignition-on, including however long the car sat
	 * in the garage. */
	TEST_ASSERT_EQUAL_INT(1, e.lap_number);
	TEST_ASSERT_EQUAL_FLOAT(0.0f, e.lap_time_last);
	TEST_ASSERT_EQUAL_FLOAT(0.0f, e.lap_time_best);
}

static void test_crossing_is_interpolated_between_fixes(void) {
	/* This is the whole ballgame: at 25 Hz the fixes are 40 ms apart, but the
	 * crossing instant lands anywhere inside that interval. The claimed
	 * accuracy of the product (+/-0.02 s) is exactly this interpolation. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	lap_core_set_track(&e, &t);

	/* Arm: fixes at -2 m and +2 m, so the crossing is at exactly half the
	 * interval — t = 1020 ms. */
	lap_core_update(&e, FIX(0, -2, 1000, 100));
	lap_core_update(&e, FIX(0, 2, 1040, 100));

	/* Reposition south of the line. Going from +2 to -2 is the wrong
	 * direction and must NOT register (that is the slow-down lap driving back
	 * through the line). */
	TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, FIX(0, -2, 60000, 100)));

	/* Cross again, again at the interval midpoint: t = 60020 ms.
	 * Lap = 60020 - 1020 = 59000 ms exactly. */
	TEST_ASSERT_EQUAL_INT(LAP_EVT_LAP_COMPLETE, lap_core_update(&e, FIX(0, 2, 60040, 100)));
	TEST_ASSERT_TRUE(fabsf(e.lap_time_last - 59.0f) < 0.002f);
	TEST_ASSERT_EQUAL_INT(2, e.lap_number);
}

static void test_asymmetric_interpolation(void) {
	/* Crossing at 1/4 of the interval rather than the midpoint, so a naive
	 * "use the later fix's timestamp" implementation cannot pass by luck. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	lap_core_set_track(&e, &t);

	/* -1 m to +3 m => frac 0.25 => cross at 1000 + 10 = 1010 ms */
	lap_core_update(&e, FIX(0, -1, 1000, 100));
	lap_core_update(&e, FIX(0, 3, 1040, 100));

	lap_core_update(&e, FIX(0, -3, 30000, 100));
	/* -3 m to +1 m => frac 0.75 => cross at 30000 + 30 = 30030 ms */
	lap_core_update(&e, FIX(0, 1, 30040, 100));

	TEST_ASSERT_TRUE(fabsf(e.lap_time_last - 29.020f) < 0.002f);
}

static void test_crossing_outside_line_width_is_rejected(void) {
	/* The infinite extension of a start/finish line runs through the paddock,
	 * the pit lane and quite often a public road. Only the segment counts. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	t.start_finish.half_width_m = 10.0f;
	lap_core_set_track(&e, &t);

	lap_core_update(&e, FIX(0, -2, 1000, 100));
	lap_core_update(&e, FIX(0, 2, 1040, 100));
	TEST_ASSERT_TRUE(e.armed);

	/* Same crossing, but 50 m off to the side. */
	lap_core_update(&e, FIX(50, -2, 60000, 100));
	TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, FIX(50, 2, 60040, 100)));
	TEST_ASSERT_EQUAL_INT(0, e.lap_number > 1 ? 1 : 0);
	TEST_ASSERT_EQUAL_UINT(1, e.crossings_rejected_width);
}

static void test_min_lap_time_blocks_retrigger(void) {
	/* A car stopped astride the line, jittering on GPS noise, must not clock
	 * a lap per fix. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	t.min_lap_time_s = 10.0f;
	lap_core_set_track(&e, &t);

	lap_core_update(&e, FIX(0, -2, 1000, 100));
	lap_core_update(&e, FIX(0, 2, 1040, 100));

	/* Jitter back and forth well inside the guard window. */
	for (uint32_t i = 0; i < 5; i++) {
		lap_core_update(&e, FIX(0, -0.5, 2000 + i * 200, 1));
		TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, FIX(0, 0.5, 2100 + i * 200, 1)));
	}
	TEST_ASSERT_EQUAL_INT(1, e.lap_number);
	TEST_ASSERT_TRUE(e.crossings_rejected_mintime >= 5);
}

static void test_gps_dropout_does_not_invent_a_crossing(void) {
	/* If the fix is lost on one side of the line and regained on the other,
	 * interpolating between them would invent a straight line through
	 * whatever the car actually did. Better to miss the lap than time it
	 * wrongly and quietly poison the driver's best. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	lap_core_set_track(&e, &t);

	lap_core_update(&e, FIX(0, -2, 1000, 100));
	lap_core_update(&e, FIX(0, 2, 1040, 100));
	lap_core_update(&e, FIX(0, -2, 60000, 100));

	/* Lose the fix, then regain it past the line. */
	lap_fix_t lost = fix_at(0, 0, 60020, 100);
	lost.usable = false;
	lap_core_update(&e, &lost);

	TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, FIX(0, 2, 60040, 100)));
	TEST_ASSERT_EQUAL_INT(1, e.lap_number);
}

static void test_silent_gap_does_not_invent_a_lap(void) {
	/* The dropout above is signalled by an explicit usable=false fix. But if
	 * the fix stream simply STOPS (CAN bus-off, a puck brownout, lost motion
	 * frames), no unusable fix is ever delivered — the engine just sees a long
	 * jump in t_ms between two usable fixes straddling the line. Interpolating
	 * a crossing across that gap would clock an invented lap. The interval
	 * guard must reject it, exactly like the explicit dropout. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	lap_core_set_track(&e, &t);

	/* Arm on the first crossing (this starts lap 1, completes nothing). */
	lap_core_update(&e, FIX(0, -2, 1000, 100));
	lap_core_update(&e, FIX(0, 2, 1040, 100));
	TEST_ASSERT_EQUAL_INT(1, e.lap_number);

	/* Reposition just before the line with continuity intact (short interval,
	 * no crossing: going + -> - does not trigger the north-facing line). */
	lap_core_update(&e, FIX(0, -2, 1080, 100));

	/* Now a >2 s silence, then a usable fix past the line. Without the guard
	 * this interpolates a crossing and completes a bogus lap. */
	TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, FIX(0, 2, 65000, 100)));
	TEST_ASSERT_EQUAL_INT(1, e.lap_number);

	/* Continuity must RECOVER: a clean crossing at normal cadence still times
	 * a lap, so the guard drops the bad interval without wedging the engine. */
	lap_core_update(&e, FIX(0, -2, 65040, 100));
	TEST_ASSERT_EQUAL_INT(LAP_EVT_LAP_COMPLETE, lap_core_update(&e, FIX(0, 2, 65080, 100)));
	TEST_ASSERT_EQUAL_INT(2, e.lap_number);
}

static void test_clearing_track_resets_published_times(void) {
	/* Clearing the track (the DELETE path) must also clear the session — a UI
	 * still reading lap_time_best etc. would otherwise show the last track's
	 * times as if live. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	lap_core_set_track(&e, &t);

	/* Run a couple of laps so best/last/number are non-zero. */
	lap_core_update(&e, FIX(0, -2, 1000, 100));
	lap_core_update(&e, FIX(0, 2, 1040, 100));       /* arm */
	lap_core_update(&e, FIX(0, -2, 20000, 100));
	lap_core_update(&e, FIX(0, 2, 20040, 100));      /* lap 1 complete */
	TEST_ASSERT_TRUE(e.lap_time_best > 0.0f);
	TEST_ASSERT_TRUE(e.lap_number >= 2);

	lap_core_set_track(&e, NULL);   /* clear */
	TEST_ASSERT_FALSE(e.have_track);
	TEST_ASSERT_EQUAL_FLOAT(0.0f, e.lap_time_best);
	TEST_ASSERT_EQUAL_FLOAT(0.0f, e.lap_time_last);
	TEST_ASSERT_EQUAL_INT(0, e.lap_number);
	TEST_ASSERT_FALSE(e.armed);
}

static void test_non_finite_fix_is_treated_as_unusable(void) {
	/* A NaN/Inf coordinate or speed (garbage decode) must not poison distance
	 * or hang the trace loop — it is handled like a lost fix. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	lap_core_set_track(&e, &t);

	lap_core_update(&e, FIX(0, -2, 1000, 100));
	lap_core_update(&e, FIX(0, 2, 1040, 100));       /* armed */

	lap_fix_t bad = fix_at(0, 0, 1080, 100);
	bad.speed_kph = (float)NAN;
	TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, &bad));
	TEST_ASSERT_TRUE(isfinite(e.distance_m));

	/* And a normal lap still completes afterwards (continuity recovers). */
	lap_core_update(&e, FIX(0, -2, 20000, 100));
	TEST_ASSERT_EQUAL_INT(LAP_EVT_LAP_COMPLETE, lap_core_update(&e, FIX(0, 2, 20040, 100)));
}

static void test_zero_width_line_is_normalized(void) {
	/* A track whose line arrives with half_width 0 (corrupt/hand-edited file)
	 * would reject every crossing. set_track must floor it to a usable width. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();
	t.start_finish.half_width_m = 0.0f;
	lap_core_set_track(&e, &t);
	TEST_ASSERT_TRUE(e.track.start_finish.half_width_m > 0.0f);

	/* And a crossing at the centre of the line now arms as normal. */
	lap_core_update(&e, FIX(0, -2, 1000, 100));
	TEST_ASSERT_EQUAL_INT(LAP_EVT_ARMED, lap_core_update(&e, FIX(0, 2, 1040, 100)));
}

/* ══════════════════════════════════════════════════════════════════════
 * Circuit simulator — a car driving round a circle
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
	double radius_m;
	double theta;   /* 0 = start/finish, increasing clockwise */
	uint32_t t_ms;
} sim_t;

/* Start/finish at the top of the circle, crossed heading due east. */
static lap_track_t track_circle(double radius_m, uint8_t sector_count) {
	lap_track_t t;
	memset(&t, 0, sizeof(t));
	strcpy(t.name, "circle");
	t.start_finish.lat_deg = north_to_lat(radius_m);
	t.start_finish.lon_deg = 0.0;
	t.start_finish.heading_deg = 90.0f;
	t.start_finish.half_width_m = 25.0f;
	t.min_lap_time_s = 5.0f;

	/* Sector lines spaced evenly round the circle. At angle th the car is at
	 * (R sin th, R cos th) travelling on heading (90 + th) degrees. */
	t.sector_count = sector_count;
	for (uint8_t i = 0; i < sector_count; i++) {
		double th = 2.0 * M_PI * (double)(i + 1) / (double)(sector_count + 1);
		t.sectors[i].lat_deg = north_to_lat(radius_m * cos(th));
		t.sectors[i].lon_deg = east_to_lon(radius_m * sin(th));
		t.sectors[i].heading_deg = (float)(90.0 + th * R2D);
		t.sectors[i].half_width_m = 25.0f;
	}
	return t;
}

/* Drive for `duration_s` at `speed_mps`, emitting fixes at `hz`. */
static void sim_drive(lap_engine_t *e, sim_t *s, double speed_mps, double duration_s, int hz) {
	double dt = 1.0 / (double)hz;
	int steps = (int)(duration_s * hz);
	for (int i = 0; i < steps; i++) {
		s->theta += (speed_mps * dt) / s->radius_m;
		s->t_ms += (uint32_t)(dt * 1000.0);

		double east = s->radius_m * sin(s->theta);
		double north = s->radius_m * cos(s->theta);
		lap_fix_t f = fix_at(east, north, s->t_ms, (float)(speed_mps * 3.6));
		lap_core_update(e, &f);
	}
}

static void test_simulated_laps_are_timed_accurately(void) {
	const double R = 200.0;
	const double circumference = 2.0 * M_PI * R; /* 1256.6 m */
	const double v = 30.0;                       /* m/s, 108 km/h */
	const double expected_lap = circumference / v; /* 41.89 s */

	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, v, 200.0, 25); /* ~4.7 laps */

	TEST_ASSERT_TRUE(e.armed);
	TEST_ASSERT_TRUE(e.lap_number >= 4);
	/* 25 Hz interpolation should land well inside 50 ms on a lap this long. */
	TEST_ASSERT_TRUE(fabs(e.lap_time_last - expected_lap) < 0.05);
	TEST_ASSERT_TRUE(fabs(e.lap_time_best - expected_lap) < 0.05);
}

static void test_best_lap_tracks_the_fastest(void) {
	const double R = 200.0;
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, 30.0, 100.0, 25); /* ~2.4 laps at 108 km/h */
	float fast_best = e.lap_time_best;
	TEST_ASSERT_TRUE(fast_best > 0.0f);

	sim_drive(&e, &s, 20.0, 200.0, 25); /* slower laps at 72 km/h */
	/* Slower laps must not displace the best. */
	TEST_ASSERT_TRUE(fabsf(e.lap_time_best - fast_best) < 0.001f);
	TEST_ASSERT_TRUE(e.lap_time_last > e.lap_time_best);
}

static void test_sectors_split_the_lap(void) {
	const double R = 200.0;
	const double v = 30.0;
	const double expected_lap = 2.0 * M_PI * R / v;

	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 2); /* 2 split lines => 3 sectors */
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, v, 200.0, 25);

	TEST_ASSERT_TRUE(e.lap_number >= 3);

	/* Three equal thirds of a constant-speed lap. */
	for (int i = 0; i < 3; i++) {
		TEST_ASSERT_TRUE(e.best_sector_s[i] > 0.0f);
		TEST_ASSERT_TRUE(fabs(e.best_sector_s[i] - expected_lap / 3.0) < 0.1);
	}

	/* Theoretical best is the sum of the best sectors, and can never be
	 * slower than the best actual lap. */
	TEST_ASSERT_TRUE(e.lap_time_theoretical > 0.0f);
	TEST_ASSERT_TRUE(e.lap_time_theoretical <= e.lap_time_best + 0.01f);
	TEST_ASSERT_TRUE(fabs(e.lap_time_theoretical - expected_lap) < 0.15);
}

/* ══════════════════════════════════════════════════════════════════════
 * Predictive delta
 * ════════════════════════════════════════════════════════════════════ */

static void test_reference_lookup_interpolates(void) {
	lap_reference_t ref;
	memset(&ref, 0, sizeof(ref));
	ref.valid = true;
	ref.bucket_m = 10.0f;
	ref.total_time_s = 100.0f;
	/* A coherent reference also declares its lap length — lookups past it
	 * clamp (tested separately below), so keep the probes here inside it. */
	ref.total_distance_m = 10.0f * (float)LAP_REF_BUCKETS;
	for (int i = 0; i < LAP_REF_BUCKETS; i++)
		ref.time_at[i] = (float)i; /* 1 s per 10 m bucket */

	float t = 0.0f;
	TEST_ASSERT_TRUE(lap_reference_time_at(&ref, 100.0f, &t));
	TEST_ASSERT_TRUE(fabsf(t - 10.0f) < 0.001f);

	/* Half a bucket in: linear interpolation, not a step. */
	TEST_ASSERT_TRUE(lap_reference_time_at(&ref, 105.0f, &t));
	TEST_ASSERT_TRUE(fabsf(t - 10.5f) < 0.001f);

	/* An invalid reference must report so rather than return 0, which would
	 * read as "dead level with your best". */
	ref.valid = false;
	TEST_ASSERT_FALSE(lap_reference_time_at(&ref, 100.0f, &t));
}

static void test_reference_lookup_past_lap_end_does_not_hit_zeros(void) {
	/* The reference array is 512 buckets but a real lap only fills the first
	 * N of them; the rest are zero. A current lap that runs slightly longer
	 * than the reference (wider line through the last corner) queries past N —
	 * and interpolating into the zeros would slam the delta hugely positive
	 * right at the finish line. It must clamp to the reference's total time
	 * instead, staying continuous with the last real bucket. */
	lap_reference_t ref;
	memset(&ref, 0, sizeof(ref));
	ref.valid = true;
	ref.bucket_m = 10.0f;
	for (int i = 0; i < 100; i++)
		ref.time_at[i] = (float)i; /* lap ends at bucket 99: 990 m, 99 s */
	ref.total_distance_m = 990.0f;
	ref.total_time_s = 99.0f;
	/* buckets 100..511 left at 0.0 — exactly the trap */

	float t = 0.0f;
	/* 995 m: past the lap end, inside the zero region. The un-clamped
	 * interpolation between time_at[99]=99 and time_at[100]=0 would give
	 * ~49.5 — half the lap time, an instant -50 s "gain". */
	TEST_ASSERT_TRUE(lap_reference_time_at(&ref, 995.0f, &t));
	TEST_ASSERT_TRUE(fabsf(t - 99.0f) < 0.001f);

	/* Far past: same clamp. */
	TEST_ASSERT_TRUE(lap_reference_time_at(&ref, 5000.0f, &t));
	TEST_ASSERT_TRUE(fabsf(t - 99.0f) < 0.001f);

	/* Just inside the lap still interpolates normally. */
	TEST_ASSERT_TRUE(lap_reference_time_at(&ref, 985.0f, &t));
	TEST_ASSERT_TRUE(fabsf(t - 98.5f) < 0.001f);
}

static void test_reference_distance_matches_the_circuit(void) {
	/* The promoted reference's total distance must land on the true lap
	 * length. Before the crossing-residual fix, each lap's distance included
	 * a stray fraction of the fix interval on both ends; at 30 m/s and 25 Hz
	 * that is up to 1.2 m per end. Pin it to well under one fix-step. */
	const double R = 200.0;
	const double circumference = 2.0 * M_PI * R;
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, 30.0, 130.0, 25);

	TEST_ASSERT_TRUE(e.reference.valid);
	TEST_ASSERT_TRUE(fabs(e.reference.total_distance_m - circumference) < 1.5);
}

static void test_delta_goes_positive_when_slower(void) {
	const double R = 200.0;
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	/* Two quick laps to establish a best and its reference trace. */
	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, 30.0, 130.0, 25);
	TEST_ASSERT_TRUE(e.reference.valid);
	float best = e.lap_time_best;

	/* Now go round slower and check the delta reports losing time, well
	 * before the lap ends — that is what makes it *predictive*. */
	sim_drive(&e, &s, 22.0, 25.0, 25);
	TEST_ASSERT_TRUE(e.lap_delta_valid);
	TEST_ASSERT_TRUE(e.lap_delta > 0.5f);

	/* And the eventual lap really is slower, so the sign was right. */
	sim_drive(&e, &s, 22.0, 60.0, 25);
	TEST_ASSERT_TRUE(e.lap_time_last > best);
}

static void test_delta_stays_sane_through_the_finish_line(void) {
	/* The killer case the mid-lap delta tests can't see: a lap length is
	 * almost never an exact multiple of the reference bucket size, so the
	 * FINAL partial bucket used to interpolate from the last real sample
	 * toward the zero-filled tail — spiking the delta by tens of seconds in
	 * the last few metres of every lap, right where the driver looks at it.
	 * Sample the delta at EVERY fix of a full repeat lap and pin the maximum.
	 * (Circle circumference 1256.6 m vs 10 m buckets: the trap window is the
	 * last 6.6 m of the lap.) */
	const double R = 200.0;
	const double v = 30.0;
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	/* Establish a best lap + reference (~3 laps). */
	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, v, 130.0, 25);
	TEST_ASSERT_TRUE(e.reference.valid);

	/* One more full lap at the same speed, watching every fix. */
	float max_abs_delta = 0.0f;
	uint16_t start_lap = e.lap_number;
	double dt = 1.0 / 25.0;
	/* Bound the loop: a timing regression that stops completing laps would
	 * otherwise spin here forever and hang CI instead of failing. One lap is
	 * ~42 s of sim time; 120 s of fixes is a generous ceiling. */
	int guard = 0;
	while (e.lap_number == start_lap) {
		if (++guard >= 25 * 120)
			TEST_FAIL("lap never completed — timing regression (would hang CI)");
		s.theta += (v * dt) / s.radius_m;
		s.t_ms += (uint32_t)(dt * 1000.0);
		lap_fix_t f = fix_at(s.radius_m * sin(s.theta), s.radius_m * cos(s.theta), s.t_ms,
		                     (float)(v * 3.6));
		lap_core_update(&e, &f);
		if (e.lap_delta_valid && fabsf(e.lap_delta) > max_abs_delta)
			max_abs_delta = fabsf(e.lap_delta);
	}
	/* Matching the reference pace the whole way round, including the final
	 * metres: before the tail-seal fix this read ~+22 s here. */
	TEST_ASSERT_TRUE(max_abs_delta < 0.5f);
}

static void test_long_circuit_grows_buckets_instead_of_truncating(void) {
	/* First lap of a circuit longer than 512 x 10 m: the fixed-size trace
	 * must halve its resolution to fit, not silently truncate — a truncated
	 * reference made the delta read "ahead by tens of seconds" for the whole
	 * back half of tracks like the Nordschleife or a long airfield layout. */
	const double R = 1200.0; /* circumference 7539.8 m > 5120 m table */
	const double v = 40.0;
	const double circumference = 2.0 * M_PI * R;

	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.05, .t_ms = 0};
	/* ~2.5 laps at 188.5 s per lap. */
	sim_drive(&e, &s, v, 480.0, 25);

	TEST_ASSERT_TRUE(e.lap_number >= 2);
	TEST_ASSERT_TRUE(e.reference.valid);
	/* The reference must cover the WHOLE lap... */
	TEST_ASSERT_TRUE(fabs(e.reference.total_distance_m - circumference) < 5.0);
	/* ...which forced the bucket size up from the 10 m default. */
	TEST_ASSERT_TRUE(e.reference.bucket_m * (float)LAP_REF_BUCKETS >=
	                 e.reference.total_distance_m);

	/* And a repeat lap's delta must be usable deep into the lap — this is
	 * where a truncated reference used to clamp to total_time and read as
	 * catastrophically wrong. */
	sim_drive(&e, &s, v, 150.0, 25); /* ~80% of another lap */
	TEST_ASSERT_TRUE(e.lap_delta_valid);
	TEST_ASSERT_TRUE(fabsf(e.lap_delta) < 1.0f);
}

static void test_delta_is_near_zero_on_a_repeat_lap(void) {
	const double R = 200.0;
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, 30.0, 130.0, 25);
	TEST_ASSERT_TRUE(e.reference.valid);

	/* Same speed again: the driver is matching their best, so the delta
	 * should sit near zero rather than drifting. Drift here would mean the
	 * distance integration and the reference disagree. */
	sim_drive(&e, &s, 30.0, 20.0, 25);
	TEST_ASSERT_TRUE(e.lap_delta_valid);
	TEST_ASSERT_TRUE(fabsf(e.lap_delta) < 0.4f);
}

/* ══════════════════════════════════════════════════════════════════════
 * Point-to-point (sprint / hillclimb)
 *
 * A run STARTS on start_finish and ENDS on a different line, after which
 * nothing is timed until the car crosses the start again. The car never
 * returns through the finish, so none of the circuit's "re-cross the same
 * line" behaviour applies.
 * ════════════════════════════════════════════════════════════════════ */

/* Start at the origin crossed heading north; finish 1000 m north, also
 * crossed heading north. A car driving due north runs start -> finish. */
static lap_track_t track_sprint(void) {
	lap_track_t t;
	memset(&t, 0, sizeof(t));
	strcpy(t.name, "hillclimb");
	t.start_finish = line_north();
	t.sector_count = 0;
	t.min_lap_time_s = 10.0f;
	t.point_to_point = true;
	t.finish = line_north();
	t.finish.lat_deg = north_to_lat(1000.0);
	return t;
}

/* Drive due north from `from_m` to `to_m` at `v` m/s, 25 Hz. */
static uint32_t sprint_drive(lap_engine_t *e, uint32_t t_ms, double from_m, double to_m, double v) {
	const double dt = 0.04;
	for (double n = from_m; n <= to_m; n += v * dt) {
		lap_fix_t f = fix_at(0.0, n, t_ms, (float)(v * 3.6));
		lap_core_update(e, &f);
		t_ms += 40;
	}
	return t_ms;
}

static void test_sprint_times_start_to_finish(void) {
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_sprint();
	lap_core_set_track(&e, &t);

	const double v = 50.0;                    /* m/s = 180 km/h */
	const double expected = 1000.0 / v;       /* 20.0 s over the 1 km */
	sprint_drive(&e, 0, -200.0, 1200.0, v);

	TEST_ASSERT_TRUE(e.lap_time_last > 0.0f);
	TEST_ASSERT_TRUE(fabs(e.lap_time_last - expected) < 0.05);
	TEST_ASSERT_TRUE(fabs(e.lap_time_best - expected) < 0.05);
	/* The run is over: nothing is timed on the coast back down. */
	TEST_ASSERT_FALSE(e.armed);
}

static void test_sprint_does_not_close_on_the_start_line(void) {
	/* The circuit rule — re-crossing start_finish ends the lap — must NOT
	 * apply, or a hillclimb would clock a "lap" the instant it set off. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_sprint();
	lap_core_set_track(&e, &t);

	sprint_drive(&e, 0, -100.0, 500.0, 50.0);   /* through start, short of finish */
	TEST_ASSERT_TRUE(e.armed);
	TEST_ASSERT_EQUAL_FLOAT(0.0f, e.lap_time_last);   /* nothing banked yet */
	TEST_ASSERT_TRUE(e.lap_time_current > 0.0f);      /* but the clock is running */
}

static void test_sprint_second_run_numbers_and_keeps_best(void) {
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_sprint();
	lap_core_set_track(&e, &t);

	uint32_t ms = sprint_drive(&e, 0, -200.0, 1200.0, 50.0);   /* run 1: 20.0 s */
	float first = e.lap_time_last;
	uint16_t after_first = e.lap_number;

	ms += 60000;                                                /* trundle back */
	sprint_drive(&e, ms, -200.0, 1200.0, 62.5);                 /* run 2: 16.0 s */

	TEST_ASSERT_TRUE(e.lap_time_last < first);                  /* quicker */
	TEST_ASSERT_TRUE(fabs(e.lap_time_best - e.lap_time_last) < 0.001);
	/* Re-arming for a second run must not renumber back to run 1. */
	TEST_ASSERT_TRUE(e.lap_number > after_first);
	TEST_ASSERT_FALSE(e.armed);
}

static void test_sprint_restart_discards_the_aborted_run(void) {
	/* Driver sets off, aborts, returns and re-crosses the start line. That
	 * must restart the clock, not bank a run that never reached the finish. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_sprint();
	lap_core_set_track(&e, &t);

	uint32_t ms = sprint_drive(&e, 0, -100.0, 400.0, 50.0);     /* set off */
	TEST_ASSERT_TRUE(e.armed);

	ms += 30000;                                                /* back to the start */
	sprint_drive(&e, ms, -100.0, 1200.0, 50.0);                 /* re-cross, full run */

	/* Timed from the RE-cross, so a clean 20 s — not the aborted attempt
	 * plus the 30 s spent turning round. */
	TEST_ASSERT_TRUE(fabs(e.lap_time_last - 20.0f) < 0.05f);
	TEST_ASSERT_FALSE(e.armed);
}

static void test_sprint_sectors_split_the_run(void) {
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_sprint();
	t.sector_count = 1;
	t.sectors[0] = line_north();
	t.sectors[0].lat_deg = north_to_lat(400.0);   /* split 400 m up the hill */
	lap_core_set_track(&e, &t);

	sprint_drive(&e, 0, -200.0, 1200.0, 50.0);
	/* 400 m at 50 m/s = 8 s for sector 1; the run total is still 20 s. */
	TEST_ASSERT_TRUE(fabs(e.best_sector_s[0] - 8.0f) < 0.05f);
	TEST_ASSERT_TRUE(fabs(e.lap_time_last - 20.0f) < 0.05f);
}

static void test_circuit_is_unaffected_by_the_finish_line_field(void) {
	/* A circuit track carries a zeroed `finish`. It must be ignored entirely,
	 * not treated as a line at (0,0) that the car keeps crossing. */
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_simple();          /* point_to_point == false */
	lap_core_set_track(&e, &t);

	lap_core_update(&e, FIX(0.0, -20.0, 0, 72.0f));
	lap_core_update(&e, FIX(0.0, 20.0, 1000, 72.0f));      /* arms */
	TEST_ASSERT_TRUE(e.armed);
	TEST_ASSERT_EQUAL_UINT(1, e.lap_number);

	lap_core_update(&e, FIX(0.0, -20.0, 20000, 72.0f));
	lap_core_update(&e, FIX(0.0, 20.0, 21000, 72.0f));     /* completes lap 1 */
	TEST_ASSERT_EQUAL_UINT(2, e.lap_number);
	TEST_ASSERT_TRUE(e.armed);                             /* circuits stay armed */
}

/* ══════════════════════════════════════════════════════════════════════
 * Session handling
 * ════════════════════════════════════════════════════════════════════ */

static void test_session_reset_keeps_track_but_clears_times(void) {
	const double R = 200.0;
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, 30.0, 130.0, 25);
	TEST_ASSERT_TRUE(e.lap_time_best > 0.0f);

	lap_core_reset_session(&e);
	TEST_ASSERT_TRUE(e.have_track); /* still at the same circuit */
	TEST_ASSERT_EQUAL_FLOAT(0.0f, e.lap_time_best);
	TEST_ASSERT_EQUAL_INT(0, e.lap_number);
	TEST_ASSERT_FALSE(e.armed);
	TEST_ASSERT_FALSE(e.reference.valid);
}

static void test_no_track_is_inert(void) {
	/* With no track loaded the engine must sit quietly, not crash and not
	 * invent laps — this is the state every dash without a GPS is in. */
	lap_engine_t e;
	lap_core_init(&e);

	for (uint32_t i = 0; i < 100; i++)
		TEST_ASSERT_EQUAL_INT(LAP_EVT_NONE, lap_core_update(&e, FIX(0, -50.0 + i, 1000 + i * 40, 100)));

	TEST_ASSERT_EQUAL_INT(0, e.lap_number);
	TEST_ASSERT_FALSE(e.armed);
}

static void test_changing_track_clears_stale_times(void) {
	const double R = 200.0;
	lap_engine_t e;
	lap_core_init(&e);
	lap_track_t t = track_circle(R, 0);
	lap_core_set_track(&e, &t);

	sim_t s = {.radius_m = R, .theta = -0.5, .t_ms = 0};
	sim_drive(&e, &s, 30.0, 130.0, 25);
	TEST_ASSERT_TRUE(e.lap_time_best > 0.0f);

	/* Driving to a different circuit must not carry the old best over — a
	 * 41 s best from a 1.2 km circle would make every lap of a longer track
	 * read as catastrophically slow. */
	lap_track_t other = track_circle(400.0, 0);
	lap_core_set_track(&e, &other);
	TEST_ASSERT_EQUAL_FLOAT(0.0f, e.lap_time_best);
	TEST_ASSERT_FALSE(e.reference.valid);
}

int main(void) {
	UNITY_BEGIN();

	RUN_TEST(test_projection_scales_correctly);
	RUN_TEST(test_signed_distance_sign_convention);
	RUN_TEST(test_lateral_offset);
	RUN_TEST(test_heading_rotates_the_line);

	RUN_TEST(test_first_crossing_arms_but_does_not_time_a_lap);
	RUN_TEST(test_crossing_is_interpolated_between_fixes);
	RUN_TEST(test_asymmetric_interpolation);
	RUN_TEST(test_crossing_outside_line_width_is_rejected);
	RUN_TEST(test_min_lap_time_blocks_retrigger);
	RUN_TEST(test_gps_dropout_does_not_invent_a_crossing);
	RUN_TEST(test_silent_gap_does_not_invent_a_lap);
	RUN_TEST(test_clearing_track_resets_published_times);
	RUN_TEST(test_non_finite_fix_is_treated_as_unusable);
	RUN_TEST(test_zero_width_line_is_normalized);

	RUN_TEST(test_simulated_laps_are_timed_accurately);
	RUN_TEST(test_best_lap_tracks_the_fastest);
	RUN_TEST(test_sectors_split_the_lap);

	RUN_TEST(test_reference_lookup_interpolates);
	RUN_TEST(test_reference_lookup_past_lap_end_does_not_hit_zeros);
	RUN_TEST(test_reference_distance_matches_the_circuit);
	RUN_TEST(test_delta_goes_positive_when_slower);
	RUN_TEST(test_delta_is_near_zero_on_a_repeat_lap);
	RUN_TEST(test_delta_stays_sane_through_the_finish_line);
	RUN_TEST(test_long_circuit_grows_buckets_instead_of_truncating);

	RUN_TEST(test_sprint_times_start_to_finish);
	RUN_TEST(test_sprint_does_not_close_on_the_start_line);
	RUN_TEST(test_sprint_second_run_numbers_and_keeps_best);
	RUN_TEST(test_sprint_restart_discards_the_aborted_run);
	RUN_TEST(test_sprint_sectors_split_the_run);
	RUN_TEST(test_circuit_is_unaffected_by_the_finish_line_field);

	RUN_TEST(test_session_reset_keeps_track_but_clears_times);
	RUN_TEST(test_no_track_is_inert);
	RUN_TEST(test_changing_track_clears_stale_times);

	return UNITY_END();
}
