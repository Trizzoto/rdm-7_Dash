#pragma once

/* signal_replay — Play back a previously logged CSV through the signal system.
 *
 * Reads a CSV produced by data_logger (header: timestamp_ms,sig1,sig2,...) one
 * row at a time, scheduled by an LVGL timer that walks the file at the same
 * cadence the row timestamps imply. For each row, every column whose header
 * matches a registered signal name gets injected via signal_inject_test_value.
 *
 * Use cases:
 *   - Verify a layout works without being in the car
 *   - Debug a field issue from a stored log
 *   - Demo the dashboard with realistic values
 *
 * Threading: the worker timer runs on the LVGL task and uses signal injection
 * APIs that require LVGL ownership — start/stop must therefore be called from
 * the LVGL task too (use lv_async_call from web/serial handlers).
 *
 * Format: same CSV the data_logger writes —
 *     timestamp_ms,Signal A,Signal B,...
 *     0,123,45.6
 *     50,124,45.6
 *
 * Behavior:
 *   - Empty cells = skip injection for that signal on that row (lets you
 *     replay logs that have stale columns).
 *   - Speed multiplier: 1.0 = real-time, 2.0 = 2x faster, 0.5 = half speed.
 *     Maps to the LVGL timer period.
 *   - Loop = restart from row 0 when the file ends; otherwise stops cleanly.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start replaying the CSV at `path`. Existing replay is stopped first.
 *   speed = 1.0 for real-time, 2.0 for 2x, etc. Clamped to [0.1, 100.0].
 *   loop  = true to restart the file when EOF is reached.
 * Returns ESP_OK on success, an error if the file can't be opened or parsed.
 *
 * MUST be called on the LVGL task. */
esp_err_t signal_replay_start(const char *path, float speed, bool loop);

/* Stop the current replay. Safe no-op if not active. MUST be called on LVGL. */
void signal_replay_stop(void);

bool signal_replay_is_active(void);

/* Returns the row number we're currently positioned at, or 0 if inactive. */
uint32_t signal_replay_get_row(void);

/* Returns the total number of data rows discovered in the open file, or 0. */
uint32_t signal_replay_get_total_rows(void);

/* Returns the path of the currently-open file, or "" if inactive. */
const char *signal_replay_get_file(void);

/* Currently-active speed multiplier (1.0 = real-time). */
float signal_replay_get_speed(void);

#ifdef __cplusplus
}
#endif
