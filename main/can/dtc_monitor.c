/**
 * dtc_monitor.c — see dtc_monitor.h
 *
 * Implementation detail: we register DTC_COUNT as a runtime signal at
 * start() so widgets can bind to it. We DO NOT pre-warm with a Mode 03
 * fire at start — the OBD2 RX path isn't fully set up at boot-time
 * dashboard_init order, and we don't want failed polls during cold-start
 * to lock the request slot or generate spurious "no response" logs.
 * The first scheduled tick fires the first real poll.
 */
#include "dtc_monitor.h"

#include "obd2.h"
#include "signal.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "dtc_monitor";

static lv_timer_t *s_timer                = NULL;
static uint8_t     s_last_count           = 0;
static bool        s_in_flight            = false;
static uint16_t    s_consecutive_failures = 0;
static bool        s_in_slow_mode         = false;

/* Switch the timer period without recreating the timer. lv_timer_set_period
 * is safe to call from inside a timer callback. */
static void _set_cadence(uint32_t period_ms, bool slow) {
    if (!s_timer) return;
    if (s_in_slow_mode == slow) return;
    lv_timer_set_period(s_timer, period_ms);
    s_in_slow_mode = slow;
    ESP_LOGI(TAG, "DTC poll cadence -> %s (%u ms)",
             slow ? "SLOW (bus inactive)" : "FAST (bus alive)",
             (unsigned)period_ms);
}

/* ── DTC response handler ────────────────────────────────────────────── */

static void _on_dtc_response(bool ok, const obd2_dtc_t *codes, uint8_t count,
                              uint8_t mode, void *user) {
    (void)codes; (void)mode; (void)user;
    s_in_flight = false;
    if (!ok) {
        /* ECU didn't respond, NRC'd, or TX queued-but-not-acked because
         * the bus is offline. Don't clobber DTC_COUNT — last good value
         * remains; warning indicators don't flicker on transient drops.
         *
         * Adaptive backoff: after N consecutive failures, slow the poll
         * cadence to keep bench/ignition-off use quiet. A single success
         * snaps back to fast mode (handled in the success branch). */
        if (s_consecutive_failures < UINT16_MAX) {
            s_consecutive_failures++;
        }
        if (s_consecutive_failures == DTC_MONITOR_SLOW_THRESHOLD) {
            _set_cadence(DTC_MONITOR_SLOW_INTERVAL_MS, true);
        }
        ESP_LOGD(TAG, "Stored DTC poll: no response (fails=%u)",
                 s_consecutive_failures);
        return;
    }
    /* Success — reset failure counter and restore fast cadence if we'd
     * been throttled. */
    if (s_consecutive_failures > 0 || s_in_slow_mode) {
        s_consecutive_failures = 0;
        _set_cadence(DTC_MONITOR_FAST_INTERVAL_MS, false);
    }
    s_last_count = count;
    signal_set_external_value("DTC_COUNT", (float)count);
    ESP_LOGI(TAG, "DTC_COUNT = %u", count);
}

/* ── Timer callback ──────────────────────────────────────────────────── */

static void _tick_cb(lv_timer_t *t) {
    (void)t;
    if (s_in_flight) {
        /* Previous poll never came back — likely vehicle off. Skip this
         * tick to avoid stacking up; next tick will retry. */
        return;
    }
    s_in_flight = true;
    obd2_read_stored_dtcs(_on_dtc_response, NULL);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

void dtc_monitor_start(void) {
    /* Always check + re-register the signal: layout reloads call
     * signal_registry_reset() which memsets the signal array, dropping
     * any previously-registered DTC_COUNT. The timer itself only needs
     * to be created once (guarded below). */
    if (signal_find_by_name("DTC_COUNT") < 0) {
        int16_t idx = signal_register("DTC_COUNT",
                                       /*can_id=*/0,
                                       /*bit_start=*/0,
                                       /*bit_length=*/0,
                                       /*scale=*/1.0f,
                                       /*offset=*/0.0f,
                                       /*is_signed=*/false,
                                       /*endian=*/1,
                                       /*unit=*/"");
        if (idx < 0) {
            ESP_LOGW(TAG, "Failed to register DTC_COUNT signal — monitor disabled");
            return;
        }
        /* Re-prime the just-registered signal with the cached count so
         * widgets that bind it on layout-load see the last-known value
         * immediately, not stale "no data". */
        signal_set_external_value("DTC_COUNT", (float)s_last_count);
    }

    if (s_timer) return;   /* timer already running — nothing more to do */

    /* Start in FAST mode; backoff path will switch to SLOW automatically
     * if the bus is silent for DTC_MONITOR_SLOW_THRESHOLD ticks. */
    s_timer = lv_timer_create(_tick_cb, DTC_MONITOR_FAST_INTERVAL_MS, NULL);
    s_in_slow_mode = false;
    s_consecutive_failures = 0;
    ESP_LOGI(TAG, "DTC monitor started (fast: %u ms, slow: %u ms after %u fails)",
             (unsigned)DTC_MONITOR_FAST_INTERVAL_MS,
             (unsigned)DTC_MONITOR_SLOW_INTERVAL_MS,
             (unsigned)DTC_MONITOR_SLOW_THRESHOLD);
}

void dtc_monitor_stop(void) {
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_in_flight = false;
    s_consecutive_failures = 0;
    s_in_slow_mode = false;
    ESP_LOGI(TAG, "DTC monitor stopped");
}

void dtc_monitor_refresh_now(void) {
    if (!s_timer) return;   /* not started yet */
    if (s_in_flight) return;
    s_in_flight = true;
    obd2_read_stored_dtcs(_on_dtc_response, NULL);
}

bool dtc_monitor_is_running(void) {
    return s_timer != NULL;
}

uint8_t dtc_monitor_last_count(void) {
    return s_last_count;
}
