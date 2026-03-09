/**
 * dashboard.h — Phase 4: Dashboard coordinator.
 *
 * Wraps layout_manager_load() and wires the resulting widget instances
 * into the CAN dispatch system.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "widget_types.h"
#include "cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the dashboard widget layer on @p parent.
 *
 * Must be called after config_store_load_* and after the LVGL screen
 * object has been created.
 *
 * Sequence:
 *  1. layout_manager_init()  — mount LittleFS, generate default layout
 *  2. layout_manager_get_active() — retrieve saved layout name
 *  3. layout_manager_load()  — deserialise JSON, call factory + create()
 *  4. Fallback to widget_X_create() if load fails
 *  5. rebuild_can_dispatch() — refresh CAN → widget routing
 *
 * @param parent LVGL screen object (ui_Screen3).
 */
void dashboard_init(lv_obj_t *parent);

/**
 * @brief Return pointer to the internal widget pointer array.
 *
 * Valid only after dashboard_init() has been called.
 * The caller must not free any returned pointers.
 */
widget_t **dashboard_get_widgets(void);

/**
 * @brief Return the number of widget instances created by dashboard_init().
 */
uint8_t dashboard_get_widget_count(void);

/**
 * @brief Apply a pre-parsed JSON layout for hot-reload / live preview.
 *
 * Resets registry, re-creates all widgets from @p root, then rebuilds
 * CAN dispatch.  Falls back to default widgets if parsing fails.
 *
 * @param parent LVGL screen object.
 * @param root   Parsed cJSON layout tree (must have "widgets" array).
 */
void dashboard_apply_layout_json(lv_obj_t *parent, cJSON *root);

/**
 * @brief Sync legacy config globals (values_config[], label_texts[], etc.)
 *        into each widget's type_data struct.
 *
 * Call this after config modal callbacks have modified the legacy globals,
 * before persisting the layout as JSON.
 */
void dashboard_sync_config_to_widgets(void);

/**
 * @brief Reverse sync: push widget type_data back into legacy globals.
 *
 * Call after layout_manager_load() so that values_config[], label_texts[],
 * rpm_gauge_max, etc. reflect the freshly loaded JSON config.  This prevents
 * stale globals from overwriting type_data on the next save.
 */
void dashboard_sync_widgets_to_config(void);

/**
 * @brief Persist the current dashboard layout to LittleFS as JSON.
 *
 * Calls dashboard_sync_config_to_widgets() internally, then serialises
 * all widgets via to_json() and writes to the active layout file.
 *
 * @return ESP_OK on success.
 */
esp_err_t dashboard_persist_layout(void);

#ifdef __cplusplus
}
#endif
