#include "widget_warning.h"
#include "widget_image.h"
#include "widget_rules.h"
#include "screen_config.h"
#include "widget_panel.h"
#include "system/night_mode.h"
#include "can/can_decode.h"
#include "driver/twai.h"
#include "esp_heap_caps.h"
#include "signal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "ui/callbacks/ui_callbacks.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/dashboard.h"
#include "widget_registry.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_warning";

/* ── Helper: look up warning_data_t by slot via registry ───────────────── */
static warning_data_t *_lookup_warning_data(uint8_t slot) {
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_WARNING, slot);
	return w ? (warning_data_t *)w->type_data : NULL;
}

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
static lv_obj_t *warning_touch_areas[8] = {NULL};
static lv_obj_t *warning_labels[8] = {NULL};
uint64_t last_signal_times[8] = {0};
bool toggle_debounce[8] = {false};
uint64_t toggle_start_time[8] = {0};
bool previous_bit_states[8] = {false};

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
		/* value_id is 1-based panel slot (1-8) */
		widget_panel_set_warning_high(value_id - 1, (float)atof(txt), true);
	}
}

void warning_low_threshold_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		const char *txt = lv_textarea_get_text(textarea);
		widget_panel_set_warning_low(value_id - 1, (float)atof(txt), true);
	}
}

void warning_high_color_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *dropdown = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		uint16_t selected = lv_dropdown_get_selected(dropdown);
		lv_color_t c = selected == 0 ? THEME_COLOR_RED : THEME_COLOR_BLUE_DARK;
		widget_panel_set_warning_high_color(value_id - 1, c);
	}
}

void warning_low_color_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *dropdown = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		uint16_t selected = lv_dropdown_get_selected(dropdown);
		lv_color_t c = selected == 0 ? THEME_COLOR_RED : THEME_COLOR_BLUE_DARK;
		widget_panel_set_warning_low_color(value_id - 1, c);
	}
}

__attribute__((unused)) static void warning_longpress_cb(lv_event_t *e) {
	/* Retained for future use — the long-press config menu wiring was moved
	   to the centralised dashboard long-press dispatcher. */
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
		ESP_LOGE(TAG, "Invalid save data for preconfig");
		return;
	}

	lv_obj_t **inputs = save_data->input_objects;

	// Get the warning dropdown from save_data
	lv_obj_t *warning_dd = save_data->preconfig_warning_dd;
	if (!warning_dd) {
		ESP_LOGE(TAG, "Warning dropdown not found in save_data");
		return;
	}

	uint16_t selected_warning = lv_dropdown_get_selected(warning_dd);
	if (selected_warning >=
		sizeof(ford_babf_warnings) / sizeof(ford_babf_warnings[0])) {
		ESP_LOGE(TAG, "Invalid warning selection");
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

	ESP_LOGI(TAG, "Applied preconfig: Ford BA/BF/FG - %s", preconfig->label);
}

