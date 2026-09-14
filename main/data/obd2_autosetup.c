/*
 * obd2_autosetup.c — see obd2_autosetup.h, ADR-0073 and ADR-0074.
 *
 * The pieces already existed and are reused, not copied: the discovery scan
 * (obd2_discovery_start), the PID -> channel resolver the web page and the
 * dash's scan sheet both use (channel_obd2_matches), and the batch binder
 * (channel_apply_obd2). This file only decides WHEN to run them, whether to
 * bind or only offer, and keeps both promises across ignition cycles.
 *
 * Bus safety: discovery tries the other CAN bit rate only when the bus is
 * silent at the current one (its bus-alive gate), so on a running car the
 * dash never transmits at a rate the car isn't using. With the ignition off
 * nothing else is on the wire to disturb.
 */

#include "obd2_autosetup.h"
#include "../storage/config_store.h"
#include "../system/rdm_lv_async.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "obd2_auto";

/* First try after boot waits for CAN to come up and the layout to load. */
#define AUTOSETUP_BOOT_DELAY_MS   15000
/* A car that didn't answer is asked again this often... */
#define AUTOSETUP_RETRY_MS        45000
/* ...this many times per boot for a SETUP. The flag survives, so the next
 * ignition cycle gets the same again: a dash wired to ignition retries every
 * drive without polling the bus for the whole of one. */
#define AUTOSETUP_MAX_TRIES       6
/* A CHECK is a courtesy, not a promise: a few tries this session (the car may
 * be switched on a minute after the preset went on), then leave a car that
 * doesn't speak OBD2 alone. Not persisted. */
#define CHECK_MAX_TRIES           4
/* Someone else's scan holds the bus: look again shortly. */
#define AUTOSETUP_BUSY_MS         3000

/* Presets whose car broadcasts only part of the picture, set up without
 * asking. Every other preset gets a CHECK and an offer.
 *
 * `version` NULL matches every version of the make. The identity differs by
 * path: the wizard and Studio import use the preconfig catalogue's make and
 * version, Studio's Quick ECU Setup uses ECU_PRESETS', so a car listed under
 * two names needs both here. */
static const struct { const char *make; const char *version; } WANTS_OBD2[] = {
	/* Ford Falcon BA/BF and FG — make-wide: both generations answer OBD2 on
	 * the same bus as their broadcast, and a future Falcon preset should
	 * inherit this. OBD2 adds timing, lambda, trims; on an FG MAP and IAT. */
	{ "Ford", NULL },
	/* Toyota 86 / Subaru BRZ / Scion FR-S (2012-2020). The factory bus has
	 * RPM, throttle, speed, coolant, oil temp, yaw and lateral g; MAP, intake
	 * temp, lambda, short fuel trim, ignition timing, battery and fuel level
	 * are OBD2-only on this platform (hardware-verified 2026-07-25, see the
	 * ECU_PRESETS entry). By version, not make: a future Toyota preset may
	 * broadcast everything. */
	{ "Toyota",          "GT86 Gen 1" },   /* preconfig catalogue */
	{ "Toyota / Subaru", "86 / BRZ" },     /* ECU_PRESETS */
};

static bool                s_pending  = false;   /* SETUP owed (persisted) */
static bool                s_checking = false;   /* CHECK under way        */
static bool                s_running  = false;
static uint8_t             s_tries    = 0;
static lv_timer_t         *s_timer    = NULL;
static obd2_autosetup_cb_t s_cb       = NULL;
static void               *s_cb_user  = NULL;

/* The standing offer: the PIDs the car answered on its last CHECK. Kept raw
 * and resolved on read, so an offer never lists a channel that was set up
 * some other way in the meantime. */
static uint8_t             s_offer_pids[OBD2_SCAN_MAX_PIDS];
static uint8_t             s_offer_n  = 0;

static EXT_RAM_BSS_ATTR obd2_channel_match_t s_matches[CH_OBD2_MATCH_MAX];
static EXT_RAM_BSS_ATTR obd2_channel_match_t s_fresh[CH_OBD2_MATCH_MAX];

static void _attempt(void);

static void _timer_cb(lv_timer_t *t) {
	(void)t;
	s_timer = NULL;          /* one-shot: LVGL deletes it after this returns */
	_attempt();
}

static void _schedule(uint32_t ms) {
	if (s_timer) {
		lv_timer_set_period(s_timer, ms);
		lv_timer_reset(s_timer);
		return;
	}
	s_timer = lv_timer_create(_timer_cb, ms, NULL);
	if (s_timer) lv_timer_set_repeat_count(s_timer, 1);
}

