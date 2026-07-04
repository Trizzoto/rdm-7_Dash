#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Shared infrastructure included by serial_commands.c and every
// serial_commands_<domain>.c. Do NOT include this from headers that are
// pulled in by non-serial-command translation units.

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Image/font directory paths (same as web_server.c). Shared across
 * serial_commands_assets.c, serial_commands_logger.c and
 * serial_commands_upload.c. */
#define LFS_IMAGE_DIR "/lfs/images"
#define LFS_FONT_DIR  "/lfs/fonts"
#define IMAGE_MAX_SIZE (1200 * 1024)

/* Download chunk size (raw binary frames, no base64 overhead) — shared by
 * the image/font download handlers (serial_commands_assets.c) and the log
 * download handlers (serial_commands_logger.c). */
#define DOWNLOAD_CHUNK_SIZE (32 * 1024)

// ── JSON-RPC response helpers (defined in serial_commands.c) ──────────────
// Used by literally every handler across every serial_commands_*.c file.
void _send_response(int id, cJSON *result, const char *error);
void _send_ok(int id);
void _send_error(int id, const char *msg);

// ── Path-safety helpers (defined in serial_commands.c) ─────────────────────
// _name_is_safe: no dots, no slashes, no control chars.
// _filename_is_safe: allows dots (extensions like .csv); rejects "..".
// Used by the layout/image/font/sd/log-download domains.
bool _name_is_safe(const char *name);
bool _filename_is_safe(const char *name);

// ── Directory helper (defined in serial_commands.c) ────────────────────────
// Used by the image/font/sd domains.
void _ensure_dir(const char *path);

#ifdef __cplusplus
}
#endif
