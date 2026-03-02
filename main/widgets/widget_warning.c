#include "widget_warning.h"
#include "can/can_decode.h"
#include "can/can_dispatch.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "storage/config_store.h"
#include "ui/callbacks/ui_callbacks.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widget_dispatcher.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* forward declarations */
static void free_warning_idx_event_cb(lv_event_t *e);
static void invert_warning_toggle_event_cb(lv_event_t *e);

static const struct {
	int16_t x;
	int16_t y;
} warning_positions[] = {
	{-352, -148}, // Warning 1
	{-292, -148}, // Warning 2
	{-232, -148}, // Warning 3
	{-172, -148}, // Warning 4
	{172, -148},  // Warning 5
	{232, -148},  // Warning 6
	{292, -148},  // Warning 7
	{352, -148}	  // Warning 8
};

static lv_obj_t *warning_circles[8] = {NULL};
static lv_obj_t *warning_labels[8] = {NULL};
uint64_t last_signal_times[8] = {0};
bool toggle_debounce[8] = {false};
uint64_t toggle_start_time[8] = {0};
bool previous_bit_states[8] = {false};

// Forward declarations for optimization functions
static void batch_update_rpm_circles_color(lv_color_t color);

typedef struct {
	uint8_t warning_idx;
	lv_obj_t **input_objects;
	lv_obj_t **preview_objects;
	lv_obj_t *preconfig_warning_dd; // Reference to warning dropdown in
									// preconfig container
} warning_save_data_t;
void warning_high_threshold_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		const char *txt = lv_textarea_get_text(textarea);
		values_config[value_id - 1].warning_high_threshold = atof(txt);
		values_config[value_id - 1].warning_high_enabled = true;
	}
}

void warning_low_threshold_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		const char *txt = lv_textarea_get_text(textarea);
		values_config[value_id - 1].warning_low_threshold = atof(txt);
		values_config[value_id - 1].warning_low_enabled = true;
	}
}

void warning_high_color_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *dropdown = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		uint16_t selected = lv_dropdown_get_selected(dropdown);
		values_config[value_id - 1].warning_high_color =
			selected == 0 ? THEME_COLOR_RED : THEME_COLOR_BLUE_DARK;
	}
}

void warning_low_color_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *dropdown = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		uint16_t selected = lv_dropdown_get_selected(dropdown);
		values_config[value_id - 1].warning_low_color =
			selected == 0 ? THEME_COLOR_RED : THEME_COLOR_BLUE_DARK;
	}
}

static void warning_longpress_cb(lv_event_t *e) {
	uint8_t warning_idx = *(uint8_t *)lv_event_get_user_data(e);
	create_warning_config_menu(warning_idx);
}

static void label_text_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	warning_save_data_t *data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	const char *txt = lv_textarea_get_text(textarea);
	if (data->preview_objects && data->preview_objects[1]) {
		lv_label_set_text(data->preview_objects[1], txt);
	}
}

