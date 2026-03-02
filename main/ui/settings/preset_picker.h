#ifndef PRESET_PICKER_H
#define PRESET_PICKER_H

#include "lvgl.h"

/* Legacy floating-panel preconfig (used by RPM/Gear/Speed screens) */
void show_preconfig_menu(lv_obj_t * parent);
void destroy_preconfig_menu(void);

/**
 * Open a full-screen preset picker overlay on lv_layer_top().
 * When the user selects a preset it updates values_config[value_id-1]
 * and the global g_*_input / g_*_dropdown widget arrays in-place.
 * @param parent_screen  The current active screen (used for context only).
 * @param value_id       1-13  — which config slot to populate.
 */
void open_preset_picker(lv_obj_t *parent_screen, uint8_t value_id);

#endif /* PRESET_PICKER_H */