static void save_warning_config_cb(lv_event_t *e) {
	warning_save_data_t *save_data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	if (!save_data) {
		ESP_LOGE(TAG, "Invalid save data");
		return;
	}

	uint8_t warning_idx = save_data->warning_idx;
	lv_obj_t **inputs = save_data->input_objects;

	if (!inputs) {
		ESP_LOGE(TAG, "Invalid input objects");
		lv_mem_free(save_data);
		return;
	}

	// Get values from inputs
	const char *can_id_text = lv_textarea_get_text(inputs[0]);
	uint8_t bit_pos = lv_dropdown_get_selected(inputs[1]);
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

	// Update warning type_data
	warning_data_t *wd = _lookup_warning_data(warning_idx);
	if (wd) {
		if (label_text) {
			safe_strncpy(wd->label, label_text, sizeof(wd->label));
		}

		// Handle highlighted color selection
		if (inputs[4]) {
			uint8_t selected_color = lv_dropdown_get_selected(inputs[4]);
			switch (selected_color) {
			case 0: wd->active_color = THEME_COLOR_GREEN; break;
			case 1: wd->active_color = THEME_COLOR_BLUE_PURE; break;
			case 2: wd->active_color = THEME_COLOR_ORANGE_WEB; break;
			case 3: wd->active_color = THEME_COLOR_RED; break;
			case 4: wd->active_color = THEME_COLOR_YELLOW; break;
			default: wd->active_color = THEME_COLOR_GREEN; break;
			}
		}

		// Save toggle mode setting
		if (inputs[5]) {
			bool was_momentary = wd->is_momentary;
			wd->is_momentary = (lv_dropdown_get_selected(inputs[5]) == 1);

			if (was_momentary != wd->is_momentary) {
				wd->current_state = false;
				previous_bit_states[warning_idx] = false;
				update_warning_ui_immediate(warning_idx);
			}
		}
	}

	// Add callbacks for live preview updates
	lv_obj_add_event_cb(inputs[3], label_text_cb, LV_EVENT_VALUE_CHANGED,
						save_data);
	lv_obj_add_event_cb(inputs[4], color_dropdown_cb, LV_EVENT_VALUE_CHANGED,
						save_data);

	// Debug output
	ESP_LOGI(TAG, "Warning %d configuration saved: CAN ID=0x%X bit=%d label=%s",
			 warning_idx + 1, can_id, bit_pos, label_text ? label_text : "");
	if (wd) {
		ESP_LOGI(TAG, "  Highlight Color: %06X  Mode: %s",
				 (unsigned)wd->active_color.full, wd->is_momentary ? "Momentary" : "Toggle");
	}

	// Update the label on Screen3 dynamically
	if (warning_labels[warning_idx] && wd) {
		lv_label_set_text(warning_labels[warning_idx], wd->label);
	}

	// Clean up
	lv_mem_free(inputs);
	lv_mem_free(save_data);

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

	/* Delegate to the immediate version which handles both image and circle */
	update_warning_ui_immediate(warning_idx);
}

