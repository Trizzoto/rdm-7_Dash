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
    uint8_t    tx_bit_start;        /* bit position (0-63, default: 0) */
    uint8_t    tx_bit_length;       /* bit width (1-32, default: 1) */
    uint8_t    tx_endian;           /* 0 = big, 1 = little (default: 1) */
    /* Appearance */
    lv_color_t active_color;        /* default: 0x00FF00 */
    lv_color_t inactive_color;      /* default: 0x555555 */
    lv_color_t label_color;         /* default: 0xFFFFFF */
    char       font[32];            /* font name, default: "" (use theme default) */
    uint8_t    label_align;          /* 0=left, 1=center, 2=right (default: 1) */
    int16_t    label_x;             /* label offset X from center, default: 0 */
    int16_t    label_y;             /* label offset Y from center, default: 0 */
    bool       show_label;          /* show/hide label text (default: true) */
    /* Image mode (empty = use switch, non-empty = use image) */
    char       image_name[64];
    uint8_t    active_opa;          /* opacity when ON (0-255, default: 255) */
    uint8_t    inactive_opa;        /* opacity when OFF (0-255, default: 100) */
    /* LVGL runtime */
    lv_obj_t  *sw_obj;
    lv_obj_t  *label_obj;
    lv_obj_t  *img_obj;             /* runtime: LVGL image object (image mode) */
    void      *img_dsc;             /* runtime: lv_img_dsc_t* from rdm_image_load() */
} toggle_data_t;

widget_t *widget_toggle_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
