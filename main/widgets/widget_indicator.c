#include "widget_indicator.h"
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

void update_indicator_ui_immediate(uint8_t indicator_idx);

bool indicator_toggle_debounce[2] = {false};
uint64_t indicator_toggle_start_time[2] = {0};
bool previous_indicator_bit_states[2] = {false};

// Animation variables
lv_timer_t *indicator_animation_timer = NULL;
static bool indicator_animation_state = false;
static bool previous_indicator_states[2] = {false, false};

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
void indicator_longpress_cb(lv_event_t *e) {
	void *user_data = lv_event_get_user_data(e);
	if (!user_data) {
		printf("Error: No user data in indicator longpress callback\n");
		return;
	}

	uint8_t indicator_idx = *(uint8_t *)user_data;
	printf("Indicator longpress detected for indicator %d\n", indicator_idx);
	create_indicator_config_menu(indicator_idx);
}

static void indicator_input_src_changed_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	indicator_input_visibility_t *vis =
		(indicator_input_visibility_t *)lv_event_get_user_data(e);
	if (!vis)
		return;
	uint16_t sel = lv_dropdown_get_selected(dropdown);
	bool show_can = (sel == 1); /* 0 = Wire, 1 = CAN BUS */
	if (vis->can_id_label)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->can_id_label,
														 LV_OBJ_FLAG_HIDDEN);
	if (vis->can_id_0x)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->can_id_0x,
														 LV_OBJ_FLAG_HIDDEN);
	if (vis->can_id_input)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->can_id_input,
														 LV_OBJ_FLAG_HIDDEN);
	if (vis->bit_pos_label)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->bit_pos_label,
														 LV_OBJ_FLAG_HIDDEN);
	if (vis->bit_pos_dropdown)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->bit_pos_dropdown,
														 LV_OBJ_FLAG_HIDDEN);
	if (vis->toggle_mode_label)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->toggle_mode_label,
														 LV_OBJ_FLAG_HIDDEN);
	if (vis->toggle_mode_dropdown)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(
			vis->toggle_mode_dropdown, LV_OBJ_FLAG_HIDDEN);
	if (vis->animation_label)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->animation_label,
														 LV_OBJ_FLAG_HIDDEN);
	if (vis->animation_switch)
		(show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->animation_switch,
														 LV_OBJ_FLAG_HIDDEN);
}

static void indicator_config_screen_delete_cb(lv_event_t *e) {
	indicator_input_visibility_t *vis =
		(indicator_input_visibility_t *)lv_event_get_user_data(e);
	if (vis)
		lv_mem_free(vis);
}

static void save_indicator_config_cb(lv_event_t *e) {
	indicator_input_visibility_t *vis =
		(indicator_input_visibility_t *)lv_event_get_user_data(e);
	if (vis && vis->input_src_dropdown &&
		lv_obj_is_valid(vis->input_src_dropdown) && vis->indicator_idx < 2) {
		uint8_t new_src =
			(uint8_t)lv_dropdown_get_selected(vis->input_src_dropdown);
		indicator_configs[vis->indicator_idx].input_source = new_src;
		/* When switching to CAN BUS, turn indicator off until CAN message sets
		 * it on */
		if (new_src == 1) {
			indicator_configs[vis->indicator_idx].current_state = false;
			previous_indicator_states[vis->indicator_idx] = false;
		}
		config_store_save_indicators(indicator_configs, 2);
		/* Sync main screen indicator opacity to current state before returning
		 */
		update_indicator_ui_immediate(0);
		update_indicator_ui_immediate(1);
	}
	printf("Indicator configuration completed - returning to main screen\n");

	// Clear preview references
	preview_indicator_config = NULL;
	preview_status_text_config = NULL;
	preview_indicator_idx = 0;

	// Return to Screen3
	lv_scr_load(ui_Screen3);
}

static void back_indicator_config_cb(lv_event_t *e) {
	// Clear preview references
	preview_indicator_config = NULL;
	preview_status_text_config = NULL;
	preview_indicator_idx = 0;

	// Return to Screen3 without saving
	lv_scr_load(ui_Screen3);
}

