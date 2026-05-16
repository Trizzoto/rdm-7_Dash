/**
 * dtc_monitor.h — Background DTC poller exposing DTC_COUNT as a signal.
 *
 * Periodically fires Mode 03 (stored DTCs) and pushes the result count
 * into the signal registry as DTC_COUNT. This lets warning widgets bind
 * to "DTC_COUNT > 0" and light up whenever the vehicle has any stored
 * fault — no manual PID config required.
 *
 * Independent of the OBD2 picker's polled PID list: works even with
 * zero PIDs enabled, because Mode 03 doesn't require PID enrolment.
 *
 * Runs entirely on the LVGL task (lv_timer drives the cadence; the
 * obd2_read_stored_dtcs callback fires on the OBD2 RX path which also
 * runs on the LVGL task). No threading concerns.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default poll cadence — DTCs don't appear second-to-second so 30 s
 * keeps the signal "live enough" without significant bus traffic. */
#define DTC_MONITOR_DEFAULT_INTERVAL_MS  30000

/* Register the DTC_COUNT signal and start the periodic poll. Idempotent —
 * calling twice is a no-op. Call once at boot after signal_registry_init
 * + obd2_init have run. */
void dtc_monitor_start(void);

/* Stop polling and (optionally) keep the last-known count. Idempotent. */
void dtc_monitor_stop(void);

/* Force a Mode 03 poll right now (bypassing the cadence timer). Used by
 * dtc_reader.c after a successful Clear so the warning widget refreshes
 * to 0 immediately rather than after the next 30 s tick. */
void dtc_monitor_refresh_now(void);

/* True if polling is active. */
bool dtc_monitor_is_running(void);

/* Last DTC count read (0 if never polled). Cached so a re-render path
 * doesn't have to wait for a fresh poll. */
uint8_t dtc_monitor_last_count(void);

#ifdef __cplusplus
}
#endif
