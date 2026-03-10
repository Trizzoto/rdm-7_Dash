#ifndef PRESET_PICKER_H
#define PRESET_PICKER_H

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/* Preconfig item — one predefined CAN signal definition */
typedef struct {
    const char* ecu;
    const char* version;
    const char* label;
    const char* can_id;
    uint8_t endianess;
    uint8_t bit_start;
    uint8_t bit_length;
    float scale;
    float value_offset;
    uint8_t decimals;
    bool is_signed;
} preconfig_item_t;

/* NULL-terminated array of preset CAN signal definitions */
extern const preconfig_item_t preconfig_items[];
extern const int preconfig_items_count;

/* Legacy floating-panel preconfig (used by RPM/Gear/Speed screens) */
void show_preconfig_menu(lv_obj_t * parent);
void destroy_preconfig_menu(void);

/**
 * Open a full-screen preset picker overlay on lv_layer_top().
 * When the user selects a preset it updates the widget type_data via
 * config_bridge and the global g_*_input / g_*_dropdown widget arrays in-place.
 * @param parent_screen  The current active screen (used for context only).
 * @param value_id       1-13  — which config slot to populate.
 */
void open_preset_picker(lv_obj_t *parent_screen, uint8_t value_id);

#endif /* PRESET_PICKER_H */
