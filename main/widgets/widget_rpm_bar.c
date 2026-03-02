#include "widget_rpm_bar.h"
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
#include "widget_bar.h"
#include "widget_dispatcher.h"
#include "widget_panel.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t last_rpm_can_received = 0;

/* Forward declarations */
void stop_limiter_effect_demo(void);
static void stop_real_limiter_effect(void);
static void update_rpm_lights(int rpm_value);
static void clear_rpm_lights_circles(void);
static void batch_update_rpm_circles_color(lv_color_t color);
void update_rpm_ui_immediate(const char *rpm_str, int rpm_value);

/* Missing static state variables */
static bool real_limiter_active = false;
static int last_rpm_for_limiter_check = 0;
static bool limiter_active = false;

/* menu_rpm_value_label is owned by menu_screen.c */
extern lv_obj_t *menu_rpm_value_label;

static lv_obj_t *rpm_lights_circles[8] = {
	NULL}; // Separate circles for RPM Lights
static lv_timer_t *limiter_demo_timer = NULL;
static lv_timer_t *limiter_flash_timer = NULL;
static bool limiter_demo_active = false;
static bool limiter_flash_state = false;
static lv_color_t original_rpm_color;
static uint8_t current_effect_type = 0;

static int current_canbus_rpm = 0;	  // Store the current CAN bus RPM value
static int saved_rpm_before_demo = 0; // Save RPM value before demo starts

// CAN timeout tracking
static bool rpm_color_needs_update = false;
static lv_color_t new_rpm_color;
void rpm_gauge_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);
	rpm_gauge_max = 3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

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

void rpm_redline_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);
	rpm_redline_value =
		3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Stop any active limiter demo before UI changes
	stop_limiter_effect_demo();

	update_redline_position();
}

void rpm_ecu_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	// Indices: 0 = Custom, 1 = MaxxECU, 2 = Haltech

	if (selected == 0) {
		// ========== CUSTOM ==========
		printf("ECU Presets: Custom (no changes)\n");
		// Do nothing, or set defaults if you prefer
	} else if (selected == 1) {
		// ========== MAXXECU ==========
		printf("ECU Presets: MaxxECU\n");
		values_config[RPM_VALUE_ID - 1].can_id = 520; // 0x208
		values_config[RPM_VALUE_ID - 1].endianess =
			1; // 0=Big,1=Little (check your usage)
		values_config[RPM_VALUE_ID - 1].bit_start = 0;
		values_config[RPM_VALUE_ID - 1].bit_length = 16;
		values_config[RPM_VALUE_ID - 1].scale = 1.0f;
		values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
		values_config[RPM_VALUE_ID - 1].decimals = 0;

		// Now update the UI fields for RPM (ID=9 => index=8)
		// --------------------------------------------------
		if (g_can_id_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[RPM_VALUE_ID - 1].can_id);
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

		// Bit length (the dropdown might list 1..64, so we subtract 1 for
		// zero-based index)
		if (g_bit_length_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[RPM_VALUE_ID - 1],
				values_config[RPM_VALUE_ID - 1].bit_length - 1);
		}

		// Scale
		if (g_scale_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
		}

		// Value offset
		if (g_offset_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
		}

		// Decimals
		if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].decimals);
		}
	} else if (selected == 2) {
		// ========== HALTECH ==========
		printf("ECU Presets: Haltech\n");
		values_config[RPM_VALUE_ID - 1].can_id = 360; // 0x209
		values_config[RPM_VALUE_ID - 1].endianess = 0;
		values_config[RPM_VALUE_ID - 1].bit_start = 0;
		values_config[RPM_VALUE_ID - 1].bit_length = 16;
		values_config[RPM_VALUE_ID - 1].scale = 1.0f;
		values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
		values_config[RPM_VALUE_ID - 1].decimals = 0;

		// Update UI fields similarly
		// --------------------------------------------------
		if (g_can_id_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[RPM_VALUE_ID - 1].can_id);
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
			lv_dropdown_set_selected(
				g_bit_length_dropdown[RPM_VALUE_ID - 1],
				values_config[RPM_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_offset_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].decimals);
		}
	} else if (selected == 3) {
		// ========== FORD BA/BF/FG ==========
		printf("ECU Presets: Ford BA/BF/FG\n");
		values_config[RPM_VALUE_ID - 1].can_id = 0x3E8; // 1000 decimal
		values_config[RPM_VALUE_ID - 1].endianess = 1;	// Little Endian
		values_config[RPM_VALUE_ID - 1].bit_start = 0;
		values_config[RPM_VALUE_ID - 1].bit_length = 16;
		values_config[RPM_VALUE_ID - 1].scale = 0.25f;
		values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
		values_config[RPM_VALUE_ID - 1].decimals = 2;

		// Update UI fields
		// --------------------------------------------------
		if (g_can_id_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%X",
					 values_config[RPM_VALUE_ID - 1].can_id);
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
			lv_dropdown_set_selected(
				g_bit_length_dropdown[RPM_VALUE_ID - 1],
				values_config[RPM_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_offset_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].decimals);
		}
	}
}

void rpm_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	// Determine new color based on selection - SUPER VIBRANT COLORS
	switch (selected) {
	case 0:
		new_rpm_color = THEME_COLOR_GREEN;
		break; // Bright Green
	case 1:
		new_rpm_color = THEME_COLOR_CYAN;
		break; // Bright Cyan
	case 2:
		new_rpm_color = THEME_COLOR_YELLOW;
		break; // Bright Yellow
	case 3:
		new_rpm_color = THEME_COLOR_ORANGE;
		break; // Bright Orange
	case 4:
		new_rpm_color = THEME_COLOR_RED;
		break; // Bright Red
	case 5:
		new_rpm_color = THEME_COLOR_BLUE;
		break; // Bright Blue
	case 6:
		new_rpm_color = THEME_COLOR_PURPLE;
		break; // Bright Purple
	case 7:
		new_rpm_color = THEME_COLOR_MAGENTA;
		break; // Bright Magenta
	case 8:
		new_rpm_color = THEME_COLOR_PINK;
		break; // Bright Hot Pink
	case 9:	   // Custom color - open color wheel popup
		create_rpm_color_wheel_popup();
		return; // Don't update color yet, wait for color wheel selection
	default:
		new_rpm_color = THEME_COLOR_GREEN;
		break;
	}

	// Don't update colors when real limiter effect is active to avoid conflicts
	// with flashing
	if (!real_limiter_active) {
		rpm_color_needs_update = true;
	}
	values_config[RPM_VALUE_ID - 1].rpm_bar_color = new_rpm_color;
}

