#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "driver/twai.h"
#include "ui_preconfig.h"
#include "screens/ui_Screen3.h"
#include "esp_log.h"
#include "ui.h"
#include "ui_helpers.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "ui_wifi.h"
#include "freertos/semphr.h"
#include "ota_handler.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdio.h>
#include "version.h"
#include "nvs_flash.h"
#include "../ui.h"
#include "../ui_helpers.h"
#include "../ui_preconfig.h"
#include "../config/create_config_controls.h"
#include "../callbacks/ui_callbacks.h"
#include "../menu/menu_screen.h"
#include "gps/gps.h"      // Add GPS functionality
#include "driver/ledc.h"
#include "device_id.h"
#include "device_settings.h"
#include "fuel_input.h"

#define RPM_VALUE_ID 9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID 11
#define BAR1_VALUE_ID 12
#define BAR2_VALUE_ID 13
#define MAX_VALUES 13
#define BAR_UPDATE_THRESHOLD 1.0
#define LONG_PRESS_COOLDOWN 500
#define LABEL_TEXT_MAX_LEN 32
#define MAX_SPEED_CHANGE_PER_UPDATE 20.0  // Maximum realistic speed change between updates
#define MAX_VALID_SPEED 400.0  // Maximum valid speed value
#define MAX_RPM_LINES 200  // Maximum number of ticks per set

#ifndef EXAMPLE_MAX_CHAR_SIZE
#define EXAMPLE_MAX_CHAR_SIZE 64
#endif

// External declarations
extern TaskHandle_t canTaskHandle;
extern twai_timing_config_t g_t_config;
extern twai_general_config_t g_config;
extern twai_filter_config_t f_config;
extern volatile bool can_task_should_stop;
extern void can_receive_task(void *pvParameter);
extern void save_values_config_to_nvs();

lv_obj_t * ui_Label[13] = {NULL};
lv_obj_t * ui_Value[13] = {NULL};
lv_obj_t * ui_Box[8] = {NULL};
lv_obj_t * config_bars[13] = {NULL};
lv_obj_t * ui_MenuScreen = NULL;
lv_obj_t * keyboard = NULL; // Global keyboard object
lv_obj_t * rpm_bar_gauge = NULL;
extern lv_obj_t * menu_rpm_value_label; // RPM value label in menu screen for demo updates

// External references to menu preview objects for live updates
extern lv_obj_t * menu_speed_value_label;
extern lv_obj_t * menu_speed_units_label;
extern lv_obj_t * menu_gear_value_label;
extern lv_obj_t * menu_panel_value_labels[8];
extern lv_obj_t * menu_bar_objects[2];

lv_obj_t * ui_Gear_Label = NULL;
lv_obj_t * g_label_input[MAX_VALUES];
lv_obj_t * g_can_id_input[MAX_VALUES];
lv_obj_t * g_endian_dropdown[MAX_VALUES];
lv_obj_t * g_bit_start_dropdown[MAX_VALUES];
lv_obj_t * g_bit_length_dropdown[MAX_VALUES];
lv_obj_t * g_scale_input[MAX_VALUES];
lv_obj_t * g_offset_input[MAX_VALUES];
lv_obj_t * g_decimals_dropdown[MAX_VALUES];
lv_obj_t * g_type_dropdown[MAX_VALUES];

int rpm_gauge_max = 7000; // Default max RPM value
int rpm_redline_value = 6000; // Default redline RPM value
lv_obj_t * rpm_redline_zone = NULL; // Red rectangle for redline zone
value_config_t values_config[13];
uint8_t current_value_id;

typedef struct {
    uint8_t panel_index;
    char value_str[EXAMPLE_MAX_CHAR_SIZE];
    double final_value;
} panel_update_t;

typedef struct {
    uint8_t bar_index;    // 0 for BAR1, 1 for BAR2.
    int32_t bar_value;    // The clamped value for display.
    double final_value;   // The processed (scaled) CANbus value.
    int config_index;     // The index into values_config for this bar.
} bar_update_t;

typedef struct {
    char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
    int rpm_value;
} rpm_update_t;

typedef struct {
    char speed_str[EXAMPLE_MAX_CHAR_SIZE];
} speed_update_t;

typedef struct {
    char gear_str[EXAMPLE_MAX_CHAR_SIZE];
} gear_update_t;

char label_texts[13][64] = {"PANEL 1","PANEL 2","PANEL 3","PANEL 4","PANEL 5","PANEL 6","PANEL 7","PANEL 8", "RPM", "SPEED", "GEAR", "BAR 1", "BAR 2"};  // default texts
char value_offset_texts[13][64] = {"0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0"};  // default offsets
uint8_t endianess[13] = {1}; //Storage for endianness: 0 = Big Endian, 1 = Small Endian

static const lv_coord_t label_positions[8][2] = {
    {-312, -54}, {-146, -54}, {-312, 54}, {-146, 54},
    {146, -54}, {312, -54}, {146, 54}, {312, 54}
};
static const lv_coord_t value_positions[8][2] = {
    {-312, -17}, {-146, -17}, {-312, 91}, {-146, 91},
    {146, -17}, {312, -17}, {146, 91}, {312, 91}
};
static const lv_coord_t box_positions[8][2] = {
    {-312, -26}, {-146, -26}, {-312, 82}, {-146, 82},
    {146, -26}, {312, -26}, {146, 82}, {312, 82}
};

static const struct {
    int16_t x;
    int16_t y;
} warning_positions[] = {
    {-352, -148},  // Warning 1
    {-292, -148},  // Warning 2
    {-232, -148},  // Warning 3
    {-172, -148},  // Warning 4
    {172, -148},   // Warning 5
    {232, -148},   // Warning 6
    {292, -148},   // Warning 7
    {352, -148}    // Warning 8
};

warning_config_t warning_configs[8];
static lv_obj_t* warning_circles[8] = {NULL};
static lv_obj_t* warning_labels[8] = {NULL};
static uint64_t last_signal_times[8] = {0};
static bool toggle_debounce[8] = {false};
static uint64_t toggle_start_time[8] = {0};
// Limiter circles removed - only bar flash effect is supported
static lv_obj_t * rpm_lights_circles[8] = {NULL}; // Separate circles for RPM Lights
static lv_timer_t* limiter_demo_timer = NULL;
static lv_timer_t* limiter_flash_timer = NULL;
static bool limiter_demo_active = false;
static bool limiter_flash_state = false;
static lv_color_t original_rpm_color;
static uint8_t current_effect_type = 0;

static int current_canbus_rpm = 0;  // Store the current CAN bus RPM value
static int saved_rpm_before_demo = 0;  // Save RPM value before demo starts

// CAN timeout tracking
static uint64_t last_panel_can_received[8] = {0};
static uint64_t last_speed_can_received = 0;
static uint64_t last_gear_can_received = 0;
static uint64_t last_bar_can_received[2] = {0};
static uint64_t last_rpm_can_received = 0;

// Limiter activation tracking
static bool limiter_active = false;
static bool real_limiter_active = false;
static int last_rpm_for_limiter_check = -1;

// Forward declarations for optimization functions
static void batch_update_rpm_circles_color(lv_color_t color);

typedef struct {
    uint8_t warning_idx;
    lv_obj_t** input_objects;
    lv_obj_t** preview_objects;
} warning_save_data_t;

// Indicator config variables
indicator_config_t indicator_configs[2];  // Left and Right indicators
static bool indicator_toggle_debounce[2] = {false};
static uint64_t indicator_toggle_start_time[2] = {0};
static bool previous_indicator_bit_states[2] = {false};

// Animation variables
static lv_timer_t* indicator_animation_timer = NULL;
static bool indicator_animation_state = false; // true for bright, false for dim
static bool previous_indicator_states[2] = {false}; // Track previous states to avoid redundant updates

void init_values_config_defaults(void) {
    for(int i = 0; i < 13; i++) {
        values_config[i].enabled = false;
        values_config[i].can_id = 0;
        values_config[i].endianess = 0;
        values_config[i].bit_start = 0;
        values_config[i].bit_length = 0;
        values_config[i].decimals = 0;
        values_config[i].value_offset = 0;
        values_config[i].scale = 1;
        values_config[i].is_signed = false;
      
		if (i < 8) {
		values_config[i].warning_high_threshold = 0;
        values_config[i].warning_low_threshold = 0;
        values_config[i].warning_high_color = lv_color_hex(0xFF0000); // Default red
        values_config[i].warning_low_color = lv_color_hex(0x19439a); // Default blue
        values_config[i].warning_high_enabled = false;
        values_config[i].warning_low_enabled = false;
        }
    }
  
    values_config[RPM_VALUE_ID - 1].enabled = true;
    values_config[RPM_VALUE_ID - 1].rpm_bar_color = lv_color_hex(0xFF0000); // Default red
    values_config[RPM_VALUE_ID - 1].rpm_limiter_effect = 0; // Default: None
    values_config[RPM_VALUE_ID - 1].rpm_limiter_value = 7000; // Default: 7000 RPM
    values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0xFF0000); // Default: Red
    values_config[RPM_VALUE_ID - 1].rpm_lights_enabled = false; // Default: Disabled
    values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled = false; // Default: Disabled
    values_config[SPEED_VALUE_ID - 1].enabled = true;
    values_config[GEAR_VALUE_ID - 1].enabled = true;
    values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 1; // Default to MaxxECU
    // Initialize custom gear CAN IDs to 0 (disabled)
    for (int j = 0; j < 12; j++) {
        values_config[GEAR_VALUE_ID - 1].gear_custom_can_ids[j] = 0;
    }
    values_config[BAR1_VALUE_ID - 1].enabled = true;
    values_config[BAR2_VALUE_ID - 1].enabled = true;
  
    // Set default ranges for bars
    values_config[BAR1_VALUE_ID - 1].bar_min = 0;
    values_config[BAR1_VALUE_ID - 1].bar_max = 100;
    values_config[BAR2_VALUE_ID - 1].bar_min = 0;
    values_config[BAR2_VALUE_ID - 1].bar_max = 100;
    
    // Set default bar colors for BAR1 and BAR2
    values_config[BAR1_VALUE_ID - 1].bar_low_color = lv_color_hex(0x19439a);      // Blue
    values_config[BAR1_VALUE_ID - 1].bar_high_color = lv_color_hex(0xFF0000);     // Red
    values_config[BAR1_VALUE_ID - 1].bar_in_range_color = lv_color_hex(0x38FF00); // Green
    
    values_config[BAR2_VALUE_ID - 1].bar_low_color = lv_color_hex(0x19439a);      // Blue
    values_config[BAR2_VALUE_ID - 1].bar_high_color = lv_color_hex(0xFF0000);     // Red
    values_config[BAR2_VALUE_ID - 1].bar_in_range_color = lv_color_hex(0x38FF00); // Green
}

static void init_warning_configs(void) {
    // Initialize warning configurations with defaults
    for (int i = 0; i < 8; i++) {
        warning_configs[i].can_id = 0x000;
        warning_configs[i].bit_position = 0;
        warning_configs[i].active_color = lv_color_hex(0xFF0000);
        snprintf(warning_configs[i].label, sizeof(warning_configs[i].label), "Warning %d", i + 1);
        warning_configs[i].is_momentary = true;
        warning_configs[i].current_state = false;
    }
}

void init_indicator_configs(void) {
    // Initialize indicator configurations with defaults
    for (int i = 0; i < 2; i++) {
        indicator_configs[i].can_id = 0x000;
        indicator_configs[i].bit_position = 0;
        indicator_configs[i].is_momentary = true;
        indicator_configs[i].current_state = false;
        indicator_configs[i].animation_enabled = true; // Default to animated
    }
}

void print_value_config(uint8_t value_id) {
    uint8_t idx = value_id - 1;
    printf("Value #%d Configuration:\n", value_id);
    printf("  Enabled: %s\n", values_config[idx].enabled ? "Yes" : "No");
    printf("  CAN ID: %u\n", values_config[idx].can_id);
    printf("  Endianess: %s\n", values_config[idx].endianess == 0 ? "Big Endian" : "Little Endian");
    printf("  Bit Start: %d\n", values_config[idx].bit_start);
    printf("  Bit Length: %d\n", values_config[idx].bit_length);
    printf("  Decimals: %d\n", values_config[idx].decimals);
    printf("  Value Offset: %g\n", values_config[idx].value_offset);
    printf("  Scale: %g\n", values_config[idx].scale);
    printf("-------------------------------------------\n");
}

/////////////////////////////////////////////	FORWARD DECLERATIONS	/////////////////////////////////////////////
void update_rpm_lines(lv_obj_t *parent);
void set_rpm_value(int rpm);
void update_redline_position(void);
void create_warning_config_menu(uint8_t warning_idx);
void create_indicator_config_menu(uint8_t indicator_idx);
static void value_long_press_event_cb(lv_event_t * e);
void keyboard_ready_event_cb(lv_event_t * e);
void rpm_color_dropdown_event_cb(lv_event_t * e);
void rpm_limiter_effect_dropdown_event_cb(lv_event_t * e);
void rpm_limiter_roller_event_cb(lv_event_t * e);
void rpm_limiter_color_dropdown_event_cb(lv_event_t * e);
void rpm_lights_switch_event_cb(lv_event_t * e);
void bar_low_color_event_cb(lv_event_t * e);
void bar_high_color_event_cb(lv_event_t * e);
void bar_in_range_color_event_cb(lv_event_t * e);
// Limiter circles color update function removed - only bar flash effect is supported
static void update_rpm_lights(int rpm_value);
// Limiter circles creation function removed - only bar flash effect is supported
void create_rpm_lights_circles(lv_obj_t * parent);
static void update_menu_rpm_value_text(int rpm_value);
// Limiter circles clear function removed - only bar flash effect is supported
static void clear_rpm_lights_circles(void);

void start_limiter_effect_demo(uint8_t effect_type);
void stop_limiter_effect_demo(void);
static void start_real_limiter_effect(uint8_t effect_type);
static void stop_real_limiter_effect(void);
void update_panel_ui(void *param);
void update_bar_ui(void *param);
void update_rpm_ui(void *param);
void update_speed_ui(void *param);
void update_gear_ui(void *param);
void update_warning_ui(void *param);
void update_indicator_ui(void *param);
void speed_preview_timer_cb(lv_timer_t * timer);  // Speed preview update timer callback
void calibration_speed_roller_event_cb(lv_event_t * e);  // Calibration speed roller callback
void gps_speed_update_timer_cb(lv_timer_t * timer);
static void save_indicator_config_cb(lv_event_t* e);
static void back_indicator_config_cb(lv_event_t* e);
void update_config_preview(uint8_t indicator_idx);
static void indicator_can_id_changed_cb(lv_event_t* e);
static void indicator_bit_pos_changed_cb(lv_event_t* e);
static void indicator_toggle_mode_changed_cb(lv_event_t* e);
static void indicator_animation_changed_cb(lv_event_t* e);
void indicator_animation_timer_cb(lv_timer_t* timer);

static bool rpm_color_needs_update = false;
static lv_color_t new_rpm_color;
char previous_values[13][64] = {0};
bool reset_can_tracking = false; // Flag to reset CAN tracking variables after screen recreation
float previous_bar_values[2] = {0, 0};
static uint32_t last_long_press_time = 0;
																												
/////////////////////////////////////////////	CALLBACKS	/////////////////////////////////////////////

void keyboard_ready_event_cb(lv_event_t * e) {
    lv_obj_t * keyboard = lv_event_get_target(e);
    // Hide the keyboard
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void value_long_press_event_cb(lv_event_t * e) {
    uint32_t current_time = lv_tick_get();
    if (current_time - last_long_press_time < LONG_PRESS_COOLDOWN) return;
    last_long_press_time = current_time;

    uint8_t * p_id = (uint8_t *)lv_event_get_user_data(e);
    if (!p_id) return;

    current_value_id = *p_id;
    printf("Long press detected on value %u\n", current_value_id);
    load_menu_screen_for_value(current_value_id);
}

void rpm_gauge_roller_event_cb(lv_event_t * e) {
    lv_obj_t * roller = lv_event_get_target(e);
    uint16_t selected = lv_roller_get_selected(roller);
    rpm_gauge_max = 3000 + (selected * 1000); //scale from 3000 to 12000
    
    // Stop any active demos before UI changes
    stop_limiter_effect_demo();
    
    // Clear RPM lights circles as they will be invalidated by UI changes
    clear_rpm_lights_circles();
    
    // Update the RPM bar gauge range to match the new max
    if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
        lv_bar_set_range(rpm_bar_gauge, 0, rpm_gauge_max);
    }
    
	update_rpm_lines(lv_scr_act());
	update_redline_position();
}

void rpm_redline_roller_event_cb(lv_event_t * e) {
    lv_obj_t * roller = lv_event_get_target(e);
    uint16_t selected = lv_roller_get_selected(roller);
    rpm_redline_value = 3000 + (selected * 500); // Scale from 3000 to 12000 in 500 RPM increments
    
    // Stop any active limiter demo before UI changes
    stop_limiter_effect_demo();
    
    update_redline_position();
}

void rpm_ecu_dropdown_event_cb(lv_event_t * e)
{
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    // Indices: 0 = Custom, 1 = MaxxECU, 2 = Haltech

    if (selected == 0) {
        // ========== CUSTOM ==========
        printf("ECU Presets: Custom (no changes)\n");
        // Do nothing, or set defaults if you prefer
    }
    else if (selected == 1) {
        // ========== MAXXECU ==========
        printf("ECU Presets: MaxxECU\n");
        values_config[RPM_VALUE_ID - 1].can_id       = 520;  // 0x208
        values_config[RPM_VALUE_ID - 1].endianess    = 1;    // 0=Big,1=Little (check your usage)
        values_config[RPM_VALUE_ID - 1].bit_start    = 0;
        values_config[RPM_VALUE_ID - 1].bit_length   = 16;
        values_config[RPM_VALUE_ID - 1].scale        = 1.0f;
        values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
        values_config[RPM_VALUE_ID - 1].decimals     = 0;

        // Now update the UI fields for RPM (ID=9 => index=8)
        // --------------------------------------------------
        if (g_can_id_input[RPM_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", values_config[RPM_VALUE_ID - 1].can_id);
            lv_textarea_set_text(g_can_id_input[RPM_VALUE_ID - 1], buf);
        }

        // Endianness: your dropdown presumably has "Big Endian\nLittle Endian"
        // 0 => Big, 1 => Little
        if (g_endian_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_endian_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].endianess);
        }

        // Bit start
        if (g_bit_start_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_start_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].bit_start);
        }

        // Bit length (the dropdown might list 1..64, so we subtract 1 for zero-based index)
        if (g_bit_length_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_length_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].bit_length - 1);
        }

        // Scale
        if (g_scale_input[RPM_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[RPM_VALUE_ID - 1].scale);
            lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
        }

        // Value offset
        if (g_offset_input[RPM_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[RPM_VALUE_ID - 1].value_offset);
            lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
        }

        // Decimals
        if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].decimals);
        }
    }
    else if (selected == 2) {
        // ========== HALTECH ==========
        printf("ECU Presets: Haltech\n");
        values_config[RPM_VALUE_ID - 1].can_id       = 360;  // 0x209
        values_config[RPM_VALUE_ID - 1].endianess    = 0;
        values_config[RPM_VALUE_ID - 1].bit_start    = 0;
        values_config[RPM_VALUE_ID - 1].bit_length   = 16;
        values_config[RPM_VALUE_ID - 1].scale        = 1.0f;
        values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
        values_config[RPM_VALUE_ID - 1].decimals     = 0;

        // Update UI fields similarly
        // --------------------------------------------------
        if (g_can_id_input[RPM_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", values_config[RPM_VALUE_ID - 1].can_id);
            lv_textarea_set_text(g_can_id_input[RPM_VALUE_ID - 1], buf);
        }

        if (g_endian_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_endian_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].endianess);
        }

        if (g_bit_start_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_start_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].bit_start);
        }

        if (g_bit_length_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_length_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].bit_length - 1);
        }

        if (g_scale_input[RPM_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[RPM_VALUE_ID - 1].scale);
            lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
        }

        if (g_offset_input[RPM_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[RPM_VALUE_ID - 1].value_offset);
            lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
        }

        if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
                                     values_config[RPM_VALUE_ID - 1].decimals);
        }
    }

    // Print config to confirm
    print_value_config(RPM_VALUE_ID);
}

void bar_range_input_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * textarea = lv_event_get_target(e);
        const char * txt = lv_textarea_get_text(textarea);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
      
        bool is_min = lv_obj_get_user_data(textarea) != NULL; // Check if this is min input
        int32_t value = atoi(txt);

        if (value_id == BAR1_VALUE_ID) {
            if (is_min) {
                values_config[value_id - 1].bar_min = value;
                lv_bar_set_range(ui_Bar_1, value, values_config[value_id - 1].bar_max);
            } else {
                values_config[value_id - 1].bar_max = value;
                lv_bar_set_range(ui_Bar_1, values_config[value_id - 1].bar_min, value);
            }
        }
        else if (value_id == BAR2_VALUE_ID) {
            if (is_min) {
                values_config[value_id - 1].bar_min = value;
                lv_bar_set_range(ui_Bar_2, value, values_config[value_id - 1].bar_max);
            } else {
                values_config[value_id - 1].bar_max = value;
                lv_bar_set_range(ui_Bar_2, values_config[value_id - 1].bar_min, value);
            }
        }
    }
}

void bar_low_value_event_cb(lv_event_t * e) {
    lv_obj_t * ta = lv_event_get_target(e);
    const char * txt = lv_textarea_get_text(ta);
    int low_val = atoi(txt);
  
    // Retrieve value_id from the event's user data
    uint8_t * id_ptr = lv_event_get_user_data(e);
    uint8_t value_id = *id_ptr;
  
    // Update the configuration structure (make sure 'bar_low' is a valid field)
    values_config[value_id - 1].bar_low = low_val;
  
    // Retrieve the preview bar pointer (stored in the config_bars[] global array)
    lv_obj_t * menu_bar = config_bars[value_id - 1];
    if (menu_bar) {
        int current_val = lv_bar_get_value(menu_bar);
        if (current_val < low_val) {
            // Use configured low color
            lv_obj_set_style_bg_color(menu_bar, values_config[value_id - 1].bar_low_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        } else if (current_val > values_config[value_id - 1].bar_high) {
            // Use configured high color
            lv_obj_set_style_bg_color(menu_bar, values_config[value_id - 1].bar_high_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        } else {
            // Use configured in-range color
            lv_obj_set_style_bg_color(menu_bar, values_config[value_id - 1].bar_in_range_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        }
    }
}

void bar_high_value_event_cb(lv_event_t * e) {
       lv_obj_t * ta = lv_event_get_target(e);
       const char * txt = lv_textarea_get_text(ta);
       int high_val = atoi(txt);
     
       // Retrieve value_id from the event's user data
       uint8_t * id_ptr = lv_event_get_user_data(e);
       uint8_t value_id = *id_ptr;
     
       // Update the configuration structure with the new bar high threshold
       values_config[value_id - 1].bar_high = high_val;
     
       // Retrieve the preview bar pointer from the global config_bars array
       lv_obj_t * menu_bar = config_bars[value_id - 1];
       if (menu_bar) {
           int current_val = lv_bar_get_value(menu_bar);
           if (current_val < values_config[value_id - 1].bar_low) {
               // Use configured low color
               lv_obj_set_style_bg_color(menu_bar, values_config[value_id - 1].bar_low_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
           } else if (current_val > high_val) {
               // Use configured high color
               lv_obj_set_style_bg_color(menu_bar, values_config[value_id - 1].bar_high_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
           } else {
               // Use configured in-range color
               lv_obj_set_style_bg_color(menu_bar, values_config[value_id - 1].bar_in_range_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
           }
       }
   }

// Forward declaration for color wheel popup
void create_rpm_color_wheel_popup(void);
void create_limiter_color_wheel_popup(void);

void rpm_color_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
  
    // Determine new color based on selection - SUPER VIBRANT COLORS
    switch(selected) {
        case 0: new_rpm_color = lv_color_hex(0x00FF00); break; // Bright Green
        case 1: new_rpm_color = lv_color_hex(0x00FFFF); break; // Bright Cyan
        case 2: new_rpm_color = lv_color_hex(0xFFFF00); break; // Bright Yellow
        case 3: new_rpm_color = lv_color_hex(0xFF7F00); break; // Bright Orange
        case 4: new_rpm_color = lv_color_hex(0xFF0000); break; // Bright Red
        case 5: new_rpm_color = lv_color_hex(0x0080FF); break; // Bright Blue
        case 6: new_rpm_color = lv_color_hex(0x8000FF); break; // Bright Purple
        case 7: new_rpm_color = lv_color_hex(0xFF00FF); break; // Bright Magenta
        case 8: new_rpm_color = lv_color_hex(0xFF1493); break; // Bright Hot Pink
        case 9: // Custom color - open color wheel popup
            create_rpm_color_wheel_popup();
            return; // Don't update color yet, wait for color wheel selection
        default: new_rpm_color = lv_color_hex(0x00FF00); break;
    }
  
    // Don't update colors when real limiter effect is active to avoid conflicts with flashing
    if (!real_limiter_active) {
        rpm_color_needs_update = true;
    }
    values_config[RPM_VALUE_ID - 1].rpm_bar_color = new_rpm_color;
}

static void check_rpm_color_update(lv_timer_t * timer) {
    // Don't update colors when real limiter effect is active to avoid conflicts with flashing
    if (rpm_color_needs_update && !real_limiter_active) {
        if (rpm_bar_gauge) {
            lv_obj_set_style_bg_color(rpm_bar_gauge, new_rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            // Maintain gradient when updating color if enabled
            if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
                lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            } else {
                lv_obj_set_style_bg_grad_color(rpm_bar_gauge, new_rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            }
        }
        if (ui_Panel9) {
            lv_obj_set_style_bg_color(ui_Panel9, new_rpm_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        rpm_color_needs_update = false;
    }
}

// RPM Limiter Effect event callbacks
void rpm_limiter_effect_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    // Map dropdown selection to effect type (0=None, 2=Bar Flash, 3=Bar & Circles Flash, 4=Circles Flash)
    uint8_t effect_type = 0;
    if (selected == 1) {
        effect_type = 2; // Bar Flash only
    } else if (selected == 2) {
        effect_type = 3; // Bar & Circles Flash (combined effect)
    } else if (selected == 3) {
        effect_type = 4; // Circles Flash only
    }
    
    // Update configuration
    values_config[RPM_VALUE_ID - 1].rpm_limiter_effect = effect_type;
    
    // Demo the selected effect for 1 second
    start_limiter_effect_demo(effect_type);
}

void rpm_limiter_roller_event_cb(lv_event_t * e) {
    lv_obj_t * roller = lv_event_get_target(e);
    uint16_t selected = lv_roller_get_selected(roller);
    
    // Calculate RPM value: 3000 + (selected * 500)
    int32_t rpm_value = 3000 + (selected * 500);
    
    // Update configuration
    values_config[RPM_VALUE_ID - 1].rpm_limiter_value = rpm_value;
}

void rpm_limiter_color_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    switch (selected) {
        case 0: // Green
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0x00FF00);
            break;
        case 1: // Light Blue
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0x00FFFF);
            break;
        case 2: // Yellow
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0xFFFF00);
            break;
        case 3: // Orange
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0xFF7F00);
            break;
        case 4: // Red
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0xFF0000);
            break;
        case 5: // Dark Blue
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0x0080FF);
            break;
        case 6: // Purple
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0x8000FF);
            break;
        case 7: // Magenta
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0xFF00FF);
            break;
        case 8: // Pink
            values_config[RPM_VALUE_ID - 1].rpm_limiter_color = lv_color_hex(0xFF1493);
            break;
        case 9: // Custom
            create_limiter_color_wheel_popup();
            break;
    }
    
    // Limiter circles color update removed - only bar flash effect is supported
}

