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

/* Query SD card free space.  Any out-pointer may be NULL. */
esp_err_t sd_manager_get_info(size_t *total, size_t *used, size_t *free_out);

#ifdef __cplusplus
}
#endif