void check_rpm_color_update(lv_timer_t *timer) {
	// Don't update colors when real limiter effect is active to avoid conflicts
	// with flashing
	if (rpm_color_needs_update && !real_limiter_active) {
		if (rpm_bar_gauge) {
			lv_obj_set_style_bg_color(rpm_bar_gauge, new_rpm_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			// Set gradient color to same as main color for solid appearance
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, new_rpm_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9) {
			lv_obj_set_style_bg_color(ui_Panel9, new_rpm_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		rpm_color_needs_update = false;
	}
}

// RPM Limiter Effect event callbacks
void rpm_limiter_effect_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	// Map dropdown selection to effect type (0=None, 2=Bar Flash, 3=Bar &
	// Circles Flash, 4=Circles Flash, 5=Bar Solid, 6=Bar & Circles Solid,
	// 7=Circles Solid)
	uint8_t effect_type = 0;
	if (selected == 1) {
		effect_type = 2; // Bar Flash only
	} else if (selected == 2) {
		effect_type = 3; // Bar & Circles Flash (combined effect)
	} else if (selected == 3) {
		effect_type = 4; // Circles Flash only
	} else if (selected == 4) {
		effect_type = 5; // Bar Solid only
	} else if (selected == 5) {
		effect_type = 6; // Bar & Circles Solid
	} else if (selected == 6) {
		effect_type = 7; // Circles Solid only
	}

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_limiter_effect = effect_type;

	// Demo the selected effect for 1 second
	start_limiter_effect_demo(effect_type);
}

void rpm_limiter_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);

	int32_t rpm_value =
		3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_limiter_value = rpm_value;
}

void rpm_limiter_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0: // Green
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_GREEN;
		break;
	case 1: // Light Blue
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_CYAN;
		break;
	case 2: // Yellow
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_YELLOW;
		break;
	case 3: // Orange
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_ORANGE;
		break;
	case 4: // Red
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_RED;
		break;
	case 5: // Dark Blue
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_BLUE;
		break;
	case 6: // Purple
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_PURPLE;
		break;
	case 7: // Magenta
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_MAGENTA;
		break;
	case 8: // Pink
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color = THEME_COLOR_PINK;
		break;
	case 9: // Custom
		create_limiter_color_wheel_popup();
		break;
	}

	// Limiter circles color update removed - only bar flash effect is supported
}
void rpm_lights_switch_event_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	bool is_checked = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_lights_enabled = is_checked;

	// If disabled, hide all RPM lights circles
	if (!is_checked) {
		for (int i = 0; i < 8; i++) {
			if (rpm_lights_circles[i] &&
				lv_obj_is_valid(rpm_lights_circles[i])) {
				lv_obj_add_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN);
			}
		}
	} else {
		// If enabled, create RPM lights circles if they don't exist
		if (rpm_lights_circles[0] == NULL) {
			lv_obj_t *current_screen = lv_scr_act();
			create_rpm_lights_circles(current_screen);
		}

		// Update RPM lights based on current RPM value
		update_rpm_lights(current_canbus_rpm);
	}
}

void rpm_background_switch_event_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	bool is_checked = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_background_enabled = is_checked;
}

void rpm_background_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	// Determine new color based on selection - same colors as limiter
	lv_color_t new_background_color;
	switch (selected) {
	case 0:
		new_background_color = THEME_COLOR_GREEN;
		break; // Green
	case 1:
		new_background_color = THEME_COLOR_CYAN;
		break; // Light Blue
	case 2:
		new_background_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 3:
		new_background_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 4:
		new_background_color = THEME_COLOR_RED;
		break; // Red
	case 5:
		new_background_color = THEME_COLOR_BLUE;
		break; // Blue
	case 6:
		new_background_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 7:
		new_background_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:
		new_background_color = THEME_COLOR_PINK;
		break; // Pink
	case 9:	   // Custom color - open color wheel popup
		create_rpm_background_color_wheel_popup();
		return; // Don't update color yet, wait for color wheel selection
	default:
		new_background_color = THEME_COLOR_GREEN;
		break;
	}

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_background_color = new_background_color;
}

void rpm_background_threshold_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);

	int32_t threshold_value =
		3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_background_value = threshold_value;
}

