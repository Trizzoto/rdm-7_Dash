#include "lap/lap_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "can/can_decode.h"
#include "can/can_manager.h"
#include "data/channel_manager.h"
#include "widgets/signal.h"

static const char *TAG = "lap_engine";

/* ── RDM GPS frame offsets from the block base ─────────────────────────
 * Restated from the RDM GPS repo's docs/PROTOCOL.md. They cannot be included
 * from here (different repo, different build) so they are duplicated — if the
 * puck's protocol ever changes, this table and rdm_gps_proto.h must move
 * together. The protocol is additive-only after first ship precisely so this
 * duplication stays safe. */
#define GPS_OFF_STATUS   0x0u
#define GPS_OFF_POSITION 0x1u
#define GPS_OFF_MOTION   0x2u
#define GPS_OFF_QUALITY  0x3u

#define GPS_FLAG_FIX_OK 0x01u

/* Byte 0 of the status frame. 0x400 is only a CONVENTION — nothing stops a
 * customer's other ECU broadcasting there. The device type byte is what makes
 * detection trustworthy: no 0x02, no auto-bind, no fix trust. */
#define GPS_DEVICE_TYPE_GPS 0x02u

#define GPS_STALE_US 2000000 /* status silent this long = puck gone */

/* A position frame this old is not paired with an arriving motion frame. The
 * puck emits position then motion back to back, so anything beyond a couple of
 * broadcast periods means we dropped one. */
#define POSITION_PAIR_MAX_MS 200

#define TRACK_PATH     "/lfs/lap_track.json"
#define TRACK_TMP_PATH TRACK_PATH ".tmp"
#define TRACK_BAK_PATH TRACK_PATH ".bak"
#define TRACK_SCHEMA_VERSION 1
#define TRACK_MAX_FILE_BYTES (16 * 1024)

/* ── State ─────────────────────────────────────────────────────────────── */
static lap_engine_t s_engine;
static uint16_t s_base_id = LAP_GPS_BASE_ID_DEFAULT;
static bool s_started = false;

static double s_pending_lat = 0.0;
static double s_pending_lon = 0.0;
static uint32_t s_pending_ms = 0;
static bool s_have_pending = false;

/* Latest complete fix, kept for the line-capture flow (drive across the line,
 * tap "capture" — this is what gets captured). Doubles, not channel floats. */
static double s_last_lat = 0.0;
static double s_last_lon = 0.0;
static float s_last_heading_deg = 0.0f;
static float s_last_speed_kph = 0.0f;
static bool s_have_last_fix = false;

static bool s_fix_ok = false;
static int64_t s_last_status_us = 0;

/* Auto-bind: the first time a puck announces itself we wire up the Position &
 * GPS channels, so plugging one in is genuinely all a customer has to do. The
 * CAN handler only raises a flag — the actual binding activates channels,
 * upserts signals and writes channels.json, none of which belongs in the frame
 * drain loop. A slow timer does the work. */
static bool s_gps_channels_bound = false;
static volatile bool s_bind_requested = false;
static lv_timer_t *s_housekeeping_timer = NULL;

/* Channels the engine feeds, with the internal signal that carries each one.
 * Same two-step as channel_math.c: register a synthetic signal, bind the
 * channel to it, then push values through signal_set_external_value so peaks,
 * freshness, zone transitions and subscribers all behave normally. */
static const struct {
	const char *channel_id;
	const char *signal_name;
} LAP_CHANNELS[] = {
	{"lap_time_current",    "LAP_TIME_CUR"},
	{"sector_time_current", "LAP_SECT_TIME"},
	{"lap_delta_best",      "LAP_DELTA"},
	{"lap_time_last",       "LAP_TIME_LAST"},
	{"lap_time_best",       "LAP_TIME_BEST"},
	{"lap_time_theoretical","LAP_TIME_THEO"},
	{"lap_number",          "LAP_NUMBER"},
	{"sector_number",       "LAP_SECT_NUM"},
};
#define LAP_CHANNEL_COUNT (sizeof(LAP_CHANNELS) / sizeof(LAP_CHANNELS[0]))

/* ── The Position & GPS channels, and where each lives on the wire ──────
 * Mirrors docs/rdm_gps.dbc. All little-endian (Intel, endian = 1). */
