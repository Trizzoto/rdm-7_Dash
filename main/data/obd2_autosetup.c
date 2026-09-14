/*
 * obd2_autosetup.c — see obd2_autosetup.h and ADR-0073.
 *
 * The pieces already existed and are reused, not copied: the discovery scan
 * (obd2_discovery_start), the PID -> channel resolver the web page and the
 * dash's scan sheet both use (channel_obd2_matches), and the batch binder
 * (channel_apply_obd2). This file only decides WHEN to run them, and keeps
 * the promise across ignition cycles.
 *
 * Bus safety: discovery tries the other CAN bit rate only when the bus is
 * silent at the current one (its bus-alive gate), so on a Falcon with the
 * ignition on the dash never transmits at a rate the car isn't using. With
 * the ignition off nothing else is on the wire to disturb.
 */

#include "obd2_autosetup.h"
#include "channel_source_apply.h"
#include "../can/obd2.h"
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
/* ...this many times per boot. The flag survives, so the next ignition
 * cycle gets the same again: a dash wired to ignition retries every drive
 * without polling the bus for the whole of one. */
#define AUTOSETUP_MAX_TRIES       6
/* Someone else's scan holds the bus: look again shortly. */
#define AUTOSETUP_BUSY_MS         3000

/* Presets whose car broadcasts only part of the picture. Matched on make
 * alone: both Falcon generations (BA/BF and FG) answer OBD2 on the same bus
 * as their broadcast, and a future Falcon preset should inherit this. */
static const char *const WANTS_OBD2_MAKES[] = {
	"Ford",
};

static bool                s_pending  = false;
static bool                s_running  = false;
static uint8_t             s_tries    = 0;
static lv_timer_t         *s_timer    = NULL;
static obd2_autosetup_cb_t s_cb       = NULL;
static void               *s_cb_user  = NULL;

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

static void _scan_done(const obd2_scan_result_t *r, void *user) {
	(void)user;
	s_running = false;

	obd2_autosetup_result_t res = { .err = ESP_OK };
	if (r) {
		for (uint8_t i = 0; i < r->count; i++) {
			uint8_t p = r->pids[i];
			/* 0x20/0x40/... are the "which PIDs are supported" blocks — scan
			 * scaffolding, not readings. */
			if ((p & 0x1F) == 0) continue;
			res.readings++;
		}
	}
	res.answered = r && r->completed && res.readings > 0;

	if (res.answered) {
		size_t n = channel_obd2_matches(r->pids, r->count, s_matches,
		                                CH_OBD2_MATCH_MAX);
		size_t fresh = 0;
		for (size_t i = 0; i < n; i++)
			if (!s_matches[i].bound) s_fresh[fresh++] = s_matches[i];
		res.offered = n;
		if (fresh)
			res.err = channel_apply_obd2(s_fresh, fresh, &res.bound);
		/* Answered once is the promise kept, even if everything it offers
		 * was already set up. A later change of mind is the Scan button's. */
		_set_pending(false);
		ESP_LOGI(TAG, "car answered %u readings: %u offered, %u connected",
		         (unsigned)res.readings, (unsigned)n, (unsigned)res.bound);
	} else if (s_pending && ++s_tries < AUTOSETUP_MAX_TRIES) {
		res.will_retry = true;
		_schedule(AUTOSETUP_RETRY_MS);
		ESP_LOGI(TAG, "no OBD2 answer (try %u of %u) — asking again in %us",
		         (unsigned)s_tries, (unsigned)AUTOSETUP_MAX_TRIES,
		         (unsigned)(AUTOSETUP_RETRY_MS / 1000));
	} else {
		ESP_LOGI(TAG, "no OBD2 answer — still owed, next boot tries again");
	}

	if (s_cb) s_cb(&res, s_cb_user);
}

static void _attempt(void) {
	if (!s_pending || s_running) return;
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
	(void)version;
	if (!make || !make[0]) return false;
	for (size_t i = 0; i < sizeof(WANTS_OBD2_MAKES) / sizeof(WANTS_OBD2_MAKES[0]); i++)
		if (strcmp(make, WANTS_OBD2_MAKES[i]) == 0) return true;
	return false;
}

void obd2_autosetup_start(obd2_autosetup_cb_t cb, void *user) {
	s_cb = cb;
	s_cb_user = user;
	_set_pending(true);
	s_tries = 0;
	_unschedule();
	_attempt();
}

void obd2_autosetup_for_ecu(const char *make, const char *version) {
	if (obd2_autosetup_ecu_wants(make, version)) {
		ESP_LOGI(TAG, "%s %s reads part of the car over OBD2 — setting it up",
		         make, version ? version : "");
		/* Keep whoever is listening (the wizard) — this is the same setup. */
		obd2_autosetup_start(s_cb, s_cb_user);
	} else {
		obd2_autosetup_cancel();
	}
}

void obd2_autosetup_cancel(void) {
	_unschedule();
	s_tries = 0;
	_set_pending(false);
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

bool obd2_autosetup_pending(void) { return s_pending; }
bool obd2_autosetup_running(void) { return s_running; }

static void _boot_async(void *arg) {
	(void)arg;
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
