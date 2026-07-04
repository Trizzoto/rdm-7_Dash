/**
 * sd_file_ops.h — SD-card directory listing + file copy, shared by the
 * HTTP (web_server_assets.c) and serial (serial_commands.c) transports.
 *
 * Both transports expose the same layout/font SD listing and LittleFS<->SD
 * copy operations; this used to be two independently hand-rolled copies.
 * Image listing is deliberately NOT included here — the HTTP side validates
 * the RDMIMG header and reports width/height, which the serial side never
 * did, so unifying it would mean either regressing HTTP's richer metadata or
 * growing serial's response shape. Left as two implementations until a
 * concrete need to share that one makes the tradeoff worth it.
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief List every file in `dir` matching `ext` into `arr`.
 *
 * Adds one {"name": <filename without extension>, "size": <bytes>} object
 * per match. Silently adds nothing if `dir` doesn't exist or can't be
 * opened — callers are responsible for checking SD-card-mounted state
 * first, this is just the directory walk.
 *
 * @param arr      cJSON array to append entries to.
 * @param dir      Directory to scan (e.g. SD_LAYOUT_DIR).
 * @param ext      Extension to match, including the dot (e.g. ".json").
 * @param ext_len  strlen(ext) — passed in so callers with a compile-time
 *                 constant don't pay for a redundant strlen() per call.
 */
void sd_list_dir(cJSON *arr, const char *dir, const char *ext, size_t ext_len);

/**
 * @brief Copy `src` to `dst` in 4 KB chunks.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if `src` doesn't exist,
 *         ESP_FAIL on a zero-byte source or any read/write/allocation
 *         failure (partial `dst` is removed before returning).
 */
esp_err_t sd_copy_file(const char *src, const char *dst);

#ifdef __cplusplus
}
#endif