static void _unschedule(void) {
	if (s_timer) {
		lv_timer_del(s_timer);
		s_timer = NULL;
	}
}

static void _set_pending(bool p) {
	if (s_pending == p) return;
	s_pending = p;
	config_store_save_obd2_autosetup_pending(p);
}

static void _set_offer(const uint8_t *pids, uint8_t n) {
	if (n > OBD2_SCAN_MAX_PIDS) n = OBD2_SCAN_MAX_PIDS;
	if (n == 0 && s_offer_n == 0) return;
	if (n) memcpy(s_offer_pids, pids, n);
	s_offer_n = n;
	config_store_save_obd2_offer(s_offer_pids, n);
}

/* Readings a scan answered, not counting the supported-PID bitmask blocks
 * (0x20, 0x40, ...), which are scan scaffolding rather than readings. */
static uint8_t _count_readings(const uint8_t *pids, uint8_t n) {
	uint8_t c = 0;
	for (uint8_t i = 0; i < n; i++)
		if ((pids[i] & 0x1F) != 0) c++;
	return c;
}

/* Matches for `pids`, keeping only channels nothing feeds yet. */
static size_t _fresh_matches(const uint8_t *pids, uint8_t n, size_t *offered) {
	size_t m = channel_obd2_matches(pids, n, s_matches, CH_OBD2_MATCH_MAX);
	size_t fresh = 0;
	for (size_t i = 0; i < m; i++)
		if (!s_matches[i].bound) s_fresh[fresh++] = s_matches[i];
	if (offered) *offered = m;
	return fresh;
}

static void _scan_done(const obd2_scan_result_t *r, void *user) {
	(void)user;
	s_running = false;

	obd2_autosetup_result_t res = { .err = ESP_OK, .check = !s_pending };
	if (r) res.readings = _count_readings(r->pids, r->count);
	res.answered = r && r->completed && res.readings > 0;

	if (res.answered) {
		res.fresh = _fresh_matches(r->pids, r->count, &res.offered);
		if (s_pending) {
			if (res.fresh)
				res.err = channel_apply_obd2(s_fresh, res.fresh, &res.bound);
			/* Answered once is the promise kept, even if everything it
			 * offers was already set up. A later change of mind is the Scan
			 * button's. Nothing is left to offer either. */
			_set_pending(false);
			_set_offer(NULL, 0);
			ESP_LOGI(TAG, "SETUP: car answered %u readings: %u offered, %u connected",
			         (unsigned)res.readings, (unsigned)res.offered, (unsigned)res.bound);
		} else {
			/* CHECK: remember the answer; show what it could add. */
			_set_offer(r->pids, res.fresh ? r->count : 0);
			ESP_LOGI(TAG, "CHECK: car answers OBD2 (%u readings) — %u channel(s) it could fill",
			         (unsigned)res.readings, (unsigned)res.fresh);
		}
		s_checking = false;
	} else if (s_pending && ++s_tries < AUTOSETUP_MAX_TRIES) {
		res.will_retry = true;
		_schedule(AUTOSETUP_RETRY_MS);
		ESP_LOGI(TAG, "SETUP: no OBD2 answer (try %u of %u) — asking again in %us",
		         (unsigned)s_tries, (unsigned)AUTOSETUP_MAX_TRIES,
		         (unsigned)(AUTOSETUP_RETRY_MS / 1000));
	} else if (s_pending) {
		ESP_LOGI(TAG, "SETUP: no OBD2 answer — still owed, next boot tries again");
	} else if (s_checking && ++s_tries < CHECK_MAX_TRIES) {
		res.will_retry = true;
		_schedule(AUTOSETUP_RETRY_MS);
		ESP_LOGI(TAG, "CHECK: no OBD2 answer (try %u of %u)",
		         (unsigned)s_tries, (unsigned)CHECK_MAX_TRIES);
	} else {
		/* This car doesn't speak OBD2 (or not yet) — leave it alone. */
		s_checking = false;
		ESP_LOGI(TAG, "CHECK: no OBD2 answer — not offering it");
	}

	if (s_cb) s_cb(&res, s_cb_user);
}

static void _attempt(void) {
	if (!(s_pending || s_checking) || s_running) return;
	if (obd2_discovery_in_progress()) {
		/* A scan the user started is on the bus. Its answer would do, but it
		 * isn't ours to consume — ask again once it is finished. */
		_schedule(AUTOSETUP_BUSY_MS);
		return;
	}
	s_running = true;
	obd2_discovery_start(_scan_done, NULL);
	/* obd2_discovery_start refuses silently if a scan slipped in between. */
	if (!obd2_discovery_in_progress()) {
		s_running = false;
		_schedule(AUTOSETUP_BUSY_MS);
	}
}