void rpm_lights_switch_event_cb(lv_event_t * e) {
    lv_obj_t * switch_obj = lv_event_get_target(e);
    bool is_checked = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
    
    // Update configuration
    values_config[RPM_VALUE_ID - 1].rpm_lights_enabled = is_checked;
    
    // If disabled, hide all RPM lights circles
    if (!is_checked) {
        for (int i = 0; i < 8; i++) {
            if (rpm_lights_circles[i] && lv_obj_is_valid(rpm_lights_circles[i])) {
                lv_obj_add_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        // If enabled, create RPM lights circles if they don't exist
        if (rpm_lights_circles[0] == NULL) {
            lv_obj_t * current_screen = lv_scr_act();
            create_rpm_lights_circles(current_screen);
        }
        
        // Update RPM lights based on current RPM value
        update_rpm_lights(current_canbus_rpm);
    }
}

void rpm_gradient_switch_event_cb(lv_event_t * e) {
    lv_obj_t * switch_obj = lv_event_get_target(e);
    bool is_checked = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
    
    // Update configuration
    values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled = is_checked;
    
    // Save configuration to NVS
    save_values_config_to_nvs();
    
    // Update the RPM bar gauge gradient immediately
    if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
        if (is_checked) {
            // Enable gradient
            lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        } else {
            // Disable gradient - set gradient color to same as main color for solid appearance
            lv_color_t current_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
            lv_obj_set_style_bg_grad_color(rpm_bar_gauge, current_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        }
    }
}

void bar_low_color_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    switch(selected) {
        case 0: values_config[value_id - 1].bar_low_color = lv_color_hex(0x19439a); break; // Blue
        case 1: values_config[value_id - 1].bar_low_color = lv_color_hex(0xFF0000); break; // Red
        case 2: values_config[value_id - 1].bar_low_color = lv_color_hex(0x38FF00); break; // Green
        case 3: values_config[value_id - 1].bar_low_color = lv_color_hex(0xFFFF00); break; // Yellow
        case 4: values_config[value_id - 1].bar_low_color = lv_color_hex(0xFF7F00); break; // Orange
        case 5: values_config[value_id - 1].bar_low_color = lv_color_hex(0x8000FF); break; // Purple
        case 6: values_config[value_id - 1].bar_low_color = lv_color_hex(0x00FFFF); break; // Cyan
        case 7: values_config[value_id - 1].bar_low_color = lv_color_hex(0xFF00FF); break; // Magenta
        case 8: // Custom color - open color wheel popup
            create_bar_low_color_wheel_popup(value_id);
            return; // Don't update color yet, wait for color wheel selection
    }
}

void bar_high_color_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    switch(selected) {
        case 0: values_config[value_id - 1].bar_high_color = lv_color_hex(0x19439a); break; // Blue
        case 1: values_config[value_id - 1].bar_high_color = lv_color_hex(0xFF0000); break; // Red
        case 2: values_config[value_id - 1].bar_high_color = lv_color_hex(0x38FF00); break; // Green
        case 3: values_config[value_id - 1].bar_high_color = lv_color_hex(0xFFFF00); break; // Yellow
        case 4: values_config[value_id - 1].bar_high_color = lv_color_hex(0xFF7F00); break; // Orange
        case 5: values_config[value_id - 1].bar_high_color = lv_color_hex(0x8000FF); break; // Purple
        case 6: values_config[value_id - 1].bar_high_color = lv_color_hex(0x00FFFF); break; // Cyan
        case 7: values_config[value_id - 1].bar_high_color = lv_color_hex(0xFF00FF); break; // Magenta
        case 8: // Custom color - open color wheel popup
            create_bar_high_color_wheel_popup(value_id);
            return; // Don't update color yet, wait for color wheel selection
    }
}

void bar_in_range_color_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    switch(selected) {
        case 0: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0x19439a); break; // Blue
        case 1: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0xFF0000); break; // Red
        case 2: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0x38FF00); break; // Green
        case 3: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0xFFFF00); break; // Yellow
        case 4: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0xFF7F00); break; // Orange
        case 5: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0x8000FF); break; // Purple
        case 6: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0x00FFFF); break; // Cyan
        case 7: values_config[value_id - 1].bar_in_range_color = lv_color_hex(0xFF00FF); break; // Magenta
        case 8: // Custom color - open color wheel popup
            create_bar_in_range_color_wheel_popup(value_id);
            return; // Don't update color yet, wait for color wheel selection
    }
}

// Limiter circles color update function removed - only bar flash effect is supported

