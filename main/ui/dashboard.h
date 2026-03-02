/**
 * dashboard.h — Phase 4: Dashboard coordinator.
 *
 * Wraps layout_manager_load() and wires the resulting widget instances
 * into the CAN dispatch system.
 */
#pragma once

#include "lvgl.h"
#include "widget_types.h"
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

#ifdef __cplusplus
}
#endif
