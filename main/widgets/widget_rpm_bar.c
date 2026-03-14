#include "widget_rpm_bar.h"
#include "esp_heap_caps.h"
#include "signal.h"
#include "can/can_decode.h"
#include "driver/twai.h"
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
#include "ui/config_bridge.h"
#include "widget_bar.h"
#include "widget_panel.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t last_rpm_can_received = 0;

/* Forward declarations */

void update_rpm_ui_immediate(const char *rpm_str, int rpm_value);

/* Missing static state variables */
static lv_obj_t *rpm_lines_parent = NULL;
static lv_obj_t *s_rpm_container = NULL;

/* menu_rpm_value_label is owned by menu_screen.c */
extern lv_obj_t *menu_rpm_value_label;

void widget_rpm_bar_clear_stale_pointers(void) {
	/* After lv_obj_clean(screen), all child objects are already freed.
	 * NULL out our bookkeeping so update_rpm_lines won't touch freed ptrs. */
	for (int i = 0; i < num_rpm_lines; i++) {
		rpm_lines[i] = NULL;
		if (i < MAX_RPM_LINES)
			rpm_labels[i] = NULL;
	}
	num_rpm_lines = 0;
	rpm_lines_parent = NULL;
	s_rpm_container = NULL;
}

/* ── Helper: look up rpm_bar_data_t ────────────────────────────────────── */
static rpm_bar_data_t *_get_rpm_bar_data(void) {
	widget_t **widgets = dashboard_get_widgets();
	uint8_t count = dashboard_get_widget_count();
	for (uint8_t i = 0; i < count; i++) {
		if (widgets[i] && widgets[i]->type == WIDGET_RPM_BAR) {
			return (rpm_bar_data_t *)widgets[i]->type_data;
		}
	}
	return NULL;
}

static int current_canbus_rpm = 0; // Store the current CAN bus RPM value

// CAN timeout tracking
static bool rpm_color_needs_update = false;
static lv_color_t new_rpm_color;
void rpm_gauge_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);
	rpm_gauge_max = 3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

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

	update_redline_position();
}

void rpm_ecu_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	// Indices: 0 = Custom, 1 = MaxxECU, 2 = Haltech

	if (selected == 0) {
		// ========== CUSTOM ==========
		printf("ECU Presets: Custom (no changes)\n");
		return;
	}

	/* Preset CAN parameters: can_id, endian, bit_start, bit_length, scale, offset, decimals */
	uint32_t can_id = 0;
	uint8_t endian = 1, bit_start = 0, bit_length = 16, decimals = 0;
	float scale = 1.0f, offset = 0.0f;
	const char *name = "Custom";

	if (selected == 1) {
		name = "MaxxECU"; can_id = 0x208; endian = 1;
		scale = 1.0f; offset = 0.0f; decimals = 0;
	} else if (selected == 2) {
		name = "Haltech"; can_id = 0x168; endian = 0;
		scale = 1.0f; offset = 0.0f; decimals = 0;
	} else if (selected == 3) {
		name = "Ford BA/BF/FG"; can_id = 0x3E8; endian = 1;
		scale = 0.25f; offset = 0.0f; decimals = 2;
	}

	printf("ECU Presets: %s\n", name);

	/* Write to signal registry via config_bridge */
	config_bridge_set_can_id(RPM_VALUE_ID, can_id);
	config_bridge_set_endian(RPM_VALUE_ID, endian);
	config_bridge_set_bit_start(RPM_VALUE_ID, bit_start);
	config_bridge_set_bit_length(RPM_VALUE_ID, bit_length);
	config_bridge_set_scale(RPM_VALUE_ID, scale);
	config_bridge_set_offset(RPM_VALUE_ID, offset);
	config_bridge_set_decimals(RPM_VALUE_ID, decimals);

	/* Update UI form controls to reflect the preset */
	char hex_buf[16];
	snprintf(hex_buf, sizeof(hex_buf), "%X", can_id);
	if (g_can_id_input[RPM_VALUE_ID - 1])
		lv_textarea_set_text(g_can_id_input[RPM_VALUE_ID - 1], hex_buf);
	if (g_endian_dropdown[RPM_VALUE_ID - 1])
		lv_dropdown_set_selected(g_endian_dropdown[RPM_VALUE_ID - 1], endian);
	if (g_bit_start_dropdown[RPM_VALUE_ID - 1])
		lv_dropdown_set_selected(g_bit_start_dropdown[RPM_VALUE_ID - 1], bit_start);
	if (g_bit_length_dropdown[RPM_VALUE_ID - 1])
		lv_dropdown_set_selected(g_bit_length_dropdown[RPM_VALUE_ID - 1], bit_length - 1);
	char scale_buf[16];
	snprintf(scale_buf, sizeof(scale_buf), "%g", scale);
	if (g_scale_input[RPM_VALUE_ID - 1])
		lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], scale_buf);
	char offset_buf[16];
	snprintf(offset_buf, sizeof(offset_buf), "%g", offset);
	if (g_offset_input[RPM_VALUE_ID - 1])
		lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], offset_buf);
	if (g_decimals_dropdown[RPM_VALUE_ID - 1])
		lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1], decimals);
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
	rpm_color_needs_update = true;
		rpm_bar_data_t *rd = _get_rpm_bar_data();
	if (rd) rd->bar_color = new_rpm_color;
}

