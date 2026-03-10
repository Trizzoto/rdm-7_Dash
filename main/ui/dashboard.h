/**
 * dashboard.h — Dashboard coordinator.
 *
 * Wraps layout_manager_load() and wires the resulting widget instances
 * into the signal system.
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
 * Sequence:
 *  1. layout_manager_init()  — mount LittleFS, generate default layout
 *  2. layout_manager_get_active() — retrieve saved layout name
 *  3. layout_manager_load()  — deserialise JSON, call factory + create()
 *  4. Fallback to widget_X_create() if load fails
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
 * Resets registry, re-creates all widgets from @p root.
 * Falls back to default widgets if parsing fails.
 *
 * @param parent LVGL screen object.
 * @param root   Parsed cJSON layout tree (must have "widgets" array).
 */
void dashboard_apply_layout_json(lv_obj_t *parent, cJSON *root);

/**
 * @brief Persist the current dashboard layout to LittleFS as JSON.
 *
 * Serialises all widgets via to_json() and writes to the active layout file.
 *
 * @return ESP_OK on success.
 */
esp_err_t dashboard_persist_layout(void);

#ifdef __cplusplus
}
#endif
