#include "menu_screen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "../screens/ui_Screen3.h"
#include "../ui.h"
#include "../ui_preconfig.h"
#include "../config/create_config_controls.h"
#include "../callbacks/ui_callbacks.h"
#include "../menu/menu_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl_helpers.h"  // Add this for lvgl_mux
#include "gps/gps.h"      // Add GPS functionality
#include "../device_settings.h"
#include "fuel_input.h"
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

// Timezone management
static uint8_t selected_timezone = 0; // Default to UTC+0

// Function to get current timezone offset
uint8_t get_timezone_offset(void) {
    // Load from NVS if available
    nvs_handle_t handle;
    if (nvs_open("gps_config", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "timezone", &selected_timezone) != ESP_OK) {
            selected_timezone = 0; // Default to UTC+0
        }
        nvs_close(handle);
    }
    return selected_timezone;
}

// Function to save timezone setting
static void save_timezone_setting(uint8_t timezone_index) {
    selected_timezone = timezone_index;
    nvs_handle_t handle;
    if (nvs_open("gps_config", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "timezone", selected_timezone);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI("GPS", "Timezone setting saved: %d", selected_timezone);
    }
}

// Timezone dropdown event callback
static void timezone_dropdown_event_cb(lv_event_t *e) {
    lv_obj_t * dd = lv_event_get_target(e);
    uint8_t selected = lv_dropdown_get_selected(dd);
    save_timezone_setting(selected);
    ESP_LOGI("GPS", "Timezone changed to index: %d", selected);
}

// Function to get DST enabled state
bool get_dst_enabled(void) {
    uint8_t dst_enabled = 0;
    nvs_handle_t handle;
    if (nvs_open("gps_config", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "dst_enabled", &dst_enabled) != ESP_OK) {
            dst_enabled = 0; // Default to DST off
        }
        nvs_close(handle);
    }
    return dst_enabled != 0;
}