/* Immediate warning update */
void update_warning_ui_immediate(uint8_t warning_idx) {
	if (warning_idx >= 8)
		return;
	if (warning_circles[warning_idx] == NULL ||
		lv_obj_get_screen(warning_circles[warning_idx]) == NULL) {
		return;
	}
	warning_data_t *wd = _lookup_warning_data(warning_idx);
	bool state = wd ? wd->current_state : false;
	lv_color_t active = wd ? wd->active_color : THEME_COLOR_RED;
	lv_color_t inactive = wd ? wd->inactive_color : THEME_COLOR_INACTIVE;
	lv_color_t new_color = state ? active : inactive;
	uint8_t active_opa = wd ? wd->active_opa : 255;
	uint8_t inactive_opa = wd ? wd->inactive_opa : 80;
	uint8_t new_opa = state ? active_opa : inactive_opa;

	bool is_image = wd && wd->img_obj != NULL;

	if (is_image) {
		/* Image mode: use recolor for tinting, img_opa for opacity */
		lv_obj_set_style_img_recolor(warning_circles[warning_idx], new_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_recolor_opa(warning_circles[warning_idx], LV_OPA_COVER,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_opa(warning_circles[warning_idx], new_opa,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	} else {
		/* Circle mode: use bg_color and bg_opa */
		lv_obj_set_style_bg_color(warning_circles[warning_idx], new_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(warning_circles[warning_idx], new_opa,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	if (warning_labels[warning_idx] &&
		lv_obj_is_valid(warning_labels[warning_idx])) {
		bool hide_label = wd && !wd->show_label;
		if (state && !hide_label) {
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
		ESP_LOGE(TAG, "Failed to allocate memory for input objects");
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
		ESP_LOGE(TAG, "Failed to allocate memory for preview objects");
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
		ESP_LOGE(TAG, "Failed to allocate memory for save data");
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
	warning_data_t *wd_cfg = _lookup_warning_data(warning_idx);
	lv_color_t preview_color = wd_cfg ? wd_cfg->active_color : THEME_COLOR_RED;
	lv_obj_set_style_bg_color(preview_circle, preview_color,
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
	lv_label_set_text(preview_label, wd_cfg ? wd_cfg->label : "Alert");
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
	lv_label_set_text_fmt(title, "Alert %d Configuration", warning_idx + 1);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

	// Create a container for inputs
	lv_obj_t *inputs_container = lv_obj_create(config_screen);
	lv_obj_set_size(inputs_container, SCREEN_W,
					SCREEN_H); /* Full-screen overlay */
	lv_obj_align(inputs_container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_opa(inputs_container, 0, 0);
	lv_obj_set_style_border_opa(inputs_container, 0, 0);
	lv_obj_clear_flag(inputs_container, LV_OBJ_FLAG_SCROLLABLE);

	// Warning label input (moved to top)
	lv_obj_t *label_text_label = lv_label_create(inputs_container);
	lv_label_set_text(label_text_label, "Alert Label:");
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
	lv_textarea_set_text(input_objects[3], wd_cfg ? wd_cfg->label : "Alert");

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
	snprintf(can_id_text, sizeof(can_id_text), "%X", 0); /* CAN ID now in signal */
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
	lv_dropdown_set_selected(input_objects[1], 0); /* bit pos now in signal */

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
	lv_color_t current_color = wd_cfg ? wd_cfg->active_color : THEME_COLOR_GREEN;
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
							 (wd_cfg && wd_cfg->is_momentary) ? 1 : 0);

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
	if (wd_cfg && wd_cfg->invert_toggle) {
		lv_obj_add_state(invert_toggle_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_clear_state(invert_toggle_switch, LV_STATE_CHECKED);
	}

	// Add event callback for invert toggle
	uint8_t *invert_toggle_id_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (!invert_toggle_id_ptr) return;
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
	lv_label_set_text(preconfig_warning_label, "Alert:");
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
	warning_data_t *wd = _lookup_warning_data(warning_idx);
	if (!wd) return;

	bool new_invert_toggle = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
	bool old_invert_toggle = wd->invert_toggle;

	if (new_invert_toggle != old_invert_toggle) {
		wd->current_state = !wd->current_state;
		previous_bit_states[warning_idx] = !previous_bit_states[warning_idx];
		update_warning_ui_immediate(warning_idx);

		ESP_LOGI("WARNING",
				 "Invert toggle changed for warning %d: %s -> %s, state "
				 "flipped to %s",
				 warning_idx, old_invert_toggle ? "enabled" : "disabled",
				 new_invert_toggle ? "enabled" : "disabled",
				 wd->current_state ? "ON" : "OFF");
	}

	wd->invert_toggle = new_invert_toggle;

	ESP_LOGI("WARNING", "Invert toggle %s for warning %d",
			 new_invert_toggle ? "enabled" : "disabled", warning_idx);
}
void check_warning_timeouts(lv_timer_t *timer) {
	// Timeout function is no longer needed for momentary warnings
	// as they now follow the live bit state directly.
	// This function is kept for potential future use but does nothing.
	(void)timer; // Suppress unused parameter warning
}

void widget_warning_reset(void) {
	for (int i = 0; i < 8; i++) {
		warning_circles[i] = NULL;
		warning_labels[i] = NULL;
		warning_touch_areas[i] = NULL;
	}
}

void widget_warning_create_one(lv_obj_t *parent, uint8_t i) {
	if (i >= 8)
		return;
	if (warning_circles[i] != NULL)
		return;

	warning_data_t *wd_style = _lookup_warning_data(i);
	bool use_image = wd_style && wd_style->image_name[0] != '\0';

	if (use_image) {
		/* Image mode: load RDMIMG and create lv_img object */
		wd_style->img_dsc = rdm_image_load(wd_style->image_name);
		if (wd_style->img_dsc) {
			warning_circles[i] = lv_img_create(parent);
			lv_img_set_src(warning_circles[i], wd_style->img_dsc);
			lv_obj_set_align(warning_circles[i], LV_ALIGN_CENTER);
			lv_obj_set_pos(warning_circles[i], warning_positions[i].x, warning_positions[i].y);
			/* Apply color overlay using recolor */
			lv_color_t init_color = wd_style->inactive_color;
			uint8_t init_opa = wd_style->inactive_opa;
			lv_obj_set_style_img_recolor(warning_circles[i], init_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_recolor_opa(warning_circles[i], LV_OPA_COVER,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_opa(warning_circles[i], init_opa,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
			wd_style->img_obj = warning_circles[i];
		} else {
			/* Image load failed, fall back to circle mode */
			ESP_LOGW(TAG, "Image '%s' not found for warning %d, using circle", wd_style->image_name, i);
			use_image = false;
		}
	}

	if (!use_image) {
		/* Circle mode — use widget w/h if available, else default 15x15 */
		widget_t *wt = widget_registry_find_by_type_and_slot(WIDGET_WARNING, i);
		int16_t cw = (wt && wt->w > 0) ? wt->w : 15;
		int16_t ch = (wt && wt->h > 0) ? wt->h : 15;

		warning_circles[i] = lv_obj_create(parent);
		lv_obj_set_size(warning_circles[i], cw, ch);
		lv_obj_set_x(warning_circles[i], warning_positions[i].x);
		lv_obj_set_y(warning_circles[i], warning_positions[i].y);
		lv_obj_set_align(warning_circles[i], LV_ALIGN_CENTER);
		lv_obj_clear_flag(warning_circles[i], LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_radius(warning_circles[i], wd_style ? wd_style->radius : 100,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_color_t init_color = wd_style ? wd_style->inactive_color : THEME_COLOR_INACTIVE;
		uint8_t init_opa = wd_style ? wd_style->inactive_opa : 80;
		lv_obj_set_style_bg_color(warning_circles[i], init_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(warning_circles[i], init_opa,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(warning_circles[i], wd_style ? wd_style->border_width : 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		if (wd_style && wd_style->border_width > 0) {
			lv_obj_set_style_border_color(warning_circles[i], wd_style->border_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_opa(warning_circles[i], 255,
										LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}

	warning_labels[i] = lv_label_create(parent);
	lv_obj_set_width(warning_labels[i], LV_SIZE_CONTENT);
	lv_obj_set_height(warning_labels[i], LV_SIZE_CONTENT);
	lv_obj_set_x(warning_labels[i], warning_positions[i].x);
	/* Position label just below the circle/image */
	{
		int16_t obj_h = lv_obj_get_height(warning_circles[i]);
		if (obj_h <= 0) obj_h = 15;
		lv_obj_set_y(warning_labels[i], warning_positions[i].y + obj_h / 2 + 4);
	}
	lv_obj_set_align(warning_labels[i], LV_ALIGN_CENTER);
	lv_obj_add_flag(warning_labels[i], LV_OBJ_FLAG_HIDDEN);
	/* If show_label is false, mark the label as permanently hidden using user data */
	if (wd_style && !wd_style->show_label) {
		lv_obj_set_user_data(warning_labels[i], (void *)1);
	}
	warning_data_t *wd_label = _lookup_warning_data(i);
	const char *saved_label = wd_label ? wd_label->label : NULL;
	if (saved_label && saved_label[0] != '\0') {
		lv_label_set_text(warning_labels[i], saved_label);
	} else {
		char label_text[20];
		snprintf(label_text, sizeof(label_text), "Alert\n%d", i + 1);
		lv_label_set_text(warning_labels[i], label_text);
	}
	lv_obj_set_style_text_color(warning_labels[i], wd_style ? wd_style->label_color : THEME_COLOR_TEXT_PRIMARY,
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

	/* Store touch area so _warning_create can use it as w->root.
	 * Long-press is handled by dashboard.c's _register_widget_long_press(). */
	warning_touch_areas[i] = touch_area;

	update_warning_ui_immediate(i);

	// Sync global ui_Warning_X pointers for SquareLine compatibility
	extern lv_obj_t *ui_Warning_1, *ui_Warning_2, *ui_Warning_3, *ui_Warning_4;
	extern lv_obj_t *ui_Warning_5, *ui_Warning_6, *ui_Warning_7, *ui_Warning_8;
	lv_obj_t **globals[] = {&ui_Warning_1, &ui_Warning_2, &ui_Warning_3,
							&ui_Warning_4, &ui_Warning_5, &ui_Warning_6,
							&ui_Warning_7, &ui_Warning_8};
	if (i < 8) {
		*globals[i] = warning_circles[i];
	}
}

void widget_warning_create(lv_obj_t *parent) {
	for (int i = 0; i < 8; i++) {
		widget_warning_create_one(parent, (uint8_t)i);
	}
}


/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

static void _warning_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	warning_data_t *wd = (warning_data_t *)w->type_data;
	if (!wd) return;
	uint8_t slot = wd->slot;
	if (slot >= 8) return;
	bool bit_on = !is_stale && (value != 0.0f);
	if (wd->invert_toggle) bit_on = !bit_on;
	wd->current_state = bit_on;
	update_warning_ui_immediate(slot);
}

/* Forward declarations — used by _warning_create / _warning_destroy below. */
static void _warning_apply_night_mode(widget_t *w, bool active);
static void _warning_night_cb(bool active, void *user_data);

static void _warning_create(widget_t *w, lv_obj_t *parent) {
	warning_data_t *wd = (warning_data_t *)w->type_data;
	uint8_t slot = wd ? wd->slot : 0;
	widget_warning_create_one(parent, slot);
	/* Use the touch area as w->root so long-press always works
	 * (the circle may have ADV_HITTEST or be tiny/invisible). */
	w->root = (slot < 8) ? warning_touch_areas[slot] : NULL;

	/* Subscribe to signal if bound */
	if (wd && wd->signal_index >= 0)
		signal_subscribe(wd->signal_index, _warning_on_signal, w);

	/* Subscribe to night-mode changes if any night override is set. */
	if (wd && (wd->night.has_active_color || wd->night.has_inactive_color ||
	           wd->night.has_border_color || wd->night.has_label_color ||
	           wd->night.has_image_name)) {
		night_mode_subscribe(_warning_night_cb, w);
		_warning_apply_night_mode(w, night_mode_is_active());
	}
}
static void _warning_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, nw, nh);
	w->w = nw;
	w->h = nh;
	/* Reposition label below the resized circle */
	warning_data_t *wd = (warning_data_t *)w->type_data;
	if (wd && wd->slot < 8 && warning_labels[wd->slot] &&
	    lv_obj_is_valid(warning_labels[wd->slot])) {
		lv_obj_set_y(warning_labels[wd->slot], w->y + nh / 2 + 4);
	}
}
static void _warning_open_settings(widget_t *w) {
	load_menu_screen_for_widget(w);
}
static void _warning_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	warning_data_t *wd = (warning_data_t *)w->type_data;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	if (wd) {
		cJSON_AddNumberToObject(cfg, "slot", wd->slot);
		cJSON_AddNumberToObject(cfg, "active_color", (int)wd->active_color.full);
		cJSON_AddStringToObject(cfg, "label", wd->label);
		cJSON_AddBoolToObject(cfg, "is_momentary", wd->is_momentary);
		cJSON_AddBoolToObject(cfg, "invert_toggle", wd->invert_toggle);
		if (wd->signal_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "signal_name", wd->signal_name);
		/* Appearance overrides — only serialize non-default values */
		if (wd->inactive_color.full != THEME_COLOR_INACTIVE.full)
			cJSON_AddNumberToObject(cfg, "inactive_color", (int)wd->inactive_color.full);
		if (wd->border_width != 0)
			cJSON_AddNumberToObject(cfg, "border_width", wd->border_width);
		if (wd->border_color.full != lv_color_hex(0x000000).full)
			cJSON_AddNumberToObject(cfg, "border_color_style", (int)wd->border_color.full);
		if (wd->radius != 100)
			cJSON_AddNumberToObject(cfg, "radius", wd->radius);
		if (!wd->show_label)
			cJSON_AddBoolToObject(cfg, "show_label", false);
		if (wd->label_color.full != THEME_COLOR_TEXT_PRIMARY.full)
			cJSON_AddNumberToObject(cfg, "label_color", (int)wd->label_color.full);
		if (wd->image_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "image_name", wd->image_name);
		if (wd->active_opa != 255)
			cJSON_AddNumberToObject(cfg, "active_opa", wd->active_opa);
		if (wd->inactive_opa != 80)
			cJSON_AddNumberToObject(cfg, "inactive_opa", wd->inactive_opa);
		/* Night-mode overrides — emit only fields that have an override set */
		{
			cJSON *n = cJSON_CreateObject();
			NIGHT_SERIALIZE_COLOR(n, wd->night, active_color);
			NIGHT_SERIALIZE_COLOR(n, wd->night, inactive_color);
			NIGHT_SERIALIZE_COLOR(n, wd->night, border_color);
			NIGHT_SERIALIZE_COLOR(n, wd->night, label_color);
			NIGHT_SERIALIZE_IMAGE(n, wd->night, image_name);
			if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
			else cJSON_Delete(n);
		}
	}
}
static void _warning_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
	warning_data_t *wd = (warning_data_t *)w->type_data;
	if (!wd) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;
	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	if (cJSON_IsNumber(item)) {
		wd->slot = (uint8_t)item->valueint;
		w->slot = wd->slot;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "active_color");
	if (cJSON_IsNumber(item)) wd->active_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(wd->label, item->valuestring, sizeof(wd->label));
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "is_momentary");
	if (cJSON_IsBool(item)) wd->is_momentary = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "invert_toggle");
	if (cJSON_IsBool(item)) wd->invert_toggle = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(wd->signal_name, item->valuestring, sizeof(wd->signal_name));
	}

	/* Appearance overrides */
	item = cJSON_GetObjectItemCaseSensitive(cfg, "inactive_color");
	if (cJSON_IsNumber(item)) wd->inactive_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "border_width");
	if (cJSON_IsNumber(item)) wd->border_width = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "border_color_style");
	if (cJSON_IsNumber(item)) wd->border_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "radius");
	if (cJSON_IsNumber(item)) wd->radius = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_label");
	if (cJSON_IsBool(item)) wd->show_label = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label_color");
	if (cJSON_IsNumber(item)) wd->label_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "image_name");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(wd->image_name, item->valuestring, sizeof(wd->image_name));
	item = cJSON_GetObjectItemCaseSensitive(cfg, "active_opa");
	if (cJSON_IsNumber(item)) wd->active_opa = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "inactive_opa");
	if (cJSON_IsNumber(item)) wd->inactive_opa = (uint8_t)item->valueint;

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, wd->night, active_color);
		NIGHT_PARSE_COLOR(night, wd->night, inactive_color);
		NIGHT_PARSE_COLOR(night, wd->night, border_color);
		NIGHT_PARSE_COLOR(night, wd->night, label_color);
		NIGHT_PARSE_IMAGE(night, wd->night, image_name);
	}

	/* Resolve signal name → index */
	if (wd->signal_name[0] != '\0')
		wd->signal_index = signal_find_by_name(wd->signal_name);
}
static void _warning_destroy(widget_t *w) {
	warning_data_t *wd = (warning_data_t *)w->type_data;
	uint8_t slot = wd ? wd->slot : 0;
	if (wd && wd->signal_index >= 0)
		signal_unsubscribe(wd->signal_index, _warning_on_signal, w);
	night_mode_unsubscribe(_warning_night_cb, w);
	widget_rules_free(w);
	/* Label is a sibling of root (child of parent), delete explicitly */
	if (slot < 8 && warning_labels[slot] && lv_obj_is_valid(warning_labels[slot]))
		lv_obj_del(warning_labels[slot]);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	/* Clear static and global pointers */
	if (slot < 8) {
		warning_circles[slot] = NULL;
		warning_labels[slot] = NULL;
		extern lv_obj_t *ui_Warning_1, *ui_Warning_2, *ui_Warning_3, *ui_Warning_4;
		extern lv_obj_t *ui_Warning_5, *ui_Warning_6, *ui_Warning_7, *ui_Warning_8;
		lv_obj_t **globals[] = {&ui_Warning_1, &ui_Warning_2, &ui_Warning_3,
								&ui_Warning_4, &ui_Warning_5, &ui_Warning_6,
								&ui_Warning_7, &ui_Warning_8};
		*globals[slot] = NULL;
	}
	if (wd) {
		rdm_image_free(wd->img_dsc);
		wd->img_dsc = NULL;
		wd->img_obj = NULL;
	}
	free(w->type_data);
	free(w);
}

static void _warning_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	warning_data_t *wd = (warning_data_t *)w->type_data;
	if (!wd) return;
	uint8_t slot = wd->slot;

	/* Start from base warning_data_t values (restore defaults) */
	lv_color_t active_color   = wd->active_color;
	lv_color_t inactive_color = wd->inactive_color;
	lv_color_t bdr_color      = wd->border_color;
	uint8_t    bdr_width      = wd->border_width;
	lv_color_t lbl_color      = wd->label_color;

	/* Apply active overrides on top */
	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "active_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			lv_color_t c; c.full = (uint16_t)o->value.color;
			active_color = c;
		} else if (strcmp(o->field_name, "inactive_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			lv_color_t c; c.full = (uint16_t)o->value.color;
			inactive_color = c;
		} else if (strcmp(o->field_name, "border_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bdr_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "border_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bdr_width = (uint8_t)o->value.num;
		} else if (strcmp(o->field_name, "label_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			lbl_color.full = (uint16_t)o->value.color;
		}
	}

	/* Apply color based on current on/off state */
	lv_color_t cur_color = wd->current_state ? active_color : inactive_color;
	uint8_t cur_opa = wd->current_state ? wd->active_opa : wd->inactive_opa;

	if (wd->img_obj != NULL) {
		/* Image mode */
		lv_obj_set_style_img_recolor(w->root, cur_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_recolor_opa(w->root, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_opa(w->root, cur_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	} else {
		/* Circle mode */
		lv_obj_set_style_bg_color(w->root, cur_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(w->root, cur_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Apply border styles (circle mode only) */
	if (wd->img_obj == NULL) {
		lv_obj_set_style_border_color(w->root, bdr_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(w->root, bdr_width, LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Apply label color if label exists */
	if (slot < 8 && warning_labels[slot] && lv_obj_is_valid(warning_labels[slot])) {
		lv_obj_set_style_text_color(warning_labels[slot], lbl_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

/* Re-apply colors/image based on current night-mode state. Called by
 * night_mode subscribers. Image swap is not supported at runtime — swapping
 * the image requires re-creating the LVGL image object, so we only apply
 * colors here. TODO: image swap via re-create if needed. */
static void _warning_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	warning_data_t *wd = (warning_data_t *)w->type_data;
	if (!wd) return;
	uint8_t slot = wd->slot;

	lv_color_t active_c   = NIGHT_PICK_COLOR(active, wd->night, active_color,   wd->active_color);
	lv_color_t inactive_c = NIGHT_PICK_COLOR(active, wd->night, inactive_color, wd->inactive_color);
	lv_color_t bdr_c      = NIGHT_PICK_COLOR(active, wd->night, border_color,  wd->border_color);
	lv_color_t lbl_c      = NIGHT_PICK_COLOR(active, wd->night, label_color,   wd->label_color);

	/* Which color is currently shown depends on state */
	lv_color_t cur = wd->current_state ? active_c : inactive_c;
	uint8_t cur_opa = wd->current_state ? wd->active_opa : wd->inactive_opa;

	if (slot < 8 && warning_circles[slot] && lv_obj_is_valid(warning_circles[slot])) {
		if (wd->img_obj != NULL) {
			/* Image mode */
			lv_obj_set_style_img_recolor(warning_circles[slot], cur,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_recolor_opa(warning_circles[slot], LV_OPA_COVER,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_opa(warning_circles[slot], cur_opa,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			/* Circle mode */
			lv_obj_set_style_bg_color(warning_circles[slot], cur,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_opa(warning_circles[slot], cur_opa,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			if (wd->border_width > 0) {
				lv_obj_set_style_border_color(warning_circles[slot], bdr_c,
					LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		}
	}

	if (slot < 8 && warning_labels[slot] && lv_obj_is_valid(warning_labels[slot])) {
		lv_obj_set_style_text_color(warning_labels[slot], lbl_c,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _warning_night_cb(bool active, void *user_data) {
	_warning_apply_night_mode((widget_t *)user_data, active);
}

widget_t *widget_warning_create_instance(uint8_t slot) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	warning_data_t *wd = heap_caps_calloc(1, sizeof(warning_data_t), MALLOC_CAP_SPIRAM);
	if (!wd) wd = calloc(1, sizeof(warning_data_t));
	if (!wd) { free(w); return NULL; }

	uint8_t s = slot < 8 ? slot : 0;
	wd->slot = s;
	wd->active_color = THEME_COLOR_RED;
	snprintf(wd->label, sizeof(wd->label), "Alert %u", s + 1);
	wd->is_momentary = true;
	wd->invert_toggle = false;
	wd->current_state = false;
	wd->signal_index = -1;
	wd->inactive_color = THEME_COLOR_INACTIVE;
	wd->border_width = 0;
	wd->border_color = lv_color_hex(0x000000);
	wd->radius = 100;
	wd->show_label = true;
	wd->label_color = THEME_COLOR_TEXT_PRIMARY;
	wd->image_name[0] = '\0';
	wd->active_opa = 255;
	wd->inactive_opa = 80;
	wd->img_dsc = NULL;
	wd->img_obj = NULL;

	w->type = WIDGET_WARNING;
	w->slot = s;
	w->x = 0;
	w->y = 0;
	w->w = 25;
	w->h = 25;
	w->type_data = wd;
	snprintf(w->id, sizeof(w->id), "warning_%u", s);

	w->create = _warning_create;
	w->resize = _warning_resize;
	w->open_settings = _warning_open_settings;
	w->to_json = _warning_to_json;
	w->from_json = _warning_from_json;
	w->destroy = _warning_destroy;
	w->apply_overrides = _warning_apply_overrides;
	w->apply_night_mode = _warning_apply_night_mode;

	return w;
}

uint8_t widget_warning_get_slot(const widget_t *w) {
	if (!w || w->type != WIDGET_WARNING || !w->type_data) return 0;
	return ((const warning_data_t *)w->type_data)->slot;
}

bool widget_warning_has_signal(const widget_t *w) {
	if (!w || w->type != WIDGET_WARNING || !w->type_data) return false;
	return ((const warning_data_t *)w->type_data)->signal_index >= 0;
}
