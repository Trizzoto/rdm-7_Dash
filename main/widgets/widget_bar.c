#include "widget_bar.h"
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

uint64_t last_bar_can_received[2] = {0, 0};
float previous_bar_values[2] = {0, 0};

/* Global LVGL object definitions (declared extern in widget_bar.h) */
lv_obj_t *ui_Bar_1_Value = NULL;
lv_obj_t *ui_Bar_2_Value = NULL;

/* Color wheel popup state (one active at a time) */
static lv_obj_t *bar_low_color_wheel_popup = NULL;
static lv_obj_t *bar_low_color_wheel = NULL;
static uint8_t bar_low_color_value_id = 0;
static lv_color_t selected_bar_low_custom_color = {0};
static lv_obj_t *bar_high_color_wheel_popup = NULL;
static lv_obj_t *bar_high_color_wheel = NULL;
static uint8_t bar_high_color_value_id = 0;
static lv_color_t selected_bar_high_custom_color = {0};
static lv_obj_t *bar_in_range_color_wheel_popup = NULL;
static lv_obj_t *bar_in_range_color_wheel = NULL;
static uint8_t bar_in_range_color_value_id = 0;
static lv_color_t selected_bar_in_range_custom_color = {0};

void bar_range_input_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		const char *txt = lv_textarea_get_text(textarea);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);

		bool is_min = lv_obj_get_user_data(textarea) !=
					  NULL; // Check if this is min input
		int32_t value = atoi(txt);

		if (value_id == BAR1_VALUE_ID) {
			if (is_min) {
				values_config[value_id - 1].bar_min = value;
				lv_bar_set_range(ui_Bar_1, value,
								 values_config[value_id - 1].bar_max);
			} else {
				values_config[value_id - 1].bar_max = value;
				lv_bar_set_range(ui_Bar_1, values_config[value_id - 1].bar_min,
								 value);
			}
		} else if (value_id == BAR2_VALUE_ID) {
			if (is_min) {
				values_config[value_id - 1].bar_min = value;
				lv_bar_set_range(ui_Bar_2, value,
								 values_config[value_id - 1].bar_max);
			} else {
				values_config[value_id - 1].bar_max = value;
				lv_bar_set_range(ui_Bar_2, values_config[value_id - 1].bar_min,
								 value);
			}
		}
	}
}

