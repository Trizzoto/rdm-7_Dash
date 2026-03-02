#ifndef DEVICE_SETTINGS_H
#define DEVICE_SETTINGS_H

#include "lvgl.h"
#include <stdint.h>

// Public function declarations
void device_settings_longpress_cb(lv_event_t* e);
void device_settings_with_return_screen(lv_obj_t* return_screen);
void set_display_brightness(int percent);
void init_display_brightness(void);
void load_ecu_preconfig(void);
uint8_t get_selected_ecu_preconfig(void);
uint8_t get_selected_ecu_version(void);

// Brightness dimmer switch
typedef struct {
    uint32_t can_id;
    uint8_t bit_position;
    bool is_momentary;
    bool invert_toggle;
    uint8_t brightness_value;
    bool enabled;
} brightness_dimmer_config_t;

extern brightness_dimmer_config_t dimmer_config;
extern bool previous_dimmer_bit_state;
extern uint8_t current_brightness;

void save_dimmer_config_to_nvs(void);
void load_dimmer_config_from_nvs(void);

#endif // DEVICE_SETTINGS_H 