static const struct {
	const char *channel_id;
	const char *signal_name;
	uint8_t frame_offset;
	uint8_t bit_start;
	uint8_t bit_length;
	float   scale;
	bool    is_signed;
	const char *unit;
} GPS_CHANNELS[] = {
	{"gps_latitude",   "GPS_LAT",   GPS_OFF_POSITION,  0, 32, 1e-7f, true,  "deg"},
	{"gps_longitude",  "GPS_LON",   GPS_OFF_POSITION, 32, 32, 1e-7f, true,  "deg"},
	{"gps_speed",      "GPS_SPEED", GPS_OFF_MOTION,    0, 16, 0.01f, false, "km/h"},
	{"gps_heading",    "GPS_HEAD",  GPS_OFF_MOTION,   16, 16, 0.01f, false, "deg"},
	{"gps_altitude",   "GPS_ALT",   GPS_OFF_MOTION,   32, 16, 0.1f,  true,  "m"},
	{"gps_accuracy",   "GPS_HACC",  GPS_OFF_QUALITY,   0, 16, 0.01f, false, "m"},
	{"gps_fix_type",   "GPS_FIX",   GPS_OFF_STATUS,    8,  8, 1.0f,  false, ""},
	{"gps_satellites", "GPS_SATS",  GPS_OFF_STATUS,   16,  8, 1.0f,  false, "count"},
};
#define GPS_CHANNEL_COUNT (sizeof(GPS_CHANNELS) / sizeof(GPS_CHANNELS[0]))

/* ── Publishing ────────────────────────────────────────────────────────── */
static float channel_value_for(size_t i) {
	const lap_engine_t *e = &s_engine;
	switch (i) {
	case 0: return e->lap_time_current;
	case 1: return e->sector_time_current;
	case 2: return e->lap_delta_valid ? e->lap_delta : 0.0f;
	case 3: return e->lap_time_last;
	case 4: return e->lap_time_best;
	case 5: return e->lap_time_theoretical;
	case 6: return (float)e->lap_number;
	case 7: return (float)e->sector_number;
	default: return 0.0f;
	}
}

static void publish(void) {
	/* Every channel, every fix — including the ones that only change at
	 * lap/sector events. signal_set_external_value refreshes last_update_ms
	 * unconditionally but only notifies subscribers (i.e. repaints widgets)
	 * on a VALUE CHANGE, so this costs nothing on screen. An earlier version
	 * skipped unchanged values entirely, which let lap_number hit the 2000 ms
	 * staleness timeout mid-lap and grey out its widget until the next lap. */
	for (size_t i = 0; i < LAP_CHANNEL_COUNT; i++)
		signal_set_external_value(LAP_CHANNELS[i].signal_name, channel_value_for(i));
}

/* ── Track persistence ─────────────────────────────────────────────────── */
static cJSON *line_to_json(const lap_line_t *l) {
	cJSON *j = cJSON_CreateObject();
	if (!j)
		return NULL;
	cJSON_AddNumberToObject(j, "lat", l->lat_deg);
	cJSON_AddNumberToObject(j, "lon", l->lon_deg);
	cJSON_AddNumberToObject(j, "heading", l->heading_deg);
	cJSON_AddNumberToObject(j, "half_width_m", l->half_width_m);
	return j;
}

static void line_from_json(const cJSON *j, lap_line_t *l) {
	if (!cJSON_IsObject(j))
		return;
	const cJSON *it;
	if ((it = cJSON_GetObjectItemCaseSensitive(j, "lat") ) && cJSON_IsNumber(it))
		l->lat_deg = it->valuedouble;
	if ((it = cJSON_GetObjectItemCaseSensitive(j, "lon")) && cJSON_IsNumber(it))
		l->lon_deg = it->valuedouble;
	if ((it = cJSON_GetObjectItemCaseSensitive(j, "heading")) && cJSON_IsNumber(it))
		l->heading_deg = (float)it->valuedouble;
	if ((it = cJSON_GetObjectItemCaseSensitive(j, "half_width_m")) && cJSON_IsNumber(it))
		l->half_width_m = (float)it->valuedouble;
}

