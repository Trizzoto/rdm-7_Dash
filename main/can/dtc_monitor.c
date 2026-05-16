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

static lv_timer_t *s_timer       = NULL;
static uint8_t     s_last_count  = 0;
static bool        s_in_flight   = false;

/* ── DTC response handler ────────────────────────────────────────────── */

static void _on_dtc_response(bool ok, const obd2_dtc_t *codes, uint8_t count,
                              uint8_t mode, void *user) {
    (void)codes; (void)mode; (void)user;
    s_in_flight = false;
    if (!ok) {
        /* ECU didn't respond, or NRC. Don't clobber DTC_COUNT — keep
         * whatever the last successful poll reported. A vehicle-off
         * state shouldn't make a stale warning indicator flicker. */
        ESP_LOGD(TAG, "Stored DTC poll: no response");
        return;
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

    s_timer = lv_timer_create(_tick_cb, DTC_MONITOR_DEFAULT_INTERVAL_MS, NULL);
    ESP_LOGI(TAG, "DTC monitor started (poll every %u ms)",
             (unsigned)DTC_MONITOR_DEFAULT_INTERVAL_MS);
}

void dtc_monitor_stop(void) {
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_in_flight = false;
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