// Function to save DST setting
static void save_dst_setting(bool enabled) {
    nvs_handle_t handle;
    if (nvs_open("gps_config", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "dst_enabled", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI("GPS", "DST setting saved: %s", enabled ? "ON" : "OFF");
    }
}

// DST switch event callback
static void dst_switch_event_cb(lv_event_t *e) {
    lv_obj_t * sw = lv_event_get_target(e);
    bool is_checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    save_dst_setting(is_checked);
    ESP_LOGI("GPS", "DST changed to: %s", is_checked ? "ON" : "OFF");
}

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
    uint8_t bar_index;
    int32_t bar_value;
    double final_value;
    int config_index;
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

// GPS status update timer callback declaration
void gps_status_update_timer_cb(lv_timer_t * timer);

// RPM limiter effect callback declarations
extern void rpm_limiter_effect_dropdown_event_cb(lv_event_t * e);
extern void rpm_limiter_roller_event_cb(lv_event_t * e);
extern void rpm_limiter_color_dropdown_event_cb(lv_event_t * e);
extern void rpm_lights_switch_event_cb(lv_event_t * e);
extern void rpm_gradient_switch_event_cb(lv_event_t * e);
extern void stop_limiter_effect_demo(void);
extern void create_rpm_lights_circles(lv_obj_t * parent);
extern void bar_low_color_event_cb(lv_event_t * e);
extern void bar_high_color_event_cb(lv_event_t * e);
extern void bar_in_range_color_event_cb(lv_event_t * e);

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
      
     // High Warning Threshold
    lv_obj_t * warning_high_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_high_label, "High Warning:");
    lv_obj_set_style_text_color(warning_high_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_high_label, LV_ALIGN_CENTER, -50, -87);

    lv_obj_t * warning_high_input = lv_textarea_create(ui_MenuScreen);
    lv_obj_add_style(warning_high_input, get_common_style(), LV_PART_MAIN);
    lv_textarea_set_one_line(warning_high_input, true);
    lv_obj_set_width(warning_high_input, 100);
    lv_obj_align(warning_high_input, LV_ALIGN_CENTER, 80, -87);
    lv_obj_add_event_cb(warning_high_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // High Warning Color
    lv_obj_t * warning_high_color_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_high_color_label, "High Color:");
    lv_obj_set_style_text_color(warning_high_color_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_high_color_label, LV_ALIGN_CENTER, -50, -47);

    lv_obj_t * warning_high_color_dd = lv_dropdown_create(ui_MenuScreen);
    lv_dropdown_set_options(warning_high_color_dd, "Red\nBlue");
    lv_obj_set_width(warning_high_color_dd, 100);
    lv_obj_align(warning_high_color_dd, LV_ALIGN_CENTER, 80, -47);
    lv_obj_add_style(warning_high_color_dd, get_common_style(), LV_PART_MAIN);

    // Low Warning Threshold
    lv_obj_t * warning_low_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_low_label, "Low Warning:");
    lv_obj_set_style_text_color(warning_low_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_low_label, LV_ALIGN_CENTER, -50, -7);

    lv_obj_t * warning_low_input = lv_textarea_create(ui_MenuScreen);
    lv_obj_add_style(warning_low_input, get_common_style(), LV_PART_MAIN);
    lv_textarea_set_one_line(warning_low_input, true);
    lv_obj_set_width(warning_low_input, 100);
    lv_obj_align(warning_low_input, LV_ALIGN_CENTER, 80, -7);
    lv_obj_add_event_cb(warning_low_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // Low Warning Color
    lv_obj_t * warning_low_color_label = lv_label_create(ui_MenuScreen);
    lv_label_set_text(warning_low_color_label, "Low Color:");
    lv_obj_set_style_text_color(warning_low_color_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(warning_low_color_label, LV_ALIGN_CENTER, -50, 33);

    lv_obj_t * warning_low_color_dd = lv_dropdown_create(ui_MenuScreen);
    lv_dropdown_set_options(warning_low_color_dd, "Red\nBlue");
    lv_obj_set_width(warning_low_color_dd, 100);
    lv_obj_align(warning_low_color_dd, LV_ALIGN_CENTER, 80, 33);
    lv_obj_add_style(warning_low_color_dd, get_common_style(), LV_PART_MAIN);

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
    	
		// Add RPM Gauge Roller
		lv_obj_t * rpm_gauge_roller = lv_roller_create(ui_MenuScreen);
		lv_roller_set_options(rpm_gauge_roller,
   		 "3000\n4000\n5000\n6000\n7000\n8000\n9000\n10000\n11000\n12000",
   		 LV_ROLLER_MODE_NORMAL);
		uint16_t selected_index = 0;
		if (rpm_gauge_max >= 3000 && rpm_gauge_max <= 12000) {
		    selected_index = (rpm_gauge_max - 3000) / 1000;
		}
		lv_roller_set_selected(rpm_gauge_roller, selected_index, LV_ANIM_OFF);
		lv_roller_set_visible_row_count(rpm_gauge_roller, 1);
		lv_obj_set_width(rpm_gauge_roller, 80);
		lv_obj_set_height(rpm_gauge_roller, 35);
		lv_obj_align(rpm_gauge_roller, LV_ALIGN_CENTER, 320, -87); // Position in menu
		lv_obj_add_event_cb(rpm_gauge_roller, rpm_gauge_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
		apply_common_roller_styles(rpm_gauge_roller);
		
		lv_obj_t * rpm_ecu_dropdown_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(rpm_ecu_dropdown_label, "ECU Presets:");
    	lv_obj_set_style_text_color(rpm_ecu_dropdown_label, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(rpm_ecu_dropdown_label, LV_ALIGN_CENTER, -50, -87);
    	
    	lv_obj_t * rpm_ecu_dropdown = lv_dropdown_create(ui_MenuScreen);
    	// Provide the three options
    	lv_dropdown_set_options(rpm_ecu_dropdown, "Custom\nMaxxECU\nHaltech");
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
		
		lv_obj_t * redline_rpm_roller = lv_roller_create(ui_MenuScreen);
		// Create options from 3000 to 12000 in 500 RPM increments (19 options total)
		lv_roller_set_options(redline_rpm_roller,
		    "3000\n3500\n4000\n4500\n5000\n5500\n6000\n6500\n7000\n7500\n8000\n8500\n9000\n9500\n10000\n10500\n11000\n11500\n12000",
		    LV_ROLLER_MODE_NORMAL);
		
		// Set current selection based on redline value
		uint16_t redline_selected_index = 0;
		extern int rpm_redline_value;
		if (rpm_redline_value >= 3000 && rpm_redline_value <= 12000) {
		    redline_selected_index = (rpm_redline_value - 3000) / 500;
		}
		lv_roller_set_selected(redline_rpm_roller, redline_selected_index, LV_ANIM_OFF);
		lv_roller_set_visible_row_count(redline_rpm_roller, 1);
		lv_obj_set_width(redline_rpm_roller, 80);
		lv_obj_set_height(redline_rpm_roller, 35);
		lv_obj_align(redline_rpm_roller, LV_ALIGN_CENTER, 320, -47); // Position below the gauge roller
		lv_obj_add_event_cb(redline_rpm_roller, rpm_redline_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
		apply_common_roller_styles(redline_rpm_roller);

		// Limiter Effect dropdown
		lv_obj_t * limiter_effect_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(limiter_effect_label, "Limiter Effect:");
		lv_obj_set_style_text_color(limiter_effect_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(limiter_effect_label, LV_ALIGN_CENTER, -50, -7); // Position below redline
		lv_obj_set_style_text_align(limiter_effect_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * limiter_effect_dropdown = lv_dropdown_create(ui_MenuScreen);
		lv_obj_add_style(limiter_effect_dropdown, get_common_style(), LV_PART_MAIN);
		lv_dropdown_set_options(limiter_effect_dropdown, "None\nBar Flash\nBar & Circles Flash\nCircles Flash");
		lv_obj_set_width(limiter_effect_dropdown, 120);
		lv_obj_align(limiter_effect_dropdown, LV_ALIGN_CENTER, 80, -7);
		
		// Set current selection based on saved configuration
		uint8_t limiter_effect = values_config[RPM_VALUE_ID - 1].rpm_limiter_effect;
		// Map internal effect type to dropdown index (0=None, 2=Bar Flash, 3=Bar & Circles Flash, 4=Circles Flash)
		uint8_t dropdown_index = 0;
		if (limiter_effect == 2) {
			dropdown_index = 1; // Bar Flash
		} else if (limiter_effect == 3) {
			dropdown_index = 2; // Bar & Circles Flash
		} else if (limiter_effect == 4) {
			dropdown_index = 3; // Circles Flash
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
		
		lv_obj_t * rpm_limiter_roller = lv_roller_create(ui_MenuScreen);
		// Create options from 3000 to 12000 in 500 RPM increments (19 options total)
		lv_roller_set_options(rpm_limiter_roller,
		    "3000\n3500\n4000\n4500\n5000\n5500\n6000\n6500\n7000\n7500\n8000\n8500\n9000\n9500\n10000\n10500\n11000\n11500\n12000",
		    LV_ROLLER_MODE_NORMAL);
		
		// Set current selection based on limiter value
		uint16_t limiter_selected_index = 0;
		int32_t limiter_value = values_config[RPM_VALUE_ID - 1].rpm_limiter_value;
		if (limiter_value >= 3000 && limiter_value <= 12000) {
		    limiter_selected_index = (limiter_value - 3000) / 500;
		}
		lv_roller_set_selected(rpm_limiter_roller, limiter_selected_index, LV_ANIM_OFF);
		lv_roller_set_visible_row_count(rpm_limiter_roller, 1);
		lv_obj_set_width(rpm_limiter_roller, 80);
		lv_obj_set_height(rpm_limiter_roller, 35);
		lv_obj_align(rpm_limiter_roller, LV_ALIGN_CENTER, 320, -7); // Position below redline roller
		lv_obj_add_event_cb(rpm_limiter_roller, rpm_limiter_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
		apply_common_roller_styles(rpm_limiter_roller);

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

		// Gradient toggle
		lv_obj_t * gradient_label = lv_label_create(ui_MenuScreen);
		lv_label_set_text(gradient_label, "Gradient:");
		lv_obj_set_style_text_color(gradient_label, lv_color_hex(0xCCCCCC), 0);
		lv_obj_align(gradient_label, LV_ALIGN_CENTER, 220, 73); // Position next to RPM lights
		lv_obj_set_style_text_align(gradient_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		lv_obj_t * gradient_switch = lv_switch_create(ui_MenuScreen);
		lv_obj_set_size(gradient_switch, 50, 25);
		lv_obj_align(gradient_switch, LV_ALIGN_CENTER, 320, 73);
		
		// Set current state based on saved configuration
		if (values_config[RPM_VALUE_ID - 1].rpm_gradient_enabled) {
		    lv_obj_add_state(gradient_switch, LV_STATE_CHECKED);
		} else {
		    lv_obj_clear_state(gradient_switch, LV_STATE_CHECKED);
		}
		
		// Add event handler for gradient switch
		lv_obj_add_event_cb(gradient_switch, rpm_gradient_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

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
    	
    	// Add GPS/CAN toggle under Customisation header
    	lv_obj_t * speed_source_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(speed_source_label, "Speed Source:");
    	lv_obj_set_style_text_color(speed_source_label, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(speed_source_label, LV_ALIGN_CENTER, -50, -87);
    	lv_obj_set_style_text_align(speed_source_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    	
    	lv_obj_t * speed_source_dropdown = lv_dropdown_create(ui_MenuScreen);
    	lv_obj_add_style(speed_source_dropdown, get_common_style(), LV_PART_MAIN);
    	lv_dropdown_set_options(speed_source_dropdown, "CAN ID\nGPS");
    	lv_obj_set_align(speed_source_dropdown, LV_ALIGN_CENTER);
    	lv_obj_set_width(speed_source_dropdown, 120);
    	lv_obj_set_pos(speed_source_dropdown, 80, -87);
    	
    	// Set current selection based on configuration
    	uint16_t current_selection = values_config[SPEED_VALUE_ID - 1].use_gps_for_speed ? 1 : 0;
    	lv_dropdown_set_selected(speed_source_dropdown, current_selection);
    	
    	// Add event callback for speed source dropdown
    	lv_obj_add_event_cb(speed_source_dropdown, speed_source_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    	
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
    	
    	// GPS Status and Raw Values Display
    	lv_obj_t * gps_status_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(gps_status_label, "GPS Status:");
    	lv_obj_set_style_text_color(gps_status_label, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(gps_status_label, LV_ALIGN_CENTER, -50, 20);  // Back under speed units, lower position
    	lv_obj_set_style_text_align(gps_status_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    	
    	// GPS raw values display - positioned inline with GPS Status label
    	lv_obj_t * gps_values_label = lv_label_create(ui_MenuScreen);
    	lv_obj_set_style_text_color(gps_values_label, lv_color_hex(0x00FF00), LV_PART_MAIN | LV_STATE_DEFAULT);
    	lv_obj_set_style_text_font(gps_values_label, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);
    	lv_obj_align_to(gps_values_label, gps_status_label, LV_ALIGN_OUT_RIGHT_TOP, 10, 0);  // Align inline with GPS Status label
    	lv_obj_set_width(gps_values_label, 300);
    	lv_obj_set_style_text_align(gps_values_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Create GPS status update timer for this menu screen
        lv_timer_t * gps_status_timer = lv_timer_create(gps_status_update_timer_cb, 500, gps_values_label);
        lv_timer_set_repeat_count(gps_status_timer, -1); // Repeat indefinitely

        // Timezone selection
        lv_obj_t * timezone_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(timezone_label, "Timezone:");
        lv_obj_set_style_text_color(timezone_label, lv_color_hex(0xCCCCCC), 0);
        lv_obj_align(timezone_label, LV_ALIGN_CENTER, -50, 80);  // Below GPS status
        lv_obj_set_style_text_align(timezone_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        // Timezone dropdown
        lv_obj_t * timezone_dropdown = lv_dropdown_create(ui_MenuScreen);
        lv_dropdown_set_options(timezone_dropdown, 
            "UTC+0 - London, Dublin\n"
            "UTC+1 - Paris, Berlin, Rome\n"
            "UTC+2 - Athens, Cairo\n"
            "UTC+3 - Moscow, Dubai\n"
            "UTC+4 - Abu Dhabi\n"
            "UTC+5 - Karachi\n"
            "UTC+5:30 - Mumbai, Delhi\n"
            "UTC+6 - Dhaka\n"
            "UTC+7 - Bangkok, Jakarta\n"
            "UTC+8 - Perth, Singapore, HK\n"
            "UTC+9 - Tokyo, Seoul\n"
            "UTC+9:30 - Adelaide, Darwin\n"
            "UTC+10 - Sydney, Melbourne\n"
            "UTC+11 - Sydney (DST)\n"
            "UTC+12 - Auckland\n"
            "UTC-12 - Baker Island\n"
            "UTC-11 - Samoa\n"
            "UTC-10 - Hawaii\n"
            "UTC-9 - Alaska\n"
            "UTC-8 - Los Angeles, Seattle\n"
            "UTC-7 - Denver, Phoenix\n"
            "UTC-6 - Chicago, Dallas\n"
            "UTC-5 - New York, Toronto\n"
            "UTC-4 - Halifax\n"
            "UTC-3 - Buenos Aires\n"
            "UTC-2 - South Georgia\n"
            "UTC-1 - Cape Verde");
        lv_obj_set_size(timezone_dropdown, 250, 35);
        lv_obj_align_to(timezone_dropdown, timezone_label, LV_ALIGN_OUT_RIGHT_TOP, 10, -5);
        lv_obj_set_style_bg_color(timezone_dropdown, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(timezone_dropdown, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(timezone_dropdown, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(timezone_dropdown, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(timezone_dropdown, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        // Daylight Saving Toggle
        lv_obj_t * dst_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(dst_label, "Daylight Saving:");
        lv_obj_set_style_text_color(dst_label, lv_color_hex(0xCCCCCC), 0);
        lv_obj_align(dst_label, LV_ALIGN_CENTER, -50, 115);  // Below timezone
        lv_obj_set_style_text_align(dst_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        lv_obj_t * dst_switch = lv_switch_create(ui_MenuScreen);
        lv_obj_set_size(dst_switch, 50, 25);
        lv_obj_align_to(dst_switch, dst_label, LV_ALIGN_OUT_RIGHT_TOP, 10, -5);
        lv_obj_set_style_bg_color(dst_switch, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(dst_switch, lv_color_hex(0x4CAF50), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(dst_switch, lv_color_hex(0x666666), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(dst_switch, lv_color_white(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        
        // Load saved DST setting
        bool dst_enabled = get_dst_enabled();
        if (dst_enabled) {
            lv_obj_add_state(dst_switch, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(dst_switch, dst_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        
        // Load saved timezone setting
        uint8_t saved_timezone = get_timezone_offset();
        lv_dropdown_set_selected(timezone_dropdown, saved_timezone);
        
        // Add event callback for timezone dropdown
        lv_obj_add_event_cb(timezone_dropdown, timezone_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    } else if (value_id == GEAR_VALUE_ID) {
		create_config_controls(ui_MenuScreen, GEAR_VALUE_ID);
	
		ui_Gear_Label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(ui_Gear_Label, label_texts[GEAR_VALUE_ID - 1]); 
    	lv_obj_set_style_text_color(ui_Gear_Label, lv_color_hex(0xFFFFFF), 0);
    	lv_obj_align(ui_Gear_Label, LV_ALIGN_CENTER, -312, -216);
    	
    	// Create Gear value preview and store global reference for live updates
    	const char *current_gear_value = lv_label_get_text(ui_GEAR_Value);
    	menu_gear_value_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(menu_gear_value_label, current_gear_value ? current_gear_value : " ");
    	lv_obj_set_style_text_color(menu_gear_value_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    	lv_obj_set_style_text_opa(menu_gear_value_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    	lv_obj_set_style_text_font(menu_gear_value_label, &ui_font_Manrope_54_BOLD, LV_PART_MAIN | LV_STATE_DEFAULT);
    	lv_obj_set_style_text_align(menu_gear_value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    	lv_obj_set_style_transform_zoom(menu_gear_value_label, 150, LV_PART_MAIN | LV_STATE_DEFAULT); // Slightly smaller than Screen3
    	lv_obj_set_width(menu_gear_value_label, 80);
    	lv_obj_align(menu_gear_value_label, LV_ALIGN_CENTER, -312, -178);
    	
    	// Add Gear ECU dropdown
    	lv_obj_t * gear_ecu_dropdown_label = lv_label_create(ui_MenuScreen);
    	lv_label_set_text(gear_ecu_dropdown_label, "Gear ECU:");
    	lv_obj_set_style_text_color(gear_ecu_dropdown_label, lv_color_hex(0xCCCCCC), 0);
    	lv_obj_align(gear_ecu_dropdown_label, LV_ALIGN_CENTER, -50, -87);
    	lv_obj_set_style_text_align(gear_ecu_dropdown_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    	
    	lv_obj_t * gear_ecu_dropdown = lv_dropdown_create(ui_MenuScreen);
    	lv_dropdown_set_options(gear_ecu_dropdown, "Custom\nMaxxECU\nHaltech");
    	lv_obj_align(gear_ecu_dropdown, LV_ALIGN_CENTER, 80, -87);
    	lv_obj_set_width(gear_ecu_dropdown, 120);
    	lv_obj_add_style(gear_ecu_dropdown, get_common_style(), LV_PART_MAIN);

    	// Auto-select the saved gear detection mode
    	uint8_t gear_mode = values_config[GEAR_VALUE_ID - 1].gear_detection_mode;
    	
    	// If not in custom mode (0), sync with device settings ECU preconfig
    	if (gear_mode != 0) {
    	    uint8_t device_ecu_preconfig = get_selected_ecu_preconfig();
    	    // Map device settings to gear detection mode: 0=Custom, 1=MaxxECU, 2=Haltech
    	    if (device_ecu_preconfig == 1) {
    	        gear_mode = 1; // MaxxECU
    	        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 1;
    	    } else if (device_ecu_preconfig == 2) {
    	        gear_mode = 2; // Haltech
    	        values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 2;
    	    }
    	    // If device_ecu_preconfig is 0 (Custom), keep current gear_mode
    	}
    	
    	lv_dropdown_set_selected(gear_ecu_dropdown, gear_mode);
    	
    	// Attach event callback for gear ECU dropdown
    	lv_obj_add_event_cb(gear_ecu_dropdown, gear_ecu_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    	
    	// Add Custom Gear Configuration button (only show if Custom mode is selected)
    	if (gear_mode == 0) {
    	    // Custom gear configuration button - darker themed with new text
    	    custom_gear_config_button = lv_btn_create(ui_MenuScreen);
    	    lv_obj_set_size(custom_gear_config_button, 180, 40);
    	    lv_obj_align(custom_gear_config_button, LV_ALIGN_CENTER, 0, -47);
    	    
    	    // Dark theme styling for the button
    	    lv_obj_set_style_bg_color(custom_gear_config_button, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DEFAULT);
    	    lv_obj_set_style_bg_opa(custom_gear_config_button, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    	    lv_obj_set_style_border_color(custom_gear_config_button, lv_color_hex(0x404040), LV_PART_MAIN | LV_STATE_DEFAULT);
    	    lv_obj_set_style_border_width(custom_gear_config_button, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    	    lv_obj_set_style_radius(custom_gear_config_button, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    	    lv_obj_set_style_shadow_width(custom_gear_config_button, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    	    lv_obj_set_style_shadow_color(custom_gear_config_button, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    	    lv_obj_set_style_shadow_opa(custom_gear_config_button, 128, LV_PART_MAIN | LV_STATE_DEFAULT);
    	    
    	    // Hover/pressed states
    	    lv_obj_set_style_bg_color(custom_gear_config_button, lv_color_hex(0x2A2A2A), LV_PART_MAIN | LV_STATE_PRESSED);
    	    lv_obj_set_style_border_color(custom_gear_config_button, lv_color_hex(0x606060), LV_PART_MAIN | LV_STATE_PRESSED);
    	    
    	    lv_obj_t * custom_gear_btn_label = lv_label_create(custom_gear_config_button);
    	    lv_label_set_text(custom_gear_btn_label, "Custom Gear CAN IDs");
    	    lv_obj_set_style_text_color(custom_gear_btn_label, lv_color_hex(0xFFFFFF), 0);
    	    lv_obj_set_style_text_font(custom_gear_btn_label, &lv_font_montserrat_12, 0);
    	    lv_obj_center(custom_gear_btn_label);
    	    
    	    // Add event callback for custom gear button
    	    lv_obj_add_event_cb(custom_gear_config_button, custom_gear_config_btn_event_cb, LV_EVENT_CLICKED, NULL);
    	} else {
    	    // Not in custom mode, ensure button is hidden if it exists and is valid
    	    if (custom_gear_config_button != NULL && lv_obj_is_valid(custom_gear_config_button)) {
    	        lv_obj_add_flag(custom_gear_config_button, LV_OBJ_FLAG_HIDDEN);
    	    } else {
    	        // If button is invalid, reset the pointer
    	        custom_gear_config_button = NULL;
    	    }
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
        lv_label_set_text(bar_min_label, "Bar Min Value:");
        lv_obj_set_style_text_color(bar_min_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_min_label, LV_ALIGN_CENTER, -50, -87);
      
        lv_obj_t * bar_min_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_min_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_min_input, true);
        lv_obj_set_width(bar_min_input, 100);
        lv_obj_align(bar_min_input, LV_ALIGN_CENTER, 80, -87);
        lv_obj_add_event_cb(bar_min_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_set_user_data(bar_min_input, (void*)1); // Mark as min input
      
        // Bar Maximum input
        lv_obj_t * bar_max_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_max_label, "Bar Max Value:");
        lv_obj_set_style_text_color(bar_max_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_max_label, LV_ALIGN_CENTER, -50, -47);
      
        lv_obj_t * bar_max_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_max_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_max_input, true);
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
        snprintf(buf, sizeof(buf), "%d", values_config[value_id - 1].bar_min);
        lv_textarea_set_text(bar_min_input, buf);
        snprintf(buf, sizeof(buf), "%d", values_config[value_id - 1].bar_max);
        lv_textarea_set_text(bar_max_input, buf);
        
        // NEW: Create "Bar Low Value:" label and input field
        lv_obj_t * bar_low_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_low_label, "Bar Low Value:");
        lv_obj_set_style_text_color(bar_low_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_low_label, LV_ALIGN_CENTER, -50, -7);
      
        lv_obj_t * bar_low_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_low_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_low_input, true);
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
        
        // NEW: Create "Bar High Value:" label and input field
        lv_obj_t * bar_high_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_high_label, "Bar High Value:");
        lv_obj_set_style_text_color(bar_high_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_high_label, LV_ALIGN_CENTER, -50, 33);   // Adjust vertical positioning as needed
     
        lv_obj_t * bar_high_input = lv_textarea_create(ui_MenuScreen);
        lv_obj_add_style(bar_high_input, get_common_style(), LV_PART_MAIN);
        lv_textarea_set_one_line(bar_high_input, true);
        lv_obj_set_width(bar_high_input, 100);
        lv_obj_align(bar_high_input, LV_ALIGN_CENTER, 80, 33);    // Adjust vertical positioning as needed
        lv_obj_add_event_cb(bar_high_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
     
        uint8_t *bar_high_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_high_id_ptr = value_id;
        lv_obj_add_event_cb(bar_high_input, bar_high_value_event_cb, LV_EVENT_VALUE_CHANGED, bar_high_id_ptr);
        lv_obj_add_event_cb(bar_high_input, free_value_id_event_cb, LV_EVENT_DELETE, bar_high_id_ptr);
     
        char buf_high[16];
        snprintf(buf_high, sizeof(buf_high), "%d", values_config[value_id - 1].bar_high);
        lv_textarea_set_text(bar_high_input, buf_high);
        
        // Bar Color Configuration
        // Bar Low Color
        lv_obj_t * bar_low_color_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_low_color_label, "Bar Low Colour:");
        lv_obj_set_style_text_color(bar_low_color_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_low_color_label, LV_ALIGN_CENTER, -50, 73);

        lv_obj_t * bar_low_color_dropdown = lv_dropdown_create(ui_MenuScreen);
        lv_dropdown_set_options(bar_low_color_dropdown, 
            "Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom");
        lv_obj_set_width(bar_low_color_dropdown, 100);
        lv_obj_align(bar_low_color_dropdown, LV_ALIGN_CENTER, 80, 73);
        lv_obj_add_style(bar_low_color_dropdown, get_common_style(), LV_PART_MAIN);

        // Bar High Color
        lv_obj_t * bar_high_color_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_high_color_label, "Bar High Colour:");
        lv_obj_set_style_text_color(bar_high_color_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_high_color_label, LV_ALIGN_CENTER, -50, 113);

        lv_obj_t * bar_high_color_dropdown = lv_dropdown_create(ui_MenuScreen);
        lv_dropdown_set_options(bar_high_color_dropdown, 
            "Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom");
        lv_obj_set_width(bar_high_color_dropdown, 100);
        lv_obj_align(bar_high_color_dropdown, LV_ALIGN_CENTER, 80, 113);
        lv_obj_add_style(bar_high_color_dropdown, get_common_style(), LV_PART_MAIN);

        // Bar In Range Color
        lv_obj_t * bar_in_range_color_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(bar_in_range_color_label, "Bar In Range Colour:");
        lv_obj_set_style_text_color(bar_in_range_color_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(bar_in_range_color_label, LV_ALIGN_CENTER, -50, 153);

        lv_obj_t * bar_in_range_color_dropdown = lv_dropdown_create(ui_MenuScreen);
        lv_dropdown_set_options(bar_in_range_color_dropdown, 
            "Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom");
        lv_obj_set_width(bar_in_range_color_dropdown, 100);
        lv_obj_align(bar_in_range_color_dropdown, LV_ALIGN_CENTER, 80, 153);
        lv_obj_add_style(bar_in_range_color_dropdown, get_common_style(), LV_PART_MAIN);

        // Set current color selections based on saved values
        // Determine which color index to select for each dropdown
        uint16_t low_color_selected = 0; // Default to Blue
        uint16_t high_color_selected = 1; // Default to Red
        uint16_t in_range_color_selected = 2; // Default to Green
        
        // Check bar low color
        if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x19439a).full) low_color_selected = 0; // Blue
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFF0000).full) low_color_selected = 1; // Red
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x38FF00).full) low_color_selected = 2; // Green
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFFFF00).full) low_color_selected = 3; // Yellow
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFF7F00).full) low_color_selected = 4; // Orange
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x8000FF).full) low_color_selected = 5; // Purple
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0x00FFFF).full) low_color_selected = 6; // Cyan
        else if (values_config[value_id - 1].bar_low_color.full == lv_color_hex(0xFF00FF).full) low_color_selected = 7; // Magenta
        else low_color_selected = 8; // Custom
        
        // Check bar high color
        if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x19439a).full) high_color_selected = 0; // Blue
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFF0000).full) high_color_selected = 1; // Red
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x38FF00).full) high_color_selected = 2; // Green
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFFFF00).full) high_color_selected = 3; // Yellow
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFF7F00).full) high_color_selected = 4; // Orange
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x8000FF).full) high_color_selected = 5; // Purple
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0x00FFFF).full) high_color_selected = 6; // Cyan
        else if (values_config[value_id - 1].bar_high_color.full == lv_color_hex(0xFF00FF).full) high_color_selected = 7; // Magenta
        else high_color_selected = 8; // Custom
        
        // Check bar in range color
        if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x19439a).full) in_range_color_selected = 0; // Blue
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFF0000).full) in_range_color_selected = 1; // Red
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x38FF00).full) in_range_color_selected = 2; // Green
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFFFF00).full) in_range_color_selected = 3; // Yellow
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFF7F00).full) in_range_color_selected = 4; // Orange
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x8000FF).full) in_range_color_selected = 5; // Purple
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0x00FFFF).full) in_range_color_selected = 6; // Cyan
        else if (values_config[value_id - 1].bar_in_range_color.full == lv_color_hex(0xFF00FF).full) in_range_color_selected = 7; // Magenta
        else in_range_color_selected = 8; // Custom
        
        lv_dropdown_set_selected(bar_low_color_dropdown, low_color_selected);
        lv_dropdown_set_selected(bar_high_color_dropdown, high_color_selected);
        lv_dropdown_set_selected(bar_in_range_color_dropdown, in_range_color_selected);

        // Add event callbacks for color dropdowns
        uint8_t *bar_low_color_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_low_color_id_ptr = value_id;
        lv_obj_add_event_cb(bar_low_color_dropdown, bar_low_color_event_cb, LV_EVENT_VALUE_CHANGED, bar_low_color_id_ptr);
        lv_obj_add_event_cb(bar_low_color_dropdown, free_value_id_event_cb, LV_EVENT_DELETE, bar_low_color_id_ptr);

        uint8_t *bar_high_color_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_high_color_id_ptr = value_id;
        lv_obj_add_event_cb(bar_high_color_dropdown, bar_high_color_event_cb, LV_EVENT_VALUE_CHANGED, bar_high_color_id_ptr);
        lv_obj_add_event_cb(bar_high_color_dropdown, free_value_id_event_cb, LV_EVENT_DELETE, bar_high_color_id_ptr);

        uint8_t *bar_in_range_color_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *bar_in_range_color_id_ptr = value_id;
        lv_obj_add_event_cb(bar_in_range_color_dropdown, bar_in_range_color_event_cb, LV_EVENT_VALUE_CHANGED, bar_in_range_color_id_ptr);
        lv_obj_add_event_cb(bar_in_range_color_dropdown, free_value_id_event_cb, LV_EVENT_DELETE, bar_in_range_color_id_ptr);
        
        // Fuel Input Controls (move to top right, next to bar min/max etc)
        int fuel_x = 220; // right of value fields
        int fuel_y = -87; // align with bar min value
        int fuel_y_step = 40;

        // Fuel Input Enable Label and Switch
        lv_obj_t * fuel_enable_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(fuel_enable_label, "Use Fuel Input:");
        lv_obj_set_style_text_color(fuel_enable_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(fuel_enable_label, LV_ALIGN_CENTER, fuel_x, fuel_y);

        lv_obj_t * fuel_enable_switch = lv_switch_create(ui_MenuScreen);
        lv_obj_align(fuel_enable_switch, LV_ALIGN_CENTER, fuel_x + 120, fuel_y);
        lv_obj_set_size(fuel_enable_switch, 50, 25);
        if (values_config[value_id - 1].use_fuel_input) {
            lv_obj_add_state(fuel_enable_switch, LV_STATE_CHECKED);
        }
        uint8_t *fuel_enable_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *fuel_enable_id_ptr = value_id;
        lv_obj_add_event_cb(fuel_enable_switch, fuel_enable_switch_event_cb, LV_EVENT_VALUE_CHANGED, fuel_enable_id_ptr);
        lv_obj_add_event_cb(fuel_enable_switch, free_value_id_event_cb, LV_EVENT_DELETE, fuel_enable_id_ptr);

        // Fuel Calibration Section
        lv_obj_t * fuel_calib_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(fuel_calib_label, "Fuel Calibration:");
        lv_obj_set_style_text_color(fuel_calib_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(fuel_calib_label, LV_ALIGN_CENTER, fuel_x, fuel_y + fuel_y_step);

        // Empty Calibration Button
        lv_obj_t * fuel_empty_btn = lv_btn_create(ui_MenuScreen);
        lv_obj_set_size(fuel_empty_btn, 80, 30);
        lv_obj_align(fuel_empty_btn, LV_ALIGN_CENTER, fuel_x, fuel_y + 2 * fuel_y_step);
        lv_obj_t * fuel_empty_btn_label = lv_label_create(fuel_empty_btn);
        lv_label_set_text(fuel_empty_btn_label, "EMPTY");
        lv_obj_center(fuel_empty_btn_label);

        // Full Calibration Button
        lv_obj_t * fuel_full_btn = lv_btn_create(ui_MenuScreen);
        lv_obj_set_size(fuel_full_btn, 80, 30);
        lv_obj_align(fuel_full_btn, LV_ALIGN_CENTER, fuel_x + 100, fuel_y + 2 * fuel_y_step);
        lv_obj_t * fuel_full_btn_label = lv_label_create(fuel_full_btn);
        lv_label_set_text(fuel_full_btn_label, "FULL");
        lv_obj_center(fuel_full_btn_label);

        // Voltage Display Labels
        lv_obj_t * fuel_empty_voltage_label = lv_label_create(ui_MenuScreen);
        char empty_voltage_text[32];
        snprintf(empty_voltage_text, sizeof(empty_voltage_text), "Empty: %.3fV", values_config[value_id - 1].fuel_empty_voltage);
        lv_label_set_text(fuel_empty_voltage_label, empty_voltage_text);
        lv_obj_set_style_text_color(fuel_empty_voltage_label, lv_color_hex(0xCCCCCC), 0);
        lv_obj_align(fuel_empty_voltage_label, LV_ALIGN_CENTER, fuel_x, fuel_y + 3 * fuel_y_step);

        lv_obj_t * fuel_full_voltage_label = lv_label_create(ui_MenuScreen);
        char full_voltage_text[32];
        snprintf(full_voltage_text, sizeof(full_voltage_text), "Full: %.3fV", values_config[value_id - 1].fuel_full_voltage);
        lv_label_set_text(fuel_full_voltage_label, full_voltage_text);
        lv_obj_set_style_text_color(fuel_full_voltage_label, lv_color_hex(0xCCCCCC), 0);
        lv_obj_align(fuel_full_voltage_label, LV_ALIGN_CENTER, fuel_x + 100, fuel_y + 3 * fuel_y_step);

        // Current Voltage Display
        lv_obj_t * fuel_current_voltage_label = lv_label_create(ui_MenuScreen);
        lv_label_set_text(fuel_current_voltage_label, "Current: 0.000V");
        lv_obj_set_style_text_color(fuel_current_voltage_label, lv_color_hex(0x00FF00), 0);
        lv_obj_align(fuel_current_voltage_label, LV_ALIGN_CENTER, fuel_x + 50, fuel_y + 4 * fuel_y_step);

        // Start timer for periodic voltage updates (update every 500ms)
        if (values_config[value_id - 1].use_fuel_input) {
            lv_timer_t * voltage_timer = lv_timer_create(fuel_voltage_update_timer_cb, 500, fuel_current_voltage_label);
            lv_timer_ready(voltage_timer); // Update immediately
        }

        // Add event callbacks for calibration buttons
        uint8_t *fuel_empty_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *fuel_empty_id_ptr = value_id;
        lv_obj_add_event_cb(fuel_empty_btn, fuel_empty_calib_event_cb, LV_EVENT_CLICKED, fuel_empty_id_ptr);
        lv_obj_add_event_cb(fuel_empty_btn, free_value_id_event_cb, LV_EVENT_DELETE, fuel_empty_id_ptr);

        uint8_t *fuel_full_id_ptr = lv_mem_alloc(sizeof(uint8_t));
        *fuel_full_id_ptr = value_id;
        lv_obj_add_event_cb(fuel_full_btn, fuel_full_calib_event_cb, LV_EVENT_CLICKED, fuel_full_id_ptr);
        lv_obj_add_event_cb(fuel_full_btn, free_value_id_event_cb, LV_EVENT_DELETE, fuel_full_id_ptr);

        // Initially hide fuel calibration controls if fuel input is disabled
        if (!values_config[value_id - 1].use_fuel_input) {
            lv_obj_add_flag(fuel_calib_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fuel_empty_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fuel_full_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fuel_empty_voltage_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fuel_full_voltage_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fuel_current_voltage_label, LV_OBJ_FLAG_HIDDEN);
        }
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
    
    // Label (store in global array for live label updates)
    menu_panel_labels[idx] = lv_label_create(parent);
    lv_label_set_text(menu_panel_labels[idx], label_texts[idx]);
    lv_obj_set_style_text_color(menu_panel_labels[idx], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(menu_panel_labels[idx], &ui_font_fugaz_14, 0);
    lv_obj_set_x(menu_panel_labels[idx], -312);
    lv_obj_set_y(menu_panel_labels[idx], -216);
    lv_obj_set_align(menu_panel_labels[idx], LV_ALIGN_CENTER);

    // Value (store in global array for live updates)
    menu_panel_value_labels[idx] = lv_label_create(parent);
    // Initialize with current Screen3 value if available
    const char *current_value = (ui_Value[idx] && lv_obj_is_valid(ui_Value[idx])) ? 
                                lv_label_get_text(ui_Value[idx]) : "0";
    lv_label_set_text(menu_panel_value_labels[idx], current_value);
    lv_obj_set_style_text_color(menu_panel_value_labels[idx], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(menu_panel_value_labels[idx], &ui_font_Manrope_35_BOLD, 0);
    lv_obj_set_x(menu_panel_value_labels[idx], -312);
    lv_obj_set_y(menu_panel_value_labels[idx], -178);
    lv_obj_set_align(menu_panel_value_labels[idx], LV_ALIGN_CENTER);

    // Box (store reference for border effects)
    menu_panel_boxes[idx] = lv_obj_create(parent);
    lv_obj_set_size(menu_panel_boxes[idx], 155, 92);
    lv_obj_set_pos(menu_panel_boxes[idx], -312, -185);
    lv_obj_set_align(menu_panel_boxes[idx], LV_ALIGN_CENTER);
    lv_obj_clear_flag(menu_panel_boxes[idx], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(menu_panel_boxes[idx], get_box_style(), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Set initial border properties for warning effects
    lv_obj_set_style_border_width(menu_panel_boxes[idx], 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(menu_panel_boxes[idx], lv_color_hex(0x2e2f2e), LV_PART_MAIN | LV_STATE_DEFAULT);

    // Add configuration controls for any value
    create_config_controls(parent, value_id);
}

// Speed source dropdown event callback
void speed_source_dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    
    // Update the speed configuration
    values_config[SPEED_VALUE_ID - 1].use_gps_for_speed = (selected == 1);
    
    // Save configuration to NVS immediately
    save_values_config_to_nvs();
    
    ESP_LOGI("MENU", "Speed source changed to: %s (saved to NVS)", 
        values_config[SPEED_VALUE_ID - 1].use_gps_for_speed ? "GPS" : "CAN ID");
    
    // If GPS is selected, immediately trigger a speed update to show "---" if no fix
    if (values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) {
        // Create a speed update to show "---" immediately while GPS searches for fix
        speed_update_t *s_upd = malloc(sizeof(speed_update_t));
        if (s_upd) {
            strcpy(s_upd->speed_str, "---");
            lv_async_call(update_speed_ui, s_upd);
        }
    }
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

// GPS status update timer callback for menu screen
void gps_status_update_timer_cb(lv_timer_t * timer) {
    lv_obj_t * gps_label = (lv_obj_t *)timer->user_data;
    if (!gps_label || !lv_obj_is_valid(gps_label)) {
        return;
    }
    
    gps_data_t gps_data;
    char status_text[400];
    
    // Speed preview is handled through the status text display below
    
    if (gps_get_data(&gps_data)) {
        if (gps_data.fix_valid) {
            // Show GPS speed in preview if GPS is selected
            if (values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) {
                float display_speed = gps_data.speed_kmh;
                const char* units = "km/h";
                
                // Convert to MPH if selected
                if (values_config[SPEED_VALUE_ID - 1].use_mph) {
                    display_speed = display_speed * 0.621371f;
                    units = "mph";
                }
                
                snprintf(status_text, sizeof(status_text), 
                    "GPS: VALID FIX (%lu baud)\n"
                    "Mode: Ultra Low Latency\n"
                    "Speed: %.1f %s [ACTIVE]\n"
                    "Satellites: %d\n"
                    "Position: %.4f, %.4f",
                    gps_data.detected_baud_rate,
                    display_speed,
                    units,
                    gps_data.satellites,
                    gps_data.latitude,
                    gps_data.longitude);
            } else {
                float display_speed = gps_data.speed_kmh;
                const char* units = "km/h";
                
                // Convert to MPH if selected (for display consistency)
                if (values_config[SPEED_VALUE_ID - 1].use_mph) {
                    display_speed = display_speed * 0.621371f;
                    units = "mph";
                }
                
                snprintf(status_text, sizeof(status_text), 
                    "GPS: VALID FIX (%lu baud)\n"
                    "Mode: Ultra Low Latency\n"
                    "Speed: %.1f %s\n"
                    "Satellites: %d\n"
                    "Position: %.4f, %.4f",
                    gps_data.detected_baud_rate,
                    display_speed,
                    units,
                    gps_data.satellites,
                    gps_data.latitude,
                    gps_data.longitude);
            }
            lv_obj_set_style_text_color(gps_label, lv_color_hex(0x00FF00), LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            // No GPS fix
            if (values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) {
                snprintf(status_text, sizeof(status_text), 
                    "GPS: NO FIX (%lu baud)\n"
                    "Mode: Ultra Low Latency\n"
                    "Speed: --- [ACTIVE]\n"
                    "Satellites: %d\n"
                    "Searching for fix...",
                    gps_data.detected_baud_rate,
                    gps_data.satellites);
            } else {
                snprintf(status_text, sizeof(status_text), 
                    "GPS: NO FIX (%lu baud)\n"
                    "Mode: Ultra Low Latency\n"
                    "Satellites: %d\n"
                    "Searching for fix...",
                    gps_data.detected_baud_rate,
                    gps_data.satellites);
            }
            lv_obj_set_style_text_color(gps_label, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        // GPS not available
        if (values_config[SPEED_VALUE_ID - 1].use_gps_for_speed) {
            snprintf(status_text, sizeof(status_text), 
                "GPS: NOT AVAILABLE\n"
                "Speed: --- [ACTIVE]\n"
                "Check connections");
        } else {
            snprintf(status_text, sizeof(status_text), 
                "GPS: NOT AVAILABLE\n"
                "Check connections");
        }
        lv_obj_set_style_text_color(gps_label, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    
    lv_label_set_text(gps_label, status_text);
}

// Custom gear configuration button event callback
void custom_gear_config_btn_event_cb(lv_event_t * e) {
    ESP_LOGI("GEAR", "Opening custom gear configuration screen");
    create_custom_gear_config_menu();
}

// Timer callback for refreshing menu after fuel input toggle
static void fuel_menu_refresh_timer_cb(lv_timer_t * timer) {
    // Get the value_id from timer user data
    uint8_t value_id = (uint8_t)(uintptr_t)timer->user_data;
    
    // Close and reopen the menu to refresh the fuel controls
    if (lv_scr_act() && lv_scr_act() == ui_MenuScreen) {
        load_menu_screen_for_value(value_id);
    }
    
    // Delete the timer after use
    lv_timer_del(timer);
}

// Fuel input enable switch event callback
void fuel_enable_switch_event_cb(lv_event_t * e) {
    lv_obj_t * switch_obj = lv_event_get_target(e);
    uint8_t *value_id_ptr = (uint8_t *)lv_event_get_user_data(e);
    uint8_t value_id = *value_id_ptr;
    
    bool is_enabled = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
    values_config[value_id - 1].use_fuel_input = is_enabled;
    
    // Simple approach: just set a flag to refresh the entire menu screen
    // This avoids complex child searching that can cause watchdog timeouts
    ESP_LOGI("FUEL", "Fuel input %s for bar %d - refreshing menu", 
             is_enabled ? "enabled" : "disabled", value_id);
    
    // Save configuration to NVS (this should be quick)
    save_values_config_to_nvs();
    
    // Start or stop fuel update timer based on whether any bar has fuel input enabled
    if (any_fuel_input_enabled()) {
        start_fuel_update_timer();
    } else {
        stop_fuel_update_timer();
    }
    
    // Create a small delay timer to refresh the menu after a short delay
    // This allows the current event to complete before doing heavy UI work
    lv_timer_t * refresh_timer = lv_timer_create(fuel_menu_refresh_timer_cb, 50, (void*)(uintptr_t)value_id);
    lv_timer_set_repeat_count(refresh_timer, 1); // Run only once
}

// Fuel empty calibration button event callback
void fuel_empty_calib_event_cb(lv_event_t * e) {
    uint8_t *value_id_ptr = (uint8_t *)lv_event_get_user_data(e);
    uint8_t value_id = *value_id_ptr;
    
    // Read current voltage and store as empty voltage
    float current_voltage = fuel_input_read_voltage();
    values_config[value_id - 1].fuel_empty_voltage = current_voltage;
    
    // Update voltage display
    lv_obj_t * parent = lv_obj_get_parent(lv_event_get_target(e));
    if (parent) {
        lv_obj_t * child = lv_obj_get_child(parent, -1);
        while (child) {
            if (lv_obj_get_class(child) == &lv_label_class) {
                const char *text = lv_label_get_text(child);
                if (strstr(text, "Empty:")) {
                    char empty_voltage_text[32];
                    snprintf(empty_voltage_text, sizeof(empty_voltage_text), "Empty: %.3fV", current_voltage);
                    lv_label_set_text(child, empty_voltage_text);
                    break;
                }
            }
            child = lv_obj_get_child(parent, lv_obj_get_index(child) - 1);
        }
    }
    
    // Save configuration to NVS
    save_values_config_to_nvs();
    
    ESP_LOGI("FUEL", "Empty calibration set to %.3fV for bar %d", current_voltage, value_id);
}

// Fuel full calibration button event callback
void fuel_full_calib_event_cb(lv_event_t * e) {
    uint8_t *value_id_ptr = (uint8_t *)lv_event_get_user_data(e);
    uint8_t value_id = *value_id_ptr;
    
    // Read current voltage and store as full voltage
    float current_voltage = fuel_input_read_voltage();
    values_config[value_id - 1].fuel_full_voltage = current_voltage;
    
    // Update voltage display
    lv_obj_t * parent = lv_obj_get_parent(lv_event_get_target(e));
    if (parent) {
        lv_obj_t * child = lv_obj_get_child(parent, -1);
        while (child) {
            if (lv_obj_get_class(child) == &lv_label_class) {
                const char *text = lv_label_get_text(child);
                if (strstr(text, "Full:")) {
                    char full_voltage_text[32];
                    snprintf(full_voltage_text, sizeof(full_voltage_text), "Full: %.3fV", current_voltage);
                    lv_label_set_text(child, full_voltage_text);
                    break;
                }
            }
            child = lv_obj_get_child(parent, lv_obj_get_index(child) - 1);
        }
    }
    
    // Save configuration to NVS
    save_values_config_to_nvs();
    
    ESP_LOGI("FUEL", "Full calibration set to %.3fV for bar %d", current_voltage, value_id);
}

// Fuel voltage update timer callback
void fuel_voltage_update_timer_cb(lv_timer_t * timer) {
    lv_obj_t * voltage_label = (lv_obj_t *)timer->user_data;
    if (!voltage_label || !lv_obj_is_valid(voltage_label)) {
        // Timer's target object is invalid, delete timer
        lv_timer_del(timer);
        return;
    }
    
    // Read current voltage and update label
    float current_voltage = fuel_input_read_voltage();
    char voltage_text[32];
    snprintf(voltage_text, sizeof(voltage_text), "Current: %.3fV", current_voltage);
    lv_label_set_text(voltage_label, voltage_text);
}