static esp_err_t track_save(void) {
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return ESP_ERR_NO_MEM;

	cJSON_AddNumberToObject(root, "schema_version", TRACK_SCHEMA_VERSION);
	cJSON_AddNumberToObject(root, "gps_base_id", s_base_id);
	if (s_engine.have_track) {
		const lap_track_t *t = &s_engine.track;
		cJSON_AddStringToObject(root, "name", t->name);
		cJSON_AddNumberToObject(root, "min_lap_time_s", t->min_lap_time_s);
		cJSON_AddItemToObject(root, "start_finish", line_to_json(&t->start_finish));
		cJSON *arr = cJSON_AddArrayToObject(root, "sectors");
		for (uint8_t i = 0; i < t->sector_count && arr; i++)
			cJSON_AddItemToArray(arr, line_to_json(&t->sectors[i]));
	}

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return ESP_ERR_NO_MEM;

	/* Atomic write, same idiom as channel_manager_save_to_lfs(): stage into
	 * .tmp, fsync, then rename over the live file, so a 12 V drop mid-write
	 * can never leave a half-written track. */
	size_t len = strlen(json);
	FILE *f = fopen(TRACK_TMP_PATH, "w");
	if (!f) {
		cJSON_free(json);
		ESP_LOGE(TAG, "cannot open %s", TRACK_TMP_PATH);
		return ESP_FAIL;
	}
	size_t w = fwrite(json, 1, len, f);
	cJSON_free(json);
	if (w != len || fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		remove(TRACK_TMP_PATH);
		ESP_LOGE(TAG, "short write saving the track");
		return ESP_FAIL;
	}
	if (fclose(f) != 0) {
		remove(TRACK_TMP_PATH);
		return ESP_FAIL;
	}
	rename(TRACK_PATH, TRACK_BAK_PATH);
	if (rename(TRACK_TMP_PATH, TRACK_PATH) != 0) {
		rename(TRACK_BAK_PATH, TRACK_PATH);
		remove(TRACK_TMP_PATH);
		ESP_LOGE(TAG, "rename failed saving the track");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "track saved: %s", s_engine.have_track ? s_engine.track.name : "(none)");
	return ESP_OK;
}

/* Same rule the puck and the web handler enforce: 16-aligned inside the GPS
 * class range. A file value outside this would desync the hardware acceptance
 * filter (which masks the base to 11 bits) from the unmasked compare in
 * lap_engine_on_can_frame, silently hiding the puck. */
static bool gps_base_id_valid(uint16_t id) {
	return id >= 0x400 && id <= 0x430 && (id % 0x10) == 0;
}

static cJSON *track_read_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > TRACK_MAX_FILE_BYTES) {
		fclose(f);
		ESP_LOGW(TAG, "track file %s has an implausible size (%ld)", path, size);
		return NULL;
	}
	char *buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	return root;
}

static void track_load(void) {
	cJSON *root = track_read_file(TRACK_PATH);
	if (!root) {
		/* Main file missing or corrupt. track_save() rotates the previous good
		 * copy to .bak before the rename, so recover from it rather than boot
		 * with no track after a mid-write power loss. */
		root = track_read_file(TRACK_BAK_PATH);
		if (root)
			ESP_LOGW(TAG, "track file unreadable — recovered from %s", TRACK_BAK_PATH);
	}
	if (!root) {
		ESP_LOGI(TAG, "no track configured yet");
		return;
	}

	const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "gps_base_id");
	if (cJSON_IsNumber(it)) {
		uint16_t id = (uint16_t)it->valueint;
		if (gps_base_id_valid(id))
			s_base_id = id;
		else
			ESP_LOGW(TAG, "track file gps_base_id 0x%X invalid — keeping 0x%03X", id, s_base_id);
	}

	const cJSON *sf = cJSON_GetObjectItemCaseSensitive(root, "start_finish");
	if (cJSON_IsObject(sf)) {
		lap_track_t t;
		memset(&t, 0, sizeof(t));
		it = cJSON_GetObjectItemCaseSensitive(root, "name");
		if (cJSON_IsString(it) && it->valuestring)
			snprintf(t.name, sizeof(t.name), "%s", it->valuestring);
		it = cJSON_GetObjectItemCaseSensitive(root, "min_lap_time_s");
		if (cJSON_IsNumber(it))
			t.min_lap_time_s = (float)it->valuedouble;
		line_from_json(sf, &t.start_finish);

		const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "sectors");
		if (cJSON_IsArray(arr)) {
			const cJSON *el;
			cJSON_ArrayForEach(el, arr) {
				if (t.sector_count >= LAP_MAX_SECTORS)
					break;
				line_from_json(el, &t.sectors[t.sector_count]);
				t.sector_count++;
			}
		}
		lap_core_set_track(&s_engine, &t);
		ESP_LOGI(TAG, "track loaded: '%s', %u sector line(s)", t.name, t.sector_count);
	}
	cJSON_Delete(root);
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* (Re-)assert the lap channels' signals and bindings. Called from
 * lap_engine_start() on EVERY dashboard_init, mirroring dtc_monitor_start()
 * and signal_internal_start(), because that is the only context where it can
 * work at all and the only way it stays working:
 *
 *  - signal_registry_init() first runs at splash/dashboard, long after
 *    lap_engine_init() — registering these signals at init time silently
 *    returns -1 and signal_set_external_value() no-ops forever after.
 *  - The registry is wiped by signal_registry_reset() on ECU switch and the
 *    fallback-layout path, and resolve_signals() then CLEARS any binding
 *    whose name no longer resolves. Re-asserting here repairs both.
 *
 * Disk discipline: channel_manager_set_signal() flushes channels.json
 *  synchronously, so it is called ONLY when the stored binding actually
 * differs (first activation, or repair after a registry wipe cleared it) and
 * inside a bulk window so at most ONE write happens. The steady-state boot
 * performs zero flash writes here. */
