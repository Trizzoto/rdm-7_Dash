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

/**
 * @brief Write /lfs/layouts/rpm_meter_test.json with the new analog dial
 * layout.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t generate_rpm_meter_test_layout(void);

/**
 * @brief Write /lfs/layouts/_splash_Default.json with a centred RDM logo.
 *
 * Called on first boot when no splash layouts exist, giving the desktop
 * editor something to load and the device a proper splash to display.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t generate_default_splash(void);

#ifdef __cplusplus
}
#endif