static void update_rpm_lights(int rpm_value) {
    // Only update if RPM lights are enabled and circles exist and not in limiter demo mode
    if (!values_config[RPM_VALUE_ID - 1].rpm_lights_enabled || rpm_lights_circles[0] == NULL || limiter_demo_active) {
        return;
    }
    
    extern int rpm_gauge_max;
    if (rpm_gauge_max <= 0) return; // Avoid division by zero
    
    // Calculate which zone we're in (0-4)
    float rpm_percentage = (float)rpm_value / (float)rpm_gauge_max;
    if (rpm_percentage < 0) rpm_percentage = 0;
    if (rpm_percentage > 1) rpm_percentage = 1;
    
    int zone = (int)(rpm_percentage * 5);
    if (zone > 4) zone = 4;
    
    // Circle order: [7,0] [6,1] [5,2] [4,3] (outermost to innermost pairs)
    int circle_pairs[4][2] = {
        {7, 0}, // Outermost pair
        {6, 1}, // Second pair  
        {5, 2}, // Third pair
        {4, 3}  // Innermost pair
    };
    
    // All lights use the RPM bar color
    lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
    
            // Update all circles
        for (int pair = 0; pair < 4; pair++) {
            bool should_show = (zone > pair); // Show if we've passed this zone
            lv_color_t color = rpm_color; // All lights use RPM color
            
            for (int j = 0; j < 2; j++) {
                int circle_idx = circle_pairs[pair][j];
                if (rpm_lights_circles[circle_idx] && lv_obj_is_valid(rpm_lights_circles[circle_idx])) {
                    if (should_show) {
                        lv_obj_set_style_bg_color(rpm_lights_circles[circle_idx], color, LV_PART_MAIN | LV_STATE_DEFAULT);
                        lv_obj_clear_flag(rpm_lights_circles[circle_idx], LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(rpm_lights_circles[circle_idx], LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
        }
}

void create_rpm_lights_circles(lv_obj_t * parent) {
    // Use the same positions as warning circles but these are for RPM Lights (background layer)
    const struct {
        int16_t x;
        int16_t y;
    } rpm_lights_positions[] = {
        {-352, -148},  // Position 1
        {-292, -148},  // Position 2
        {-232, -148},  // Position 3
        {-172, -148},  // Position 4
        {172, -148},   // Position 5
        {232, -148},   // Position 6
        {292, -148},   // Position 7
        {352, -148}    // Position 8
    };
    
    for (int i = 0; i < 8; i++) {
        rpm_lights_circles[i] = lv_obj_create(parent);
        lv_obj_set_width(rpm_lights_circles[i], 15);
        lv_obj_set_height(rpm_lights_circles[i], 15);
        lv_obj_set_x(rpm_lights_circles[i], rpm_lights_positions[i].x);
        lv_obj_set_y(rpm_lights_circles[i], rpm_lights_positions[i].y);
        lv_obj_set_align(rpm_lights_circles[i], LV_ALIGN_CENTER);
        lv_obj_clear_flag(rpm_lights_circles[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(rpm_lights_circles[i], 100, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(rpm_lights_circles[i], values_config[RPM_VALUE_ID - 1].rpm_bar_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(rpm_lights_circles[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(rpm_lights_circles[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        // Initially hidden
        lv_obj_add_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Limiter circles creation function removed - only bar flash effect is supported

static void limiter_demo_timeout_cb(lv_timer_t* timer) {
    stop_limiter_effect_demo();
}

static void limiter_flash_cb(lv_timer_t* timer) {
    // Safety check: if demo is no longer active, stop the timer
    if (!limiter_demo_active) {
        return;
    }
    
    limiter_flash_state = !limiter_flash_state;
    
    // Handle flash effects (type 2 = Bar only, type 3 = Bar & Circles, type 4 = Circles only)
    if (current_effect_type == 2 || current_effect_type == 3 || current_effect_type == 4) { // Flash effects
        if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
            // Keep RPM bar at max value during demo, just like real limiter effect
            extern int rpm_gauge_max;
            // Map RPM to extended bar range to properly fill the extended bar width
            const float bar_extension_ratio = 782.5f / 765.0f;
            int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);
            int32_t scaled_rpm = (rpm_gauge_max * extended_rpm_max) / rpm_gauge_max;
            lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
            
            if (limiter_flash_state) {
                // Flash RPM bar and panel for Bar Flash (type 2) and Bar & Circles Flash (type 3)
                if (current_effect_type == 2 || current_effect_type == 3) {
                    // Flash RPM bar to limiter color
                    lv_obj_set_style_bg_color(rpm_bar_gauge, values_config[RPM_VALUE_ID - 1].rpm_limiter_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    // Maintain gradient during flash if enabled
                    if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
                        lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT);
                        lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                        lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    } else {
                        lv_obj_set_style_bg_grad_color(rpm_bar_gauge, values_config[RPM_VALUE_ID - 1].rpm_limiter_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                        lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    }
                    // Flash Panel 9 (left side) to limiter color
                    if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
                        lv_obj_set_style_bg_color(ui_Panel9, values_config[RPM_VALUE_ID - 1].rpm_limiter_color, LV_PART_MAIN | LV_STATE_DEFAULT);
                    }
                }
                
                // Flash circles for Bar & Circles Flash (type 3) and Circles Flash (type 4)
                if (current_effect_type == 3 || current_effect_type == 4) {
                    // Ultra-fast batch update for perfect circle synchronization
                    batch_update_rpm_circles_color(values_config[RPM_VALUE_ID - 1].rpm_limiter_color);
                }
            } else {
                // Restore RPM bar and panel for Bar Flash (type 2) and Bar & Circles Flash (type 3)
                if (current_effect_type == 2 || current_effect_type == 3) {
                    // Restore RPM bar to original color (but keep at max value)
                    lv_obj_set_style_bg_color(rpm_bar_gauge, original_rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    // Maintain gradient during restore if enabled
                    if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
                        lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT);
                        lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                        lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    } else {
                        lv_obj_set_style_bg_grad_color(rpm_bar_gauge, original_rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                        lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    }
                    // Restore Panel 9 to original color
                    if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
                        lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color, LV_PART_MAIN | LV_STATE_DEFAULT);
                    }
                }
                
                // Restore circles for Bar & Circles Flash (type 3) and Circles Flash (type 4)
                if (current_effect_type == 3 || current_effect_type == 4) {
                    // Ultra-fast batch restore for perfect circle synchronization
                    batch_update_rpm_circles_color(original_rpm_color);
                }
            }
        }
    }
}

// Limiter circles clear function removed - only bar flash effect is supported

static void clear_rpm_lights_circles(void) {
    // Clear the RPM lights circles array when RPM gauge is recreated
    for (int i = 0; i < 8; i++) {
        rpm_lights_circles[i] = NULL;
    }
}

// Ultra-fast batch update function for perfect circle synchronization
static void batch_update_rpm_circles_color(lv_color_t color) {
    if (!values_config[RPM_VALUE_ID - 1].rpm_lights_enabled || !rpm_lights_circles[0]) {
        return; // Early exit if RPM lights are disabled or not initialized
    }
    
    // Single function call to update all circles with the same color for perfect timing
    for (int i = 0; i < 8; i++) {
        if (rpm_lights_circles[i] && lv_obj_is_valid(rpm_lights_circles[i]) && 
            !lv_obj_has_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_set_style_bg_color(rpm_lights_circles[i], color, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void update_menu_rpm_value_text(int rpm_value) {
    // Update the RPM value text in menu screen during demos
    if (menu_rpm_value_label && lv_obj_is_valid(menu_rpm_value_label)) {
        // Apply same 102.3% scaling to the actual RPM value for consistency
        int display_rpm_value = (int)((float)rpm_value * 1.0229f);
        char rpm_text[16];
        snprintf(rpm_text, sizeof(rpm_text), "%d", display_rpm_value);
        lv_label_set_text(menu_rpm_value_label, rpm_text);
    }
}





void start_limiter_effect_demo(uint8_t effect_type) {
    // Stop any existing demo
    stop_limiter_effect_demo();
    

    
    if (effect_type == 0) return; // None selected
    
    // Save current RPM value before starting demo
    saved_rpm_before_demo = current_canbus_rpm;
    
    limiter_demo_active = true;
    limiter_flash_state = false;
    current_effect_type = effect_type;
    
    // Save original RPM color for effects
    original_rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
    
    // Immediately set panels to RPM color to avoid initial white flash
    if (ui_Panel9) {
        lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    // Panel 10 stays white always - removed from demo effects
    
    // Set RPM to max for demo
    if (rpm_bar_gauge) {
        extern int rpm_gauge_max;
        // Map RPM to extended bar range to properly fill the extended bar width
        const float bar_extension_ratio = 782.5f / 765.0f;
        int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);
        int32_t scaled_rpm = (rpm_gauge_max * extended_rpm_max) / rpm_gauge_max;
        lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
        // Update menu RPM value text to show max RPM
        update_menu_rpm_value_text(rpm_gauge_max);
    }
    
    // Flash effects supported (type 2 = Bar only, type 3 = Bar & Circles, type 4 = Circles only)
    
    // Create LVGL timers for perfect synchronization with real limiter
    limiter_flash_timer = lv_timer_create(limiter_flash_cb, 100, NULL); // 100ms flash rate - same as real limiter
    limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000, NULL); // 1 second timeout
    lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
}

void stop_limiter_effect_demo(void) {
    if (!limiter_demo_active) return;
    
    limiter_demo_active = false;
    limiter_flash_state = false;  // Reset flash state
    
    // Stop and delete LVGL timers
    if (limiter_demo_timer) {
        lv_timer_del(limiter_demo_timer);
        limiter_demo_timer = NULL;
    }
    if (limiter_flash_timer) {
        lv_timer_del(limiter_flash_timer);
        limiter_flash_timer = NULL;
    }
    
    // Limiter circles hiding removed - only bar flash effect is supported

    // Restore original RPM bar color and side panels
    if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
        lv_obj_set_style_bg_color(rpm_bar_gauge, original_rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        // Restore gradient if enabled
        if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
            lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_bg_grad_color(rpm_bar_gauge, original_rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        }
    }
    // Restore Panel 9 to original RPM color
    if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
        lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    // Restore RPM value to saved value (either CAN bus value or 0)
    if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
        // Map RPM to extended bar range to properly fill the extended bar width
        const float bar_extension_ratio = 782.5f / 765.0f;
        int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);
        int32_t scaled_rpm = (saved_rpm_before_demo * extended_rpm_max) / rpm_gauge_max;
        lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
    }
    
    // Restore menu RPM value text to saved value
    update_menu_rpm_value_text(saved_rpm_before_demo);
    
    // If RPM lights were enabled, update them for the restored RPM value
    if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
        update_rpm_lights(saved_rpm_before_demo);
    }
}

// Real limiter effect implementation (triggered by actual RPM)
static lv_timer_t * real_limiter_flash_timer = NULL;
static bool real_limiter_flash_state = false;
static uint8_t real_limiter_effect_type = 0;

static void real_limiter_flash_cb(lv_timer_t * timer) {
    // Toggle flash state
    real_limiter_flash_state = !real_limiter_flash_state;
    
    // Get current colors
    lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
    lv_color_t limiter_color = values_config[RPM_VALUE_ID - 1].rpm_limiter_color;
    
    // Handle flash effects (type 2 = Bar only, type 3 = Bar & Circles, type 4 = Circles only)
    
    // Flash RPM bar and panel for Bar Flash (type 2) and Bar & Circles Flash (type 3)
    if (real_limiter_effect_type == 2 || real_limiter_effect_type == 3) {
        if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
            lv_color_t current_color = real_limiter_flash_state ? limiter_color : rpm_color;
            lv_obj_set_style_bg_color(rpm_bar_gauge, current_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            // Maintain gradient during real limiter flash if enabled
            if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
                lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            } else {
                lv_obj_set_style_bg_grad_color(rpm_bar_gauge, current_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            }
        }
        if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
            lv_obj_set_style_bg_color(ui_Panel9, 
                real_limiter_flash_state ? limiter_color : rpm_color, 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    
    // Flash circles for Bar & Circles Flash (type 3) and Circles Flash (type 4)
    if (real_limiter_effect_type == 3 || real_limiter_effect_type == 4) {
        // Ultra-fast batch update for perfect circle synchronization
        lv_color_t circle_color = real_limiter_flash_state ? limiter_color : rpm_color;
        batch_update_rpm_circles_color(circle_color);
    }
}

static void start_real_limiter_effect(uint8_t effect_type) {
    // Don't start if already active or no effect selected
    if (real_limiter_active || effect_type == 0) return;
    
    // Additional check: if timer already exists, we're already running
    if (real_limiter_flash_timer != NULL) return;
    
    real_limiter_active = true;
    real_limiter_flash_state = false;
    real_limiter_effect_type = effect_type;
    
    // Flash effects supported (type 2 = Bar only, type 3 = Bar & Circles, type 4 = Circles only)
    
    // Create LVGL timer instead of ESP timer for better coordination
    real_limiter_flash_timer = lv_timer_create(real_limiter_flash_cb, 100, NULL); // 100ms flash rate
}

static void stop_real_limiter_effect(void) {
    if (!real_limiter_active) return;
    
    real_limiter_active = false;
    
    // Stop and delete LVGL timer
    if (real_limiter_flash_timer) {
        lv_timer_del(real_limiter_flash_timer);
        real_limiter_flash_timer = NULL;
    }
    
    // Limiter circles hiding removed - only bar flash effect is supported
    
    // Restore original colors and RPM bar value
    lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
    
    if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
        lv_obj_set_style_bg_color(rpm_bar_gauge, rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        // Restore gradient if enabled
        if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
            lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_bg_grad_color(rpm_bar_gauge, rpm_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        }
        // Restore the bar to show the current actual CAN bus RPM value
        // Map RPM to extended bar range to properly fill the extended bar width
        extern int rpm_gauge_max;
        const float bar_extension_ratio = 782.5f / 765.0f;
        int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);
        int32_t scaled_rpm = (current_canbus_rpm * extended_rpm_max) / rpm_gauge_max;
        lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
    }
    if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
        lv_obj_set_style_bg_color(ui_Panel9, rpm_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

// Global variables for color wheel popup
static lv_obj_t * color_wheel_popup = NULL;
static lv_obj_t * color_wheel = NULL;
static lv_color_t selected_custom_color;

// Color wheel popup event callbacks
static void color_wheel_ok_event_cb(lv_event_t * e) {
    // Apply the selected color from the color wheel
    // But don't update if real limiter effect is active to avoid conflicts
    if (!real_limiter_active) {
        new_rpm_color = selected_custom_color;
        rpm_color_needs_update = true;
    }
    values_config[RPM_VALUE_ID - 1].rpm_bar_color = selected_custom_color;
    
    // Close the popup
    if (color_wheel_popup) {
        lv_obj_del(color_wheel_popup);
        color_wheel_popup = NULL;
        color_wheel = NULL;
    }
}

static void color_wheel_cancel_event_cb(lv_event_t * e) {
    // Just close the popup without applying changes
    if (color_wheel_popup) {
        lv_obj_del(color_wheel_popup);
        color_wheel_popup = NULL;
        color_wheel = NULL;
    }
}

static void color_wheel_value_changed_cb(lv_event_t * e) {
    // Update the selected color as user moves the color wheel
    lv_obj_t * colorwheel = lv_event_get_target(e);
    selected_custom_color = lv_colorwheel_get_rgb(colorwheel);
    
    // Show live preview by updating the RPM bar immediately
    // But don't update if real limiter effect is active to avoid conflicts
    if (!real_limiter_active) {
        new_rpm_color = selected_custom_color;
        rpm_color_needs_update = true;
    }
}

void create_rpm_color_wheel_popup(void) {
    // Don't create multiple popups
    if (color_wheel_popup) return;
    
    // Create popup background
    color_wheel_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(color_wheel_popup, 400, 350);
    lv_obj_center(color_wheel_popup);
    lv_obj_set_style_bg_color(color_wheel_popup, lv_color_hex(0x2E2F2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(color_wheel_popup, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(color_wheel_popup, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(color_wheel_popup, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(color_wheel_popup, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(color_wheel_popup, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(color_wheel_popup, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Title label
    lv_obj_t * title_label = lv_label_create(color_wheel_popup);
    lv_label_set_text(title_label, "Select Custom RPM Colour");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // Create color wheel
    color_wheel = lv_colorwheel_create(color_wheel_popup, true);
    lv_obj_set_size(color_wheel, 200, 200);
    lv_obj_align(color_wheel, LV_ALIGN_CENTER, 0, -10);
    
    // Set initial color to current RPM color
    lv_color_t current_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
    lv_colorwheel_set_rgb(color_wheel, current_color);
    selected_custom_color = current_color;
    
    // Add color wheel change event
    lv_obj_add_event_cb(color_wheel, color_wheel_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // OK button
    lv_obj_t * ok_btn = lv_btn_create(color_wheel_popup);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ok_label);
    
    lv_obj_add_event_cb(ok_btn, color_wheel_ok_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Cancel button
    lv_obj_t * cancel_btn = lv_btn_create(color_wheel_popup);
    lv_obj_set_size(cancel_btn, 80, 35);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xF44336), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(cancel_label);
    
    lv_obj_add_event_cb(cancel_btn, color_wheel_cancel_event_cb, LV_EVENT_CLICKED, NULL);
}

// Global variables for limiter color wheel popup
static lv_obj_t * limiter_color_wheel_popup = NULL;
static lv_obj_t * limiter_color_wheel = NULL;
static lv_color_t selected_limiter_custom_color;

// Global variables for bar color wheel popups
static lv_obj_t * bar_low_color_wheel_popup = NULL;
static lv_obj_t * bar_low_color_wheel = NULL;
static lv_color_t selected_bar_low_custom_color;
static uint8_t bar_low_color_value_id = 0;

static lv_obj_t * bar_high_color_wheel_popup = NULL;
static lv_obj_t * bar_high_color_wheel = NULL;
static lv_color_t selected_bar_high_custom_color;
static uint8_t bar_high_color_value_id = 0;

static lv_obj_t * bar_in_range_color_wheel_popup = NULL;
static lv_obj_t * bar_in_range_color_wheel = NULL;
static lv_color_t selected_bar_in_range_custom_color;
static uint8_t bar_in_range_color_value_id = 0;

// Limiter color wheel popup event callbacks
static void limiter_color_wheel_ok_event_cb(lv_event_t * e) {
    // Apply the selected color from the color wheel
    values_config[RPM_VALUE_ID - 1].rpm_limiter_color = selected_limiter_custom_color;
    
    // Limiter circles color update removed - only bar flash effect is supported
    
    // Close the popup
    if (limiter_color_wheel_popup) {
        lv_obj_del(limiter_color_wheel_popup);
        limiter_color_wheel_popup = NULL;
        limiter_color_wheel = NULL;
    }
}

static void limiter_color_wheel_cancel_event_cb(lv_event_t * e) {
    // Just close the popup without applying changes
    if (limiter_color_wheel_popup) {
        lv_obj_del(limiter_color_wheel_popup);
        limiter_color_wheel_popup = NULL;
        limiter_color_wheel = NULL;
    }
}

static void limiter_color_wheel_value_changed_cb(lv_event_t * e) {
    // Update the selected color as user moves the color wheel
    lv_obj_t * colorwheel = lv_event_get_target(e);
    selected_limiter_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

// Bar color wheel popup event callbacks
static void bar_low_color_wheel_ok_event_cb(lv_event_t * e) {
    // Apply the selected color from the color wheel
    values_config[bar_low_color_value_id - 1].bar_low_color = selected_bar_low_custom_color;
    
    // Close the popup
    if (bar_low_color_wheel_popup) {
        lv_obj_del(bar_low_color_wheel_popup);
        bar_low_color_wheel_popup = NULL;
        bar_low_color_wheel = NULL;
    }
}

static void bar_low_color_wheel_cancel_event_cb(lv_event_t * e) {
    // Just close the popup without applying changes
    if (bar_low_color_wheel_popup) {
        lv_obj_del(bar_low_color_wheel_popup);
        bar_low_color_wheel_popup = NULL;
        bar_low_color_wheel = NULL;
    }
}

static void bar_low_color_wheel_value_changed_cb(lv_event_t * e) {
    // Update the selected color as user moves the color wheel
    lv_obj_t * colorwheel = lv_event_get_target(e);
    selected_bar_low_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

static void bar_high_color_wheel_ok_event_cb(lv_event_t * e) {
    // Apply the selected color from the color wheel
    values_config[bar_high_color_value_id - 1].bar_high_color = selected_bar_high_custom_color;
    
    // Close the popup
    if (bar_high_color_wheel_popup) {
        lv_obj_del(bar_high_color_wheel_popup);
        bar_high_color_wheel_popup = NULL;
        bar_high_color_wheel = NULL;
    }
}

static void bar_high_color_wheel_cancel_event_cb(lv_event_t * e) {
    // Just close the popup without applying changes
    if (bar_high_color_wheel_popup) {
        lv_obj_del(bar_high_color_wheel_popup);
        bar_high_color_wheel_popup = NULL;
        bar_high_color_wheel = NULL;
    }
}

static void bar_high_color_wheel_value_changed_cb(lv_event_t * e) {
    // Update the selected color as user moves the color wheel
    lv_obj_t * colorwheel = lv_event_get_target(e);
    selected_bar_high_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

static void bar_in_range_color_wheel_ok_event_cb(lv_event_t * e) {
    // Apply the selected color from the color wheel
    values_config[bar_in_range_color_value_id - 1].bar_in_range_color = selected_bar_in_range_custom_color;
    
    // Close the popup
    if (bar_in_range_color_wheel_popup) {
        lv_obj_del(bar_in_range_color_wheel_popup);
        bar_in_range_color_wheel_popup = NULL;
        bar_in_range_color_wheel = NULL;
    }
}

static void bar_in_range_color_wheel_cancel_event_cb(lv_event_t * e) {
    // Just close the popup without applying changes
    if (bar_in_range_color_wheel_popup) {
        lv_obj_del(bar_in_range_color_wheel_popup);
        bar_in_range_color_wheel_popup = NULL;
        bar_in_range_color_wheel = NULL;
    }
}

static void bar_in_range_color_wheel_value_changed_cb(lv_event_t * e) {
    // Update the selected color as user moves the color wheel
    lv_obj_t * colorwheel = lv_event_get_target(e);
    selected_bar_in_range_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

void create_limiter_color_wheel_popup(void) {
    // Don't create multiple popups
    if (limiter_color_wheel_popup) return;
    
    // Create popup background
    limiter_color_wheel_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(limiter_color_wheel_popup, 400, 350);
    lv_obj_center(limiter_color_wheel_popup);
    lv_obj_set_style_bg_color(limiter_color_wheel_popup, lv_color_hex(0x2E2F2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(limiter_color_wheel_popup, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(limiter_color_wheel_popup, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(limiter_color_wheel_popup, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(limiter_color_wheel_popup, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(limiter_color_wheel_popup, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(limiter_color_wheel_popup, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Title label
    lv_obj_t * title_label = lv_label_create(limiter_color_wheel_popup);
    lv_label_set_text(title_label, "Select Custom Limiter Colour");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // Create color wheel
    limiter_color_wheel = lv_colorwheel_create(limiter_color_wheel_popup, true);
    lv_obj_set_size(limiter_color_wheel, 200, 200);
    lv_obj_align(limiter_color_wheel, LV_ALIGN_CENTER, 0, -10);
    
    // Set initial color to current limiter color
    lv_color_t current_color = values_config[RPM_VALUE_ID - 1].rpm_limiter_color;
    lv_colorwheel_set_rgb(limiter_color_wheel, current_color);
    selected_limiter_custom_color = current_color;
    
    // Add color wheel change event
    lv_obj_add_event_cb(limiter_color_wheel, limiter_color_wheel_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // OK button
    lv_obj_t * ok_btn = lv_btn_create(limiter_color_wheel_popup);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ok_label);
    
    lv_obj_add_event_cb(ok_btn, limiter_color_wheel_ok_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Cancel button
    lv_obj_t * cancel_btn = lv_btn_create(limiter_color_wheel_popup);
    lv_obj_set_size(cancel_btn, 80, 35);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xF44336), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(cancel_label);
    
    lv_obj_add_event_cb(cancel_btn, limiter_color_wheel_cancel_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_bar_low_color_wheel_popup(uint8_t value_id) {
    // Don't create multiple popups
    if (bar_low_color_wheel_popup) return;
    
    // Store the value ID for the callback
    bar_low_color_value_id = value_id;
    
    // Create popup background
    bar_low_color_wheel_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bar_low_color_wheel_popup, 400, 350);
    lv_obj_center(bar_low_color_wheel_popup);
    lv_obj_set_style_bg_color(bar_low_color_wheel_popup, lv_color_hex(0x2E2F2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(bar_low_color_wheel_popup, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar_low_color_wheel_popup, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar_low_color_wheel_popup, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(bar_low_color_wheel_popup, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(bar_low_color_wheel_popup, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(bar_low_color_wheel_popup, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Title label
    lv_obj_t * title_label = lv_label_create(bar_low_color_wheel_popup);
    lv_label_set_text(title_label, "Select Custom Bar Low Colour");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // Create color wheel
    bar_low_color_wheel = lv_colorwheel_create(bar_low_color_wheel_popup, true);
    lv_obj_set_size(bar_low_color_wheel, 200, 200);
    lv_obj_align(bar_low_color_wheel, LV_ALIGN_CENTER, 0, -10);
    
    // Set initial color to current bar low color
    lv_color_t current_color = values_config[value_id - 1].bar_low_color;
    lv_colorwheel_set_rgb(bar_low_color_wheel, current_color);
    selected_bar_low_custom_color = current_color;
    
    // Add color wheel change event
    lv_obj_add_event_cb(bar_low_color_wheel, bar_low_color_wheel_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // OK button
    lv_obj_t * ok_btn = lv_btn_create(bar_low_color_wheel_popup);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ok_label);
    
    lv_obj_add_event_cb(ok_btn, bar_low_color_wheel_ok_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Cancel button
    lv_obj_t * cancel_btn = lv_btn_create(bar_low_color_wheel_popup);
    lv_obj_set_size(cancel_btn, 80, 35);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xF44336), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(cancel_label);
    
    lv_obj_add_event_cb(cancel_btn, bar_low_color_wheel_cancel_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_bar_high_color_wheel_popup(uint8_t value_id) {
    // Don't create multiple popups
    if (bar_high_color_wheel_popup) return;
    
    // Store the value ID for the callback
    bar_high_color_value_id = value_id;
    
    // Create popup background
    bar_high_color_wheel_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bar_high_color_wheel_popup, 400, 350);
    lv_obj_center(bar_high_color_wheel_popup);
    lv_obj_set_style_bg_color(bar_high_color_wheel_popup, lv_color_hex(0x2E2F2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(bar_high_color_wheel_popup, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar_high_color_wheel_popup, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar_high_color_wheel_popup, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(bar_high_color_wheel_popup, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(bar_high_color_wheel_popup, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(bar_high_color_wheel_popup, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Title label
    lv_obj_t * title_label = lv_label_create(bar_high_color_wheel_popup);
    lv_label_set_text(title_label, "Select Custom Bar High Colour");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // Create color wheel
    bar_high_color_wheel = lv_colorwheel_create(bar_high_color_wheel_popup, true);
    lv_obj_set_size(bar_high_color_wheel, 200, 200);
    lv_obj_align(bar_high_color_wheel, LV_ALIGN_CENTER, 0, -10);
    
    // Set initial color to current bar high color
    lv_color_t current_color = values_config[value_id - 1].bar_high_color;
    lv_colorwheel_set_rgb(bar_high_color_wheel, current_color);
    selected_bar_high_custom_color = current_color;
    
    // Add color wheel change event
    lv_obj_add_event_cb(bar_high_color_wheel, bar_high_color_wheel_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // OK button
    lv_obj_t * ok_btn = lv_btn_create(bar_high_color_wheel_popup);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ok_label);
    
    lv_obj_add_event_cb(ok_btn, bar_high_color_wheel_ok_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Cancel button
    lv_obj_t * cancel_btn = lv_btn_create(bar_high_color_wheel_popup);
    lv_obj_set_size(cancel_btn, 80, 35);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xF44336), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(cancel_label);
    
    lv_obj_add_event_cb(cancel_btn, bar_high_color_wheel_cancel_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_bar_in_range_color_wheel_popup(uint8_t value_id) {
    // Don't create multiple popups
    if (bar_in_range_color_wheel_popup) return;
    
    // Store the value ID for the callback
    bar_in_range_color_value_id = value_id;
    
    // Create popup background
    bar_in_range_color_wheel_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bar_in_range_color_wheel_popup, 400, 350);
    lv_obj_center(bar_in_range_color_wheel_popup);
    lv_obj_set_style_bg_color(bar_in_range_color_wheel_popup, lv_color_hex(0x2E2F2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(bar_in_range_color_wheel_popup, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar_in_range_color_wheel_popup, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar_in_range_color_wheel_popup, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(bar_in_range_color_wheel_popup, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(bar_in_range_color_wheel_popup, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(bar_in_range_color_wheel_popup, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Title label
    lv_obj_t * title_label = lv_label_create(bar_in_range_color_wheel_popup);
    lv_label_set_text(title_label, "Select Custom Bar In-Range Colour");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // Create color wheel
    bar_in_range_color_wheel = lv_colorwheel_create(bar_in_range_color_wheel_popup, true);
    lv_obj_set_size(bar_in_range_color_wheel, 200, 200);
    lv_obj_align(bar_in_range_color_wheel, LV_ALIGN_CENTER, 0, -10);
    
    // Set initial color to current bar in-range color
    lv_color_t current_color = values_config[value_id - 1].bar_in_range_color;
    lv_colorwheel_set_rgb(bar_in_range_color_wheel, current_color);
    selected_bar_in_range_custom_color = current_color;
    
    // Add color wheel change event
    lv_obj_add_event_cb(bar_in_range_color_wheel, bar_in_range_color_wheel_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // OK button
    lv_obj_t * ok_btn = lv_btn_create(bar_in_range_color_wheel_popup);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ok_label);
    
    lv_obj_add_event_cb(ok_btn, bar_in_range_color_wheel_ok_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Cancel button
    lv_obj_t * cancel_btn = lv_btn_create(bar_in_range_color_wheel_popup);
    lv_obj_set_size(cancel_btn, 80, 35);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xF44336), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t * cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(cancel_label);
    
    lv_obj_add_event_cb(cancel_btn, bar_in_range_color_wheel_cancel_event_cb, LV_EVENT_CLICKED, NULL);
}

void warning_high_threshold_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * textarea = lv_event_get_target(e);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        const char * txt = lv_textarea_get_text(textarea);
        values_config[value_id - 1].warning_high_threshold = atof(txt);
        values_config[value_id - 1].warning_high_enabled = true;
    }
}

void warning_low_threshold_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * textarea = lv_event_get_target(e);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        const char * txt = lv_textarea_get_text(textarea);
        values_config[value_id - 1].warning_low_threshold = atof(txt);
        values_config[value_id - 1].warning_low_enabled = true;
    }
}

void warning_high_color_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        uint16_t selected = lv_dropdown_get_selected(dropdown);
        values_config[value_id - 1].warning_high_color = selected == 0 ? 
            lv_color_hex(0xFF0000) : lv_color_hex(0x19439a);
    }
}

void warning_low_color_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        uint16_t selected = lv_dropdown_get_selected(dropdown);
        values_config[value_id - 1].warning_low_color = selected == 0 ? 
            lv_color_hex(0xFF0000) : lv_color_hex(0x19439a);
    }
}

static void warning_longpress_cb(lv_event_t* e) {
    uint8_t warning_idx = *(uint8_t*)lv_event_get_user_data(e);
    create_warning_config_menu(warning_idx);
}

static void indicator_longpress_cb(lv_event_t* e) {
    void* user_data = lv_event_get_user_data(e);
    if (!user_data) {
        printf("Error: No user data in indicator longpress callback\n");
        return;
    }
    
    uint8_t indicator_idx = *(uint8_t*)user_data;
    printf("Indicator longpress detected for indicator %d\n", indicator_idx);
    create_indicator_config_menu(indicator_idx);
}

static void label_text_cb(lv_event_t* e) {
    lv_obj_t* textarea = lv_event_get_target(e);
    warning_save_data_t* data = (warning_save_data_t*)lv_event_get_user_data(e);
    const char* txt = lv_textarea_get_text(textarea);
    if (data->preview_objects && data->preview_objects[1]) {
        lv_label_set_text(data->preview_objects[1], txt);
    }
}

static void color_dropdown_cb(lv_event_t* e) {
    lv_obj_t* dropdown = lv_event_get_target(e);
    warning_save_data_t* data = (warning_save_data_t*)lv_event_get_user_data(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    lv_color_t color;
    switch (selected) {
        case 0: color = lv_color_hex(0x00FF00); break; // Green
        case 1: color = lv_color_hex(0x0000FF); break; // Blue
        case 2: color = lv_color_hex(0xFFA500); break; // Orange
        case 3: color = lv_color_hex(0xFF0000); break; // Red
        case 4: color = lv_color_hex(0xFFFF00); break; // Yellow
        default: color = lv_color_hex(0x00FF00); break;
    }
    if (data->preview_objects && data->preview_objects[0]) {
        lv_obj_set_style_bg_color(data->preview_objects[0], color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void save_warning_config_cb(lv_event_t* e) {
    warning_save_data_t* save_data = (warning_save_data_t*)lv_event_get_user_data(e);
    if (!save_data) {
        printf("Error: Invalid save data\n");
        return;
    }

    uint8_t warning_idx = save_data->warning_idx;
    lv_obj_t** inputs = save_data->input_objects;

    if (!inputs) {
        printf("Error: Invalid input objects\n");
        lv_mem_free(save_data);
        return;
    }

    // Get values from inputs
    const char* can_id_text = lv_textarea_get_text(inputs[0]);
    uint8_t bit_pos = lv_dropdown_get_selected(inputs[1]);
    const char* label_text = lv_textarea_get_text(inputs[3]);

    // Convert CAN ID from hex string to integer
    uint32_t can_id = 0;
    if (can_id_text && *can_id_text) {
        if (strncmp(can_id_text, "0x", 2) == 0) {
            sscanf(can_id_text + 2, "%x", &can_id);
        } else {
            sscanf(can_id_text, "%x", &can_id);
        }
    }

    // Update warning configuration
    warning_configs[warning_idx].can_id = can_id;
    warning_configs[warning_idx].bit_position = bit_pos;
    if (label_text) {
        strncpy(warning_configs[warning_idx].label, label_text, sizeof(warning_configs[warning_idx].label) - 1);
        warning_configs[warning_idx].label[sizeof(warning_configs[warning_idx].label) - 1] = '\0';
    }

    // Handle highlighted color selection
    if (inputs[4]) {
        uint8_t selected_color = lv_dropdown_get_selected(inputs[4]);
        switch (selected_color) {
            case 0: warning_configs[warning_idx].active_color = lv_color_hex(0x00FF00); break; // Green
            case 1: warning_configs[warning_idx].active_color = lv_color_hex(0x0000FF); break; // Blue
            case 2: warning_configs[warning_idx].active_color = lv_color_hex(0xFFA500); break; // Orange
            case 3: warning_configs[warning_idx].active_color = lv_color_hex(0xFF0000); break; // Red
            case 4: warning_configs[warning_idx].active_color = lv_color_hex(0xFFFF00); break; // Yellow
            default: warning_configs[warning_idx].active_color = lv_color_hex(0x00FF00); break;
        }
    }

    // Save toggle mode setting
    if (inputs[5]) {
        warning_configs[warning_idx].is_momentary = (lv_dropdown_get_selected(inputs[5]) == 1);
        warning_configs[warning_idx].current_state = false; // Reset state when changing modes
    }

    // Add callbacks for live preview updates
    lv_obj_add_event_cb(inputs[3], label_text_cb, LV_EVENT_VALUE_CHANGED, save_data);
    lv_obj_add_event_cb(inputs[4], color_dropdown_cb, LV_EVENT_VALUE_CHANGED, save_data);

    // Debug output
    printf("Warning %d configuration saved:\n", warning_idx + 1);
    printf("  CAN ID: 0x%X\n", can_id);
    printf("  Bit Position: %d\n", bit_pos);
    printf("  Label: %s\n", label_text ? label_text : "");
    printf("  Highlight Color: %06X\n", warning_configs[warning_idx].active_color.full);
    printf("  Mode: %s\n", warning_configs[warning_idx].is_momentary ? "Momentary" : "Toggle");

    // Update the label on Screen3 dynamically
    if (warning_labels[warning_idx]) {
        lv_label_set_text(warning_labels[warning_idx], warning_configs[warning_idx].label);
    }

    // Clean up
    lv_mem_free(inputs);
    lv_mem_free(save_data);
    save_warning_configs_to_nvs();

    // Return to Screen3
    lv_scr_load(ui_Screen3);
}

// Structure to hold all input objects for the save callback
typedef struct {
    uint8_t indicator_idx;
    lv_obj_t* can_id_input;
    lv_obj_t* bit_pos_dropdown;
    lv_obj_t* toggle_mode_dropdown;
} indicator_save_data_t;

// Global variables for preview elements (to allow live updates)
static lv_obj_t* preview_indicator_config = NULL;
static lv_obj_t* preview_status_text_config = NULL;
static uint8_t preview_indicator_idx = 0;

static void save_indicator_config_cb(lv_event_t* e) {
    // Settings are now saved automatically, so this just closes the menu
    printf("Indicator configuration completed - returning to main screen\n");
    
    // Clear preview references
    preview_indicator_config = NULL;
    preview_status_text_config = NULL;
    preview_indicator_idx = 0;

    // Return to Screen3
    lv_scr_load(ui_Screen3);
}

static void back_indicator_config_cb(lv_event_t* e) {
    // Clear preview references
    preview_indicator_config = NULL;
    preview_status_text_config = NULL;
    preview_indicator_idx = 0;
    
    // Return to Screen3 without saving
    lv_scr_load(ui_Screen3);
}

// Callback for CAN ID input changes
static void indicator_can_id_changed_cb(lv_event_t* e) {
    lv_obj_t* input = lv_event_get_target(e);
    uint8_t* indicator_idx_ptr = (uint8_t*)lv_event_get_user_data(e);
    
    if (!input || !indicator_idx_ptr || *indicator_idx_ptr >= 2) return;
    
    uint8_t indicator_idx = *indicator_idx_ptr;
    const char* can_id_text = lv_textarea_get_text(input);
    
    // Convert CAN ID from hex string to integer
    uint32_t can_id = 0;
    if (can_id_text && *can_id_text) {
        if (strncmp(can_id_text, "0x", 2) == 0) {
            sscanf(can_id_text + 2, "%x", &can_id);
        } else {
            sscanf(can_id_text, "%x", &can_id);
        }
    }
    
    // Update configuration and save to NVS
    indicator_configs[indicator_idx].can_id = can_id;
    printf("Calling save_indicator_configs_to_nvs() for CAN ID change...\n");
    save_indicator_configs_to_nvs();
    
    printf("Indicator %d CAN ID updated to: 0x%X\n", indicator_idx, can_id);
}

// Callback for bit position dropdown changes
static void indicator_bit_pos_changed_cb(lv_event_t* e) {
    lv_obj_t* dropdown = lv_event_get_target(e);
    uint8_t* indicator_idx_ptr = (uint8_t*)lv_event_get_user_data(e);
    
    if (!dropdown || !indicator_idx_ptr || *indicator_idx_ptr >= 2) return;
    
    uint8_t indicator_idx = *indicator_idx_ptr;
    uint8_t bit_pos = lv_dropdown_get_selected(dropdown);
    
    // Update configuration and save to NVS
    indicator_configs[indicator_idx].bit_position = bit_pos;
    printf("Calling save_indicator_configs_to_nvs() for bit position change...\n");
    save_indicator_configs_to_nvs();
    
    printf("Indicator %d bit position updated to: %d\n", indicator_idx, bit_pos);
}

// Callback for toggle mode dropdown changes
static void indicator_toggle_mode_changed_cb(lv_event_t* e) {
    lv_obj_t* dropdown = lv_event_get_target(e);
    uint8_t* indicator_idx_ptr = (uint8_t*)lv_event_get_user_data(e);
    
    if (!dropdown || !indicator_idx_ptr || *indicator_idx_ptr >= 2) return;
    
    uint8_t indicator_idx = *indicator_idx_ptr;
    bool is_momentary = (lv_dropdown_get_selected(dropdown) == 0);
    
    // Update configuration and save to NVS
    indicator_configs[indicator_idx].is_momentary = is_momentary;
    printf("Calling save_indicator_configs_to_nvs() for toggle mode change...\n");
    save_indicator_configs_to_nvs();
    
    printf("Indicator %d toggle mode updated to: %s\n", indicator_idx, is_momentary ? "Momentary" : "Toggle");
}

static void indicator_animation_changed_cb(lv_event_t* e) {
    lv_obj_t* switch_obj = lv_event_get_target(e);
    uint8_t* indicator_idx_ptr = (uint8_t*)lv_event_get_user_data(e);
    
    if (!switch_obj || !indicator_idx_ptr || *indicator_idx_ptr >= 2) return;
    
    uint8_t indicator_idx = *indicator_idx_ptr;
    bool is_enabled = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
    
    // Update configuration and save to NVS
    indicator_configs[indicator_idx].animation_enabled = is_enabled;
    printf("Calling save_indicator_configs_to_nvs() for animation change...\n");
    save_indicator_configs_to_nvs();
    
    printf("Indicator %d animation updated to: %s\n", indicator_idx, is_enabled ? "Enabled" : "Disabled");
}

void create_indicator_config_menu(uint8_t indicator_idx) {
    printf("Creating indicator config menu for indicator %d\n", indicator_idx);
    
    // Validate indicator index
    if (indicator_idx >= 2) {
        printf("Error: Invalid indicator index %d\n", indicator_idx);
        return;
    }
    
    // Initialize common style
    init_common_style();
    
    // Create the configuration screen
    lv_obj_t* config_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(config_screen, lv_color_hex(0x0000), 0);
    lv_obj_set_style_bg_opa(config_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(config_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Create main border/background panel
    lv_obj_t* main_border = lv_obj_create(config_screen);
    lv_obj_set_width(main_border, 780); 
    lv_obj_set_height(main_border, 325);
    lv_obj_set_align(main_border, LV_ALIGN_CENTER);
    lv_obj_set_y(main_border, 67);
    lv_obj_clear_flag(main_border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(main_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(main_border, lv_color_hex(0x292C29), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(main_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(main_border, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Create input border panel
    lv_obj_t * input_border = lv_obj_create(config_screen);
    lv_obj_set_width(input_border, 275);
    lv_obj_set_height(input_border, 310);
    lv_obj_set_x(input_border, -244);
    lv_obj_set_y(input_border, 67);
    lv_obj_set_align(input_border, LV_ALIGN_CENTER);
    lv_obj_clear_flag(input_border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(input_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(input_border, lv_color_hex(0x181818), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(input_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(input_border, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Create preview panel to contain the indicator and status
    lv_obj_t* preview_panel = lv_obj_create(config_screen);
    lv_obj_set_width(preview_panel, 480);
    lv_obj_set_height(preview_panel, 220);
    lv_obj_set_x(preview_panel, 130);
    lv_obj_set_y(preview_panel, 67);
    lv_obj_set_align(preview_panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(preview_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(preview_panel, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(preview_panel, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(preview_panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(preview_panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(preview_panel, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);

    // Create preview title
    lv_obj_t* preview_title = lv_label_create(preview_panel);
    lv_label_set_text(preview_title, "Live Preview");
    lv_obj_align(preview_title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(preview_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(preview_title, &lv_font_montserrat_14, 0);

    // Create preview indicator centered in the panel
    lv_obj_t* preview_indicator = lv_img_create(preview_panel);
    lv_obj_set_width(preview_indicator, LV_SIZE_CONTENT);
    lv_obj_set_height(preview_indicator, LV_SIZE_CONTENT);
    
    // Position the preview indicator based on which indicator is being configured
    if (indicator_idx == 0) {
        // Left indicator
        lv_img_set_src(preview_indicator, &ui_img_indicator_left_png);
    } else {
        // Right indicator
        lv_img_set_src(preview_indicator, &ui_img_indicator_right_png);
    }
    
    lv_obj_align(preview_indicator, LV_ALIGN_CENTER, 0, -20);
    lv_obj_clear_flag(preview_indicator, LV_OBJ_FLAG_SCROLLABLE);
    
    // Set opacity based on current state
    if (indicator_idx < 2 && indicator_configs[indicator_idx].current_state) {
        lv_obj_set_style_opa(preview_indicator, 255, LV_PART_MAIN | LV_STATE_DEFAULT); // 100% opacity when active
    } else {
        lv_obj_set_style_opa(preview_indicator, 50, LV_PART_MAIN | LV_STATE_DEFAULT);  // 50% opacity when inactive
    }

    // Create status text below the indicator
    lv_obj_t* status_text = lv_label_create(preview_panel);
    lv_label_set_text_fmt(status_text, "%s INDICATOR\n%s", 
        indicator_idx == 0 ? "LEFT" : "RIGHT",
        (indicator_idx < 2 && indicator_configs[indicator_idx].current_state) ? "ACTIVE" : "INACTIVE");
    lv_obj_align(status_text, LV_ALIGN_CENTER, 0, 30);
    
    // Set text color based on state
    if (indicator_idx < 2 && indicator_configs[indicator_idx].current_state) {
        lv_obj_set_style_text_color(status_text, lv_color_hex(0x00FF00), 0); // Green when active
    } else {
        lv_obj_set_style_text_color(status_text, lv_color_hex(0xCCCCCC), 0); // Gray when inactive
    }
    
    lv_obj_set_style_text_align(status_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status_text, &lv_font_montserrat_12, 0);

    // Store references for live preview updates
    preview_indicator_config = preview_indicator;
    preview_status_text_config = status_text;
    preview_indicator_idx = indicator_idx;

    // Create the keyboard
    keyboard = lv_keyboard_create(config_screen);
    lv_obj_set_parent(keyboard, lv_layer_top());
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY, NULL);

    // Create title
    lv_obj_t* title = lv_label_create(config_screen);
    lv_label_set_text_fmt(title, "%s Indicator Configuration", indicator_idx == 0 ? "Left" : "Right");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Create a container for inputs
    lv_obj_t* inputs_container = lv_obj_create(config_screen);
    lv_obj_set_size(inputs_container, 800, 480);
    lv_obj_align(inputs_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(inputs_container, 0, 0);
    lv_obj_set_style_border_opa(inputs_container, 0, 0);
    lv_obj_clear_flag(inputs_container, LV_OBJ_FLAG_SCROLLABLE);

    // Create input section title
    lv_obj_t* input_title = lv_label_create(inputs_container);
    lv_label_set_text(input_title, "Configuration Settings");
    lv_obj_align(input_title, LV_ALIGN_CENTER, -244, -90);
    lv_obj_set_style_text_color(input_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(input_title, &lv_font_montserrat_14, 0);

    // CAN ID input
    lv_obj_t* can_id_label = lv_label_create(inputs_container);
    lv_label_set_text(can_id_label, "CAN ID (hex):");
    lv_obj_set_width(can_id_label, 110);
    lv_obj_set_style_text_align(can_id_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(can_id_label, LV_ALIGN_CENTER, -312, -47);
    lv_obj_set_style_text_color(can_id_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(can_id_label, &lv_font_montserrat_12, 0);

    // CAN ID "0x" prefix
    lv_obj_t* can_id_0x = lv_label_create(inputs_container);
    lv_label_set_text(can_id_0x, "0x");
    lv_obj_set_width(can_id_0x, 20);
    lv_obj_set_style_text_color(can_id_0x, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(can_id_0x, LV_ALIGN_CENTER, -200, -47);
    lv_obj_set_style_text_align(can_id_0x, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(can_id_0x, &lv_font_montserrat_12, 0);

    lv_obj_t* can_id_input = lv_textarea_create(inputs_container);
    lv_obj_add_style(can_id_input, get_common_style(), LV_PART_MAIN);
    lv_textarea_set_one_line(can_id_input, true);
    lv_obj_set_width(can_id_input, 120);
    lv_obj_align(can_id_input, LV_ALIGN_CENTER, -180, -47);
    lv_obj_add_event_cb(can_id_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
    
    // Add real-time saving callback for CAN ID - use both VALUE_CHANGED and DEFOCUSED events
    uint8_t* can_id_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
    if (can_id_idx_ptr) {
        *can_id_idx_ptr = indicator_idx;
        lv_obj_add_event_cb(can_id_input, indicator_can_id_changed_cb, LV_EVENT_VALUE_CHANGED, can_id_idx_ptr);
        lv_obj_add_event_cb(can_id_input, indicator_can_id_changed_cb, LV_EVENT_DEFOCUSED, can_id_idx_ptr);
        printf("Added CAN ID callbacks for indicator %d\n", indicator_idx);
    }
    
    // Set placeholder and initial value
    char can_id_text[32];
    if (indicator_idx < 2) {
        snprintf(can_id_text, sizeof(can_id_text), "%X", indicator_configs[indicator_idx].can_id);
    } else {
        snprintf(can_id_text, sizeof(can_id_text), "0");
    }
    lv_textarea_set_text(can_id_input, can_id_text);
    lv_textarea_set_placeholder_text(can_id_input, "Enter CAN ID");

    // Bit position dropdown
    lv_obj_t* bit_pos_label = lv_label_create(inputs_container);
    lv_label_set_text(bit_pos_label, "Bit Position:");
    lv_obj_set_width(bit_pos_label, 110);
    lv_obj_set_style_text_align(bit_pos_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(bit_pos_label, LV_ALIGN_CENTER, -312, -7);
    lv_obj_set_style_text_color(bit_pos_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(bit_pos_label, &lv_font_montserrat_12, 0);

    lv_obj_t* bit_pos_dropdown = lv_dropdown_create(inputs_container);
    lv_obj_add_style(bit_pos_dropdown, get_common_style(), LV_PART_MAIN);
    lv_dropdown_set_options(bit_pos_dropdown, 
        "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
        "16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
        "32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
        "48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63");
    lv_obj_set_width(bit_pos_dropdown, 120);
    lv_obj_align(bit_pos_dropdown, LV_ALIGN_CENTER, -180, -7);
    
    // Add real-time saving callback for bit position
    uint8_t* bit_pos_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
    if (bit_pos_idx_ptr) {
        *bit_pos_idx_ptr = indicator_idx;
        lv_obj_add_event_cb(bit_pos_dropdown, indicator_bit_pos_changed_cb, LV_EVENT_VALUE_CHANGED, bit_pos_idx_ptr);
        printf("Added bit position callback for indicator %d\n", indicator_idx);
    }
    
    if (indicator_idx < 2) {
        lv_dropdown_set_selected(bit_pos_dropdown, indicator_configs[indicator_idx].bit_position);
    } else {
        lv_dropdown_set_selected(bit_pos_dropdown, 0);
    }

    // Toggle mode dropdown
    lv_obj_t* toggle_mode_label = lv_label_create(inputs_container);
    lv_label_set_text(toggle_mode_label, "Toggle Mode:");
    lv_obj_set_width(toggle_mode_label, 110);
    lv_obj_set_style_text_align(toggle_mode_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(toggle_mode_label, LV_ALIGN_CENTER, -312, 33);
    lv_obj_set_style_text_color(toggle_mode_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(toggle_mode_label, &lv_font_montserrat_12, 0);

    lv_obj_t* toggle_mode_dropdown = lv_dropdown_create(inputs_container);
    lv_obj_add_style(toggle_mode_dropdown, get_common_style(), LV_PART_MAIN);
    lv_dropdown_set_options(toggle_mode_dropdown, "Momentary\nToggle");
    lv_obj_set_width(toggle_mode_dropdown, 120);
    lv_obj_align(toggle_mode_dropdown, LV_ALIGN_CENTER, -180, 33);
    
    // Add real-time saving callback for toggle mode
    uint8_t* toggle_mode_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
    if (toggle_mode_idx_ptr) {
        *toggle_mode_idx_ptr = indicator_idx;
        lv_obj_add_event_cb(toggle_mode_dropdown, indicator_toggle_mode_changed_cb, LV_EVENT_VALUE_CHANGED, toggle_mode_idx_ptr);
        printf("Added toggle mode callback for indicator %d\n", indicator_idx);
    }
    
    if (indicator_idx < 2) {
        lv_dropdown_set_selected(toggle_mode_dropdown, indicator_configs[indicator_idx].is_momentary ? 0 : 1);
    } else {
        lv_dropdown_set_selected(toggle_mode_dropdown, 0);
    }

    // Animation setting
    lv_obj_t* animation_label = lv_label_create(inputs_container);
    lv_label_set_text(animation_label, "Animation:");
    lv_obj_set_width(animation_label, 110);
    lv_obj_set_style_text_align(animation_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(animation_label, LV_ALIGN_CENTER, -312, 73);
    lv_obj_set_style_text_color(animation_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(animation_label, &lv_font_montserrat_12, 0);

    lv_obj_t* animation_switch = lv_switch_create(inputs_container);
    lv_obj_add_style(animation_switch, get_common_style(), LV_PART_MAIN);
    lv_obj_set_width(animation_switch, 50);
    lv_obj_align(animation_switch, LV_ALIGN_CENTER, -155, 73);
    
    // Add real-time saving callback for animation
    uint8_t* animation_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
    if (animation_idx_ptr) {
        *animation_idx_ptr = indicator_idx;
        lv_obj_add_event_cb(animation_switch, indicator_animation_changed_cb, LV_EVENT_VALUE_CHANGED, animation_idx_ptr);
        printf("Added animation callback for indicator %d\n", indicator_idx);
    }
    
    if (indicator_idx < 2) {
        if (indicator_configs[indicator_idx].animation_enabled) {
            lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(animation_switch, LV_STATE_CHECKED);
        }
    } else {
        lv_obj_add_state(animation_switch, LV_STATE_CHECKED); // Default to enabled
    }



    // Create button container
    lv_obj_t* button_container = lv_obj_create(config_screen);
    lv_obj_set_width(button_container, 240);
    lv_obj_set_height(button_container, 60);
    lv_obj_align(button_container, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_opa(button_container, 0, 0);
    lv_obj_set_style_border_opa(button_container, 0, 0);
    lv_obj_clear_flag(button_container, LV_OBJ_FLAG_SCROLLABLE);

    // Save button
    lv_obj_t* save_btn = lv_btn_create(button_container);
    lv_obj_set_size(save_btn, 110, 45);
    lv_obj_align(save_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_style(save_btn, get_common_style(), LV_PART_MAIN);
    
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Done");
    lv_obj_center(save_label);
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_12, 0);

    // Back button
    lv_obj_t* back_btn = lv_btn_create(button_container);
    lv_obj_set_size(back_btn, 110, 45);
    lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(back_btn, get_common_style(), LV_PART_MAIN);
    
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_12, 0);

    // Save button event handler
    lv_obj_add_event_cb(save_btn, save_indicator_config_cb, LV_EVENT_CLICKED, NULL);

    // Back button event handler
    lv_obj_add_event_cb(back_btn, back_indicator_config_cb, LV_EVENT_CLICKED, NULL);

    // No need to store save data since settings save automatically

    // Yield to prevent blocking other tasks
    vTaskDelay(pdMS_TO_TICKS(1));
    
    // Load the config screen
    lv_scr_load(config_screen);
}

static void check_warning_timeouts(lv_timer_t * timer) {
    // Timeout function is no longer needed for momentary warnings
    // as they now follow the live bit state directly.
    // This function is kept for potential future use but does nothing.
    (void)timer; // Suppress unused parameter warning
}

// CAN timeout checking function
static void check_can_timeouts(lv_timer_t * timer) {
    (void)timer; // Suppress unused parameter warning
    uint64_t current_time = esp_timer_get_time() / 1000; // ms
    const uint64_t CAN_TIMEOUT_MS = 1000; // 1 second timeout
    
    // Check panel timeouts (excluding RPM which is panel 9)
    for (int i = 0; i < 8; i++) {
        if (values_config[i].enabled && 
            (current_time - last_panel_can_received[i]) > CAN_TIMEOUT_MS) {
            
            // Set panel to "---" if it's not already
            if (strcmp(previous_values[i], "---") != 0) {
                strcpy(previous_values[i], "---");
                panel_update_t *p_upd = malloc(sizeof(panel_update_t));
                if (p_upd) {
                    p_upd->panel_index = i;
                    strcpy(p_upd->value_str, "---");
                    p_upd->final_value = 0; // Use 0 to ensure no threshold warnings
                    lv_async_call(update_panel_ui, p_upd);
                }
            }
        }
    }
    
    // Check speed timeout (only if using CAN for speed, not GPS)
    if (!values_config[SPEED_VALUE_ID - 1].use_gps_for_speed &&
        values_config[SPEED_VALUE_ID - 1].enabled &&
        (current_time - last_speed_can_received) > CAN_TIMEOUT_MS) {
        
        // Set speed to "---" if it's not already
        if (strcmp(previous_values[SPEED_VALUE_ID - 1], "---") != 0) {
            strcpy(previous_values[SPEED_VALUE_ID - 1], "---");
            speed_update_t *s_upd = malloc(sizeof(speed_update_t));
            if (s_upd) {
                strcpy(s_upd->speed_str, "---");
                lv_async_call(update_speed_ui, s_upd);
            }
        }
    }
    
    // Check gear timeout
    if (values_config[GEAR_VALUE_ID - 1].enabled &&
        (current_time - last_gear_can_received) > CAN_TIMEOUT_MS) {
        
        // Set gear to "---" if it's not already
        if (strcmp(previous_values[GEAR_VALUE_ID - 1], "---") != 0) {
            strcpy(previous_values[GEAR_VALUE_ID - 1], "---");
            gear_update_t *g_upd = malloc(sizeof(gear_update_t));
            if (g_upd) {
                strcpy(g_upd->gear_str, "---");
                lv_async_call(update_gear_ui, g_upd);
            }
        }
    }
    
    // Check bar timeouts
    for (int i = 0; i < 2; i++) {
        int value_index = (i == 0) ? BAR1_VALUE_ID - 1 : BAR2_VALUE_ID - 1;
        if (values_config[value_index].enabled &&
            !values_config[value_index].use_fuel_input && // Only for CAN bars, not fuel input
            (current_time - last_bar_can_received[i]) > CAN_TIMEOUT_MS) {
            
            // Set bar to minimum value (representing "no data")
            bar_update_t *b_upd = malloc(sizeof(bar_update_t));
            if (b_upd) {
                b_upd->bar_index = i;
                b_upd->bar_value = values_config[value_index].bar_min; // Use minimum value
                b_upd->final_value = values_config[value_index].bar_min;
                b_upd->config_index = value_index;
                lv_async_call(update_bar_ui, b_upd);
            }
        }
    }
    
    // Check RPM timeout - instantly go to 0
    if (values_config[RPM_VALUE_ID - 1].enabled &&
        (current_time - last_rpm_can_received) > CAN_TIMEOUT_MS) {
        
        // Set RPM to "0" if it's not already
        if (strcmp(previous_values[RPM_VALUE_ID - 1], "0") != 0) {
            strcpy(previous_values[RPM_VALUE_ID - 1], "0");
            rpm_update_t *r_upd = malloc(sizeof(rpm_update_t));
            if (r_upd) {
                strcpy(r_upd->rpm_str, "0");
                r_upd->rpm_value = 0; // Use 0 for both display and gauge
                lv_async_call(update_rpm_ui, r_upd);
            }
        }
    }
}

/////////////////////////////////////////////	PROCESSING	/////////////////////////////////////////////

void set_rpm_value(int rpm) {
    if (rpm < 0) rpm = 0;
    
    // Store the current CAN bus RPM value when not in demo mode
    if (!limiter_demo_active) {
        current_canbus_rpm = rpm;
    }
    
    if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
        // Map RPM to extended bar range to properly fill the extended bar width
        // When RPM reaches rpm_gauge_max, the bar should be completely filled
        const float bar_extension_ratio = 782.5f / 765.0f;
        int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);
        
        // Scale the RPM value to the extended range
        int32_t scaled_rpm = (rpm * extended_rpm_max) / rpm_gauge_max;
        
        lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
    }
    
    // Update RPM lights based on current RPM value (only if not in demo mode)
    if (!limiter_demo_active) {
        update_rpm_lights(rpm);
        
        // Check if we should activate limiter effects based on real RPM
        // Only process if RPM actually changed to avoid redundant checks
        if (rpm != last_rpm_for_limiter_check) {
            last_rpm_for_limiter_check = rpm;
            
            uint8_t limiter_effect = values_config[RPM_VALUE_ID - 1].rpm_limiter_effect;
            int32_t limiter_threshold = values_config[RPM_VALUE_ID - 1].rpm_limiter_value;
            
            // Add hysteresis: activate at threshold, deactivate at threshold - 200 RPM
            const int32_t HYSTERESIS = 200;
            
            if (limiter_effect > 0) {
                if (!limiter_active && rpm >= limiter_threshold) {
                    // RPM exceeded limiter threshold, start effect
                    limiter_active = true;
                    start_real_limiter_effect(limiter_effect);
                } else if (limiter_active && rpm < (limiter_threshold - HYSTERESIS)) {
                    // RPM dropped below threshold minus hysteresis, stop effect
                    limiter_active = false;
                    stop_real_limiter_effect();
                }
                // If RPM is between (threshold - HYSTERESIS) and threshold, maintain current state
            }
        }
    }
}

void update_redline_position(void) {
    if (!rpm_redline_zone) return;
    
    // Calculate redline position as percentage of max RPM
    float redline_percentage = (float)rpm_redline_value / (float)rpm_gauge_max;
    
    // Clamp to prevent going beyond the bar
    if (redline_percentage > 1.0f) redline_percentage = 1.0f;
    if (redline_percentage < 0.0f) redline_percentage = 0.0f;
    
    // Screen and RPM bar dimensions
    const lv_coord_t screen_width = 800; // Full screen width
    const lv_coord_t bar_width = 765;
    const lv_coord_t screen_right_edge = screen_width / 2; // Right edge relative to center
    
    // Calculate redline zone dimensions - extends from right edge of screen to redline position
    lv_coord_t redline_rpm_position = -(bar_width / 2) + (redline_percentage * bar_width); // RPM position on bar
    lv_coord_t redline_width = screen_right_edge - redline_rpm_position; // From redline to right edge of screen
    
    // If redline is at or beyond max RPM, hide the zone
    if (redline_percentage >= 1.0f || redline_width <= 0) {
        lv_obj_add_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);
        printf("Redline zone hidden (redline at or beyond max RPM)\n");
        return;
    }
    
    // Show and position the redline zone
    lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(rpm_redline_zone, redline_width);
    // Position so it starts at redline RPM position and extends to right edge
    lv_obj_set_x(rpm_redline_zone, redline_rpm_position + (redline_width / 2));
    
    printf("Redline updated: %d RPM at %.1f%% (zone: from RPM pos %d to screen edge, width=%d)\n", 
           rpm_redline_value, redline_percentage * 100, redline_rpm_position, redline_width);
}

// Asynchronous callback for updating a panel's UI (e.g., the label text)
void update_panel_ui(void *param)
{
    panel_update_t *update = (panel_update_t *)param;
    if (!update) return;

    uint8_t i = update->panel_index;
    if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]) && lv_obj_get_screen(ui_Value[i]) != NULL) {
        lv_label_set_text(ui_Value[i], update->value_str);
    }
    
    // Also update menu preview if it exists, is valid, and menu is visible
    if (menu_panel_value_labels[i] && lv_obj_is_valid(menu_panel_value_labels[i]) && 
        ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
        lv_label_set_text(menu_panel_value_labels[i], update->value_str);
    }
    
    // Also update menu panel box border effects if menu is visible
    if (menu_panel_boxes[i] && lv_obj_is_valid(menu_panel_boxes[i]) && 
        ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
        // Apply same border logic as main screen panels
        if (strcmp(update->value_str, "---") == 0) {
            lv_obj_set_style_border_color(menu_panel_boxes[i], 
                lv_color_hex(0x2e2f2e), 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (values_config[i].warning_high_enabled && 
            update->final_value > values_config[i].warning_high_threshold) {
            // High warning threshold exceeded
            lv_obj_set_style_border_color(menu_panel_boxes[i], 
                values_config[i].warning_high_color, 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        } 
        else if (values_config[i].warning_low_enabled && 
               update->final_value < values_config[i].warning_low_threshold) {
            // Low warning threshold exceeded
            lv_obj_set_style_border_color(menu_panel_boxes[i], 
                values_config[i].warning_low_color, 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        } 
        else {
            // No thresholds exceeded, use default color
            lv_obj_set_style_border_color(menu_panel_boxes[i], 
                lv_color_hex(0x2e2f2e), 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        // Ensure border is visible
        lv_obj_set_style_border_width(menu_panel_boxes[i], 3, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(menu_panel_boxes[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    // Update border color based on thresholds
    if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) && lv_obj_get_screen(ui_Box[i]) != NULL) {
        // Special case: if showing "---", always use default grey color
        if (strcmp(update->value_str, "---") == 0) {
            lv_obj_set_style_border_color(ui_Box[i], 
                lv_color_hex(0x2e2f2e), 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (values_config[i].warning_high_enabled && 
            update->final_value > values_config[i].warning_high_threshold) {
            // High warning threshold exceeded
            lv_obj_set_style_border_color(ui_Box[i], 
                values_config[i].warning_high_color, 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        } 
        else if (values_config[i].warning_low_enabled && 
               update->final_value < values_config[i].warning_low_threshold) {
            // Low warning threshold exceeded
            lv_obj_set_style_border_color(ui_Box[i], 
                values_config[i].warning_low_color, 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        } 
        else {
            // No thresholds exceeded, use default color
            lv_obj_set_style_border_color(ui_Box[i], 
                lv_color_hex(0x2e2f2e), 
                LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        // Ensure border is visible
        lv_obj_set_style_border_width(ui_Box[i], 3, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(ui_Box[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    free(update);
}

// Asynchronous callback for updating a bar's display and color
void update_bar_ui(void *param)
{
    bar_update_t *upd = (bar_update_t *)param;
    // Select the appropriate bar object (assuming bar_index 0 means ui_Bar_1 and 1 means ui_Bar_2)
    lv_obj_t *bar_obj = (upd->bar_index == 0) ? ui_Bar_1 : ui_Bar_2;
    
    // Check if the bar object is still valid.
    if (bar_obj == NULL || !lv_obj_is_valid(bar_obj) || lv_obj_get_screen(bar_obj) == NULL) {
        free(upd);
        return;
    }
    
    lv_bar_set_value(bar_obj, upd->bar_value, LV_ANIM_OFF);
    
    // Use configured colors instead of hardcoded values
    lv_color_t new_color;
    if (upd->final_value < values_config[upd->config_index].bar_low) {
        new_color = values_config[upd->config_index].bar_low_color;
    } else if (upd->final_value > values_config[upd->config_index].bar_high) {
        new_color = values_config[upd->config_index].bar_high_color;
    } else {
        new_color = values_config[upd->config_index].bar_in_range_color;
    }
    
    lv_obj_set_style_bg_color(bar_obj, new_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    
    // Also update menu preview bar if it exists, is valid, and menu is visible
    lv_obj_t *menu_bar = menu_bar_objects[upd->bar_index];
    if (menu_bar && lv_obj_is_valid(menu_bar) && 
        ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
        lv_bar_set_value(menu_bar, upd->bar_value, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(menu_bar, new_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
    
    free(upd);
}

// Asynchronous callback for updating the RPM UI (label and gauge)
void update_rpm_ui(void *param)
{
    rpm_update_t *r_upd = (rpm_update_t *)param;
    
    // Check if both the RPM label and RPM gauge are valid.
    if ((ui_RPM_Value == NULL || lv_obj_get_screen(ui_RPM_Value) == NULL) ||
        (rpm_bar_gauge == NULL || lv_obj_get_screen(rpm_bar_gauge) == NULL)) {
         free(r_upd);
         return;
    }
    
    lv_label_set_text(ui_RPM_Value, r_upd->rpm_str);
    set_rpm_value(r_upd->rpm_value);
    
    // Update menu RPM value text when CAN bus is active and no limiter demo is running
    if (!limiter_demo_active) {
        update_menu_rpm_value_text(r_upd->rpm_value);
    }
    
    free(r_upd);
}

// Asynchronous callback for updating the Speed label
void update_speed_ui(void *param)
{
    speed_update_t *s_upd = (speed_update_t *)param;
    
    if (ui_Speed_Value == NULL || lv_obj_get_screen(ui_Speed_Value) == NULL) {
          free(s_upd);
          return;
    }
    
    lv_label_set_text(ui_Speed_Value, s_upd->speed_str);
    
    // Also update menu preview if it exists, is valid, and menu is visible
    if (menu_speed_value_label && lv_obj_is_valid(menu_speed_value_label) && 
        ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
        lv_label_set_text(menu_speed_value_label, s_upd->speed_str);
    }
    
    free(s_upd);
}

// Asynchronous callback for updating the Gear label
void update_gear_ui(void *param)
{
    gear_update_t *g_upd = (gear_update_t *)param;
    
    if (ui_GEAR_Value == NULL || lv_obj_get_screen(ui_GEAR_Value) == NULL) {
          free(g_upd);
          return;
    }
    
    lv_label_set_text(ui_GEAR_Value, g_upd->gear_str);
    
    // Also update menu preview if it exists, is valid, and menu is visible
    if (menu_gear_value_label && lv_obj_is_valid(menu_gear_value_label) && 
        ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
        lv_label_set_text(menu_gear_value_label, g_upd->gear_str);
    }
    
    free(g_upd);
}

// Asynchronous callback for updating a warning indicator
void update_warning_ui(void *param)
{
    uint8_t warning_idx = *(uint8_t *)param;
    free(param);
    
    if (warning_circles[warning_idx] == NULL || lv_obj_get_screen(warning_circles[warning_idx]) == NULL) {
         return;
    }
    
    lv_color_t new_color = warning_configs[warning_idx].current_state ?
         warning_configs[warning_idx].active_color :
         lv_color_hex(0x292C29);  // Default "off" color.
    
    lv_obj_set_style_bg_color(warning_circles[warning_idx],
         new_color,
         LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Also update the warning label visibility
    if (warning_labels[warning_idx] && lv_obj_is_valid(warning_labels[warning_idx])) {
        if (warning_configs[warning_idx].current_state) {
            lv_obj_clear_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Function to update the config preview 
void update_config_preview(uint8_t indicator_idx) {
    if (preview_indicator_config && lv_obj_is_valid(preview_indicator_config) && 
        preview_status_text_config && lv_obj_is_valid(preview_status_text_config) &&
        preview_indicator_idx == indicator_idx) {
        
        printf("Updating config preview for indicator %d: %s\n", indicator_idx, 
               indicator_configs[indicator_idx].current_state ? "ACTIVE" : "INACTIVE");
        
        // Update preview indicator opacity
        if (indicator_configs[indicator_idx].current_state) {
            lv_obj_set_style_opa(preview_indicator_config, 255, LV_PART_MAIN | LV_STATE_DEFAULT); // 100% opacity
        } else {
            lv_obj_set_style_opa(preview_indicator_config, 50, LV_PART_MAIN | LV_STATE_DEFAULT);  // 50% opacity
        }
        
        // Update preview status text
        lv_label_set_text_fmt(preview_status_text_config, "%s INDICATOR\n%s", 
            indicator_idx == 0 ? "LEFT" : "RIGHT",
            indicator_configs[indicator_idx].current_state ? "ACTIVE" : "INACTIVE");
        
        // Update text color based on state
        if (indicator_configs[indicator_idx].current_state) {
            lv_obj_set_style_text_color(preview_status_text_config, lv_color_hex(0x00FF00), 0); // Green when active
        } else {
            lv_obj_set_style_text_color(preview_status_text_config, lv_color_hex(0xCCCCCC), 0); // Gray when inactive
        }
    }
}

// Asynchronous callback for updating an indicator
void update_indicator_ui(void *param)
{
    uint8_t indicator_idx = *(uint8_t *)param;
    free(param);

    if (indicator_idx >= 2) return;

    // Check if state actually changed to avoid redundant updates
    bool current_state = indicator_configs[indicator_idx].current_state;
    if (previous_indicator_states[indicator_idx] == current_state) {
        return; // No change, skip update
    }
    
    // Update the previous state
    previous_indicator_states[indicator_idx] = current_state;

    // Update main indicator opacity based on state
    lv_obj_t* indicator_obj = (indicator_idx == 0) ? ui_Indicator_Left : ui_Indicator_Right;
    
    if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
        if (current_state) {
            printf("Indicator %d: Setting to ACTIVE\n", indicator_idx);
            if (indicator_configs[indicator_idx].animation_enabled) {
                printf("Indicator %d: Animation enabled - starting timer\n", indicator_idx);
                // Always start with 100% opacity for immediate visibility
                lv_obj_set_style_opa(indicator_obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                // Set animation state to false so first timer tick will toggle to true (100%)
                indicator_animation_state = false; // Next toggle will be bright
                if (indicator_animation_timer) {
                    lv_timer_resume(indicator_animation_timer);
                }
            } else {
                printf("Indicator %d: Solid mode - 100%% opacity\n", indicator_idx);
                lv_obj_set_style_opa(indicator_obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        } else {
            printf("Indicator %d: Setting to INACTIVE (50%% opacity)\n", indicator_idx);
            lv_obj_set_style_opa(indicator_obj, 50, LV_PART_MAIN | LV_STATE_DEFAULT);  // 50% opacity (default)
        }
    } else {
        printf("Indicator %d: Object is null or invalid\n", indicator_idx);
    }
    
    // Also update the config preview if it's visible
    update_config_preview(indicator_idx);
}

static int64_t extract_bits(const uint8_t *data, uint8_t bit_offset, uint8_t bit_length, int endian, bool is_signed) {
    // Fast path for common cases
    if (bit_length == 0) return 0;
    if (bit_length > 64) bit_length = 64;
    
    uint64_t value = 0;
    
    if (endian == 0) { // Big Endian
        // Calculate byte boundaries
        uint8_t start_byte = bit_offset / 8;
        uint8_t end_byte = (bit_offset + bit_length - 1) / 8;
        uint8_t bytes_needed = end_byte - start_byte + 1;
        
        // Fast path for single byte extraction
        if (bytes_needed == 1) {
            uint8_t bit_pos = bit_offset % 8;
            uint8_t mask = ((1U << bit_length) - 1) << (8 - bit_pos - bit_length);
            value = (data[start_byte] & mask) >> (8 - bit_pos - bit_length);
        }
        // Fast path for aligned multi-byte values (8, 16, 32 bits)
        else if ((bit_offset % 8) == 0 && (bit_length % 8) == 0) {
            for (int i = 0; i < bytes_needed && i < 8; i++) {
                value = (value << 8) | data[start_byte + i];
            }
        }
        // General case - optimized bit operations
        else {
            // Load all relevant bytes into a 64-bit word
            uint64_t word = 0;
            for (int i = 0; i < bytes_needed && i < 8; i++) {
                word = (word << 8) | data[start_byte + i];
            }
            // Shift and mask to extract the bits
            uint8_t shift = (bytes_needed * 8) - (bit_offset % 8) - bit_length;
            uint64_t mask = ((1ULL << bit_length) - 1);
            value = (word >> shift) & mask;
        }
    } else { // Little Endian  
        // Calculate byte boundaries
        uint8_t start_byte = bit_offset / 8;
        uint8_t end_byte = (bit_offset + bit_length - 1) / 8;
        uint8_t bytes_needed = end_byte - start_byte + 1;
        
        // Fast path for single byte extraction
        if (bytes_needed == 1) {
            uint8_t bit_pos = bit_offset % 8;
            uint8_t mask = (1U << bit_length) - 1;
            value = (data[start_byte] >> bit_pos) & mask;
        }
        // Fast path for aligned multi-byte values (8, 16, 32 bits)
        else if ((bit_offset % 8) == 0 && (bit_length % 8) == 0) {
            for (int i = 0; i < bytes_needed && i < 8; i++) {
                value |= ((uint64_t)data[start_byte + i]) << (i * 8);
            }
        }
        // General case - optimized bit operations
        else {
            // Load all relevant bytes
            uint64_t word = 0;
            for (int i = 0; i < bytes_needed && i < 8; i++) {
                word |= ((uint64_t)data[start_byte + i]) << (i * 8);
            }
            // Shift and mask to extract the bits
            uint8_t shift = bit_offset % 8;
            uint64_t mask = ((1ULL << bit_length) - 1);
            value = (word >> shift) & mask;
        }
    }
    
    // Handle signed values with optimized sign extension
    if (is_signed && (value & ((uint64_t)1 << (bit_length - 1)))) {
        // Sign extend using arithmetic right shift
        value |= (~((1ULL << bit_length) - 1));
    }
    
    return (int64_t)value;
}

// ---------- Stage 2: O(1) dispatch mapping from CAN ID to affected indices ----------
typedef struct {
    uint32_t can_id;
    uint8_t num_warning;
    uint8_t warning_indices[8];
    uint8_t num_indicator;
    uint8_t indicator_indices[2];
    uint8_t num_values;
    uint8_t value_indices[13];
} can_dispatch_entry_t;

static can_dispatch_entry_t can_dispatch_entries[32];
static int can_dispatch_count = 0;
static int16_t can_id_to_dispatch_index[2048]; // -1 if none

static int get_or_add_dispatch_entry(uint32_t can_id)
{
    uint32_t sid = can_id & 0x7FF;
    if (sid < 2048) {
        int16_t idx = can_id_to_dispatch_index[sid];
        if (idx >= 0) return idx;
    }
    if (can_dispatch_count >= (int)(sizeof(can_dispatch_entries)/sizeof(can_dispatch_entries[0]))) {
        return -1;
    }
    int idx = can_dispatch_count++;
    can_dispatch_entries[idx].can_id = sid;
    can_dispatch_entries[idx].num_warning = 0;
    can_dispatch_entries[idx].num_indicator = 0;
    can_dispatch_entries[idx].num_values = 0;
    if (sid < 2048) can_id_to_dispatch_index[sid] = (int16_t)idx;
    return idx;
}

void rebuild_can_dispatch(void)
{
    for (int i = 0; i < 2048; i++) can_id_to_dispatch_index[i] = -1;
    can_dispatch_count = 0;

    for (int i = 0; i < 8; i++) {
        if (warning_configs[i].can_id != 0) {
            int idx = get_or_add_dispatch_entry(warning_configs[i].can_id);
            if (idx >= 0 && can_dispatch_entries[idx].num_warning < 8) {
                can_dispatch_entries[idx].warning_indices[can_dispatch_entries[idx].num_warning++] = (uint8_t)i;
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        if (indicator_configs[i].can_id != 0) {
            int idx = get_or_add_dispatch_entry(indicator_configs[i].can_id);
            if (idx >= 0 && can_dispatch_entries[idx].num_indicator < 2) {
                can_dispatch_entries[idx].indicator_indices[can_dispatch_entries[idx].num_indicator++] = (uint8_t)i;
            }
        }
    }
    for (int i = 0; i < 13; i++) {
        if (values_config[i].enabled && values_config[i].can_id != 0) {
            int idx = get_or_add_dispatch_entry(values_config[i].can_id);
            if (idx >= 0 && can_dispatch_entries[idx].num_values < 13) {
                can_dispatch_entries[idx].value_indices[can_dispatch_entries[idx].num_values++] = (uint8_t)i;
            }
        }
    }
}

void process_can_message(const twai_message_t *message)
{
    static uint64_t last_panel_updates[8] = {0};
    static uint64_t last_bar_updates[2] = {0};
    static bool previous_bit_states[8] = {false};
    uint64_t current_time = esp_timer_get_time() / 1000; // ms
    uint32_t received_id = message->identifier;
    
    // Reset tracking variables after screen recreation to force UI updates
    if (reset_can_tracking) {
        reset_can_tracking = false;
        memset(last_panel_updates, 0, sizeof(last_panel_updates));
        memset(last_bar_updates, 0, sizeof(last_bar_updates));
        memset(previous_bit_states, 0, sizeof(previous_bit_states));
        // Reset CAN timeout tracking
        memset(last_panel_can_received, 0, sizeof(last_panel_can_received));
        memset(last_bar_can_received, 0, sizeof(last_bar_can_received));
        last_speed_can_received = 0;
        last_gear_can_received = 0;
        last_rpm_can_received = 0;
        // Force immediate updates by setting all times to 0
        for (int i = 0; i < 2; i++) {
            previous_bar_values[i] = -999; // Use impossible values to force bar updates
        }
    }

    // Fast-dispatch path using prebuilt mapping; fallback to linear scans if not found
    int16_t didx = (received_id <= 0x7FF) ? can_id_to_dispatch_index[received_id] : -1;
    if (didx >= 0) {
        // Precompute 64-bit data value once for bit-extraction consumers
        uint64_t data_value = 0;
        for (int j = 0; j < message->data_length_code && j < 8; j++) {
            data_value |= (uint64_t)message->data[j] << (j * 8);
        }

        // Warnings
        for (uint8_t wi = 0; wi < can_dispatch_entries[didx].num_warning; wi++) {
            int i = can_dispatch_entries[didx].warning_indices[wi];
            bool current_bit_state = (data_value >> warning_configs[i].bit_position) & 0x01;
            if (warning_configs[i].is_momentary) {
                if (current_bit_state != warning_configs[i].current_state) {
                    warning_configs[i].current_state = current_bit_state;
                    if (current_bit_state) {
                        last_signal_times[i] = current_time;
                    }
                }
            } else {
                if (previous_bit_states[i] && !current_bit_state) {
                    if (!toggle_debounce[i]) {
                        toggle_debounce[i] = true;
                        toggle_start_time[i] = current_time;
                        warning_configs[i].current_state = !warning_configs[i].current_state;
                    }
                } else if (!current_bit_state) {
                    toggle_debounce[i] = false;
                }
            }
            previous_bit_states[i] = current_bit_state;
            uint8_t *w_idx = malloc(sizeof(uint8_t));
            if (w_idx) { *w_idx = i; lv_async_call(update_warning_ui, w_idx); }
        }

        // Indicators
        for (uint8_t ii = 0; ii < can_dispatch_entries[didx].num_indicator; ii++) {
            int i = can_dispatch_entries[didx].indicator_indices[ii];
            bool current_bit_state = (data_value >> indicator_configs[i].bit_position) & 0x01;
            if (indicator_configs[i].is_momentary) {
                if (current_bit_state != indicator_configs[i].current_state) {
                    indicator_configs[i].current_state = current_bit_state;
                }
            } else {
                if (previous_indicator_bit_states[i] && !current_bit_state) {
                    if (!indicator_toggle_debounce[i]) {
                        indicator_toggle_debounce[i] = true;
                        indicator_toggle_start_time[i] = current_time;
                        indicator_configs[i].current_state = !indicator_configs[i].current_state;
                    }
                } else if (!current_bit_state) {
                    indicator_toggle_debounce[i] = false;
                }
            }
            previous_indicator_bit_states[i] = current_bit_state;
            uint8_t *ind_idx = malloc(sizeof(uint8_t));
            if (ind_idx) { *ind_idx = i; lv_async_call(update_indicator_ui, ind_idx); }
        }

        // Values
        for (uint8_t vi = 0; vi < can_dispatch_entries[didx].num_values; vi++) {
            int i = can_dispatch_entries[didx].value_indices[vi];
            if (values_config[i].enabled) {
                uint8_t value_id = i + 1;
                int64_t raw_value = extract_bits(message->data,
                                                  values_config[i].bit_start,
                                                  values_config[i].bit_length,
                                                  values_config[i].endianess,
                                                  values_config[i].is_signed);
                double final_value = (double)raw_value * values_config[i].scale + values_config[i].value_offset;

                if (i < 8) {
                    last_panel_can_received[i] = current_time;
                    if (i == 6) {
                        char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
                        if (values_config[i].decimals == 0) {
                            snprintf(new_value_str, sizeof(new_value_str), "%d", (int)final_value);
                        } else {
                            snprintf(new_value_str, sizeof(new_value_str), "%.*f", values_config[i].decimals, final_value);
                        }
                        if (strcmp(new_value_str, previous_values[i]) != 0) {
                            strcpy(previous_values[i], new_value_str);
                            panel_update_t *p_upd = malloc(sizeof(panel_update_t));
                            if (p_upd) { p_upd->panel_index = i; strcpy(p_upd->value_str, new_value_str); p_upd->final_value = final_value; lv_async_call(update_panel_ui, p_upd); }
                        }
                    } else {
                        if (current_time - last_panel_updates[i] >= 25) {
                            char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
                            if (values_config[i].decimals == 0) {
                                snprintf(new_value_str, sizeof(new_value_str), "%d", (int)final_value);
                            } else {
                                snprintf(new_value_str, sizeof(new_value_str), "%.*f", values_config[i].decimals, final_value);
                            }
                            if (strcmp(new_value_str, previous_values[i]) != 0) {
                                strcpy(previous_values[i], new_value_str);
                                panel_update_t *p_upd = malloc(sizeof(panel_update_t));
                                if (p_upd) { p_upd->panel_index = i; strcpy(p_upd->value_str, new_value_str); p_upd->final_value = final_value; lv_async_call(update_panel_ui, p_upd); }
                            }
                            last_panel_updates[i] = current_time;
                        }
                    }
                    continue;
                }

                if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
                    int bar_index = value_id - BAR1_VALUE_ID;
                    last_bar_can_received[bar_index] = current_time;
                    if (values_config[i].use_fuel_input) {
                        float current_voltage = fuel_input_read_voltage();
                        float fuel_level = fuel_input_calculate_level(current_voltage, values_config[i].fuel_empty_voltage, values_config[i].fuel_full_voltage);
                        final_value = fuel_level;
                    }
                    if (value_id == BAR2_VALUE_ID || current_time - last_bar_updates[bar_index] >= 25) {
                        if (value_id == BAR2_VALUE_ID || fabs(final_value - previous_bar_values[bar_index]) >= BAR_UPDATE_THRESHOLD) {
                            previous_bar_values[bar_index] = final_value;
                            lv_obj_t *bar_obj = (value_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;
                            if (bar_obj) {
                                int32_t bar_value = (int32_t)final_value;
                                if (bar_value < values_config[i].bar_min) bar_value = values_config[i].bar_min;
                                else if (bar_value > values_config[i].bar_max) bar_value = values_config[i].bar_max;
                                bar_update_t *b_upd = malloc(sizeof(bar_update_t));
                                if (b_upd) { b_upd->bar_index = bar_index; b_upd->bar_value = bar_value; b_upd->final_value = final_value; b_upd->config_index = i; lv_async_call(update_bar_ui, b_upd); }
                            }
                        }
                        if (value_id != BAR2_VALUE_ID) { last_bar_updates[bar_index] = current_time; }
                    }
                    continue;
                }

                if (value_id == RPM_VALUE_ID) {
                    last_rpm_can_received = current_time;
                    int rpm_value = (int)final_value;
                    int gauge_rpm_value = rpm_value < 0 ? 0 : (rpm_value > rpm_gauge_max ? rpm_gauge_max : rpm_value);
                    int display_rpm_value = (int)((float)rpm_value * 1.0229f);
                    char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
                    snprintf(rpm_str, sizeof(rpm_str), "%d", display_rpm_value);
                    static int last_rpm_value = -1;
                    if (strcmp(rpm_str, previous_values[i]) != 0 || rpm_value != last_rpm_value) {
                        strcpy(previous_values[i], rpm_str);
                        last_rpm_value = rpm_value;
                        rpm_update_t *r_upd = malloc(sizeof(rpm_update_t));
                        if (r_upd) { strcpy(r_upd->rpm_str, rpm_str); r_upd->rpm_value = gauge_rpm_value; lv_async_call(update_rpm_ui, r_upd); }
                    }
                } else if (value_id == SPEED_VALUE_ID) {
                    if (values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) { continue; }
                    last_speed_can_received = current_time;
                    double speed_value = final_value;
                    char speed_str[EXAMPLE_MAX_CHAR_SIZE];
                    snprintf(speed_str, sizeof(speed_str), "%.0f", speed_value);
                    if (strcmp(speed_str, previous_values[i]) != 0) {
                        strcpy(previous_values[i], speed_str);
                        speed_update_t *s_upd = malloc(sizeof(speed_update_t));
                        if (s_upd) { strcpy(s_upd->speed_str, speed_str); lv_async_call(update_speed_ui, s_upd); }
                    }
                } else if (value_id == GEAR_VALUE_ID) {
                    last_gear_can_received = current_time;
                    char gear_str[EXAMPLE_MAX_CHAR_SIZE];
                    if (strcasecmp(label_texts[GEAR_VALUE_ID - 1], "GEAR") == 0) {
                        uint8_t gear_mode = values_config[GEAR_VALUE_ID - 1].gear_detection_mode;
                        if (gear_mode == 1) {
                            if (final_value == 0) { snprintf(gear_str, sizeof(gear_str), "N"); }
                            else if (final_value == 0xFE || final_value == 0xF0 || final_value == 0xFF) { snprintf(gear_str, sizeof(gear_str), "R"); }
                            else if (final_value >= 1 && final_value <= 10) { snprintf(gear_str, sizeof(gear_str), "%d", (int)final_value); }
                            else { snprintf(gear_str, sizeof(gear_str), "-"); }
                        } else if (gear_mode == 2) {
                            if (final_value == 0) { snprintf(gear_str, sizeof(gear_str), "N"); }
                            else if (final_value == 255 || final_value == 0xFE) { snprintf(gear_str, sizeof(gear_str), "R"); }
                            else if (final_value >= 1 && final_value <= 8) { snprintf(gear_str, sizeof(gear_str), "%d", (int)final_value); }
                            else { snprintf(gear_str, sizeof(gear_str), "-"); }
                        } else {
                            bool gear_found = false;
                            uint32_t current_can_id = values_config[value_id - 1].can_id;
                            for (int gear_idx = 0; gear_idx < 12; gear_idx++) {
                                if (values_config[GEAR_VALUE_ID - 1].gear_custom_can_ids[gear_idx] == current_can_id && current_can_id != 0) {
                                    if (gear_idx == 0) { snprintf(gear_str, sizeof(gear_str), "N"); }
                                    else if (gear_idx == 1) { snprintf(gear_str, sizeof(gear_str), "R"); }
                                    else { snprintf(gear_str, sizeof(gear_str), "%d", gear_idx - 1); }
                                    gear_found = true; break;
                                }
                            }
                            if (!gear_found) {
                                if (final_value == 0) { snprintf(gear_str, sizeof(gear_str), "N"); }
                                else if (final_value == 0xFE || final_value == 0xF0 || final_value == 0xFF) { snprintf(gear_str, sizeof(gear_str), "R"); }
                                else if (final_value >= 1 && final_value <= 10) { snprintf(gear_str, sizeof(gear_str), "%d", (int)final_value); }
                                else { snprintf(gear_str, sizeof(gear_str), "-"); }
                            }
                        }
                    } else {
                        snprintf(gear_str, sizeof(gear_str), "%.0f", final_value);
                    }
                    if (strcmp(gear_str, previous_values[i]) != 0) {
                        strcpy(previous_values[i], gear_str);
                        gear_update_t *g_upd = malloc(sizeof(gear_update_t));
                        if (g_upd) { strcpy(g_upd->gear_str, gear_str); lv_async_call(update_gear_ui, g_upd); }
                    }
                }
            }
        }
        return; // handled via fast path
    }

    // Process warning configurations (for warnings 0-7)
    for (int i = 0; i < 8; i++) {
        if (warning_configs[i].can_id == received_id) {
            // Create 64-bit value from message data
            uint64_t data_value = 0;
            for (int j = 0; j < message->data_length_code && j < 8; j++) {
                data_value |= (uint64_t)message->data[j] << (j * 8);
            }
            
            // Extract bit using 64-bit position
            bool current_bit_state = (data_value >> warning_configs[i].bit_position) & 0x01;
            


            if (warning_configs[i].is_momentary) {
                // For momentary mode: activate warning directly based on bit state (active high)
                // This will show warning when bit is 1, hide when bit is 0
                if (current_bit_state != warning_configs[i].current_state) {
                    warning_configs[i].current_state = current_bit_state;
                    if (current_bit_state) {
                        last_signal_times[i] = current_time;
                    }
                }
            } else {
                // For toggle mode: toggle on falling edge (1->0 transition)
                if (previous_bit_states[i] && !current_bit_state) {
                    if (!toggle_debounce[i]) {
                        toggle_debounce[i] = true;
                        toggle_start_time[i] = current_time;
                        warning_configs[i].current_state = !warning_configs[i].current_state;
                    }
                } else if (!current_bit_state) {
                    toggle_debounce[i] = false;
                }
            }
            previous_bit_states[i] = current_bit_state;

            // Update warning UI
            uint8_t *w_idx = malloc(sizeof(uint8_t));
            if (w_idx) {
                *w_idx = i;
                lv_async_call(update_warning_ui, w_idx);
            }
        }
    }

    // Process indicator configurations (for left and right indicators)
    for (int i = 0; i < 2; i++) {
        if (indicator_configs[i].can_id == received_id) {
            // Create 64-bit value from message data
            uint64_t data_value = 0;
            for (int j = 0; j < message->data_length_code && j < 8; j++) {
                data_value |= (uint64_t)message->data[j] << (j * 8);
            }
            
            // Extract bit using 64-bit position
            bool current_bit_state = (data_value >> indicator_configs[i].bit_position) & 0x01;

            if (indicator_configs[i].is_momentary) {
                // For momentary mode: activate indicator directly based on bit state (active high)
                if (current_bit_state != indicator_configs[i].current_state) {
                    indicator_configs[i].current_state = current_bit_state;
                }
            } else {
                // For toggle mode: toggle on falling edge (1->0 transition)
                if (previous_indicator_bit_states[i] && !current_bit_state) {
                    if (!indicator_toggle_debounce[i]) {
                        indicator_toggle_debounce[i] = true;
                        indicator_toggle_start_time[i] = current_time;
                        indicator_configs[i].current_state = !indicator_configs[i].current_state;
                    }
                } else if (!current_bit_state) {
                    indicator_toggle_debounce[i] = false;
                }
            }
            previous_indicator_bit_states[i] = current_bit_state;

            // Update indicator UI
            uint8_t *ind_idx = malloc(sizeof(uint8_t));
            if (ind_idx) {
                *ind_idx = i;
                lv_async_call(update_indicator_ui, ind_idx);
            }
        }
    }

    // Process value configurations
    for (int i = 0; i < 13; i++) {
        if (values_config[i].enabled && values_config[i].can_id == received_id) {
            uint8_t value_id = i + 1;
            int64_t raw_value = extract_bits(message->data,
                                              values_config[i].bit_start,
                                              values_config[i].bit_length,
                                              values_config[i].endianess,
                                              values_config[i].is_signed);
            double final_value = (double)raw_value * values_config[i].scale + values_config[i].value_offset;

            // Handle panels (values 1-8)
            if (i < 8) {
                // Update CAN received timestamp for this panel
                last_panel_can_received[i] = current_time;
                
                // Special handling for wideband (panel 7)
                if (i == 6) { // Panel 7 (wideband)
                    char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
                    if (values_config[i].decimals == 0) {
                        snprintf(new_value_str, sizeof(new_value_str), "%d", (int)final_value);
                    } else {
                        snprintf(new_value_str, sizeof(new_value_str), "%.*f", values_config[i].decimals, final_value);
                    }
                    if (strcmp(new_value_str, previous_values[i]) != 0) {
                        strcpy(previous_values[i], new_value_str);
                        panel_update_t *p_upd = malloc(sizeof(panel_update_t));
                        if (p_upd) {
                            p_upd->panel_index = i;
                            strcpy(p_upd->value_str, new_value_str);
                            p_upd->final_value = final_value;
                            lv_async_call(update_panel_ui, p_upd);
                        }
                    }
                } else {
                    // Other panels with 25ms update rate
                if (current_time - last_panel_updates[i] >= 25) {
                    char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
                    if (values_config[i].decimals == 0) {
                        snprintf(new_value_str, sizeof(new_value_str), "%d", (int)final_value);
                    } else {
                        snprintf(new_value_str, sizeof(new_value_str), "%.*f", values_config[i].decimals, final_value);
                    }
                    if (strcmp(new_value_str, previous_values[i]) != 0) {
                        strcpy(previous_values[i], new_value_str);
                        panel_update_t *p_upd = malloc(sizeof(panel_update_t));
                        if (p_upd) {
                            p_upd->panel_index = i;
                            strcpy(p_upd->value_str, new_value_str);
                                p_upd->final_value = final_value;
                            lv_async_call(update_panel_ui, p_upd);
                        }
                    }
                    last_panel_updates[i] = current_time;
                    }
                }
                continue;
            }

            // Handle bars (for BAR1 and BAR2)
            if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
                int bar_index = value_id - BAR1_VALUE_ID;
                // Update CAN received timestamp for this bar
                last_bar_can_received[bar_index] = current_time;
                
                // Check if fuel input is enabled for this bar
                if (values_config[i].use_fuel_input) {
                    // Use fuel input instead of CAN data
                    float current_voltage = fuel_input_read_voltage();
                    float fuel_level = fuel_input_calculate_level(current_voltage, 
                                                                values_config[i].fuel_empty_voltage,
                                                                values_config[i].fuel_full_voltage);
                    final_value = fuel_level; // Use fuel level as percentage (0-100)
                }
                
                // Special fast handling for BAR2
                if (value_id == BAR2_VALUE_ID || current_time - last_bar_updates[bar_index] >= 25) {
                    if (value_id == BAR2_VALUE_ID || fabs(final_value - previous_bar_values[bar_index]) >= BAR_UPDATE_THRESHOLD) {
                        previous_bar_values[bar_index] = final_value;
                        lv_obj_t *bar_obj = (value_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;
                        if (bar_obj) {
                            int32_t bar_value = (int32_t)final_value;
                            // Clamp the value per configuration.
                            if (bar_value < values_config[i].bar_min) {
                                bar_value = values_config[i].bar_min;
                            }
                            else if (bar_value > values_config[i].bar_max) {
                                bar_value = values_config[i].bar_max;
                            }

                            // Create and fill our bar update data.
                            bar_update_t *b_upd = malloc(sizeof(bar_update_t));
                            if (b_upd) {
                                b_upd->bar_index = bar_index;
                                b_upd->bar_value = bar_value;
                                b_upd->final_value = final_value;
                                b_upd->config_index = i;
                                lv_async_call(update_bar_ui, b_upd);
                            }
                        }
                    }
                    if (value_id != BAR2_VALUE_ID) {
                    last_bar_updates[bar_index] = current_time;
                    }
                }
                continue;
            }

            // Handle RPM
            if (value_id == RPM_VALUE_ID) {
                // Update CAN received timestamp for RPM
                last_rpm_can_received = current_time;
                
                // Always process RPM updates for better responsiveness
                int rpm_value = (int)final_value;
                
                // For the gauge bar, limit to rpm_gauge_max
                int gauge_rpm_value = rpm_value < 0 ? 0 : (rpm_value > rpm_gauge_max ? rpm_gauge_max : rpm_value);
                
                // For the display value, apply 102.3% scaling to the actual CAN value (no gauge limit)
                int display_rpm_value = (int)((float)rpm_value * 1.0229f); // 102.3% scaling of actual CAN value
                
                // Update the display string with the scaled actual CAN value
                char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
                snprintf(rpm_str, sizeof(rpm_str), "%d", display_rpm_value);
                
                // Update if string changed OR if actual RPM value changed
                // This ensures limiter effects activate immediately
                static int last_rpm_value = -1;
                if (strcmp(rpm_str, previous_values[i]) != 0 || rpm_value != last_rpm_value) {
                    strcpy(previous_values[i], rpm_str);
                    last_rpm_value = rpm_value;
                    
                    rpm_update_t *r_upd = malloc(sizeof(rpm_update_t));
                    if (r_upd) {
                        strcpy(r_upd->rpm_str, rpm_str);
                        r_upd->rpm_value = gauge_rpm_value; // Use gauge-limited value for bar
                        lv_async_call(update_rpm_ui, r_upd);
                    }
                }
            }
            // Handle Speed
            else if (value_id == SPEED_VALUE_ID) {
                // Skip CAN speed processing if GPS is selected as speed source
                if (values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) {
                    continue; // Skip this CAN message for speed
                }
                
                // Update CAN received timestamp for speed
                last_speed_can_received = current_time;
                
                // Use raw CAN bus value without conversion - let CAN provide the value in desired units
                double speed_value = final_value;
                
                char speed_str[EXAMPLE_MAX_CHAR_SIZE];
                snprintf(speed_str, sizeof(speed_str), "%.0f", speed_value);
                    
                if (strcmp(speed_str, previous_values[i]) != 0) {
                    strcpy(previous_values[i], speed_str);
                    speed_update_t *s_upd = malloc(sizeof(speed_update_t));
                    if (s_upd) {
                        strcpy(s_upd->speed_str, speed_str);
                        lv_async_call(update_speed_ui, s_upd);
                    }
                }
            }
            // Handle Gear
            else if (value_id == GEAR_VALUE_ID) {
                // Update CAN received timestamp for gear
                last_gear_can_received = current_time;
                
                char gear_str[EXAMPLE_MAX_CHAR_SIZE];
                
                // Check if label is "GEAR" (case insensitive)
                if (strcasecmp(label_texts[GEAR_VALUE_ID - 1], "GEAR") == 0) {
                    // Format gear value based on detection mode
                    uint8_t gear_mode = values_config[GEAR_VALUE_ID - 1].gear_detection_mode;
                    
                    if (gear_mode == 1) {
                        // MaxxECU gear detection
                        if (final_value == 0) {
                            snprintf(gear_str, sizeof(gear_str), "N");        // Neutral
                        } else if (final_value == 0xFE || final_value == 0xF0 || final_value == 0xFF) {
                            snprintf(gear_str, sizeof(gear_str), "R");        // Reverse
                        } else if (final_value >= 1 && final_value <= 10) {
                            snprintf(gear_str, sizeof(gear_str), "%d", (int)final_value);
                        } else {
                            snprintf(gear_str, sizeof(gear_str), "-");        // Invalid gear
                        }
                    } else if (gear_mode == 2) {
                        // Haltech gear detection (different encoding)
                        if (final_value == 0) {
                            snprintf(gear_str, sizeof(gear_str), "N");        // Neutral
                        } else if (final_value == 255 || final_value == 0xFE) {
                            snprintf(gear_str, sizeof(gear_str), "R");        // Reverse
                        } else if (final_value >= 1 && final_value <= 8) {
                            snprintf(gear_str, sizeof(gear_str), "%d", (int)final_value);
                        } else {
                            snprintf(gear_str, sizeof(gear_str), "-");        // Invalid gear
                        }
                    } else {
                        // Custom gear detection (mode 0)
                        // Check against custom CAN IDs for each gear
                        bool gear_found = false;
                        uint32_t current_can_id = values_config[value_id - 1].can_id;
                        
                        // Check each custom gear CAN ID
                        for (int gear_idx = 0; gear_idx < 12; gear_idx++) {
                            if (values_config[GEAR_VALUE_ID - 1].gear_custom_can_ids[gear_idx] == current_can_id && current_can_id != 0) {
                                if (gear_idx == 0) {
                                    snprintf(gear_str, sizeof(gear_str), "N");  // Neutral
                                } else if (gear_idx == 1) {
                                    snprintf(gear_str, sizeof(gear_str), "R");  // Reverse
                                } else {
                                    snprintf(gear_str, sizeof(gear_str), "%d", gear_idx - 1); // Gears 1-10
                                }
                                gear_found = true;
                                break;
                            }
                        }
                        
                        if (!gear_found) {
                            // Default MaxxECU format if no custom mapping found
                    if (final_value == 0) {
                        snprintf(gear_str, sizeof(gear_str), "N");
                    } else if (final_value == 0xFE || final_value == 0xF0 || final_value == 0xFF) {
                        snprintf(gear_str, sizeof(gear_str), "R");
                    } else if (final_value >= 1 && final_value <= 10) {
                        snprintf(gear_str, sizeof(gear_str), "%d", (int)final_value);
                    } else {
                                snprintf(gear_str, sizeof(gear_str), "-");
                            }
                        }
                    }
                } else {
                    // Use normal numeric formatting if label isn't "GEAR"
                    snprintf(gear_str, sizeof(gear_str), "%.0f", final_value);
                }

                if (strcmp(gear_str, previous_values[i]) != 0) {
                    strcpy(previous_values[i], gear_str);
                    gear_update_t *g_upd = malloc(sizeof(gear_update_t));
                    if (g_upd) {
                        strcpy(g_upd->gear_str, gear_str);
                        lv_async_call(update_gear_ui, g_upd);
                    }
                }
            }
        }
    }
}

////////////////////////	STYLES	/////////////////////////////////////////////
static lv_style_t box_style;
lv_style_t common_style;

void apply_common_roller_styles(lv_obj_t * roller) {
    // Set text color for dall items
    lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(roller, 0, LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// Initialize styles
static void init_styles(void) {
    // Box Style
    lv_style_init(&box_style);
    lv_style_set_radius(&box_style, 7);
    lv_style_set_bg_color(&box_style, lv_color_hex(0x2e2f2e));
    lv_style_set_bg_opa(&box_style, 0);
    lv_style_set_clip_corner(&box_style, false);
    lv_style_set_border_color(&box_style, lv_color_hex(0x2e2f2e));
    lv_style_set_border_opa(&box_style, 255);
    lv_style_set_border_width(&box_style, 3);
    lv_style_set_border_post(&box_style, true);  // Ensure border is drawn on top
    lv_style_set_outline_width(&box_style, 4);
    lv_style_set_outline_pad(&box_style, 0);
}

void init_common_style(void) {
    lv_style_init(&common_style);
    lv_style_set_radius(&common_style, 7);
    lv_style_set_pad_all(&common_style, 8); // 7px padding on all sides
    lv_style_set_bg_color(&common_style, lv_color_hex(0xFFFFFF)); // White background
    lv_style_set_bg_opa(&common_style, LV_OPA_COVER);
    lv_style_set_border_color(&common_style, lv_color_hex(0xCCCCCC)); // Light gray border
    lv_style_set_border_width(&common_style, 1);
    lv_style_set_text_color(&common_style, lv_color_black()); // Black text
    lv_style_set_text_font(&common_style, &lv_font_montserrat_12); // Common font
}

// Getter function for common_style to allow access from other files
lv_style_t* get_common_style(void) {
    return &common_style;
}

lv_style_t* get_box_style(void) {
    return &box_style;
}

//RPM border style
static lv_obj_t* create_panel(lv_obj_t *parent, int width, int height, int x, int y, int radius, lv_color_t bg_color, int transform_angle) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_align(panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (transform_angle != 0) {
        lv_obj_set_style_transform_angle(panel, transform_angle, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    return panel;
}
/////////////////////////////////////////////	ITEM CREATION	/////////////////////////////////////////////

void create_rpm_bar_gauge(lv_obj_t * parent_screen) {
    ui_RPM_Base_1 = create_panel(parent_screen, 800, 6, 0, -180, 0, lv_color_hex(0x2E2F2E), 0);
    ui_RPM_Base_2 = create_panel(parent_screen, 49, 22, -41, -193, 7, lv_color_hex(0x2E2F2E), 550);
    ui_RPM_Base_3 = create_panel(parent_screen, 49, 22, 105, -181, 7, lv_color_hex(0x2E2F2E), 1250);
    ui_RPM_Base_4 = create_panel(parent_screen, 111, 44, 0, -176, 7, lv_color_hex(0x2E2F2E), 0);
	lv_color_t saved_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
    ui_Panel9 = create_panel(parent_screen, 55, 55, -372, -211, 0, saved_color, 0);
    
    // Calculate extended RPM max for rightward extension to screen edge
    // Original RPM bar: 765px centered (left edge at -382.5px, right edge at +382.5px)  
    // New RPM bar: extends from -382.5px to +400px (screen edge) = 782.5px total
    // Keep left edge at -382.5px, extend only rightward
    const float bar_extension_ratio = 782.5f / 765.0f;
    int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);
    
    // Create the RPM bar gauge with extended range and rightward extension
    rpm_bar_gauge = lv_bar_create(parent_screen);
    lv_bar_set_range(rpm_bar_gauge, 0, extended_rpm_max);
    lv_bar_set_value(rpm_bar_gauge, 0, LV_ANIM_OFF);
    lv_obj_set_size(rpm_bar_gauge, 783, 55); // 782.5px rounded up to 783px
    
    // Position bar so left edge stays at -382.5px and extends to +400px (screen edge)
    // Left edge needs to be at -382.5px, so center should be at: -382.5 + (783/2) = 8.75px
    lv_obj_align(rpm_bar_gauge, LV_ALIGN_TOP_MID, 9, 2); // 8.75px rounded to 9px

    // Set styles for the RPM bar gauge (no gradient, solid color)
    lv_obj_set_style_radius(rpm_bar_gauge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(rpm_bar_gauge, lv_color_hex(0xF3F3F3), LV_PART_MAIN | LV_STATE_DEFAULT); // Light gray
    lv_obj_set_style_bg_opa(rpm_bar_gauge, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

	// Use the saved color for the RPM bar indicator
    lv_obj_set_style_radius(rpm_bar_gauge, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(rpm_bar_gauge, saved_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(rpm_bar_gauge, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    
    // Set up gradient for redline indicator if enabled
    if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
        // Set up gradient for redline indicator - transitions to light red in the last 1/7th
        lv_obj_set_style_bg_grad_color(rpm_bar_gauge, lv_color_hex(0xFF9999), LV_PART_INDICATOR | LV_STATE_DEFAULT); // Light red
        lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT); // Horizontal gradient
        // Set gradient stops: start at 6/7 (85.7%) with main color, end at 7/7 (100%) with light red
        lv_obj_set_style_bg_grad_stop(rpm_bar_gauge, 214, LV_PART_INDICATOR | LV_STATE_DEFAULT); // 214 = 85.7% of 255
    } else {
        // No gradient - set gradient color to same as main color for solid appearance
        lv_obj_set_style_bg_grad_color(rpm_bar_gauge, saved_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
  
    // Create redline zone rectangle (above RPM bar, below numbers/lines)
    rpm_redline_zone = lv_obj_create(parent_screen);
    lv_obj_set_height(rpm_redline_zone, 12); // Same height as the taller RPM lines
    lv_obj_set_y(rpm_redline_zone, -189); // Same Y position as RPM bar
    lv_obj_set_align(rpm_redline_zone, LV_ALIGN_CENTER);
    lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(rpm_redline_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(rpm_redline_zone, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT); // Bright red
    lv_obj_set_style_bg_opa(rpm_redline_zone, 180, LV_PART_MAIN | LV_STATE_DEFAULT); // Semi-transparent
    lv_obj_set_style_border_width(rpm_redline_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // Initial position and width will be set by update_redline_position()
    
    // Create large transparent click zone covering the entire extended RPM bar gauge
    lv_obj_t * rpm_click_zone = lv_obj_create(parent_screen);
    lv_obj_set_size(rpm_click_zone, 783, 55);  // Match extended RPM bar size
    lv_obj_align(rpm_click_zone, LV_ALIGN_TOP_MID, 9, 2);  // Match extended RPM bar position
    lv_obj_clear_flag(rpm_click_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(rpm_click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);  // Completely transparent
    lv_obj_set_style_border_opa(rpm_click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);  // No border
    lv_obj_add_flag(rpm_click_zone, LV_OBJ_FLAG_CLICKABLE);
    
    // Allocate memory to store RPM value_id and pass it to the event callback
    uint8_t *rpm_id_ptr = lv_mem_alloc(sizeof(uint8_t));
    *rpm_id_ptr = RPM_VALUE_ID;
    lv_obj_add_event_cb(rpm_click_zone, value_long_press_event_cb, LV_EVENT_LONG_PRESSED, rpm_id_ptr);
    lv_obj_add_event_cb(rpm_click_zone, free_value_id_event_cb, LV_EVENT_DELETE, rpm_id_ptr);
}

int num_rpm_lines = 0;
lv_obj_t* rpm_labels[MAX_RPM_LINES]; // Only need labels for the first set
lv_obj_t* rpm_lines[MAX_RPM_LINES * 2]; // Two sets of lines

void update_rpm_lines(lv_obj_t *parent) {
    // Delete existing lines and labels
    for (int i = 0; i < num_rpm_lines; i++) {
        if (rpm_lines[i] != NULL) {
            lv_obj_del(rpm_lines[i]);
            rpm_lines[i] = NULL;
        }
        if (i < MAX_RPM_LINES && rpm_labels[i] != NULL) {
            lv_obj_del(rpm_labels[i]);
            rpm_labels[i] = NULL;
        }
    }
    num_rpm_lines = 0;

    // Step in increments of 500 RPM for medium and main ticks
    int increments = 500; 
    int num_lines = (rpm_gauge_max / increments) + 1; // Include 0 RPM

    // Ensure we don't exceed MAX_RPM_LINES per set
    if (num_lines > MAX_RPM_LINES) {
        num_lines = MAX_RPM_LINES;
    }

    // Get the position and size of the RPM bar
    lv_coord_t bar_x = 18;
    lv_coord_t bar_y_set1 = 2;    // First set starts at px
    lv_coord_t bar_y_set2 = 44;   // Second set starts at px

    // For each tick, calculate its position for both sets
    for (int i = 0; i < num_lines; i++) {
        // Current RPM value for the tick
        int rpm_value = i * increments;

        // Calculate the x position based on rpm_value
        lv_coord_t x_pos = bar_x + ((rpm_value * 765) / rpm_gauge_max);

        // Decide which size line/tick to draw
        //    - Every 1000 RPM: main tick (width=3, height=13)
        //    - Every 500 RPM: medium tick (width=2, height=11)
        lv_coord_t line_width;
        lv_coord_t line_height;
        bool add_label = false;  // Only label the 1000s in the first set

        if ((rpm_value % 1000) == 0) {
            // Main tick
            line_width = 3;
            line_height = 12;
            add_label = true;
        } else {
            // Medium tick (500 RPM)
            line_width = 2;
            line_height = 8;
        }

        // Create the first set of lines (top row)
        lv_obj_t *line_top = lv_obj_create(parent);
        lv_obj_set_size(line_top, line_width, line_height);
        lv_obj_set_style_radius(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(line_top, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(line_top, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(line_top, LV_OBJ_FLAG_SCROLLABLE);

        // Position the line so it's centered horizontally on x_pos
        lv_coord_t adjusted_x = x_pos - (line_width / 2);
        lv_obj_set_pos(line_top, adjusted_x, bar_y_set1);

        rpm_lines[num_rpm_lines] = line_top;

        // Add a label for 1000 RPM ticks in the first set
        if (add_label) {
            lv_obj_t* label = lv_label_create(parent);

            // Display the "thousands" place (e.g., "7" for 7000)
            char rpm_str[8];
            snprintf(rpm_str, sizeof(rpm_str), "%d", rpm_value / 1000);
            lv_label_set_text(label, rpm_str);

            // Style the label
            lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN);
            lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_font(label, &ui_font_fugaz_17, LV_PART_MAIN | LV_STATE_DEFAULT);

            // Position the label below the line
            lv_obj_align_to(label, line_top, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);

            rpm_labels[num_rpm_lines] = label;
        }

        num_rpm_lines++;

        // Create the second set of lines (bottom row, flipped height)
        lv_obj_t* line_bottom = lv_obj_create(parent);
        lv_obj_set_size(line_bottom, line_width, line_height);
        lv_obj_set_style_radius(line_bottom, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(line_bottom, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(line_bottom, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(line_bottom, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(line_bottom, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(line_bottom, LV_OBJ_FLAG_SCROLLABLE);

        // Position the bottom line flat at the bottom with its height flipped
        lv_obj_set_pos(line_bottom, adjusted_x, bar_y_set2 + (13 - line_height));

        rpm_lines[num_rpm_lines] = line_bottom;

        num_rpm_lines++;

        // Stop if we exceed the maximum number of lines
        if (num_rpm_lines >= MAX_RPM_LINES * 2) {
            break;
        }
    }
}

// Create a transparent click zone and associate with value_id
static void create_transparent_click_zone(lv_obj_t * parent, lv_obj_t * target_label, uint8_t value_id) 
{
    lv_obj_t * click_zone = lv_obj_create(parent);
    lv_obj_set_size(click_zone, 60, 60);
    lv_obj_align_to(click_zone, target_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(click_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(click_zone, LV_OBJ_FLAG_CLICKABLE);

    // Allocate memory to store value_id and pass it to the event callback
    uint8_t *id_ptr = lv_mem_alloc(sizeof(uint8_t));
    *id_ptr = value_id;
    lv_obj_add_event_cb(click_zone, value_long_press_event_cb, LV_EVENT_LONG_PRESSED, id_ptr);
    lv_obj_add_event_cb(click_zone, free_value_id_event_cb, LV_EVENT_DELETE, id_ptr);
}

void create_warning_config_menu(uint8_t warning_idx) {
    init_common_style();
    
    // Allocate memory for input objects array - increase size to 7 to include the toggle dropdown
    lv_obj_t** input_objects = lv_mem_alloc(7 * sizeof(lv_obj_t*));
    if (!input_objects) {
        printf("Failed to allocate memory for input objects\n");
        return;
    }
    // Initialize all pointers to NULL
    for(int i = 0; i < 7; i++) {
        input_objects[i] = NULL;
    }

    // Allocate memory for preview objects
    lv_obj_t** preview_objects = lv_mem_alloc(2 * sizeof(lv_obj_t*));
    if (!preview_objects) {
        lv_mem_free(input_objects);
        printf("Failed to allocate memory for preview objects\n");
        return;
    }
    preview_objects[0] = NULL;
    preview_objects[1] = NULL;

    // Create the configuration screen
    lv_obj_t* config_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(config_screen, lv_color_hex(0x0000), 0);
    lv_obj_set_style_bg_opa(config_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(config_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Create save data structure
    warning_save_data_t* save_data = lv_mem_alloc(sizeof(warning_save_data_t));
    if (!save_data) {
        lv_mem_free(input_objects);
        lv_mem_free(preview_objects);
        printf("Failed to allocate memory for save data\n");
        return;
    }
    save_data->warning_idx = warning_idx;
    save_data->input_objects = input_objects;
    save_data->preview_objects = preview_objects;
  
    // Create main border/background panel
	lv_obj_t* main_border = lv_obj_create(config_screen);
	lv_obj_set_width(main_border, 780); 
	lv_obj_set_height(main_border, 325);
	lv_obj_set_align(main_border, LV_ALIGN_CENTER);
	lv_obj_set_y(main_border, 67);  // Vertical
	lv_obj_clear_flag(main_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(main_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(main_border, lv_color_hex(0x292C29), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(main_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(main_border, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	lv_obj_t * input_border = lv_obj_create(config_screen);
    lv_obj_set_width(input_border, 275);
    lv_obj_set_height(input_border, 310);
    lv_obj_set_x(input_border, -244);
    lv_obj_set_y(input_border, 67);
    lv_obj_set_align(input_border, LV_ALIGN_CENTER);
    lv_obj_clear_flag(input_border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(input_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(input_border, lv_color_hex(0x181818), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(input_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(input_border, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  
    // Create preview warning circle in exact Screen3 position
    lv_obj_t* preview_circle = lv_obj_create(config_screen);
    lv_obj_set_width(preview_circle, 15);
    lv_obj_set_height(preview_circle, 15);
    lv_obj_set_x(preview_circle, warning_positions[warning_idx].x);
    lv_obj_set_y(preview_circle, warning_positions[warning_idx].y);
    lv_obj_set_align(preview_circle, LV_ALIGN_CENTER);
    lv_obj_clear_flag(preview_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(preview_circle, 100, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(preview_circle, warning_configs[warning_idx].active_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(preview_circle, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(preview_circle, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Create preview warning label in exact Screen3 position
    lv_obj_t* preview_label = lv_label_create(config_screen);
    lv_obj_set_width(preview_label, 51);
    lv_obj_set_height(preview_label, 41);
    lv_obj_set_x(preview_label, warning_positions[warning_idx].x);
    lv_obj_set_y(preview_label, -112); // Same y-position as in Screen3
    lv_obj_set_align(preview_label, LV_ALIGN_CENTER);
    lv_label_set_text(preview_label, warning_configs[warning_idx].label);
    lv_obj_set_style_text_color(preview_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(preview_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(preview_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(preview_label, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);

    preview_objects[1] = preview_label;
  
    // Create the keyboard
    keyboard = lv_keyboard_create(config_screen);
    lv_obj_set_parent(keyboard, lv_layer_top());
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY, NULL);

    // Create title
    lv_obj_t* title = lv_label_create(config_screen);
    lv_label_set_text_fmt(title, "Warning %d Configuration", warning_idx + 1);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

	// Create a container for inputs
	lv_obj_t* inputs_container = lv_obj_create(config_screen);
	lv_obj_set_size(inputs_container, 800, 480);  // Adjusted size to fit within the border
	lv_obj_align(inputs_container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_opa(inputs_container, 0, 0);
	lv_obj_set_style_border_opa(inputs_container, 0, 0);
	lv_obj_clear_flag(inputs_container, LV_OBJ_FLAG_SCROLLABLE);

// Warning label input (moved to top)
lv_obj_t* label_text_label = lv_label_create(inputs_container);
lv_label_set_text(label_text_label, "Warning Label:");
lv_obj_set_width(label_text_label, 110);
lv_obj_set_style_text_align(label_text_label, LV_TEXT_ALIGN_LEFT, 0);
lv_obj_align(label_text_label, LV_ALIGN_CENTER, -312, -47);  // Was 73, now -47
lv_obj_set_style_text_color(label_text_label, lv_color_hex(0xCCCCCC), 0);

input_objects[3] = lv_textarea_create(inputs_container);
lv_obj_add_style(input_objects[3], &common_style, LV_PART_MAIN);
lv_textarea_set_one_line(input_objects[3], true);
lv_obj_set_width(input_objects[3], 120);
lv_obj_align(input_objects[3], LV_ALIGN_CENTER, -180, -47);  // Was 73, now -47
lv_obj_add_event_cb(input_objects[3], keyboard_event_cb, LV_EVENT_ALL, NULL);
lv_textarea_set_text(input_objects[3], warning_configs[warning_idx].label);

// CAN ID input (moved down)
lv_obj_t* can_id_label = lv_label_create(inputs_container);
lv_label_set_text(can_id_label, "CAN ID (hex):");
lv_obj_set_width(can_id_label, 110);
lv_obj_set_style_text_align(can_id_label, LV_TEXT_ALIGN_LEFT, 0);
lv_obj_align(can_id_label, LV_ALIGN_CENTER, -312, -7);  // Was -47, now -7
lv_obj_set_style_text_color(can_id_label, lv_color_hex(0xCCCCCC), 0);

input_objects[0] = lv_textarea_create(inputs_container);
lv_obj_add_style(input_objects[0], &common_style, LV_PART_MAIN);
lv_textarea_set_one_line(input_objects[0], true);
lv_obj_set_width(input_objects[0], 120);
lv_obj_align(input_objects[0], LV_ALIGN_CENTER, -180, -7);  // Was -47, now -7
lv_obj_add_event_cb(input_objects[0], keyboard_event_cb, LV_EVENT_ALL, NULL);
char can_id_text[32];
snprintf(can_id_text, sizeof(can_id_text), "%X", warning_configs[warning_idx].can_id);
lv_textarea_set_text(input_objects[0], can_id_text);

// Bit position dropdown
lv_obj_t* bit_pos_label = lv_label_create(inputs_container);
lv_label_set_text(bit_pos_label, "Bit Position:");
lv_obj_set_width(bit_pos_label, 110);
lv_obj_set_style_text_align(bit_pos_label, LV_TEXT_ALIGN_LEFT, 0);
lv_obj_align(bit_pos_label, LV_ALIGN_CENTER, -312, 33);  // Was -7, now 33
lv_obj_set_style_text_color(bit_pos_label, lv_color_hex(0xCCCCCC), 0);

input_objects[1] = lv_dropdown_create(inputs_container);
lv_obj_add_style(input_objects[1], &common_style, LV_PART_MAIN);
lv_dropdown_set_options(input_objects[1], 
    "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
    "16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
    "32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
    "48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63");
lv_obj_set_width(input_objects[1], 120);
lv_obj_align(input_objects[1], LV_ALIGN_CENTER, -180, 33);
lv_dropdown_set_selected(input_objects[1], warning_configs[warning_idx].bit_position);

// Highlighted color dropdown
lv_obj_t* color_label = lv_label_create(inputs_container);
lv_label_set_text(color_label, "Active Colour:");
lv_obj_set_width(color_label, 110);
lv_obj_set_style_text_align(color_label, LV_TEXT_ALIGN_LEFT, 0);
lv_obj_align(color_label, LV_ALIGN_CENTER, -312, 73);  // No change needed
lv_obj_set_style_text_color(color_label, lv_color_hex(0xCCCCCC), 0);

input_objects[4] = lv_dropdown_create(inputs_container);
lv_obj_add_style(input_objects[4], &common_style, LV_PART_MAIN);
lv_dropdown_set_options(input_objects[4], "Green\nBlue\nOrange\nRed\nYellow");
lv_obj_set_width(input_objects[4], 120);
lv_obj_align(input_objects[4], LV_ALIGN_CENTER, -180, 73);  // No change needed

// Set the current color selection based on the saved configuration
lv_color_t current_color = warning_configs[warning_idx].active_color;
uint8_t selected_color = 0; // Default to Green
if (current_color.full == lv_color_hex(0x0000FF).full) selected_color = 1; // Blue
else if (current_color.full == lv_color_hex(0xFFA500).full) selected_color = 2; // Orange
else if (current_color.full == lv_color_hex(0xFF0000).full) selected_color = 3; // Red
else if (current_color.full == lv_color_hex(0xFFFF00).full) selected_color = 4; // Yellow
lv_dropdown_set_selected(input_objects[4], selected_color);
                       
// Add Toggle Mode dropdown
lv_obj_t* toggle_mode_label = lv_label_create(inputs_container);
lv_label_set_text(toggle_mode_label, "Toggle Mode:");
lv_obj_set_width(toggle_mode_label, 110);
lv_obj_set_style_text_align(toggle_mode_label, LV_TEXT_ALIGN_LEFT, 0);
lv_obj_set_style_text_color(toggle_mode_label, lv_color_hex(0xCCCC), 0);
lv_obj_align(toggle_mode_label, LV_ALIGN_CENTER, -312, 113);

input_objects[5] = lv_dropdown_create(inputs_container);
lv_obj_add_style(input_objects[5], &common_style, LV_PART_MAIN);
lv_dropdown_set_options(input_objects[5], "On/Off\nMomentary");
lv_obj_set_width(input_objects[5], 120);
lv_obj_align(input_objects[5], LV_ALIGN_CENTER, -180, 113);
lv_dropdown_set_selected(input_objects[5], 
    warning_configs[warning_idx].is_momentary ? 1 : 0);

    // Save button
    lv_obj_t* save_btn = lv_btn_create(config_screen);
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
  
    lv_obj_add_event_cb(save_btn, save_warning_config_cb, LV_EVENT_CLICKED, save_data);

    lv_scr_load(config_screen);
}

// Creation and initializing Boxes and Arcs
static void init_boxes_and_arcs(void) {
    for(uint8_t i = 0; i < 8; i++) {
        // Create Box
        ui_Box[i] = lv_obj_create(ui_Screen3);
        lv_obj_set_size(ui_Box[i], 155, 92);
        lv_obj_set_pos(ui_Box[i], box_positions[i][0], box_positions[i][1]);
        lv_obj_set_align(ui_Box[i], LV_ALIGN_CENTER);
        lv_obj_clear_flag(ui_Box[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(ui_Box[i], &box_style, LV_PART_MAIN | LV_STATE_DEFAULT);

    }
}

/////////////////////////////////////////////	UI_SCREEN3__SCREEN_INIT	/////////////////////////////////////////////

void ui_Screen3_screen_init(void)
{
    // Initialize styles
    init_styles();
    init_common_style();
    
    // Initialize configurations
    init_values_config_defaults();
    init_warning_configs();
    // Note: indicator configs already initialized and loaded in main.c
    load_values_config_from_nvs();
    load_warning_configs_from_nvs();
    
    // Debug: Verify indicator configs are initialized
    printf("Indicator configs initialized - Left: CAN=0x%X, Bit=%d, Momentary=%d\n", 
           indicator_configs[0].can_id, indicator_configs[0].bit_position, indicator_configs[0].is_momentary);
    printf("Indicator configs initialized - Right: CAN=0x%X, Bit=%d, Momentary=%d\n", 
           indicator_configs[1].can_id, indicator_configs[1].bit_position, indicator_configs[1].is_momentary);
  
    ui_Screen3 = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Screen3, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Screen3, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Screen3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Create timers only if they don't already exist to prevent duplicates
    static bool timers_created = false;
    if (!timers_created) {
        // Create timer for RPM color updates
        lv_timer_create(check_rpm_color_update, 500, NULL);

            // Create timer for warning timeouts
    lv_timer_create(check_warning_timeouts, 50, NULL);
    
    // Create timer for CAN data timeouts (check every 1 second)
    lv_timer_create(check_can_timeouts, 1000, NULL);

        // Create timer for GPS speed updates - INCREASED to 100Hz for maximum responsiveness
        lv_timer_create(gps_speed_update_timer_cb, 50, NULL); // 10ms = 100Hz for instant response
        
        // Create timer for indicator animation (350ms = realistic car indicator timing)
        indicator_animation_timer = lv_timer_create(indicator_animation_timer_cb, 350, NULL);
        
        timers_created = true;
    }

	// Clear all object pointers before recreation to prevent race conditions
	for (int i = 0; i < 13; i++) {
		ui_Label[i] = NULL;
		ui_Value[i] = NULL;
		if (i < 8) ui_Box[i] = NULL;
		// CRITICAL: Reset previous_values to force UI updates after screen recreation
		// This ensures the first CAN message updates the display even if the value hasn't changed
		memset(previous_values[i], 0, sizeof(previous_values[i]));
	}
	ui_RPM_Value = NULL;
	ui_Speed_Value = NULL;
	ui_GEAR_Value = NULL;
	ui_Bar_1 = NULL;
	ui_Bar_2 = NULL;
	ui_Bar_1_Label = NULL;
	ui_Bar_2_Label = NULL;
	rpm_bar_gauge = NULL;
	
	// Set flag to reset CAN tracking variables on next CAN message
	reset_can_tracking = true;

	init_boxes_and_arcs();
	create_rpm_bar_gauge(ui_Screen3);
	update_rpm_lines(ui_Screen3);
	update_redline_position(); // Set initial redline position
	
    // Initialize labels, units, bars, and values
    for (uint8_t i = 0; i < 8; i++) {
        // Create label
        ui_Label[i] = lv_label_create(ui_Screen3);
        lv_label_set_text(ui_Label[i], label_texts[i]);
        lv_obj_set_style_text_color(ui_Label[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_Label[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_Label[i], &ui_font_fugaz_14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_x(ui_Label[i], label_positions[i][0]);
        lv_obj_set_y(ui_Label[i], label_positions[i][1]);
        lv_obj_set_align(ui_Label[i], LV_ALIGN_CENTER);

        // Create value label
        ui_Value[i] = lv_label_create(ui_Screen3);
        // Set initial value - "---" if panel is enabled (indicating no CAN data yet), "0" if disabled
        if (values_config[i].enabled) {
            lv_label_set_text(ui_Value[i], "---");
            strcpy(previous_values[i], "---"); // Initialize previous value to match
        } else {
            lv_label_set_text(ui_Value[i], "0");
        }
        lv_obj_set_style_text_color(ui_Value[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_Value[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_Value[i], &ui_font_Manrope_35_BOLD, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_Value[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(ui_Value[i], 140);
        lv_label_set_long_mode(ui_Value[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_x(ui_Value[i], value_positions[i][0]);
        lv_obj_set_y(ui_Value[i], value_positions[i][1]);
        lv_obj_set_align(ui_Value[i], LV_ALIGN_CENTER);

        // Create transparent click zone
        create_transparent_click_zone(ui_Screen3, ui_Value[i], i + 1);
    }

    ui_RPM_Value = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_RPM_Value, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_RPM_Value, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_RPM_Value, 0);
    lv_obj_set_y(ui_RPM_Value, -127);
    lv_obj_set_align(ui_RPM_Value, LV_ALIGN_CENTER);
    lv_label_set_text(ui_RPM_Value, "0");
    lv_obj_set_style_text_color(ui_RPM_Value, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_RPM_Value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_RPM_Value, &ui_font_fugaz_28, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_RPM_Label = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_RPM_Label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_RPM_Label, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_RPM_Label, 0);
    lv_obj_set_y(ui_RPM_Label, -164);
    lv_obj_set_align(ui_RPM_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_RPM_Label, "RPM");
    lv_obj_set_style_text_color(ui_RPM_Label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_RPM_Label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_RPM_Label, &ui_font_fugaz_14, LV_PART_MAIN | LV_STATE_DEFAULT);
  
    ui_Speed_Value = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_Speed_Value, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Speed_Value, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Speed_Value, LV_ALIGN_CENTER);
    lv_obj_set_x(ui_Speed_Value, 0);
    lv_obj_set_y(ui_Speed_Value, 30);
    // Set initial speed value - "---" if GPS selected (since no signal initially), "0" if CAN
    if (values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) {
        lv_label_set_text(ui_Speed_Value, "---");
    } else {
        lv_label_set_text(ui_Speed_Value, "0");
    }
    lv_obj_set_style_text_color(ui_Speed_Value, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Speed_Value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Speed_Value, &ui_font_fugaz_56, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Kmh = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_Kmh, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Kmh, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_Kmh, 37);
    lv_obj_set_y(ui_Kmh, 64);
    lv_obj_set_align(ui_Kmh, LV_ALIGN_CENTER);
    // Set units text based on saved configuration
    lv_label_set_text(ui_Kmh, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
    lv_obj_set_style_text_color(ui_Kmh, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Kmh, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Kmh, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Indicator_Left = lv_img_create(ui_Screen3);
    lv_img_set_src(ui_Indicator_Left, &ui_img_indicator_left_png);
    lv_obj_set_width(ui_Indicator_Left, LV_SIZE_CONTENT);   /// 32
    lv_obj_set_height(ui_Indicator_Left, LV_SIZE_CONTENT);    /// 30
    lv_obj_set_x(ui_Indicator_Left, -95);
    lv_obj_set_y(ui_Indicator_Left, -133);
    lv_obj_set_align(ui_Indicator_Left, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Indicator_Left, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags
    lv_obj_clear_flag(ui_Indicator_Left, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_opa(ui_Indicator_Left, 50, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Indicator_Right = lv_img_create(ui_Screen3);
    lv_img_set_src(ui_Indicator_Right, &ui_img_indicator_right_png);
    lv_obj_set_width(ui_Indicator_Right, LV_SIZE_CONTENT);   /// 32
    lv_obj_set_height(ui_Indicator_Right, LV_SIZE_CONTENT);    /// 30
    lv_obj_set_x(ui_Indicator_Right, 95);
    lv_obj_set_y(ui_Indicator_Right, -133);
    lv_obj_set_align(ui_Indicator_Right, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Indicator_Right, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags
    lv_obj_clear_flag(ui_Indicator_Right, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_opa(ui_Indicator_Right, 50, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Create transparent touch areas for indicators
    // Left indicator touch area
    lv_obj_t * left_indicator_touch_area = lv_obj_create(ui_Screen3);
    lv_obj_set_size(left_indicator_touch_area, 50, 50);
    lv_obj_set_x(left_indicator_touch_area, -95);
    lv_obj_set_y(left_indicator_touch_area, -133);
    lv_obj_set_align(left_indicator_touch_area, LV_ALIGN_CENTER);
    lv_obj_clear_flag(left_indicator_touch_area, LV_OBJ_FLAG_SCROLLABLE);
    
    // Make it transparent
    lv_obj_set_style_bg_opa(left_indicator_touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(left_indicator_touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Add long press handler for left indicator
    uint8_t *left_indicator_id = lv_mem_alloc(sizeof(uint8_t));
    if (left_indicator_id) {
        *left_indicator_id = 0;
        lv_obj_add_event_cb(left_indicator_touch_area, indicator_longpress_cb, LV_EVENT_LONG_PRESSED, left_indicator_id);
        printf("Left indicator touch area created with ID %d\n", *left_indicator_id);
    } else {
        printf("Error: Failed to allocate memory for left indicator ID\n");
    }

    // Right indicator touch area
    lv_obj_t * right_indicator_touch_area = lv_obj_create(ui_Screen3);
    lv_obj_set_size(right_indicator_touch_area, 50, 50);
    lv_obj_set_x(right_indicator_touch_area, 95);
    lv_obj_set_y(right_indicator_touch_area, -133);
    lv_obj_set_align(right_indicator_touch_area, LV_ALIGN_CENTER);
    lv_obj_clear_flag(right_indicator_touch_area, LV_OBJ_FLAG_SCROLLABLE);
    
    // Make it transparent
    lv_obj_set_style_bg_opa(right_indicator_touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(right_indicator_touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Add long press handler for right indicator
    uint8_t *right_indicator_id = lv_mem_alloc(sizeof(uint8_t));
    if (right_indicator_id) {
        *right_indicator_id = 1;
        lv_obj_add_event_cb(right_indicator_touch_area, indicator_longpress_cb, LV_EVENT_LONG_PRESSED, right_indicator_id);
        printf("Right indicator touch area created with ID %d\n", *right_indicator_id);
    } else {
        printf("Error: Failed to allocate memory for right indicator ID\n");
    }
  
	ui_Bar_1 = lv_bar_create(ui_Screen3);
    // Set default range if not configured
    if (values_config[BAR1_VALUE_ID - 1].bar_max <= values_config[BAR1_VALUE_ID - 1].bar_min) {
        values_config[BAR1_VALUE_ID - 1].bar_min = 0;
        values_config[BAR1_VALUE_ID - 1].bar_max = 100;
    }
    lv_bar_set_range(ui_Bar_1, 
        values_config[BAR1_VALUE_ID - 1].bar_min, 
        values_config[BAR1_VALUE_ID - 1].bar_max);
    lv_bar_set_value(ui_Bar_1, values_config[BAR1_VALUE_ID - 1].bar_min, LV_ANIM_OFF);
    lv_obj_set_width(ui_Bar_1, 300);
    lv_obj_set_height(ui_Bar_1, 30);
    lv_obj_set_x(ui_Bar_1, -240);
    lv_obj_set_y(ui_Bar_1, 209);
    lv_obj_set_align(ui_Bar_1, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_1, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Bar_1, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_Bar_1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Bar_1, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui_Bar_1, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_pad(ui_Bar_1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_radius(ui_Bar_1, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_1, lv_color_hex(0x38FF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);  //(0x19439a) = cold blue
    lv_obj_set_style_bg_opa(ui_Bar_1, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    ui_Bar_1_Label = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_Bar_1_Label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Bar_1_Label, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_Bar_1_Label, -240);
    lv_obj_set_y(ui_Bar_1_Label, 181);
    lv_obj_set_align(ui_Bar_1_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Bar_1_Label, label_texts[BAR1_VALUE_ID - 1]);
    lv_obj_set_style_text_color(ui_Bar_1_Label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Bar_1_Label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Bar_1_Label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Bar_1_Label, &ui_font_fugaz_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Bar_1_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_1_Label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_1_Label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_Bar_1_Label, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Bar_1_Label, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Bar_1_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Bar_1_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Bar_2 = lv_bar_create(ui_Screen3);
    // Set default range if not configured
    if (values_config[BAR2_VALUE_ID - 1].bar_max <= values_config[BAR2_VALUE_ID - 1].bar_min) {
        values_config[BAR2_VALUE_ID - 1].bar_min = 0;
        values_config[BAR2_VALUE_ID - 1].bar_max = 100;
    }
    lv_bar_set_range(ui_Bar_2, 
        values_config[BAR2_VALUE_ID - 1].bar_min, 
        values_config[BAR2_VALUE_ID - 1].bar_max);
    lv_bar_set_value(ui_Bar_2, values_config[BAR2_VALUE_ID - 1].bar_min, LV_ANIM_OFF);
    lv_obj_set_width(ui_Bar_2, 300);
    lv_obj_set_height(ui_Bar_2, 30);
    lv_obj_set_x(ui_Bar_2, 240);
    lv_obj_set_y(ui_Bar_2, 209);
    lv_obj_set_align(ui_Bar_2, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_2, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Bar_2, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_Bar_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Bar_2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui_Bar_2, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_pad(ui_Bar_2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_radius(ui_Bar_2, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_2, lv_color_hex(0x38FF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_2, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    ui_Bar_2_Label = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_Bar_2_Label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Bar_2_Label, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_Bar_2_Label, 240);
    lv_obj_set_y(ui_Bar_2_Label, 181);
    lv_obj_set_align(ui_Bar_2_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Bar_2_Label, label_texts[BAR2_VALUE_ID - 1]);
    lv_obj_set_style_text_color(ui_Bar_2_Label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Bar_2_Label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Bar_2_Label, &ui_font_fugaz_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Bar_2_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_2_Label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_2_Label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_Bar_2_Label, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Bar_2_Label, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Bar_2_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Bar_2_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Gear_Panel = lv_obj_create(ui_Screen3);
    lv_obj_set_width(ui_Gear_Panel, 90);
    lv_obj_set_height(ui_Gear_Panel, 90);
    lv_obj_set_x(ui_Gear_Panel, 0);
    lv_obj_set_y(ui_Gear_Panel, 180);
    lv_obj_set_align(ui_Gear_Panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Gear_Panel, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_Gear_Panel, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Gear_Panel, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Gear_Panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Gear_Panel, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_Gear_Panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Gear_Panel, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(ui_Gear_Panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(ui_Gear_Panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui_Gear_Panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_pad(ui_Gear_Panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Gear_Label = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_Gear_Label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Gear_Label, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_Gear_Label, 0);
    lv_obj_set_y(ui_Gear_Label, 152);
    lv_obj_set_align(ui_Gear_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Gear_Label, label_texts[GEAR_VALUE_ID - 1]); 
    lv_obj_set_style_text_color(ui_Gear_Label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Gear_Label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Gear_Label, &ui_font_fugaz_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Gear_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_Gear_Label, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Gear_Label, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Gear_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Gear_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_GEAR_Value = lv_label_create(ui_Screen3);
    lv_obj_set_width(ui_GEAR_Value, 115);
    lv_obj_set_height(ui_GEAR_Value, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_GEAR_Value, 10);
    lv_obj_set_y(ui_GEAR_Value, 198);
    lv_obj_set_align(ui_GEAR_Value, LV_ALIGN_CENTER);
    // Set initial gear value - "---" if gear is enabled (indicating no CAN data yet), " " if disabled
    if (values_config[GEAR_VALUE_ID - 1].enabled) {
        lv_label_set_text(ui_GEAR_Value, "---");
        strcpy(previous_values[GEAR_VALUE_ID - 1], "---"); // Initialize previous value to match
    } else {
        lv_label_set_text(ui_GEAR_Value, " ");
    }
    lv_obj_set_style_text_color(ui_GEAR_Value, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_GEAR_Value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_GEAR_Value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_GEAR_Value, &ui_font_Manrope_54_BOLD, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_zoom(ui_GEAR_Value, 210, LV_PART_MAIN | LV_STATE_DEFAULT);
  
    create_transparent_click_zone(ui_Screen3, ui_RPM_Value, RPM_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_Speed_Value, SPEED_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_GEAR_Value, GEAR_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_Bar_1, BAR1_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_Bar_2, BAR2_VALUE_ID);

    ui_RDM_Logo_Text = lv_img_create(ui_Screen3);
    lv_img_set_src(ui_RDM_Logo_Text, &ui_img_363163260);
    lv_obj_set_width(ui_RDM_Logo_Text, LV_SIZE_CONTENT);   /// 107
    lv_obj_set_height(ui_RDM_Logo_Text, LV_SIZE_CONTENT);    /// 40
    lv_obj_set_x(ui_RDM_Logo_Text, 0);
    lv_obj_set_y(ui_RDM_Logo_Text, -65);
    lv_obj_set_align(ui_RDM_Logo_Text, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_RDM_Logo_Text, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags
    lv_obj_clear_flag(ui_RDM_Logo_Text, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_add_event_cb(ui_RDM_Logo_Text, device_settings_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_flag(ui_RDM_Logo_Text, LV_OBJ_FLAG_CLICKABLE);  // Make sure it's clickable

    // In ui_Screen3_screen_init(), replace all individual warning creation with:
    for(int i = 0; i < 8; i++) {
        // Create warning circle
        warning_circles[i] = lv_obj_create(ui_Screen3);
        lv_obj_set_width(warning_circles[i], 15);
        lv_obj_set_height(warning_circles[i], 15);
        lv_obj_set_x(warning_circles[i], warning_positions[i].x);
        lv_obj_set_y(warning_circles[i], warning_positions[i].y);
        lv_obj_set_align(warning_circles[i], LV_ALIGN_CENTER);
        lv_obj_clear_flag(warning_circles[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(warning_circles[i], 100, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(warning_circles[i], lv_color_hex(0x292C29), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(warning_circles[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(warning_circles[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);

// Create warning label
warning_labels[i] = lv_label_create(ui_Screen3);
lv_obj_set_width(warning_labels[i], 51);
lv_obj_set_height(warning_labels[i], 41);
lv_obj_set_x(warning_labels[i], warning_positions[i].x);
lv_obj_set_y(warning_labels[i], -112);
lv_obj_set_align(warning_labels[i], LV_ALIGN_CENTER);
lv_obj_add_flag(warning_labels[i], LV_OBJ_FLAG_HIDDEN);

// Use the saved configuration label if it exists, otherwise use default
const char* saved_label = warning_configs[i].label;
if (saved_label && saved_label[0] != '\0') {
    // If there's a saved label, use it
    lv_label_set_text(warning_labels[i], saved_label);
} else {
    // Use default label if no saved configuration
    char label_text[20];
    snprintf(label_text, sizeof(label_text), "Warning\n%d", i + 1);
    lv_label_set_text(warning_labels[i], label_text);
}

lv_obj_set_style_text_color(warning_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_text_opa(warning_labels[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_text_align(warning_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_text_font(warning_labels[i], &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);

warning_configs[i].current_state = false;

        // Create transparent touch area
        lv_obj_t * touch_area = lv_obj_create(ui_Screen3);
        lv_obj_set_size(touch_area, 50,80);
        lv_obj_set_x(touch_area, warning_positions[i].x);
        lv_obj_set_y(touch_area, warning_positions[i].y);
        lv_obj_set_align(touch_area, LV_ALIGN_CENTER);
        lv_obj_clear_flag(touch_area, LV_OBJ_FLAG_SCROLLABLE);
      
        // Make it transparent
        lv_obj_set_style_bg_opa(touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
      
        // Add long press handler
        uint8_t *warning_id = lv_mem_alloc(sizeof(uint8_t));
        *warning_id = i;
        lv_obj_add_event_cb(touch_area, warning_longpress_cb, LV_EVENT_LONG_PRESSED, warning_id);
    }

    // Store references if needed
    ui_Warning_1 = warning_circles[0];
    ui_Warning_2 = warning_circles[1];
    ui_Warning_3 = warning_circles[2];
    ui_Warning_4 = warning_circles[3];
    ui_Warning_5 = warning_circles[4];
    ui_Warning_6 = warning_circles[5];
    ui_Warning_7 = warning_circles[6];
    ui_Warning_8 = warning_circles[7];

    // Create RPM lights circles if enabled (background layer)
    if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
        create_rpm_lights_circles(ui_Screen3);
    }
    
    // Start fuel update timer if any bar has fuel input enabled
    if (any_fuel_input_enabled()) {
        start_fuel_update_timer();
    }

    // Ensure indicators start with correct visual state (OFF unless bit is active)
    printf("Setting initial indicator states - Left: %s, Right: %s\n", 
           indicator_configs[0].current_state ? "ACTIVE" : "INACTIVE",
           indicator_configs[1].current_state ? "ACTIVE" : "INACTIVE");
    
    // Force update indicator UI to match current state
    for (int i = 0; i < 2; i++) {
        lv_obj_t* indicator_obj = (i == 0) ? ui_Indicator_Left : ui_Indicator_Right;
        if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
            if (indicator_configs[i].current_state) {
                // Active - set to 100% opacity
                lv_obj_set_style_opa(indicator_obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                printf("Indicator %d: Set to ACTIVE (100%% opacity)\n", i);
            } else {
                // Inactive - set to 50% opacity (default/dimmed state)
                lv_obj_set_style_opa(indicator_obj, 50, LV_PART_MAIN | LV_STATE_DEFAULT);
                printf("Indicator %d: Set to INACTIVE (50%% opacity)\n", i);
            }
        }
    }
}

// Interface function for other modules to refresh Screen3 labels
void refresh_screen3_labels(void) {
    printf("DEBUG: Refreshing Screen3 labels\n");
    
    // Update panel labels (1-8)
    for (int i = 0; i < 8; i++) {
        if (ui_Label[i] && lv_obj_is_valid(ui_Label[i])) {
            lv_label_set_text(ui_Label[i], label_texts[i]);
        }
    }
    
    // Update gear label
    if (ui_Gear_Label && lv_obj_is_valid(ui_Gear_Label)) {
        lv_label_set_text(ui_Gear_Label, label_texts[GEAR_VALUE_ID - 1]);
    }
    
    // Update bar labels
    if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
        lv_label_set_text(ui_Bar_1_Label, label_texts[BAR1_VALUE_ID - 1]);
    }
    if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
        lv_label_set_text(ui_Bar_2_Label, label_texts[BAR2_VALUE_ID - 1]);
    }
    
    printf("DEBUG: Screen3 labels refreshed\n");
}

// GPS speed update timer callback
void gps_speed_update_timer_cb(lv_timer_t * timer) {
    // Only update if GPS is selected as speed source
    if (!values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) {
        return;
    }
    
    char speed_str[EXAMPLE_MAX_CHAR_SIZE];
    static float last_gps_speed_value = -999.0f;  // Use float instead of string comparison
    static bool first_run = true;  // Force initial update
    
    // Check GPS data availability and fix status
    gps_data_t gps_data;
    bool gps_available = gps_get_data(&gps_data);
    
    if (gps_available && gps_data.fix_valid) {
        // GPS has valid fix - display actual speed with 0.1 precision for responsiveness
        float current_speed = gps_data.speed_kmh;
        
        // Convert to MPH if selected
        if (values_config[SPEED_VALUE_ID - 1].use_mph) {
            current_speed = current_speed * 0.621371f; // Convert KMH to MPH
        }
        
        // Update if speed changed by more than 0.1 units OR if this is the first reading
        if (fabsf(current_speed - last_gps_speed_value) >= 0.1f || last_gps_speed_value < 0) {
            last_gps_speed_value = current_speed;
            
            // Show integer for clean display but update more frequently
            snprintf(speed_str, sizeof(speed_str), "%.0f", current_speed);
            
            // Create speed update structure and send to UI immediately
            speed_update_t *s_upd = malloc(sizeof(speed_update_t));
            if (s_upd) {
                strcpy(s_upd->speed_str, speed_str);
                lv_async_call(update_speed_ui, s_upd);
            }
        }
    } else {
        // GPS selected but no valid fix or not available - display "---"
        if (first_run || last_gps_speed_value >= 0) {  // Update on first run or when transitioning from valid speed
            last_gps_speed_value = -999.0f;
            snprintf(speed_str, sizeof(speed_str), "---");
            
            speed_update_t *s_upd = malloc(sizeof(speed_update_t));
            if (s_upd) {
                strcpy(s_upd->speed_str, speed_str);
                lv_async_call(update_speed_ui, s_upd);
            }
        }
    }
    
    // Clear first run flag after first execution
    first_run = false;
}

void gear_ecu_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    // Indices: 0 = Custom, 1 = MaxxECU, 2 = Haltech

    if (selected == 0) {
        // ========== CUSTOM ==========
        printf("Gear ECU Presets: Custom (overriding device settings)\n");
        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 0;
        // Keep existing CAN ID settings for custom mode
        // Custom mode overrides device settings - save this choice
        save_values_config_to_nvs();
        ESP_LOGI("MENU", "Gear ECU set to Custom mode - device settings overridden");
    }
    else if (selected == 1) {
        // ========== MAXXECU ==========
        printf("Gear ECU Presets: MaxxECU\n");
        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 1;
        values_config[GEAR_VALUE_ID - 1].can_id       = 536;  // 0x218
        values_config[GEAR_VALUE_ID - 1].endianess    = 1;    // Little Endian
        values_config[GEAR_VALUE_ID - 1].bit_start    = 0;
        values_config[GEAR_VALUE_ID - 1].bit_length   = 16;
        values_config[GEAR_VALUE_ID - 1].scale        = 1.0f;
        values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
        values_config[GEAR_VALUE_ID - 1].decimals     = 0;

        // Update UI fields for GEAR (ID=11 => index=10)
        if (g_can_id_input[GEAR_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", values_config[GEAR_VALUE_ID - 1].can_id);
            lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
        }

        if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_endian_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].endianess);
        }

        if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_start_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].bit_start);
        }

        if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_length_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].bit_length - 1);
        }

        if (g_scale_input[GEAR_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[GEAR_VALUE_ID - 1].scale);
            lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
        }

        if (g_offset_input[GEAR_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[GEAR_VALUE_ID - 1].value_offset);
            lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
        }

        if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].decimals);
        }
    }
    else if (selected == 2) {
        // ========== HALTECH ==========
        printf("Gear ECU Presets: Haltech\n");
        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 2;
        // Note: Haltech doesn't have a standard gear CAN ID in the preconfig
        // Users will need to configure manually or we can add a common one
        values_config[GEAR_VALUE_ID - 1].can_id       = 0x370;  // Common Haltech gear CAN ID
        values_config[GEAR_VALUE_ID - 1].endianess    = 0;      // Big Endian
        values_config[GEAR_VALUE_ID - 1].bit_start    = 16;
        values_config[GEAR_VALUE_ID - 1].bit_length   = 16;
        values_config[GEAR_VALUE_ID - 1].scale        = 1.0f;
        values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
        values_config[GEAR_VALUE_ID - 1].decimals     = 0;

        // Update UI fields similarly
        if (g_can_id_input[GEAR_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", values_config[GEAR_VALUE_ID - 1].can_id);
            lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
        }

        if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_endian_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].endianess);
        }

        if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_start_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].bit_start);
        }

        if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_bit_length_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].bit_length - 1);
        }

        if (g_scale_input[GEAR_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[GEAR_VALUE_ID - 1].scale);
            lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
        }

        if (g_offset_input[GEAR_VALUE_ID - 1]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%g", values_config[GEAR_VALUE_ID - 1].value_offset);
            lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
        }

        if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
            lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
                                     values_config[GEAR_VALUE_ID - 1].decimals);
        }
    }

    // Save configuration to NVS immediately (moved outside custom check)
    save_values_config_to_nvs();
    ESP_LOGI("MENU", "Gear ECU preset changed to: %s (saved to NVS)", 
        selected == 0 ? "Custom" : (selected == 1 ? "MaxxECU" : "Haltech"));
    
    // Show/hide custom gear config button based on selected mode
    if (custom_gear_config_button != NULL && lv_obj_is_valid(custom_gear_config_button)) {
        if (selected == 0) {
            // Custom mode - show the button
            lv_obj_clear_flag(custom_gear_config_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            // MaxxECU or Haltech mode - hide the button
            lv_obj_add_flag(custom_gear_config_button, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // If button is invalid, reset the pointer
        custom_gear_config_button = NULL;
        ESP_LOGW("GEAR", "Custom gear button reference was invalid, reset to NULL");
    }
}

// Custom gear CAN ID input objects
static lv_obj_t * custom_gear_can_inputs[10] = {NULL}; // N, R, 1-8

void custom_gear_can_id_event_cb(lv_event_t * e) {
    lv_obj_t * textarea = lv_event_get_target(e);
    
    // Find which gear this input corresponds to
    int gear_index = -1;
    for (int i = 0; i < 10; i++) {
        if (custom_gear_can_inputs[i] == textarea) {
            gear_index = i;
            break;
        }
    }
    
    if (gear_index == -1) return;
    
    // Get the CAN ID value from the input
    const char* can_id_str = lv_textarea_get_text(textarea);
    uint32_t can_id = 0;
    
    // Parse CAN ID (support both decimal and hex)
    if (strncmp(can_id_str, "0x", 2) == 0 || strncmp(can_id_str, "0X", 2) == 0) {
        can_id = strtoul(can_id_str, NULL, 16);
    } else {
        can_id = strtoul(can_id_str, NULL, 10);
    }
    
    // Validate CAN ID range (0x000 to 0x7FF for standard CAN)
    if (can_id > 0x7FF) {
        can_id = 0x7FF;
        char corrected_str[16];
        snprintf(corrected_str, sizeof(corrected_str), "0x%03X", can_id);
        lv_textarea_set_text(textarea, corrected_str);
    }
    
    // Store the CAN ID
    values_config[GEAR_VALUE_ID - 1].gear_custom_can_ids[gear_index] = can_id;
    
    // Save to NVS
    save_values_config_to_nvs();
    
    // Log the change
    const char* gear_names[] = {"N", "R", "1", "2", "3", "4", "5", "6", "7", "8"};
    ESP_LOGI("GEAR", "Custom gear %s CAN ID set to: 0x%03X", gear_names[gear_index], can_id);
}

void create_custom_gear_config_menu(void) {
    // Create a new screen for custom gear configuration
    lv_obj_t * custom_gear_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(custom_gear_screen, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Title
    lv_obj_t * title_label = lv_label_create(custom_gear_screen);
    lv_label_set_text(title_label, "Custom Gear CAN ID Configuration");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_label, &ui_font_fugaz_14, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);
    
    // Instructions
    lv_obj_t * instructions = lv_label_create(custom_gear_screen);
    lv_label_set_text(instructions, "Set individual CAN IDs for each gear position.\nLeave blank (0x000) to disable a gear.");
    lv_obj_set_style_text_color(instructions, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(instructions, &lv_font_montserrat_10, 0);
    lv_obj_align(instructions, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_text_align(instructions, LV_TEXT_ALIGN_CENTER, 0);
    
    // Create gear configuration grid
    const char* gear_labels[] = {"Neutral (N)", "Reverse (R)", "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Gear 5", "Gear 6", "Gear 7", "Gear 8"};
    
    for (int i = 0; i < 10; i++) {
        int row = i / 2;
        int col = i % 2;
        
        // Gear label
        lv_obj_t * gear_label = lv_label_create(custom_gear_screen);
        lv_label_set_text(gear_label, gear_labels[i]);
        lv_obj_set_style_text_color(gear_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(gear_label, LV_ALIGN_TOP_LEFT, 50 + col * 350, 100 + row * 60);
        
        // CAN ID input
        custom_gear_can_inputs[i] = lv_textarea_create(custom_gear_screen);
        lv_textarea_set_one_line(custom_gear_can_inputs[i], true);
        lv_textarea_set_max_length(custom_gear_can_inputs[i], 8);
        lv_obj_set_width(custom_gear_can_inputs[i], 100);
        lv_obj_set_height(custom_gear_can_inputs[i], 35);
        lv_obj_align(custom_gear_can_inputs[i], LV_ALIGN_TOP_LEFT, 200 + col * 350, 95 + row * 60);
        lv_obj_add_style(custom_gear_can_inputs[i], get_common_style(), LV_PART_MAIN);
        
        // Set current value
        char current_value[16];
        uint32_t can_id = values_config[GEAR_VALUE_ID - 1].gear_custom_can_ids[i];
        if (can_id == 0) {
            strcpy(current_value, "0x000");
        } else {
            snprintf(current_value, sizeof(current_value), "0x%03X", can_id);
        }
        lv_textarea_set_text(custom_gear_can_inputs[i], current_value);
        
        // Add event callback
        lv_obj_add_event_cb(custom_gear_can_inputs[i], custom_gear_can_id_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(custom_gear_can_inputs[i], custom_gear_can_id_event_cb, LV_EVENT_DEFOCUSED, NULL);
    }
    
    // Back button
    lv_obj_t * back_btn = lv_btn_create(custom_gear_screen);
    lv_obj_set_size(back_btn, 100, 40);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, custom_gear_back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Save button
    lv_obj_t * save_btn = lv_btn_create(custom_gear_screen);
    lv_obj_set_size(save_btn, 100, 40);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
    lv_obj_t * save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, custom_gear_save_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Load the custom gear screen
    lv_scr_load(custom_gear_screen);
}

// Custom gear back button event callback
void custom_gear_back_btn_event_cb(lv_event_t * e) {
    ESP_LOGI("GEAR", "Returning to gear configuration menu");
    // Go back to the gear configuration menu
    load_menu_screen_for_value(GEAR_VALUE_ID);
}

// Custom gear save button event callback  
void custom_gear_save_btn_event_cb(lv_event_t * e) {
    ESP_LOGI("GEAR", "Saving custom gear configuration");
    // Save configuration to NVS
    save_values_config_to_nvs();
    // Go back to the gear configuration menu
    load_menu_screen_for_value(GEAR_VALUE_ID);
}

// Fuel input update timer
static lv_timer_t * fuel_update_timer = NULL;

// Fuel input timer callback
static void fuel_input_timer_cb(lv_timer_t * timer) {
    // Check both bars for fuel input
    for (int bar_id = BAR1_VALUE_ID; bar_id <= BAR2_VALUE_ID; bar_id++) {
        int config_index = bar_id - 1; // Convert to 0-based index
        int bar_index = bar_id - BAR1_VALUE_ID; // 0 for BAR1, 1 for BAR2
        
        // Only process if fuel input is enabled for this bar
        if (values_config[config_index].use_fuel_input) {
            // Read fuel level
            float current_voltage = fuel_input_read_voltage();
            float fuel_level = fuel_input_calculate_level(current_voltage, 
                                                        values_config[config_index].fuel_empty_voltage,
                                                        values_config[config_index].fuel_full_voltage);
            
            // Increased threshold to reduce sensitivity to small changes and reduce jitter
            const float FUEL_UPDATE_THRESHOLD = 2.0f; // Increased from BAR_UPDATE_THRESHOLD (1.0)
            
            // Check if value changed significantly to avoid unnecessary updates
            if (fabs(fuel_level - previous_bar_values[bar_index]) >= FUEL_UPDATE_THRESHOLD) {
                previous_bar_values[bar_index] = fuel_level;
                
                lv_obj_t *bar_obj = (bar_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;
                if (bar_obj) {
                    int32_t bar_value = (int32_t)fuel_level;
                    
                    // Clamp the value per configuration
                    if (bar_value < values_config[config_index].bar_min) {
                        bar_value = values_config[config_index].bar_min;
                    }
                    else if (bar_value > values_config[config_index].bar_max) {
                        bar_value = values_config[config_index].bar_max;
                    }

                    // Create and send bar update
                    bar_update_t *b_upd = malloc(sizeof(bar_update_t));
                    if (b_upd) {
                        b_upd->bar_index = bar_index;
                        b_upd->bar_value = bar_value;
                        b_upd->final_value = fuel_level;
                        b_upd->config_index = config_index;
                        lv_async_call(update_bar_ui, b_upd);
                    }
                }
            }
        }
    }
}

// Start fuel input update timer
void start_fuel_update_timer(void) {
    if (fuel_update_timer == NULL) {
        // Increased interval to 1 second to reduce CPU usage significantly
        fuel_update_timer = lv_timer_create(fuel_input_timer_cb, 1000, NULL); // Update every 1000ms
    }
}

// Stop fuel input update timer
void stop_fuel_update_timer(void) {
    if (fuel_update_timer != NULL) {
        lv_timer_del(fuel_update_timer);
        fuel_update_timer = NULL;
    }
}

// Check if any bar has fuel input enabled
bool any_fuel_input_enabled(void) {
    return values_config[BAR1_VALUE_ID - 1].use_fuel_input || 
           values_config[BAR2_VALUE_ID - 1].use_fuel_input;
}

// Timer callback for indicator animation
void indicator_animation_timer_cb(lv_timer_t* timer) {
    // Toggle animation state (realistic car indicator timing)
    indicator_animation_state = !indicator_animation_state;
    
    bool any_indicator_animating = false;
    
    // Update both indicators if they're active and have animation enabled
    for (int i = 0; i < 2; i++) {
        if (indicator_configs[i].current_state) {
            lv_obj_t* indicator_obj = (i == 0) ? ui_Indicator_Left : ui_Indicator_Right;
            if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
                if (indicator_configs[i].animation_enabled) {
                    // Flash between 100% and 50% opacity like a real indicator
                    uint8_t opacity = indicator_animation_state ? 255 : 128; // 100% or 50% (128 = 50% of 255)
                    lv_obj_set_style_opa(indicator_obj, opacity, LV_PART_MAIN | LV_STATE_DEFAULT);
                    any_indicator_animating = true;
                } else {
                    // Solid mode - always 100% opacity when active
                    lv_obj_set_style_opa(indicator_obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
    }
    
    // If no indicators are animating, pause the timer to save CPU
    if (!any_indicator_animating && indicator_animation_timer) {
        lv_timer_pause(indicator_animation_timer);
    }
}
          