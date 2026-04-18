#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initialize the data logger (call once at boot). Reads the persisted rate
 * from NVS via config_store_load_log_rate_hz so logging picks up where it
 * left off. */
void data_logger_init(void);

/* Start logging to a new CSV file on SD card at the currently configured rate. */
esp_err_t data_logger_start(void);

/* Start logging at a specific rate. Equivalent to:
 *     data_logger_set_rate_hz(rate_hz);
 *     data_logger_start();
 * but does both atomically and only persists the rate to NVS if persist=true.
 *
 * rate_hz semantics:
 *     0   = "Max" — sample on every LVGL tick (effectively bus-coalesced,
 *           ~70-200 Hz depending on LVGL load). One row per tick, regardless of
 *           how many signals updated.
 *     >0  = sample at the requested fixed rate (timer interval = 1000/hz ms,
 *           clamped to >=1 ms). Realistic upper bound is ~200 Hz before SD
 *           writes start backing up.
 */
esp_err_t data_logger_start_with_rate(uint16_t rate_hz, bool persist);

/* Stop logging and close the file */
esp_err_t data_logger_stop(void);

/* Check if currently logging */
bool data_logger_is_active(void);

/* Get current log filename (or empty string if not logging) */
const char *data_logger_current_file(void);

/* Get log statistics */
uint32_t data_logger_get_sample_count(void);
uint32_t data_logger_get_elapsed_ms(void);

/* ── Rate configuration ─────────────────────────────────────────────────── */

/* Set the sample rate. 0 = "Max" (every tick). Clamped to 0..1000.
 * Persists to NVS so the setting survives reboots.
 * If logging is currently active, the rate change takes effect immediately
 * by tearing down + recreating the timer. */
void data_logger_set_rate_hz(uint16_t hz);

/* Get the currently configured rate (0 = Max). */
uint16_t data_logger_get_rate_hz(void);

#ifdef __cplusplus
}
#endif