void bar_low_value_event_cb(lv_event_t *e) {
	lv_obj_t *ta = lv_event_get_target(e);
	const char *txt = lv_textarea_get_text(ta);
	int low_val = atoi(txt);

	// Retrieve value_id from the event's user data
	uint8_t *id_ptr = lv_event_get_user_data(e);
	uint8_t value_id = *id_ptr;

	// Update the configuration structure (make sure 'bar_low' is a valid field)
	values_config[value_id - 1].bar_low = low_val;

	// Retrieve the preview bar pointer (stored in the config_bars[] global
	// array)
	lv_obj_t *menu_bar = config_bars[value_id - 1];
	if (menu_bar) {
		int current_val = lv_bar_get_value(menu_bar);
		if (current_val < low_val) {
			// Use configured low color
			lv_obj_set_style_bg_color(menu_bar,
									  values_config[value_id - 1].bar_low_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else if (current_val > values_config[value_id - 1].bar_high) {
			// Use configured high color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_high_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else {
			// Use configured in-range color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_in_range_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
	}
}

void bar_high_value_event_cb(lv_event_t *e) {
	lv_obj_t *ta = lv_event_get_target(e);
	const char *txt = lv_textarea_get_text(ta);
	int high_val = atoi(txt);

	// Retrieve value_id from the event's user data
	uint8_t *id_ptr = lv_event_get_user_data(e);
	uint8_t value_id = *id_ptr;

	// Update the configuration structure with the new bar high threshold
	values_config[value_id - 1].bar_high = high_val;

	// Retrieve the preview bar pointer from the global config_bars array
	lv_obj_t *menu_bar = config_bars[value_id - 1];
	if (menu_bar) {
		int current_val = lv_bar_get_value(menu_bar);
		if (current_val < values_config[value_id - 1].bar_low) {
			// Use configured low color
			lv_obj_set_style_bg_color(menu_bar,
									  values_config[value_id - 1].bar_low_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else if (current_val > high_val) {
			// Use configured high color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_high_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else {
			// Use configured in-range color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_in_range_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
	}
}

// Forward declaration for color wheel popup
void create_rpm_color_wheel_popup(void);
void create_limiter_color_wheel_popup(void);

void bar_low_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_BLUE_DARK;
		break; // Blue
	case 1:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_RED;
		break; // Red
	case 2:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_GREEN_BRIGHT;
		break; // Green
	case 3:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 4:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 5:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 6:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_CYAN;
		break; // Cyan
	case 7:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:	   // Custom color - open color wheel popup
		create_bar_low_color_wheel_popup(value_id);
		return; // Don't update color yet, wait for color wheel selection
	}
}

void bar_high_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_BLUE_DARK;
		break; // Blue
	case 1:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_RED;
		break; // Red
	case 2:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_GREEN_BRIGHT;
		break; // Green
	case 3:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 4:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 5:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 6:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_CYAN;
		break; // Cyan
	case 7:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:	   // Custom color - open color wheel popup
		create_bar_high_color_wheel_popup(value_id);
		return; // Don't update color yet, wait for color wheel selection
	}
}

void bar_in_range_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_BLUE_DARK;
		break; // Blue
	case 1:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_RED;
		break; // Red
	case 2:
		values_config[value_id - 1].bar_in_range_color =
			THEME_COLOR_GREEN_BRIGHT;
		break; // Green
	case 3:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 4:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 5:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 6:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_CYAN;
		break; // Cyan
	case 7:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:	   // Custom color - open color wheel popup
		create_bar_in_range_color_wheel_popup(value_id);
		return; // Don't update color yet, wait for color wheel selection
	}
}

// Limiter circles color update function removed - only bar flash effect is
// supported

static void bar_low_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[bar_low_color_value_id - 1].bar_low_color =
		selected_bar_low_custom_color;

	// Close the popup
	if (bar_low_color_wheel_popup) {
		lv_obj_del(bar_low_color_wheel_popup);
		bar_low_color_wheel_popup = NULL;
		bar_low_color_wheel = NULL;
	}
}

static void bar_low_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (bar_low_color_wheel_popup) {
		lv_obj_del(bar_low_color_wheel_popup);
		bar_low_color_wheel_popup = NULL;
		bar_low_color_wheel = NULL;
	}
}

static void bar_low_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_bar_low_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

static void bar_high_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[bar_high_color_value_id - 1].bar_high_color =
		selected_bar_high_custom_color;

	// Close the popup
	if (bar_high_color_wheel_popup) {
		lv_obj_del(bar_high_color_wheel_popup);
		bar_high_color_wheel_popup = NULL;
		bar_high_color_wheel = NULL;
	}
}

static void bar_high_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (bar_high_color_wheel_popup) {
		lv_obj_del(bar_high_color_wheel_popup);
		bar_high_color_wheel_popup = NULL;
		bar_high_color_wheel = NULL;
	}
}

static void bar_high_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_bar_high_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

static void bar_in_range_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[bar_in_range_color_value_id - 1].bar_in_range_color =
		selected_bar_in_range_custom_color;

	// Close the popup
	if (bar_in_range_color_wheel_popup) {
		lv_obj_del(bar_in_range_color_wheel_popup);
		bar_in_range_color_wheel_popup = NULL;
		bar_in_range_color_wheel = NULL;
	}
}

static void bar_in_range_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (bar_in_range_color_wheel_popup) {
		lv_obj_del(bar_in_range_color_wheel_popup);
		bar_in_range_color_wheel_popup = NULL;
		bar_in_range_color_wheel = NULL;
	}
}

