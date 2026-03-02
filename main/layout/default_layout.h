/**
 * default_layout.h — Phase 3: first-boot default layout generator.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write /lfs/layouts/default.json with the hardcoded Screen3 positions.
 *
 * Called once on first boot when no layouts exist.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t generate_default_layout(void);

#ifdef __cplusplus
}
#endif
