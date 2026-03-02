#include "menu_screen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "lvgl.h"
#include "../screens/ui_Screen3.h"
#include "../ui.h"
#include "../ui_preconfig.h"
#include "../config/create_config_controls.h"
#include "../callbacks/ui_callbacks.h"

// External declaration for Smart_Car_Key image
extern const lv_img_dsc_t Smart_Car_Key;
#include "../menu/menu_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl_helpers.h"  // Add this for lvgl_mux
#include "../device_settings.h"
#include "nvs_flash.h"    // Add NVS support for timezone settings

// Reconfigure CAN acceptance filter after saving monitored IDs
extern void reconfigure_can_filter(void);

// Constants
#define EXAMPLE_MAX_CHAR_SIZE 64

// External references to global variables from ui_Screen3.c
extern lv_obj_t* ui_MenuScreen;
extern value_config_t values_config[13];
extern uint8_t current_value_id;
extern char previous_values[13][64];
extern float previous_bar_values[2];
extern SemaphoreHandle_t xGuiSemaphore;


// External references to UI objects
extern lv_obj_t* ui_Screen3;

// External references to update functions
extern void update_panel_ui(void *param);
extern void update_bar_ui(void *param);
extern void update_rpm_ui(void *param);
extern void update_speed_ui(void *param);
extern void update_gear_ui(void *param);

// External references to panel/bar/rpm/speed/gear update structures
typedef struct {
    uint8_t panel_index;
    char value_str[EXAMPLE_MAX_CHAR_SIZE];
    double final_value;
} panel_update_t;


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

// External references to constants
#define RPM_VALUE_ID 9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID 11
#define BAR1_VALUE_ID 12
#define BAR2_VALUE_ID 13

// External function references
extern void save_values_config_to_nvs();
extern void load_values_config_from_nvs();
extern void destroy_preconfig_menu();
extern void ui_Screen3_screen_init(void);
extern void update_rpm_lines(lv_obj_t *parent);

// Fuel timer management functions from ui_Screen3.c
extern void start_fuel_update_timer(void);
extern void stop_fuel_update_timer(void);
extern bool any_fuel_input_enabled(void);

// Forward declaration for timer callback
static void delete_old_screen_cb(lv_timer_t * timer);

// Custom text input event callback declaration
void custom_text_input_event_cb(lv_event_t * e);

// Custom gear values section
static lv_obj_t * custom_gear_values_container = NULL;
static lv_obj_t * custom_gear_value_inputs[14] = {NULL}; // P, R, N, D, 1-10
static lv_obj_t * custom_icon_inputs[7] = {NULL}; // Custom icons (KEY, etc.)
static lv_obj_t * custom_icon_type_dropdowns[7] = {NULL}; // Icon type dropdowns (KEY, etc.)
static lv_obj_t * custom_icon_images[7] = {NULL}; // Icon images (for KEY, etc.)

// Custom gear value functions
void create_custom_gear_values_section(lv_obj_t * parent, uint8_t gear_mode);
void hide_custom_gear_values_section(void);
void custom_icon_input_event_cb(lv_event_t * e);
void custom_gear_value_input_event_cb(lv_event_t * e);

// RPM limiter effect callback declarations
extern void rpm_limiter_effect_dropdown_event_cb(lv_event_t * e);
extern void rpm_limiter_roller_event_cb(lv_event_t * e);
extern void rpm_limiter_color_dropdown_event_cb(lv_event_t * e);
extern void rpm_lights_switch_event_cb(lv_event_t * e);
extern void rpm_background_switch_event_cb(lv_event_t * e);
extern void rpm_background_color_dropdown_event_cb(lv_event_t * e);
extern void rpm_background_threshold_roller_event_cb(lv_event_t * e);
extern void stop_limiter_effect_demo(void);
extern void create_rpm_lights_circles(lv_obj_t * parent);
extern void bar_low_color_event_cb(lv_event_t * e);
extern void bar_high_color_event_cb(lv_event_t * e);
extern void bar_in_range_color_event_cb(lv_event_t * e);
extern void show_value_switch_event_cb(lv_event_t * e);
extern void invert_value_switch_event_cb(lv_event_t * e);
extern void fuel_sender_switch_event_cb(lv_event_t * e);
extern void fuel_sender_ctx_free_event_cb(lv_event_t * e);
extern void fs_empty_btn_event_cb(lv_event_t * e);
extern void fs_full_btn_event_cb(lv_event_t * e);
extern void fs_empty_v_input_event_cb(lv_event_t * e);
extern void fs_full_v_input_event_cb(lv_event_t * e);
static void fs_voltage_update_timer_cb(lv_timer_t * timer);
static void fs_filter_slider_event_cb(lv_event_t * e);

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

// External references
extern lv_obj_t *ui_MenuScreen;
extern lv_obj_t *keyboard;
extern lv_obj_t *config_bars[];
extern lv_style_t common_style;
extern lv_style_t box_style;
extern char label_texts[13][64];
extern lv_obj_t *rpm_bar_gauge;
extern int rpm_gauge_max;

// Import correct UI object references from ui_Screen3.c
extern lv_obj_t *ui_Label[];
extern lv_obj_t *ui_Value[];
extern lv_obj_t *ui_Box[];
extern lv_obj_t *ui_Bar_1_Value;
extern lv_obj_t *ui_Bar_2_Value;
extern lv_obj_t *ui_RPM_Value;
extern lv_obj_t *ui_Speed_Value;
extern lv_obj_t *ui_Kmh;
extern lv_obj_t *ui_Gear_Label;
extern lv_obj_t *ui_Bar_1;
extern lv_obj_t *ui_Bar_2;
extern lv_obj_t *ui_Bar_1_Label;
extern lv_obj_t *ui_Bar_2_Label;

// Global variables for menu components
lv_obj_t * custom_gear_config_button = NULL;    // Track the custom gear config button
lv_obj_t * menu_rpm_value_label = NULL;         // Track the RPM value label in menu for demo updates

// Global variables for menu preview objects that need live updates
lv_obj_t * menu_speed_value_label = NULL;       // Speed value label in menu for live updates
lv_obj_t * menu_speed_units_label = NULL;       // Speed units label in menu for live updates
lv_obj_t * menu_gear_value_label = NULL;        // Gear value label in menu for live updates
lv_obj_t * menu_gear_icon = NULL;                // Gear icon in menu for custom icon display
lv_obj_t * menu_panel_value_labels[8] = {NULL}; // Panel value labels in menu for live updates
lv_obj_t * menu_panel_boxes[8] = {NULL}; // Panel boxes in menu for border effects
lv_obj_t * menu_panel_labels[8] = {NULL}; // Panel text labels in menu for label updates
lv_obj_t * menu_bar_objects[2] = {NULL};        // Bar objects in menu for live updates
lv_obj_t * menu_bar_labels[2] = {NULL};         // Bar text labels in menu for label updates

static void delete_old_screen_cb(lv_timer_t * timer) {
    lv_obj_t * screen = (lv_obj_t *)timer->user_data;
    if (screen) {
        destroy_preconfig_menu();
        lv_obj_del(screen);
    }
}

void close_menu_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // Stop any active demos to prevent crashes
    stop_limiter_effect_demo();
    
    // Clear menu RPM value label reference
    menu_rpm_value_label = NULL;

    // Store current screen for cleanup
    lv_obj_t * old_screen = lv_scr_act();

    // First, try to save configuration with timeout protection
    ESP_LOGI("menu_screen", "Attempting to save configuration...");
    
    // Before saving, explicitly read all custom gear and icon input values
    // This ensures values like "0" are saved even if input events haven't fired
    // Note: custom_gear_value_inputs and custom_icon_inputs are static arrays in this file
    
    // Read all custom gear values
    for (int i = 0; i < 14; i++) {
        if (custom_gear_value_inputs[i] && lv_obj_is_valid(custom_gear_value_inputs[i])) {
            const char* value_str = lv_textarea_get_text(custom_gear_value_inputs[i]);
            uint32_t gear_value = UINT32_MAX;
            if (value_str != NULL && strlen(value_str) > 0) {
                if (strncmp(value_str, "0x", 2) == 0 || strncmp(value_str, "0X", 2) == 0) {
                    gear_value = strtoul(value_str, NULL, 16);
                } else {
                    gear_value = strtoul(value_str, NULL, 10);
                }
            }
            values_config[GEAR_VALUE_ID - 1].gear_custom_values[i] = gear_value;
        }
    }
    
    // Read all custom icon values
    for (int i = 0; i < 7; i++) {
        if (custom_icon_inputs[i] && lv_obj_is_valid(custom_icon_inputs[i])) {
            const char* value_str = lv_textarea_get_text(custom_icon_inputs[i]);
            uint32_t icon_value = UINT32_MAX;
            if (value_str != NULL && strlen(value_str) > 0) {
                if (strncmp(value_str, "0x", 2) == 0 || strncmp(value_str, "0X", 2) == 0) {
                    icon_value = strtoul(value_str, NULL, 16);
                } else {
                    icon_value = strtoul(value_str, NULL, 10);
                }
            }
            values_config[GEAR_VALUE_ID - 1].custom_icon_values[i] = icon_value;
        }
    }
    
    // Disable the button to prevent double-clicks
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    
    // Create a simple loading indicator
    lv_obj_t * loading_label = lv_label_create(old_screen);
    lv_label_set_text(loading_label, "Saving...");
    lv_obj_align(loading_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(loading_label, lv_color_hex(0xFFFFFF), 0);
    
    // Force UI update to show the loading text
    lv_refr_now(NULL);
    
    // Save configuration with error handling
    bool save_successful = true;
    
    // Try to save with a reasonable timeout mechanism
    uint32_t save_start_time = esp_timer_get_time() / 1000;
    
    // Call save function (this already has its own mutex handling)
    save_values_config_to_nvs();
    // Rebuild fast dispatch and apply TWAI acceptance filter based on new configuration
    rebuild_can_dispatch();
    reconfigure_can_filter();
    
    uint32_t save_duration = (esp_timer_get_time() / 1000) - save_start_time;
    ESP_LOGI("menu_screen", "Save operation took %lu ms", save_duration);
    
    // Small delay to ensure save completion
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Remove loading indicator
    lv_obj_del(loading_label);
    
    // Only proceed with screen transition if save was successful
    if (save_successful) {
    // Create and initialize new screen before deleting old one
    ui_Screen3 = lv_obj_create(NULL);
    if (!ui_Screen3) {
        ESP_LOGE("menu_screen", "Failed to create new screen");
        // Re-enable button and return
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
        return;
    }

    // Initialize the new screen (this will load the updated configuration)
    ui_Screen3_screen_init();

        // Load new screen with fade animation
        lv_scr_load_anim(ui_Screen3, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);

        // Clean up old screen after a longer delay to ensure smooth transition
        lv_timer_t * del_timer = lv_timer_create(delete_old_screen_cb, 300, old_screen);
        lv_timer_set_repeat_count(del_timer, 1);

    } else {
        ESP_LOGE("menu_screen", "Save operation failed, staying on current screen");
        // Re-enable button so user can try again
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    }

    // Clear menu preview object references to prevent accessing invalid objects
    menu_speed_value_label = NULL;
    menu_speed_units_label = NULL;
    menu_gear_value_label = NULL;
    menu_gear_icon = NULL;
    for (int i = 0; i < 8; i++) {
        menu_panel_value_labels[i] = NULL;
        menu_panel_boxes[i] = NULL;
        menu_panel_labels[i] = NULL;
    }
    for (int i = 0; i < 2; i++) {
        menu_bar_objects[i] = NULL;
        menu_bar_labels[i] = NULL;
    }
}

void cancel_menu_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // Stop any active demos to prevent crashes
    stop_limiter_effect_demo();
    
    // Clear menu RPM value label reference
    menu_rpm_value_label = NULL;

    // Store current screen for cleanup
    lv_obj_t * old_screen = lv_scr_act();

    ESP_LOGI("menu_screen", "Canceling changes and reloading from NVS...");
    
    // Disable the button to prevent double-clicks
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    
    // Create a simple loading indicator
    lv_obj_t * loading_label = lv_label_create(old_screen);
    lv_label_set_text(loading_label, "Canceling...");
    lv_obj_align(loading_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(loading_label, lv_color_hex(0xFFFFFF), 0);
    
    // Force UI update to show the loading text
    lv_refr_now(NULL);
    
    // Load previous configuration from NVS (this will overwrite any unsaved changes)
    load_values_config_from_nvs();
    
    ESP_LOGI("menu_screen", "Configuration reloaded from NVS");
    
    // Small delay to show the loading message
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Remove loading indicator
    lv_obj_del(loading_label);
    
    // Create and initialize new screen before deleting old one
    ui_Screen3 = lv_obj_create(NULL);
    if (!ui_Screen3) {
        ESP_LOGE("menu_screen", "Failed to create new screen");
        // Re-enable button and return
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
        return;
    }

    // Initialize the new screen (this will load the reloaded configuration)
    ui_Screen3_screen_init();

    // Load new screen with fade animation
    lv_scr_load_anim(ui_Screen3, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);

    // Clean up old screen after a delay to ensure smooth transition
    lv_timer_t * del_timer = lv_timer_create(delete_old_screen_cb, 300, old_screen);
    lv_timer_set_repeat_count(del_timer, 1);

    // Clear menu preview object references to prevent accessing invalid objects
    menu_speed_value_label = NULL;
    menu_speed_units_label = NULL;
    menu_gear_value_label = NULL;
    menu_gear_icon = NULL;
    for (int i = 0; i < 8; i++) {
        menu_panel_value_labels[i] = NULL;
        menu_panel_boxes[i] = NULL;
        menu_panel_labels[i] = NULL;
    }
    for (int i = 0; i < 2; i++) {
        menu_bar_objects[i] = NULL;
        menu_bar_labels[i] = NULL;
    }
}

