#include "widget_gear.h"
#include "widget_dispatcher.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "can/can_decode.h"
#include "can/can_dispatch.h"
#include "storage/config_store.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/menu/menu_screen.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/callbacks/ui_callbacks.h"

uint64_t last_gear_can_received = 0;

/* Global LVGL object definitions (declared extern in widget_gear.h) */
lv_obj_t *ui_Gear_Label = NULL;

static bool should_show_gear_icon(uint32_t raw_value);
void update_gear_ui_immediate(const char *gear_str, uint32_t raw_value);

static lv_obj_t *custom_gear_can_inputs[10] = {NULL}; // N, R, 1-8
static lv_obj_t *speed_rpm_tire_circumference_input = NULL;
static lv_obj_t *speed_rpm_final_drive_input = NULL;
static lv_obj_t *speed_rpm_reverse_ratio_input = NULL; // Reverse gear ratio
static lv_obj_t *speed_rpm_gear_ratio_inputs[10] = {NULL}; // Gear ratios for 1-10
static bool should_show_gear_icon(uint32_t raw_value) {
	// Check if custom gear mode is enabled
	if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode != 0) {
		return false; // Only custom mode supports icons
	}
	
	// Check each custom icon value
	for (int i = 0; i < 7; i++) {
		uint32_t icon_value = values_config[GEAR_VALUE_ID - 1].custom_icon_values[i];
		uint8_t icon_type = values_config[GEAR_VALUE_ID - 1].custom_icon_types[i];
		
		// Check if this icon is KEY type (1) and is configured (not UINT32_MAX)
		// Allow 0 as a valid configured value
		if (icon_type == 1 && icon_value != UINT32_MAX) {
			// Exact match required for icon display
			if (icon_value == raw_value) {
				return true;
			}
		}
	}
	return false;
}

