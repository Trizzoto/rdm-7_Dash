#pragma once

/* can_raw_logger — Capture every CAN frame to CSV.
 *
 * Unlike data_logger (which writes decoded signal values, requires a layout
 * with signal definitions, and samples on a timer), this module logs each
 * raw frame as it arrives — useful when the user has no DBC yet and wants
 * to send the trace off-device for offline decoding.
 *
 * Format: SavvyCAN GVRET-CSV. Imports directly into SavvyCAN (File → Load
 * → Generic CSV / GVRET) without any preprocessing. Header columns:
 *
 *     Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
 *
 *   Time Stamp = microseconds since logger started
 *   ID         = hex with 0x prefix (e.g. 0x123 / 0x18EA00F1)
 *   Extended   = "true" for 29-bit frames, "false" for 11-bit
 *   Bus        = always 0 (single CAN interface)
 *   LEN        = data length code (0..8)
 *   D1..D8     = individual bytes as 0xNN; unused bytes are blank cells
 *
 * Storage tier matches data_logger: SD when mounted, else LittleFS capped
 * at the same per-file limit. Files named "canraw_<ts>.csv" so they appear
 * in /api/log/list alongside signal logs and can be downloaded/deleted via
 * the existing endpoints.
 *
 * Threading: can_raw_logger_record_frame() is called from the LVGL task
 * inside can_process_queued_frames(). Start/stop must also run on LVGL so
 * they don't race the writer.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent — safe to call multiple times. */
void can_raw_logger_init(void);

/* Open a new capture file and start recording. Returns ESP_ERR_INVALID_STATE
 * if already active, ESP_FAIL if the file can't be opened. */
esp_err_t can_raw_logger_start(void);

/* Close the file. No-op if not active. */
esp_err_t can_raw_logger_stop(void);

bool        can_raw_logger_is_active(void);
const char *can_raw_logger_current_file(void);
uint32_t    can_raw_logger_frame_count(void);
uint32_t    can_raw_logger_elapsed_ms(void);
const char *can_raw_logger_get_storage(void);  /* "sd" or "lfs" */

/* Append a single frame to the open file. Cheap no-op when not active —
 * safe to call from can_process_queued_frames() unconditionally. */
void can_raw_logger_record_frame(uint32_t id, bool ext,
                                  const uint8_t *data, uint8_t dlc);

#ifdef __cplusplus
}
#endif
