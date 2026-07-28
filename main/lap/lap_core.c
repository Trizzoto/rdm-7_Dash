#include "lap_core.h"

#include <math.h>
#include <string.h>

/* M_PI is a POSIX extension, not ISO C. The IDF build uses gnu17 so it is
 * there, but tests/native builds with -std=c11 where it is not. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_M 6371008.8 /* WGS84 mean radius */
#define DEG_TO_RAD     (M_PI / 180.0)

/* Default distance-bucket size for the reference trace before we know how long
 * a lap actually is. 10 m x 512 buckets = 5.12 km, which covers most circuits;
 * after the first completed lap the bucket size adapts to fit exactly. */
#define DEFAULT_BUCKET_M 10.0f

/* Guard on sector re-triggering is implicit (we only ever look for the NEXT
 * sector line), but the start/finish line needs an explicit one or a car
 * stopped astride it will clock a lap per fix. */
#define DEFAULT_MIN_LAP_S 10.0f

/* Half-width fallback for a line that arrives with none (a corrupt or
 * hand-edited track file — the web/capture paths already clamp to [2,100]). A
 * zero-width line rejects every crossing, silently killing the timer. */
#define DEFAULT_HALF_WIDTH_M 15.0f

/* Longest gap between two usable fixes we will still interpolate a crossing
 * across. Beyond this the stream was interrupted — CAN bus-off/recovery, a
 * puck brownout, or motion frames lost so no unusable fix was ever delivered —
 * and interpolating would invent a straight chord through whatever the car
 * actually did and clock a false lap. Mirrors the distance integrator's 2 s
 * step guard so the two stay consistent. */
#define LAP_MAX_FIX_GAP_MS 2000.0

/* ---------------------------------------------------------------------------
 * Geometry
 * ------------------------------------------------------------------------- */
void lap_project_local(double lat0_deg, double lon0_deg, double lat_deg, double lon_deg,
                       double *east_m, double *north_m) {
	/* Equirectangular. Over the few hundred metres around a timing line the
	 * error against a proper geodesic is far below a millimetre, and it costs
	 * one cosine instead of a pile of trig — which matters when this runs at
	 * 25 Hz on the same core as the UI. */
	double lat0_rad = lat0_deg * DEG_TO_RAD;
	double dlat = (lat_deg - lat0_deg) * DEG_TO_RAD;
	double dlon = (lon_deg - lon0_deg) * DEG_TO_RAD;

	if (east_m)
		*east_m = dlon * cos(lat0_rad) * EARTH_RADIUS_M;
	if (north_m)
		*north_m = dlat * EARTH_RADIUS_M;
}

float lap_line_signed_distance(const lap_line_t *line, double lat_deg, double lon_deg,
                               float *lateral_m) {
	double east, north;
	lap_project_local(line->lat_deg, line->lon_deg, lat_deg, lon_deg, &east, &north);

	/* Travel direction unit vector, in (east, north). Heading is compass:
	 * 0 = north, 90 = east, so it is sin/cos rather than the cos/sin you get
	 * from a maths-convention angle. */
	double h = (double)line->heading_deg * DEG_TO_RAD;
	double dir_e = sin(h);
	double dir_n = cos(h);

	if (lateral_m) {
		/* Perpendicular to travel, i.e. along the line itself. */
		*lateral_m = (float)(east * dir_n - north * dir_e);
	}
	return (float)(east * dir_e + north * dir_n);
}

float lap_distance_between(double lat1, double lon1, double lat2, double lon2) {
	double east, north;
	lap_project_local(lat1, lon1, lat2, lon2, &east, &north);
	return (float)sqrt(east * east + north * north);
}

/* ---------------------------------------------------------------------------
 * Reference lap
 * ------------------------------------------------------------------------- */