// Callback for CAN ID input changes
static void indicator_can_id_changed_cb(lv_event_t *e) {
	lv_obj_t *input = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!input || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	const char *can_id_text = lv_textarea_get_text(input);

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
	config_store_save_indicators(indicator_configs, 2);

	printf("Indicator %d CAN ID updated to: 0x%X\n", indicator_idx, can_id);
}

// Callback for bit position dropdown changes
static void indicator_bit_pos_changed_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!dropdown || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	uint8_t bit_pos = lv_dropdown_get_selected(dropdown);

	// Update configuration and save to NVS
	indicator_configs[indicator_idx].bit_position = bit_pos;
	printf(
		"Calling save_indicator_configs_to_nvs() for bit position change...\n");
	config_store_save_indicators(indicator_configs, 2);

	printf("Indicator %d bit position updated to: %d\n", indicator_idx,
		   bit_pos);
}

// Callback for toggle mode dropdown changes
static void indicator_toggle_mode_changed_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!dropdown || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	bool is_momentary = (lv_dropdown_get_selected(dropdown) == 0);

	// Update configuration and save to NVS
	indicator_configs[indicator_idx].is_momentary = is_momentary;
	printf(
		"Calling save_indicator_configs_to_nvs() for toggle mode change...\n");
	config_store_save_indicators(indicator_configs, 2);

	printf("Indicator %d toggle mode updated to: %s\n", indicator_idx,
		   is_momentary ? "Momentary" : "Toggle");
}

