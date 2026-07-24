#pragma once

/* Lap timing engine — the arithmetic, with no ESP-IDF in sight.
 *
 * This is the piece that turns a stream of GPS fixes into lap times. It lives
 * on the dash rather than in the GPS puck deliberately (PLATFORM_PLAN_2026-07
 * §6.2): the puck stays dumb, so the lap engine survives a GPS-vendor swap and
 * any CAN GPS that speaks the published DBC works.
 *
 * No LVGL, no FreeRTOS, no esp_*. That is not incidental — it means
 * tests/native compiles THIS FILE directly instead of mirroring it by hand the
 * way test_calculated_gear.c has to. A mirrored test drifts silently; a
 * compiled one cannot. Keep it that way: hardware coupling belongs in
 * lap_engine.c.
 *
 * ── Why double, not float ────────────────────────────────────────────────
 * Latitude and longitude are held as `double` throughout. This is not
 * defensive habit: float32 carries ~7 significant digits, and a longitude of
 * 138.6 deg therefore quantises to roughly 1e-5 deg — about 1 metre — with the
 * error growing the further you are from the prime meridian. A lap timer
 * claiming +/-0.02 s at 200 km/h is resolving about 1.1 metres. Storing
 * position in float would put the quantisation error and the entire accuracy
 * budget at the same magnitude. Everything downstream of the projection to
 * local metres can be float; the raw angles cannot. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAP_MAX_SECTORS 8

/* Distance buckets for the reference-lap trace used by the predictive delta.
 * 512 buckets over a 5 km circuit is ~10 m resolution, which at 200 km/h is
 * 180 ms of lap time per bucket — finer than a driver can act on, and the
 * lookup interpolates between buckets anyway. */
#define LAP_REF_BUCKETS 512

/* A timing line: a segment the car drives across.
 *
 * Defined by its centre, the compass heading a car has WHEN CROSSING it, and a
 * half-width. Heading matters — it is what makes the line directional, so a
 * circuit that doubles back on itself does not trigger the line from the wrong
 * direction, and it is why we do not just use "within N metres of a point"
 * (which triggers in the pits, at a standstill, and on the slow-down lap). */
typedef struct {
	double lat_deg;
	double lon_deg;
	float  heading_deg;    /* 0 = north, clockwise; direction of travel */
	float  half_width_m;   /* crossings further off-centre than this are ignored */
} lap_line_t;

typedef struct {
	char       name[32];
	lap_line_t start_finish;
	lap_line_t sectors[LAP_MAX_SECTORS]; /* split lines, in order around the lap */
	uint8_t    sector_count;
	/* Rejects a re-trigger of the same line. Also the guard against a car
	 * stopped ON the line jittering across it once per fix. */
	float      min_lap_time_s;
} lap_track_t;

/* One GPS fix, already converted out of wire units. */
typedef struct {
	double   lat_deg;
	double   lon_deg;
	float    speed_kph;
	uint32_t t_ms;      /* monotonic; source need not be wall clock */
	bool     usable;    /* false = no fix; the engine coasts rather than guessing */
} lap_fix_t;

typedef enum {
	LAP_EVT_NONE = 0,
	LAP_EVT_ARMED,           /* first crossing seen; lap 1 is now running */
	LAP_EVT_SECTOR_COMPLETE,
	LAP_EVT_LAP_COMPLETE,
} lap_event_t;

/* A reference lap, stored as "elapsed time at each distance along the lap".
 *
 * Indexing by DISTANCE rather than by time is what makes the delta predictive:
 * at any moment we know how far around the lap we are, so we can ask how long
 * the reference took to get this far and compare. Indexing by time would only
 * ever tell us where we were, not whether we are up or down. */
typedef struct {
	float time_at[LAP_REF_BUCKETS]; /* seconds from the line, per bucket */
	float bucket_m;                 /* metres per bucket */
	float total_distance_m;
	float total_time_s;
	bool  valid;
} lap_reference_t;

typedef struct {
	/* ── Configuration ── */
	lap_track_t track;
	bool        have_track;

	/* ── Live outputs (what gets published as channels) ── */
	float    lap_time_current;
	float    lap_time_last;
	float    lap_time_best;
	float    lap_time_theoretical; /* sum of best sectors — always <= best lap */
	uint16_t lap_number;
	float    lap_delta;            /* predictive; negative = ahead of best */
	bool     lap_delta_valid;
	float    sector_time_current;
	float    sector_time_last;
	uint8_t  sector_number;        /* 1-based; 0 before the first crossing */
	float    best_sector_s[LAP_MAX_SECTORS + 1];

	/* ── Internal state ── */
	bool     armed;                /* have we crossed the line at least once? */
	/* Crossing instants are fractional: they are interpolated between two
	 * fixes, which is the whole source of the sub-fix-interval resolution.
	 * Rounding them to whole milliseconds here would throw away most of what
	 * the interpolation just bought. */
	double   lap_start_ms;
	double   sector_start_ms;
	double   last_cross_ms;
	float    distance_m;           /* travelled since the start of this lap */

	lap_fix_t prev_fix;
	bool      have_prev;
	float     prev_sf_distance;    /* signed distance to start/finish, metres */
	float     prev_sector_distance;
	bool      have_prev_distances;

	/* Reference lap for the predictive delta, plus the trace being recorded
	 * for the lap in progress (promoted to reference if it turns out best). */
	lap_reference_t reference;
	float           trace_time_at[LAP_REF_BUCKETS];
	uint16_t        trace_buckets_filled;
	float           trace_bucket_m;

	/* Diagnostics */
	uint32_t fixes_seen;
	uint32_t crossings_rejected_width;
	uint32_t crossings_rejected_mintime;
} lap_engine_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* Full reset: clears the track, best times and reference lap. */
void lap_core_init(lap_engine_t *e);

/* Clears live timing and the reference, keeping the track. This is
 * "new session" — best times reset, so a wet session does not chase a dry
 * reference all day. */
void lap_core_reset_session(lap_engine_t *e);

void lap_core_set_track(lap_engine_t *e, const lap_track_t *track);

/* Feed one fix. Returns what this fix completed, if anything.
 *
 * Fixes must arrive in time order. An unusable fix (no GPS lock) does not
 * advance timing or distance: the engine holds state and picks up when the
 * fix returns, rather than inventing a straight line across the gap. */
lap_event_t lap_core_update(lap_engine_t *e, const lap_fix_t *fix);

/* ── Geometry, exposed for testing ─────────────────────────────────────── */

/* Equirectangular projection of (lat, lon) into metres east/north of a local
 * origin. Good to well under a centimetre over the couple of hundred metres
 * that matter around a timing line; we are not navigating with it. */
void lap_project_local(double lat0_deg, double lon0_deg, double lat_deg, double lon_deg,
                       double *east_m, double *north_m);

/* Signed distance from a line, in metres, measured ALONG the direction of
 * travel: negative before the line, positive after. `lateral_m` (optional)
 * receives the offset along the line from its centre, for the width check. */
float lap_line_signed_distance(const lap_line_t *line, double lat_deg, double lon_deg,
                               float *lateral_m);

/* Great-circle-ish distance between two fixes, in metres. */
float lap_distance_between(double lat1, double lon1, double lat2, double lon2);

/* Reference-lap lookup: elapsed seconds at `distance_m` around the lap, with
 * linear interpolation between buckets. Returns false if the reference is
 * invalid or the distance is off the end of it. */
bool lap_reference_time_at(const lap_reference_t *ref, float distance_m, float *out_s);

#ifdef __cplusplus
}
#endif
