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
    /* Non-zero = this is an OBD2 PID rather than a CAN broadcast channel.
     * The apply path enables the PID in the layout's polled_pids[] and
     * binds the widget to the OBD2 signal name — the bit/scale/offset
     * fields are ignored because OBD2 decodes via polling, not bit
     * extraction.
     *
     * 16-bit so Mode 22 (UDS) PIDs (Ford/GM/VW/newer Toyota) fit
     * without truncation. Mode 01/21 PIDs sit in the low byte. */
    uint16_t obd2_pid;
    /* OBD2 service byte (0x01 = Mode 01, 0x21 = Mode 21 Toyota etc.).
     * 0 → treated as Mode 01 for back-compat. Pairs with obd2_pid so
     * the apply path can disambiguate same-byte PIDs across services. */
    uint8_t obd2_service;
    /* Multiplexed frames. Some ECUs cycle several payloads through ONE CAN ID
     * and tag each with a frame index inside the message — Link's Generic Dash
     * puts 13+ payloads on 0x3E8 selected by byte 0. Rows for such an ECU all
     * share one can_id and differ by mux_value.
     *
     * mux_bit_length == 0 means "not multiplexed", which is what every other
     * preset in the catalogue leaves these at by omitting them entirely. */
    uint8_t mux_bit_start;
    uint8_t mux_bit_length;
    uint16_t mux_value;
} preconfig_item_t;

/* NULL-terminated array of preset CAN signal definitions */
extern const preconfig_item_t preconfig_items[];
extern const int preconfig_items_count;

/**
 * Callback invoked when a preset is applied.
 * @param item  The selected preconfig item (valid until callback returns).
 * @param ctx   Opaque user data passed to build_preset_picker_embedded().
 */
typedef void (*preset_apply_cb_t)(const preconfig_item_t *item, void *ctx);

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