void check_rpm_color_update(lv_timer_t *timer) {
	// Don't update colors when real limiter effect is active to avoid conflicts
	// with flashing
	if (rpm_color_needs_update) {
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_style_bg_color(rpm_bar_gauge, new_rpm_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			// Set gradient color to same as main color for solid appearance
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, new_rpm_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
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
	rpm_bar_data_t *rd = _get_rpm_bar_data();
	if (rd) rd->limiter_effect = effect_type;
}

void rpm_limiter_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);

	int32_t rpm_value =
		3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update configuration
	rpm_bar_data_t *rd = _get_rpm_bar_data();
	if (rd) rd->limiter_value = rpm_value;
}

void rpm_limiter_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	rpm_bar_data_t *rd = _get_rpm_bar_data();
	if (!rd && selected != 9) return;

	switch (selected) {
	case 0: rd->limiter_color = THEME_COLOR_GREEN; break;
	case 1: rd->limiter_color = THEME_COLOR_CYAN; break;
	case 2: rd->limiter_color = THEME_COLOR_YELLOW; break;
	case 3: rd->limiter_color = THEME_COLOR_ORANGE; break;
	case 4: rd->limiter_color = THEME_COLOR_RED; break;
	case 5: rd->limiter_color = THEME_COLOR_BLUE; break;
	case 6: rd->limiter_color = THEME_COLOR_PURPLE; break;
	case 7: rd->limiter_color = THEME_COLOR_MAGENTA; break;
	case 8: rd->limiter_color = THEME_COLOR_PINK; break;
	case 9: create_limiter_color_wheel_popup(); break;
	}
}

void rpm_background_switch_event_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	bool is_checked = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	// Update configuration
	rpm_bar_data_t *rd = _get_rpm_bar_data();
	if (rd) rd->background_enabled = is_checked;
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
	rpm_bar_data_t *rd = _get_rpm_bar_data();
	if (rd) rd->background_color = new_background_color;
}

void rpm_background_threshold_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);

	int32_t threshold_value =
		3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update configuration
	rpm_bar_data_t *rd = _get_rpm_bar_data();
	if (rd) rd->background_value = threshold_value;
}

static void update_menu_rpm_value_text(int rpm_value) {
	// Update the RPM value text in menu screen during demos
	// Guard: menu must be the active screen and label must be valid
	if (menu_rpm_value_label && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen &&
		lv_obj_is_valid(menu_rpm_value_label)) {
		// Apply same 102.3% scaling to the actual RPM value for consistency
		int display_rpm_value = (int)((float)rpm_value * 1.0229f);
		char rpm_text[16];
		snprintf(rpm_text, sizeof(rpm_text), "%d", display_rpm_value);
		lv_label_set_text(menu_rpm_value_label, rpm_text);
	}
}

