/**
 * layout_manager.h — Phase 3: Layout persistence layer.
 *
 * Reads and writes layout JSON files from /lfs/layouts/ on the LittleFS
 * partition.  Each layout file contains a list of widget_t descriptors
 * (type, id, x, y, w, h, config) plus schema metadata.
 *
 * The active layout name is stored in NVS (namespace "layout_mgr", key
 * "active").  On first boot, if no layouts exist, default_layout.c writes
 * a default layout that mirrors the current hardcoded Screen3 positions.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "widget_types.h"
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of a layout name (without .json extension). */
#define LAYOUT_MAX_NAME 32

/* Maximum number of layouts the list function will return. */
#define LAYOUT_MAX_COUNT 16

/* VFS base path for LittleFS.  All layout files are under LFS_LAYOUT_DIR. */
#define LFS_BASE_PATH "/lfs"
#define LFS_LAYOUT_DIR "/lfs/layouts"

/**
 * @brief Mount LittleFS (if not already mounted) and create the layouts
 *        directory if it is missing.
 *
 * Must be called once before any other layout_manager_* function.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t layout_manager_init(void);

/**
 * @brief Load a layout by name, create and position all its widget instances.
 *
 * Reads /lfs/layouts/{name}.json, deserialises it, calls the appropriate
 * factory function for each widget entry and then calls w->from_json() to
 * restore widget-specific config fields, followed by w->create() to build
 * the LVGL objects on @p parent.
 *
 * @param name   Layout name (without .json suffix).
 * @param parent LVGL parent object.
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_load(const char *name, lv_obj_t *parent);

/**
 * @brief Serialise an array of widget pointers and write to
 *        /lfs/layouts/{name}.json.
 *
 * @param name    Layout name (without .json suffix).
 * @param widgets Array of widget_t pointers to serialise.
 * @param count   Number of widgets in @p widgets.
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_save(const char *name, widget_t **widgets,
							  uint8_t count);

/**
 * @brief Delete the named layout file.
 *
 * @param name Layout name (without .json suffix).
 * @return ESP_OK on success, or ESP_ERR_NOT_FOUND if the file does not exist.
 */
esp_err_t layout_manager_delete(const char *name);

/**
 * @brief Enumerate available layouts.
 *
 * @param names     Array of LAYOUT_MAX_NAME-byte buffers populated by this
 *                  function.
 * @param max_count Maximum number of entries to write into @p names.
 * @return Number of layouts found (≤ max_count), or -1 on error.
 */
int layout_manager_list(char names[][LAYOUT_MAX_NAME], int max_count);

/**
 * @brief Persist the active layout name to NVS.
 *
 * @param name Layout name string.
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_set_active(const char *name);

/**
 * @brief Read the active layout name from NVS.
 *
 * Copies the null-terminated name into @p name_out.  If no active layout has
 * been set the function returns "default".
 *
 * @param name_out Buffer to receive the name.
 * @param len      Size of @p name_out in bytes.
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_get_active(char *name_out, size_t len);

/**
 * @brief Return true if at least one layout file exists in the layouts dir.
 */
bool layout_manager_any_exist(void);

#ifdef __cplusplus
}
#endif