bool lap_reference_time_at(const lap_reference_t *ref, float distance_m, float *out_s) {
	if (!ref || !ref->valid || ref->bucket_m <= 0.0f || distance_m < 0.0f)
		return false;

	/* Past the reference lap's ACTUAL length — not just past the bucket
	 * array. The buckets beyond the recorded lap are zero-filled, and
	 * interpolating into them would send the delta hugely positive right at
	 * the finish line whenever the current lap runs slightly long (a wider
	 * line through the final corner is enough). Clamp to the reference's
	 * total instead: "you have already used your whole best lap's time",
	 * which is the honest reading and continuous with the buckets before it. */
	if (distance_m >= ref->total_distance_m) {
		if (out_s)
			*out_s = ref->total_time_s;
		return true;
	}

	float pos = distance_m / ref->bucket_m;
	int idx = (int)pos;
	if (idx < 0)
		return false;
	if (idx >= LAP_REF_BUCKETS - 1) {
		if (out_s)
			*out_s = ref->total_time_s;
		return true;
	}

	float frac = pos - (float)idx;
	float a = ref->time_at[idx];
	float b = ref->time_at[idx + 1];
	if (out_s)
		*out_s = a + (b - a) * frac;
	return true;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */
void lap_core_init(lap_engine_t *e) {
	memset(e, 0, sizeof(*e));
	e->trace_bucket_m = DEFAULT_BUCKET_M;
}

void lap_core_reset_session(lap_engine_t *e) {
	lap_track_t track = e->track;
	bool have_track = e->have_track;
	memset(e, 0, sizeof(*e));
	e->track = track;
	e->have_track = have_track;
	e->trace_bucket_m = DEFAULT_BUCKET_M;
}

/* Make a timing line safe to run trig against. A non-finite heading makes
 * sin/cos NaN so the line never triggers; a zero or non-finite half-width
 * makes the width test reject every crossing. Both are silent timer-killers,
 * so clamp them at the one chokepoint every source (web API, capture, file
 * reload) passes through. */
static void normalize_line(lap_line_t *l) {
	if (!isfinite(l->heading_deg))
		l->heading_deg = 0.0f;
	l->heading_deg = fmodf(l->heading_deg, 360.0f);
	if (l->heading_deg < 0.0f)
		l->heading_deg += 360.0f;
	if (!isfinite(l->half_width_m) || l->half_width_m <= 0.0f)
		l->half_width_m = DEFAULT_HALF_WIDTH_M;
}

void lap_core_set_track(lap_engine_t *e, const lap_track_t *track) {
	if (!track) {
		/* Clearing the track must also clear the session — otherwise the last
		 * lap's best/last/number keep being published as if still live. */
		e->have_track = false;
		lap_core_reset_session(e);
		return;
	}
	e->track = *track;
	if (!isfinite(e->track.min_lap_time_s) || e->track.min_lap_time_s <= 0.0f)
		e->track.min_lap_time_s = DEFAULT_MIN_LAP_S;
	if (e->track.sector_count > LAP_MAX_SECTORS)
		e->track.sector_count = LAP_MAX_SECTORS;
	normalize_line(&e->track.start_finish);
	for (uint8_t i = 0; i < e->track.sector_count; i++)
		normalize_line(&e->track.sectors[i]);
	if (e->track.point_to_point)
		normalize_line(&e->track.finish);
	e->have_track = true;
	/* A different track invalidates everything timed against the old one. */
	lap_core_reset_session(e);
}

/* ---------------------------------------------------------------------------
 * Crossing detection
 * ------------------------------------------------------------------------- */

/* Did we cross from before the line to on/past it between two fixes?
 *
 * `frac` receives where in the interval the crossing happened, 0..1, which is
 * the entire source of sub-fix-interval resolution: at 25 Hz the fixes are
 * 40 ms apart, but the crossing instant lands anywhere inside that. */
static bool crossed(float prev_d, float cur_d, float *frac) {
	if (!(prev_d < 0.0f && cur_d >= 0.0f))
		return false;
	float span = cur_d - prev_d;
	if (span <= 0.0f)
		return false;
	*frac = -prev_d / span;
	return true;
}

/* Was the crossing inside the line's width? Interpolates the position at the
 * crossing instant and measures the offset along the line. Without this, a car
 * in the paddock on the far side of the pit wall crosses the infinite
 * extension of the line and clocks a lap. */
static bool within_width(const lap_line_t *line, const lap_fix_t *prev, const lap_fix_t *cur,
                         float frac) {
	double lat = prev->lat_deg + (cur->lat_deg - prev->lat_deg) * (double)frac;
	double lon = prev->lon_deg + (cur->lon_deg - prev->lon_deg) * (double)frac;
	float lateral = 0.0f;
	lap_line_signed_distance(line, lat, lon, &lateral);
	return fabsf(lateral) <= line->half_width_m;
}

/* ---------------------------------------------------------------------------
 * Reference trace recording
 * ------------------------------------------------------------------------- */
static void trace_record(lap_engine_t *e, float distance_m, float elapsed_s) {
	/* A non-finite distance would spin the bucket-halving loop below forever
	 * (idx stays >= LAP_REF_BUCKETS, and the float->int cast of Inf/NaN is UB).
	 * The fix-entry guard already keeps distance_m finite; this is the last
	 * line of defence for that invariant. */
	if (e->trace_bucket_m <= 0.0f || !isfinite(distance_m))
		return;
	int idx = (int)(distance_m / e->trace_bucket_m);
	if (idx < 0)
		return;
	/* Circuit longer than the table at this resolution — the first lap of a
	 * >5.12 km track with the 10 m default bucket. Halve the resolution in
	 * place instead of truncating: a truncated reference made the delta read
	 * "ahead by tens of seconds" for the whole back half of long circuits.
	 * Compression is lossless enough (we keep every other sample) and the
	 * bucket size re-derives tighter on the next lap anyway. */
	while (idx >= LAP_REF_BUCKETS) {
		for (int i = 0; i < LAP_REF_BUCKETS / 2; i++)
			e->trace_time_at[i] = e->trace_time_at[2 * i];
		e->trace_buckets_filled = (uint16_t)((e->trace_buckets_filled + 1) / 2);
		e->trace_bucket_m *= 2.0f;
		idx = (int)(distance_m / e->trace_bucket_m);
	}

	/* Fill every bucket up to here, not just this one. At 25 Hz and 200 km/h
	 * the car covers 2.2 m per fix, so with 10 m buckets most fixes land in
	 * the same bucket — but a gap in GPS coverage can skip several, and a hole
	 * in the trace would make the delta jump when the lookup lands in it. */
	for (int i = e->trace_buckets_filled; i <= idx; i++)
		e->trace_time_at[i] = elapsed_s;
	if (idx >= e->trace_buckets_filled)
		e->trace_buckets_filled = (uint16_t)(idx + 1);
}

static void trace_promote_to_reference(lap_engine_t *e, float lap_time_s, float total_distance_m) {
	memcpy(e->reference.time_at, e->trace_time_at, sizeof(e->reference.time_at));
	/* Seal the tail. The recorded lap fills buckets 0..filled-1 and leaves
	 * the rest at zero — and a lap length is almost never an exact bucket
	 * multiple, so lookups in the final PARTIAL bucket would interpolate
	 * from the last real sample toward 0 and spike the delta tens of
	 * seconds positive in the last few metres of every lap. Filling the
	 * tail with the total makes that interpolation land on "the whole best
	 * lap's time", continuous with the total-distance clamp in
	 * lap_reference_time_at(). */
	for (int i = e->trace_buckets_filled; i < LAP_REF_BUCKETS; i++)
		e->reference.time_at[i] = lap_time_s;
	e->reference.bucket_m = e->trace_bucket_m;
	e->reference.total_distance_m = total_distance_m;
	e->reference.total_time_s = lap_time_s;
	e->reference.valid = e->trace_buckets_filled > 1;
}

static void trace_begin_lap(lap_engine_t *e) {
	memset(e->trace_time_at, 0, sizeof(e->trace_time_at));
	e->trace_buckets_filled = 0;
	/* Size the buckets so a lap the same length as the last one fills the
	 * table exactly. Before the first completed lap we have no idea, so the
	 * default stands. */
	if (e->reference.valid && e->reference.total_distance_m > 0.0f) {
		e->trace_bucket_m = e->reference.total_distance_m / (float)(LAP_REF_BUCKETS - 2);
		if (e->trace_bucket_m < 1.0f)
			e->trace_bucket_m = 1.0f;
	}
}

/* ---------------------------------------------------------------------------
 * Sector bookkeeping
 * ------------------------------------------------------------------------- */
static void close_sector(lap_engine_t *e, double cross_ms) {
	float t = (float)((cross_ms - e->sector_start_ms) / 1000.0);
	e->sector_time_last = t;

	uint8_t idx = e->sector_number ? (uint8_t)(e->sector_number - 1) : 0;
	if (idx <= LAP_MAX_SECTORS) {
		if (e->best_sector_s[idx] <= 0.0f || t < e->best_sector_s[idx])
			e->best_sector_s[idx] = t;
	}
	e->sector_start_ms = cross_ms;
}

/* Sum of the best sectors ever set, which is the lap the driver has already
 * proven they can do in pieces. Only meaningful once every sector has a best. */
static void recompute_theoretical(lap_engine_t *e) {
	uint8_t n = (uint8_t)(e->track.sector_count + 1);
	float sum = 0.0f;
	for (uint8_t i = 0; i < n; i++) {
		if (e->best_sector_s[i] <= 0.0f) {
			e->lap_time_theoretical = 0.0f;
			return;
		}
		sum += e->best_sector_s[i];
	}
	e->lap_time_theoretical = sum;
}

/* ---------------------------------------------------------------------------
 * Update
 * ------------------------------------------------------------------------- */
/* Begin timing a lap (circuit) or a run (point-to-point) at `cross_ms`.
 *
 * The crossing lands part-way through the current fix interval, so the new
 * lap has already covered (1-frac) of this step — crediting it here is what
 * keeps the distance-indexed reference trace aligned lap to lap. */
static void start_run(lap_engine_t *e, double cross_ms, float step_m, float frac) {
	e->armed = true;
	e->lap_start_ms = cross_ms;
	e->sector_start_ms = cross_ms;
	e->last_cross_ms = cross_ms;
	e->sector_number = 1;
	e->distance_m = step_m * (1.0f - frac);
	e->lap_delta = 0.0f;
	e->lap_delta_valid = false;
	trace_begin_lap(e);
}

/* Close the lap/run in progress at `cross_ms`: bank the time, update best and
 * theoretical, and promote the trace if this was the quickest so far.
 *
 * Shared by the circuit path and the point-to-point finish line so the two
 * cannot drift apart — the only difference between them is what happens NEXT
 * (a circuit starts the following lap, a sprint disarms). */
static void close_run(lap_engine_t *e, double cross_ms, float step_m, float frac) {
	float lap_s = (float)((cross_ms - e->lap_start_ms) / 1000.0);
	/* Where the lap actually ENDED: the crossing, not this fix.
	 * distance_m currently includes the full step to the fix. */
	float dist_at_cross = e->distance_m - step_m * (1.0f - frac);
	if (dist_at_cross < 0.0f)
		dist_at_cross = 0.0f;

	close_sector(e, cross_ms);
	recompute_theoretical(e);

	e->lap_time_last = lap_s;
	e->lap_time_current = lap_s;
	if (e->lap_time_best <= 0.0f || lap_s < e->lap_time_best) {
		e->lap_time_best = lap_s;
		/* The reference for the predictive delta is the best lap, and it
		 * updates the moment a better one is set — so the delta always answers
		 * "against my best", which is the question the driver is asking. */
		trace_record(e, dist_at_cross, lap_s);
		trace_promote_to_reference(e, lap_s, dist_at_cross);
	}
	e->lap_number++;
}

lap_event_t lap_core_update(lap_engine_t *e, const lap_fix_t *fix) {
	if (!fix)
		return LAP_EVT_NONE;

	/* A non-finite coordinate or speed (garbage decode, a NaN out of a bad
	 * frame) would poison distance_m and the crossing math, and an infinite
	 * distance would hang trace_record's bucket-halving loop. Treat it exactly
	 * like a lost fix: coast, do not guess. */
	bool finite = isfinite(fix->lat_deg) && isfinite(fix->lon_deg) &&
	              isfinite(fix->speed_kph);
	if (!fix->usable || !finite) {
		/* Drop continuity but keep timing. Interpolating a crossing across a
		 * GPS dropout would invent a straight line through whatever the car
		 * actually did, so we would rather miss the lap than time it wrongly. */
		e->have_prev = false;
		e->have_prev_distances = false;
		return LAP_EVT_NONE;
	}

	e->fixes_seen++;

	if (!e->have_track) {
		e->prev_fix = *fix;
		e->have_prev = true;
		return LAP_EVT_NONE;
	}

	/* Distance comes from Doppler ground speed, not from differencing
	 * positions. Position differencing accumulates GPS noise into hundreds of
	 * metres of phantom distance while the car sits in the pits, which would
	 * wreck the distance-indexed delta. Doppler speed reads a true zero.
	 *
	 * step_m is kept separately: when a crossing lands mid-interval, the new
	 * lap has already covered (1-frac) of this step, and the old lap only
	 * covered frac of it. Resetting distance to plain zero at the fix would
	 * misplace the whole reference trace by up to one step per lap. */
	float dt_s = 0.0f;
	if (e->have_prev && fix->t_ms >= e->prev_fix.t_ms)
		dt_s = (float)(fix->t_ms - e->prev_fix.t_ms) / 1000.0f;
	float step_m = 0.0f;
	if (dt_s > 0.0f && dt_s < 2.0f)
		step_m = (fix->speed_kph / 3.6f) * dt_s;
	if (e->armed)
		e->distance_m += step_m;

	float sf_lateral = 0.0f;
	float sf_d = lap_line_signed_distance(&e->track.start_finish, fix->lat_deg, fix->lon_deg,
	                                      &sf_lateral);

	float fin_d = 0.0f;
	if (e->track.point_to_point)
		fin_d = lap_line_signed_distance(&e->track.finish, fix->lat_deg, fix->lon_deg, NULL);

	bool have_sector_line = e->armed && e->sector_number >= 1 &&
	                        (e->sector_number - 1) < e->track.sector_count;
	float sec_d = 0.0f;
	if (have_sector_line) {
		const lap_line_t *line = &e->track.sectors[e->sector_number - 1];
		sec_d = lap_line_signed_distance(line, fix->lat_deg, fix->lon_deg, NULL);
	}

	lap_event_t evt = LAP_EVT_NONE;

	double interval_ms = e->have_prev ? ((double)fix->t_ms - (double)e->prev_fix.t_ms) : 0.0;
	/* A gap longer than LAP_MAX_FIX_GAP_MS means the fix stream was interrupted
	 * without ever delivering an unusable fix (bus-off, brownout, lost motion
	 * frames). Interpolating a crossing across it would clock an invented lap
	 * and can poison best/reference, so skip crossing detection this update and
	 * let prev_* at the bottom re-establish continuity for the next one. */
	bool interval_ok = interval_ms > 0.0 && interval_ms <= LAP_MAX_FIX_GAP_MS;

	if (e->have_prev && e->have_prev_distances && interval_ok) {
		float frac;

		/* Sector lines first: crossing a sector line and the start/finish line
		 * in the same 40 ms interval is not physically possible on any real
		 * circuit, but ordering them makes the outcome defined rather than
		 * dependent on evaluation order. */
		if (have_sector_line && crossed(e->prev_sector_distance, sec_d, &frac)) {
			const lap_line_t *line = &e->track.sectors[e->sector_number - 1];
			if (within_width(line, &e->prev_fix, fix, frac)) {
				double cross_ms = (double)e->prev_fix.t_ms + interval_ms * (double)frac;
				close_sector(e, cross_ms);
				e->sector_number++;
				recompute_theoretical(e);
				evt = LAP_EVT_SECTOR_COMPLETE;
			} else {
				e->crossings_rejected_width++;
			}
		}

		if (crossed(e->prev_sf_distance, sf_d, &frac)) {
			double cross_ms = (double)e->prev_fix.t_ms + interval_ms * (double)frac;

			if (!within_width(&e->track.start_finish, &e->prev_fix, fix, frac)) {
				e->crossings_rejected_width++;
			} else if (e->armed &&
			           (cross_ms - e->last_cross_ms) < (double)e->track.min_lap_time_s * 1000.0) {
				e->crossings_rejected_mintime++;
			} else if (!e->armed) {
				/* First crossing of the session: this starts lap 1, it does
				 * not complete a lap. Timing from ignition-on would otherwise
				 * report an "out lap" that includes however long the car sat
				 * in the garage. */
				start_run(e, cross_ms, step_m, frac);
				/* Only the FIRST arm of a session is run 1. A point-to-point
				 * course disarms at its finish line and re-arms for the next
				 * run, which must not renumber back to 1. */
				if (e->lap_number == 0)
					e->lap_number = 1;
				evt = LAP_EVT_ARMED;
			} else if (e->track.point_to_point) {
				/* Sprint: re-crossing the START line while a run is live. The
				 * car never returns to the start mid-run on a real hillclimb,
				 * so this is an aborted run being retaken — restart rather than
				 * time a run that never finished. min_lap_time_s above already
				 * absorbed the jitter case. */
				start_run(e, cross_ms, step_m, frac);
				evt = LAP_EVT_ARMED;
			} else {
				close_run(e, cross_ms, step_m, frac);
				/* A circuit's finish line IS its start line: the same crossing
				 * that ended that lap begins the next one. */
				start_run(e, cross_ms, step_m, frac);
				e->lap_time_current = 0.0f;
				evt = LAP_EVT_LAP_COMPLETE;
			}
		}

		/* Point-to-point: the run ends on its own line, and the car then coasts
		 * back down the hill with nothing armed. Checked after start/finish so a
		 * course whose two lines sit close together resolves in a defined order. */
		if (e->track.point_to_point && e->armed &&
		    crossed(e->prev_finish_distance, fin_d, &frac)) {
			double cross_ms = (double)e->prev_fix.t_ms + interval_ms * (double)frac;
			if (!within_width(&e->track.finish, &e->prev_fix, fix, frac)) {
				e->crossings_rejected_width++;
			} else {
				close_run(e, cross_ms, step_m, frac);
				/* Disarm: the run is over. Nothing is timed until the car
				 * crosses the start line again. */
				e->armed = false;
				e->last_cross_ms = cross_ms;
				evt = LAP_EVT_LAP_COMPLETE;
			}
		}
	}

	/* Live values for the widgets. */
	if (e->armed) {
		e->lap_time_current = (float)(((double)fix->t_ms - e->lap_start_ms) / 1000.0);
		if (e->lap_time_current < 0.0f)
			e->lap_time_current = 0.0f;
		e->sector_time_current = (float)(((double)fix->t_ms - e->sector_start_ms) / 1000.0);
		if (e->sector_time_current < 0.0f)
			e->sector_time_current = 0.0f;

		trace_record(e, e->distance_m, e->lap_time_current);

		float ref_s;
		if (lap_reference_time_at(&e->reference, e->distance_m, &ref_s)) {
			e->lap_delta = e->lap_time_current - ref_s;
			e->lap_delta_valid = true;
		} else {
			e->lap_delta = 0.0f;
			e->lap_delta_valid = false;
		}
	}

	e->prev_fix = *fix;
	e->have_prev = true;
	e->prev_sf_distance = sf_d;
	e->prev_sector_distance = sec_d;
	e->prev_finish_distance = fin_d;
	e->have_prev_distances = true;
	return evt;
}