static void update_rpm_lights(int rpm_value) {
	// Only update if RPM lights are enabled and circles exist and not in
	// limiter demo mode and no real limiter effect is active
	if (!values_config[RPM_VALUE_ID - 1].rpm_lights_enabled ||
		rpm_lights_circles[0] == NULL || limiter_demo_active ||
		real_limiter_active) {
		return;
	}

	extern int rpm_gauge_max;
	if (rpm_gauge_max <= 0)
		return; // Avoid division by zero

	// Calculate which zone we're in (0-4)
	float rpm_percentage = (float)rpm_value / (float)rpm_gauge_max;
	if (rpm_percentage < 0)
		rpm_percentage = 0;
	if (rpm_percentage > 1)
		rpm_percentage = 1;

	int zone = (int)(rpm_percentage * 5);
	if (zone > 4)
		zone = 4;

	// Circle order: [7,0] [6,1] [5,2] [4,3] (outermost to innermost pairs)
	int circle_pairs[4][2] = {
		{7, 0}, // Outermost pair
		{6, 1}, // Second pair
		{5, 2}, // Third pair
		{4, 3}	// Innermost pair
	};

	// All lights use the RPM bar color
	lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;

	// Update all circles
	for (int pair = 0; pair < 4; pair++) {
		bool should_show = (zone > pair); // Show if we've passed this zone
		lv_color_t color = rpm_color;	  // All lights use RPM color

		for (int j = 0; j < 2; j++) {
			int circle_idx = circle_pairs[pair][j];
			if (rpm_lights_circles[circle_idx] &&
				lv_obj_is_valid(rpm_lights_circles[circle_idx])) {
				if (should_show) {
					lv_obj_set_style_bg_color(rpm_lights_circles[circle_idx],
											  color,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
					lv_obj_clear_flag(rpm_lights_circles[circle_idx],
									  LV_OBJ_FLAG_HIDDEN);
				} else {
					lv_obj_add_flag(rpm_lights_circles[circle_idx],
									LV_OBJ_FLAG_HIDDEN);
				}
			}
		}
	}
}

void create_rpm_lights_circles(lv_obj_t *parent) {
	// Use the same positions as warning circles but these are for RPM Lights
	// (background layer)
	const struct {
		int16_t x;
		int16_t y;
	} rpm_lights_positions[] = {
		{-352, -148}, // Position 1
		{-292, -148}, // Position 2
		{-232, -148}, // Position 3
		{-172, -148}, // Position 4
		{172, -148},  // Position 5
		{232, -148},  // Position 6
		{292, -148},  // Position 7
		{352, -148}	  // Position 8
	};

	for (int i = 0; i < 8; i++) {
		rpm_lights_circles[i] = lv_obj_create(parent);
		lv_obj_set_width(rpm_lights_circles[i], 15);
		lv_obj_set_height(rpm_lights_circles[i], 15);
		lv_obj_set_x(rpm_lights_circles[i], rpm_lights_positions[i].x);
		lv_obj_set_y(rpm_lights_circles[i], rpm_lights_positions[i].y);
		lv_obj_set_align(rpm_lights_circles[i], LV_ALIGN_CENTER);
		lv_obj_clear_flag(rpm_lights_circles[i], LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_radius(rpm_lights_circles[i], 100,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(rpm_lights_circles[i],
								  values_config[RPM_VALUE_ID - 1].rpm_bar_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(rpm_lights_circles[i], 255,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(rpm_lights_circles[i], 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		// No outline for rpm circles
		lv_obj_set_style_outline_width(rpm_lights_circles[i], 0,
									   LV_PART_MAIN | LV_STATE_DEFAULT);

		// Initially hidden
		lv_obj_add_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN);
	}
}

// Limiter circles creation function removed - only bar flash effect is
// supported

static void limiter_demo_timeout_cb(lv_timer_t *timer) {
	stop_limiter_effect_demo();
}

static void limiter_flash_cb(lv_timer_t *timer) {
	// Safety check: if demo is no longer active, stop the timer
	if (!limiter_demo_active) {
		return;
	}

	limiter_flash_state = !limiter_flash_state;

	// Handle flash effects (type 2 = Bar only, type 3 = Bar & Circles, type 4 =
	// Circles only)
	if (current_effect_type == 2 || current_effect_type == 3 ||
		current_effect_type == 4) { // Flash effects
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			// Keep RPM bar at max value during demo, just like real limiter
			// effect
			extern int rpm_gauge_max;
			// Map RPM to extended bar range to properly fill the extended bar
			// width
			const float bar_extension_ratio = 782.5f / 765.0f;
			int32_t extended_rpm_max =
				(int32_t)(rpm_gauge_max * bar_extension_ratio);
			int32_t scaled_rpm =
				(rpm_gauge_max * extended_rpm_max) / rpm_gauge_max;
			lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);

			if (limiter_flash_state) {
				// Flash RPM bar and panel for Bar Flash (type 2) and Bar &
				// Circles Flash (type 3)
				if (current_effect_type == 2 || current_effect_type == 3) {
					// Flash RPM bar to limiter color
					lv_obj_set_style_bg_color(
						rpm_bar_gauge,
						values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					// Set gradient color to same as limiter color for solid
					// appearance
					lv_obj_set_style_bg_grad_color(
						rpm_bar_gauge,
						values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					lv_obj_set_style_bg_grad_dir(
						rpm_bar_gauge, LV_GRAD_DIR_NONE,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					// Flash Panel 9 (left side) to limiter color
					if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
						lv_obj_set_style_bg_color(
							ui_Panel9,
							values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
							LV_PART_MAIN | LV_STATE_DEFAULT);
					}
				}

				// Flash circles for Bar & Circles Flash (type 3) and Circles
				// Flash (type 4)
				if (current_effect_type == 3 || current_effect_type == 4) {
					// Ultra-fast batch update for perfect circle
					// synchronization
					batch_update_rpm_circles_color(
						values_config[RPM_VALUE_ID - 1].rpm_limiter_color);
				}
			} else {
				// Restore RPM bar and panel for Bar Flash (type 2) and Bar &
				// Circles Flash (type 3)
				if (current_effect_type == 2 || current_effect_type == 3) {
					// Restore RPM bar to original color (but keep at max value)
					lv_obj_set_style_bg_color(rpm_bar_gauge, original_rpm_color,
											  LV_PART_INDICATOR |
												  LV_STATE_DEFAULT);
					// Set gradient color to same as original color for solid
					// appearance
					lv_obj_set_style_bg_grad_color(
						rpm_bar_gauge, original_rpm_color,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					lv_obj_set_style_bg_grad_dir(
						rpm_bar_gauge, LV_GRAD_DIR_NONE,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					// Restore Panel 9 to original color
					if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
						lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color,
												  LV_PART_MAIN |
													  LV_STATE_DEFAULT);
					}
				}

				// Restore circles for Bar & Circles Flash (type 3) and Circles
				// Flash (type 4)
				if (current_effect_type == 3 || current_effect_type == 4) {
					// Ultra-fast batch restore for perfect circle
					// synchronization
					batch_update_rpm_circles_color(original_rpm_color);
				}
			}
		}
	}
}

static void clear_rpm_lights_circles(void) {
	// Clear the RPM lights circles array when RPM gauge is recreated
	for (int i = 0; i < 8; i++) {
		rpm_lights_circles[i] = NULL;
	}
}

// Ultra-fast batch update function for perfect circle synchronization
static void batch_update_rpm_circles_color(lv_color_t color) {
	if (!values_config[RPM_VALUE_ID - 1].rpm_lights_enabled ||
		!rpm_lights_circles[0]) {
		return; // Early exit if RPM lights are disabled or not initialized
	}

	// Single function call to update all circles with the same color for
	// perfect timing
	for (int i = 0; i < 8; i++) {
		if (rpm_lights_circles[i] && lv_obj_is_valid(rpm_lights_circles[i]) &&
			!lv_obj_has_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN)) {
			lv_obj_set_style_bg_color(rpm_lights_circles[i], color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
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

	if (effect_type == 0)
		return; // None selected

	// Save current RPM value before starting demo
	saved_rpm_before_demo = current_canbus_rpm;

	limiter_demo_active = true;
	limiter_flash_state = false;
	current_effect_type = effect_type;

	// Save original RPM color for effects
	original_rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;

	// Immediately set panels to RPM color to avoid initial white flash
	if (ui_Panel9) {
		lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	// Panel 10 stays white always - removed from demo effects

	// Set RPM to max for demo
	if (rpm_bar_gauge) {
		extern int rpm_gauge_max;
		// Map RPM to extended bar range to properly fill the extended bar width
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);
		int32_t scaled_rpm = (rpm_gauge_max * extended_rpm_max) / rpm_gauge_max;
		lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
		// Update menu RPM value text to show max RPM
		update_menu_rpm_value_text(rpm_gauge_max);
	}

	// Handle different effect types
	if (effect_type == 5) {
		// Bar Solid effect - just set the limiter color immediately, no
		// flashing
		if (rpm_bar_gauge) {
			lv_obj_set_style_bg_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9) {
			lv_obj_set_style_bg_color(
				ui_Panel9, values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Only create the demo timeout timer for solid effect (no flash timer
		// needed)
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	} else if (effect_type == 6) {
		// Bar & Circles Solid effect - set both bar and circles to limiter
		// color immediately, no flashing
		if (rpm_bar_gauge) {
			lv_obj_set_style_bg_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9) {
			lv_obj_set_style_bg_color(
				ui_Panel9, values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Set circles to limiter color
		batch_update_rpm_circles_color(
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color);
		// Only create the demo timeout timer for solid effect (no flash timer
		// needed)
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	} else if (effect_type == 7) {
		// Circles Solid effect - only set circles to limiter color immediately,
		// no flashing, bar stays at original color Set circles to limiter color
		batch_update_rpm_circles_color(
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color);
		// Only create the demo timeout timer for solid effect (no flash timer
		// needed)
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	} else {
		// Flash effects supported (type 2 = Bar only, type 3 = Bar & Circles,
		// type 4 = Circles only)

		// Create LVGL timers for perfect synchronization with real limiter
		limiter_flash_timer =
			lv_timer_create(limiter_flash_cb, 100,
							NULL); // 100ms flash rate - same as real limiter
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	}
}

void stop_limiter_effect_demo(void) {
	if (!limiter_demo_active)
		return;

	limiter_demo_active = false;
	limiter_flash_state = false; // Reset flash state

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
		lv_obj_set_style_bg_color(rpm_bar_gauge, original_rpm_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		// Set gradient color to same as original color for solid appearance
		lv_obj_set_style_bg_grad_color(rpm_bar_gauge, original_rpm_color,
									   LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
									 LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}
	// Restore Panel 9 to original RPM color
	if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
		lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Restore RPM value to saved value (either CAN bus value or 0)
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		// Map RPM to extended bar range to properly fill the extended bar width
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);
		int32_t scaled_rpm =
			(saved_rpm_before_demo * extended_rpm_max) / rpm_gauge_max;
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
static lv_timer_t *real_limiter_flash_timer = NULL;
static bool real_limiter_flash_state = false;
static uint8_t real_limiter_effect_type = 0;

static void real_limiter_flash_cb(lv_timer_t *timer) {
	// Toggle flash state
	real_limiter_flash_state = !real_limiter_flash_state;

	// Get current colors
	lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
	lv_color_t limiter_color =
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

	// Handle flash effects (type 2 = Bar only, type 3 = Bar & Circles, type 4 =
	// Circles only)

	// Flash RPM bar and panel for Bar Flash (type 2) and Bar & Circles Flash
	// (type 3)
	if (real_limiter_effect_type == 2 || real_limiter_effect_type == 3) {
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_color_t current_color =
				real_limiter_flash_state ? limiter_color : rpm_color;
			lv_obj_set_style_bg_color(rpm_bar_gauge, current_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			// Set gradient color to same as current color for solid appearance
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, current_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
			lv_obj_set_style_bg_color(
				ui_Panel9, real_limiter_flash_state ? limiter_color : rpm_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}

	// Flash circles for Bar & Circles Flash (type 3) and Circles Flash (type 4)
	if (real_limiter_effect_type == 3 || real_limiter_effect_type == 4) {
		// Ultra-fast batch update for perfect circle synchronization
		lv_color_t circle_color =
			real_limiter_flash_state ? limiter_color : rpm_color;
		batch_update_rpm_circles_color(circle_color);
	}
}

static void start_real_limiter_effect(uint8_t effect_type) {
	// Don't start if already active or no effect selected
	if (real_limiter_active || effect_type == 0)
		return;

	// Additional check: if timer already exists, we're already running
	if (real_limiter_flash_timer != NULL)
		return;

	real_limiter_active = true;
	real_limiter_flash_state = false;
	real_limiter_effect_type = effect_type;

	if (effect_type == 5) {
		// Bar Solid effect - just set the limiter color immediately, no
		// flashing timer needed
		lv_color_t limiter_color =
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_style_bg_color(rpm_bar_gauge, limiter_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, limiter_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
			lv_obj_set_style_bg_color(ui_Panel9, limiter_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// No timer needed for solid effect
	} else if (effect_type == 6) {
		// Bar & Circles Solid effect - set both bar and circles to limiter
		// color immediately, no flashing timer needed
		lv_color_t limiter_color =
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_style_bg_color(rpm_bar_gauge, limiter_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, limiter_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
			lv_obj_set_style_bg_color(ui_Panel9, limiter_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Set circles to limiter color
		batch_update_rpm_circles_color(limiter_color);
		// No timer needed for solid effect
	} else if (effect_type == 7) {
		// Circles Solid effect - only set circles to limiter color immediately,
		// no flashing timer needed, bar stays at normal color
		lv_color_t limiter_color =
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

		// Set circles to limiter color
		batch_update_rpm_circles_color(limiter_color);
		// No timer needed for solid effect
	} else {
		// Flash effects supported (type 2 = Bar only, type 3 = Bar & Circles,
		// type 4 = Circles only)

		// Create LVGL timer instead of ESP timer for better coordination
		real_limiter_flash_timer = lv_timer_create(real_limiter_flash_cb, 100,
												   NULL); // 100ms flash rate
	}
}

static void stop_real_limiter_effect(void) {
	if (!real_limiter_active)
		return;

	real_limiter_active = false;

	// Stop and delete LVGL timer
	if (real_limiter_flash_timer) {
		lv_timer_del(real_limiter_flash_timer);
		real_limiter_flash_timer = NULL;
	}

	// Restore original colors and RPM bar value
	lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;

	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		lv_obj_set_style_bg_color(rpm_bar_gauge, rpm_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		// Set gradient color to same as RPM color for solid appearance
		lv_obj_set_style_bg_grad_color(rpm_bar_gauge, rpm_color,
									   LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
									 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		// Restore the bar to show the current actual CAN bus RPM value
		// Map RPM to extended bar range to properly fill the extended bar width
		extern int rpm_gauge_max;
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);
		int32_t scaled_rpm =
			(current_canbus_rpm * extended_rpm_max) / rpm_gauge_max;
		lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
	}
	if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
		lv_obj_set_style_bg_color(ui_Panel9, rpm_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Restore circles to original color for effects that involve circles (types
	// 3, 4, 6, 7)
	if (real_limiter_effect_type == 3 || real_limiter_effect_type == 4 ||
		real_limiter_effect_type == 6 || real_limiter_effect_type == 7) {
		batch_update_rpm_circles_color(rpm_color);
		// Update RPM lights to properly restore the state based on current RPM
		// value This ensures circles show the correct state rather than all
		// being lit solid
		update_rpm_lights(current_canbus_rpm);
	}
}

// Global variables for color wheel popup
static lv_obj_t *color_wheel_popup = NULL;
static lv_obj_t *color_wheel = NULL;
static lv_color_t selected_custom_color;

// Global variables for RPM background color wheel popup
static lv_obj_t *rpm_background_color_wheel_popup = NULL;
static lv_obj_t *rpm_background_color_wheel = NULL;
static lv_color_t selected_rpm_background_custom_color;

// Global variables for RPM background functionality
static bool rpm_background_active = false;
static lv_color_t original_screen_bg_color;
static bool original_screen_bg_color_saved = false;

// Color wheel popup event callbacks
static void color_wheel_ok_event_cb(lv_event_t *e) {
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

static void color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (color_wheel_popup) {
		lv_obj_del(color_wheel_popup);
		color_wheel_popup = NULL;
		color_wheel = NULL;
	}
}

static void color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_custom_color = lv_colorwheel_get_rgb(colorwheel);

	// Show live preview by updating the RPM bar immediately
	// But don't update if real limiter effect is active to avoid conflicts
	if (!real_limiter_active) {
		new_rpm_color = selected_custom_color;
		rpm_color_needs_update = true;
	}
}

// RPM Background color wheel popup event callbacks
static void rpm_background_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[RPM_VALUE_ID - 1].rpm_background_color =
		selected_rpm_background_custom_color;

	// Close the popup
	if (rpm_background_color_wheel_popup) {
		lv_obj_del(rpm_background_color_wheel_popup);
		rpm_background_color_wheel_popup = NULL;
		rpm_background_color_wheel = NULL;
	}
}

static void rpm_background_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (rpm_background_color_wheel_popup) {
		lv_obj_del(rpm_background_color_wheel_popup);
		rpm_background_color_wheel_popup = NULL;
		rpm_background_color_wheel = NULL;
	}
}

static void rpm_background_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_rpm_background_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

void create_rpm_background_color_wheel_popup(void) {
	// Don't create multiple popups
	if (rpm_background_color_wheel_popup)
		return;

	// Create popup background
	rpm_background_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(rpm_background_color_wheel_popup, 400, 350);
	lv_obj_center(rpm_background_color_wheel_popup);
	lv_obj_set_style_bg_color(rpm_background_color_wheel_popup,
							  THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(rpm_background_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(rpm_background_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(rpm_background_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(rpm_background_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(rpm_background_color_wheel_popup,
								  THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(rpm_background_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(rpm_background_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Background Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	rpm_background_color_wheel =
		lv_colorwheel_create(rpm_background_color_wheel_popup, true);
	lv_obj_set_size(rpm_background_color_wheel, 200, 200);
	lv_obj_align(rpm_background_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current background color
	lv_color_t current_color =
		values_config[RPM_VALUE_ID - 1].rpm_background_color;
	lv_colorwheel_set_rgb(rpm_background_color_wheel, current_color);
	selected_rpm_background_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(rpm_background_color_wheel,
						rpm_background_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(rpm_background_color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);
	lv_obj_add_event_cb(ok_btn, rpm_background_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(rpm_background_color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);
	lv_obj_add_event_cb(cancel_btn, rpm_background_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void create_rpm_color_wheel_popup(void) {
	// Don't create multiple popups
	if (color_wheel_popup)
		return;

	// Create popup background
	color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(color_wheel_popup, 400, 350);
	lv_obj_center(color_wheel_popup);
	lv_obj_set_style_bg_color(color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(color_wheel_popup, THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(color_wheel_popup, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom RPM Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
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
	lv_obj_add_event_cb(color_wheel, color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);

	lv_obj_add_event_cb(ok_btn, color_wheel_ok_event_cb, LV_EVENT_CLICKED,
						NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);

	lv_obj_add_event_cb(cancel_btn, color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

// Global variables for limiter color wheel popup
static lv_obj_t *limiter_color_wheel_popup = NULL;
static lv_obj_t *limiter_color_wheel = NULL;
static lv_color_t selected_limiter_custom_color;

// Global variables for bar color wheel popups
static lv_obj_t *bar_low_color_wheel_popup = NULL;
static lv_obj_t *bar_low_color_wheel = NULL;
static lv_color_t selected_bar_low_custom_color;
static uint8_t bar_low_color_value_id = 0;

static lv_obj_t *bar_high_color_wheel_popup = NULL;
static lv_obj_t *bar_high_color_wheel = NULL;
static lv_color_t selected_bar_high_custom_color;
static uint8_t bar_high_color_value_id = 0;

static lv_obj_t *bar_in_range_color_wheel_popup = NULL;
static lv_obj_t *bar_in_range_color_wheel = NULL;
static lv_color_t selected_bar_in_range_custom_color;
static uint8_t bar_in_range_color_value_id = 0;

// Limiter color wheel popup event callbacks
static void limiter_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
		selected_limiter_custom_color;

	// Limiter circles color update removed - only bar flash effect is supported

	// Close the popup
	if (limiter_color_wheel_popup) {
		lv_obj_del(limiter_color_wheel_popup);
		limiter_color_wheel_popup = NULL;
		limiter_color_wheel = NULL;
	}
}

static void limiter_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (limiter_color_wheel_popup) {
		lv_obj_del(limiter_color_wheel_popup);
		limiter_color_wheel_popup = NULL;
		limiter_color_wheel = NULL;
	}
}

static void limiter_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_limiter_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

void create_limiter_color_wheel_popup(void) {
	// Don't create multiple popups
	if (limiter_color_wheel_popup)
		return;

	// Create popup background
	limiter_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(limiter_color_wheel_popup, 400, 350);
	lv_obj_center(limiter_color_wheel_popup);
	lv_obj_set_style_bg_color(limiter_color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(limiter_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(limiter_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(limiter_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(limiter_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(limiter_color_wheel_popup, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(limiter_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(limiter_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Limiter Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	limiter_color_wheel = lv_colorwheel_create(limiter_color_wheel_popup, true);
	lv_obj_set_size(limiter_color_wheel, 200, 200);
	lv_obj_align(limiter_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current limiter color
	lv_color_t current_color =
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color;
	lv_colorwheel_set_rgb(limiter_color_wheel, current_color);
	selected_limiter_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(limiter_color_wheel,
						limiter_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(limiter_color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);

	lv_obj_add_event_cb(ok_btn, limiter_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(limiter_color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);

	lv_obj_add_event_cb(cancel_btn, limiter_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}
void set_rpm_value(int rpm) {
	if (rpm < 0)
		rpm = 0;

	// Store the current CAN bus RPM value when not in demo mode
	if (!limiter_demo_active) {
		current_canbus_rpm = rpm;
	}

	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		// Map RPM to extended bar range to properly fill the extended bar width
		// When RPM reaches rpm_gauge_max, the bar should be completely filled
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);

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

			uint8_t limiter_effect =
				values_config[RPM_VALUE_ID - 1].rpm_limiter_effect;
			int32_t limiter_threshold =
				values_config[RPM_VALUE_ID - 1].rpm_limiter_value;

			// Add hysteresis: activate at threshold, deactivate at threshold -
			// 200 RPM
			const int32_t HYSTERESIS = 200;

			if (limiter_effect > 0) {
				if (!limiter_active && rpm >= limiter_threshold) {
					// RPM exceeded limiter threshold, start effect
					limiter_active = true;
					start_real_limiter_effect(limiter_effect);
				} else if (limiter_active &&
						   rpm < (limiter_threshold - HYSTERESIS)) {
					// RPM dropped below threshold minus hysteresis, stop effect
					limiter_active = false;
					stop_real_limiter_effect();
				}
				// If RPM is between (threshold - HYSTERESIS) and threshold,
				// maintain current state
			}
		}

		// Check if we should activate RPM background effect based on real RPM
		if (values_config[RPM_VALUE_ID - 1].rpm_background_enabled) {
			int32_t background_threshold =
				values_config[RPM_VALUE_ID - 1].rpm_background_value;

			// Add hysteresis for background effect too: activate at threshold,
			// deactivate at threshold - 200 RPM
			const int32_t BACKGROUND_HYSTERESIS = 200;

			if (!rpm_background_active && rpm >= background_threshold) {
				// RPM exceeded background threshold, change background color
				rpm_background_active = true;

				// Save original background color if not already saved
				if (!original_screen_bg_color_saved && ui_Screen3 &&
					lv_obj_is_valid(ui_Screen3)) {
					// Get current background color (assuming it's the default)
					original_screen_bg_color =
						THEME_COLOR_BG; // Default black background
					original_screen_bg_color_saved = true;
				}

				// Set background to the configured RPM background color
				if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
					lv_obj_set_style_bg_color(
						ui_Screen3,
						values_config[RPM_VALUE_ID - 1].rpm_background_color,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}

				// Change panel backgrounds to grey when RPM background is
				// active
				for (int i = 0; i < 8; i++) {
					if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
						lv_obj_set_style_bg_color(ui_Box[i], THEME_COLOR_PANEL,
												  LV_PART_MAIN |
													  LV_STATE_DEFAULT);
					}
				}

				// Change text elements to grey when RPM background is active
				if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
					lv_obj_set_style_text_color(ui_RPM_Value, THEME_COLOR_PANEL,
												LV_PART_MAIN |
													LV_STATE_DEFAULT);
				}
				if (ui_Speed_Value && lv_obj_is_valid(ui_Speed_Value)) {
					lv_obj_set_style_text_color(
						ui_Speed_Value, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Kmh && lv_obj_is_valid(ui_Kmh)) {
					lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_PANEL,
												LV_PART_MAIN |
													LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Label, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Value, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Label, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Value, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			} else if (rpm_background_active &&
					   rpm < (background_threshold - BACKGROUND_HYSTERESIS)) {
				// RPM dropped below threshold minus hysteresis, restore
				// original background
				rpm_background_active = false;

				// Restore original background color
				if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
					lv_obj_set_style_bg_color(ui_Screen3,
											  original_screen_bg_color,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				}

				// Restore panel backgrounds to black when RPM background is
				// inactive
				for (int i = 0; i < 8; i++) {
					if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
						lv_obj_set_style_bg_color(ui_Box[i], THEME_COLOR_BG,
												  LV_PART_MAIN |
													  LV_STATE_DEFAULT);
					}
				}

				// Restore text elements to white when RPM background is
				// inactive
				if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
					lv_obj_set_style_text_color(
						ui_RPM_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Speed_Value && lv_obj_is_valid(ui_Speed_Value)) {
					lv_obj_set_style_text_color(
						ui_Speed_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Kmh && lv_obj_is_valid(ui_Kmh)) {
					lv_obj_set_style_text_color(
						ui_Kmh, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Label, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Label, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}
			// If RPM is between (threshold - HYSTERESIS) and threshold,
			// maintain current state
		} else if (rpm_background_active) {
			// Background feature is disabled but background is still active -
			// restore it
			rpm_background_active = false;
			if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
				lv_obj_set_style_bg_color(ui_Screen3, original_screen_bg_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
			}

			// Restore panel backgrounds to black when RPM background is
			// disabled
			for (int i = 0; i < 8; i++) {
				if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
					lv_obj_set_style_bg_color(ui_Box[i], THEME_COLOR_BG,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}

			// Restore text elements to white when RPM background is disabled
			if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
				lv_obj_set_style_text_color(ui_RPM_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Speed_Value && lv_obj_is_valid(ui_Speed_Value)) {
				lv_obj_set_style_text_color(ui_Speed_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Kmh && lv_obj_is_valid(ui_Kmh)) {
				lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
				lv_obj_set_style_text_color(ui_Bar_1_Label,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
				lv_obj_set_style_text_color(ui_Bar_1_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
				lv_obj_set_style_text_color(ui_Bar_2_Label,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
				lv_obj_set_style_text_color(ui_Bar_2_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		}
	}
}
void update_redline_position(void) {
	if (!rpm_redline_zone)
		return;

	// Calculate redline position as percentage of max RPM
	float redline_percentage = (float)rpm_redline_value / (float)rpm_gauge_max;

	// Clamp to prevent going beyond the bar
	if (redline_percentage > 1.0f)
		redline_percentage = 1.0f;
	if (redline_percentage < 0.0f)
		redline_percentage = 0.0f;

	// Screen and RPM bar dimensions
	const lv_coord_t screen_width = 800; // Full screen width
	const lv_coord_t bar_width = 765;
	const lv_coord_t screen_right_edge =
		screen_width / 2; // Right edge relative to center

	// Calculate redline zone dimensions - extends from right edge of screen to
	// redline position
	lv_coord_t redline_rpm_position =
		-(bar_width / 2) +
		(redline_percentage * bar_width); // RPM position on bar
	lv_coord_t redline_width =
		screen_right_edge -
		redline_rpm_position; // From redline to right edge of screen

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

	printf("Redline updated: %d RPM at %.1f%% (zone: from RPM pos %d to screen "
		   "edge, width=%d)\n",
		   rpm_redline_value, redline_percentage * 100, redline_rpm_position,
		   redline_width);
}
void update_rpm_ui(void *param) {
	rpm_update_t *r_upd = (rpm_update_t *)param;

	// Check if both the RPM label and RPM gauge are valid.
	if ((ui_RPM_Value == NULL || lv_obj_get_screen(ui_RPM_Value) == NULL) ||
		(rpm_bar_gauge == NULL || lv_obj_get_screen(rpm_bar_gauge) == NULL)) {
		free(r_upd);
		return;
	}

	lv_label_set_text(ui_RPM_Value, r_upd->rpm_str);
	set_rpm_value(r_upd->rpm_value);

	// Update menu RPM value text when CAN bus is active and no limiter demo is
	// running
	if (!limiter_demo_active) {
		update_menu_rpm_value_text(r_upd->rpm_value);
	}

	free(r_upd);
}

// Immediate RPM update
void update_rpm_ui_immediate(const char *rpm_str, int rpm_value) {
	if ((ui_RPM_Value == NULL || lv_obj_get_screen(ui_RPM_Value) == NULL) ||
		(rpm_bar_gauge == NULL || lv_obj_get_screen(rpm_bar_gauge) == NULL)) {
		return;
	}
	lv_label_set_text(ui_RPM_Value, rpm_str);
	set_rpm_value(rpm_value);
	if (!limiter_demo_active) {
		update_menu_rpm_value_text(rpm_value);
	}
}
void create_rpm_bar_gauge(lv_obj_t *parent_screen) {
	ui_RPM_Base_1 = create_panel(parent_screen, 800, 6, 0, -182, 0,
								 THEME_COLOR_PANEL, 0); // Moved up 2px
	ui_RPM_Base_2 = create_panel(parent_screen, 49, 22, -41, -193, 7,
								 THEME_COLOR_PANEL, 550);
	ui_RPM_Base_3 = create_panel(parent_screen, 49, 22, 105, -181, 7,
								 THEME_COLOR_PANEL, 1250);
	ui_RPM_Base_4 =
		create_panel(parent_screen, 111, 44, 0, -176, 7, THEME_COLOR_PANEL,
					 0); // Back to original position
	lv_color_t saved_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
	ui_Panel9 = create_panel(parent_screen, 55, 55, -373, -213, 0, saved_color,
							 0); // Moved up 2px, left 1px

	// Calculate extended RPM max for rightward extension to screen edge
	// Original RPM bar: 765px centered (left edge at -382.5px, right edge at
	// +382.5px) New RPM bar: extends from -382.5px to +400px (screen edge) =
	// 782.5px total Keep left edge at -382.5px, extend only rightward
	const float bar_extension_ratio = 782.5f / 765.0f;
	int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);

	// Create the RPM bar gauge with extended range and rightward extension
	rpm_bar_gauge = lv_bar_create(parent_screen);
	lv_bar_set_range(rpm_bar_gauge, 0, extended_rpm_max);
	lv_bar_set_value(rpm_bar_gauge, 0, LV_ANIM_OFF);
	lv_obj_set_size(rpm_bar_gauge, 783, 55); // 782.5px rounded up to 783px

	// Position bar so left edge stays at -382.5px and extends to +400px (screen
	// edge) Left edge needs to be at -382.5px, so center should be at: -382.5 +
	// (783/2) = 8.75px
	lv_obj_align(rpm_bar_gauge, LV_ALIGN_TOP_MID, 8,
				 0); // Adjusted to fill remaining space (1px left, 2px up)

	// Set styles for the RPM bar gauge (no gradient, solid color)
	lv_obj_set_style_radius(rpm_bar_gauge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_bar_gauge, THEME_COLOR_RPM_BAR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT); // Light gray
	lv_obj_set_style_bg_opa(rpm_bar_gauge, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);

	// Use the saved color for the RPM bar indicator
	lv_obj_set_style_radius(rpm_bar_gauge, 0,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_bar_gauge, saved_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(rpm_bar_gauge, 255,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Set gradient color to same as main color for solid appearance
	lv_obj_set_style_bg_grad_color(rpm_bar_gauge, saved_color,
								   LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
								 LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Create redline zone rectangle (above RPM bar, below numbers/lines)
	rpm_redline_zone = lv_obj_create(parent_screen);
	lv_obj_set_height(rpm_redline_zone,
					  12);				  // Same height as the taller RPM lines
	lv_obj_set_y(rpm_redline_zone, -191); // Moved up 2px
	lv_obj_set_align(rpm_redline_zone, LV_ALIGN_CENTER);
	lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(rpm_redline_zone, 0,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_redline_zone, THEME_COLOR_RED,
							  LV_PART_MAIN | LV_STATE_DEFAULT); // Bright red
	lv_obj_set_style_bg_opa(rpm_redline_zone, 180,
							LV_PART_MAIN |
								LV_STATE_DEFAULT); // Semi-transparent
	lv_obj_set_style_border_width(rpm_redline_zone, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	// Initial position and width will be set by update_redline_position()

	// Create large transparent click zone covering the entire extended RPM bar
	// gauge
	lv_obj_t *rpm_click_zone = lv_obj_create(parent_screen);
	lv_obj_set_size(rpm_click_zone, 783, 55); // Match extended RPM bar size
	lv_obj_align(rpm_click_zone, LV_ALIGN_TOP_MID, 9,
				 2); // Match extended RPM bar position
	lv_obj_clear_flag(rpm_click_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(rpm_click_zone, 0,
							LV_PART_MAIN |
								LV_STATE_DEFAULT); // Completely transparent
	lv_obj_set_style_border_opa(rpm_click_zone, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT); // No border
	lv_obj_add_flag(rpm_click_zone, LV_OBJ_FLAG_CLICKABLE);

	// Allocate memory to store RPM value_id and pass it to the event callback
	uint8_t *rpm_id_ptr = lv_mem_alloc(sizeof(uint8_t));
	*rpm_id_ptr = RPM_VALUE_ID;
	lv_obj_add_event_cb(rpm_click_zone, value_long_press_event_cb,
						LV_EVENT_LONG_PRESSED, rpm_id_ptr);
	lv_obj_add_event_cb(rpm_click_zone, free_value_id_event_cb, LV_EVENT_DELETE,
						rpm_id_ptr);
}

int num_rpm_lines = 0;
lv_obj_t *rpm_labels[MAX_RPM_LINES];	// Only need labels for the first set
lv_obj_t *rpm_lines[MAX_RPM_LINES * 2]; // Two sets of lines

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
	lv_coord_t bar_y_set1 = 0;	// First set starts at px (moved up 2px)
	lv_coord_t bar_y_set2 = 42; // Second set starts at px (moved up 2px)

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
		bool add_label = false; // Only label the 1000s in the first set

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
		lv_obj_set_style_bg_color(line_top, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(line_top, LV_OPA_COVER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(line_top, 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_clear_flag(line_top, LV_OBJ_FLAG_SCROLLABLE);

		// Position the line so it's centered horizontally on x_pos
		lv_coord_t adjusted_x = x_pos - (line_width / 2);
		lv_obj_set_pos(line_top, adjusted_x, bar_y_set1);

		rpm_lines[num_rpm_lines] = line_top;

		// Add a label for 1000 RPM ticks in the first set
		if (add_label) {
			lv_obj_t *label = lv_label_create(parent);

			// Display the "thousands" place (e.g., "7" for 7000)
			char rpm_str[8];
			snprintf(rpm_str, sizeof(rpm_str), "%d", rpm_value / 1000);
			lv_label_set_text(label, rpm_str);

			// Style the label
			lv_obj_set_style_text_color(label, THEME_COLOR_BG, LV_PART_MAIN);
			lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
			lv_obj_set_style_text_font(label, THEME_FONT_DASH_TICK,
									   LV_PART_MAIN | LV_STATE_DEFAULT);

			// Position the label below the line
			lv_obj_align_to(label, line_top, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);

			rpm_labels[num_rpm_lines] = label;
		}

		num_rpm_lines++;

		// Create the second set of lines (bottom row, flipped height)
		lv_obj_t *line_bottom = lv_obj_create(parent);
		lv_obj_set_size(line_bottom, line_width, line_height);
		lv_obj_set_style_radius(line_bottom, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(line_bottom, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(line_bottom, LV_OPA_COVER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(line_bottom, 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(line_bottom, 0,
								 LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_clear_flag(line_bottom, LV_OBJ_FLAG_SCROLLABLE);

		// Position the bottom line flat at the bottom with its height flipped
		lv_obj_set_pos(line_bottom, adjusted_x,
					   bar_y_set2 + (13 - line_height));

		rpm_lines[num_rpm_lines] = line_bottom;

		num_rpm_lines++;

		// Stop if we exceed the maximum number of lines
		if (num_rpm_lines >= MAX_RPM_LINES * 2) {
			break;
		}
	}
}

void widget_rpm_bar_create(lv_obj_t *parent) {
	create_rpm_bar_gauge(parent);
	update_rpm_lines(parent);
	update_redline_position();

	ui_RPM_Value = lv_label_create(parent);
	lv_obj_set_width(ui_RPM_Value, LV_SIZE_CONTENT);
	lv_obj_set_height(ui_RPM_Value, LV_SIZE_CONTENT);
	lv_obj_set_x(ui_RPM_Value, 0);
	lv_obj_set_y(ui_RPM_Value, -127);
	lv_obj_set_align(ui_RPM_Value, LV_ALIGN_CENTER);
	lv_label_set_text(ui_RPM_Value, "---");
	strcpy(previous_values[RPM_VALUE_ID - 1], "---");
	lv_obj_set_style_text_color(ui_RPM_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_RPM_Value, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_RPM_Value, THEME_FONT_DASH_RPM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_RPM_Label = lv_label_create(parent);
	lv_obj_set_x(ui_RPM_Label, 0);
	lv_obj_set_y(ui_RPM_Label, -164);
	lv_obj_set_align(ui_RPM_Label, LV_ALIGN_CENTER);
	lv_label_set_text(ui_RPM_Label, "RPM");
	lv_obj_set_style_text_color(ui_RPM_Label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_RPM_Label, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
		create_rpm_lights_circles(parent);
	}
}

uint64_t *widget_rpm_bar_get_last_can_time(void) {
	return &last_rpm_can_received;
}

/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

static void _rpm_bar_create(widget_t *w, lv_obj_t *parent) {
	widget_rpm_bar_create(parent);
	/* The RPM bar spans the full top of the screen; use the gauge as root */
	w->root = rpm_bar_gauge;
}
static void _rpm_bar_update(widget_t *w, void *data) {
	(void)w;
	update_rpm_ui(data);
}
static void _rpm_bar_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	w->w = nw;
	w->h = nh;
}
static void _rpm_bar_open_settings(widget_t *w) { (void)w; }
static void _rpm_bar_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	uint8_t idx = RPM_VALUE_ID - 1;
	cJSON_AddNumberToObject(cfg, "can_id", values_config[idx].can_id);
	cJSON_AddNumberToObject(cfg, "rpm_max", rpm_gauge_max);
	cJSON_AddNumberToObject(cfg, "redline", rpm_redline_value);
	cJSON_AddNumberToObject(cfg, "limiter",
							values_config[idx].rpm_limiter_value);
	cJSON_AddBoolToObject(cfg, "lights_enabled",
						  values_config[idx].rpm_lights_enabled);
}
static void _rpm_bar_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
}
static void _rpm_bar_destroy(widget_t *w) { free(w); }

widget_t *widget_rpm_bar_create_instance(void) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	w->type = WIDGET_RPM_BAR;
	/* RPM bar occupies full screen width at top */
	w->x = 0;
	w->y = 0;
	w->w = 800;
	w->h = 55;
	snprintf(w->id, sizeof(w->id), "rpm_bar_0");

	w->create = _rpm_bar_create;
	w->update = _rpm_bar_update;
	w->resize = _rpm_bar_resize;
	w->open_settings = _rpm_bar_open_settings;
	w->to_json = _rpm_bar_to_json;
	w->from_json = _rpm_bar_from_json;
	w->destroy = _rpm_bar_destroy;

	return w;
}
