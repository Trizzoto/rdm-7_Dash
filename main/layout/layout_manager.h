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
#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of a layout name (without .json extension). */
#define LAYOUT_MAX_NAME 32

/* Maximum number of layouts the list function will return. */
#define LAYOUT_MAX_COUNT 16

/* Maximum JSON file size we'll read or accept (in bytes). */
#define LAYOUT_MAX_FILE_BYTES 32768

/* Current layout JSON schema version (single source of truth).
 * Written into every saved layout. No longer drives default.json regeneration
 * on boot — user edits to "default" are preserved across reboots. Factory
 * reset is the way to revert to the compiled-in default layout.
 *
 * v13: night-mode appearance overrides. Each widget's `config` may contain
 *      a `night` sub-object with delta values for color and image fields.
 *      Layout root may also contain a `night_mode` block with `signal_name`
 *      + `active_when` for CAN-signal-driven night mode triggering.
 *      Older schemas load fine — missing night blocks just mean no overrides. */
#define LAYOUT_SCHEMA_VERSION 13

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

/**
 * @brief Build an in-memory JSON representation of the current layout.
 *
 * This uses the same schema as layout_manager_save() but returns a cJSON tree
 * instead of writing to disk.  The caller owns the returned object and must
 * call cJSON_Delete() when done.
 *
 * @param name    Layout name (used for the \"name\" field).
 * @param widgets Array of widget_t pointers.
 * @param count   Number of widgets in @p widgets.
 * @return cJSON* root object on success, or NULL on error.
 */
cJSON *layout_manager_build_json(const char *name, widget_t **widgets,
								 uint8_t count);

/**
 * @brief Write a raw cJSON layout tree to /lfs/layouts/{name}.json.
 *
 * The root must contain at least a \"widgets\" array; other fields are
 * preserved as-is.  This is intended for layouts received from the web UI
 * which are already in the correct schema.
 *
 * @param name Layout name (without .json suffix).
 * @param root Parsed cJSON layout object.
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_save_raw(const char *name, const cJSON *root);

/**
 * @brief Read a layout file as raw text into a caller-provided buffer.
 *
 * @param name     Layout name (without .json suffix).
 * @param buf      Destination buffer.
 * @param buf_size Size of @p buf in bytes (file is truncated to buf_size-1).
 * @param out_len  If non-NULL, receives the number of bytes read.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file does not exist.
 */
esp_err_t layout_manager_read_raw(const char *name, char *buf,
								  size_t buf_size, size_t *out_len);

/**
 * @brief Return a monotonically increasing version counter.
 *
 * Incremented on every layout save (save_raw) and load.  The web editor
 * can poll this cheaply to decide whether the full layout JSON needs to
 * be re-fetched.
 */
uint32_t layout_manager_get_version(void);

/**
 * @brief Apply an already-parsed cJSON layout tree, creating widgets on
 *        @p parent without reading from disk.
 *
 * Used for hot-reload / live preview from the web editor.
 *
 * @param root   Parsed cJSON layout object (must have a "widgets" array).
 * @param parent LVGL parent object.
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_apply_json(cJSON *root, lv_obj_t *parent);

/**
 * @brief Read the current layout's night-mode CAN trigger config (if any).
 *
 * Populated whenever a layout is loaded that contains a `night_mode` block at
 * the root. Returns true if the layout has a trigger; false if night mode is
 * controlled solely by manual toggle / settings.
 *
 * @param out_signal       Buffer that receives the signal name (caller-owned).
 * @param signal_buf_size  Size of @p out_signal in bytes (33 is plenty).
 * @param out_active_when  Receives the value at-or-above which night mode is
 *                         considered active (e.g. 1.0 for boolean signals).
 * @return true if a trigger is configured, false otherwise.
 */
bool layout_manager_get_night_trigger(char *out_signal, size_t signal_buf_size,
                                      float *out_active_when);

/**
 * @brief Enumerate available splash layouts.
 *
 * Scans /lfs/layouts/ for files matching `_splash_*.json`, strips the
 * prefix and suffix, and writes bare names into @p names.
 *
 * @param names     Array of LAYOUT_MAX_NAME-byte buffers.
 * @param max_count Maximum entries to write.
 * @return Number of splash layouts found (≤ max_count), or -1 on error.
 */
int layout_manager_list_splash(char names[][LAYOUT_MAX_NAME], int max_count);

/**
 * @brief Persist the active splash layout name to NVS.
 *
 * @param name Bare splash name (e.g. "Default", "Racing").
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_set_active_splash(const char *name);

/**
 * @brief Read the active splash layout name from NVS.
 *
 * Copies the bare name into @p name_out.  Defaults to "Default" if
 * no splash has been explicitly set.
 *
 * @param name_out Buffer to receive the name.
 * @param len      Size of @p name_out in bytes.
 * @return ESP_OK on success.
 */
esp_err_t layout_manager_get_active_splash(char *name_out, size_t len);

#ifdef __cplusplus
}
#endif
