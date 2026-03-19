#ifndef DEVICE_SETTINGS_H
#define DEVICE_SETTINGS_H

#include "lvgl.h"
#include <stdint.h>

// Public function declarations
void device_settings_longpress_cb(lv_event_t* e);
void device_settings_with_return_screen(lv_obj_t* return_screen);
void set_display_brightness(int percent);
void init_display_brightness(void);

// Brightness dimmer switch — signal-based configurable input
typedef struct {
    char     signal_name[32];    // Signal name (e.g., "INDICATOR_LEFT", "Headlights")
    float    threshold;          // Value >= threshold = "active" (default: 0.5)
    bool     is_momentary;       // Momentary toggle vs persistent on/off
    bool     invert;             // Invert logic (active = dim when signal BELOW threshold)
    uint8_t  dim_brightness;     // Brightness when dimmed (5-100%)
    bool     enabled;            // Feature enabled
} brightness_dimmer_config_t;

extern brightness_dimmer_config_t dimmer_config;
extern uint8_t current_brightness;

void save_dimmer_config_to_nvs(void);
void load_dimmer_config_from_nvs(void);

/** Subscribe the dimmer to its configured signal.  Call after layout load
 *  (signal registry is reset on every reload, so this must re-subscribe). */
void dimmer_subscribe(void);

#endif // DEVICE_SETTINGS_H 