// Asynchronous callback for updating the Gear label
void update_gear_ui(void *param) {
	gear_update_t *g_upd = (gear_update_t *)param;

	if (ui_GEAR_Value == NULL || lv_obj_get_screen(ui_GEAR_Value) == NULL) {
		free(g_upd);
		return;
	}

	// Check if we should show icon instead of text
	bool show_icon = should_show_gear_icon(g_upd->raw_value);
	
	if (show_icon) {
		// Show icon, hide text
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_clear_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_add_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
	} else {
		// Show text, hide icon
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_add_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_clear_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(ui_GEAR_Value, g_upd->gear_str);
	}

	// Also update menu preview if it exists, is valid, and menu is visible
	if (menu_gear_value_label && lv_obj_is_valid(menu_gear_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		// Update menu icon if it exists
		extern lv_obj_t * menu_gear_icon;
		if (menu_gear_icon && lv_obj_is_valid(menu_gear_icon)) {
			if (show_icon) {
				lv_obj_clear_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_add_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_clear_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
				lv_label_set_text(menu_gear_value_label, g_upd->gear_str);
			}
		} else {
			lv_label_set_text(menu_gear_value_label, g_upd->gear_str);
		}
	}

	free(g_upd);
}

// Immediate gear update
void update_gear_ui_immediate(const char *gear_str, uint32_t raw_value) {
	if (ui_GEAR_Value == NULL || lv_obj_get_screen(ui_GEAR_Value) == NULL) {
		return;
	}
	
	// Check if we should show icon instead of text
	bool show_icon = should_show_gear_icon(raw_value);
	
	if (show_icon) {
		// Show icon, hide text
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_clear_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_add_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
	} else {
		// Show text, hide icon
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_add_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_clear_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(ui_GEAR_Value, gear_str);
	}
	
	if (menu_gear_value_label && lv_obj_is_valid(menu_gear_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		// Update menu icon if it exists
		extern lv_obj_t * menu_gear_icon;
		if (menu_gear_icon && lv_obj_is_valid(menu_gear_icon)) {
			if (show_icon) {
				lv_obj_clear_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_add_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_clear_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
				lv_label_set_text(menu_gear_value_label, gear_str);
			}
		} else {
			lv_label_set_text(menu_gear_value_label, gear_str);
		}
	}
}
void speed_rpm_gear_update_timer_cb(lv_timer_t *timer) {
	// Only calculate gear if Speed/RPM Ratio mode is selected
	if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode != 4) {
		return;
	}

	// Get current RPM and Speed values (reset each time to avoid stale data)
	int current_rpm = 0;
	float current_speed = 0.0f;
	bool rpm_valid = false;
	bool speed_valid = false;
	
	// Parse RPM from previous_values array (it's stored as a string)
	if (strlen(previous_values[RPM_VALUE_ID - 1]) > 0 && 
		strcmp(previous_values[RPM_VALUE_ID - 1], "---") != 0 &&
		strcmp(previous_values[RPM_VALUE_ID - 1], "-") != 0) {
		current_rpm = atoi(previous_values[RPM_VALUE_ID - 1]);
		if (current_rpm > 0) {
			rpm_valid = true;
		}
	}
	
	// Parse Speed from previous_values array (it's stored as a string)
	if (strlen(previous_values[SPEED_VALUE_ID - 1]) > 0 && 
		strcmp(previous_values[SPEED_VALUE_ID - 1], "---") != 0 &&
		strcmp(previous_values[SPEED_VALUE_ID - 1], "-") != 0) {
		current_speed = atof(previous_values[SPEED_VALUE_ID - 1]);
		if (current_speed >= 0) {
			speed_valid = true;
		}
	}
	
	// Safety checks: need valid RPM and speed to calculate gear
	if (!rpm_valid || !speed_valid || current_rpm <= 500 || current_speed < 5.0f) {
		// Display "N" if not enough data or vehicle is stationary/idle
		char gear_str[EXAMPLE_MAX_CHAR_SIZE];
		snprintf(gear_str, sizeof(gear_str), "N");
		if (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0) {
			strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
			gear_update_t *g_upd = malloc(sizeof(gear_update_t));
			if (g_upd) {
				strcpy(g_upd->gear_str, gear_str);
				g_upd->raw_value = 0; // Speed/RPM ratio mode - no raw CAN value
				lv_async_call(update_gear_ui, g_upd);
			}
		}
		return;
	}
	
	// Convert speed to km/h if it's in MPH
	float speed_kmh = current_speed;
	if (values_config[SPEED_VALUE_ID - 1].use_mph) {
		speed_kmh = current_speed / 0.621371f; // Convert MPH to KMH
	}
	
	// Get configuration
	float tire_circ_mm = values_config[GEAR_VALUE_ID - 1].tire_circumference_mm;
	float final_drive = values_config[GEAR_VALUE_ID - 1].final_drive_ratio;
	
	// Safety check for configuration
	if (tire_circ_mm <= 0 || final_drive <= 0) {
		ESP_LOGW("GEAR", "Speed/RPM Ratio mode enabled but tire/final drive not configured!");
		return;
	}
	
	// Calculate current overall gear ratio
	// Formula: gear_ratio = (RPM * tire_circumference_mm * 60) / (speed_kph * final_drive_ratio * 1000000)
	// This simplifies to: gear_ratio = (RPM * tire_circ_mm * 0.00006) / (speed_kph * final_drive)
	float current_ratio = (current_rpm * tire_circ_mm * 0.00006f) / (speed_kmh * final_drive);
	
	// Find the closest matching gear by comparing ratios
	// Check reverse first (negative speed indicates reverse, but we'll check ratio anyway)
	int best_gear = -1;
	float smallest_diff = 999999.0f;
	static int last_gear = -1; // Remember last gear for hysteresis (-1 = reverse, 0 = neutral, 1-10 = forward gears)
	
	// Check reverse gear first
	float reverse_ratio = values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio;
	if (reverse_ratio > 0.001f) {
		float diff = fabsf(current_ratio - reverse_ratio);
		float tolerance = reverse_ratio * 0.25f;
		
		// Add hysteresis: if this was the previous gear, give it preference
		if (last_gear == -1) {
			diff *= 0.8f;
		}
		
		if (diff < smallest_diff && diff < tolerance) {
			smallest_diff = diff;
			best_gear = -1; // -1 represents reverse
		}
	}
	
	// Check forward gears (1-10)
	for (int i = 0; i < 10; i++) {
		float gear_ratio = values_config[GEAR_VALUE_ID - 1].gear_ratios[i];
		
		// Skip gears with 0 ratio (not configured)
		if (gear_ratio <= 0.001f) {
			continue;
		}
		
		float diff = fabsf(current_ratio - gear_ratio);
		
		// Allow 25% tolerance for ratio matching (increased from 15% for better stability)
		float tolerance = gear_ratio * 0.25f;
		
		// Add hysteresis: if this was the previous gear, give it preference (reduce its diff by 20%)
		if (i + 1 == last_gear) {
			diff *= 0.8f;
		}
		
		if (diff < smallest_diff && diff < tolerance) {
			smallest_diff = diff;
			best_gear = i + 1; // Gear numbers are 1-10
		}
	}
	
	// Debug logging (can be removed later)
	static int log_counter = 0;
	if (log_counter++ % 10 == 0) { // Log every 10th calculation (every 2 seconds)
		ESP_LOGI("GEAR", "Speed/RPM Ratio: RPM=%d, Speed=%.1f km/h, Ratio=%.3f, Gear=%d", 
				 current_rpm, speed_kmh, current_ratio, best_gear);
	}
	
	// Update gear display
	char gear_str[EXAMPLE_MAX_CHAR_SIZE];
	if (best_gear == -1) {
		snprintf(gear_str, sizeof(gear_str), "R"); // Reverse
		last_gear = -1;
	} else if (best_gear > 0) {
		snprintf(gear_str, sizeof(gear_str), "%d", best_gear);
		last_gear = best_gear; // Remember this gear for next time
	} else {
		snprintf(gear_str, sizeof(gear_str), "-"); // No matching gear found
		last_gear = -1;
	}
	
	// Only update if gear changed
	if (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0) {
		strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
		gear_update_t *g_upd = malloc(sizeof(gear_update_t));
		if (g_upd) {
			strcpy(g_upd->gear_str, gear_str);
			g_upd->raw_value = 0; // Speed/RPM ratio mode - no raw CAN value
			lv_async_call(update_gear_ui, g_upd);
		}
	}
}
void gear_ecu_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	// Indices: 0 = Custom, 1 = MaxxECU, 2 = Haltech, 3 = Ford, 4 = Speed/RPM Ratio

	// External functions from menu_screen.c
	extern void create_custom_gear_values_section(lv_obj_t * parent,
												  uint8_t gear_mode);
	extern void hide_custom_gear_values_section(void);
	extern void create_speed_rpm_ratio_config_menu(void);

	if (selected == 0) {
		// ========== CUSTOM ==========
		printf("Gear ECU Presets: Custom (overriding device settings)\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 0;
		// Keep existing CAN ID settings for custom mode
		// Custom mode overrides device settings - save this choice

		// Show custom gear values section - use ui_MenuScreen if available, otherwise current screen
		lv_obj_t *parent_screen = (ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen)) ? ui_MenuScreen : lv_scr_act();
		create_custom_gear_values_section(parent_screen, 0);
		config_store_save_values(values_config, MAX_VALUES);
		ESP_LOGI("MENU",
				 "Gear ECU set to Custom mode - device settings overridden");
	} else if (selected == 1) {
		// ========== MAXXECU ==========
		printf("Gear ECU Presets: MaxxECU\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 1;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		values_config[GEAR_VALUE_ID - 1].can_id = 536;	// 0x218
		values_config[GEAR_VALUE_ID - 1].endianess = 1; // Little Endian
		values_config[GEAR_VALUE_ID - 1].bit_start = 0;
		values_config[GEAR_VALUE_ID - 1].bit_length = 16;
		values_config[GEAR_VALUE_ID - 1].scale = 1.0f;
		values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
		values_config[GEAR_VALUE_ID - 1].decimals = 0;
		values_config[GEAR_VALUE_ID - 1].is_signed = true; // MaxxECU uses signed values (-1 for reverse)

		// Update UI fields for GEAR (ID=11 => index=10)
		if (g_can_id_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[GEAR_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_endian_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_start_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_offset_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
									 values_config[GEAR_VALUE_ID - 1].decimals);
		}
	} else if (selected == 2) {
		// ========== HALTECH ==========
		printf("Gear ECU Presets: Haltech\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 2;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		// Note: Haltech doesn't have a standard gear CAN ID in the preconfig
		// Users will need to configure manually or we can add a common one
		values_config[GEAR_VALUE_ID - 1].can_id =
			0x370; // Common Haltech gear CAN ID
		values_config[GEAR_VALUE_ID - 1].endianess = 0; // Big Endian
		values_config[GEAR_VALUE_ID - 1].bit_start = 16;
		values_config[GEAR_VALUE_ID - 1].bit_length = 16;
		values_config[GEAR_VALUE_ID - 1].scale = 1.0f;
		values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
		values_config[GEAR_VALUE_ID - 1].decimals = 0;

		// Update UI fields similarly
		if (g_can_id_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[GEAR_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_endian_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_start_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_offset_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
									 values_config[GEAR_VALUE_ID - 1].decimals);
		}
	} else if (selected == 3) {
		// ========== FORD ==========
		printf("Gear ECU Presets: Ford BA/BF/FG\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 3;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		values_config[GEAR_VALUE_ID - 1].can_id = 0x3E9; // Ford gear CAN ID
		values_config[GEAR_VALUE_ID - 1].endianess = 1;	 // Little Endian
		values_config[GEAR_VALUE_ID - 1].bit_start = 4;
		values_config[GEAR_VALUE_ID - 1].bit_length = 4;
		values_config[GEAR_VALUE_ID - 1].scale = 1.0f;
		values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
		values_config[GEAR_VALUE_ID - 1].decimals = 0;

		// Update UI fields for GEAR (ID=11 => index=10)
		if (g_can_id_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[GEAR_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_endian_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_start_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_offset_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
									 values_config[GEAR_VALUE_ID - 1].decimals);
		}
	} else if (selected == 4) {
		// ========== SPEED/RPM RATIO ==========
		printf("Gear ECU Presets: Speed/RPM Ratio (calculated gear estimation)\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 4;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		
		// This mode doesn't use CAN for gear - it calculates from speed and RPM
		// The CAN processing sections will skip gear when in this mode
		
		// Open the Speed/RPM Ratio configuration menu
		create_speed_rpm_ratio_config_menu();
		
		config_store_save_values(values_config, MAX_VALUES);
		ESP_LOGI("MENU", "Gear ECU set to Speed/RPM Ratio mode - opens configuration");
		return; // Exit early since we're opening a new menu
	}

	// Save configuration to NVS immediately (moved outside custom check)
	config_store_save_values(values_config, MAX_VALUES);
	ESP_LOGI("MENU", "Gear ECU preset changed to: %s (saved to NVS)",
			 selected == 0
				 ? "Custom"
				 : (selected == 1 ? "MaxxECU"
								  : (selected == 2 ? "Haltech" 
								  : (selected == 3 ? "Ford" : "Speed/RPM Ratio"))));

	// Show/hide custom gear config button based on selected mode
	if (custom_gear_config_button != NULL &&
		lv_obj_is_valid(custom_gear_config_button)) {
		if (selected == 0) {
			// Custom mode - show the button
			lv_obj_clear_flag(custom_gear_config_button, LV_OBJ_FLAG_HIDDEN);
		} else {
			// MaxxECU, Haltech, Ford, or Speed/RPM Ratio mode - hide the button
			lv_obj_add_flag(custom_gear_config_button, LV_OBJ_FLAG_HIDDEN);
		}
	} else {
		// If button is invalid, reset the pointer
		custom_gear_config_button = NULL;
		ESP_LOGW("GEAR",
				 "Custom gear button reference was invalid, reset to NULL");
	}
}
void custom_gear_can_id_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);

	// Find which gear this input corresponds to
	int gear_index = -1;
	for (int i = 0; i < 10; i++) {
		if (custom_gear_can_inputs[i] == textarea) {
			gear_index = i;
			break;
		}
	}

	if (gear_index == -1)
		return;

	// Get the CAN ID value from the input
	const char *can_id_str = lv_textarea_get_text(textarea);
	uint32_t can_id = 0;

	// Parse CAN ID (support both decimal and hex)
	if (strncmp(can_id_str, "0x", 2) == 0 ||
		strncmp(can_id_str, "0X", 2) == 0) {
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
	// This function is deprecated - custom gear values are now handled in
	// menu_screen.c

	// Save to NVS
	config_store_save_values(values_config, MAX_VALUES);

	// Log the change
	const char *gear_names[] = {"P", "R", "N", "D", "1", "2", "3",
								"4", "5", "6", "7", "8", "9", "10"};
	ESP_LOGI("GEAR", "Custom gear %s CAN ID set to: 0x%03X",
			 gear_names[gear_index], can_id);
}

void create_custom_gear_config_menu(void) {
	// Create a new screen for custom gear configuration
	lv_obj_t *custom_gear_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(custom_gear_screen, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title
	lv_obj_t *title_label = lv_label_create(custom_gear_screen);
	lv_label_set_text(title_label, "Custom Gear CAN ID Configuration");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(title_label, THEME_FONT_DASH_LABEL, 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

	// Instructions
	lv_obj_t *instructions = lv_label_create(custom_gear_screen);
	lv_label_set_text(instructions,
					  "Set individual CAN IDs for each gear position.\nLeave "
					  "blank (0x000) to disable a gear.");
	lv_obj_set_style_text_color(instructions, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(instructions, THEME_FONT_TINY, 0);
	lv_obj_align(instructions, LV_ALIGN_TOP_MID, 0, 50);
	lv_obj_set_style_text_align(instructions, LV_TEXT_ALIGN_CENTER, 0);

	// Create gear configuration grid
	const char *gear_labels[] = {
		"Neutral (N)", "Reverse (R)", "Gear 1", "Gear 2", "Gear 3",
		"Gear 4",	   "Gear 5",	  "Gear 6", "Gear 7", "Gear 8"};

	for (int i = 0; i < 10; i++) {
		int row = i / 2;
		int col = i % 2;

		// Gear label
		lv_obj_t *gear_label = lv_label_create(custom_gear_screen);
		lv_label_set_text(gear_label, gear_labels[i]);
		lv_obj_set_style_text_color(gear_label, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_align(gear_label, LV_ALIGN_TOP_LEFT, 50 + col * 350,
					 100 + row * 60);

		// CAN ID input
		custom_gear_can_inputs[i] = lv_textarea_create(custom_gear_screen);
		lv_textarea_set_one_line(custom_gear_can_inputs[i], true);
		lv_textarea_set_max_length(custom_gear_can_inputs[i], 8);
		lv_obj_set_width(custom_gear_can_inputs[i], 100);
		lv_obj_set_height(custom_gear_can_inputs[i], 35);
		lv_obj_align(custom_gear_can_inputs[i], LV_ALIGN_TOP_LEFT,
					 200 + col * 350, 95 + row * 60);
		lv_obj_add_style(custom_gear_can_inputs[i], get_common_style(),
						 LV_PART_MAIN);

		// Set current value
		char current_value[16];
		uint32_t can_id = 0; // Deprecated function - no longer used
		if (can_id == 0) {
			strcpy(current_value, "0x000");
		} else {
			snprintf(current_value, sizeof(current_value), "0x%03X", can_id);
		}
		lv_textarea_set_text(custom_gear_can_inputs[i], current_value);

		// Add event callback
		lv_obj_add_event_cb(custom_gear_can_inputs[i],
							custom_gear_can_id_event_cb, LV_EVENT_VALUE_CHANGED,
							NULL);
		lv_obj_add_event_cb(custom_gear_can_inputs[i],
							custom_gear_can_id_event_cb, LV_EVENT_DEFOCUSED,
							NULL);
	}

	// Back button
	lv_obj_t *back_btn = lv_btn_create(custom_gear_screen);
	lv_obj_set_size(back_btn, 100, 40);
	lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_t *back_label = lv_label_create(back_btn);
	lv_label_set_text(back_label, "Back");
	lv_obj_center(back_label);
	lv_obj_add_event_cb(back_btn, custom_gear_back_btn_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Save button
	lv_obj_t *save_btn = lv_btn_create(custom_gear_screen);
	lv_obj_set_size(save_btn, 100, 40);
	lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_center(save_label);
	lv_obj_add_event_cb(save_btn, custom_gear_save_btn_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Load the custom gear screen
	lv_scr_load(custom_gear_screen);
}

// Custom gear back button event callback
void custom_gear_back_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Returning to gear configuration menu");
	// Go back to the gear configuration menu
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

// Custom gear save button event callback
void custom_gear_save_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Saving custom gear configuration");
	config_store_save_values(values_config, MAX_VALUES);
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

void speed_rpm_tire_circumference_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	const char *text = lv_textarea_get_text(textarea);
	values_config[GEAR_VALUE_ID - 1].tire_circumference_mm = atof(text);
	ESP_LOGI("GEAR", "Tire circumference set to: %.2f mm", values_config[GEAR_VALUE_ID - 1].tire_circumference_mm);
}

void speed_rpm_final_drive_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	const char *text = lv_textarea_get_text(textarea);
	values_config[GEAR_VALUE_ID - 1].final_drive_ratio = atof(text);
	ESP_LOGI("GEAR", "Final drive ratio set to: %.3f", values_config[GEAR_VALUE_ID - 1].final_drive_ratio);
}

void speed_rpm_reverse_ratio_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	const char *text = lv_textarea_get_text(textarea);
	values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio = atof(text);
	ESP_LOGI("GEAR", "Reverse gear ratio set to: %.3f", values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio);
}

void speed_rpm_gear_ratio_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	
	// Check if it's the reverse gear input
	if (speed_rpm_reverse_ratio_input == textarea) {
		speed_rpm_reverse_ratio_event_cb(e);
		return;
	}
	
	// Find which gear this input corresponds to
	int gear_index = -1;
	for (int i = 0; i < 10; i++) {
		if (speed_rpm_gear_ratio_inputs[i] == textarea) {
			gear_index = i;
			break;
		}
	}
	
	if (gear_index >= 0) {
		const char *text = lv_textarea_get_text(textarea);
		values_config[GEAR_VALUE_ID - 1].gear_ratios[gear_index] = atof(text);
		ESP_LOGI("GEAR", "Gear %d ratio set to: %.3f", gear_index + 1, values_config[GEAR_VALUE_ID - 1].gear_ratios[gear_index]);
	}
}

void speed_rpm_ratio_back_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Returning to gear configuration menu");
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

void speed_rpm_ratio_save_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Saving Speed/RPM Ratio configuration");
	config_store_save_values(values_config, MAX_VALUES);
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

void create_speed_rpm_ratio_config_menu(void) {
	// Create a new screen for Speed/RPM Ratio configuration
	lv_obj_t *speed_rpm_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(speed_rpm_screen, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title
	lv_obj_t *title_label = lv_label_create(speed_rpm_screen);
	lv_label_set_text(title_label, "Speed/RPM Ratio Gear Configuration");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(title_label, THEME_FONT_DASH_LABEL, 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);

	// Instructions
	lv_obj_t *instructions = lv_label_create(speed_rpm_screen);
	lv_label_set_text(instructions,
					  "Configure tire size, final drive, and gear ratios.\nGear is estimated from speed and RPM.");
	lv_obj_set_style_text_color(instructions, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(instructions, THEME_FONT_TINY, 0);
	lv_obj_align(instructions, LV_ALIGN_TOP_MID, 0, 35);
	lv_obj_set_style_text_align(instructions, LV_TEXT_ALIGN_CENTER, 0);

	// Tire Circumference
	lv_obj_t *tire_label = lv_label_create(speed_rpm_screen);
	lv_label_set_text(tire_label, "Tire Circumference (mm):");
	lv_obj_set_style_text_color(tire_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_align(tire_label, LV_ALIGN_TOP_LEFT, 50, 70);
	
	speed_rpm_tire_circumference_input = lv_textarea_create(speed_rpm_screen);
	lv_textarea_set_one_line(speed_rpm_tire_circumference_input, true);
	lv_obj_set_width(speed_rpm_tire_circumference_input, 100);
	lv_obj_align(speed_rpm_tire_circumference_input, LV_ALIGN_TOP_LEFT, 250, 65);
	lv_obj_add_style(speed_rpm_tire_circumference_input, get_common_style(), LV_PART_MAIN);
	lv_obj_clear_flag(speed_rpm_tire_circumference_input, LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling
	char tire_buf[16];
	snprintf(tire_buf, sizeof(tire_buf), "%.1f", values_config[GEAR_VALUE_ID - 1].tire_circumference_mm);
	lv_textarea_set_text(speed_rpm_tire_circumference_input, tire_buf);
	lv_obj_add_event_cb(speed_rpm_tire_circumference_input, speed_rpm_tire_circumference_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(speed_rpm_tire_circumference_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

	// Final Drive Ratio
	lv_obj_t *final_drive_label = lv_label_create(speed_rpm_screen);
	lv_label_set_text(final_drive_label, "Final Drive Ratio:");
	lv_obj_set_style_text_color(final_drive_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_align(final_drive_label, LV_ALIGN_TOP_LEFT, 400, 70);
	
	speed_rpm_final_drive_input = lv_textarea_create(speed_rpm_screen);
	lv_textarea_set_one_line(speed_rpm_final_drive_input, true);
	lv_obj_set_width(speed_rpm_final_drive_input, 100);
	lv_obj_align(speed_rpm_final_drive_input, LV_ALIGN_TOP_LEFT, 580, 65);
	lv_obj_add_style(speed_rpm_final_drive_input, get_common_style(), LV_PART_MAIN);
	lv_obj_clear_flag(speed_rpm_final_drive_input, LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling
	char final_drive_buf[16];
	snprintf(final_drive_buf, sizeof(final_drive_buf), "%.3f", values_config[GEAR_VALUE_ID - 1].final_drive_ratio);
	lv_textarea_set_text(speed_rpm_final_drive_input, final_drive_buf);
	lv_obj_add_event_cb(speed_rpm_final_drive_input, speed_rpm_final_drive_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(speed_rpm_final_drive_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

	// Gear Ratios Grid (Reverse + 1-10, total 11 inputs)
	const char *gear_labels[] = {"Reverse", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th", "9th", "10th"};
	
	for (int i = 0; i < 11; i++) {
		int row = i / 5;
		int col = i % 5;

		// Gear label
		lv_obj_t *gear_label = lv_label_create(speed_rpm_screen);
		lv_label_set_text(gear_label, gear_labels[i]);
		lv_obj_set_style_text_color(gear_label, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_align(gear_label, LV_ALIGN_TOP_LEFT, 50 + col * 150, 120 + row * 50);

		// Gear ratio input
		if (i == 0) {
			// Reverse gear input
			speed_rpm_reverse_ratio_input = lv_textarea_create(speed_rpm_screen);
			lv_textarea_set_one_line(speed_rpm_reverse_ratio_input, true);
			lv_obj_set_width(speed_rpm_reverse_ratio_input, 80);
			lv_obj_set_height(speed_rpm_reverse_ratio_input, 30);
			lv_obj_align(speed_rpm_reverse_ratio_input, LV_ALIGN_TOP_LEFT, 90 + col * 150, 115 + row * 50);
			lv_obj_add_style(speed_rpm_reverse_ratio_input, get_common_style(), LV_PART_MAIN);
			lv_obj_clear_flag(speed_rpm_reverse_ratio_input, LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling
			
			// Set current value
			char reverse_buf[16];
			snprintf(reverse_buf, sizeof(reverse_buf), "%.3f", values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio);
			lv_textarea_set_text(speed_rpm_reverse_ratio_input, reverse_buf);
			
			// Add event callbacks
			lv_obj_add_event_cb(speed_rpm_reverse_ratio_input, speed_rpm_reverse_ratio_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
			lv_obj_add_event_cb(speed_rpm_reverse_ratio_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
		} else {
			// Forward gear inputs (1-10)
			int gear_index = i - 1; // Map to gear_ratios array (0-9 for gears 1-10)
			speed_rpm_gear_ratio_inputs[gear_index] = lv_textarea_create(speed_rpm_screen);
			lv_textarea_set_one_line(speed_rpm_gear_ratio_inputs[gear_index], true);
			lv_obj_set_width(speed_rpm_gear_ratio_inputs[gear_index], 80);
			lv_obj_set_height(speed_rpm_gear_ratio_inputs[gear_index], 30);
			lv_obj_align(speed_rpm_gear_ratio_inputs[gear_index], LV_ALIGN_TOP_LEFT, 90 + col * 150, 115 + row * 50);
			lv_obj_add_style(speed_rpm_gear_ratio_inputs[gear_index], get_common_style(), LV_PART_MAIN);
			lv_obj_clear_flag(speed_rpm_gear_ratio_inputs[gear_index], LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling

			// Set current value
			char ratio_buf[16];
			snprintf(ratio_buf, sizeof(ratio_buf), "%.3f", values_config[GEAR_VALUE_ID - 1].gear_ratios[gear_index]);
			lv_textarea_set_text(speed_rpm_gear_ratio_inputs[gear_index], ratio_buf);

			// Add event callbacks
			lv_obj_add_event_cb(speed_rpm_gear_ratio_inputs[gear_index], speed_rpm_gear_ratio_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
			lv_obj_add_event_cb(speed_rpm_gear_ratio_inputs[gear_index], keyboard_event_cb, LV_EVENT_ALL, NULL);
		}
	}

	// Back button
	lv_obj_t *back_btn = lv_btn_create(speed_rpm_screen);
	lv_obj_set_size(back_btn, 100, 40);
	lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_t *back_label = lv_label_create(back_btn);
	lv_label_set_text(back_label, "Back");
	lv_obj_center(back_label);
	lv_obj_add_event_cb(back_btn, speed_rpm_ratio_back_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Save button
	lv_obj_t *save_btn = lv_btn_create(speed_rpm_screen);
	lv_obj_set_size(save_btn, 100, 40);
	lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_center(save_label);
	lv_obj_add_event_cb(save_btn, speed_rpm_ratio_save_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Load the Speed/RPM Ratio screen
	lv_scr_load(speed_rpm_screen);
}

void widget_gear_create(lv_obj_t *parent)
{
    extern const lv_img_dsc_t Smart_Car_Key;

    ui_Gear_Panel = lv_obj_create(parent);
    lv_obj_set_size(ui_Gear_Panel, 90, 90);
    lv_obj_set_x(ui_Gear_Panel, 0); lv_obj_set_y(ui_Gear_Panel, 180);
    lv_obj_set_align(ui_Gear_Panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Gear_Panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Gear_Panel, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Gear_Panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Gear_Panel, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Gear_Panel, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Gear_Panel, 7, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Gear_Label = lv_label_create(parent);
    lv_obj_set_x(ui_Gear_Label, 0); lv_obj_set_y(ui_Gear_Label, 152);
    lv_obj_set_align(ui_Gear_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Gear_Label, "GEAR");
    lv_obj_set_style_text_color(ui_Gear_Label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Gear_Label, THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_GEAR_Value = lv_label_create(parent);
    lv_obj_set_width(ui_GEAR_Value, 115);
    lv_obj_set_x(ui_GEAR_Value, 0); lv_obj_set_y(ui_GEAR_Value, 198);
    lv_obj_set_align(ui_GEAR_Value, LV_ALIGN_CENTER);
    lv_label_set_text(ui_GEAR_Value, "-");
    strcpy(previous_values[GEAR_VALUE_ID - 1], "-");
    lv_obj_set_style_text_color(ui_GEAR_Value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_GEAR_Value, THEME_FONT_DASH_GEAR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_GEAR_Value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_zoom(ui_GEAR_Value, 210, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_GEAR_Icon = lv_img_create(parent);
    lv_img_set_src(ui_GEAR_Icon, &Smart_Car_Key);
    lv_img_set_zoom(ui_GEAR_Icon, 225);
    lv_img_set_pivot(ui_GEAR_Icon, 15, 29);
    lv_obj_set_x(ui_GEAR_Icon, 0); lv_obj_set_y(ui_GEAR_Icon, 194);
    lv_obj_set_align(ui_GEAR_Icon, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
}

uint64_t *widget_gear_get_last_can_time(void) { return &last_gear_can_received; }
