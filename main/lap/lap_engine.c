#include "lap/lap_engine.h"

#include <string.h>

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
#define GPS_OFF_LAP      0x7u
#define GPS_OFF_SECTOR   0x8u
#define GPS_OFF_DELTA    0x9u

#define GPS_FLAG_FIX_OK 0x01u

/* Byte 7 of the lap/sector/delta frames — see rdm_gps.dbc / PROTOCOL.md §3. */
#define LAP_FLAG_HAVE_TRACK  0x01u
#define LAP_FLAG_ARMED       0x02u
#define LAP_FLAG_DELTA_VALID 0x04u

/* The no-time-yet sentinel. Times otherwise saturate at 0xFFFE, which is a
 * real (if implausible) 655.34 s lap — only the exact sentinel means "none
 * yet", never a merely huge value. */
#define LAP_TIME_NONE 0xFFFFu

/* Byte 0 of the status frame. 0x400 is only a CONVENTION — nothing stops a
 * customer's other ECU broadcasting there. The device type byte is what makes
 * detection trustworthy: no 0x02, no auto-bind, no fix trust. */
#define GPS_DEVICE_TYPE_GPS 0x02u

#define GPS_STALE_US 2000000 /* status silent this long = puck gone */

/* ── State ─────────────────────────────────────────────────────────────── */
static uint16_t s_base_id = LAP_GPS_BASE_ID_DEFAULT;
static bool s_started = false;

static int64_t s_last_status_us = 0;

/* The puck's own last broadcast — read-only mirror, nothing computed here.
 * Zero-initialised, which reads as "no lap yet" exactly like the sentinel
 * decode below produces, so a dash that has never heard a lap frame shows
 * the same 0:00.00 it always did. */
static float    s_lap_time_current = 0.0f;
static float    s_lap_time_last = 0.0f;
static float    s_lap_time_best = 0.0f;
static float    s_lap_time_theoretical = 0.0f;
static uint16_t s_lap_number = 0;
static float    s_sector_time_current = 0.0f;
static uint8_t  s_sector_number = 0;
static float    s_lap_delta = 0.0f;

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
 * freshness, zone transitions and subscribers all behave normally.
 *
 * These used to be COMPUTED here (lap_core.c, removed — see ADR-0008's
 * 2026-07-30 addendum). Now they are a straight mirror of what the puck
 * already decided; publish() below is fed by the CAN decode, not arithmetic. */
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
 * Mirrors docs/rdm_gps.dbc. All little-endian (Intel, endian = 1). Generic
 * channel-owned decode (ADR-0005) handles these independently of the frame
 * handler below — binding them here is a one-time setup, not a per-frame cost. */
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
	switch (i) {
	case 0: return s_lap_time_current;
	case 1: return s_sector_time_current;
	case 2: return s_lap_delta;
	case 3: return s_lap_time_last;
	case 4: return s_lap_time_best;
	case 5: return s_lap_time_theoretical;
	case 6: return (float)s_lap_number;
	case 7: return (float)s_sector_number;
	default: return 0.0f;
	}
}

static void publish(void) {
	/* Every channel, every frame — including the ones that only change at
	 * lap/sector events. signal_set_external_value refreshes last_update_ms
	 * unconditionally but only notifies subscribers (i.e. repaints widgets)
	 * on a VALUE CHANGE, so this costs nothing on screen. */
	for (size_t i = 0; i < LAP_CHANNEL_COUNT; i++)
		signal_set_external_value(LAP_CHANNELS[i].signal_name, channel_value_for(i));
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

/* A u16 lap/sector time field: the sentinel means "no time yet", anything
 * else (including the 0xFFFE saturation cap) is a real value in seconds. */
static float decode_lap_time(const uint8_t *data, uint8_t bit_start) {
	int64_t raw = can_extract_bits(data, bit_start, 16, 1, false);
	return (raw == LAP_TIME_NONE) ? 0.0f : (float)raw * 0.01f;
}

void lap_engine_on_can_frame(uint32_t can_id, bool extd, const uint8_t *data, uint8_t dlc) {
	/* Hot path: every frame on the bus lands here. Reject in one compare
	 * before doing anything else. Extended (29-bit) ids that are numerically
	 * inside the block — perfectly legal for someone's J1939 device — are
	 * not RDM GPS frames and must not alias in. */
	if (can_id < s_base_id || can_id > (uint32_t)(s_base_id + 0xF) || extd || !data)
		return;

	uint8_t offset = (uint8_t)(can_id - s_base_id);

	/* Position, motion and quality need no handling here at all: they are
	 * Position & GPS canonical channels with their own CAN decode
	 * (GPS_CHANNELS above), which the generic channel-owned pipeline
	 * (ADR-0005) already applies to every matching frame independently of
	 * this function. The only reason this function used to touch them was to
	 * feed the dash's own lap_core.c computation at double precision — gone
	 * now that the puck does that arithmetic and broadcasts the answer. */

	if (offset == GPS_OFF_STATUS && dlc >= 4) {
		/* Only a frame that IDENTIFIES as an RDM GPS counts. Anything else
		 * broadcasting at base+0 — a mis-configured ECU, another vendor's
		 * device — must not trigger auto-bind or be trusted as presence. */
		if (data[0] != GPS_DEVICE_TYPE_GPS)
			return;
		s_last_status_us = esp_timer_get_time();
		if (!s_gps_channels_bound)
			s_bind_requested = true; /* handled by the housekeeping timer */
		return;
	}

	if (offset == GPS_OFF_LAP && dlc >= 8) {
		uint8_t flags = data[7];
		/* The puck only ever SENDS this frame with a track loaded — "all
		 * three are sent only with a track loaded" (PROTOCOL.md §3) — so
		 * checking HAVE_TRACK here is a defensive belt, not the primary
		 * signal; silence is what "no track" actually looks like. */
		if (!(flags & LAP_FLAG_HAVE_TRACK))
			return;
		s_lap_time_current = decode_lap_time(data, 0);
		s_lap_time_last    = decode_lap_time(data, 16);
		s_lap_time_best    = decode_lap_time(data, 32);
		s_lap_number       = data[6];
		publish();
		return;
	}

	if (offset == GPS_OFF_SECTOR && dlc >= 8) {
		uint8_t flags = data[7];
		if (!(flags & LAP_FLAG_HAVE_TRACK))
			return;
		s_sector_time_current = decode_lap_time(data, 0);
		s_lap_time_theoretical = decode_lap_time(data, 32);
		s_sector_number = data[6];
		publish();
		return;
	}

	if (offset == GPS_OFF_DELTA && dlc >= 8) {
		uint8_t flags = data[7];
		/* No sentinel on delta — 0.00 s is dead-level with the reference, a
		 * real result. Validity rides on the flag alone (PROTOCOL.md §3); a
		 * delta received before it is valid is left at its last value rather
		 * than shown as a bogus number sourced from an undefined byte. */
		if (!(flags & LAP_FLAG_HAVE_TRACK) || !(flags & LAP_FLAG_DELTA_VALID))
			return;
		int64_t raw = can_extract_bits(data, 0, 16, 1, true);
		s_lap_delta = (float)raw * 0.01f;
		publish();
		return;
	}
}

/* ── Accessors ─────────────────────────────────────────────────────────── */
void lap_engine_set_base_id(uint16_t base_id) {
	if (base_id == s_base_id)
		return;
	s_base_id = base_id;
	s_gps_channels_bound = lap_engine_bind_gps_channels() > 0;
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
