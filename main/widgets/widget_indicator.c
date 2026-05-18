#include "widget_indicator.h"
#include "widget_rules.h"
#include "screen_config.h"
#include "can/can_decode.h"
#include "driver/twai.h"
#include "esp_heap_caps.h"
#include "signal.h"
#include "system/night_mode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "ui/callbacks/ui_callbacks.h"
#include "ui/dashboard.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widget_registry.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_indicator";

/* ── Helper: look up indicator_data_t by slot via registry ─────────────── */
static indicator_data_t *_lookup_indicator_data(uint8_t slot) {
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_INDICATOR, slot);
	return w ? (indicator_data_t *)w->type_data : NULL;
}

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
static lv_obj_t *s_ta_left = NULL;   /* touch area for left indicator */
static lv_obj_t *s_ta_right = NULL;  /* touch area for right indicator */
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
		ESP_LOGE(TAG, "No user data in indicator longpress callback");
		return;
	}

	uint8_t indicator_idx = *(uint8_t *)user_data;
	ESP_LOGD(TAG, "Indicator longpress detected for indicator %d", indicator_idx);
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
		indicator_data_t *id = _lookup_indicator_data(vis->indicator_idx);
		if (id) {
			id->input_source = new_src;
			/* When switching to CAN BUS, turn indicator off until CAN message sets
			 * it on */
			if (new_src == 1) {
				id->current_state = false;
				previous_indicator_states[vis->indicator_idx] = false;
			}
		}
		/* Sync main screen indicator opacity to current state before returning
		 */
		update_indicator_ui_immediate(0);
		update_indicator_ui_immediate(1);
	}
	ESP_LOGD(TAG, "Indicator configuration completed - returning to main screen");

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

	/* CAN ID is now managed via signals — this callback is a no-op placeholder */
	(void)indicator_idx;
	ESP_LOGD(TAG, "Indicator %d CAN ID field: 0x%X (managed via signal)", indicator_idx, can_id);
}

// Callback for bit position dropdown changes
static void indicator_bit_pos_changed_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!dropdown || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	uint8_t bit_pos = lv_dropdown_get_selected(dropdown);

	/* Bit position is now managed via signals — this callback is a no-op placeholder */
	(void)indicator_idx;
	ESP_LOGD(TAG, "Indicator %d bit position field: %d (managed via signal)", indicator_idx,
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

	indicator_data_t *id = _lookup_indicator_data(indicator_idx);
	if (id) id->is_momentary = is_momentary;

	ESP_LOGD(TAG, "Indicator %d toggle mode updated to: %s", indicator_idx,
		   is_momentary ? "Momentary" : "Toggle");
}

static void indicator_animation_changed_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!switch_obj || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	bool is_enabled = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	indicator_data_t *id = _lookup_indicator_data(indicator_idx);
	if (id) id->animation_enabled = is_enabled;

	ESP_LOGD(TAG, "Indicator %d animation updated to: %s", indicator_idx,
		   is_enabled ? "Enabled" : "Disabled");
}

