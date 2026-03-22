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
    uint8_t    tx_bit_start;       /* bit position (0-63, default: 0) */
    uint8_t    tx_bit_length;      /* bit width (1-32, default: 1) */
    uint8_t    tx_endian;          /* 0 = big, 1 = little (default: 1) */
    bool       tx_send_release;    /* send frame on release (default: false) */
    bool       latch;              /* true = toggle on/off, false = momentary (default: false) */
    bool       latch_state;        /* runtime: current latched state */
    /* Appearance */
    lv_color_t bg_color;           /* default: 0x333333 */
    lv_color_t text_color;         /* default: 0xFFFFFF */
    lv_color_t pressed_color;      /* default: 0x555555 (visual feedback) */
    uint8_t    border_radius;      /* default: 5 */
    char       font[32];           /* font name, default: "" (use theme default) */
    uint8_t    label_align;         /* 0=left, 1=center, 2=right (default: 1) */
    int16_t    label_x;            /* label offset X from center, default: 0 */
    int16_t    label_y;            /* label offset Y from center, default: 0 */
    bool       show_label;         /* show/hide label text (default: true) */
    /* Image mode (empty = normal button, non-empty = image button) */
    char       image_name[64];
    /* LVGL runtime */
    lv_obj_t  *btn_obj;
    lv_obj_t  *label_obj;
    lv_obj_t  *img_obj;            /* runtime: LVGL image object (image mode) */
    void      *img_dsc;            /* runtime: lv_img_dsc_t* from rdm_image_load() */
} button_data_t;

widget_t *widget_button_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