// Real limiter effect implementation (triggered by actual RPM)

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

static void color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_custom_color = lv_colorwheel_get_rgb(colorwheel);

	// Show live preview by updating the RPM bar immediately
	new_rpm_color = selected_custom_color;
	rpm_color_needs_update = true;
}

// Color wheel popup event callbacks
static void color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	new_rpm_color = selected_custom_color;
	rpm_color_needs_update = true;
	{
		rpm_bar_data_t *rd = _get_rpm_bar_data();
		if (rd) rd->bar_color = selected_custom_color;
	}

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

// RPM Background color wheel popup event callbacks
static void rpm_background_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	{
		rpm_bar_data_t *rd = _get_rpm_bar_data();
		if (rd) rd->background_color = selected_rpm_background_custom_color;
	}

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
	rpm_bar_data_t *rd_bg = _get_rpm_bar_data();
	lv_color_t current_color = rd_bg ? rd_bg->background_color : THEME_COLOR_GREEN;
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
	rpm_bar_data_t *rd_cw = _get_rpm_bar_data();
	lv_color_t current_color = rd_cw ? rd_cw->bar_color : THEME_COLOR_GREEN;
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

// Limiter color wheel popup event callbacks
static void limiter_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	{
		rpm_bar_data_t *rd = _get_rpm_bar_data();
		if (rd) rd->limiter_color = selected_limiter_custom_color;
	}

	// Limiter circles color update removed - only bar flash effect is
	// supported

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
	rpm_bar_data_t *rd_lc = _get_rpm_bar_data();
	lv_color_t current_color = rd_lc ? rd_lc->limiter_color : THEME_COLOR_RED;
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

	// Store the current CAN bus RPM value
	current_canbus_rpm = rpm;

	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		// Map RPM to extended bar range to properly fill the extended
		// bar width When RPM reaches rpm_gauge_max, the bar should be
		// completely filled
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);

		// Scale the RPM value to the extended range
		int32_t scaled_rpm = (rpm * extended_rpm_max) / rpm_gauge_max;

		lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
	}

	// Check if we should activate RPM background effect based on
	// real RPM
	rpm_bar_data_t *rd_bg = _get_rpm_bar_data();
	if (rd_bg && rd_bg->background_enabled) {
		int32_t background_threshold = rd_bg->background_value;

		// Add hysteresis for background effect too: activate at
		// threshold, deactivate at threshold - 200 RPM
		const int32_t BACKGROUND_HYSTERESIS = 200;

		if (!rpm_background_active && rpm >= background_threshold) {
			// RPM exceeded background threshold, change background
			// color
			rpm_background_active = true;

			// Save original background color if not already saved
			if (!original_screen_bg_color_saved && ui_Screen3 &&
				lv_obj_is_valid(ui_Screen3)) {
				// Get current background color (assuming it's the
				// default)
				original_screen_bg_color =
					THEME_COLOR_BG; // Default black background
				original_screen_bg_color_saved = true;
			}

			// Set background to the configured RPM background color
			if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
				lv_obj_set_style_bg_color(
					ui_Screen3,
					rd_bg->background_color,
					LV_PART_MAIN | LV_STATE_DEFAULT);
			}

			// Change panel backgrounds to grey when RPM background
			// is active
			for (int i = 0; i < 8; i++) {
				if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
					lv_obj_set_style_bg_color(ui_Box[i], THEME_COLOR_PANEL,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}

			// Change text elements to grey when RPM background is
			// active
			if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
				lv_obj_set_style_text_color(ui_RPM_Value, THEME_COLOR_PANEL,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
				lv_obj_set_style_text_color(ui_Bar_1_Label, THEME_COLOR_PANEL,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
				lv_obj_set_style_text_color(ui_Bar_1_Value, THEME_COLOR_PANEL,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
				lv_obj_set_style_text_color(ui_Bar_2_Label, THEME_COLOR_PANEL,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
				lv_obj_set_style_text_color(ui_Bar_2_Value, THEME_COLOR_PANEL,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		} else if (rpm_background_active &&
				   rpm < (background_threshold - BACKGROUND_HYSTERESIS)) {
			// RPM dropped below threshold minus hysteresis, restore
			// original background
			rpm_background_active = false;

			// Restore original background color
			if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
				lv_obj_set_style_bg_color(ui_Screen3, original_screen_bg_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
			}

			// Restore panel backgrounds to black when RPM
			// background is inactive
			for (int i = 0; i < 8; i++) {
				if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
					lv_obj_set_style_bg_color(ui_Box[i], THEME_COLOR_BG,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}

			// Restore text elements to white when RPM background is
			// inactive
			if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
				lv_obj_set_style_text_color(ui_RPM_Value,
											THEME_COLOR_TEXT_PRIMARY,
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
		// If RPM is between (threshold - HYSTERESIS) and threshold,
		// maintain current state
	} else if (rpm_background_active) {
		// Background feature is disabled but background is still
		// active - restore it
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

		// Restore text elements to white when RPM background is
		// disabled
		if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
			lv_obj_set_style_text_color(ui_RPM_Value, THEME_COLOR_TEXT_PRIMARY,
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

	// Calculate redline zone dimensions - extends from right edge of
	// screen to redline position
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
	// Position so it starts at redline RPM position and extends to
	// right edge
	lv_obj_set_x(rpm_redline_zone, redline_rpm_position + (redline_width / 2));

	printf("Redline updated: %d RPM at %.1f%% (zone: from RPM pos %d "
		   "to screen "
		   "edge, width=%d)\n",
		   rpm_redline_value, redline_percentage * 100, redline_rpm_position,
		   redline_width);
}
/* Async update payload for lv_async_call(update_rpm_ui, ...) */
typedef struct {
	char rpm_str[32];
	int  rpm_value;
} rpm_update_t;

void update_rpm_ui(void *param) {
	rpm_update_t *r_upd = (rpm_update_t *)param;

	if (rpm_bar_gauge == NULL || lv_obj_get_screen(rpm_bar_gauge) == NULL) {
		free(r_upd);
		return;
	}

	if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
		lv_label_set_text(ui_RPM_Value, r_upd->rpm_str);
	set_rpm_value(r_upd->rpm_value);

	// Update menu RPM value text when CAN bus is active
	update_menu_rpm_value_text(r_upd->rpm_value);

	free(r_upd);
}

// Immediate RPM update
void update_rpm_ui_immediate(const char *rpm_str, int rpm_value) {
	if (rpm_bar_gauge == NULL || lv_obj_get_screen(rpm_bar_gauge) == NULL) {
		return;
	}
	if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
		lv_label_set_text(ui_RPM_Value, rpm_str);
	set_rpm_value(rpm_value);
	update_menu_rpm_value_text(rpm_value);
}
void create_rpm_bar_gauge(lv_obj_t *container) {
	rpm_bar_data_t *rd_bar = _get_rpm_bar_data();
	lv_color_t saved_color = rd_bar ? rd_bar->bar_color : THEME_COLOR_GREEN;

	/* Panel9 — color indicator square at left edge.
	 * Inside the 800x55 container, center-relative: (-373, 0). */
	ui_Panel9 =
		create_panel(container, 55, 55, -373, 0, 0, saved_color, 0);

	const float bar_extension_ratio = 782.5f / 765.0f;
	int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);

	rpm_bar_gauge = lv_bar_create(container);
	lv_bar_set_range(rpm_bar_gauge, 0, extended_rpm_max);
	lv_bar_set_value(rpm_bar_gauge, 0, LV_ANIM_OFF);
	lv_obj_set_size(rpm_bar_gauge, 783, 55);
	lv_obj_align(rpm_bar_gauge, LV_ALIGN_TOP_MID, 20, 0);

	lv_obj_set_style_radius(rpm_bar_gauge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_bar_gauge, THEME_COLOR_RPM_BAR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(rpm_bar_gauge, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_set_style_radius(rpm_bar_gauge, 0,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_bar_gauge, saved_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(rpm_bar_gauge, 255,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);

	lv_obj_set_style_bg_grad_color(rpm_bar_gauge, saved_color,
								   LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
								 LV_PART_INDICATOR | LV_STATE_DEFAULT);

	/* Redline zone — inside container, center-relative y.
	 * Screen y=-191 → container y = -191 - (-213) = 22. */
	rpm_redline_zone = lv_obj_create(container);
	lv_obj_set_height(rpm_redline_zone, 12);
	lv_obj_set_y(rpm_redline_zone, 22);
	lv_obj_set_align(rpm_redline_zone, LV_ALIGN_CENTER);
	lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(rpm_redline_zone, 0,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_redline_zone, THEME_COLOR_RED,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(rpm_redline_zone, 180,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(rpm_redline_zone, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Click zone */
	lv_obj_t *rpm_click_zone = lv_obj_create(container);
	lv_obj_set_size(rpm_click_zone, 783, 55);
	lv_obj_align(rpm_click_zone, LV_ALIGN_TOP_MID, 9, 2);
	lv_obj_clear_flag(rpm_click_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(rpm_click_zone, 0,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(rpm_click_zone, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_flag(rpm_click_zone, LV_OBJ_FLAG_CLICKABLE);

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
/* Track the parent we last built lines for so we don't try to delete
 * children of a screen that has already been destroyed. */

void update_rpm_lines(lv_obj_t *parent) {
	/* If the parent has changed (e.g. Screen3 was recreated after a
	 * layout save), the old LVGL objects have already been deleted
	 * when the previous screen was destroyed.  In that case, just
	 * clear our bookkeeping without touching the stale pointers. */
	if (parent != rpm_lines_parent) {
		for (int i = 0; i < num_rpm_lines; i++) {
			rpm_lines[i] = NULL;
			if (i < MAX_RPM_LINES)
				rpm_labels[i] = NULL;
		}
		num_rpm_lines = 0;
		rpm_lines_parent = parent;
	} else {
		// Same parent: safe to delete and rebuild in-place.
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
	}

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

		// Position the bottom line flat at the bottom with its height
		// flipped
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

lv_obj_t *widget_rpm_bar_create(lv_obj_t *parent) {
	/* Create a transparent container that holds all RPM sub-components.
	 * This allows the whole RPM widget to be moved as a single unit. */
	lv_obj_t *container = lv_obj_create(parent);
	lv_obj_set_size(container, 800, 55);
	lv_obj_set_align(container, LV_ALIGN_CENTER);
	lv_obj_set_pos(container, 0, -213);
	lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	create_rpm_bar_gauge(container);
	update_rpm_lines(container);
	update_redline_position();

	s_rpm_container = container;
	return container;
}

uint64_t *widget_rpm_bar_get_last_can_time(void) {
	return &last_rpm_can_received;
}

/* ── Phase 2: widget_t factory
 * ───────────────────────────────────────────── */

static void _rpm_bar_on_signal(float value, bool is_stale, void *user_data) {
	(void)user_data;
	if (is_stale) {
		update_rpm_ui_immediate("---", 0);
		return;
	}
	int rpm = (int)value;
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", rpm);
	update_rpm_ui_immediate(buf, rpm);
}

static void _rpm_bar_create(widget_t *w, lv_obj_t *parent) {
	lv_obj_t *container = widget_rpm_bar_create(parent);
	w->root = container;

	/* Apply layout-defined size and position (overrides hardcoded defaults) */
	lv_obj_set_size(container, w->w, w->h);
	lv_obj_set_pos(container, w->x, w->y);

	/* Subscribe to signal if bound */
	rpm_bar_data_t *rbd = (rpm_bar_data_t *)w->type_data;
	if (rbd && rbd->signal_index >= 0)
		signal_subscribe(rbd->signal_index, _rpm_bar_on_signal, w);
}
static void _rpm_bar_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	w->w = nw;
	w->h = nh;
}
static void _rpm_bar_open_settings(widget_t *w) { (void)w; }
static void _rpm_bar_to_json(widget_t *w, cJSON *out) {
	rpm_bar_data_t *rd = (rpm_bar_data_t *)w->type_data;
	widget_base_to_json(w, out);
	if (!rd) return;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	cJSON_AddNumberToObject(cfg, "rpm_max", rd->gauge_max);
	cJSON_AddNumberToObject(cfg, "redline", rd->redline);
	cJSON_AddNumberToObject(cfg, "bar_color", (int)rd->bar_color.full);
	cJSON_AddNumberToObject(cfg, "limiter_effect", rd->limiter_effect);
	cJSON_AddNumberToObject(cfg, "limiter_value", rd->limiter_value);
	cJSON_AddNumberToObject(cfg, "limiter_color", (int)rd->limiter_color.full);
	cJSON_AddBoolToObject(cfg, "lights_enabled", rd->lights_enabled);
	cJSON_AddBoolToObject(cfg, "background_enabled", rd->background_enabled);
	cJSON_AddNumberToObject(cfg, "background_value", rd->background_value);
	cJSON_AddNumberToObject(cfg, "background_color", (int)rd->background_color.full);
	if (rd->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", rd->signal_name);
}
static void _rpm_bar_from_json(widget_t *w, cJSON *in) {
	rpm_bar_data_t *rd = (rpm_bar_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!rd) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;
	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "rpm_max");
	if (cJSON_IsNumber(item) && item->valueint > 0) {
		rd->gauge_max = item->valueint;
		rpm_gauge_max = rd->gauge_max; /* sync global for config_modal */
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "redline");
	if (cJSON_IsNumber(item) && item->valueint >= 0) {
		rd->redline = item->valueint;
		rpm_redline_value = rd->redline; /* sync global for config_modal */
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_color");
	if (cJSON_IsNumber(item)) rd->bar_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_effect");
	if (cJSON_IsNumber(item)) rd->limiter_effect = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_value");
	if (cJSON_IsNumber(item)) rd->limiter_value = (int32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_color");
	if (cJSON_IsNumber(item)) rd->limiter_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "lights_enabled");
	if (cJSON_IsBool(item)) rd->lights_enabled = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "background_enabled");
	if (cJSON_IsBool(item)) rd->background_enabled = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "background_value");
	if (cJSON_IsNumber(item)) rd->background_value = (int32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "background_color");
	if (cJSON_IsNumber(item)) rd->background_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(rd->signal_name, item->valuestring, sizeof(rd->signal_name) - 1);

	/* Resolve signal name → index */
	if (rd->signal_name[0] != '\0')
		rd->signal_index = signal_find_by_name(rd->signal_name);
}
static void _rpm_bar_destroy(widget_t *w) {
	if (w) {
		free(w->type_data);
		free(w);
	}
}

widget_t *widget_rpm_bar_create_instance(void) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	rpm_bar_data_t *rd = heap_caps_calloc(1, sizeof(rpm_bar_data_t), MALLOC_CAP_SPIRAM);
	if (!rd)
		rd = calloc(1, sizeof(rpm_bar_data_t));
	if (!rd) {
		free(w);
		return NULL;
	}

	rd->signal_index = -1;

	/* Sensible defaults */
	rd->gauge_max = 8000;
	rd->redline = 6500;
	rd->bar_color = lv_color_hex(0x00FF00);  /* green */
	rd->limiter_effect = 0;
	rd->limiter_value = 7500;
	rd->limiter_color = lv_color_hex(0xFF0000);  /* red */
	rd->lights_enabled = false;
	rd->background_enabled = false;
	rd->background_value = 0;
	rd->background_color = lv_color_hex(0x000000);

	w->type_data = rd;
	w->type = WIDGET_RPM_BAR;
	w->slot = 0;
	/* RPM bar occupies full screen width at top.
	 * y = -240 + 55/2 = -213 in center-origin coords. */
	w->x = 0;
	w->y = -213;
	w->w = 800;
	w->h = 55;
	snprintf(w->id, sizeof(w->id), "rpm_bar_0");

	w->create = _rpm_bar_create;
	w->resize = _rpm_bar_resize;
	w->open_settings = _rpm_bar_open_settings;
	w->to_json = _rpm_bar_to_json;
	w->from_json = _rpm_bar_from_json;
	w->destroy = _rpm_bar_destroy;

	return w;
}