static void bar_in_range_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_bar_in_range_custom_color = lv_colorwheel_get_rgb(colorwheel);
}
void create_bar_low_color_wheel_popup(uint8_t value_id) {
	// Don't create multiple popups
	if (bar_low_color_wheel_popup)
		return;

	// Store the value ID for the callback
	bar_low_color_value_id = value_id;

	// Create popup background
	bar_low_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(bar_low_color_wheel_popup, 400, 350);
	lv_obj_center(bar_low_color_wheel_popup);
	lv_obj_set_style_bg_color(bar_low_color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(bar_low_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar_low_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar_low_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(bar_low_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(bar_low_color_wheel_popup, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(bar_low_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(bar_low_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Bar Low Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
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
	lv_obj_add_event_cb(bar_low_color_wheel,
						bar_low_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(bar_low_color_wheel_popup);
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

	lv_obj_add_event_cb(ok_btn, bar_low_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(bar_low_color_wheel_popup);
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

	lv_obj_add_event_cb(cancel_btn, bar_low_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void create_bar_high_color_wheel_popup(uint8_t value_id) {
	// Don't create multiple popups
	if (bar_high_color_wheel_popup)
		return;

	// Store the value ID for the callback
	bar_high_color_value_id = value_id;

	// Create popup background
	bar_high_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(bar_high_color_wheel_popup, 400, 350);
	lv_obj_center(bar_high_color_wheel_popup);
	lv_obj_set_style_bg_color(bar_high_color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(bar_high_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar_high_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar_high_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(bar_high_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(bar_high_color_wheel_popup, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(bar_high_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(bar_high_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Bar High Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	bar_high_color_wheel =
		lv_colorwheel_create(bar_high_color_wheel_popup, true);
	lv_obj_set_size(bar_high_color_wheel, 200, 200);
	lv_obj_align(bar_high_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current bar high color
	lv_color_t current_color = values_config[value_id - 1].bar_high_color;
	lv_colorwheel_set_rgb(bar_high_color_wheel, current_color);
	selected_bar_high_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(bar_high_color_wheel,
						bar_high_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(bar_high_color_wheel_popup);
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

	lv_obj_add_event_cb(ok_btn, bar_high_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(bar_high_color_wheel_popup);
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

	lv_obj_add_event_cb(cancel_btn, bar_high_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void create_bar_in_range_color_wheel_popup(uint8_t value_id) {
	// Don't create multiple popups
	if (bar_in_range_color_wheel_popup)
		return;

	// Store the value ID for the callback
	bar_in_range_color_value_id = value_id;

	// Create popup background
	bar_in_range_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(bar_in_range_color_wheel_popup, 400, 350);
	lv_obj_center(bar_in_range_color_wheel_popup);
	lv_obj_set_style_bg_color(bar_in_range_color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(bar_in_range_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar_in_range_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar_in_range_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(bar_in_range_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(bar_in_range_color_wheel_popup,
								  THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(bar_in_range_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(bar_in_range_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Bar In-Range Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	bar_in_range_color_wheel =
		lv_colorwheel_create(bar_in_range_color_wheel_popup, true);
	lv_obj_set_size(bar_in_range_color_wheel, 200, 200);
	lv_obj_align(bar_in_range_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current bar in-range color
	lv_color_t current_color = values_config[value_id - 1].bar_in_range_color;
	lv_colorwheel_set_rgb(bar_in_range_color_wheel, current_color);
	selected_bar_in_range_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(bar_in_range_color_wheel,
						bar_in_range_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(bar_in_range_color_wheel_popup);
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

	lv_obj_add_event_cb(ok_btn, bar_in_range_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(bar_in_range_color_wheel_popup);
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

	lv_obj_add_event_cb(cancel_btn, bar_in_range_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void update_bar_ui(void *param) {
	bar_update_t *upd = (bar_update_t *)param;
	// Select the appropriate bar object (assuming bar_index 0 means ui_Bar_1
	// and 1 means ui_Bar_2)
	lv_obj_t *bar_obj = (upd->bar_index == 0) ? ui_Bar_1 : ui_Bar_2;

	// Check if the bar object is still valid.
	if (bar_obj == NULL || !lv_obj_is_valid(bar_obj) ||
		lv_obj_get_screen(bar_obj) == NULL) {
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

	lv_obj_set_style_bg_color(bar_obj, new_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Also update menu preview bar if it exists, is valid, and menu is visible
	lv_obj_t *menu_bar = menu_bar_objects[upd->bar_index];
	if (menu_bar && lv_obj_is_valid(menu_bar) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_bar_set_value(menu_bar, upd->bar_value, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(menu_bar, new_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}

	// Update Bar numeric value displays (only if show_bar_value is enabled)
	if (upd->bar_index == 0 && ui_Bar_1_Value &&
		lv_obj_is_valid(ui_Bar_1_Value) &&
		values_config[upd->config_index].show_bar_value) {
		char value_str[16];
		if (upd->is_timeout) {
			strcpy(value_str, "---");
		} else {
			// Format the value based on decimals configuration
			int decimals = values_config[upd->config_index].decimals;
			if (decimals == 0) {
				snprintf(value_str, sizeof(value_str), "%d",
						 (int)upd->final_value);
			} else {
				snprintf(value_str, sizeof(value_str), "%.*f", decimals,
						 upd->final_value);
			}
		}
		lv_label_set_text(ui_Bar_1_Value, value_str);
	} else if (upd->bar_index == 1 && ui_Bar_2_Value &&
			   lv_obj_is_valid(ui_Bar_2_Value) &&
			   values_config[upd->config_index].show_bar_value) {
		char value_str[16];
		if (upd->is_timeout) {
			strcpy(value_str, "---");
		} else {
			// Format the value based on decimals configuration
			int decimals = values_config[upd->config_index].decimals;
			if (decimals == 0) {
				snprintf(value_str, sizeof(value_str), "%d",
						 (int)upd->final_value);
			} else {
				snprintf(value_str, sizeof(value_str), "%.*f", decimals,
						 upd->final_value);
			}
		}
		lv_label_set_text(ui_Bar_2_Value, value_str);
	}

	free(upd);
}

// Immediate (no-alloc, no-async) bar update
void update_bar_ui_immediate(int bar_index, int32_t bar_value,
							 double final_value, int config_index) {
	lv_obj_t *bar_obj = (bar_index == 0) ? ui_Bar_1 : ui_Bar_2;
	if (bar_obj == NULL || !lv_obj_is_valid(bar_obj) ||
		lv_obj_get_screen(bar_obj) == NULL) {
		return;
	}
	lv_bar_set_value(bar_obj, bar_value, LV_ANIM_OFF);
	lv_color_t new_color;
	if (final_value < values_config[config_index].bar_low) {
		new_color = values_config[config_index].bar_low_color;
	} else if (final_value > values_config[config_index].bar_high) {
		new_color = values_config[config_index].bar_high_color;
	} else {
		new_color = values_config[config_index].bar_in_range_color;
	}
	lv_obj_set_style_bg_color(bar_obj, new_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Update Bar numeric value displays (only if show_bar_value is enabled)
	if (bar_index == 0 && ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value) &&
		values_config[config_index].show_bar_value) {
		char value_str[16];
		// Format the value based on decimals configuration
		int decimals = values_config[config_index].decimals;
		if (decimals == 0) {
			snprintf(value_str, sizeof(value_str), "%d", (int)final_value);
		} else {
			snprintf(value_str, sizeof(value_str), "%.*f", decimals,
					 final_value);
		}
		lv_label_set_text(ui_Bar_1_Value, value_str);
	} else if (bar_index == 1 && ui_Bar_2_Value &&
			   lv_obj_is_valid(ui_Bar_2_Value) &&
			   values_config[config_index].show_bar_value) {
		char value_str[16];
		// Format the value based on decimals configuration
		int decimals = values_config[config_index].decimals;
		if (decimals == 0) {
			snprintf(value_str, sizeof(value_str), "%d", (int)final_value);
		} else {
			snprintf(value_str, sizeof(value_str), "%.*f", decimals,
					 final_value);
		}
		lv_label_set_text(ui_Bar_2_Value, value_str);
	}

	lv_obj_t *menu_bar = menu_bar_objects[bar_index];
	if (menu_bar && lv_obj_is_valid(menu_bar) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_bar_set_value(menu_bar, bar_value, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(menu_bar, new_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}
}

void widget_bar_create(lv_obj_t *parent) {
	if (values_config[BAR1_VALUE_ID - 1].bar_max <=
		values_config[BAR1_VALUE_ID - 1].bar_min) {
		values_config[BAR1_VALUE_ID - 1].bar_min = 0;
		values_config[BAR1_VALUE_ID - 1].bar_max = 100;
	}
	ui_Bar_1 = lv_bar_create(parent);
	lv_bar_set_range(ui_Bar_1, values_config[BAR1_VALUE_ID - 1].bar_min,
					 values_config[BAR1_VALUE_ID - 1].bar_max);
	lv_bar_set_value(ui_Bar_1, values_config[BAR1_VALUE_ID - 1].bar_min,
					 LV_ANIM_OFF);
	lv_obj_set_width(ui_Bar_1, 300);
	lv_obj_set_height(ui_Bar_1, 30);
	lv_obj_set_x(ui_Bar_1, -240);
	lv_obj_set_y(ui_Bar_1, 209);
	lv_obj_set_align(ui_Bar_1, LV_ALIGN_CENTER);
	lv_obj_set_style_radius(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_1, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_Bar_1, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(ui_Bar_1, THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Bar_1, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_1, THEME_COLOR_GREEN_BRIGHT,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_1, 255,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);

	ui_Bar_1_Label = lv_label_create(parent);
	lv_obj_set_x(ui_Bar_1_Label, -240);
	lv_obj_set_y(ui_Bar_1_Label, 181);
	lv_obj_set_align(ui_Bar_1_Label, LV_ALIGN_CENTER);
	lv_label_set_text(ui_Bar_1_Label, label_texts[BAR1_VALUE_ID - 1]);
	lv_obj_set_style_text_color(ui_Bar_1_Label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_1_Label, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Bar_1_Value = lv_label_create(parent);
	lv_obj_set_width(ui_Bar_1_Value, 80);
	lv_obj_set_height(ui_Bar_1_Value, LV_SIZE_CONTENT);
	lv_obj_set_align(ui_Bar_1_Value, LV_ALIGN_CENTER);
	lv_obj_set_x(ui_Bar_1_Value, -140);
	lv_obj_set_y(ui_Bar_1_Value, 181);
	lv_label_set_text(ui_Bar_1_Value, "---");
	lv_obj_set_style_text_color(ui_Bar_1_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_1_Value, THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_Bar_1_Value, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	if (!values_config[BAR1_VALUE_ID - 1].show_bar_value)
		lv_obj_add_flag(ui_Bar_1_Value, LV_OBJ_FLAG_HIDDEN);

	if (values_config[BAR2_VALUE_ID - 1].bar_max <=
		values_config[BAR2_VALUE_ID - 1].bar_min) {
		values_config[BAR2_VALUE_ID - 1].bar_min = 0;
		values_config[BAR2_VALUE_ID - 1].bar_max = 100;
	}
	ui_Bar_2 = lv_bar_create(parent);
	lv_bar_set_range(ui_Bar_2, values_config[BAR2_VALUE_ID - 1].bar_min,
					 values_config[BAR2_VALUE_ID - 1].bar_max);
	lv_bar_set_value(ui_Bar_2, values_config[BAR2_VALUE_ID - 1].bar_min,
					 LV_ANIM_OFF);
	lv_obj_set_width(ui_Bar_2, 300);
	lv_obj_set_height(ui_Bar_2, 30);
	lv_obj_set_x(ui_Bar_2, 240);
	lv_obj_set_y(ui_Bar_2, 209);
	lv_obj_set_align(ui_Bar_2, LV_ALIGN_CENTER);
	lv_obj_set_style_radius(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_2, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_Bar_2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(ui_Bar_2, THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Bar_2, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_2, THEME_COLOR_GREEN_BRIGHT,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_2, 255,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);

	ui_Bar_2_Label = lv_label_create(parent);
	lv_obj_set_x(ui_Bar_2_Label, 240);
	lv_obj_set_y(ui_Bar_2_Label, 181);
	lv_obj_set_align(ui_Bar_2_Label, LV_ALIGN_CENTER);
	lv_label_set_text(ui_Bar_2_Label, label_texts[BAR2_VALUE_ID - 1]);
	lv_obj_set_style_text_color(ui_Bar_2_Label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_2_Label, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Bar_2_Value = lv_label_create(parent);
	lv_obj_set_width(ui_Bar_2_Value, 80);
	lv_obj_set_height(ui_Bar_2_Value, LV_SIZE_CONTENT);
	lv_obj_set_align(ui_Bar_2_Value, LV_ALIGN_CENTER);
	lv_obj_set_x(ui_Bar_2_Value, 340);
	lv_obj_set_y(ui_Bar_2_Value, 181);
	lv_label_set_text(ui_Bar_2_Value, "---");
	lv_obj_set_style_text_color(ui_Bar_2_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_2_Value, THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_Bar_2_Value, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	if (!values_config[BAR2_VALUE_ID - 1].show_bar_value)
		lv_obj_add_flag(ui_Bar_2_Value, LV_OBJ_FLAG_HIDDEN);
}

uint64_t *widget_bar_get_last_can_time(uint8_t bar_idx) {
	return &last_bar_can_received[bar_idx & 1];
}

/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

/* bar_create() creates both BAR1 and BAR2 together; slot 0 triggers creation.
 */
static void _bar_create(widget_t *w, lv_obj_t *parent) {
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	if (slot == 0) {
		widget_bar_create(parent);
	}
	w->root = (slot == 0) ? ui_Bar_1 : ui_Bar_2;
}
static void _bar_update(widget_t *w, void *data) {
	(void)w;
	update_bar_ui(data);
}
static void _bar_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, nw, nh);
	w->w = nw;
	w->h = nh;
}
static void _bar_open_settings(widget_t *w) { (void)w; }
static void _bar_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
	uint8_t vid = (slot == 0) ? BAR1_VALUE_ID : BAR2_VALUE_ID;
	uint8_t idx = vid - 1;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	cJSON_AddNumberToObject(cfg, "slot", slot);
	cJSON_AddNumberToObject(cfg, "can_id", values_config[idx].can_id);
	cJSON_AddNumberToObject(cfg, "bar_min", values_config[idx].bar_min);
	cJSON_AddNumberToObject(cfg, "bar_max", values_config[idx].bar_max);
}
static void _bar_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
}
static void _bar_destroy(widget_t *w) { free(w); }

/* Default x offsets from screen centre for BAR1 and BAR2 */
static const int16_t s_bar_default_x[2] = {-240, 240};

widget_t *widget_bar_create_instance(uint8_t slot) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	w->type = WIDGET_BAR;
	w->x = s_bar_default_x[slot & 1];
	w->y = 209;
	w->w = 300;
	w->h = 30;
	w->type_data = (void *)(uintptr_t)(slot & 1);
	snprintf(w->id, sizeof(w->id), "bar_%u", slot & 1);

	w->create = _bar_create;
	w->update = _bar_update;
	w->resize = _bar_resize;
	w->open_settings = _bar_open_settings;
	w->to_json = _bar_to_json;
	w->from_json = _bar_from_json;
	w->destroy = _bar_destroy;

	return w;
}