static void assert_channel_bindings(void) {
	channel_manager_begin_bulk();
	for (size_t i = 0; i < LAP_CHANNEL_COUNT; i++) {
		channel_t *c = channel_manager_get(LAP_CHANNELS[i].channel_id);
		if (!c)
			c = channel_manager_activate(LAP_CHANNELS[i].channel_id);
		if (!c) {
			ESP_LOGW(TAG, "could not activate '%s'", LAP_CHANNELS[i].channel_id);
			continue;
		}
		signal_register_with_source(LAP_CHANNELS[i].signal_name, 0, 0, 0, 1.0f, 0.0f, false, 1,
		                            c->units_native, SIGNAL_SOURCE_INTERNAL);
		if (strcmp(c->signal_name, LAP_CHANNELS[i].signal_name) != 0)
			channel_manager_set_signal(c, LAP_CHANNELS[i].signal_name);
	}
	channel_manager_end_bulk();
	/* Bind channel->signal indexes for everything just registered. No disk
	 * writes on the happy path — resolve only persists when it must clear a
	 * stale binding. */
	channel_manager_resolve_signals();
}

esp_err_t lap_engine_init(void) {
	lap_core_init(&s_engine);

	/* Channel ACTIVATION only — the signal registry does not exist yet at
	 * this point in boot (see assert_channel_bindings above), so all signal
	 * work waits for lap_engine_start(). Activating here means the channels
	 * are visible to the web API from the first moment it serves. */
	channel_manager_begin_bulk();
	for (size_t i = 0; i < LAP_CHANNEL_COUNT; i++) {
		if (!channel_manager_get(LAP_CHANNELS[i].channel_id) &&
		    !channel_manager_activate(LAP_CHANNELS[i].channel_id))
			ESP_LOGW(TAG, "could not activate '%s'", LAP_CHANNELS[i].channel_id);
	}
	channel_manager_end_bulk();

	track_load();

	/* If a previous boot already bound the GPS channels, channels.json has
	 * them with the right decode — don't rewrite the file on every startup
	 * just because a puck said hello. */
	channel_t *lat = channel_manager_get("gps_latitude");
	if (lat && lat->can_id == (uint32_t)(s_base_id + GPS_OFF_POSITION))
		s_gps_channels_bound = true;

	ESP_LOGI(TAG, "lap engine ready (GPS base 0x%03X, channels %s)", s_base_id,
	         s_gps_channels_bound ? "already bound" : "bind on detect");
	return ESP_OK;
}

/* Runs at 1 Hz. Deliberately does nothing on a dash with no GPS. */
static void housekeeping_cb(lv_timer_t *t) {
	(void)t;
	if (s_bind_requested && !s_gps_channels_bound) {
		s_bind_requested = false;
		ESP_LOGI(TAG, "RDM GPS detected at 0x%03X — binding Position & GPS channels", s_base_id);
		if (lap_engine_bind_gps_channels() > 0)
			s_gps_channels_bound = true;
	}
}