void create_indicator_config_menu(uint8_t indicator_idx) {
	ESP_LOGD(TAG, "Creating indicator config menu for indicator %d", indicator_idx);

	// Validate indicator index
	if (indicator_idx >= 2) {
		ESP_LOGE(TAG, "Invalid indicator index %d", indicator_idx);
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
	indicator_data_t *id_cfg = _lookup_indicator_data(indicator_idx);
	bool cur_state = id_cfg ? id_cfg->current_state : false;
	if (cur_state) {
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
		cur_state ? "ACTIVE" : "INACTIVE");
	lv_obj_align(status_text, LV_ALIGN_CENTER, 0, 30);

	// Set text color based on state
	if (cur_state) {
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
	lv_obj_set_size(inputs_container, SCREEN_W, SCREEN_H);
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
		id_cfg ? id_cfg->input_source : 0);
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
		ESP_LOGD(TAG, "Added CAN ID callbacks for indicator %d", indicator_idx);
	}

	// Set placeholder and initial value
	char can_id_text[32];
	snprintf(can_id_text, sizeof(can_id_text), "0"); /* CAN ID now in signal */
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
		ESP_LOGD(TAG, "Added bit position callback for indicator %d", indicator_idx);
	}

	lv_dropdown_set_selected(bit_pos_dropdown, 0); /* bit pos now in signal */

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
		ESP_LOGD(TAG, "Added toggle mode callback for indicator %d", indicator_idx);
	}

	lv_dropdown_set_selected(
		toggle_mode_dropdown,
		(id_cfg && id_cfg->is_momentary) ? 0 : 1);

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
		ESP_LOGD(TAG, "Added animation callback for indicator %d", indicator_idx);
	}

	if (id_cfg && id_cfg->animation_enabled) {
		lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
	} else if (!id_cfg) {
		lv_obj_add_state(animation_switch,
						 LV_STATE_CHECKED); // Default to enabled
	} else {
		lv_obj_clear_state(animation_switch, LV_STATE_CHECKED);
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
	indicator_data_t *id = _lookup_indicator_data(indicator_idx);
	bool current_state = id ? id->current_state : false;
	lv_obj_t *indicator_obj =
		(indicator_idx == 0) ? ui_Indicator_Left : ui_Indicator_Right;
	if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
		bool night_active = night_mode_is_active();
		lv_color_t color = (id && current_state)
			? (id ? NIGHT_PICK_COLOR(night_active, id->night, color_on,  id->color_on)  : lv_color_hex(0xFFBF00))
			: (id ? NIGHT_PICK_COLOR(night_active, id->night, color_off, id->color_off) : lv_color_hex(0x333333));
		uint8_t opa = (id && current_state) ? id->opa_on : (id ? id->opa_off : 0);
		lv_obj_set_style_img_recolor(indicator_obj, color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_recolor_opa(indicator_obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_opa(indicator_obj, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		if (current_state && id && id->animation_enabled) {
			indicator_animation_state = false;
			if (indicator_animation_timer) {
				lv_timer_resume(indicator_animation_timer);
			}
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

		indicator_data_t *id = _lookup_indicator_data(indicator_idx);
		bool state = id ? id->current_state : false;

		ESP_LOGD(TAG, "Updating config preview for indicator %d: %s", indicator_idx,
			   state ? "ACTIVE" : "INACTIVE");

		// Update preview indicator color
		lv_color_t color = (id && state) ? id->color_on : (id ? id->color_off : lv_color_hex(0x333333));
		uint8_t opa = (id && state) ? id->opa_on : (id ? id->opa_off : 0);
		lv_obj_set_style_img_recolor(preview_indicator_config, color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_recolor_opa(preview_indicator_config, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_opa(preview_indicator_config, opa > 0 ? opa : 50, LV_PART_MAIN | LV_STATE_DEFAULT);

		// Update preview status text
		lv_label_set_text_fmt(preview_status_text_config, "%s INDICATOR\n%s",
							  indicator_idx == 0 ? "LEFT" : "RIGHT",
							  state ? "ACTIVE" : "INACTIVE");

		// Update text color based on state
		if (state) {
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
		indicator_data_t *id = _lookup_indicator_data(i);
		if (!id || id->input_source != 0)
			continue; /* CAN BUS or no widget - leave state to CAN */
		bool new_state = (i == 0) ? left_on : right_on;
		id->current_state = new_state;
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

	indicator_data_t *id = _lookup_indicator_data(indicator_idx);
	bool current_state = id ? id->current_state : false;

	// Check if state actually changed to avoid redundant updates
	if (previous_indicator_states[indicator_idx] == current_state) {
		return; // No change, skip update
	}

	// Update the previous state
	previous_indicator_states[indicator_idx] = current_state;

	// Update main indicator color based on state
	lv_obj_t *indicator_obj =
		(indicator_idx == 0) ? ui_Indicator_Left : ui_Indicator_Right;

	if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
		lv_color_t color = (id && current_state) ? id->color_on : (id ? id->color_off : lv_color_hex(0x333333));
		uint8_t opa = (id && current_state) ? id->opa_on : (id ? id->opa_off : 0);
		lv_obj_set_style_img_recolor(indicator_obj, color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_recolor_opa(indicator_obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_opa(indicator_obj, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		if (current_state && id && id->animation_enabled) {
			indicator_animation_state = false;
			if (indicator_animation_timer) {
				lv_timer_resume(indicator_animation_timer);
			}
		}
	} else {
		ESP_LOGW(TAG, "Indicator %d: Object is null or invalid", indicator_idx);
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
		indicator_data_t *id = _lookup_indicator_data(i);
		if (id && id->current_state) {
			lv_obj_t *indicator_obj =
				(i == 0) ? ui_Indicator_Left : ui_Indicator_Right;
			if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
				if (id->animation_enabled) {
					// Flash between full and half opacity using color-based rendering
					uint8_t flash_opa = indicator_animation_state
						? id->opa_on
						: (id->opa_on / 2);
					lv_obj_set_style_img_recolor(indicator_obj, id->color_on, LV_PART_MAIN | LV_STATE_DEFAULT);
					lv_obj_set_style_img_opa(indicator_obj, flash_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
					any_indicator_animating = true;
				} else {
					// Solid mode - full color when active
					lv_obj_set_style_img_recolor(indicator_obj, id->color_on, LV_PART_MAIN | LV_STATE_DEFAULT);
					lv_obj_set_style_img_opa(indicator_obj, id->opa_on, LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}
		}
	}

	// If no indicators are animating, pause the timer to save CPU
	if (!any_indicator_animating && indicator_animation_timer) {
		lv_timer_pause(indicator_animation_timer);
	}
}

void widget_indicator_reset(void) {
	ui_Indicator_Left = NULL;
	ui_Indicator_Right = NULL;
	s_ta_left = NULL;
	s_ta_right = NULL;
}

/* Helper: compute LVGL zoom (256 = 1x) to scale src image to target size */
static uint16_t _calc_zoom(uint16_t src_w, uint16_t src_h, uint16_t tgt_w, uint16_t tgt_h) {
	if (src_w == 0 || src_h == 0) return 256;
	uint16_t zx = (uint16_t)((uint32_t)tgt_w * 256 / src_w);
	uint16_t zy = (uint16_t)((uint32_t)tgt_h * 256 / src_h);
	return (zx < zy) ? zx : zy; /* fit inside, maintain aspect ratio */
}

void widget_indicator_create_one(lv_obj_t *parent, uint8_t i) {
	const lv_img_dsc_t *src = (i == 0) ? &ui_img_indicator_left_png : &ui_img_indicator_right_png;
	int16_t def_x = (i == 0) ? -95 : 95;
	lv_obj_t **obj_ptr = (i == 0) ? &ui_Indicator_Left : &ui_Indicator_Right;

	/* Look up widget for sizing and color data */
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_INDICATOR, i);
	indicator_data_t *id = w ? (indicator_data_t *)w->type_data : NULL;
	uint16_t tgt_w = w ? w->w : 50;
	uint16_t tgt_h = w ? w->h : 50;

	*obj_ptr = lv_img_create(parent);
	lv_img_set_src(*obj_ptr, src);
	lv_obj_set_width(*obj_ptr, LV_SIZE_CONTENT);
	lv_obj_set_height(*obj_ptr, LV_SIZE_CONTENT);
	lv_img_set_zoom(*obj_ptr, _calc_zoom(src->header.w, src->header.h, tgt_w, tgt_h));
	lv_obj_set_x(*obj_ptr, w ? w->x : def_x);
	lv_obj_set_y(*obj_ptr, w ? w->y : -133);
	lv_obj_set_align(*obj_ptr, LV_ALIGN_CENTER);
	lv_obj_add_flag(*obj_ptr, LV_OBJ_FLAG_ADV_HITTEST);
	lv_obj_clear_flag(*obj_ptr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_outline_width(*obj_ptr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Apply initial color-based state (off by default) */
	lv_color_t init_color = id ? id->color_off : lv_color_hex(0x333333);
	uint8_t init_opa = id ? id->opa_off : 0;
	lv_obj_set_style_img_recolor(*obj_ptr, init_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_img_recolor_opa(*obj_ptr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_img_opa(*obj_ptr, init_opa, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Transparent touch area — used as w->root for long-press detection.
	 * The image itself has ADV_HITTEST and may be invisible (opa_off=0),
	 * so we need a solid touch target on top. */
	lv_obj_t *ta = lv_obj_create(parent);
	lv_obj_set_size(ta, tgt_w, tgt_h);
	lv_obj_set_x(ta, w ? w->x : def_x);
	lv_obj_set_y(ta, w ? w->y : -133);
	lv_obj_set_align(ta, LV_ALIGN_CENTER);
	lv_obj_clear_flag(ta, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Store the touch area so _indicator_create can use it as w->root */
	if (i == 0) s_ta_left = ta;
	else        s_ta_right = ta;
}

void widget_indicator_create(lv_obj_t *parent) {
	widget_indicator_create_one(parent, 0);
	widget_indicator_create_one(parent, 1);
}


/* ── Phase 2: widget_t factory
 * ───────────────────────────────────────────── */

/* Forward declarations — used by _indicator_create / _indicator_destroy. */
static void _indicator_apply_night_mode(widget_t *w, bool active);
static void _indicator_night_cb(bool active, void *user_data);

static void _indicator_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	indicator_data_t *id = (indicator_data_t *)w->type_data;
	if (!id) return;
	uint8_t slot = id->slot;
	if (slot >= 2) return;
	bool on = !is_stale && (value != 0.0f);
	id->current_state = on;
	update_indicator_ui_immediate(slot);
}

static void _indicator_create(widget_t *w, lv_obj_t *parent) {
	indicator_data_t *id = (indicator_data_t *)w->type_data;
	uint8_t slot = id ? id->slot : 0;
	widget_indicator_create_one(parent, slot);
	/* root = the transparent touch area (always touchable, unlike the image
	 * which has ADV_HITTEST and may be invisible at opa_off=0) */
	w->root = (slot == 0) ? s_ta_left : s_ta_right;

	/* Subscribe to signal if bound */
	if (id && id->signal_index >= 0)
		signal_subscribe(id->signal_index, _indicator_on_signal, w);

	/* Subscribe to night-mode changes if any night override is set, and apply
	 * current state immediately so the widget renders correctly even if it
	 * was created while night-mode is already active. */
	if (id && (id->night.has_color_on || id->night.has_color_off)) {
		night_mode_subscribe(_indicator_night_cb, w);
		_indicator_apply_night_mode(w, night_mode_is_active());
	}
}
static void _indicator_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	w->w = nw;
	w->h = nh;
	if (w->root && lv_obj_is_valid(w->root)) {
		indicator_data_t *id = (indicator_data_t *)w->type_data;
		uint8_t slot = id ? id->slot : 0;
		const lv_img_dsc_t *src = (slot == 0) ? &ui_img_indicator_left_png : &ui_img_indicator_right_png;
		lv_img_set_zoom(w->root, _calc_zoom(src->header.w, src->header.h, nw, nh));
	}
}
static void _indicator_open_settings(widget_t *w) {
	/* Use the standard tabbed config modal (DATA + PRESETS + INDICATOR tabs) */
	load_menu_screen_for_widget(w);
}
static void _indicator_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	indicator_data_t *id = (indicator_data_t *)w->type_data;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	if (id) {
		cJSON_AddNumberToObject(cfg, "slot", id->slot);
		cJSON_AddNumberToObject(cfg, "input_source", id->input_source);
		cJSON_AddBoolToObject(cfg, "animation", id->animation_enabled);
		cJSON_AddBoolToObject(cfg, "is_momentary", id->is_momentary);
		if (id->signal_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "signal_name", id->signal_name);
		cJSON_AddNumberToObject(cfg, "color_on", (int)id->color_on.full);
		cJSON_AddNumberToObject(cfg, "opa_on", id->opa_on);
		cJSON_AddNumberToObject(cfg, "color_off", (int)id->color_off.full);
		cJSON_AddNumberToObject(cfg, "opa_off", id->opa_off);
		/* Night-mode overrides — emit only fields that have an override set */
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, id->night, color_on);
		NIGHT_SERIALIZE_COLOR(n, id->night, color_off);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
	}
}
static void _indicator_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
	indicator_data_t *id = (indicator_data_t *)w->type_data;
	if (!id) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;
	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	if (cJSON_IsNumber(item)) {
		id->slot = (uint8_t)item->valueint;
		w->slot = id->slot;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "input_source");
	if (cJSON_IsNumber(item)) id->input_source = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "animation");
	if (cJSON_IsBool(item)) id->animation_enabled = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "is_momentary");
	if (cJSON_IsBool(item)) id->is_momentary = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(id->signal_name, item->valuestring, sizeof(id->signal_name));
	}

	item = cJSON_GetObjectItemCaseSensitive(cfg, "color_on");
	if (cJSON_IsNumber(item)) id->color_on.full = (uint16_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "opa_on");
	if (cJSON_IsNumber(item)) id->opa_on = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "color_off");
	if (cJSON_IsNumber(item)) id->color_off.full = (uint16_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "opa_off");
	if (cJSON_IsNumber(item)) id->opa_off = (uint8_t)item->valueint;

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, id->night, color_on);
		NIGHT_PARSE_COLOR(night, id->night, color_off);
	}

	/* Resolve signal name → index */
	if (id->signal_name[0] != '\0')
		id->signal_index = signal_find_by_name(id->signal_name);
}
static void _indicator_destroy(widget_t *w) {
	indicator_data_t *id = (indicator_data_t *)w->type_data;
	if (id && id->signal_index >= 0)
		signal_unsubscribe(id->signal_index, _indicator_on_signal, w);
	night_mode_unsubscribe(_indicator_night_cb, w);
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	free(w->type_data);
	free(w);
}

/* Re-apply colors based on current night-mode state. The indicator image is
 * a separate global LVGL object (ui_Indicator_Left / ui_Indicator_Right) and
 * uses img_recolor rather than text/bg color. We only swap the recolor — opa
 * and current on/off state come from the runtime current_state flag and are
 * re-applied via update_indicator_ui_immediate(). */
static void _indicator_apply_night_mode(widget_t *w, bool active) {
	if (!w) return;
	indicator_data_t *id = (indicator_data_t *)w->type_data;
	if (!id) return;
	uint8_t slot = id->slot;
	if (slot >= 2) return;

	lv_obj_t *img_obj = (slot == 0) ? ui_Indicator_Left : ui_Indicator_Right;
	if (!img_obj || !lv_obj_is_valid(img_obj)) return;

	lv_color_t color = id->current_state
		? NIGHT_PICK_COLOR(active, id->night, color_on,  id->color_on)
		: NIGHT_PICK_COLOR(active, id->night, color_off, id->color_off);
	lv_obj_set_style_img_recolor(img_obj, color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_img_recolor_opa(img_obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _indicator_night_cb(bool active, void *user_data) {
	_indicator_apply_night_mode((widget_t *)user_data, active);
}

/* Default positions (relative to LV_ALIGN_CENTER): left x=-95, right x=+95,
 * y=-133 */
static const int16_t s_indicator_default_x[2] = {-95, 95};

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Schema names: schema/widgets.schema.json -> indicator fields. Live preview
 * routes through update_indicator_ui_immediate(slot) so the visible recolor
 * / opacity matches the runtime path exactly. `slot` is read-only here —
 * the widget IS its slot, so the schema entry exists to display the side
 * in the inspector header but writes are no-ops (changing slot would mean
 * re-instantiating). */

static bool _indicator_inspector_get(const widget_t *w, const char *name,
                                     widget_field_value_t *out) {
	if (!w || w->type != WIDGET_INDICATOR || !w->type_data || !name || !out) return false;
	const indicator_data_t *id = (const indicator_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0)  { out->str = id->signal_name;        return true; }
	if (strcmp(name, "slot") == 0)         { out->i = id->slot;                 return true; }
	if (strcmp(name, "input_source") == 0) { out->i = id->input_source;         return true; }
	if (strcmp(name, "animation") == 0)    { out->b = id->animation_enabled;    return true; }
	if (strcmp(name, "is_momentary") == 0) { out->b = id->is_momentary;         return true; }
	if (strcmp(name, "opa_on") == 0)       { out->i = id->opa_on;               return true; }
	if (strcmp(name, "opa_off") == 0)      { out->i = id->opa_off;              return true; }
	if (strcmp(name, "color_on") == 0)     { out->color = lv_color_to32(id->color_on)  & 0xFFFFFF; return true; }
	if (strcmp(name, "color_off") == 0)    { out->color = lv_color_to32(id->color_off) & 0xFFFFFF; return true; }
	return false;
}

static bool _indicator_inspector_set(widget_t *w, const char *name,
                                     const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_INDICATOR || !w->type_data || !name || !in) return false;
	indicator_data_t *id = (indicator_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0 && in->str) {
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;

		if (id->signal_index >= 0)
			signal_unsubscribe(id->signal_index, _indicator_on_signal, w);
		safe_strncpy(id->signal_name, in->str, sizeof(id->signal_name));
		id->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _indicator_on_signal, w);
		return true;
	}
	if (strcmp(name, "slot") == 0) {
		/* No-op — changing slot live would mean retargeting the global
		 * ui_Indicator_Left / Right pair. Persists for layout reload. */
		return true;
	}
	if (strcmp(name, "input_source") == 0) {
		id->input_source = (uint8_t)in->i;
		return true;
	}
	if (strcmp(name, "animation") == 0) {
		id->animation_enabled = in->b;
		return true;
	}
	if (strcmp(name, "is_momentary") == 0) {
		id->is_momentary = in->b;
		return true;
	}
	if (strcmp(name, "color_on") == 0) {
		id->color_on = lv_color_hex(in->color);
		update_indicator_ui_immediate(id->slot);
		return true;
	}
	if (strcmp(name, "color_off") == 0) {
		id->color_off = lv_color_hex(in->color);
		update_indicator_ui_immediate(id->slot);
		return true;
	}
	if (strcmp(name, "opa_on") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		id->opa_on = (uint8_t)v;
		update_indicator_ui_immediate(id->slot);
		return true;
	}
	if (strcmp(name, "opa_off") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		id->opa_off = (uint8_t)v;
		update_indicator_ui_immediate(id->slot);
		return true;
	}
	return false;
}

widget_t *widget_indicator_create_instance(uint8_t slot) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	indicator_data_t *id = heap_caps_calloc(1, sizeof(indicator_data_t), MALLOC_CAP_SPIRAM);
	if (!id) id = calloc(1, sizeof(indicator_data_t));
	if (!id) { free(w); return NULL; }

	uint8_t s = slot & 1;
	id->slot = s;
	id->input_source = 0;        /* default: Wire */
	id->animation_enabled = true;
	id->is_momentary = true;
	id->current_state = false;
	id->signal_index = -1;
	id->color_on = lv_color_hex(0xFFBF00);   /* amber */
	id->opa_on = 255;                         /* fully visible */
	id->color_off = lv_color_hex(0x333333);   /* dark grey */
	id->opa_off = 70;                         /* dimmed when off (visible) */

	w->type = WIDGET_INDICATOR;
	w->slot = s;
	w->x = s_indicator_default_x[s];
	w->y = -133;
	w->w = 50;
	w->h = 50;
	w->type_data = id;
	snprintf(w->id, sizeof(w->id), "indicator_%u", s);

	w->create = _indicator_create;
	w->resize = _indicator_resize;
	w->open_settings = _indicator_open_settings;
	w->to_json = _indicator_to_json;
	w->from_json = _indicator_from_json;
	w->destroy = _indicator_destroy;
	w->apply_night_mode = _indicator_apply_night_mode;
	w->inspector_get = _indicator_inspector_get;
	w->inspector_set = _indicator_inspector_set;

	return w;
}

uint8_t widget_indicator_get_slot(const widget_t *w) {
	if (!w || w->type != WIDGET_INDICATOR || !w->type_data) return 0;
	return ((const indicator_data_t *)w->type_data)->slot;
}

bool widget_indicator_has_signal(const widget_t *w) {
	if (!w || w->type != WIDGET_INDICATOR || !w->type_data) return false;
	return ((const indicator_data_t *)w->type_data)->signal_index >= 0;
}