bool obd2_autosetup_ecu_wants(const char *make, const char *version) {
	if (!make || !make[0]) return false;
	for (size_t i = 0; i < sizeof(WANTS_OBD2) / sizeof(WANTS_OBD2[0]); i++) {
		if (strcmp(make, WANTS_OBD2[i].make) != 0) continue;
		if (!WANTS_OBD2[i].version) return true;
		if (version && strcmp(version, WANTS_OBD2[i].version) == 0) return true;
	}
	return false;
}

void obd2_autosetup_start(obd2_autosetup_cb_t cb, void *user) {
	s_cb = cb;
	s_cb_user = user;
	s_checking = false;
	_set_pending(true);
	s_tries = 0;
	_unschedule();
	_attempt();
}

static void _check(void) {
	s_checking = true;
	s_tries = 0;
	_unschedule();
	_attempt();
}

void obd2_autosetup_check(obd2_autosetup_cb_t cb, void *user) {
	s_cb = cb;
	s_cb_user = user;
	if (s_pending) return;       /* a SETUP answers the same question, and binds */
	_check();
}

void obd2_autosetup_for_ecu(const char *make, const char *version) {
	if (obd2_autosetup_ecu_wants(make, version)) {
		ESP_LOGI(TAG, "%s %s reads part of the car over OBD2 — setting it up",
		         make, version ? version : "");
		/* Keep whoever is listening (the wizard) — this is the same setup. */
		obd2_autosetup_start(s_cb, s_cb_user);
	} else {
		/* A new car's ECU: nothing is owed to the old one, and the old
		 * car's offer is not this car's. Then ask whether this one speaks
		 * OBD2 too. With no preset at all there is nothing to fill in
		 * behind, so no check — the OBD2 screen or scan is for that car. */
		obd2_autosetup_cancel();
		if (make && make[0]) _check();
	}
}

void obd2_autosetup_cancel(void) {
	_unschedule();
	s_tries = 0;
	s_checking = false;
	_set_pending(false);
	_set_offer(NULL, 0);
}

void obd2_autosetup_listen(obd2_autosetup_cb_t cb, void *user) {
	s_cb = cb;
	s_cb_user = user;
}

void obd2_autosetup_forget(obd2_autosetup_cb_t cb) {
	if (s_cb == cb) {
		s_cb = NULL;
		s_cb_user = NULL;
	}
}

bool obd2_autosetup_pending(void)  { return s_pending; }
bool obd2_autosetup_running(void)  { return s_running; }
bool obd2_autosetup_checking(void) { return s_checking; }

size_t obd2_autosetup_offer(obd2_channel_match_t *out, size_t max_out,
                            uint8_t *readings) {
	if (readings) *readings = s_offer_n ? _count_readings(s_offer_pids, s_offer_n) : 0;
	if (!s_offer_n) return 0;
	size_t fresh = _fresh_matches(s_offer_pids, s_offer_n, NULL);
	if (fresh == 0) {
		/* Everything it offered got set up some other way: retired. */
		_set_offer(NULL, 0);
		if (readings) *readings = 0;
		return 0;
	}
	if (fresh > max_out) fresh = max_out;
	if (out) memcpy(out, s_fresh, fresh * sizeof(*out));
	return fresh;
}

bool obd2_autosetup_offer_scan(obd2_scan_result_t *out) {
	if (!s_offer_n || !out) return false;
	memset(out, 0, sizeof(*out));
	memcpy(out->pids, s_offer_pids, s_offer_n);
	out->count = s_offer_n;
	out->completed = true;
	return true;
}

void obd2_autosetup_dismiss_offer(void) {
	_set_offer(NULL, 0);
}

static void _boot_async(void *arg) {
	(void)arg;
	s_offer_n = config_store_load_obd2_offer(s_offer_pids, sizeof(s_offer_pids));
	if (!config_store_load_obd2_autosetup_pending()) return;
	s_pending = true;
	s_tries = 0;
	ESP_LOGI(TAG, "OBD2 setup still owed to this car — trying in %us",
	         (unsigned)(AUTOSETUP_BOOT_DELAY_MS / 1000));
	_schedule(AUTOSETUP_BOOT_DELAY_MS);
}

void obd2_autosetup_boot(void) {
	/* rdm_async_call takes the LVGL mutex itself — callable from app_main. */
	rdm_async_call(_boot_async, NULL);
}