void lap_engine_start(void) {
	/* NOT guarded by s_started: the re-assert must run on every layout load
	 * because an ECU switch or fallback load wipes the signal registry. */
	assert_channel_bindings();

	if (s_started)
		return;
	s_started = true;
	/* 1 Hz is plenty: its only job is to notice a newly-appeared puck and
	 * bind its channels once. Everything time-critical is driven by CAN. */
	if (!s_housekeeping_timer)
		s_housekeeping_timer = lv_timer_create(housekeeping_cb, 1000, NULL);
	ESP_LOGI(TAG, "lap engine started");
}

/* ── GPS channel binding ───────────────────────────────────────────────── */
int lap_engine_bind_gps_channels(void) {
	int bound = 0;
	channel_manager_begin_bulk();
	for (size_t i = 0; i < GPS_CHANNEL_COUNT; i++) {
		channel_t *c = channel_manager_get(GPS_CHANNELS[i].channel_id);
		if (!c)
			c = channel_manager_activate(GPS_CHANNELS[i].channel_id);
		if (!c) {
			ESP_LOGW(TAG, "could not activate '%s'", GPS_CHANNELS[i].channel_id);
			continue;
		}
		channel_manager_set_signal(c, GPS_CHANNELS[i].signal_name);
		/* endian 1 = Intel/little, matching the puck and the published DBC.
		 * persist_now = false: we are in a bulk loop, one flush at the end. */
		channel_manager_set_decode(c, (uint32_t)(s_base_id + GPS_CHANNELS[i].frame_offset),
		                           GPS_CHANNELS[i].bit_start, GPS_CHANNELS[i].bit_length,
		                           GPS_CHANNELS[i].scale, 0.0f, GPS_CHANNELS[i].is_signed, 1,
		                           GPS_CHANNELS[i].unit, false);
		bound++;
	}
	channel_manager_end_bulk();
	ESP_LOGI(TAG, "bound %d GPS channel(s) to base 0x%03X", bound, s_base_id);
	return bound;
}

/* ── CAN ingest ────────────────────────────────────────────────────────── */
void lap_engine_on_can_frame(uint32_t can_id, bool extd, const uint8_t *data, uint8_t dlc) {
	/* Hot path: every frame on the bus lands here. Reject in one compare
	 * before doing anything else. Extended (29-bit) ids that are numerically
	 * inside the block — perfectly legal for someone's J1939 device — are
	 * not RDM GPS frames and must not alias in. */
	if (can_id < s_base_id || can_id > (uint32_t)(s_base_id + 0xF) || extd || !data)
		return;

	uint8_t offset = (uint8_t)(can_id - s_base_id);
	/* Timestamp is taken when the frame is DRAINED on the LVGL task (fed here
	 * one-frame-at-a-time from can_process_queued_frames, not from the coalesced
	 * set), so no GPS fix is dropped and position/motion stay paired in arrival
	 * order. It does inherit LVGL-drain cadence rather than wire-arrival time;
	 * sub-frame timing precision beyond that would need an rx timestamp carried
	 * through s_can_queue. Good enough for lap timing at GPS rates; revisit if a
	 * puck timestamp field lands in the protocol. */
	uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

	if (offset == GPS_OFF_STATUS && dlc >= 4) {
		/* Only a frame that IDENTIFIES as an RDM GPS counts. Anything else
		 * broadcasting at base+0 — a mis-configured ECU, another vendor's
		 * device — must not trigger auto-bind or be trusted for fix state. */
		if (data[0] != GPS_DEVICE_TYPE_GPS)
			return;
		s_last_status_us = esp_timer_get_time();
		if (!s_gps_channels_bound)
			s_bind_requested = true; /* handled by the housekeeping timer */
		uint8_t fix_type = data[1];
		uint8_t flags = data[3];
		/* Same predicate as the puck's rdm_gps_fix_is_usable(): 2D or better,
		 * not time-only, and the receiver's own validity flag set. */
		s_fix_ok = (fix_type >= 2 && fix_type != 5 && (flags & GPS_FLAG_FIX_OK));
		return;
	}

	if (offset == GPS_OFF_POSITION && dlc >= 8) {
		/* Decoded here rather than read back from gps_latitude/gps_longitude
		 * because those channels are floats. int32 at 1e-7 deg into a double
		 * keeps the full 11 mm the puck actually sends. */
		int64_t lat_raw = can_extract_bits(data, 0, 32, 1, true);
		int64_t lon_raw = can_extract_bits(data, 32, 32, 1, true);
		s_pending_lat = (double)lat_raw * 1e-7;
		s_pending_lon = (double)lon_raw * 1e-7;
		s_pending_ms = now_ms;
		s_have_pending = true;
		return;
	}

	if (offset == GPS_OFF_MOTION && dlc >= 2) {
		/* Motion arrives immediately after position in the puck's burst, so
		 * this is the point at which a complete fix exists. Driving the engine
		 * from here (rather than from a timer) keeps latency at one CAN frame:
		 * at 200 km/h, 40 ms of scheduling slop is 2.2 m of position error at
		 * the line, which is larger than the entire accuracy budget. */
		if (!s_have_pending || (now_ms - s_pending_ms) > POSITION_PAIR_MAX_MS)
			return;

		int64_t speed_raw = can_extract_bits(data, 0, 16, 1, false);

		lap_fix_t fix = {
		    .lat_deg = s_pending_lat,
		    .lon_deg = s_pending_lon,
		    .speed_kph = (float)speed_raw * 0.01f,
		    .t_ms = s_pending_ms,
		    .usable = s_fix_ok,
		};
		s_have_pending = false;

		if (fix.usable) {
			s_last_lat = fix.lat_deg;
			s_last_lon = fix.lon_deg;
			s_last_speed_kph = fix.speed_kph;
			/* Heading (course over ground) is bytes 2-3. The puck sends a full
			 * 8-byte motion frame, but a short one would leave those bytes stale
			 * — only trust the heading when the frame actually carried them, or
			 * a garbage capture heading makes the timing line point nowhere. */
			if (dlc >= 4)
				s_last_heading_deg = (float)can_extract_bits(data, 16, 16, 1, false) * 0.01f;
			s_have_last_fix = true;
		}

		lap_event_t evt = lap_core_update(&s_engine, &fix);
		switch (evt) {
		case LAP_EVT_ARMED:
			ESP_LOGI(TAG, "start/finish crossed — timing lap 1");
			break;
		case LAP_EVT_SECTOR_COMPLETE:
			ESP_LOGI(TAG, "sector %u: %.3f s", (unsigned)(s_engine.sector_number - 1),
			         (double)s_engine.sector_time_last);
			break;
		case LAP_EVT_LAP_COMPLETE:
			ESP_LOGI(TAG, "lap %u: %.3f s (best %.3f s)", (unsigned)(s_engine.lap_number - 1),
			         (double)s_engine.lap_time_last, (double)s_engine.lap_time_best);
			break;
		default:
			break;
		}
		publish();
	}
}

