/**
 * can_forward.c — forward the fuel-sender analog input over CAN.
 *
 * Pattern follows the button/toggle TX widgets (LVGL timer + can_pack_bits
 * + can_transmit_frame_ext) with OBD2-style tolerance for a bus that isn't
 * listening: transmit failures are counted, and after a burst of
 * consecutive failures the transmitter backs off for a few seconds instead
 * of driving the TWAI error counter towards BUS_OFF forever.
 *
 * Deliberate guards:
 *  - can_is_suspended(): the bus-scan owns the peripheral; sends would
 *    fail with INVALID_STATE.
 *  - signal_sim_is_active(): NEVER put simulated fuel data on a real bus —
 *    an ECU consuming this frame must only ever see the real sender.
 */
#include "can_forward.h"
#include "can/can_decode.h"
#include "can/can_manager.h"
#include "system/rdm_lv_async.h"
#include "widgets/signal_internal.h"
#include "widgets/signal_sim.h"
#include "esp_log.h"
#include "lvgl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "can_forward";

/* Backoff: after this many consecutive failures, pause for BACKOFF_TICKS
 * timer periods before trying again. */
#define FWD_FAIL_BURST     20
#define FWD_BACKOFF_MS     5000

static fuel_forward_config_t s_cfg;
static lv_timer_t *s_timer = NULL;
static uint32_t s_tx_ok = 0;
static uint32_t s_tx_fail = 0;
static uint32_t s_consec_fail = 0;
static int64_t  s_backoff_until_ms = 0;

static uint8_t _clamp_rate(uint8_t hz) {
	if (hz < 1) return 1;
	if (hz > 20) return 20;
	return hz;
}

static void _fwd_timer_cb(lv_timer_t *t) {
	(void)t;
	if (!s_cfg.enabled) return;
	if (can_is_suspended()) return;
	if (signal_sim_is_active()) return;

	if (s_backoff_until_ms) {
		int64_t now = (int64_t)lv_tick_get();
		/* lv_tick wraps at 2^32 ms (~49 days) — treat "past the mark or
		 * wrapped far below it" as expiry. */
		if (now < s_backoff_until_ms &&
		    (s_backoff_until_ms - now) < FWD_BACKOFF_MS * 2)
			return;
		s_backoff_until_ms = 0;
		s_consec_fail = 0;
	}

	float reading = (s_cfg.mode == 1) ? signal_internal_get_fuel_voltage()
	                                  : signal_internal_get_fuel_level();
	float scaled = reading * s_cfg.scale + s_cfg.offset;
	if (!isfinite(scaled)) return;

	/* Clamp to the field's representable range rather than wrapping —
	 * a mis-scaled config should peg, not alias. Field is unsigned. */
	uint32_t max_raw = (s_cfg.bit_length >= 32)
		? UINT32_MAX
		: ((1UL << s_cfg.bit_length) - 1UL);
	long raw_l = lroundf(scaled);
	uint32_t raw = raw_l <= 0 ? 0
	             : ((uint32_t)raw_l > max_raw ? max_raw : (uint32_t)raw_l);

	uint8_t frame[8] = {0};
	can_pack_bits(frame, s_cfg.bit_start, s_cfg.bit_length, raw, s_cfg.endian);

	esp_err_t err = can_transmit_frame_ext(s_cfg.can_id, s_cfg.extd, frame, 8);
	if (err == ESP_OK) {
		s_tx_ok++;
		s_consec_fail = 0;
	} else {
		s_tx_fail++;
		if (++s_consec_fail >= FWD_FAIL_BURST) {
			s_backoff_until_ms = (int64_t)lv_tick_get() + FWD_BACKOFF_MS;
			ESP_LOGW(TAG, "%u consecutive TX failures — backing off %d ms "
			         "(bus quiet / not ACKing?)",
			         (unsigned)s_consec_fail, FWD_BACKOFF_MS);
		}
	}
}

/* (Re)build the timer to match s_cfg. LVGL task only. */
static void _fwd_rebuild_timer(void) {
	if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
	if (!s_cfg.enabled) return;
	s_cfg.rate_hz = _clamp_rate(s_cfg.rate_hz);
	s_timer = lv_timer_create(_fwd_timer_cb, 1000 / s_cfg.rate_hz, NULL);
	ESP_LOGI(TAG, "forwarding fuel %s on 0x%lX @ %u Hz (%s, bit %u len %u)",
	         s_cfg.mode == 1 ? "voltage" : "level",
	         (unsigned long)s_cfg.can_id, (unsigned)s_cfg.rate_hz,
	         s_cfg.endian ? "LE" : "BE",
	         (unsigned)s_cfg.bit_start, (unsigned)s_cfg.bit_length);
}

static void _fwd_apply_async(void *arg) {
	fuel_forward_config_t *cfg = (fuel_forward_config_t *)arg;
	if (cfg) {
		s_cfg = *cfg;
		free(cfg);
	}
	s_consec_fail = 0;
	s_backoff_until_ms = 0;
	_fwd_rebuild_timer();
}

void can_forward_init(void) {
	static bool s_inited = false;
	if (s_inited) return;
	s_inited = true;
	config_store_load_fuel_forward(&s_cfg);
	_fwd_rebuild_timer();
}

void can_forward_apply(const fuel_forward_config_t *cfg) {
	if (!cfg) return;
	fuel_forward_config_t *copy = malloc(sizeof(*copy));
	if (!copy) return;
	*copy = *cfg;
	rdm_async_call(_fwd_apply_async, copy);
}

void can_forward_get_status(fuel_forward_config_t *cfg,
                            uint32_t *tx_ok, uint32_t *tx_fail) {
	if (cfg) *cfg = s_cfg;
	if (tx_ok) *tx_ok = s_tx_ok;
	if (tx_fail) *tx_fail = s_tx_fail;
}