static void color_dropdown_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	warning_save_data_t *data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	lv_color_t color;
	switch (selected) {
	case 0:
		color = THEME_COLOR_GREEN;
		break; // Green
	case 1:
		color = THEME_COLOR_BLUE_PURE;
		break; // Blue
	case 2:
		color = THEME_COLOR_ORANGE_WEB;
		break; // Orange
	case 3:
		color = THEME_COLOR_RED;
		break; // Red
	case 4:
		color = THEME_COLOR_YELLOW;
		break; // Yellow
	default:
		color = THEME_COLOR_GREEN;
		break;
	}
	if (data->preview_objects && data->preview_objects[0]) {
		lv_obj_set_style_bg_color(data->preview_objects[0], color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

// Structure for preconfigured warnings
typedef struct {
	const char *label;
	const char *can_id_hex;
	uint8_t bit_position;
	uint8_t endianess;	  // 0 = Big, 1 = Little
	uint8_t color_index;  // 0=Green, 1=Blue, 2=Orange, 3=Red, 4=Yellow
	uint8_t is_momentary; // 0 = On/Off, 1 = Momentary
} warning_preconfig_t;

// Ford BA/BF/FG preconfigured warnings
static const warning_preconfig_t ford_babf_warnings[] = {
	{"Foglights On", "128", 2, 1, 2, 1},	 // Orange, Momentary
	{"High Beams On", "128", 3, 1, 0, 1},	 // Green, Momentary
	{"Driver Door", "403", 0, 1, 2, 1},		 // Orange, Momentary
	{"Passenger Door", "403", 1, 1, 2, 1},	 // Orange, Momentary
	{"Rear Doors", "403", 3, 1, 2, 1},		 // Orange, Momentary
	{"Cruise On", "425", 0, 1, 0, 1},		 // Green, Momentary
	{"Cruise Set", "425", 1, 1, 1, 1},		 // Blue, Momentary
	{"Low Oil Press", "427", 41, 1, 3, 1},	 // Red, Momentary
	{"Alternator Fail", "427", 42, 1, 3, 1}, // Red, Momentary
	{"Engine Light", "427", 43, 1, 2, 1},	 // Orange, Momentary
	{"Handbrake On", "437", 18, 1, 3, 1}	 // Red, Momentary
};

// Callback for applying preconfigured warning
static void apply_preconfig_warning_cb(lv_event_t *e) {
	warning_save_data_t *save_data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	if (!save_data || !save_data->input_objects) {
		printf("Error: Invalid save data for preconfig\n");
		return;
	}

	lv_obj_t **inputs = save_data->input_objects;

	// Get the warning dropdown from save_data
	lv_obj_t *warning_dd = save_data->preconfig_warning_dd;
	if (!warning_dd) {
		printf("Error: Warning dropdown not found in save_data\n");
		return;
	}

	uint16_t selected_warning = lv_dropdown_get_selected(warning_dd);
	if (selected_warning >=
		sizeof(ford_babf_warnings) / sizeof(ford_babf_warnings[0])) {
		printf("Error: Invalid warning selection\n");
		return;
	}

	const warning_preconfig_t *preconfig =
		&ford_babf_warnings[selected_warning];

	// Apply the preconfig
	lv_textarea_set_text(inputs[0], preconfig->can_id_hex);
	lv_dropdown_set_selected(inputs[1], preconfig->bit_position);
	// Endianess removed - always defaults to Little Endian (1)
	lv_textarea_set_text(inputs[3], preconfig->label);
	lv_dropdown_set_selected(inputs[4], preconfig->color_index);
	lv_dropdown_set_selected(inputs[5], preconfig->is_momentary);

	// Get color based on color_index
	lv_color_t color;
	switch (preconfig->color_index) {
	case 0:
		color = THEME_COLOR_GREEN;
		break; // Green
	case 1:
		color = THEME_COLOR_BLUE_PURE;
		break; // Blue
	case 2:
		color = THEME_COLOR_ORANGE_WEB;
		break; // Orange
	case 3:
		color = THEME_COLOR_RED;
		break; // Red
	case 4:
		color = THEME_COLOR_YELLOW;
		break; // Yellow
	default:
		color = THEME_COLOR_GREEN;
		break;
	}

	// Update preview color
	if (save_data->preview_objects && save_data->preview_objects[0]) {
		lv_obj_set_style_bg_color(save_data->preview_objects[0], color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Update preview label
	if (save_data->preview_objects && save_data->preview_objects[1]) {
		lv_label_set_text(save_data->preview_objects[1], preconfig->label);
	}

	printf("Applied preconfig: Ford BA/BF/FG - %s\n", preconfig->label);
}

static void save_warning_config_cb(lv_event_t *e) {
	warning_save_data_t *save_data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	if (!save_data) {
		printf("Error: Invalid save data\n");
		return;
	}

	uint8_t warning_idx = save_data->warning_idx;
	lv_obj_t **inputs = save_data->input_objects;

	if (!inputs) {
		printf("Error: Invalid input objects\n");
		lv_mem_free(save_data);
		return;
	}

	// Get values from inputs
	const char *can_id_text = lv_textarea_get_text(inputs[0]);
	uint8_t bit_pos = lv_dropdown_get_selected(inputs[1]);
	uint8_t endianess = 1; // Default to Little Endian (not needed for warnings,
						   // but kept for compatibility)
	const char *label_text = lv_textarea_get_text(inputs[3]);

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
	warning_configs[warning_idx].endianess = endianess;
	if (label_text) {
		strncpy(warning_configs[warning_idx].label, label_text,
				sizeof(warning_configs[warning_idx].label) - 1);
		warning_configs[warning_idx]
			.label[sizeof(warning_configs[warning_idx].label) - 1] = '\0';
	}

	// Handle highlighted color selection
	if (inputs[4]) {
		uint8_t selected_color = lv_dropdown_get_selected(inputs[4]);
		switch (selected_color) {
		case 0:
			warning_configs[warning_idx].active_color = THEME_COLOR_GREEN;
			break; // Green
		case 1:
			warning_configs[warning_idx].active_color = THEME_COLOR_BLUE_PURE;
			break; // Blue
		case 2:
			warning_configs[warning_idx].active_color = THEME_COLOR_ORANGE_WEB;
			break; // Orange
		case 3:
			warning_configs[warning_idx].active_color = THEME_COLOR_RED;
			break; // Red
		case 4:
			warning_configs[warning_idx].active_color = THEME_COLOR_YELLOW;
			break; // Yellow
		default:
			warning_configs[warning_idx].active_color = THEME_COLOR_GREEN;
			break;
		}
	}

	// Save toggle mode setting
	if (inputs[5]) {
		bool was_momentary = warning_configs[warning_idx].is_momentary;
		warning_configs[warning_idx].is_momentary =
			(lv_dropdown_get_selected(inputs[5]) == 1);

		// If mode changed, reset state and previous_bit_state
		if (was_momentary != warning_configs[warning_idx].is_momentary) {
			warning_configs[warning_idx].current_state = false;
			previous_bit_states[warning_idx] =
				false; // Reset previous bit state for toggle mode
			// Update UI to reflect the reset state
			update_warning_ui_immediate(warning_idx);
		}
	}

	// Add callbacks for live preview updates
	lv_obj_add_event_cb(inputs[3], label_text_cb, LV_EVENT_VALUE_CHANGED,
						save_data);
	lv_obj_add_event_cb(inputs[4], color_dropdown_cb, LV_EVENT_VALUE_CHANGED,
						save_data);

	// Debug output
	printf("Warning %d configuration saved:\n", warning_idx + 1);
	printf("  CAN ID: 0x%X\n", can_id);
	printf("  Bit Position: %d\n", bit_pos);
	printf("  Label: %s\n", label_text ? label_text : "");
	printf("  Highlight Color: %06X\n",
		   warning_configs[warning_idx].active_color.full);
	printf("  Mode: %s\n",
		   warning_configs[warning_idx].is_momentary ? "Momentary" : "Toggle");

	// Update the label on Screen3 dynamically
	if (warning_labels[warning_idx]) {
		lv_label_set_text(warning_labels[warning_idx],
						  warning_configs[warning_idx].label);
	}

	// Clean up
	lv_mem_free(inputs);
	lv_mem_free(save_data);
	config_store_save_warnings(warning_configs, 8);

	// Return to Screen3
	lv_scr_load(ui_Screen3);
}

// Structure to hold all input objects for the save callback
typedef struct {
	uint8_t indicator_idx;
	lv_obj_t *can_id_input;
	lv_obj_t *bit_pos_dropdown;
	lv_obj_t *toggle_mode_dropdown;
} indicator_save_data_t;

// Global variables for preview elements (to allow live updates)
static lv_obj_t *preview_indicator_config = NULL;
static lv_obj_t *preview_status_text_config = NULL;
static uint8_t preview_indicator_idx = 0;

/* Pointers for INPUT dropdown visibility: when Wire selected, hide CAN ID / bit
 * / toggle / animation rows */
typedef struct {
	uint8_t indicator_idx;
	lv_obj_t *input_src_dropdown;
	lv_obj_t *can_id_label;
	lv_obj_t *can_id_0x;
	lv_obj_t *can_id_input;
	lv_obj_t *bit_pos_label;
	lv_obj_t *bit_pos_dropdown;
	lv_obj_t *toggle_mode_label;
	lv_obj_t *toggle_mode_dropdown;
	lv_obj_t *animation_label;
	lv_obj_t *animation_switch;
} indicator_input_visibility_t;

void update_warning_ui(void *param) {
	uint8_t warning_idx = *(uint8_t *)param;
	free(param);

	if (warning_circles[warning_idx] == NULL ||
		lv_obj_get_screen(warning_circles[warning_idx]) == NULL) {
		return;
	}

	lv_color_t new_color = warning_configs[warning_idx].current_state
							   ? warning_configs[warning_idx].active_color
							   : THEME_COLOR_INACTIVE; // Default "off" color.

	lv_obj_set_style_bg_color(warning_circles[warning_idx], new_color,
							  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Also update the warning label visibility
	if (warning_labels[warning_idx] &&
		lv_obj_is_valid(warning_labels[warning_idx])) {
		if (warning_configs[warning_idx].current_state) {
			lv_obj_clear_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

// Immediate warning update
void update_warning_ui_immediate(uint8_t warning_idx) {
	if (warning_idx >= 8)
		return;
	if (warning_circles[warning_idx] == NULL ||
		lv_obj_get_screen(warning_circles[warning_idx]) == NULL) {
		return;
	}
	lv_color_t new_color = warning_configs[warning_idx].current_state
							   ? warning_configs[warning_idx].active_color
							   : THEME_COLOR_INACTIVE;
	lv_obj_set_style_bg_color(warning_circles[warning_idx], new_color,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	if (warning_labels[warning_idx] &&
		lv_obj_is_valid(warning_labels[warning_idx])) {
		if (warning_configs[warning_idx].current_state) {
			lv_obj_clear_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		}
	}
}
void create_warning_config_menu(uint8_t warning_idx) {
	init_common_style();

	// Allocate memory for input objects array - increase size to 8 to include
	// range inputs
	lv_obj_t **input_objects = lv_mem_alloc(8 * sizeof(lv_obj_t *));
	if (!input_objects) {
		printf("Failed to allocate memory for input objects\n");
		return;
	}
	// Initialize all pointers to NULL
	for (int i = 0; i < 8; i++) {
		input_objects[i] = NULL;
	}

	// Allocate memory for preview objects
	lv_obj_t **preview_objects = lv_mem_alloc(2 * sizeof(lv_obj_t *));
	if (!preview_objects) {
		lv_mem_free(input_objects);
		printf("Failed to allocate memory for preview objects\n");
		return;
	}
	preview_objects[0] = NULL;
	preview_objects[1] = NULL;

	// Create the configuration screen
	lv_obj_t *config_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(config_screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(config_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(config_screen, LV_OBJ_FLAG_SCROLLABLE);

	// Create save data structure
	warning_save_data_t *save_data = lv_mem_alloc(sizeof(warning_save_data_t));
	if (!save_data) {
		lv_mem_free(input_objects);
		lv_mem_free(preview_objects);
		printf("Failed to allocate memory for save data\n");
		return;
	}
	save_data->warning_idx = warning_idx;
	save_data->input_objects = input_objects;
	save_data->preview_objects = preview_objects;
	save_data->preconfig_warning_dd =
		NULL; // Will be set when dropdown is created

	// Create main border/background panel - increased height for range inputs
	lv_obj_t *main_border = lv_obj_create(config_screen);
	lv_obj_set_width(main_border, 780);
	lv_obj_set_height(main_border, 405); // Increased from 325 to 405
	lv_obj_set_align(main_border, LV_ALIGN_CENTER);
	lv_obj_set_y(main_border, 117); // Lowered by 50px (was 67)
	lv_obj_clear_flag(main_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(main_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(main_border, THEME_COLOR_INACTIVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(main_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(main_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *input_border = lv_obj_create(config_screen);
	lv_obj_set_width(input_border, 275);
	lv_obj_set_height(input_border, 390); // Increased from 310 to 390
	lv_obj_set_x(input_border, -244);
	lv_obj_set_y(input_border, 117); // Lowered by 50px (was 67)
	lv_obj_set_align(input_border, LV_ALIGN_CENTER);
	lv_obj_clear_flag(input_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(input_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(input_border, THEME_COLOR_INPUT_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(input_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(input_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create preview warning circle in exact Screen3 position
	lv_obj_t *preview_circle = lv_obj_create(config_screen);
	lv_obj_set_width(preview_circle, 15);
	lv_obj_set_height(preview_circle, 15);
	lv_obj_set_x(preview_circle, warning_positions[warning_idx].x);
	lv_obj_set_y(preview_circle, warning_positions[warning_idx].y);
	lv_obj_set_align(preview_circle, LV_ALIGN_CENTER);
	lv_obj_clear_flag(preview_circle, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(preview_circle, 100,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(preview_circle,
							  warning_configs[warning_idx].active_color,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(preview_circle, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(preview_circle, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	preview_objects[0] =
		preview_circle; // Store preview circle for color updates

	// Create preview warning label in exact Screen3 position
	lv_obj_t *preview_label = lv_label_create(config_screen);
	lv_obj_set_width(preview_label,
					 LV_SIZE_CONTENT); // Auto width based on content
	lv_obj_set_height(preview_label, LV_SIZE_CONTENT); // Auto height
	lv_obj_set_x(preview_label, warning_positions[warning_idx].x);
	lv_obj_set_y(preview_label, -112); // Same y-position as in Screen3
	lv_obj_set_align(preview_label, LV_ALIGN_CENTER);
	lv_label_set_text(preview_label, warning_configs[warning_idx].label);
	lv_obj_set_style_text_color(preview_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(preview_label, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(preview_label, LV_TEXT_ALIGN_CENTER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(preview_label, THEME_FONT_TINY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	preview_objects[1] = preview_label;

	// Create the keyboard
	keyboard = lv_keyboard_create(config_screen);
	lv_obj_set_parent(keyboard, lv_layer_top());
	lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY,
						NULL);

	// Create title
	lv_obj_t *title = lv_label_create(config_screen);
	lv_label_set_text_fmt(title, "Warning %d Configuration", warning_idx + 1);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

	// Create a container for inputs
	lv_obj_t *inputs_container = lv_obj_create(config_screen);
	lv_obj_set_size(inputs_container, 800,
					480); // Adjusted size to fit within the border
	lv_obj_align(inputs_container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_opa(inputs_container, 0, 0);
	lv_obj_set_style_border_opa(inputs_container, 0, 0);
	lv_obj_clear_flag(inputs_container, LV_OBJ_FLAG_SCROLLABLE);

	// Warning label input (moved to top)
	lv_obj_t *label_text_label = lv_label_create(inputs_container);
	lv_label_set_text(label_text_label, "Warning Label:");
	lv_obj_set_width(label_text_label, 110);
	lv_obj_set_style_text_align(label_text_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(label_text_label, LV_ALIGN_CENTER, -312,
				 -47); // Was 73, now -47
	lv_obj_set_style_text_color(label_text_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[3] = lv_textarea_create(inputs_container);
	lv_obj_add_style(input_objects[3], &common_style, LV_PART_MAIN);
	lv_textarea_set_one_line(input_objects[3], true);
	lv_obj_set_width(input_objects[3], 120);
	lv_obj_align(input_objects[3], LV_ALIGN_CENTER, -180,
				 -47); // Was 73, now -47
	lv_obj_add_event_cb(input_objects[3], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	lv_textarea_set_text(input_objects[3], warning_configs[warning_idx].label);

	// CAN ID input (moved down)
	lv_obj_t *can_id_label = lv_label_create(inputs_container);
	lv_label_set_text(can_id_label, "CAN ID:");
	lv_obj_set_width(can_id_label, 110);
	lv_obj_set_style_text_align(can_id_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(can_id_label, LV_ALIGN_CENTER, -312, -7); // Was -47, now -7
	lv_obj_set_style_text_color(can_id_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[0] = lv_textarea_create(inputs_container);
	lv_obj_add_style(input_objects[0], &common_style, LV_PART_MAIN);
	lv_textarea_set_one_line(input_objects[0], true);
	lv_obj_set_width(input_objects[0], 120);
	lv_obj_align(input_objects[0], LV_ALIGN_CENTER, -180,
				 -7); // Was -47, now -7
	lv_obj_add_event_cb(input_objects[0], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	char can_id_text[32];
	snprintf(can_id_text, sizeof(can_id_text), "%X",
			 warning_configs[warning_idx].can_id);
	lv_textarea_set_text(input_objects[0], can_id_text);

	// Bit position dropdown
	lv_obj_t *bit_pos_label = lv_label_create(inputs_container);
	lv_label_set_text(bit_pos_label, "Bit Position:");
	lv_obj_set_width(bit_pos_label, 110);
	lv_obj_set_style_text_align(bit_pos_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(bit_pos_label, LV_ALIGN_CENTER, -312, 33); // Was -7, now 33
	lv_obj_set_style_text_color(bit_pos_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[1] = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_objects[1], &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(
		input_objects[1],
		"0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
		"16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
		"32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
		"48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63");
	lv_obj_set_width(input_objects[1], 120);
	lv_obj_align(input_objects[1], LV_ALIGN_CENTER, -180, 33);
	lv_dropdown_set_selected(input_objects[1],
							 warning_configs[warning_idx].bit_position);

	// Highlighted color dropdown
	lv_obj_t *color_label = lv_label_create(inputs_container);
	lv_label_set_text(color_label, "Active Colour:");
	lv_obj_set_width(color_label, 110);
	lv_obj_set_style_text_align(color_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(color_label, LV_ALIGN_CENTER, -312, 73);
	lv_obj_set_style_text_color(color_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[4] = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_objects[4], &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(input_objects[4],
							"Green\nBlue\nOrange\nRed\nYellow");
	lv_obj_set_width(input_objects[4], 120);
	lv_obj_align(input_objects[4], LV_ALIGN_CENTER, -180, 73);

	// Set the current color selection based on the saved configuration
	lv_color_t current_color = warning_configs[warning_idx].active_color;
	uint8_t selected_color = 0; // Default to Green
	if (current_color.full == THEME_COLOR_BLUE_PURE.full)
		selected_color = 1; // Blue
	else if (current_color.full == THEME_COLOR_ORANGE_WEB.full)
		selected_color = 2; // Orange
	else if (current_color.full == THEME_COLOR_RED.full)
		selected_color = 3; // Red
	else if (current_color.full == THEME_COLOR_YELLOW.full)
		selected_color = 4; // Yellow
	lv_dropdown_set_selected(input_objects[4], selected_color);

	// Add event callback to color dropdown to update preview instantly
	lv_obj_add_event_cb(input_objects[4], color_dropdown_cb,
						LV_EVENT_VALUE_CHANGED, save_data);

	// Add Toggle Mode dropdown
	lv_obj_t *toggle_mode_label = lv_label_create(inputs_container);
	lv_label_set_text(toggle_mode_label, "Toggle Mode:");
	lv_obj_set_width(toggle_mode_label, 110);
	lv_obj_set_style_text_align(toggle_mode_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_color(toggle_mode_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_align(toggle_mode_label, LV_ALIGN_CENTER, -312, 113);

	input_objects[5] = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_objects[5], &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(input_objects[5], "On/Off\nMomentary");
	lv_obj_set_width(input_objects[5], 120);
	lv_obj_align(input_objects[5], LV_ALIGN_CENTER, -180, 113);
	lv_dropdown_set_selected(input_objects[5],
							 warning_configs[warning_idx].is_momentary ? 1 : 0);

	// Invert Toggle (below Toggle Mode on the left)
	lv_obj_t *invert_toggle_label = lv_label_create(inputs_container);
	lv_label_set_text(invert_toggle_label, "Invert Toggle:");
	lv_obj_set_width(invert_toggle_label, 110);
	lv_obj_set_style_text_align(invert_toggle_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_color(invert_toggle_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_align(invert_toggle_label, LV_ALIGN_CENTER, -312, 153);

	lv_obj_t *invert_toggle_switch = lv_switch_create(inputs_container);
	lv_obj_align(invert_toggle_switch, LV_ALIGN_CENTER, -180, 153);
	lv_obj_set_size(invert_toggle_switch, 50, 25);

	// Set switch state based on configuration
	if (warning_configs[warning_idx].invert_toggle) {
		lv_obj_add_state(invert_toggle_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_clear_state(invert_toggle_switch, LV_STATE_CHECKED);
	}

	// Add event callback for invert toggle
	uint8_t *invert_toggle_id_ptr = lv_mem_alloc(sizeof(uint8_t));
	*invert_toggle_id_ptr = warning_idx;
	lv_obj_add_event_cb(invert_toggle_switch, invert_warning_toggle_event_cb,
						LV_EVENT_VALUE_CHANGED, invert_toggle_id_ptr);
	lv_obj_add_event_cb(invert_toggle_switch, free_warning_idx_event_cb,
						LV_EVENT_DELETE, invert_toggle_id_ptr);

	// Right column - Preconfigured Warnings (in grey container)
	// Create grey container for preconfig section
	lv_obj_t *preconfig_container = lv_obj_create(config_screen);
	lv_obj_set_width(preconfig_container, 240);
	lv_obj_set_height(preconfig_container, 220);
	lv_obj_align(preconfig_container, LV_ALIGN_CENTER, 250, 40);
	lv_obj_clear_flag(preconfig_container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(preconfig_container, 7,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(preconfig_container, THEME_COLOR_INPUT_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(preconfig_container, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(preconfig_container, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(preconfig_container, 15,
							 LV_PART_MAIN | LV_STATE_DEFAULT);

	// Heading for preconfig container
	lv_obj_t *preconfig_heading = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_heading, "Pre-configurations");
	lv_obj_set_style_text_color(preconfig_heading, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(preconfig_heading, THEME_FONT_BODY, 0);
	lv_obj_align(preconfig_heading, LV_ALIGN_TOP_MID, 0, 0);

	// ECU dropdown
	lv_obj_t *preconfig_ecu_label = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_ecu_label, "ECU:");
	lv_obj_set_width(preconfig_ecu_label, 70);
	lv_obj_set_style_text_align(preconfig_ecu_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(preconfig_ecu_label, LV_ALIGN_TOP_LEFT, 0, 30);
	lv_obj_set_style_text_color(preconfig_ecu_label, THEME_COLOR_TEXT_MUTED, 0);

	lv_obj_t *preconfig_ecu_dd = lv_dropdown_create(preconfig_container);
	lv_obj_add_style(preconfig_ecu_dd, &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(preconfig_ecu_dd, "Ford");
	lv_obj_set_width(preconfig_ecu_dd, 140);
	lv_obj_align(preconfig_ecu_dd, LV_ALIGN_TOP_LEFT, 80, 25);

	// Version dropdown
	lv_obj_t *preconfig_version_label = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_version_label, "Version:");
	lv_obj_set_width(preconfig_version_label, 70);
	lv_obj_set_style_text_align(preconfig_version_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(preconfig_version_label, LV_ALIGN_TOP_LEFT, 0, 70);
	lv_obj_set_style_text_color(preconfig_version_label, THEME_COLOR_TEXT_MUTED,
								0);

	lv_obj_t *preconfig_version_dd = lv_dropdown_create(preconfig_container);
	lv_obj_add_style(preconfig_version_dd, &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(preconfig_version_dd, "BA/BF/FG");
	lv_obj_set_width(preconfig_version_dd, 140);
	lv_obj_align(preconfig_version_dd, LV_ALIGN_TOP_LEFT, 80, 65);

	// Warnings dropdown
	lv_obj_t *preconfig_warning_label = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_warning_label, "Warning:");
	lv_obj_set_width(preconfig_warning_label, 70);
	lv_obj_set_style_text_align(preconfig_warning_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(preconfig_warning_label, LV_ALIGN_TOP_LEFT, 0, 110);
	lv_obj_set_style_text_color(preconfig_warning_label, THEME_COLOR_TEXT_MUTED,
								0);

	lv_obj_t *preconfig_warning_dd = lv_dropdown_create(preconfig_container);
	lv_obj_add_style(preconfig_warning_dd, &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(preconfig_warning_dd, "Foglights On\n"
												  "High Beams On\n"
												  "Driver Door\n"
												  "Passenger Door\n"
												  "Rear Doors\n"
												  "Cruise On\n"
												  "Cruise Set\n"
												  "Low Oil Press\n"
												  "Alternator Fail\n"
												  "Engine Light\n"
												  "Handbrake On");
	lv_obj_set_width(preconfig_warning_dd, 140);
	lv_obj_align(preconfig_warning_dd, LV_ALIGN_TOP_LEFT, 80, 105);

	// Store reference to warning dropdown in save_data
	save_data->preconfig_warning_dd = preconfig_warning_dd;

	// Apply button for preconfig
	lv_obj_t *apply_preconfig_btn = lv_btn_create(preconfig_container);
	lv_obj_align(apply_preconfig_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_width(apply_preconfig_btn, 140);
	lv_obj_set_height(apply_preconfig_btn, 30);

	lv_obj_t *apply_preconfig_label = lv_label_create(apply_preconfig_btn);
	lv_label_set_text(apply_preconfig_label, "Apply");
	lv_obj_center(apply_preconfig_label);

	// Add event callback to apply button
	lv_obj_add_event_cb(apply_preconfig_btn, apply_preconfig_warning_cb,
						LV_EVENT_CLICKED, save_data);

	// Save button
	lv_obj_t *save_btn = lv_btn_create(config_screen);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);

	lv_obj_add_event_cb(save_btn, save_warning_config_cb, LV_EVENT_CLICKED,
						save_data);

	lv_scr_load(config_screen);
}

// Free warning index event callback
static void free_warning_idx_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_DELETE) {
		uint8_t *p_idx = (uint8_t *)lv_event_get_user_data(e);
		if (p_idx) {
			lv_mem_free(p_idx);
		}
	}
}

// Invert warning toggle event callback
static void invert_warning_toggle_event_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	uint8_t *warning_idx_ptr = (uint8_t *)lv_event_get_user_data(e);
	if (!warning_idx_ptr)
		return;

	uint8_t warning_idx = *warning_idx_ptr;
	bool new_invert_toggle = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
	bool old_invert_toggle = warning_configs[warning_idx].invert_toggle;

	// If the invert state changed, we need to flip the current warning state
	// to reflect the inversion immediately (works both ways: on->off and
	// off->on)
	if (new_invert_toggle != old_invert_toggle) {
		// Flip the current state to reflect the inversion change
		// This works both ways: enabling invert flips once, disabling flips
		// back
		warning_configs[warning_idx].current_state =
			!warning_configs[warning_idx].current_state;
		// Also flip the previous bit state so toggle mode works correctly
		previous_bit_states[warning_idx] = !previous_bit_states[warning_idx];
		// Update the UI immediately
		update_warning_ui_immediate(warning_idx);

		ESP_LOGI("WARNING",
				 "Invert toggle changed for warning %d: %s -> %s, state "
				 "flipped to %s",
				 warning_idx, old_invert_toggle ? "enabled" : "disabled",
				 new_invert_toggle ? "enabled" : "disabled",
				 warning_configs[warning_idx].current_state ? "ON" : "OFF");
	}

	warning_configs[warning_idx].invert_toggle = new_invert_toggle;

	// Save configuration to NVS
	config_store_save_warnings(warning_configs, 8);

	ESP_LOGI("WARNING", "Invert toggle %s for warning %d",
			 new_invert_toggle ? "enabled" : "disabled", warning_idx);
}
void check_warning_timeouts(lv_timer_t *timer) {
	// Timeout function is no longer needed for momentary warnings
	// as they now follow the live bit state directly.
	// This function is kept for potential future use but does nothing.
	(void)timer; // Suppress unused parameter warning
}

void widget_warning_create(lv_obj_t *parent) {
	for (int i = 0; i < 8; i++) {
		warning_circles[i] = lv_obj_create(parent);
		lv_obj_set_width(warning_circles[i], 15);
		lv_obj_set_height(warning_circles[i], 15);
		lv_obj_set_x(warning_circles[i], warning_positions[i].x);
		lv_obj_set_y(warning_circles[i], warning_positions[i].y);
		lv_obj_set_align(warning_circles[i], LV_ALIGN_CENTER);
		lv_obj_clear_flag(warning_circles[i], LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_radius(warning_circles[i], 100,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(warning_circles[i], THEME_COLOR_INACTIVE,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(warning_circles[i], 255,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(warning_circles[i], 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);

		warning_labels[i] = lv_label_create(parent);
		lv_obj_set_width(warning_labels[i], LV_SIZE_CONTENT);
		lv_obj_set_height(warning_labels[i], LV_SIZE_CONTENT);
		lv_obj_set_x(warning_labels[i], warning_positions[i].x);
		lv_obj_set_y(warning_labels[i], -112);
		lv_obj_set_align(warning_labels[i], LV_ALIGN_CENTER);
		lv_obj_add_flag(warning_labels[i], LV_OBJ_FLAG_HIDDEN);
		const char *saved_label = warning_configs[i].label;
		if (saved_label && saved_label[0] != '\0') {
			lv_label_set_text(warning_labels[i], saved_label);
		} else {
			char label_text[20];
			snprintf(label_text, sizeof(label_text), "Warning\n%d", i + 1);
			lv_label_set_text(warning_labels[i], label_text);
		}
		lv_obj_set_style_text_color(warning_labels[i], THEME_COLOR_TEXT_PRIMARY,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_opa(warning_labels[i], 255,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_align(warning_labels[i], LV_TEXT_ALIGN_CENTER,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(warning_labels[i], THEME_FONT_TINY,
								   LV_PART_MAIN | LV_STATE_DEFAULT);

		lv_obj_t *touch_area = lv_obj_create(parent);
		lv_obj_set_size(touch_area, 50, 80);
		lv_obj_set_x(touch_area, warning_positions[i].x);
		lv_obj_set_y(touch_area, warning_positions[i].y);
		lv_obj_set_align(touch_area, LV_ALIGN_CENTER);
		lv_obj_clear_flag(touch_area, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_bg_opa(touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(touch_area, 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);

		uint8_t *warning_id = lv_mem_alloc(sizeof(uint8_t));
		*warning_id = i;
		lv_obj_add_event_cb(touch_area, warning_longpress_cb,
							LV_EVENT_LONG_PRESSED, warning_id);

		update_warning_ui_immediate(i);
	}

	extern lv_obj_t *ui_Warning_1, *ui_Warning_2, *ui_Warning_3, *ui_Warning_4;
	extern lv_obj_t *ui_Warning_5, *ui_Warning_6, *ui_Warning_7, *ui_Warning_8;
	ui_Warning_1 = warning_circles[0];
	ui_Warning_2 = warning_circles[1];
	ui_Warning_3 = warning_circles[2];
	ui_Warning_4 = warning_circles[3];
	ui_Warning_5 = warning_circles[4];
	ui_Warning_6 = warning_circles[5];
	ui_Warning_7 = warning_circles[6];
	ui_Warning_8 = warning_circles[7];
}

void init_warning_configs(void) {
	for (int i = 0; i < 8; i++) {
		warning_configs[i].can_id = 0x000;
		warning_configs[i].bit_position = 0;
		warning_configs[i].endianess = 1;
		warning_configs[i].active_color = THEME_COLOR_RED;
		char buf[32];
		snprintf(buf, sizeof(buf), "Warning %d", i + 1);
		strncpy(warning_configs[i].label, buf,
				sizeof(warning_configs[i].label) - 1);
		warning_configs[i].is_momentary = true;
		warning_configs[i].current_state = false;
		warning_configs[i].invert_toggle = false;
	}
}

/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

static void _warning_create(widget_t *w, lv_obj_t *parent) {
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	if (slot == 0) {
		widget_warning_create(parent);
	}
	w->root = (slot < 8) ? warning_circles[slot] : NULL;
}
static void _warning_update(widget_t *w, void *data) {
	(void)w;
	update_warning_ui(data);
}
static void _warning_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, nw, nh);
	w->w = nw;
	w->h = nh;
}
static void _warning_open_settings(widget_t *w) {
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	create_warning_config_menu(slot);
}
static void _warning_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	if (slot < 8) {
		cJSON *cfg = cJSON_AddObjectToObject(out, "config");
		cJSON_AddNumberToObject(cfg, "slot", slot);
		cJSON_AddNumberToObject(cfg, "can_id", warning_configs[slot].can_id);
		cJSON_AddNumberToObject(cfg, "bit_position",
								warning_configs[slot].bit_position);
		cJSON_AddStringToObject(cfg, "label", warning_configs[slot].label);
		cJSON_AddBoolToObject(cfg, "is_momentary",
							  warning_configs[slot].is_momentary);
	}
}
static void _warning_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
}
static void _warning_destroy(widget_t *w) { free(w); }

widget_t *widget_warning_create_instance(uint8_t slot) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	w->type = WIDGET_WARNING;
	/* warning_positions[] is defined internally in widget_warning.c;
	 * we use 0/0 as the logical position — layout manager overrides via
	 * from_json. */
	w->x = 0;
	w->y = 0;
	w->w = 15;
	w->h = 15;
	w->type_data = (void *)(uintptr_t)(slot < 8 ? slot : 0);
	snprintf(w->id, sizeof(w->id), "warning_%u", slot < 8 ? slot : 0);

	w->create = _warning_create;
	w->update = _warning_update;
	w->resize = _warning_resize;
	w->open_settings = _warning_open_settings;
	w->to_json = _warning_to_json;
	w->from_json = _warning_from_json;
	w->destroy = _warning_destroy;

	return w;
}
