#ifndef UI_SCREEN3_H
#define UI_SCREEN3_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "driver/twai.h"
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
extern void build_twai_filter_from_configs(twai_filter_config_t *out_filter);

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
      bool use_gps_for_speed;  // DEPRECATED - no longer used (kept for NVS compatibility)
      bool use_mph;  // true for MPH, false for KMH
       uint8_t gear_detection_mode;  // 0=Custom, 1=MaxxECU, 2=Haltech, 3=Ford, 4=Speed/RPM Ratio
       uint32_t gear_custom_values[14];  // Custom values for P, R, N, D, 1-10 (what values represent each gear)
       // Custom icon configuration for gear display (7 icons for custom gear mode)
       uint8_t custom_icon_types[7];  // Icon type: 0=None, 1=KEY
       uint32_t custom_icon_values[7];  // CAN values that trigger each custom icon
       // Speed/RPM Ratio gear detection fields
       float tire_circumference_mm;  // Tire circumference in mm (e.g. 2000mm for typical car tire)
       float final_drive_ratio;      // Final drive/differential ratio (e.g. 3.42)
       float reverse_gear_ratio;     // Reverse gear ratio (e.g. 3.50)
       float gear_ratios[10];         // Gear ratios for gears 1-10 (e.g. [3.36, 2.07, 1.40, 1.00, 0.84, ...])
       uint8_t rpm_limiter_effect;  // 0=None, 1=Warning Circles, 2=Bar Flash, 3=Combined
       int32_t rpm_limiter_value;   // RPM limit value
       lv_color_t rpm_limiter_color; // Limiter effect color
       bool rpm_lights_enabled;     // Enable/disable RPM lights feature
      // RPM Background functionality
      bool rpm_background_enabled;  // Enable/disable RPM background color change
      int32_t rpm_background_value;  // RPM threshold value for background change
      lv_color_t rpm_background_color; // Background color when RPM exceeds threshold
      // Bar color configuration (for BAR1 and BAR2)
       lv_color_t bar_low_color;      // Color when value is below bar_low
       lv_color_t bar_high_color;     // Color when value is above bar_high  
       lv_color_t bar_in_range_color; // Color when value is between bar_low and bar_high
       bool show_bar_value;           // Show/hide numeric value display on Screen 3
       bool invert_bar_value;         // Invert bar display (100 becomes 0, etc.)
       bool fuel_sender;              // Enable fuel sender input mapping for this bar
       float fuel_sender_empty_v;    // ADC voltage that represents 0 % (empty tank)
       float fuel_sender_full_v;     // ADC voltage that represents 100 % (full tank)
       uint8_t fuel_sender_filter;   // EMA smoothing 0 (off) – 100 (max)
       char custom_text[32];          // Custom text for panel display (bottom right corner)
   } value_config_t;

typedef struct {
    uint32_t can_id;    // CAN ID to monitor
    uint8_t bit_position;    // Which bit to check (0-63)
    uint8_t endianess;  // 0 = Big Endian, 1 = Little Endian
    lv_color_t active_color;  // Color when warning is active
    char label[32];    // Warning label text
    bool is_momentary;  // true for momentary, false for toggle
    bool current_state; // tracks current toggle state
    bool invert_toggle; // Invert CAN bus activation (1 becomes 0, 0 becomes 1)
} warning_config_t;

typedef struct {
    uint32_t can_id;    // CAN ID to monitor
    uint8_t bit_position;    // Which bit to check (0-63)
    bool is_momentary;  // true for momentary, false for toggle
    bool current_state; // tracks current toggle state
    bool animation_enabled; // true to flash when active, false for solid
    uint8_t input_source;   // 0 = Wire, 1 = CAN BUS
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

// Rebuild CAN ID -> indices dispatch mapping (warnings/indicators/values)
void rebuild_can_dispatch(void);

// Bar update struct shared between ui_Screen3.c and main.c
#ifndef BAR_UPDATE_T_DEFINED
#define BAR_UPDATE_T_DEFINED
typedef struct {
    uint8_t bar_index;
    int32_t bar_value;
    double  final_value;
    int     config_index;
    bool    is_timeout;
} bar_update_t;
#endif
void update_bar_ui(void *param);

// Fuel sender functions (defined in main.c)
void fuel_sender_adc_init(void);
float fuel_sender_read_voltage(void);
float fuel_sender_get_filtered_v(uint8_t bar_idx);
void fuel_sender_capture_empty(uint8_t value_id);
void fuel_sender_capture_full(uint8_t value_id);

// Indicator config management functions
void init_indicator_configs(void);
void save_indicator_configs_to_nvs(void);
void load_indicator_configs_from_nvs(void);
void create_indicator_config_menu(uint8_t indicator_idx);
void update_indicator_ui(void *param);
/** Apply analog (wire) indicator state; only updates indicators with input_source == Wire (0). */
void indicator_apply_analog_state(bool left_on, bool right_on);

#ifdef __cplusplus
}
#endif

#endif // UI_SCREEN3_H
