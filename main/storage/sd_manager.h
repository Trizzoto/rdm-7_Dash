#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define SD_BASE_PATH   "/sdcard"
#define SD_LAYOUT_DIR  "/sdcard/layouts"
#define SD_IMAGE_DIR   "/sdcard/images"
#define SD_FONT_DIR    "/sdcard/fonts"

/* Mount SD card (SPI). Non-fatal on failure — system runs without SD. */
esp_err_t sd_manager_init(void);

/* Returns true when the SD card is mounted and usable. */
bool sd_manager_is_mounted(void);

/* Query SD card free space.  Any out-pointer may be NULL.
 * Doubles as a health probe: two consecutive failures flip the mount state
 * to unmounted (hot-removal detection — the storage UI polls this). */
esp_err_t sd_manager_get_info(size_t *total, size_t *used, size_t *free_out);

/* Called by writers (loggers) after repeated write failures on /sdcard.
 * Probes the volume; if the card no longer responds, marks it unmounted so
 * status reporting and future tier selection reflect reality. Does NOT
 * unmount the VFS (open fds stay safe to fclose); reinsert needs a reboot. */
void sd_manager_notify_io_failure(void);

#ifdef __cplusplus
}
#endif