// Load menu screen
void load_menu_screen_for_value(uint8_t value_id) {
    current_value_id = value_id;
    
    // Clean up any existing objects
    destroy_preconfig_menu();
    
    // Reset global button pointer to prevent accessing stale objects
    custom_gear_config_button = NULL;
    
    // Create new menu screen
    ui_MenuScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_MenuScreen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_MenuScreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui_MenuScreen, LV_OBJ_FLAG_SCROLLABLE);

    // Create the keyboard
    keyboard = lv_keyboard_create(ui_MenuScreen);
    lv_obj_set_parent(keyboard, lv_layer_top());
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY, NULL);

    // Create Save button (positioned left)
    lv_obj_t * save_btn = lv_btn_create(ui_MenuScreen);
    lv_obj_t * save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN); // Green for save
    lv_obj_set_align(save_btn, LV_ALIGN_CENTER);
    lv_obj_set_pos(save_btn, 200, 200);
    lv_obj_center(save_label);

    // Create Cancel button (positioned right)
    lv_obj_t * close_btn = lv_btn_create(ui_MenuScreen);
    lv_obj_t * close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Cancel");
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xF44336), LV_PART_MAIN); // Red for cancel
    lv_obj_set_align(close_btn, LV_ALIGN_CENTER);
    lv_obj_set_pos(close_btn, 300, 200);
    lv_obj_center(close_label);

    if (value_id >= 1 && value_id <= 8) {
        create_menu_objects(ui_MenuScreen, value_id);
      
    // Create background boxes for customization section
    lv_obj_t * customization_box_1 = lv_obj_create(ui_MenuScreen);
    lv_obj_set_width(customization_box_1, 194);
    lv_obj_set_height(customization_box_1, 155);
    lv_obj_set_pos(customization_box_1, -18, -23);
    lv_obj_set_align(customization_box_1, LV_ALIGN_CENTER);
    lv_obj_clear_flag(customization_box_1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(customization_box_1, lv_color_hex(0x181818), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(customization_box_1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(customization_box_1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(customization_box_1, 7, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * customization_box_2 = lv_obj_create(ui_MenuScreen);
    lv_obj_set_width(customization_box_2, 194);
    lv_obj_set_height(customization_box_2, 155);
    lv_obj_set_pos(customization_box_2, -18, 137);
    lv_obj_set_align(customization_box_2, LV_ALIGN_CENTER);
    lv_obj_clear_flag(customization_box_2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(customization_box_2, lv_color_hex(0x181818), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(customization_box_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(customization_box_2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(customization_box_2, 7, LV_PART_MAIN | LV_STATE_DEFAULT);

     // High Warning Threshold
    lv_obj_t * warning_high_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_high_label, "Value:");
    lv_obj_set_style_text_color(warning_high_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_high_label, LV_ALIGN_CENTER, -78, 113);

    lv_obj_t * warning_high_input = lv_textarea_create(ui_MenuScreen);
    lv_obj_add_style(warning_high_input, get_common_style(), LV_PART_MAIN);
    lv_textarea_set_one_line(warning_high_input, true);
    lv_textarea_set_placeholder_text(warning_high_input, "Range High");
    lv_obj_set_width(warning_high_input, 100);
    lv_obj_align(warning_high_input, LV_ALIGN_CENTER, 12, 113);
    lv_obj_add_event_cb(warning_high_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // High Warning Color
    lv_obj_t * warning_high_color_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_high_color_label, "Colour:");
    lv_obj_set_style_text_color(warning_high_color_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_high_color_label, LV_ALIGN_CENTER, -78, 153);

    lv_obj_t * warning_high_color_dd = lv_dropdown_create(ui_MenuScreen);
    lv_dropdown_set_options(warning_high_color_dd, "Red\nBlue");
    lv_obj_set_width(warning_high_color_dd, 100);
    lv_obj_align(warning_high_color_dd, LV_ALIGN_CENTER, 12, 153);
    lv_obj_add_style(warning_high_color_dd, get_common_style(), LV_PART_MAIN);

    // Range Low Label
    lv_obj_t * range_low_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(range_low_label, "Range Low");
    lv_obj_set_style_text_color(range_low_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(range_low_label, LV_ALIGN_CENTER, -18, -87);

    // Low Warning Threshold
    lv_obj_t * warning_low_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_low_label, "Value:");
    lv_obj_set_style_text_color(warning_low_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_low_label, LV_ALIGN_CENTER, -78, -47);

    lv_obj_t * warning_low_input = lv_textarea_create(ui_MenuScreen);
    lv_obj_add_style(warning_low_input, get_common_style(), LV_PART_MAIN);
    lv_textarea_set_one_line(warning_low_input, true);
    lv_textarea_set_placeholder_text(warning_low_input, "Range Low");
    lv_obj_set_width(warning_low_input, 100);
    lv_obj_align(warning_low_input, LV_ALIGN_CENTER, 12, -47);
    lv_obj_add_event_cb(warning_low_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // Low Warning Color
    lv_obj_t * warning_low_color_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_low_color_label, "Colour:");
    lv_obj_set_style_text_color(warning_low_color_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_low_color_label, LV_ALIGN_CENTER, -78, -7);

    lv_obj_t * warning_low_color_dd = lv_dropdown_create(ui_MenuScreen);
    lv_dropdown_set_options(warning_low_color_dd, "Red\nBlue");
    lv_obj_set_width(warning_low_color_dd, 100);
    lv_obj_align(warning_low_color_dd, LV_ALIGN_CENTER, 12, -7);
    lv_obj_add_style(warning_low_color_dd, get_common_style(), LV_PART_MAIN);

    // Range High Label
    lv_obj_t * range_high_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(range_high_label, "Range High");
    lv_obj_set_style_text_color(range_high_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(range_high_label, LV_ALIGN_CENTER, -18, 73);

    // Display Unit Input (moved to far right of customization)
    lv_obj_t * display_unit_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(display_unit_label, "Display Unit:");
    lv_obj_set_style_text_color(display_unit_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(display_unit_label, LV_ALIGN_CENTER, 220, -47);

    lv_obj_t * display_unit_input = lv_textarea_create(ui_MenuScreen);
    lv_obj_add_style(display_unit_input, get_common_style(), LV_PART_MAIN);
    lv_textarea_set_one_line(display_unit_input, true);
    lv_obj_set_width(display_unit_input, 80);
    lv_obj_align(display_unit_input, LV_ALIGN_CENTER, 320, -47);
    lv_obj_add_event_cb(display_unit_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
    
    // Set current custom text
    lv_textarea_set_text(display_unit_input, values_config[value_id - 1].custom_text);

    // Set current values
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", values_config[value_id - 1].warning_high_threshold);
    lv_textarea_set_text(warning_high_input, buf);
    snprintf(buf, sizeof(buf), "%.2f", values_config[value_id - 1].warning_low_threshold);
    lv_textarea_set_text(warning_low_input, buf);

    // Set current colors
    bool is_high_red = values_config[value_id - 1].warning_high_color.full == lv_color_hex(0xFF0000).full;
    bool is_low_red = values_config[value_id - 1].warning_low_color.full == lv_color_hex(0xFF0000).full;
    lv_dropdown_set_selected(warning_high_color_dd, is_high_red ? 0 : 1);
    lv_dropdown_set_selected(warning_low_color_dd, is_low_red ? 0 : 1);

    // Add event handlers
    uint8_t *id_ptr = lv_mem_alloc(sizeof(uint8_t));
    *id_ptr = value_id;
  
    lv_obj_add_event_cb(warning_high_input, warning_high_threshold_event_cb, LV_EVENT_VALUE_CHANGED, id_ptr);
    lv_obj_add_event_cb(warning_low_input, warning_low_threshold_event_cb, LV_EVENT_VALUE_CHANGED, id_ptr);
    lv_obj_add_event_cb(warning_high_color_dd, warning_high_color_event_cb, LV_EVENT_VALUE_CHANGED, id_ptr);
    lv_obj_add_event_cb(warning_low_color_dd, warning_low_color_event_cb, LV_EVENT_VALUE_CHANGED, id_ptr);
    
    // Add display unit event handler
    uint8_t *display_unit_id_ptr = lv_mem_alloc(sizeof(uint8_t));
    *display_unit_id_ptr = value_id;
    lv_obj_add_event_cb(display_unit_input, custom_text_input_event_cb, LV_EVENT_VALUE_CHANGED, display_unit_id_ptr);

    } else if (value_id == RPM_VALUE_ID) {
        // RPM value
        const char *current_rpm_value = lv_label_get_text(ui_RPM_Value);
        create_config_controls(ui_MenuScreen, RPM_VALUE_ID);
        create_rpm_bar_gauge(ui_MenuScreen);
		update_rpm_lines(ui_MenuScreen);
		update_redline_position(); // Ensure redline is positioned correctly in menu
		
		// Create RPM lights circles if enabled (for demos)
		if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
		    create_rpm_lights_circles(ui_MenuScreen);
		}

        // Create RPM label
        lv_obj_t * rpm_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(rpm_label, "RPM");
        // Apply same styling as on Screen3 for RPM label
        lv_obj_set_style_text_color(rpm_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(rpm_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(rpm_label, &ui_font_fugaz_14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align_to(rpm_label, ui_RPM_Label, LV_ALIGN_CENTER, 0, 0);

        // Create RPM value
        menu_rpm_value_label = lv_label_create(ui_MenuScreen);
        lv_obj_set_width(menu_rpm_value_label, 100);  // Set fixed width for proper centering
        lv_obj_set_height(menu_rpm_value_label, LV_SIZE_CONTENT);
        lv_label_set_text(menu_rpm_value_label, current_rpm_value);
        // Apply same styling as on Screen3 for RPM value
        lv_obj_set_style_text_color(menu_rpm_value_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(menu_rpm_value_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(menu_rpm_value_label, &ui_font_fugaz_28, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(menu_rpm_value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align_to(menu_rpm_value_label, ui_RPM_Value, LV_ALIGN_CENTER, 0, 0);
      
    	lv_obj_t * max_rpm_text = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(max_rpm_text, "RPM Gauge:");
    	lv_obj_set_style_text_color(max_rpm_text, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(max_rpm_text, LV_ALIGN_CENTER, 220, -87);
   		lv_obj_set_style_text_align(max_rpm_text, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    	
		// RPM Gauge dropdown (200 RPM steps)
		lv_obj_t * rpm_gauge_roller = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(rpm_gauge_roller, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(rpm_gauge_roller,
		    "3000\n3200\n3400\n3600\n3800\n4000\n4200\n4400\n4600\n4800\n"
		    "5000\n5200\n5400\n5600\n5800\n6000\n6200\n6400\n6600\n6800\n"
		    "7000\n7200\n7400\n7600\n7800\n8000\n8200\n8400\n8600\n8800\n"
		    "9000\n9200\n9400\n9600\n9800\n10000\n10200\n10400\n10600\n10800\n"
		    "11000\n11200\n11400\n11600\n11800\n12000");
		uint16_t selected_index = 0;
		if (rpm_gauge_max >= 3000 && rpm_gauge_max <= 12000) {
		    selected_index = (rpm_gauge_max - 3000) / 200;
		}
		lv_dropdown_set_selected(rpm_gauge_roller, selected_index);
		lv_obj_set_width(rpm_gauge_roller, 90);
		lv_obj_align(rpm_gauge_roller, LV_ALIGN_CENTER, 320, -87);
		lv_obj_add_event_cb(rpm_gauge_roller, rpm_gauge_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
		
		lv_obj_t * rpm_ecu_dropdown_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(rpm_ecu_dropdown_label, "ECU Presets:");
    	lv_obj_set_style_text_color(rpm_ecu_dropdown_label, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(rpm_ecu_dropdown_label, LV_ALIGN_CENTER, -50, -87);
    	
    	lv_obj_t * rpm_ecu_dropdown = lv_dropdown_create(ui_MenuScreen);
    	// Provide the four options
    	lv_dropdown_set_options(rpm_ecu_dropdown, "Custom\nMaxxECU\nHaltech\nFord BA/BF/FG");
    	lv_obj_align(rpm_ecu_dropdown, LV_ALIGN_CENTER, 80, -87);
    	lv_obj_set_width(rpm_ecu_dropdown, 120);
    	lv_obj_add_style(rpm_ecu_dropdown, get_common_style(), LV_PART_MAIN);

    	// Auto-select the saved ECU preconfig
    	uint8_t saved_ecu = get_selected_ecu_preconfig();
    	lv_dropdown_set_selected(rpm_ecu_dropdown, saved_ecu);
    	
    	// Attach an event callback so we can detect selection changes
    	lv_obj_add_event_cb(rpm_ecu_dropdown, rpm_ecu_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    	
    	// RPM Color dropdown
		lv_obj_t * rpm_color_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(rpm_color_label, "RPM Colour:");
		lv_obj_set_style_text_color(rpm_color_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(rpm_color_label, LV_ALIGN_CENTER, -50, -47); // 40px below ECU presets (-87 + 40)
		
		lv_obj_t * rpm_color_dropdown = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(rpm_color_dropdown, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(rpm_color_dropdown, 
		    "Green\n"
		    "Light Blue\n"
		    "Yellow\n"
		    "Orange\n"
		    "Red\n"
		    "Dark Blue\n"
		    "Purple\n"
		    "Magenta\n"
		    "Pink\n"
		    "Custom");
		lv_obj_set_width(rpm_color_dropdown, 120); // Same width as ECU presets
		lv_obj_align(rpm_color_dropdown, LV_ALIGN_CENTER, 80, -47);

		// Add event handler
		lv_obj_add_event_cb(rpm_color_dropdown, rpm_color_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
		uint16_t saved_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color.full;

		// Set selected index based on current color (default to Custom if no match)
		uint16_t color_selected_index = 9; // Default to Custom
		if (saved_color == lv_color_hex(0x00FF00).full) color_selected_index = 0; // Green
		else if (saved_color == lv_color_hex(0x00FFFF).full) color_selected_index = 1; // Light Blue
		else if (saved_color == lv_color_hex(0xFFFF00).full) color_selected_index = 2; // Yellow
		else if (saved_color == lv_color_hex(0xFF7F00).full) color_selected_index = 3; // Orange
		else if (saved_color == lv_color_hex(0xFF0000).full) color_selected_index = 4; // Red
		else if (saved_color == lv_color_hex(0x0080FF).full) color_selected_index = 5; // Dark Blue
		else if (saved_color == lv_color_hex(0x8000FF).full) color_selected_index = 6; // Purple
		else if (saved_color == lv_color_hex(0xFF00FF).full) color_selected_index = 7; // Magenta
		else if (saved_color == lv_color_hex(0xFF1493).full) color_selected_index = 8; // Pink
		// If none of the predefined colors match, it's a custom color (index 9)
		lv_dropdown_set_selected(rpm_color_dropdown, color_selected_index);

		// Add Redline RPM Roller
		lv_obj_t * redline_rpm_text = lv_label_create(ui_MenuScreen);
		lv_label_set_text(redline_rpm_text, "Redline RPM:");
		lv_obj_set_style_text_color(redline_rpm_text, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(redline_rpm_text, LV_ALIGN_CENTER, 220, -47); // Position next to RPM color
		lv_obj_set_style_text_align(redline_rpm_text, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * redline_rpm_roller = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(redline_rpm_roller, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(redline_rpm_roller,
		    "3000\n3200\n3400\n3600\n3800\n4000\n4200\n4400\n4600\n4800\n"
		    "5000\n5200\n5400\n5600\n5800\n6000\n6200\n6400\n6600\n6800\n"
		    "7000\n7200\n7400\n7600\n7800\n8000\n8200\n8400\n8600\n8800\n"
		    "9000\n9200\n9400\n9600\n9800\n10000\n10200\n10400\n10600\n10800\n"
		    "11000\n11200\n11400\n11600\n11800\n12000");

		uint16_t redline_selected_index = 0;
		extern int rpm_redline_value;
		if (rpm_redline_value >= 3000 && rpm_redline_value <= 12000) {
		    redline_selected_index = (rpm_redline_value - 3000) / 200;
		}
		lv_dropdown_set_selected(redline_rpm_roller, redline_selected_index);
		lv_obj_set_width(redline_rpm_roller, 90);
		lv_obj_align(redline_rpm_roller, LV_ALIGN_CENTER, 320, -47);
		lv_obj_add_event_cb(redline_rpm_roller, rpm_redline_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

		// Limiter Effect dropdown
		lv_obj_t * limiter_effect_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(limiter_effect_label, "Limiter Effect:");
		lv_obj_set_style_text_color(limiter_effect_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(limiter_effect_label, LV_ALIGN_CENTER, -50, -7); // Position below redline
		lv_obj_set_style_text_align(limiter_effect_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * limiter_effect_dropdown = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(limiter_effect_dropdown, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(limiter_effect_dropdown, "None\nBar Flash\nBar & Circles Flash\nCircles Flash\nBar Solid\nBar & Circles Solid\nCircles Solid");
		lv_obj_set_width(limiter_effect_dropdown, 120);
		lv_obj_align(limiter_effect_dropdown, LV_ALIGN_CENTER, 80, -7);
		
		// Set current selection based on saved configuration
		uint8_t limiter_effect = values_config[RPM_VALUE_ID - 1].rpm_limiter_effect;
		// Map internal effect type to dropdown index (0=None, 2=Bar Flash, 3=Bar & Circles Flash, 4=Circles Flash, 5=Bar Solid, 6=Bar & Circles Solid, 7=Circles Solid)
		uint8_t dropdown_index = 0;
		if (limiter_effect == 2) {
			dropdown_index = 1; // Bar Flash
		} else if (limiter_effect == 3) {
			dropdown_index = 2; // Bar & Circles Flash
		} else if (limiter_effect == 4) {
			dropdown_index = 3; // Circles Flash
		} else if (limiter_effect == 5) {
			dropdown_index = 4; // Bar Solid
		} else if (limiter_effect == 6) {
			dropdown_index = 5; // Bar & Circles Solid
		} else if (limiter_effect == 7) {
			dropdown_index = 6; // Circles Solid
		}
		lv_dropdown_set_selected(limiter_effect_dropdown, dropdown_index);
		
		// Add event handler for limiter effect
		lv_obj_add_event_cb(limiter_effect_dropdown, rpm_limiter_effect_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

		// RPM Limiter roller
		lv_obj_t * rpm_limiter_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(rpm_limiter_label, "RPM Limiter:");
		lv_obj_set_style_text_color(rpm_limiter_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(rpm_limiter_label, LV_ALIGN_CENTER, 220, -7); // Position next to effect dropdown
		lv_obj_set_style_text_align(rpm_limiter_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * rpm_limiter_roller = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(rpm_limiter_roller, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(rpm_limiter_roller,
		    "3000\n3200\n3400\n3600\n3800\n4000\n4200\n4400\n4600\n4800\n"
		    "5000\n5200\n5400\n5600\n5800\n6000\n6200\n6400\n6600\n6800\n"
		    "7000\n7200\n7400\n7600\n7800\n8000\n8200\n8400\n8600\n8800\n"
		    "9000\n9200\n9400\n9600\n9800\n10000\n10200\n10400\n10600\n10800\n"
		    "11000\n11200\n11400\n11600\n11800\n12000");

		uint16_t limiter_selected_index = 0;
		int32_t limiter_value = values_config[RPM_VALUE_ID - 1].rpm_limiter_value;
		if (limiter_value >= 3000 && limiter_value <= 12000) {
		    limiter_selected_index = (limiter_value - 3000) / 200;
		}
		lv_dropdown_set_selected(rpm_limiter_roller, limiter_selected_index);
		lv_obj_set_width(rpm_limiter_roller, 90);
		lv_obj_align(rpm_limiter_roller, LV_ALIGN_CENTER, 320, -7);
		lv_obj_add_event_cb(rpm_limiter_roller, rpm_limiter_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

		// Limiter Colour dropdown
		lv_obj_t * limiter_color_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(limiter_color_label, "Limiter Colour:");
		lv_obj_set_style_text_color(limiter_color_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(limiter_color_label, LV_ALIGN_CENTER, -50, 33); // Position below RPM limiter
		lv_obj_set_style_text_align(limiter_color_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * limiter_color_dropdown = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(limiter_color_dropdown, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(limiter_color_dropdown, 
		    "Green\n"
		    "Light Blue\n"
		    "Yellow\n"
		    "Orange\n"
		    "Red\n"
		    "Dark Blue\n"
		    "Purple\n"
		    "Magenta\n"
		    "Pink\n"
		    "Custom");
		lv_obj_set_width(limiter_color_dropdown, 120);
		lv_obj_align(limiter_color_dropdown, LV_ALIGN_CENTER, 80, 33);

		// Add event handler
		lv_obj_add_event_cb(limiter_color_dropdown, rpm_limiter_color_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
		uint16_t saved_limiter_color = values_config[RPM_VALUE_ID - 1].rpm_limiter_color.full;

		// Set selected index based on current limiter color (default to Custom if no match)
		uint16_t limiter_color_selected_index = 9; // Default to Custom
		if (saved_limiter_color == lv_color_hex(0x00FF00).full) limiter_color_selected_index = 0; // Green
		else if (saved_limiter_color == lv_color_hex(0x00FFFF).full) limiter_color_selected_index = 1; // Light Blue
		else if (saved_limiter_color == lv_color_hex(0xFFFF00).full) limiter_color_selected_index = 2; // Yellow
		else if (saved_limiter_color == lv_color_hex(0xFF7F00).full) limiter_color_selected_index = 3; // Orange
		else if (saved_limiter_color == lv_color_hex(0xFF0000).full) limiter_color_selected_index = 4; // Red
		else if (saved_limiter_color == lv_color_hex(0x0080FF).full) limiter_color_selected_index = 5; // Dark Blue
		else if (saved_limiter_color == lv_color_hex(0x8000FF).full) limiter_color_selected_index = 6; // Purple
		else if (saved_limiter_color == lv_color_hex(0xFF00FF).full) limiter_color_selected_index = 7; // Magenta
		else if (saved_limiter_color == lv_color_hex(0xFF1493).full) limiter_color_selected_index = 8; // Pink
		// If none of the predefined colors match, it's a custom color (index 9)
		lv_dropdown_set_selected(limiter_color_dropdown, limiter_color_selected_index);

		// RPM Lights toggle
		lv_obj_t * rpm_lights_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(rpm_lights_label, "RPM Lights:");
		lv_obj_set_style_text_color(rpm_lights_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(rpm_lights_label, LV_ALIGN_CENTER, 220, 33); // Position next to limiter color
		lv_obj_set_style_text_align(rpm_lights_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * rpm_lights_switch = lv_switch_create(ui_MenuScreen);
		lv_obj_set_size(rpm_lights_switch, 50, 25);
		lv_obj_align(rpm_lights_switch, LV_ALIGN_CENTER, 320, 33);
		
		// Set current state based on saved configuration
		if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
		    lv_obj_add_state(rpm_lights_switch, LV_STATE_CHECKED);
		} else {
		    lv_obj_clear_state(rpm_lights_switch, LV_STATE_CHECKED);
		}
		
		// Add event handler for RPM lights switch
		lv_obj_add_event_cb(rpm_lights_switch, rpm_lights_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

		// RPM Background toggle
		lv_obj_t * rpm_background_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(rpm_background_label, "RPM Background:");
		lv_obj_set_style_text_color(rpm_background_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(rpm_background_label, LV_ALIGN_CENTER, 220, 73); // Position below RPM lights
		lv_obj_set_style_text_align(rpm_background_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * rpm_background_switch = lv_switch_create(ui_MenuScreen);
		lv_obj_set_size(rpm_background_switch, 50, 25);
		lv_obj_align(rpm_background_switch, LV_ALIGN_CENTER, 320, 73);
		
		// Set current state based on saved configuration
		if (values_config[RPM_VALUE_ID - 1].rpm_background_enabled) {
		    lv_obj_add_state(rpm_background_switch, LV_STATE_CHECKED);
		} else {
		    lv_obj_clear_state(rpm_background_switch, LV_STATE_CHECKED);
		}
		
		// Add event handler for RPM background switch
		lv_obj_add_event_cb(rpm_background_switch, rpm_background_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

		// RPM Background Colour dropdown
		lv_obj_t * rpm_background_color_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(rpm_background_color_label, "Background Colour:");
		lv_obj_set_style_text_color(rpm_background_color_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(rpm_background_color_label, LV_ALIGN_CENTER, -50, 113); // Position below RPM background toggle
		lv_obj_set_style_text_align(rpm_background_color_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * rpm_background_color_dropdown = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(rpm_background_color_dropdown, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(rpm_background_color_dropdown, "Green\nLight Blue\nYellow\nOrange\nRed\nBlue\nPurple\nMagenta\nPink\nCustom");
		lv_obj_set_width(rpm_background_color_dropdown, 120);
		lv_obj_align(rpm_background_color_dropdown, LV_ALIGN_CENTER, 80, 113);
		
		// Set current selection based on saved configuration
		uint8_t rpm_background_color_selected_index = 9; // Default to custom
		uint32_t saved_rpm_background_color = values_config[RPM_VALUE_ID - 1].rpm_background_color.full;
		if (saved_rpm_background_color == lv_color_hex(0x00FF00).full) rpm_background_color_selected_index = 0; // Green
		else if (saved_rpm_background_color == lv_color_hex(0x00FFFF).full) rpm_background_color_selected_index = 1; // Light Blue
		else if (saved_rpm_background_color == lv_color_hex(0xFFFF00).full) rpm_background_color_selected_index = 2; // Yellow
		else if (saved_rpm_background_color == lv_color_hex(0xFF7F00).full) rpm_background_color_selected_index = 3; // Orange
		else if (saved_rpm_background_color == lv_color_hex(0xFF0000).full) rpm_background_color_selected_index = 4; // Red
		else if (saved_rpm_background_color == lv_color_hex(0x0080FF).full) rpm_background_color_selected_index = 5; // Dark Blue
		else if (saved_rpm_background_color == lv_color_hex(0x8000FF).full) rpm_background_color_selected_index = 6; // Purple
		else if (saved_rpm_background_color == lv_color_hex(0xFF00FF).full) rpm_background_color_selected_index = 7; // Magenta
		else if (saved_rpm_background_color == lv_color_hex(0xFF1493).full) rpm_background_color_selected_index = 8; // Pink
		// If none of the predefined colors match, it's a custom color (index 9)
		lv_dropdown_set_selected(rpm_background_color_dropdown, rpm_background_color_selected_index);
		
		// Add event handler for RPM background color dropdown
		lv_obj_add_event_cb(rpm_background_color_dropdown, rpm_background_color_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

		// RPM Background threshold roller
		lv_obj_t * rpm_background_threshold_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(rpm_background_threshold_label, "Background RPM:");
		lv_obj_set_style_text_color(rpm_background_threshold_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(rpm_background_threshold_label, LV_ALIGN_CENTER, 220, 113); // Position next to background color
		lv_obj_set_style_text_align(rpm_background_threshold_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * rpm_background_threshold_roller = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(rpm_background_threshold_roller, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(rpm_background_threshold_roller,
		    "3000\n3200\n3400\n3600\n3800\n4000\n4200\n4400\n4600\n4800\n"
		    "5000\n5200\n5400\n5600\n5800\n6000\n6200\n6400\n6600\n6800\n"
		    "7000\n7200\n7400\n7600\n7800\n8000\n8200\n8400\n8600\n8800\n"
		    "9000\n9200\n9400\n9600\n9800\n10000\n10200\n10400\n10600\n10800\n"
		    "11000\n11200\n11400\n11600\n11800\n12000");

		uint16_t background_threshold_selected_index = 0;
		int32_t background_threshold_value = values_config[RPM_VALUE_ID - 1].rpm_background_value;
		if (background_threshold_value >= 3000 && background_threshold_value <= 12000) {
		    background_threshold_selected_index = (background_threshold_value - 3000) / 200;
		}
		lv_dropdown_set_selected(rpm_background_threshold_roller, background_threshold_selected_index);
		lv_obj_set_width(rpm_background_threshold_roller, 90);
		lv_obj_align(rpm_background_threshold_roller, LV_ALIGN_CENTER, 320, 113);
		lv_obj_add_event_cb(rpm_background_threshold_roller, rpm_background_threshold_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

		// Gradient option removed

    } else if (value_id == SPEED_VALUE_ID) {
        // Speed value
        const char *current_speed_value = lv_label_get_text(ui_Speed_Value);
        const char *current_kmh_label = lv_label_get_text(ui_Kmh);
		create_config_controls(ui_MenuScreen, SPEED_VALUE_ID);

        show_preconfig_menu(ui_MenuScreen);

        // Create Speed value label and store global reference for live updates
        menu_speed_value_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(menu_speed_value_label, current_speed_value);
        // Apply same styling as on Screen3 for Speed value
        lv_obj_set_style_text_color(menu_speed_value_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(menu_speed_value_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(menu_speed_value_label, &ui_font_fugaz_56, LV_PART_MAIN | LV_STATE_DEFAULT);
    	lv_obj_align(menu_speed_value_label, LV_ALIGN_CENTER, -328, -185);
    	
        // Create Kmh label and store global reference for live updates
        menu_speed_units_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(menu_speed_units_label, current_kmh_label);
        // Apply same styling as on Screen3 for Kmh label
        lv_obj_set_style_text_color(menu_speed_units_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(menu_speed_units_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(menu_speed_units_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
   	lv_obj_align(menu_speed_units_label, LV_ALIGN_CENTER, -295, -156);
   	
   	// Add KMH/MPH units dropdown
    	lv_obj_t * speed_units_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(speed_units_label, "Speed Units:");
    	lv_obj_set_style_text_color(speed_units_label, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(speed_units_label, LV_ALIGN_CENTER, -50, -47);
    	lv_obj_set_style_text_align(speed_units_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    	
    	lv_obj_t * speed_units_dropdown = lv_dropdown_create(ui_MenuScreen);
    	lv_obj_add_style(speed_units_dropdown, get_common_style(), LV_PART_MAIN);
    	lv_dropdown_set_options(speed_units_dropdown, "KMH\nMPH");
    	lv_obj_set_align(speed_units_dropdown, LV_ALIGN_CENTER);
    	lv_obj_set_width(speed_units_dropdown, 120);
    	lv_obj_set_pos(speed_units_dropdown, 80, -47);
    	
    	// Set current selection based on configuration
    	uint16_t units_selection = values_config[SPEED_VALUE_ID - 1].use_mph ? 1 : 0;
    	lv_dropdown_set_selected(speed_units_dropdown, units_selection);
    	
   	// Add event callback for speed units dropdown
   	lv_obj_add_event_cb(speed_units_dropdown, speed_units_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    } else if (value_id == GEAR_VALUE_ID) {
		create_config_controls(ui_MenuScreen, GEAR_VALUE_ID);
	
	// Create box for gear demo (matching panel box style but without border)
   	lv_obj_t * gear_demo_box = lv_obj_create(ui_MenuScreen);
   	lv_obj_set_size(gear_demo_box, 155, 92);
   	lv_obj_set_align(gear_demo_box, LV_ALIGN_TOP_LEFT);
   	lv_obj_set_pos(gear_demo_box, 5, 5);  // Same position as panel box
   	lv_obj_clear_flag(gear_demo_box, LV_OBJ_FLAG_SCROLLABLE);
   	lv_obj_add_style(gear_demo_box, get_box_style(), LV_PART_MAIN | LV_STATE_DEFAULT);
   	lv_obj_set_style_border_width(gear_demo_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);  // No border for gear
   	
   	// Gear label (as child of box, matching panel label)
	ui_Gear_Label = lv_label_create(gear_demo_box);
   	lv_label_set_text(ui_Gear_Label, label_texts[GEAR_VALUE_ID - 1]); 
   	lv_obj_set_style_text_color(ui_Gear_Label, lv_color_hex(0xFFFFFF), 0);
   	lv_obj_set_style_text_font(ui_Gear_Label, &ui_font_fugaz_14, 0);
   	lv_obj_set_style_text_align(ui_Gear_Label, LV_TEXT_ALIGN_CENTER, 0);  // Ensure text is centered
   	lv_obj_align(ui_Gear_Label, LV_ALIGN_TOP_MID, 0, -14);  // Top-mid alignment with offset
   	
   	// Create Gear value preview and store global reference for live updates (as child of box, perfectly centered)
   	const char *current_gear_value = lv_label_get_text(ui_GEAR_Value);
   	menu_gear_value_label = lv_label_create(gear_demo_box);
   	lv_label_set_text(menu_gear_value_label, current_gear_value ? current_gear_value : " ");
   	lv_obj_set_style_text_color(menu_gear_value_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
   	lv_obj_set_style_text_opa(menu_gear_value_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
   	lv_obj_set_style_text_font(menu_gear_value_label, &ui_font_Manrope_54_BOLD, LV_PART_MAIN | LV_STATE_DEFAULT);
   	lv_obj_set_style_text_align(menu_gear_value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
   	lv_obj_set_style_transform_zoom(menu_gear_value_label, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
   	lv_obj_set_width(menu_gear_value_label, LV_SIZE_CONTENT);
   	lv_obj_align(menu_gear_value_label, LV_ALIGN_CENTER, 0, 5);  // Perfectly centered horizontally and vertically with slight offset
   	
   	// Create gear icon for menu test panel (for custom icon display)
   	menu_gear_icon = lv_img_create(gear_demo_box);
   	lv_img_set_src(menu_gear_icon, &Smart_Car_Key);
   	// Set zoom to 88% (12% smaller): 256 * 0.88 = 225.28, use 225
   	lv_img_set_zoom(menu_gear_icon, 225);
   	lv_obj_set_size(menu_gear_icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
   	lv_img_set_size_mode(menu_gear_icon, LV_IMG_SIZE_MODE_REAL);
   	// Set pivot point to center of image (30x58 original size, so center is 15, 29)
   	// This ensures the icon stays centered when zoomed
   	lv_img_set_pivot(menu_gear_icon, 15, 29);
   	lv_obj_align(menu_gear_icon, LV_ALIGN_CENTER, -15, 5);  // Same position as label, shifted left 15px
   	// Initially hidden - will be shown when custom icon value matches
   	lv_obj_add_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
    	
    	// Add Gear ECU dropdown
    	lv_obj_t * gear_ecu_dropdown_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(gear_ecu_dropdown_label, "Gear ECU:");
    	lv_obj_set_style_text_color(gear_ecu_dropdown_label, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(gear_ecu_dropdown_label, LV_ALIGN_CENTER, -50, -87);
    	lv_obj_set_style_text_align(gear_ecu_dropdown_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    	
    	lv_obj_t * gear_ecu_dropdown = lv_dropdown_create(ui_MenuScreen);
    	lv_dropdown_set_options(gear_ecu_dropdown, "Custom\nMaxxECU\nHaltech\nFord\nSpeed/RPM Ratio");
    	lv_obj_align(gear_ecu_dropdown, LV_ALIGN_CENTER, 80, -87);
    	lv_obj_set_width(gear_ecu_dropdown, 120);
    	lv_obj_add_style(gear_ecu_dropdown, get_common_style(), LV_PART_MAIN);

    	// Auto-select the saved gear detection mode
    	uint8_t gear_mode = values_config[GEAR_VALUE_ID - 1].gear_detection_mode;
    	
    	// If not in custom mode (0) or Speed/RPM Ratio (4), sync with device settings ECU preconfig
    	if (gear_mode != 0 && gear_mode != 4) {
    	    uint8_t device_ecu_preconfig = get_selected_ecu_preconfig();
    	    // Map device settings to gear detection mode: 0=Custom, 1=MaxxECU, 2=Haltech, 3=Ford, 4=Speed/RPM Ratio
    	    if (device_ecu_preconfig == 1) {
    	        gear_mode = 1; // MaxxECU
    	        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 1;
    	    } else if (device_ecu_preconfig == 2) {
    	        gear_mode = 2; // Haltech
    	        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 2;
    	    } else if (device_ecu_preconfig == 3) {
    	        gear_mode = 3; // Ford
    	        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 3;
    	    }
    	    // If device_ecu_preconfig is 0 (Custom), keep current gear_mode
    	    // Speed/RPM Ratio (4) is independent and not synced with device settings
    	}
    	
    	lv_dropdown_set_selected(gear_ecu_dropdown, gear_mode);
    	
    	// Attach event callback for gear ECU dropdown
    	lv_obj_add_event_cb(gear_ecu_dropdown, gear_ecu_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    	
    	// Add Custom Gear Value Configuration (only show if Custom mode is selected)
    	if (gear_mode == 0) {
    	    // Create custom gear values section
    	    create_custom_gear_values_section(ui_MenuScreen, gear_mode);
    	    // Ensure save and cancel buttons are on top after custom gear container is created
    	    // This prevents the container from blocking button clicks
    	    // Note: save_btn and close_btn are local variables defined earlier in this function
    	    // We need to move them to foreground here to ensure they're above the container
    	    lv_obj_move_foreground(save_btn);
    	    lv_obj_move_foreground(close_btn);
    	} else {
    	    // Hide custom gear values section if it exists
    	    hide_custom_gear_values_section();
    	}
    	
    	// Add preconfig panel for gear menu
    	show_preconfig_menu(ui_MenuScreen);

    } else if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
        create_config_controls(ui_MenuScreen, value_id);

        // Create Bar label (store in global array for live updates)
        uint8_t bar_idx = (value_id == BAR1_VALUE_ID) ? 0 : 1;
        menu_bar_labels[bar_idx] = lv_label_create(ui_MenuScreen);
        lv_label_set_text(menu_bar_labels[bar_idx], label_texts[value_id - 1]);
        lv_obj_set_style_text_font(menu_bar_labels[bar_idx], &ui_font_fugaz_14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(menu_bar_labels[bar_idx], lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(menu_bar_labels[bar_idx], LV_ALIGN_CENTER, -312, -210);

        // Create Bar visualization (the preview bar)
        lv_obj_t * menu_bar = lv_bar_create(ui_MenuScreen);
        
        // Store reference in global array for live updates
        if (value_id == BAR1_VALUE_ID) {
            menu_bar_objects[0] = menu_bar;
        } else if (value_id == BAR2_VALUE_ID) {
            menu_bar_objects[1] = menu_bar;
        }
        
        lv_obj_set_size(menu_bar, 140, 25);
        lv_obj_align(menu_bar, LV_ALIGN_CENTER, -312, -179);
        lv_obj_set_style_bg_color(menu_bar, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(menu_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(menu_bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(menu_bar, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(menu_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(menu_bar, lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(menu_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(menu_bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_width(menu_bar, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_pad(menu_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(menu_bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(menu_bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(menu_bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(menu_bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        lv_obj_set_style_radius(menu_bar, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(menu_bar, lv_color_hex(0x38FF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(menu_bar, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        
        // Save the pointer for threshold callback compatibility (BAR1 is index 11 and BAR2 is index 12)
        config_bars[value_id - 1] = menu_bar;
        
        // Bar Minimum input
        lv_obj_t * bar_min_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_min_label, "Min Value:");
        lv_obj_set_style_text_color(bar_min_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_min_label, LV_ALIGN_CENTER, -50, -87);
      
        lv_obj_t * bar_min_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_min_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_min_input, true);
        lv_textarea_set_placeholder_text(bar_min_input, "Min Value");
        lv_obj_set_width(bar_min_input, 100);
        lv_obj_align(bar_min_input, LV_ALIGN_CENTER, 80, -87);
        lv_obj_add_event_cb(bar_min_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_set_user_data(bar_min_input, (void*)1); // Mark as min input
      
        // Show Value Toggle (second column after Min Value)
        lv_obj_t * show_value_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(show_value_label, "Show Value:");
        lv_obj_set_style_text_color(show_value_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(show_value_label, LV_ALIGN_CENTER, 200, -87);
        
        lv_obj_t * show_value_switch = lv_switch_create(ui_MenuScreen);
        lv_obj_align(show_value_switch, LV_ALIGN_CENTER, 330, -87);
        lv_obj_set_size(show_value_switch, 50, 25);
        
        // Set switch state based on configuration
        if (values_config[value_id - 1].show_bar_value) {
            lv_obj_add_state(show_value_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(show_value_switch, LV_STATE_CHECKED);
        }
        
        // Add event callbacks
        uint8_t *show_value_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *show_value_id_ptr = value_id;
        lv_obj_add_event_cb(show_value_switch, show_value_switch_event_cb, LV_EVENT_VALUE_CHANGED, show_value_id_ptr);
        lv_obj_add_event_cb(show_value_switch, free_value_id_event_cb, LV_EVENT_DELETE, show_value_id_ptr);
        
        // Invert Value Toggle (second column, down one row after Max Value)
        lv_obj_t * invert_value_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(invert_value_label, "Invert Value:");
        lv_obj_set_style_text_color(invert_value_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(invert_value_label, LV_ALIGN_CENTER, 200, -47);
        
        lv_obj_t * invert_value_switch = lv_switch_create(ui_MenuScreen);
        lv_obj_align(invert_value_switch, LV_ALIGN_CENTER, 330, -47);
        lv_obj_set_size(invert_value_switch, 50, 25);
        
        // Set switch state based on configuration
        if (values_config[value_id - 1].invert_bar_value) {
            lv_obj_add_state(invert_value_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(invert_value_switch, LV_STATE_CHECKED);
        }
        
        // Add event callbacks
        uint8_t *invert_value_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *invert_value_id_ptr = value_id;
        lv_obj_add_event_cb(invert_value_switch, invert_value_switch_event_cb, LV_EVENT_VALUE_CHANGED, invert_value_id_ptr);
        lv_obj_add_event_cb(invert_value_switch, free_value_id_event_cb, LV_EVENT_DELETE, invert_value_id_ptr);
      
        // Bar Maximum input
        lv_obj_t * bar_max_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_max_label, "Max Value:");
        lv_obj_set_style_text_color(bar_max_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_max_label, LV_ALIGN_CENTER, -50, -47);
      
        lv_obj_t * bar_max_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_max_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_max_input, true);
        lv_textarea_set_placeholder_text(bar_max_input, "Max Value");
        lv_obj_set_width(bar_max_input, 100);
        lv_obj_align(bar_max_input, LV_ALIGN_CENTER, 80, -47);
        lv_obj_add_event_cb(bar_max_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
      
        // Add event callbacks for minimum and maximum inputs
        uint8_t *id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *id_ptr = value_id;
        lv_obj_add_event_cb(bar_min_input, bar_range_input_event_cb, LV_EVENT_VALUE_CHANGED, id_ptr);
        lv_obj_add_event_cb(bar_max_input, bar_range_input_event_cb, LV_EVENT_VALUE_CHANGED, id_ptr);
      
        // Set current values for min and max
        char buf[16];
        
        // Set min/max values from configuration
        snprintf(buf, sizeof(buf), "%d", values_config[value_id - 1].bar_min);
        lv_textarea_set_text(bar_min_input, buf);
        snprintf(buf, sizeof(buf), "%d", values_config[value_id - 1].bar_max);
        lv_textarea_set_text(bar_max_input, buf);
        
        // NEW: Create "Bar Low Value:" label and input field
        lv_obj_t * bar_low_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_low_label, "Low Value:");
        lv_obj_set_style_text_color(bar_low_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_low_label, LV_ALIGN_CENTER, -50, -7);
      
        lv_obj_t * bar_low_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_low_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_low_input, true);
        lv_textarea_set_placeholder_text(bar_low_input, "Low Value");
        lv_obj_set_width(bar_low_input, 100);
        lv_obj_align(bar_low_input, LV_ALIGN_CENTER, 80, -7);
        lv_obj_add_event_cb(bar_low_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
      
        // Attach event callback for the low value input (and free the allocated id on delete)
        uint8_t *bar_low_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_low_id_ptr = value_id;
        lv_obj_add_event_cb(bar_low_input, bar_low_value_event_cb, LV_EVENT_VALUE_CHANGED, bar_low_id_ptr);
        lv_obj_add_event_cb(bar_low_input, free_value_id_event_cb, LV_EVENT_DELETE, bar_low_id_ptr);
      
        // Autopopulate "Bar Low Value:" (assumes a bar_low member exists)
        char buf_low[16];
        snprintf(buf_low, sizeof(buf_low), "%d", values_config[value_id - 1].bar_low);
        lv_textarea_set_text(bar_low_input, buf_low);
        
        // Fuel Sender Toggle (right column alongside Low Value)
        lv_obj_t * fuel_sender_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(fuel_sender_label, "Fuel Sender:");
        lv_obj_set_style_text_color(fuel_sender_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(fuel_sender_label, LV_ALIGN_CENTER, 200, -7);

        lv_obj_t * fuel_sender_switch = lv_switch_create(ui_MenuScreen);
        lv_obj_align(fuel_sender_switch, LV_ALIGN_CENTER, 330, -7);
        lv_obj_set_size(fuel_sender_switch, 50, 25);

        if (values_config[value_id - 1].fuel_sender) {
            lv_obj_add_state(fuel_sender_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(fuel_sender_switch, LV_STATE_CHECKED);
        }

        // "Set:" label — left side of the top row
        lv_obj_t * fs_set_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(fs_set_label, "Set:");
        lv_obj_set_style_text_color(fs_set_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(fs_set_label, &lv_font_montserrat_14, 0);
        lv_obj_align(fs_set_label, LV_ALIGN_CENTER, 185, 30);

        // Empty / Full calibration buttons (shown only when fuel sender is on)
        lv_obj_t * fs_empty_btn = lv_btn_create(ui_MenuScreen);
        lv_obj_set_size(fs_empty_btn, 90, 30);
        lv_obj_align(fs_empty_btn, LV_ALIGN_CENTER, 215, 55);
        lv_obj_set_style_bg_color(fs_empty_btn, lv_color_hex(0x555555), 0);
        lv_obj_set_style_radius(fs_empty_btn, 6, 0);
        lv_obj_t * fs_empty_label = lv_label_create(fs_empty_btn);
        lv_label_set_text(fs_empty_label, "Empty");
        lv_obj_set_style_text_color(fs_empty_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(fs_empty_label, &lv_font_montserrat_14, 0);
        lv_obj_center(fs_empty_label);
        uint8_t *fs_empty_id = lv_mem_alloc(sizeof(uint8_t));
        *fs_empty_id = value_id;
        lv_obj_add_event_cb(fs_empty_btn, fs_empty_btn_event_cb, LV_EVENT_CLICKED, fs_empty_id);
        lv_obj_add_event_cb(fs_empty_btn, free_value_id_event_cb, LV_EVENT_DELETE, fs_empty_id);

        lv_obj_t * fs_full_btn = lv_btn_create(ui_MenuScreen);
        lv_obj_set_size(fs_full_btn, 90, 30);
        lv_obj_align(fs_full_btn, LV_ALIGN_CENTER, 320, 55);
        lv_obj_set_style_bg_color(fs_full_btn, lv_color_hex(0x555555), 0);
        lv_obj_set_style_radius(fs_full_btn, 6, 0);
        lv_obj_t * fs_full_label = lv_label_create(fs_full_btn);
        lv_label_set_text(fs_full_label, "Full");
        lv_obj_set_style_text_color(fs_full_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(fs_full_label, &lv_font_montserrat_14, 0);
        lv_obj_center(fs_full_label);
        uint8_t *fs_full_id = lv_mem_alloc(sizeof(uint8_t));
        *fs_full_id = value_id;
        lv_obj_add_event_cb(fs_full_btn, fs_full_btn_event_cb, LV_EVENT_CLICKED, fs_full_id);
        lv_obj_add_event_cb(fs_full_btn, free_value_id_event_cb, LV_EVENT_DELETE, fs_full_id);

        // Voltage value inputs (shown below the buttons)
        lv_obj_t * fs_empty_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(fs_empty_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(fs_empty_input, true);
        lv_textarea_set_placeholder_text(fs_empty_input, "Empty V");
        lv_obj_set_width(fs_empty_input, 90);
        lv_obj_align(fs_empty_input, LV_ALIGN_CENTER, 215, 93);
        lv_obj_add_event_cb(fs_empty_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
        {
            char vbuf[12];
            snprintf(vbuf, sizeof(vbuf), "%.2f", values_config[value_id - 1].fuel_sender_empty_v);
            lv_textarea_set_text(fs_empty_input, vbuf);
        }
        uint8_t *fs_empty_v_id = lv_mem_alloc(sizeof(uint8_t));
        *fs_empty_v_id = value_id;
        lv_obj_add_event_cb(fs_empty_input, fs_empty_v_input_event_cb, LV_EVENT_VALUE_CHANGED, fs_empty_v_id);
        lv_obj_add_event_cb(fs_empty_input, free_value_id_event_cb, LV_EVENT_DELETE, fs_empty_v_id);

        lv_obj_t * fs_full_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(fs_full_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(fs_full_input, true);
        lv_textarea_set_placeholder_text(fs_full_input, "Full V");
        lv_obj_set_width(fs_full_input, 90);
        lv_obj_align(fs_full_input, LV_ALIGN_CENTER, 320, 93);
        lv_obj_add_event_cb(fs_full_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
        {
            char vbuf[12];
            snprintf(vbuf, sizeof(vbuf), "%.2f", values_config[value_id - 1].fuel_sender_full_v);
            lv_textarea_set_text(fs_full_input, vbuf);
        }
        uint8_t *fs_full_v_id = lv_mem_alloc(sizeof(uint8_t));
        *fs_full_v_id = value_id;
        lv_obj_add_event_cb(fs_full_input, fs_full_v_input_event_cb, LV_EVENT_VALUE_CHANGED, fs_full_v_id);
        lv_obj_add_event_cb(fs_full_input, free_value_id_event_cb, LV_EVENT_DELETE, fs_full_v_id);

        // Live current voltage label — right side of the top row, beside "Set:"
        lv_obj_t * fs_current_label = lv_label_create(ui_MenuScreen);
        lv_obj_set_style_text_color(fs_current_label, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(fs_current_label, &lv_font_montserrat_14, 0);
        lv_obj_align(fs_current_label, LV_ALIGN_CENTER, 325, 30);
        {
            char vbuf[24];
            uint8_t bar_idx = (value_id == 12) ? 0 : 1;
            snprintf(vbuf, sizeof(vbuf), "Current: %.2f V", fuel_sender_get_filtered_v(bar_idx));
            lv_label_set_text(fs_current_label, vbuf);
        }

        // Filter label + slider
        lv_obj_t * fs_filter_label = lv_label_create(ui_MenuScreen);
        lv_obj_set_style_text_color(fs_filter_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(fs_filter_label, &lv_font_montserrat_14, 0);
        lv_obj_align(fs_filter_label, LV_ALIGN_CENTER, 265, 125);
        {
            char fbuf[24];
            snprintf(fbuf, sizeof(fbuf), "Filter: %d%%", values_config[value_id - 1].fuel_sender_filter);
            lv_label_set_text(fs_filter_label, fbuf);
        }

        lv_obj_t * fs_filter_slider = lv_slider_create(ui_MenuScreen);
        lv_obj_set_size(fs_filter_slider, 180, 12);
        lv_obj_align(fs_filter_slider, LV_ALIGN_CENTER, 265, 143);
        lv_slider_set_range(fs_filter_slider, 0, 100);
        lv_slider_set_value(fs_filter_slider, values_config[value_id - 1].fuel_sender_filter, LV_ANIM_OFF);

        // Initial visibility matches the current toggle state
        if (!values_config[value_id - 1].fuel_sender) {
            lv_obj_add_flag(fs_set_label,     LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fs_empty_btn,     LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fs_full_btn,      LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fs_empty_input,   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fs_full_input,    LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fs_current_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fs_filter_label,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fs_filter_slider, LV_OBJ_FLAG_HIDDEN);
        }

        // Pass value_id + label + button + input pointers to the toggle callback
        fuel_sender_ctx_t *fs_ctx = lv_mem_alloc(sizeof(fuel_sender_ctx_t));
        fs_ctx->value_id      = value_id;
        fs_ctx->set_label     = fs_set_label;
        fs_ctx->empty_btn     = fs_empty_btn;
        fs_ctx->full_btn      = fs_full_btn;
        fs_ctx->empty_input   = fs_empty_input;
        fs_ctx->full_input    = fs_full_input;
        fs_ctx->current_label = fs_current_label;
        fs_ctx->filter_label  = fs_filter_label;
        fs_ctx->filter_slider = fs_filter_slider;

        // LVGL timer to refresh the filtered voltage reading every 200 ms
        fs_ctx->update_timer = lv_timer_create(fs_voltage_update_timer_cb, 200, fs_ctx);

        lv_obj_add_event_cb(fs_filter_slider, fs_filter_slider_event_cb, LV_EVENT_VALUE_CHANGED, fs_ctx);
        lv_obj_add_event_cb(fs_filter_slider, fs_filter_slider_event_cb, LV_EVENT_RELEASED, fs_ctx);

        lv_obj_add_event_cb(fuel_sender_switch, fuel_sender_switch_event_cb, LV_EVENT_VALUE_CHANGED, fs_ctx);
        lv_obj_add_event_cb(fuel_sender_switch, fuel_sender_ctx_free_event_cb, LV_EVENT_DELETE, fs_ctx);

        // Bar Low Color
        lv_obj_t * bar_low_color_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_low_color_label, "Low Colour:");
        lv_obj_set_style_text_color(bar_low_color_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_low_color_label, LV_ALIGN_CENTER, -50, 33);

        lv_obj_t * bar_low_color_dropdown = lv_dropdown_create(ui_MenuScreen);
        lv_dropdown_set_options(bar_low_color_dropdown, 
            "Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom");
        lv_obj_set_width(bar_low_color_dropdown, 100);
        lv_obj_align(bar_low_color_dropdown, LV_ALIGN_CENTER, 80, 33);
        lv_obj_add_style(bar_low_color_dropdown, get_common_style(), LV_PART_MAIN);
        
        // Set current low color selection based on saved values
        uint16_t low_color_selected = 0; // Default to Blue
        if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x19439a).full) low_color_selected = 0; // Blue
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFF0000).full) low_color_selected = 1; // Red
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x38FF00).full) low_color_selected = 2; // Green
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFFFF00).full) low_color_selected = 3; // Yellow
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFF7F00).full) low_color_selected = 4; // Orange
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x8000FF).full) low_color_selected = 5; // Purple
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x00FFFF).full) low_color_selected = 6; // Cyan
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFF00FF).full) low_color_selected = 7; // Magenta
        else low_color_selected = 8; // Custom
        lv_dropdown_set_selected(bar_low_color_dropdown, low_color_selected);
        
        // Add event callback for bar low color dropdown
        uint8_t *bar_low_color_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_low_color_id_ptr = value_id;
        lv_obj_add_event_cb(bar_low_color_dropdown, bar_low_color_event_cb, LV_EVENT_VALUE_CHANGED, bar_low_color_id_ptr);
        lv_obj_add_event_cb(bar_low_color_dropdown, free_value_id_event_cb, LV_EVENT_DELETE, bar_low_color_id_ptr);
        
        // NEW: Create "Bar High Value:" label and input field
        lv_obj_t * bar_high_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_high_label, "High Value:");
        lv_obj_set_style_text_color(bar_high_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_high_label, LV_ALIGN_CENTER, -50, 73);   // Adjust vertical positioning as needed
     
        lv_obj_t * bar_high_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_high_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_high_input, true);
        lv_textarea_set_placeholder_text(bar_high_input, "High Value");
        lv_obj_set_width(bar_high_input, 100);
        lv_obj_align(bar_high_input, LV_ALIGN_CENTER, 80, 73);    // Adjust vertical positioning as needed
        lv_obj_add_event_cb(bar_high_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
     
        uint8_t *bar_high_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_high_id_ptr = value_id;
        lv_obj_add_event_cb(bar_high_input, bar_high_value_event_cb, LV_EVENT_VALUE_CHANGED, bar_high_id_ptr);
        lv_obj_add_event_cb(bar_high_input, free_value_id_event_cb, LV_EVENT_DELETE, bar_high_id_ptr);
     
        char buf_high[16];
        snprintf(buf_high, sizeof(buf_high), "%d", values_config[value_id - 1].bar_high);
        lv_textarea_set_text(bar_high_input, buf_high);
        
        // Bar High Color
        lv_obj_t * bar_high_color_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_high_color_label, "High Colour:");
        lv_obj_set_style_text_color(bar_high_color_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_high_color_label, LV_ALIGN_CENTER, -50, 113);

        lv_obj_t * bar_high_color_dropdown = lv_dropdown_create(ui_MenuScreen);
        lv_dropdown_set_options(bar_high_color_dropdown, 
            "Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom");
        lv_obj_set_width(bar_high_color_dropdown, 100);
        lv_obj_align(bar_high_color_dropdown, LV_ALIGN_CENTER, 80, 113);
        lv_obj_add_style(bar_high_color_dropdown, get_common_style(), LV_PART_MAIN);
        
        // Set current high color selection based on saved values
        uint16_t high_color_selected = 1; // Default to Red
        if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x19439a).full) high_color_selected = 0; // Blue
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFF0000).full) high_color_selected = 1; // Red
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x38FF00).full) high_color_selected = 2; // Green
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFFFF00).full) high_color_selected = 3; // Yellow
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFF7F00).full) high_color_selected = 4; // Orange
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x8000FF).full) high_color_selected = 5; // Purple
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x00FFFF).full) high_color_selected = 6; // Cyan
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFF00FF).full) high_color_selected = 7; // Magenta
        else high_color_selected = 8; // Custom
        lv_dropdown_set_selected(bar_high_color_dropdown, high_color_selected);
        
        // Add event callback for bar high color dropdown
        uint8_t *bar_high_color_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_high_color_id_ptr = value_id;
        lv_obj_add_event_cb(bar_high_color_dropdown, bar_high_color_event_cb, LV_EVENT_VALUE_CHANGED, bar_high_color_id_ptr);
        lv_obj_add_event_cb(bar_high_color_dropdown, free_value_id_event_cb, LV_EVENT_DELETE, bar_high_color_id_ptr);
        

        // Bar In Range Color
        lv_obj_t * bar_in_range_color_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_in_range_color_label, "In Range Colour:");
        lv_obj_set_style_text_color(bar_in_range_color_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_in_range_color_label, LV_ALIGN_CENTER, -50, 153);

        lv_obj_t * bar_in_range_color_dropdown = lv_dropdown_create(ui_MenuScreen);
        lv_dropdown_set_options(bar_in_range_color_dropdown, 
            "Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom");
        lv_obj_set_width(bar_in_range_color_dropdown, 100);
        lv_obj_align(bar_in_range_color_dropdown, LV_ALIGN_CENTER, 80, 153);
        lv_obj_add_style(bar_in_range_color_dropdown, get_common_style(), LV_PART_MAIN);
        
        // Set current in range color selection based on saved values
        uint16_t in_range_color_selected = 2; // Default to Green
        if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x19439a).full) in_range_color_selected = 0; // Blue
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFF0000).full) in_range_color_selected = 1; // Red
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x38FF00).full) in_range_color_selected = 2; // Green
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFFFF00).full) in_range_color_selected = 3; // Yellow
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFF7F00).full) in_range_color_selected = 4; // Orange
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x8000FF).full) in_range_color_selected = 5; // Purple
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x00FFFF).full) in_range_color_selected = 6; // Cyan
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFF00FF).full) in_range_color_selected = 7; // Magenta
        else in_range_color_selected = 8; // Custom
        lv_dropdown_set_selected(bar_in_range_color_dropdown, in_range_color_selected);
        
        // Add event callback for bar in range color dropdown
        uint8_t *bar_in_range_color_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_in_range_color_id_ptr = value_id;
        lv_obj_add_event_cb(bar_in_range_color_dropdown, bar_in_range_color_event_cb, LV_EVENT_VALUE_CHANGED, bar_in_range_color_id_ptr);
        lv_obj_add_event_cb(bar_in_range_color_dropdown, free_value_id_event_cb, LV_EVENT_DELETE, bar_in_range_color_id_ptr);

    }	
	
	lv_obj_move_foreground(save_btn);
	lv_obj_move_foreground(close_btn);
	
    // When save is pressed, save config and go back to ui_Screen3
    lv_obj_add_event_cb(save_btn, close_menu_event_cb, LV_EVENT_CLICKED, NULL);
    
    // When close is pressed, cancel changes and go back to ui_Screen3
    lv_obj_add_event_cb(close_btn, cancel_menu_event_cb, LV_EVENT_CLICKED, NULL);

    lv_scr_load(ui_MenuScreen);
}

void create_menu_objects(lv_obj_t * parent, uint8_t value_id) {
    uint8_t idx = value_id - 1; // Adjust index (value_id is 1 to 8, arrays are 0-based)

    // Use LOCAL variables instead of overwriting global arrays to prevent corruption
    // This ensures Screen3 objects remain intact while menu is open
    
    printf("Creating menu objects for panel %d (idx %d)\n", value_id, idx);
    
    // Box (create first so labels are on top)
    menu_panel_boxes[idx] = lv_obj_create(parent);
    lv_obj_set_size(menu_panel_boxes[idx], 155, 92);
    lv_obj_set_align(menu_panel_boxes[idx], LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(menu_panel_boxes[idx], 5, 5);  // 5 pixels from top-left corner
    lv_obj_clear_flag(menu_panel_boxes[idx], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(menu_panel_boxes[idx], get_box_style(), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Set initial border properties for warning effects
    lv_obj_set_style_border_width(menu_panel_boxes[idx], 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(menu_panel_boxes[idx], lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Label (store in global array for live label updates) - Match Screen 3 style
    menu_panel_labels[idx] = lv_label_create(menu_panel_boxes[idx]);  // Parent to box for easier centering
    lv_label_set_text(menu_panel_labels[idx], label_texts[idx]);
    lv_obj_set_style_text_color(menu_panel_labels[idx], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(menu_panel_labels[idx], &ui_font_fugaz_14, 0);
    lv_obj_set_style_text_align(menu_panel_labels[idx], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(menu_panel_labels[idx], 145);
    lv_label_set_long_mode(menu_panel_labels[idx], LV_LABEL_LONG_CLIP);
    lv_obj_set_x(menu_panel_labels[idx], 0);
    lv_obj_set_y(menu_panel_labels[idx], -28);  // Relative to box center, matching Screen 3
    lv_obj_set_align(menu_panel_labels[idx], LV_ALIGN_CENTER);

    // Value (store in global array for live updates) - Match Screen 3 style
    menu_panel_value_labels[idx] = lv_label_create(menu_panel_boxes[idx]);  // Parent to box for easier centering
    // Initialize with current Screen3 value if available
    extern char previous_values[13][64];
    const char *current_value = NULL;
    
    // First try to get value from ui_Value array
    if (ui_Value[idx] && lv_obj_is_valid(ui_Value[idx])) {
        current_value = lv_label_get_text(ui_Value[idx]);
        printf("Panel %d: Using ui_Value[%d] = '%s'\n", value_id, idx, current_value);
    } 
    // Then try previous_values array
    else if (strlen(previous_values[idx]) > 0) {
        current_value = previous_values[idx];
        printf("Panel %d: Using previous_values[%d] = '%s'\n", value_id, idx, current_value);
    }
    // Fallback to "0"
    else {
        current_value = "0";
        printf("Panel %d: Using fallback value = '%s'\n", value_id, current_value);
    }
    
    lv_label_set_text(menu_panel_value_labels[idx], current_value);
    lv_obj_set_style_text_color(menu_panel_value_labels[idx], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(menu_panel_value_labels[idx], &ui_font_Manrope_35_BOLD, 0);  // Match Screen 3 font
    lv_obj_set_style_text_align(menu_panel_value_labels[idx], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(menu_panel_value_labels[idx], 140);  // Match Screen 3 width
    lv_label_set_long_mode(menu_panel_value_labels[idx], LV_LABEL_LONG_CLIP);
    lv_obj_set_x(menu_panel_value_labels[idx], 0);
    lv_obj_set_y(menu_panel_value_labels[idx], 9);  // Relative to box center, matching Screen 3
    lv_obj_set_align(menu_panel_value_labels[idx], LV_ALIGN_CENTER);

    printf("Panel %d: Created all menu objects successfully\n", value_id);
    
    // Add configuration controls for any value
    create_config_controls(parent, value_id);
}

// Speed units dropdown event callback
void speed_units_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    // Update the speed units configuration
    values_config[SPEED_VALUE_ID - 1].use_mph = (selected == 1);
    
    // Update the units label on Screen3 immediately
    if (ui_Kmh) {
        lv_label_set_text(ui_Kmh, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
    }
    
    // Also update menu preview units label if it exists and is valid
    if (menu_speed_units_label && lv_obj_is_valid(menu_speed_units_label)) {
        lv_label_set_text(menu_speed_units_label, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
    }
    
    // Save configuration to NVS immediately
    save_values_config_to_nvs();
    
    ESP_LOGI("MENU", "Speed units changed to: %s (saved to NVS)", 
        values_config[SPEED_VALUE_ID - 1].use_mph ? "MPH" : "KMH");
}

// Custom gear configuration button event callback
void custom_gear_config_btn_event_cb(lv_event_t * e) {
    ESP_LOGI("GEAR", "Opening custom gear configuration screen");
    create_custom_gear_config_menu();
}

// Show value switch event callback
void show_value_switch_event_cb(lv_event_t * e) {
    lv_obj_t * switch_obj = lv_event_get_target(e);
    uint8_t *value_id_ptr = (uint8_t *)lv_event_get_user_data(e);
    uint8_t value_id = *value_id_ptr;
    
    bool show_value = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
    values_config[value_id - 1].show_bar_value = show_value;
    
    // Update visibility on Screen 3 immediately
    if (value_id == BAR1_VALUE_ID && ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
        if (show_value) {
            lv_obj_clear_flag(ui_Bar_1_Value, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_Bar_1_Value, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (value_id == BAR2_VALUE_ID && ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
        if (show_value) {
            lv_obj_clear_flag(ui_Bar_2_Value, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_Bar_2_Value, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Save configuration to NVS
    save_values_config_to_nvs();
    
    ESP_LOGI("BAR", "Show value %s for bar %d", show_value ? "enabled" : "disabled", value_id);
}

// Invert value switch event callback
void invert_value_switch_event_cb(lv_event_t * e) {
    lv_obj_t * switch_obj = lv_event_get_target(e);
    uint8_t *value_id_ptr = (uint8_t *)lv_event_get_user_data(e);
    uint8_t value_id = *value_id_ptr;
    
    bool invert_value = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
    values_config[value_id - 1].invert_bar_value = invert_value;
    
    // The next CAN update will apply the inversion automatically
    // No need to manually update the bar here
    
    // Save configuration to NVS
    save_values_config_to_nvs();
    
    ESP_LOGI("BAR", "Invert value %s for bar %d", invert_value ? "enabled" : "disabled", value_id);
}

// Manually typed empty voltage
void fs_empty_v_input_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *ta = lv_event_get_target(e);
    uint8_t *id  = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    float v = atof(lv_textarea_get_text(ta));
    if (v < 0.0f) v = 0.0f;
    if (v > 3.3f) v = 3.3f;
    values_config[*id - 1].fuel_sender_empty_v = v;
    save_values_config_to_nvs();
}

// Manually typed full voltage
void fs_full_v_input_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *ta = lv_event_get_target(e);
    uint8_t *id  = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    float v = atof(lv_textarea_get_text(ta));
    if (v < 0.0f) v = 0.0f;
    if (v > 3.3f) v = 3.3f;
    values_config[*id - 1].fuel_sender_full_v = v;
    save_values_config_to_nvs();
}

// Capture current ADC voltage as the "empty" calibration point
void fs_empty_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint8_t *id = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    fuel_sender_capture_empty(*id);
    // Refresh the input box to show the newly captured voltage
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *screen = lv_obj_get_screen(btn);
    // Re-read the value from config:
    char vbuf[12];
    snprintf(vbuf, sizeof(vbuf), "%.2f", values_config[*id - 1].fuel_sender_empty_v);
    // Find the matching textarea by placeholder
    uint32_t cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *obj = lv_obj_get_child(screen, i);
        if (lv_obj_check_type(obj, &lv_textarea_class)) {
            const char *ph = lv_textarea_get_placeholder_text(obj);
            if (ph && strstr(ph, "Empty V")) {
                lv_textarea_set_text(obj, vbuf);
                break;
            }
        }
    }
}

// Capture current ADC voltage as the "full" calibration point
void fs_full_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint8_t *id = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    fuel_sender_capture_full(*id);
    // Refresh the input box to show the newly captured voltage
    char vbuf[12];
    snprintf(vbuf, sizeof(vbuf), "%.2f", values_config[*id - 1].fuel_sender_full_v);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *screen = lv_obj_get_screen(btn);
    uint32_t cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *obj = lv_obj_get_child(screen, i);
        if (lv_obj_check_type(obj, &lv_textarea_class)) {
            const char *ph = lv_textarea_get_placeholder_text(obj);
            if (ph && strstr(ph, "Full V")) {
                lv_textarea_set_text(obj, vbuf);
                break;
            }
        }
    }
}

// Free the fuel_sender context struct when the switch is deleted
static void fs_filter_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    values_config[ctx->value_id - 1].fuel_sender_filter = (uint8_t)val;
    // Update label while dragging; only write NVS on release
    char fbuf[24];
    snprintf(fbuf, sizeof(fbuf), "Filter: %d%%", (int)val);
    lv_label_set_text(ctx->filter_label, fbuf);
    if (code == LV_EVENT_RELEASED) {
        save_values_config_to_nvs();
    }
}

static void fs_voltage_update_timer_cb(lv_timer_t * timer) {
    fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)timer->user_data;
    if (!ctx) return;
    lv_obj_t *label = ctx->current_label;
    if (!label || !lv_obj_is_valid(label)) return;
    if (lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) return;
    // bar_idx: BAR1_VALUE_ID=12 → 0, BAR2_VALUE_ID=13 → 1
    uint8_t bar_idx = (ctx->value_id == 12) ? 0 : 1;
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), "Current: %.2f V", fuel_sender_get_filtered_v(bar_idx));
    lv_label_set_text(label, vbuf);
}

void fuel_sender_ctx_free_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)lv_event_get_user_data(e);
        if (ctx) {
            if (ctx->update_timer) {
                lv_timer_del(ctx->update_timer);
                ctx->update_timer = NULL;
            }
            lv_mem_free(ctx);
        }
    }
}

// Fuel sender switch event callback
void fuel_sender_switch_event_cb(lv_event_t * e) {
    lv_obj_t * switch_obj = lv_event_get_target(e);
    fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    bool fuel_sender = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
    values_config[ctx->value_id - 1].fuel_sender = fuel_sender;

    // Show or hide the "Set:" label, buttons and voltage inputs
#define FS_SHOW(obj) if ((obj) && lv_obj_is_valid(obj)) lv_obj_clear_flag((obj), LV_OBJ_FLAG_HIDDEN)
#define FS_HIDE(obj) if ((obj) && lv_obj_is_valid(obj)) lv_obj_add_flag((obj),   LV_OBJ_FLAG_HIDDEN)
    if (fuel_sender) {
        FS_SHOW(ctx->set_label);
        FS_SHOW(ctx->empty_btn);
        FS_SHOW(ctx->full_btn);
        FS_SHOW(ctx->empty_input);
        FS_SHOW(ctx->full_input);
        FS_SHOW(ctx->current_label);
        FS_SHOW(ctx->filter_label);
        FS_SHOW(ctx->filter_slider);
    } else {
        FS_HIDE(ctx->set_label);
        FS_HIDE(ctx->empty_btn);
        FS_HIDE(ctx->full_btn);
        FS_HIDE(ctx->empty_input);
        FS_HIDE(ctx->full_input);
        FS_HIDE(ctx->current_label);
        FS_HIDE(ctx->filter_label);
        FS_HIDE(ctx->filter_slider);
    }
#undef FS_SHOW
#undef FS_HIDE

    save_values_config_to_nvs();

    ESP_LOGI("BAR", "Fuel sender %s for bar %d", fuel_sender ? "enabled" : "disabled", ctx->value_id);
}

// Custom text input event callback
void custom_text_input_event_cb(lv_event_t * e) {
    uint8_t *value_id_ptr = (uint8_t *)lv_event_get_user_data(e);
    uint8_t value_id = *value_id_ptr;
    
    lv_obj_t * textarea = lv_event_get_target(e);
    const char * text = lv_textarea_get_text(textarea);
    
    // Update configuration with new custom text
    if (text != NULL) {
        strncpy(values_config[value_id - 1].custom_text, text, sizeof(values_config[value_id - 1].custom_text) - 1);
        values_config[value_id - 1].custom_text[sizeof(values_config[value_id - 1].custom_text) - 1] = '\0'; // Ensure null termination
    } else {
        values_config[value_id - 1].custom_text[0] = '\0'; // Empty string if text is null
    }
    
    // Update the custom text label on Screen3 immediately if it exists
    extern lv_obj_t * ui_CustomText[8];
    uint8_t panel_idx = value_id - 1;
    if (panel_idx < 8 && ui_CustomText[panel_idx] && lv_obj_is_valid(ui_CustomText[panel_idx])) {
        lv_label_set_text(ui_CustomText[panel_idx], values_config[panel_idx].custom_text);
        
        // Show/hide custom text based on whether it's empty
        if (strlen(values_config[panel_idx].custom_text) == 0) {
            lv_obj_add_flag(ui_CustomText[panel_idx], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(ui_CustomText[panel_idx], LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Save configuration to NVS
    save_values_config_to_nvs();
    
    ESP_LOGI("PANEL", "Custom text set to '%s' for panel %d", values_config[value_id - 1].custom_text, value_id);
}

// Custom icon type dropdown event callback
void custom_icon_type_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    int icon_index = (int)(intptr_t)lv_event_get_user_data(e);
    
    if (icon_index < 0 || icon_index >= 7) return;
    
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    // Save the selection: 0 = None, 1 = KEY
    values_config[GEAR_VALUE_ID - 1].custom_icon_types[icon_index] = selected;
    
    // If None is selected, clear the icon value to UINT32_MAX (not configured)
    if (selected == 0) {
        values_config[GEAR_VALUE_ID - 1].custom_icon_values[icon_index] = UINT32_MAX;
        // Clear the input box to show blank
        if (custom_icon_inputs[icon_index] && lv_obj_is_valid(custom_icon_inputs[icon_index])) {
            lv_textarea_set_text(custom_icon_inputs[icon_index], "");
        }
    }
    
    // Show/hide image based on selection (0 = None, 1 = KEY)
    if (custom_icon_images[icon_index] && lv_obj_is_valid(custom_icon_images[icon_index])) {
        if (selected == 1) {
            // KEY selected - show image
            lv_obj_clear_flag(custom_icon_images[icon_index], LV_OBJ_FLAG_HIDDEN);
        } else {
            // None (0) or other option selected - hide image
            lv_obj_add_flag(custom_icon_images[icon_index], LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Save to NVS
    save_values_config_to_nvs();
    
    ESP_LOGI("GEAR", "Custom icon %d type set to: %d", icon_index, selected);
}

// Custom icon input event callback
void custom_icon_input_event_cb(lv_event_t * e) {
    lv_obj_t * textarea = lv_event_get_target(e);
    
    // Find which icon input this corresponds to
    int icon_index = -1;
    for (int i = 0; i < 7; i++) {
        if (custom_icon_inputs[i] == textarea) {
            icon_index = i;
            break;
        }
    }
    
    if (icon_index == -1) return;
    
    // Get the value from the input
    const char* value_str = lv_textarea_get_text(textarea);
    uint32_t icon_value = UINT32_MAX; // Use UINT32_MAX as sentinel for "not configured"
    
    // Check if input is empty - if so, set to UINT32_MAX (not configured)
    // If not empty, parse the value (0 is now a valid configured value)
    if (value_str != NULL && strlen(value_str) > 0) {
        // Parse value (support both decimal and hex like other CAN config)
        if (strncmp(value_str, "0x", 2) == 0 || strncmp(value_str, "0X", 2) == 0) {
            icon_value = strtoul(value_str, NULL, 16);
        } else {
            icon_value = strtoul(value_str, NULL, 10);
        }
    }
    // If empty, icon_value remains UINT32_MAX (not configured)
    
    // Store the icon value (0 is a valid configured value, UINT32_MAX means not configured)
    values_config[GEAR_VALUE_ID - 1].custom_icon_values[icon_index] = icon_value;
    
    // Save to NVS
    save_values_config_to_nvs();
    
    ESP_LOGI("GEAR", "Custom icon %d value set to: %u", icon_index, icon_value);
}

// Custom gear value input event callback
void custom_gear_value_input_event_cb(lv_event_t * e) {
    lv_obj_t * textarea = lv_event_get_target(e);
    
    // Find which gear this input corresponds to
    int gear_index = -1;
    for (int i = 0; i < 14; i++) {
        if (custom_gear_value_inputs[i] == textarea) {
            gear_index = i;
            break;
        }
    }
    
    if (gear_index == -1) return;
    
    // Get the value from the input
    const char* value_str = lv_textarea_get_text(textarea);
    uint32_t gear_value = UINT32_MAX; // Use UINT32_MAX as sentinel for "not configured"
    
    // Check if input is empty - if so, set to UINT32_MAX (not configured)
    // If not empty, parse the value (0 is now a valid configured value)
    if (value_str != NULL && strlen(value_str) > 0) {
        // Parse value (support both decimal and hex like other CAN config)
        if (strncmp(value_str, "0x", 2) == 0 || strncmp(value_str, "0X", 2) == 0) {
            gear_value = strtoul(value_str, NULL, 16);
        } else {
            gear_value = strtoul(value_str, NULL, 10);
        }
    }
    // If empty, gear_value remains UINT32_MAX (not configured)
    
    // Store the gear value (0 is a valid configured value, UINT32_MAX means not configured)
    values_config[GEAR_VALUE_ID - 1].gear_custom_values[gear_index] = gear_value;
    
    // Save to NVS
    save_values_config_to_nvs();
    
    // Log the change
    const char* gear_names[] = {"P", "R", "N", "D", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
    ESP_LOGI("GEAR", "Custom gear %s value set to: %u", gear_names[gear_index], gear_value);
}

// Create custom gear values section
void create_custom_gear_values_section(lv_obj_t * parent, uint8_t gear_mode) {
    // If container already exists, check if it's valid and on the correct parent
    if (custom_gear_values_container != NULL) {
        if (lv_obj_is_valid(custom_gear_values_container)) {
            // Check if parent matches - if not, delete and recreate
            lv_obj_t *current_parent = lv_obj_get_parent(custom_gear_values_container);
            if (current_parent == parent) {
                // Container exists and is on correct parent - just make sure it's visible
                lv_obj_clear_flag(custom_gear_values_container, LV_OBJ_FLAG_HIDDEN);
                return; // Already created and visible
            } else {
                // Container exists but on wrong parent - delete it
                lv_obj_del(custom_gear_values_container);
                custom_gear_values_container = NULL;
                // Clear input references
                for (int i = 0; i < 14; i++) {
                    custom_gear_value_inputs[i] = NULL;
                }
                for (int i = 0; i < 7; i++) {
                    custom_icon_inputs[i] = NULL;
                    custom_icon_type_dropdowns[i] = NULL;
                    custom_icon_images[i] = NULL;
                }
            }
        } else {
            // Container is invalid - clear the pointer
            custom_gear_values_container = NULL;
        }
    }
    
    // Create container for custom gear values
    custom_gear_values_container = lv_obj_create(parent);
    lv_obj_set_size(custom_gear_values_container, 500, 330);
    lv_obj_align(custom_gear_values_container, LV_ALIGN_CENTER, 110, 75);
    lv_obj_set_style_bg_color(custom_gear_values_container, lv_color_hex(0x181818), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(custom_gear_values_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(custom_gear_values_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(custom_gear_values_container, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(custom_gear_values_container, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(custom_gear_values_container, LV_OBJ_FLAG_SCROLLABLE);
    // Make container non-clickable so it doesn't block buttons behind it
    // Only the child elements (inputs, dropdowns) should be clickable
    lv_obj_clear_flag(custom_gear_values_container, LV_OBJ_FLAG_CLICKABLE);
    
    // Gear labels and inputs - 3 column layout: left (P-R-N-D-1-2-3), middle (4-5-6-7-8-9-10), right (KEY boxes)
    const char* gear_labels[] = {"P", "R", "N", "D", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
    
    // Left column: P, R, N, D, 1, 2, 3 (first 7 gears)
    // Calculate equal spacing: container is 500px wide
    // Three columns with equal spacing between their start positions
    // Column 1: 20, Column 2: 20 + (500-40)/3 ≈ 173, Column 3: 20 + 2*(500-40)/3 ≈ 326
    int col1_x = 20;   // First column start
    int col2_x = 173;  // Second column start (equal spacing)
    int col3_x = 326;  // Third column start (equal spacing)
    
    for (int i = 0; i < 7; i++) {
        int row = i;
        int x_pos = col1_x;
        int y_pos = 25 + row * 30;
        
        // Gear label with " = " suffix (space before equals)
        char gear_label_text[8];
        snprintf(gear_label_text, sizeof(gear_label_text), "%s =", gear_labels[i]);
        lv_obj_t * gear_label = lv_label_create(custom_gear_values_container);
        lv_label_set_text(gear_label, gear_label_text);
        lv_obj_set_style_text_color(gear_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(gear_label, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(gear_label, x_pos, y_pos);
        
        // Value input - adjust position to account for space in label
        custom_gear_value_inputs[i] = lv_textarea_create(custom_gear_values_container);
        lv_textarea_set_one_line(custom_gear_value_inputs[i], true);
        lv_textarea_set_max_length(custom_gear_value_inputs[i], 8);
        lv_obj_set_width(custom_gear_value_inputs[i], 60);
        lv_obj_set_height(custom_gear_value_inputs[i], 25);
        lv_obj_set_pos(custom_gear_value_inputs[i], x_pos + 30, y_pos - 2); // Adjusted for "X =" format
        lv_obj_clear_flag(custom_gear_value_inputs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(custom_gear_value_inputs[i], get_common_style(), LV_PART_MAIN);
        
        // Set current value - leave blank if UINT32_MAX (not configured)
        uint32_t gear_val = values_config[GEAR_VALUE_ID - 1].gear_custom_values[i];
        if (gear_val != UINT32_MAX) {
            char current_value[16];
            snprintf(current_value, sizeof(current_value), "%u", gear_val);
            lv_textarea_set_text(custom_gear_value_inputs[i], current_value);
        } else {
            lv_textarea_set_text(custom_gear_value_inputs[i], "");
        }
        
        // Add event callback
        lv_obj_add_event_cb(custom_gear_value_inputs[i], custom_gear_value_input_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(custom_gear_value_inputs[i], custom_gear_value_input_event_cb, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(custom_gear_value_inputs[i], keyboard_event_cb, LV_EVENT_ALL, NULL);
    }
    
    // Middle column: 4, 5, 6, 7, 8, 9, 10 (remaining 7 gears)
    for (int i = 7; i < 14; i++) {
        int row = i - 7;
        int x_pos = col2_x;
        int y_pos = 25 + row * 30;
        
        // Gear label with " = " suffix (space before equals)
        char gear_label_text[8];
        snprintf(gear_label_text, sizeof(gear_label_text), "%s =", gear_labels[i]);
        lv_obj_t * gear_label = lv_label_create(custom_gear_values_container);
        lv_label_set_text(gear_label, gear_label_text);
        lv_obj_set_style_text_color(gear_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(gear_label, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(gear_label, x_pos, y_pos);
        
        // Value input - adjust position to account for space in label
        custom_gear_value_inputs[i] = lv_textarea_create(custom_gear_values_container);
        lv_textarea_set_one_line(custom_gear_value_inputs[i], true);
        lv_textarea_set_max_length(custom_gear_value_inputs[i], 8);
        lv_obj_set_width(custom_gear_value_inputs[i], 60);
        lv_obj_set_height(custom_gear_value_inputs[i], 25);
        lv_obj_set_pos(custom_gear_value_inputs[i], x_pos + 30, y_pos - 2); // Adjusted for "X =" format
        lv_obj_clear_flag(custom_gear_value_inputs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(custom_gear_value_inputs[i], get_common_style(), LV_PART_MAIN);
        
        // Set current value - leave blank if UINT32_MAX (not configured)
        uint32_t gear_val = values_config[GEAR_VALUE_ID - 1].gear_custom_values[i];
        if (gear_val != UINT32_MAX) {
            char current_value[16];
            snprintf(current_value, sizeof(current_value), "%u", gear_val);
            lv_textarea_set_text(custom_gear_value_inputs[i], current_value);
        } else {
            lv_textarea_set_text(custom_gear_value_inputs[i], "");
        }
        
        // Add event callback
        lv_obj_add_event_cb(custom_gear_value_inputs[i], custom_gear_value_input_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(custom_gear_value_inputs[i], custom_gear_value_input_event_cb, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(custom_gear_value_inputs[i], keyboard_event_cb, LV_EVENT_ALL, NULL);
    }
    
    // Right column: Icon type dropdowns and value inputs (7 custom icon boxes)
    for (int i = 0; i < 7; i++) {
        int row = i;
        int x_pos = col3_x;
        int y_pos = 25 + row * 30;
        
        // Icon type dropdown - replace "KEY=" label
        custom_icon_type_dropdowns[i] = lv_dropdown_create(custom_gear_values_container);
        lv_dropdown_set_options(custom_icon_type_dropdowns[i], "None\nKEY"); // None is option 0, KEY is option 1
        lv_obj_set_width(custom_icon_type_dropdowns[i], 60);
        lv_obj_set_height(custom_icon_type_dropdowns[i], 25);
        lv_obj_set_pos(custom_icon_type_dropdowns[i], x_pos, y_pos - 2);
        lv_obj_add_style(custom_icon_type_dropdowns[i], get_common_style(), LV_PART_MAIN);
        
        // Load saved icon type (0 = None, 1 = KEY)
        // Default to KEY (1) if not set - NVS load sets default to 1, so 0 means explicitly None
        uint8_t saved_icon_type = values_config[GEAR_VALUE_ID - 1].custom_icon_types[i];
        // If 0 and we want to default new entries to KEY, we'd check if it's uninitialized
        // But since NVS load defaults to 1, if it's 0 it means None was explicitly selected
        lv_dropdown_set_selected(custom_icon_type_dropdowns[i], saved_icon_type);
        
        // Create icon image (for KEY icon)
        custom_icon_images[i] = lv_img_create(custom_gear_values_container);
        lv_img_set_src(custom_icon_images[i], &Smart_Car_Key);
        // Use zoom to scale the image properly (original is 30x58, target is ~14x27)
        // Zoom: 256 = 100%, 128 = 50%, so for ~47% use ~120
        lv_img_set_zoom(custom_icon_images[i], 120); // Scale to ~47% of original size
        // Set object size to content so it matches the zoomed image size
        lv_obj_set_size(custom_icon_images[i], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        // Use REAL size mode so object size matches zoomed image size
        lv_img_set_size_mode(custom_icon_images[i], LV_IMG_SIZE_MODE_REAL);
        lv_obj_set_pos(custom_icon_images[i], x_pos + 65, y_pos + 0); // Position with more space after dropdown, vertically centered
        
        // Show/hide image based on saved icon type (1 = KEY shows image, 0 = None hides it)
        if (saved_icon_type == 1) {
            lv_obj_clear_flag(custom_icon_images[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(custom_icon_images[i], LV_OBJ_FLAG_HIDDEN);
        }
        
        // Add event callback to dropdown to show/hide image based on selection
        lv_obj_add_event_cb(custom_icon_type_dropdowns[i], custom_icon_type_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
        
        // Value input - adjust spacing after dropdown and image with more gap
        custom_icon_inputs[i] = lv_textarea_create(custom_gear_values_container);
        lv_textarea_set_one_line(custom_icon_inputs[i], true);
        lv_textarea_set_max_length(custom_icon_inputs[i], 8);
        lv_obj_set_width(custom_icon_inputs[i], 60);
        lv_obj_set_height(custom_icon_inputs[i], 25);
        lv_obj_set_pos(custom_icon_inputs[i], x_pos + 90, y_pos - 2); // More space between icon and input (10px gap)
        lv_obj_clear_flag(custom_icon_inputs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(custom_icon_inputs[i], get_common_style(), LV_PART_MAIN);
        
        // Load saved icon value - leave blank if UINT32_MAX (not configured)
        uint32_t saved_icon_value = values_config[GEAR_VALUE_ID - 1].custom_icon_values[i];
        if (saved_icon_value != UINT32_MAX) {
            char icon_value_str[16];
            snprintf(icon_value_str, sizeof(icon_value_str), "%u", saved_icon_value);
            lv_textarea_set_text(custom_icon_inputs[i], icon_value_str);
        } else {
            lv_textarea_set_text(custom_icon_inputs[i], "");
        }
        
        // Add event callbacks for saving values
        lv_obj_add_event_cb(custom_icon_inputs[i], custom_icon_input_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(custom_icon_inputs[i], custom_icon_input_event_cb, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(custom_icon_inputs[i], keyboard_event_cb, LV_EVENT_ALL, NULL);
    }
}

// Hide custom gear values section
void hide_custom_gear_values_section(void) {
    if (custom_gear_values_container != NULL && lv_obj_is_valid(custom_gear_values_container)) {
        lv_obj_del(custom_gear_values_container);
        custom_gear_values_container = NULL;
        // Clear input references
        for (int i = 0; i < 14; i++) {
            custom_gear_value_inputs[i] = NULL;
        }
        for (int i = 0; i < 7; i++) {
            custom_icon_inputs[i] = NULL;
            custom_icon_type_dropdowns[i] = NULL;
            custom_icon_images[i] = NULL;
        }
    }
}
