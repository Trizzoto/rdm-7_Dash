#ifndef UI_CALLBACKS_H
#define UI_CALLBACKS_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Callback function declarations
void label_input_event_cb(lv_event_t * e);
void bit_start_roller_event_cb(lv_event_t * e);
void bit_length_roller_event_cb(lv_event_t * e);
void decimal_dropdown_event_cb(lv_event_t * e);
void keyboard_event_cb(lv_event_t * e);
void free_value_id_event_cb(lv_event_t * e);
void endianess_roller_event_cb(lv_event_t * e);
void value_offset_input_event_cb(lv_event_t * e);
void can_id_input_event_cb(lv_event_t * e);
void scale_input_event_cb(lv_event_t * e);
void type_dropdown_event_cb(lv_event_t * e);

// Text Input Dialog System
typedef struct {
    lv_obj_t *modal;
    lv_obj_t *text_display;
    lv_obj_t *prefix_label;
    lv_obj_t *keyboard;
    lv_obj_t *target_textarea;
    char *current_text;
    size_t max_length;
    void (*on_confirm)(const char *text, void *user_data);
    void (*on_cancel)(void *user_data);
    void *user_data;
} text_input_dialog_t;

void show_text_input_dialog(lv_obj_t *target_textarea, const char *title, const char *placeholder,
                           void (*on_confirm)(const char *text, void *user_data),
                           void (*on_cancel)(void *user_data), void *user_data);
void show_text_input_dialog_ex(lv_obj_t *target_textarea, const char *title, const char *placeholder, bool show_prefix,
                              void (*on_confirm)(const char *text, void *user_data),
                              void (*on_cancel)(void *user_data), void *user_data);
void close_text_input_dialog(void);
void force_close_text_input_dialog(void);

/* =========================================================================
 * Fuel sender context — shared between menu_screen.c and ui_callbacks.c
 * ========================================================================= */
typedef struct {
    uint8_t      value_id;
    lv_obj_t    *set_label;
    lv_obj_t    *empty_btn;
    lv_obj_t    *full_btn;
    lv_obj_t    *empty_input;
    lv_obj_t    *full_input;
    lv_obj_t    *current_label;
    lv_timer_t  *update_timer;
    lv_obj_t    *filter_label;
    lv_obj_t    *filter_slider;
} fuel_sender_ctx_t;

/* Fuel sender event callbacks */
void fuel_sender_switch_event_cb(lv_event_t *e);
void fuel_sender_ctx_free_event_cb(lv_event_t *e);
void fs_empty_btn_event_cb(lv_event_t *e);
void fs_full_btn_event_cb(lv_event_t *e);
void fs_empty_v_input_event_cb(lv_event_t *e);
void fs_full_v_input_event_cb(lv_event_t *e);
void fs_filter_slider_event_cb(lv_event_t *e);
void fs_voltage_update_timer_cb(lv_timer_t *timer);

/* Bar/panel event callbacks */
void show_value_switch_event_cb(lv_event_t *e);
void invert_value_switch_event_cb(lv_event_t *e);
void speed_units_dropdown_event_cb(lv_event_t *e);
void custom_text_input_event_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif // UI_CALLBACKS_H 