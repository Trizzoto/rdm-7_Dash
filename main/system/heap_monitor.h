/**
 * heap_monitor.h — periodic heap snapshot to surface leaks.
 *
 * Wired in because we keep hitting ESP_ERR_NO_MEM crashes inside
 * esp_timer_create (called from esp_phy / WiFi paths). Without live
 * measurement we're guessing whether internal SRAM is draining and
 * how fast. This module dumps:
 *
 *   - free internal heap
 *   - largest free internal block (fragmentation indicator)
 *   - free PSRAM (sanity check; should stay roughly flat)
 *   - delta vs previous snapshot
 *
 * Every Nth snapshot (default 6 → ~1 min at 10 s cadence) it also
 * calls esp_timer_dump(stdout) so we can see every esp_timer in the
 * system. If "phy-track-pll-timer" appears multiple times across
 * snapshots, the IDF leak is confirmed.
 *
 * All logs use a single TAG ("heap_mon") so a serial filter can
 * isolate just the heap series for graphing.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default poll cadence — 10 s is fast enough to see leaks under a
 * minute, slow enough not to spam the log. */
#define HEAP_MONITOR_INTERVAL_MS  10000

/* Dump the esp_timer list every N snapshots (default = every minute
 * at 10 s cadence). Set to 0 to never dump. */
#define HEAP_MONITOR_TIMER_DUMP_EVERY  6

/* Start the monitor. Idempotent — calling twice is a no-op. */
void heap_monitor_start(void);

/* Stop the monitor. Idempotent. */
void heap_monitor_stop(void);

/* Force-log a snapshot right now (out-of-band — independent of the
 * periodic timer). Useful from event handlers (e.g. "log heap right
 * after WiFi disconnect" to correlate). */
void heap_monitor_snapshot_now(const char *tag);

#ifdef __cplusplus
}
#endif