/* ── Accessors ─────────────────────────────────────────────────────────── */
void lap_engine_set_base_id(uint16_t base_id) {
	if (base_id == s_base_id)
		return;
	s_base_id = base_id;
	s_gps_channels_bound = lap_engine_bind_gps_channels() > 0;
	track_save();
	/* The hardware acceptance filter carves out the GPS block by base id
	 * (build_twai_filter_from_signals) — rebuild it so the new block's
	 * frames actually reach us. */
	reconfigure_can_filter();
	ESP_LOGI(TAG, "GPS base id set to 0x%03X", base_id);
}

uint16_t lap_engine_get_base_id(void) {
	return s_base_id;
}

bool lap_engine_gps_present(void) {
	return s_last_status_us != 0 && (esp_timer_get_time() - s_last_status_us) < GPS_STALE_US;
}

bool lap_engine_last_position(double *lat_deg, double *lon_deg, float *heading_deg,
                              float *speed_kph) {
	if (!s_have_last_fix || !lap_engine_gps_present())
		return false;
	if (lat_deg)
		*lat_deg = s_last_lat;
	if (lon_deg)
		*lon_deg = s_last_lon;
	if (heading_deg)
		*heading_deg = s_last_heading_deg;
	if (speed_kph)
		*speed_kph = s_last_speed_kph;
	return true;
}

esp_err_t lap_engine_set_track(const lap_track_t *track) {
	lap_core_set_track(&s_engine, track);
	publish();
	return track_save();
}

const lap_track_t *lap_engine_get_track(void) {
	return s_engine.have_track ? &s_engine.track : NULL;
}

bool lap_engine_has_track(void) {
	return s_engine.have_track;
}

void lap_engine_reset_session(void) {
	lap_core_reset_session(&s_engine);
	publish();
	ESP_LOGI(TAG, "session reset");
}

const lap_engine_t *lap_engine_state(void) {
	return &s_engine;
}
