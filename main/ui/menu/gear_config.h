#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the custom gear CAN-value mapping table inside 'parent'.
 * @param gear_mode  0 = Custom mode; other values hide the section.
 */
void create_custom_gear_values_section(lv_obj_t *parent, uint8_t gear_mode);

/** Remove and clean up the custom gear values section. */
void hide_custom_gear_values_section(void);

/**
 * Read every input widget and commit the values to widget type_data
 * via config_bridge. Call before saving layout to ensure the latest
 * typed characters are persisted even if no VALUE_CHANGED event fired.
 */
void custom_gear_section_flush_to_config(void);

#ifdef __cplusplus
}
#endif
