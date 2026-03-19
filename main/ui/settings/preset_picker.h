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

/**
 * Callback invoked when a preset is applied via the callback picker variant.
 * @param item  The selected preconfig item (valid until callback returns).
 * @param ctx   Opaque user data passed to open_preset_picker_with_cb().
 */
typedef void (*preset_apply_cb_t)(const preconfig_item_t *item, void *ctx);

/**
 * Open the same full-screen preset picker, but invoke a callback instead
 * of using config_bridge.  The callback receives the selected preset item.
 */
void open_preset_picker_with_cb(preset_apply_cb_t cb, void *ctx);

/**
 * Build an embedded preset picker inside an existing LVGL container.
 * Used by the config modal PRESETS tab — same 3-column layout as the
 * full-screen picker but without the dim overlay.
 * @param parent  Container to build the picker in (caller owns lifecycle).
 * @param w       Available width in pixels.
 * @param h       Available height in pixels.
 * @param cb      Callback invoked when a preset is applied.
 * @param ctx     Opaque user data for the callback.
 */
void build_preset_picker_embedded(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                   preset_apply_cb_t cb, void *ctx);

#endif /* PRESET_PICKER_H */
