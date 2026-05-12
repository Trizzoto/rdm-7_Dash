#pragma once

/**
 * can_upload — Send a recorded CAN trace from LittleFS / SD to the Cloudflare
 *              worker for shared debugging.
 *
 * Flow:
 *   1. Open the file at /lfs/logs/<filename>  or  /sdcard/logs/<filename>
 *   2. Read it into PSRAM (file capped at 10 MB on the worker side)
 *   3. Compute HMAC-SHA256 over "{make}\n{model}\n{deviceId}\n{timestamp}"
 *   4. POST to the worker with metadata in headers, file as body, HTTPS
 *
 * Designed to be called from a short-lived task (not the LVGL task — the
 * HTTPS handshake blocks for ~1 s). Sample wiring:
 *
 *   xTaskCreate(_upload_task, "can_up", 8192, params, 3, NULL);
 *
 * The web server endpoint /api/log/upload spawns this task and returns
 * immediately; the caller polls /api/log/upload/status for progress.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAN_UPLOAD_IDLE = 0,
    CAN_UPLOAD_RUNNING,
    CAN_UPLOAD_SUCCESS,
    CAN_UPLOAD_FAILED,
} can_upload_state_t;

typedef struct {
    can_upload_state_t state;
    int  http_status;        /* HTTP code on success, 0 if not reached */
    char message[128];       /* human-readable result string (key on success, error otherwise) */
    int  uploaded_bytes;     /* file size pushed */
} can_upload_status_t;

/**
 * @brief Start an upload. Spawns a worker task and returns immediately.
 *
 * @param filename  Base filename in /lfs/logs/ or /sdcard/logs/, e.g. "canraw_12345.csv".
 *                  Must not contain path separators.
 * @param make      Car manufacturer text (1..40 chars, sanitised server-side).
 * @param model     Car model text (1..40 chars).
 * @param notes     Optional free-form notes (NULL or "" to skip).
 *
 * @return ESP_OK if the task was created (poll status for actual result).
 *         ESP_ERR_INVALID_STATE if a previous upload is still running.
 *         ESP_ERR_INVALID_ARG on bad inputs.
 */
esp_err_t can_upload_start(const char *filename, const char *make,
                           const char *model, const char *notes);

/**
 * @brief Snapshot the current upload status. Safe to call from any task.
 */
void can_upload_get_status(can_upload_status_t *out);

#ifdef __cplusplus
}
#endif
