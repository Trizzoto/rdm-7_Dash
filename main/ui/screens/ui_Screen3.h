#ifndef UI_SCREEN3_H
#define UI_SCREEN3_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui_helpers.h"
#include "ui_comp.h"
#include "ui_comp_hook.h"
#include "ui_events.h"

#define MAX_VALUES 13  // Maximum number of values that can be configured

#ifdef __cplusplus
extern "C" {
#endif

extern void save_warning_configs_to_nvs();
extern void load_warning_configs_from_nvs();
extern void load_values_config_from_nvs();
extern void device_settings_longpress_cb(lv_event_t* e);

// Style functions
void init_common_style(void);
lv_style_t* get_common_style(void);
lv_style_t* get_box_style(void);

extern lv_obj_t * g_label_input[];
extern lv_obj_t * g_can_id_input[];
extern lv_obj_t * g_endian_dropdown[];
extern lv_obj_t * g_bit_start_dropdown[];
extern lv_obj_t * g_bit_length_dropdown[];
extern lv_obj_t * g_scale_input[];
extern lv_obj_t * g_offset_input[];
extern lv_obj_t * g_decimals_dropdown[];
extern lv_obj_t * g_type_dropdown[];

extern lv_obj_t * ui_MenuScreen;
extern lv_obj_t * ui_RDM_Logo_Text;
extern lv_obj_t * brightness_bar;
extern lv_obj_t * brightness_label;

typedef enum {
    BIG_ENDIAN_ORDER = 0,
    LITTLE_ENDIAN_ORDER = 1
} endian_t;

  typedef struct {
       bool enabled;
       uint32_t can_id;
       uint8_t endianess;
       uint8_t bit_start;
       uint8_t bit_length;
       uint8_t decimals;
       float value_offset;
       float scale;
       int32_t bar_min;
       int32_t bar_max;
       int32_t bar_low;
       int32_t bar_high;  // <-- New high threshold field
       bool is_signed;
       float warning_high_threshold;
       float warning_low_threshold;
       lv_color_t warning_high_color;
       lv_color_t warning_low_color;
       bool warning_high_enabled;
       bool warning_low_enabled;
       lv_color_t rpm_bar_color;
       bool use_gps_for_speed;  // true for GPS, false for CAN ID
       bool use_mph;  // true for MPH, false for KMH
       uint8_t gear_detection_mode;  // 0=Custom, 1=MaxxECU, 2=Haltech
       uint32_t gear_custom_can_ids[12];  // Custom CAN IDs for N, R, 1-10
       uint8_t rpm_limiter_effect;  // 0=None, 1=Warning Circles, 2=Bar Flash, 3=Combined
       int32_t rpm_limiter_value;   // RPM limit value
       lv_color_t rpm_limiter_color; // Limiter effect color
       bool rpm_lights_enabled;     // Enable/disable RPM lights feature
       bool rpm_gradient_enabled;   // Enable/disable RPM bar gradient to redline
       // Fuel tank sender functionality
       bool use_fuel_input;         // Enable fuel input from IO6
       float fuel_empty_voltage;    // Voltage reading when tank is empty
       float fuel_full_voltage;     // Voltage reading when tank is full
       // Bar color configuration (for BAR1 and BAR2)
       lv_color_t bar_low_color;      // Color when value is below bar_low
       lv_color_t bar_high_color;     // Color when value is above bar_high  
       lv_color_t bar_in_range_color; // Color when value is between bar_low and bar_high
   } value_config_t;

typedef struct {
    uint32_t can_id;    // CAN ID to monitor
    uint8_t bit_position;    // Which bit to check (0-63)
    lv_color_t active_color;  // Color when warning is active
    char label[32];    // Warning label text
    bool is_momentary;  // true for momentary, false for toggle
    bool current_state; // tracks current toggle state
} warning_config_t;

typedef struct {
    uint32_t can_id;    // CAN ID to monitor
    uint8_t bit_position;    // Which bit to check (0-63)
    bool is_momentary;  // true for momentary, false for toggle
    bool current_state; // tracks current toggle state
    bool animation_enabled; // true to flash when active, false for solid
} indicator_config_t;

extern warning_config_t warning_configs[8];
extern indicator_config_t indicator_configs[2];  // Left and Right indicators
extern value_config_t values_config[13];
extern uint8_t current_value_id;

extern char label_texts[13][64];
extern char value_offset_texts[13][64];

void init_values_config_defaults(void);

// Initialize UI Screen3
void ui_Screen3_screen_init(void);

// Color wheel popup functions
void create_rpm_color_wheel_popup(void);
void create_limiter_color_wheel_popup(void);
void create_bar_low_color_wheel_popup(uint8_t value_id);
void create_bar_high_color_wheel_popup(uint8_t value_id);
void create_bar_in_range_color_wheel_popup(uint8_t value_id);

// Fuel timer management functions
void start_fuel_update_timer(void);
void stop_fuel_update_timer(void);
bool any_fuel_input_enabled(void);

// Rebuild CAN ID -> indices dispatch mapping (warnings/indicators/values)
void rebuild_can_dispatch(void);

// Indicator config management functions
void init_indicator_configs(void);
void save_indicator_configs_to_nvs(void);
void load_indicator_configs_from_nvs(void);
void create_indicator_config_menu(uint8_t indicator_idx);
void update_indicator_ui(void *param);

#ifdef __cplusplus
}
#endif

#endif // UI_SCREEN3_H
