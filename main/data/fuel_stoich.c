/*
 * fuel_stoich.c — the runtime half of the fuel choice. See fuel_stoich.h.
 */

#include "fuel_stoich.h"
#include "unit_convert.h"
#include "channel_manager.h"
#include "widgets/signal.h"
#include "storage/config_store.h"
#include "system/rdm_lv_async.h"
#include "esp_log.h"
#include "lvgl.h"

#include <math.h>

static const char *TAG = "fuel_stoich";

/* How far a FLEX reading has to move before the dash re-scales. One percent of
 * ethanol is ~0.057 AFR, and flex sensors wander by about that much at rest —
 * so without this, a gauge scale labelled in AFR would re-sync every second
 * over sensor noise. 0.05 is roughly a 1% step in the blend. */
#define FLEX_HYSTERESIS_AFR  0.05f

/* Ethanol content changes when the tank is filled, not second to second. */
#define FLEX_POLL_MS         1000

/* Written from the HTTP task (set_mode), read on the LVGL task. An enum-sized
 * store is atomic on the S3. */
static volatile fuel_mode_t s_mode = FUEL_PETROL;

static bool      s_started = false;
static lv_timer_t *s_timer = NULL;

/* FLEX state — LVGL task only. The last good reading is held across brief
 * staleness so a sensor blip or a slow CAN start does not flash the readout
 * back to petrol and forward again. It is NOT held once there is no sensor to
 * wait for (the Ethanol % channel removed or unbound): see _flex_tick. */
static bool  s_flex_have = false;
static float s_flex_pct  = NAN;

/* Move the live ratio and tell whoever baked the old one into a scale.
 *
 * Readout widgets (panel, bar, text) convert every value as it arrives, so
 * they pick up the new ratio on their next update without being told. Meter
 * and arc do not: they derive a DISPLAY range from native bases when they
 * sync, so their tick labels would keep the old ratio while the needle moved
 * with the new one. The channel-changed notify is what makes them re-sync,
 * and it only goes to channels actually converting λ <-> AFR. LVGL task. */
static void _apply(float afr) {
	float cur = unit_convert_get_stoich();
	if (fabsf(afr - cur) < 0.0005f) return;
	if (!unit_convert_set_stoich(afr)) {
		ESP_LOGW(TAG, "refused stoich %.3f (outside %.1f-%.1f)",
		         afr, UNIT_STOICH_MIN, UNIT_STOICH_MAX);
		return;
	}
	ESP_LOGI(TAG, "stoich %.3f -> %.3f (%s)", cur, afr, fuel_mode_key(s_mode));
	channel_manager_notify_conversion_changed("λ", "AFR");
}

typedef enum {
	ETH_NO_SOURCE,   /* no Ethanol % channel, or nothing bound to it */
	ETH_STALE,       /* bound, but no fresh reading right now */
	ETH_FRESH,
} eth_read_t;

/* Read the ethanol channel. Telling "no source" from "stale" matters: a stale
 * sensor is worth waiting for, a deleted channel is not. */
static eth_read_t _read_ethanol(float *out) {
	channel_t *c = channel_manager_get("ethanol_pct");
	if (!c || c->signal_index < 0) return ETH_NO_SOURCE;
	signal_t *s = signal_get_by_index((uint16_t)c->signal_index);
	if (!s) return ETH_NO_SOURCE;
	if (s->is_stale || !isfinite(s->current_value)) return ETH_STALE;
	*out = s->current_value;
	return ETH_FRESH;
}

/* Fold one read into the held FLEX state. */
static void _flex_take(eth_read_t r, float pct) {
	if (r == ETH_FRESH) {
		s_flex_have = true;
		s_flex_pct  = pct;
	} else if (r == ETH_NO_SOURCE) {
		/* Found on the bench: delete the Ethanol % channel mid-drive and the
		 * dash kept the last blend indefinitely while the editor, correctly,
		 * said nothing was set up. With no sensor left there is nothing to
		 * hold for — fall back to petrol, the same as never having had one. */
		s_flex_have = false;
		s_flex_pct  = NAN;
	}
	/* ETH_STALE: keep whatever was held. */
}

static void _flex_tick(void) {
	float pct = NAN;
	_flex_take(_read_ethanol(&pct), pct);
	/* No reading yet this boot (or no sensor any more) -> petrol, which is also what the dash showed
	 * before a fuel could be chosen at all. */
	float target = s_flex_have ? fuel_stoich_for_ethanol(s_flex_pct)
	                           : FUEL_AFR_PETROL;
	if (fabsf(target - unit_convert_get_stoich()) >= FLEX_HYSTERESIS_AFR)
		_apply(target);
}

static void _timer_cb(lv_timer_t *t) {
	(void)t;
	if (s_mode == FUEL_FLEX) _flex_tick();
}

/* Apply whatever s_mode now says. LVGL task. */
static void _apply_mode(void) {
	if (s_mode == FUEL_FLEX) {
		/* Leaving a fixed fuel for FLEX: take a reading now rather than showing
		 * the old fixed ratio for up to a second. Hysteresis does not apply to
		 * the switch itself — it guards against noise, not against a choice. */
		float pct = NAN;
		_flex_take(_read_ethanol(&pct), pct);
		_apply(s_flex_have ? fuel_stoich_for_ethanol(s_flex_pct) : FUEL_AFR_PETROL);
	} else {
		_apply(fuel_stoich_for_mode(s_mode));
	}
}

static void _deferred_apply(void *arg) {
	(void)arg;
	if (s_mode != FUEL_FLEX) {
		/* A fixed fuel makes any held ethanol reading irrelevant; clearing it
		 * means a later switch back to FLEX starts from a fresh reading. Done
		 * here rather than in set_mode because this state is LVGL-task-only. */
		s_flex_have = false;
		s_flex_pct  = NAN;
	}
	_apply_mode();
}

void fuel_stoich_start(void) {
	if (s_started) return;
	s_started = true;

	uint8_t stored = FUEL_PETROL;
	config_store_load_fuel_mode(&stored);
	s_mode = (stored < FUEL__COUNT) ? (fuel_mode_t)stored : FUEL_PETROL;

	/* dashboard_init() builds the widgets before it gets here, so a fuel other
	 * than petrol changes the ratio under meters that already synced at 14.7.
	 * _apply's notify re-syncs them, and it runs before the first frame is
	 * drawn, so nothing flashes. For petrol this is a no-op. */
	_apply_mode();

	s_timer = lv_timer_create(_timer_cb, FLEX_POLL_MS, NULL);
	ESP_LOGI(TAG, "fuel %s, stoich %.3f", fuel_mode_key(s_mode),
	         unit_convert_get_stoich());
}

fuel_mode_t fuel_stoich_get_mode(void) {
	return s_mode;
}

esp_err_t fuel_stoich_set_mode(fuel_mode_t mode) {
	if (mode >= FUEL__COUNT) return ESP_ERR_INVALID_ARG;
	esp_err_t err = config_store_save_fuel_mode((uint8_t)mode);
	if (err != ESP_OK) return err;
	s_mode = mode;
	rdm_async_call(_deferred_apply, NULL);
	return ESP_OK;
}

float fuel_stoich_current(void) {
	return unit_convert_get_stoich();
}

bool fuel_stoich_flex_has_reading(void) {
	return s_flex_have;
}

float fuel_stoich_flex_ethanol_pct(void) {
	return s_flex_pct;
}
