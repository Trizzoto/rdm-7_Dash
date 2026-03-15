#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char       label[32];
    bool       current_state;       /* runtime: ON/OFF */
    /* Signal (optional) */
    char       signal_name[32];
    int16_t    signal_index;        /* -1 = no signal */
    float      signal_on_threshold; /* default: 0.5 */
    /* CAN TX */
    uint32_t   tx_can_id;           /* 0 = disabled */
    uint8_t    tx_on_data[8];
    uint8_t    tx_on_dlc;           /* default: 8 */
    uint8_t    tx_off_data[8];
    uint8_t    tx_off_dlc;          /* default: 8 */
    /* Appearance */
    lv_color_t active_color;        /* default: 0x00FF00 */
    lv_color_t inactive_color;      /* default: 0x555555 */
    lv_color_t label_color;         /* default: 0xFFFFFF */
    uint8_t    border_radius;       /* default: 5 */
    /* LVGL runtime */
    lv_obj_t  *sw_obj;
    lv_obj_t  *label_obj;
} toggle_data_t;

widget_t *widget_toggle_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
