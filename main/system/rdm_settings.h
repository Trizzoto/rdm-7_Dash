/**
 * rdm_settings.h — high-level settings facade.
 *
 * For layouts, this provides a thin wrapper around layout_manager so that:
 *   - NVS only ever stores the active layout name (e.g. "default" or "main_dash")
 *   - All JSON layout payloads live exclusively on the LittleFS partition.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise settings subsystem.
 *
 * Currently this just ensures the layout manager has mounted LittleFS and that
 * a default layout exists. Safe to call multiple times.
 */
esp_err_t rdm_settings_init(void);

/**
 * @brief Get the active layout name stored in NVS.
 *
 * The returned name is a bare layout identifier (e.g. "default",
 * "main_dash"), not a full path or ".json" filename.
 */
esp_err_t rdm_settings_get_active_layout(char *name_out, size_t len);

/**
 * @brief Set the active layout name in NVS.
 *
 * @param name Bare layout name (without ".json").
 */
esp_err_t rdm_settings_set_active_layout(const char *name);

#ifdef __cplusplus
}
#endif

