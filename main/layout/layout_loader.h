/**
 * layout_loader.h — simple facade for loading layouts from LittleFS.
 *
 * Uses the layout_manager APIs to:
 *   - Read the active layout name from NVS (string only)
 *   - Load the corresponding JSON from /lfs/layouts/{name}.json
 *   - Instantiate widget_t instances on a given LVGL parent.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load the currently active layout onto @p parent.
 *
 * This assumes rdm_settings_init()/layout_manager_init() has already been
 * called during system startup to mount LittleFS and create default.json if
 * needed.
 */
esp_err_t layout_loader_load_active(lv_obj_t *parent);

/**
 * @brief Load a specific layout by bare name (no ".json" suffix).
 *
 * @param name   Layout name such as "default" or "main_dash".
 * @param parent LVGL parent screen.
 */
esp_err_t layout_loader_load_named(const char *name, lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