static void indicator_animation_changed_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!switch_obj || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	bool is_enabled = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	// Update configuration and save to NVS
	indicator_configs[indicator_idx].animation_enabled = is_enabled;
	printf("Calling save_indicator_configs_to_nvs() for animation change...\n");
	config_store_save_indicators(indicator_configs, 2);

	printf("Indicator %d animation updated to: %s\n", indicator_idx,
		   is_enabled ? "Enabled" : "Disabled");
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
	lv_obj_t *config_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(config_screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(config_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(config_screen, LV_OBJ_FLAG_SCROLLABLE);

	// Create main border/background panel
	lv_obj_t *main_border = lv_obj_create(config_screen);
	lv_obj_set_width(main_border, 780);
	lv_obj_set_height(main_border, 325);
	lv_obj_set_align(main_border, LV_ALIGN_CENTER);
	lv_obj_set_y(main_border, 67);
	lv_obj_clear_flag(main_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(main_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(main_border, THEME_COLOR_INACTIVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(main_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(main_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create input border panel
	lv_obj_t *input_border = lv_obj_create(config_screen);
	lv_obj_set_width(input_border, 275);
	lv_obj_set_height(input_border, 310);
	lv_obj_set_x(input_border, -244);
	lv_obj_set_y(input_border, 67);
	lv_obj_set_align(input_border, LV_ALIGN_CENTER);
	lv_obj_clear_flag(input_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(input_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(input_border, THEME_COLOR_INPUT_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(input_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(input_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create preview panel to contain the indicator and status
	lv_obj_t *preview_panel = lv_obj_create(config_screen);
	lv_obj_set_width(preview_panel, 300);
	lv_obj_set_height(preview_panel, 200);
	lv_obj_set_x(preview_panel, 130);
	lv_obj_set_y(preview_panel, 67);
	lv_obj_set_align(preview_panel, LV_ALIGN_CENTER);
	lv_obj_clear_flag(preview_panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(preview_panel, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(preview_panel, THEME_COLOR_SURFACE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(preview_panel, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(preview_panel, 1,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(preview_panel, THEME_COLOR_CONTROL_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create preview title
	lv_obj_t *preview_title = lv_label_create(preview_panel);
	lv_label_set_text(preview_title, "Live Preview");
	lv_obj_align(preview_title, LV_ALIGN_TOP_MID, 0, 10);
	lv_obj_set_style_text_color(preview_title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(preview_title, THEME_FONT_BODY, 0);

	// Create preview indicator centered in the panel
	lv_obj_t *preview_indicator = lv_img_create(preview_panel);
	lv_obj_set_width(preview_indicator, LV_SIZE_CONTENT);
	lv_obj_set_height(preview_indicator, LV_SIZE_CONTENT);

	// Position the preview indicator based on which indicator is being
	// configured
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
		lv_obj_set_style_opa(preview_indicator, 255,
							 LV_PART_MAIN |
								 LV_STATE_DEFAULT); // 100% opacity when active
	} else {
		lv_obj_set_style_opa(preview_indicator, 50,
							 LV_PART_MAIN |
								 LV_STATE_DEFAULT); // 50% opacity when inactive
	}

	// Create status text below the indicator
	lv_obj_t *status_text = lv_label_create(preview_panel);
	lv_label_set_text_fmt(
		status_text, "%s INDICATOR\n%s", indicator_idx == 0 ? "LEFT" : "RIGHT",
		(indicator_idx < 2 && indicator_configs[indicator_idx].current_state)
			? "ACTIVE"
			: "INACTIVE");
	lv_obj_align(status_text, LV_ALIGN_CENTER, 0, 30);

	// Set text color based on state
	if (indicator_idx < 2 && indicator_configs[indicator_idx].current_state) {
		lv_obj_set_style_text_color(status_text, THEME_COLOR_GREEN,
									0); // Green when active
	} else {
		lv_obj_set_style_text_color(status_text, THEME_COLOR_TEXT_MUTED,
									0); // Gray when inactive
	}

	lv_obj_set_style_text_align(status_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_text, THEME_FONT_SMALL, 0);

	// Store references for live preview updates
	preview_indicator_config = preview_indicator;
	preview_status_text_config = status_text;
	preview_indicator_idx = indicator_idx;

	// Create the keyboard
	keyboard = lv_keyboard_create(config_screen);
	lv_obj_set_parent(keyboard, lv_layer_top());
	lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY,
						NULL);

	// Create title
	lv_obj_t *title = lv_label_create(config_screen);
	lv_label_set_text_fmt(title, "%s Indicator Configuration",
						  indicator_idx == 0 ? "Left" : "Right");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);

	// Create a container for inputs
	lv_obj_t *inputs_container = lv_obj_create(config_screen);
	lv_obj_set_size(inputs_container, 800, 480);
	lv_obj_align(inputs_container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_opa(inputs_container, 0, 0);
	lv_obj_set_style_border_opa(inputs_container, 0, 0);
	lv_obj_clear_flag(inputs_container, LV_OBJ_FLAG_SCROLLABLE);

	// Create input section title
	lv_obj_t *input_title = lv_label_create(inputs_container);
	lv_label_set_text(input_title, "Configuration Settings");
	lv_obj_align(input_title, LV_ALIGN_CENTER, -244, -75);
	lv_obj_set_style_text_color(input_title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(input_title, THEME_FONT_BODY, 0);

	// INPUT: Wire / CANBUS dropdown (top row - functionality to be added)
	lv_obj_t *input_src_label = lv_label_create(inputs_container);
	lv_label_set_text(input_src_label, "INPUT:");
	lv_obj_set_width(input_src_label, 110);
	lv_obj_set_style_text_align(input_src_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(input_src_label, LV_ALIGN_CENTER, -312, -32);
	lv_obj_set_style_text_color(input_src_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(input_src_label, THEME_FONT_SMALL, 0);

	lv_obj_t *input_src_dropdown = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_src_dropdown, get_common_style(), LV_PART_MAIN);
	lv_dropdown_set_options(input_src_dropdown, "Wire\nCAN BUS");
	lv_obj_set_width(input_src_dropdown, 120);
	lv_obj_align(input_src_dropdown, LV_ALIGN_CENTER, -180, -32);
	lv_dropdown_set_selected(
		input_src_dropdown,
		indicator_idx < 2 ? indicator_configs[indicator_idx].input_source : 0);
	if (lv_dropdown_get_selected(input_src_dropdown) > 1)
		lv_dropdown_set_selected(input_src_dropdown, 0);

	// CAN ID input
	lv_obj_t *can_id_label = lv_label_create(inputs_container);
	lv_label_set_text(can_id_label, "CAN ID:");
	lv_obj_set_width(can_id_label, 110);
	lv_obj_set_style_text_align(can_id_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(can_id_label, LV_ALIGN_CENTER, -312, 8);
	lv_obj_set_style_text_color(can_id_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(can_id_label, THEME_FONT_SMALL, 0);

	// CAN ID "0x" prefix
	lv_obj_t *can_id_0x = lv_label_create(inputs_container);
	lv_label_set_text(can_id_0x, "0x");
	lv_obj_set_width(can_id_0x, 20);
	lv_obj_set_style_text_color(can_id_0x, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_align(can_id_0x, LV_ALIGN_CENTER, -200, 8);
	lv_obj_set_style_text_align(can_id_0x, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_style_text_font(can_id_0x, THEME_FONT_SMALL, 0);

	lv_obj_t *can_id_input = lv_textarea_create(inputs_container);
	lv_obj_add_style(can_id_input, get_common_style(), LV_PART_MAIN);
	lv_textarea_set_one_line(can_id_input, true);
	lv_obj_set_width(can_id_input, 120);
	lv_obj_align(can_id_input, LV_ALIGN_CENTER, -180, 8);
	lv_obj_add_event_cb(can_id_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

	// Add real-time saving callback for CAN ID - use both VALUE_CHANGED and
	// DEFOCUSED events
	uint8_t *can_id_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (can_id_idx_ptr) {
		*can_id_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(can_id_input, indicator_can_id_changed_cb,
							LV_EVENT_VALUE_CHANGED, can_id_idx_ptr);
		lv_obj_add_event_cb(can_id_input, indicator_can_id_changed_cb,
							LV_EVENT_DEFOCUSED, can_id_idx_ptr);
		printf("Added CAN ID callbacks for indicator %d\n", indicator_idx);
	}

	// Set placeholder and initial value
	char can_id_text[32];
	if (indicator_idx < 2) {
		snprintf(can_id_text, sizeof(can_id_text), "%X",
				 indicator_configs[indicator_idx].can_id);
	} else {
		snprintf(can_id_text, sizeof(can_id_text), "0");
	}
	lv_textarea_set_text(can_id_input, can_id_text);
	lv_textarea_set_placeholder_text(can_id_input, "Enter CAN ID");

	// Bit position dropdown
	lv_obj_t *bit_pos_label = lv_label_create(inputs_container);
	lv_label_set_text(bit_pos_label, "Bit Position:");
	lv_obj_set_width(bit_pos_label, 110);
	lv_obj_set_style_text_align(bit_pos_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(bit_pos_label, LV_ALIGN_CENTER, -312, 48);
	lv_obj_set_style_text_color(bit_pos_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(bit_pos_label, THEME_FONT_SMALL, 0);

	lv_obj_t *bit_pos_dropdown = lv_dropdown_create(inputs_container);
	lv_obj_add_style(bit_pos_dropdown, get_common_style(), LV_PART_MAIN);
	lv_dropdown_set_options(
		bit_pos_dropdown,
		"0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
		"16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
		"32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
		"48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63");
	lv_obj_set_width(bit_pos_dropdown, 120);
	lv_obj_align(bit_pos_dropdown, LV_ALIGN_CENTER, -180, 48);

	// Add real-time saving callback for bit position
	uint8_t *bit_pos_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (bit_pos_idx_ptr) {
		*bit_pos_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(bit_pos_dropdown, indicator_bit_pos_changed_cb,
							LV_EVENT_VALUE_CHANGED, bit_pos_idx_ptr);
		printf("Added bit position callback for indicator %d\n", indicator_idx);
	}

	if (indicator_idx < 2) {
		lv_dropdown_set_selected(bit_pos_dropdown,
								 indicator_configs[indicator_idx].bit_position);
	} else {
		lv_dropdown_set_selected(bit_pos_dropdown, 0);
	}

	// Toggle mode dropdown
	lv_obj_t *toggle_mode_label = lv_label_create(inputs_container);
	lv_label_set_text(toggle_mode_label, "Toggle Mode:");
	lv_obj_set_width(toggle_mode_label, 110);
	lv_obj_set_style_text_align(toggle_mode_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(toggle_mode_label, LV_ALIGN_CENTER, -312, 88);
	lv_obj_set_style_text_color(toggle_mode_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(toggle_mode_label, THEME_FONT_SMALL, 0);

	lv_obj_t *toggle_mode_dropdown = lv_dropdown_create(inputs_container);
	lv_obj_add_style(toggle_mode_dropdown, get_common_style(), LV_PART_MAIN);
	lv_dropdown_set_options(toggle_mode_dropdown, "Momentary\nToggle");
	lv_obj_set_width(toggle_mode_dropdown, 120);
	lv_obj_align(toggle_mode_dropdown, LV_ALIGN_CENTER, -180, 88);

	// Add real-time saving callback for toggle mode
	uint8_t *toggle_mode_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (toggle_mode_idx_ptr) {
		*toggle_mode_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(toggle_mode_dropdown,
							indicator_toggle_mode_changed_cb,
							LV_EVENT_VALUE_CHANGED, toggle_mode_idx_ptr);
		printf("Added toggle mode callback for indicator %d\n", indicator_idx);
	}

	if (indicator_idx < 2) {
		lv_dropdown_set_selected(
			toggle_mode_dropdown,
			indicator_configs[indicator_idx].is_momentary ? 0 : 1);
	} else {
		lv_dropdown_set_selected(toggle_mode_dropdown, 0);
	}

	// Animation setting
	lv_obj_t *animation_label = lv_label_create(inputs_container);
	lv_label_set_text(animation_label, "Animation:");
	lv_obj_set_width(animation_label, 110);
	lv_obj_set_style_text_align(animation_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(animation_label, LV_ALIGN_CENTER, -312, 128);
	lv_obj_set_style_text_color(animation_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(animation_label, THEME_FONT_SMALL, 0);

	lv_obj_t *animation_switch = lv_switch_create(inputs_container);
	lv_obj_add_style(animation_switch, get_common_style(), LV_PART_MAIN);
	lv_obj_set_width(animation_switch, 50);
	lv_obj_align(animation_switch, LV_ALIGN_CENTER, -155, 128);

	// Add real-time saving callback for animation
	uint8_t *animation_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (animation_idx_ptr) {
		*animation_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(animation_switch, indicator_animation_changed_cb,
							LV_EVENT_VALUE_CHANGED, animation_idx_ptr);
		printf("Added animation callback for indicator %d\n", indicator_idx);
	}

	if (indicator_idx < 2) {
		if (indicator_configs[indicator_idx].animation_enabled) {
			lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_clear_state(animation_switch, LV_STATE_CHECKED);
		}
	} else {
		lv_obj_add_state(animation_switch,
						 LV_STATE_CHECKED); // Default to enabled
	}

	/* Wire vs CAN BUS: when Wire selected, hide CAN ID / bit / toggle /
	 * animation */
	indicator_input_visibility_t *vis =
		lv_mem_alloc(sizeof(indicator_input_visibility_t));
	if (vis) {
		vis->indicator_idx = indicator_idx;
		vis->input_src_dropdown = input_src_dropdown;
		vis->can_id_label = can_id_label;
		vis->can_id_0x = can_id_0x;
		vis->can_id_input = can_id_input;
		vis->bit_pos_label = bit_pos_label;
		vis->bit_pos_dropdown = bit_pos_dropdown;
		vis->toggle_mode_label = toggle_mode_label;
		vis->toggle_mode_dropdown = toggle_mode_dropdown;
		vis->animation_label = animation_label;
		vis->animation_switch = animation_switch;
		lv_obj_add_event_cb(input_src_dropdown, indicator_input_src_changed_cb,
							LV_EVENT_VALUE_CHANGED, vis);
		lv_obj_add_event_cb(config_screen, indicator_config_screen_delete_cb,
							LV_EVENT_DELETE, vis);
		/* Initial state: Wire (0) = hide CAN rows */
		if (lv_dropdown_get_selected(input_src_dropdown) == 0) {
			lv_obj_add_flag(can_id_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(can_id_0x, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(can_id_input, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(bit_pos_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(bit_pos_dropdown, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(toggle_mode_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(toggle_mode_dropdown, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(animation_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(animation_switch, LV_OBJ_FLAG_HIDDEN);
		}
	}

	// Save button (green) - same position and style as panel config menu
	lv_obj_t *save_btn = lv_btn_create(config_screen);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE, LV_PART_MAIN);
	lv_obj_set_align(save_btn, LV_ALIGN_CENTER);
	lv_obj_set_pos(save_btn, 200, 200);
	lv_obj_center(save_label);

	// Cancel button (red) - same position and style as panel config menu
	lv_obj_t *back_btn = lv_btn_create(config_screen);
	lv_obj_t *back_label = lv_label_create(back_btn);
	lv_label_set_text(back_label, "Cancel");
	lv_obj_set_style_bg_color(back_btn, THEME_COLOR_BTN_CANCEL, LV_PART_MAIN);
	lv_obj_set_align(back_btn, LV_ALIGN_CENTER);
	lv_obj_set_pos(back_btn, 300, 200);
	lv_obj_center(back_label);

	// Save button event handler (pass vis so input_source can be read and saved
	// to NVS)
	lv_obj_add_event_cb(save_btn, save_indicator_config_cb, LV_EVENT_CLICKED,
						vis);

	// Back button event handler
	lv_obj_add_event_cb(back_btn, back_indicator_config_cb, LV_EVENT_CLICKED,
						NULL);

	// No need to store save data since settings save automatically

	// Yield to prevent blocking other tasks
	vTaskDelay(pdMS_TO_TICKS(1));

	// Load the config screen
	lv_scr_load(config_screen);
}

void update_indicator_ui_immediate(uint8_t indicator_idx) {
	if (indicator_idx >= 2)
		return;
	bool current_state = indicator_configs[indicator_idx].current_state;
	lv_obj_t *indicator_obj =
		(indicator_idx == 0) ? ui_Indicator_Left : ui_Indicator_Right;
	if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
		if (current_state) {
			if (indicator_configs[indicator_idx].animation_enabled) {
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
				indicator_animation_state = false;
				if (indicator_animation_timer) {
					lv_timer_resume(indicator_animation_timer);
				}
			} else {
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		} else {
			lv_obj_set_style_opa(indicator_obj, 50,
								 LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
	update_config_preview(indicator_idx);
}

// Function to update the config preview
void update_config_preview(uint8_t indicator_idx) {
	if (preview_indicator_config && lv_obj_is_valid(preview_indicator_config) &&
		preview_status_text_config &&
		lv_obj_is_valid(preview_status_text_config) &&
		preview_indicator_idx == indicator_idx) {

		printf("Updating config preview for indicator %d: %s\n", indicator_idx,
			   indicator_configs[indicator_idx].current_state ? "ACTIVE"
															  : "INACTIVE");

		// Update preview indicator opacity
		if (indicator_configs[indicator_idx].current_state) {
			lv_obj_set_style_opa(preview_indicator_config, 255,
								 LV_PART_MAIN |
									 LV_STATE_DEFAULT); // 100% opacity
		} else {
			lv_obj_set_style_opa(preview_indicator_config, 50,
								 LV_PART_MAIN |
									 LV_STATE_DEFAULT); // 50% opacity
		}

		// Update preview status text
		lv_label_set_text_fmt(preview_status_text_config, "%s INDICATOR\n%s",
							  indicator_idx == 0 ? "LEFT" : "RIGHT",
							  indicator_configs[indicator_idx].current_state
								  ? "ACTIVE"
								  : "INACTIVE");

		// Update text color based on state
		if (indicator_configs[indicator_idx].current_state) {
			lv_obj_set_style_text_color(preview_status_text_config,
										THEME_COLOR_GREEN,
										0); // Green when active
		} else {
			lv_obj_set_style_text_color(preview_status_text_config,
										THEME_COLOR_TEXT_MUTED,
										0); // Gray when inactive
		}
	}
}

/** Apply analog (wire) indicator state. Only updates indicators with
 * input_source == Wire (0). */
void indicator_apply_analog_state(bool left_on, bool right_on) {
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].input_source != 0)
			continue; /* CAN BUS - leave state to CAN */
		bool new_state = (i == 0) ? left_on : right_on;
		indicator_configs[i].current_state = new_state;
		previous_indicator_states[i] = new_state;
	}
	update_indicator_ui_immediate(0);
	update_indicator_ui_immediate(1);
}

// Asynchronous callback for updating an indicator
void update_indicator_ui(void *param) {
	uint8_t indicator_idx = *(uint8_t *)param;
	free(param);

	if (indicator_idx >= 2)
		return;

	// Check if state actually changed to avoid redundant updates
	bool current_state = indicator_configs[indicator_idx].current_state;
	if (previous_indicator_states[indicator_idx] == current_state) {
		return; // No change, skip update
	}

	// Update the previous state
	previous_indicator_states[indicator_idx] = current_state;

	// Update main indicator opacity based on state
	lv_obj_t *indicator_obj =
		(indicator_idx == 0) ? ui_Indicator_Left : ui_Indicator_Right;

	if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
		if (current_state) {
			printf("Indicator %d: Setting to ACTIVE\n", indicator_idx);
			if (indicator_configs[indicator_idx].animation_enabled) {
				printf("Indicator %d: Animation enabled - starting timer\n",
					   indicator_idx);
				// Always start with 100% opacity for immediate visibility
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
				// Set animation state to false so first timer tick will toggle
				// to true (100%)
				indicator_animation_state = false; // Next toggle will be bright
				if (indicator_animation_timer) {
					lv_timer_resume(indicator_animation_timer);
				}
			} else {
				printf("Indicator %d: Solid mode - 100%% opacity\n",
					   indicator_idx);
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		} else {
			printf("Indicator %d: Setting to INACTIVE (50%% opacity)\n",
				   indicator_idx);
			lv_obj_set_style_opa(indicator_obj, 50,
								 LV_PART_MAIN |
									 LV_STATE_DEFAULT); // 50% opacity (default)
		}
	} else {
		printf("Indicator %d: Object is null or invalid\n", indicator_idx);
	}

	// Also update the config preview if it's visible
	update_config_preview(indicator_idx);
}
void indicator_animation_timer_cb(lv_timer_t *timer) {
	// Toggle animation state (realistic car indicator timing)
	indicator_animation_state = !indicator_animation_state;

	bool any_indicator_animating = false;

	// Update both indicators if they're active and have animation enabled
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].current_state) {
			lv_obj_t *indicator_obj =
				(i == 0) ? ui_Indicator_Left : ui_Indicator_Right;
			if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
				if (indicator_configs[i].animation_enabled) {
					// Flash between 100% and 50% opacity like a real indicator
					uint8_t opacity =
						indicator_animation_state
							? 255
							: 128; // 100% or 50% (128 = 50% of 255)
					lv_obj_set_style_opa(indicator_obj, opacity,
										 LV_PART_MAIN | LV_STATE_DEFAULT);
					any_indicator_animating = true;
				} else {
					// Solid mode - always 100% opacity when active
					lv_obj_set_style_opa(indicator_obj, 255,
										 LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}
		}
	}

	// If no indicators are animating, pause the timer to save CPU
	if (!any_indicator_animating && indicator_animation_timer) {
		lv_timer_pause(indicator_animation_timer);
	}
}

void widget_indicator_create(lv_obj_t *parent) {
	ui_Indicator_Left = lv_img_create(parent);
	lv_img_set_src(ui_Indicator_Left, &ui_img_indicator_left_png);
	lv_obj_set_width(ui_Indicator_Left, LV_SIZE_CONTENT);
	lv_obj_set_height(ui_Indicator_Left, LV_SIZE_CONTENT);
	lv_obj_set_x(ui_Indicator_Left, -95);
	lv_obj_set_y(ui_Indicator_Left, -133);
	lv_obj_set_align(ui_Indicator_Left, LV_ALIGN_CENTER);
	lv_obj_add_flag(ui_Indicator_Left, LV_OBJ_FLAG_ADV_HITTEST);
	lv_obj_clear_flag(ui_Indicator_Left, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_opa(ui_Indicator_Left, 50,
						 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_width(ui_Indicator_Left, 0,
								   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Indicator_Right = lv_img_create(parent);
	lv_img_set_src(ui_Indicator_Right, &ui_img_indicator_right_png);
	lv_obj_set_width(ui_Indicator_Right, LV_SIZE_CONTENT);
	lv_obj_set_height(ui_Indicator_Right, LV_SIZE_CONTENT);
	lv_obj_set_x(ui_Indicator_Right, 95);
	lv_obj_set_y(ui_Indicator_Right, -133);
	lv_obj_set_align(ui_Indicator_Right, LV_ALIGN_CENTER);
	lv_obj_add_flag(ui_Indicator_Right, LV_OBJ_FLAG_ADV_HITTEST);
	lv_obj_clear_flag(ui_Indicator_Right, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_opa(ui_Indicator_Right, 50,
						 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_width(ui_Indicator_Right, 0,
								   LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *left_ta = lv_obj_create(parent);
	lv_obj_set_size(left_ta, 50, 50);
	lv_obj_set_x(left_ta, -95);
	lv_obj_set_y(left_ta, -133);
	lv_obj_set_align(left_ta, LV_ALIGN_CENTER);
	lv_obj_clear_flag(left_ta, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(left_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(left_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	uint8_t *left_id = lv_mem_alloc(sizeof(uint8_t));
	if (left_id) {
		*left_id = 0;
		lv_obj_add_event_cb(left_ta, indicator_longpress_cb,
							LV_EVENT_LONG_PRESSED, left_id);
	}

	lv_obj_t *right_ta = lv_obj_create(parent);
	lv_obj_set_size(right_ta, 50, 50);
	lv_obj_set_x(right_ta, 95);
	lv_obj_set_y(right_ta, -133);
	lv_obj_set_align(right_ta, LV_ALIGN_CENTER);
	lv_obj_clear_flag(right_ta, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(right_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(right_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	uint8_t *right_id = lv_mem_alloc(sizeof(uint8_t));
	if (right_id) {
		*right_id = 1;
		lv_obj_add_event_cb(right_ta, indicator_longpress_cb,
							LV_EVENT_LONG_PRESSED, right_id);
	}
}

void init_indicator_configs(void) {
	for (int i = 0; i < 2; i++) {
		indicator_configs[i].can_id = 0;
		indicator_configs[i].bit_position = 0;
		indicator_configs[i].input_source = 0; /* 0 = Wire, 1 = CAN */
		indicator_configs[i].is_momentary = true;
		indicator_configs[i].current_state = false;
		indicator_configs[i].animation_enabled = true;
	}
}

/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

static void _indicator_create(widget_t *w, lv_obj_t *parent) {
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	if (slot == 0) {
		widget_indicator_create(parent);
	}
	/* root points to the relevant image obj */
	w->root = (slot == 0) ? ui_Indicator_Left : ui_Indicator_Right;
}
static void _indicator_update(widget_t *w, void *data) {
	(void)w;
	update_indicator_ui(data);
}
static void _indicator_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	w->w = nw;
	w->h = nh;
}
static void _indicator_open_settings(widget_t *w) {
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	create_indicator_config_menu(slot);
}
static void _indicator_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	if (slot < 2) {
		cJSON *cfg = cJSON_AddObjectToObject(out, "config");
		cJSON_AddNumberToObject(cfg, "slot", slot);
		cJSON_AddNumberToObject(cfg, "input_source",
								indicator_configs[slot].input_source);
		cJSON_AddNumberToObject(cfg, "can_id", indicator_configs[slot].can_id);
		cJSON_AddBoolToObject(cfg, "animation",
							  indicator_configs[slot].animation_enabled);
	}
}
static void _indicator_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
}
static void _indicator_destroy(widget_t *w) { free(w); }

/* Default positions (relative to LV_ALIGN_CENTER): left x=-95, right x=+95,
 * y=-133 */
static const int16_t s_indicator_default_x[2] = {-95, 95};

widget_t *widget_indicator_create_instance(uint8_t slot) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	w->type = WIDGET_INDICATOR;
	w->x = s_indicator_default_x[slot & 1];
	w->y = -133;
	w->w = 50;
	w->h = 50;
	w->type_data = (void *)(uintptr_t)(slot & 1);
	snprintf(w->id, sizeof(w->id), "indicator_%u", slot & 1);

	w->create = _indicator_create;
	w->update = _indicator_update;
	w->resize = _indicator_resize;
	w->open_settings = _indicator_open_settings;
	w->to_json = _indicator_to_json;
	w->from_json = _indicator_from_json;
	w->destroy = _indicator_destroy;

	return w;
}
