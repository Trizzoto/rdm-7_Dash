#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char       label[32];          /* button text, default: "BTN" */
    /* CAN TX */
    uint32_t   tx_can_id;          /* 0 = disabled */
    uint8_t    tx_press_data[8];
    uint8_t    tx_press_dlc;       /* default: 8 */
    uint8_t    tx_release_data[8];
    uint8_t    tx_release_dlc;     /* default: 0 (0 = no release frame) */
    /* Appearance */
    lv_color_t bg_color;           /* default: 0x333333 */
    lv_color_t text_color;         /* default: 0xFFFFFF */
    lv_color_t pressed_color;      /* default: 0x555555 (visual feedback) */
    uint8_t    border_radius;      /* default: 5 */
    char       font[32];           /* font name, default: "" (use theme default) */
    /* LVGL runtime */
    lv_obj_t  *btn_obj;
    lv_obj_t  *label_obj;
} button_data_t;

widget_t *widget_button_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
