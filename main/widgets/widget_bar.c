#include "widget_bar.h"
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
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers: look up bar_data_t by slot or value_id ──────────────────────── */
static bar_data_t *_get_bar_data_by_slot(uint8_t slot) {
	if (slot >= 2) return NULL;
	widget_t **widgets = dashboard_get_widgets();
	uint8_t count = dashboard_get_widget_count();
	for (uint8_t i = 0; i < count; i++) {
		if (widgets[i] && widgets[i]->type == WIDGET_BAR) {
			bar_data_t *bd = (bar_data_t *)widgets[i]->type_data;
			if (bd && bd->slot == slot) return bd;
		}
	}
	return NULL;
}

static bar_data_t *_get_bar_data_by_value_id(uint8_t value_id) {
	uint8_t slot = (value_id == BAR1_VALUE_ID) ? 0 : 1;
	return _get_bar_data_by_slot(slot);
}

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
		bar_data_t *bd = _get_bar_data_by_value_id(value_id);
		if (!bd) return;

		bool is_min = lv_obj_get_user_data(textarea) != NULL;
		int32_t value = atoi(txt);
		lv_obj_t *bar_obj = (value_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;

		if (is_min) {
			bd->bar_min = value;
			lv_bar_set_range(bar_obj, value, bd->bar_max);
		} else {
			bd->bar_max = value;
			lv_bar_set_range(bar_obj, bd->bar_min, value);
		}
	}
}

void bar_low_value_event_cb(lv_event_t *e) {
	lv_obj_t *ta = lv_event_get_target(e);
	const char *txt = lv_textarea_get_text(ta);
	int low_val = atoi(txt);

	uint8_t *id_ptr = lv_event_get_user_data(e);
	uint8_t value_id = *id_ptr;
	bar_data_t *bd = _get_bar_data_by_value_id(value_id);
	if (!bd) return;

	bd->bar_low = low_val;

	lv_obj_t *menu_bar = config_bars[value_id - 1];
	if (menu_bar) {
		int current_val = lv_bar_get_value(menu_bar);
		if (current_val < low_val) {
			lv_obj_set_style_bg_color(menu_bar, bd->bar_low_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else if (current_val > bd->bar_high) {
			lv_obj_set_style_bg_color(menu_bar, bd->bar_high_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_bg_color(menu_bar, bd->bar_in_range_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
	}
}

void bar_high_value_event_cb(lv_event_t *e) {
	lv_obj_t *ta = lv_event_get_target(e);
	const char *txt = lv_textarea_get_text(ta);
	int high_val = atoi(txt);

	uint8_t *id_ptr = lv_event_get_user_data(e);
	uint8_t value_id = *id_ptr;
	bar_data_t *bd = _get_bar_data_by_value_id(value_id);
	if (!bd) return;

	bd->bar_high = high_val;

	lv_obj_t *menu_bar = config_bars[value_id - 1];
	if (menu_bar) {
		int current_val = lv_bar_get_value(menu_bar);
		if (current_val < bd->bar_low) {
			lv_obj_set_style_bg_color(menu_bar, bd->bar_low_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else if (current_val > high_val) {
			lv_obj_set_style_bg_color(menu_bar, bd->bar_high_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_bg_color(menu_bar, bd->bar_in_range_color,
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
	bar_data_t *bd = _get_bar_data_by_value_id(value_id);
	if (!bd) return;
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0: bd->bar_low_color = THEME_COLOR_BLUE_DARK; break;
	case 1: bd->bar_low_color = THEME_COLOR_RED; break;
	case 2: bd->bar_low_color = THEME_COLOR_GREEN_BRIGHT; break;
	case 3: bd->bar_low_color = THEME_COLOR_YELLOW; break;
	case 4: bd->bar_low_color = THEME_COLOR_ORANGE; break;
	case 5: bd->bar_low_color = THEME_COLOR_PURPLE; break;
	case 6: bd->bar_low_color = THEME_COLOR_CYAN; break;
	case 7: bd->bar_low_color = THEME_COLOR_MAGENTA; break;
	case 8:
		create_bar_low_color_wheel_popup(value_id);
		return;
	}
}

void bar_high_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	bar_data_t *bd = _get_bar_data_by_value_id(value_id);
	if (!bd) return;
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0: bd->bar_high_color = THEME_COLOR_BLUE_DARK; break;
	case 1: bd->bar_high_color = THEME_COLOR_RED; break;
	case 2: bd->bar_high_color = THEME_COLOR_GREEN_BRIGHT; break;
	case 3: bd->bar_high_color = THEME_COLOR_YELLOW; break;
	case 4: bd->bar_high_color = THEME_COLOR_ORANGE; break;
	case 5: bd->bar_high_color = THEME_COLOR_PURPLE; break;
	case 6: bd->bar_high_color = THEME_COLOR_CYAN; break;
	case 7: bd->bar_high_color = THEME_COLOR_MAGENTA; break;
	case 8:
		create_bar_high_color_wheel_popup(value_id);
		return;
	}
}

void bar_in_range_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	bar_data_t *bd = _get_bar_data_by_value_id(value_id);
	if (!bd) return;
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0: bd->bar_in_range_color = THEME_COLOR_BLUE_DARK; break;
	case 1: bd->bar_in_range_color = THEME_COLOR_RED; break;
	case 2: bd->bar_in_range_color = THEME_COLOR_GREEN_BRIGHT; break;
	case 3: bd->bar_in_range_color = THEME_COLOR_YELLOW; break;
	case 4: bd->bar_in_range_color = THEME_COLOR_ORANGE; break;
	case 5: bd->bar_in_range_color = THEME_COLOR_PURPLE; break;
	case 6: bd->bar_in_range_color = THEME_COLOR_CYAN; break;
	case 7: bd->bar_in_range_color = THEME_COLOR_MAGENTA; break;
	case 8:
		create_bar_in_range_color_wheel_popup(value_id);
		return;
	}
}

// Limiter circles color update function removed - only bar flash effect is
// supported

static void bar_low_color_wheel_ok_event_cb(lv_event_t *e) {
	bar_data_t *bd = _get_bar_data_by_value_id(bar_low_color_value_id);
	if (bd) bd->bar_low_color = selected_bar_low_custom_color;

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
	bar_data_t *bd = _get_bar_data_by_value_id(bar_high_color_value_id);
	if (bd) bd->bar_high_color = selected_bar_high_custom_color;

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
	bar_data_t *bd = _get_bar_data_by_value_id(bar_in_range_color_value_id);
	if (bd) bd->bar_in_range_color = selected_bar_in_range_custom_color;

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
	bar_data_t *bd_low = _get_bar_data_by_value_id(value_id);
	lv_color_t current_color = bd_low ? bd_low->bar_low_color : THEME_COLOR_BLUE_DARK;
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
	bar_data_t *bd_high = _get_bar_data_by_value_id(value_id);
	lv_color_t current_color = bd_high ? bd_high->bar_high_color : THEME_COLOR_RED;
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
	bar_data_t *bd_ir = _get_bar_data_by_value_id(value_id);
	lv_color_t current_color = bd_ir ? bd_ir->bar_in_range_color : THEME_COLOR_GREEN_BRIGHT;
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
	lv_obj_t *bar_obj = (upd->bar_index == 0) ? ui_Bar_1 : ui_Bar_2;

	if (bar_obj == NULL || !lv_obj_is_valid(bar_obj) ||
		lv_obj_get_screen(bar_obj) == NULL) {
		free(upd);
		return;
	}

	bar_data_t *bd = _get_bar_data_by_slot(upd->bar_index);
	lv_bar_set_value(bar_obj, upd->bar_value, LV_ANIM_OFF);

	lv_color_t new_color;
	if (bd) {
		if (upd->final_value < bd->bar_low) {
			new_color = bd->bar_low_color;
		} else if (upd->final_value > bd->bar_high) {
			new_color = bd->bar_high_color;
		} else {
			new_color = bd->bar_in_range_color;
		}
	} else {
		new_color = THEME_COLOR_GREEN_BRIGHT;
	}

	lv_obj_set_style_bg_color(bar_obj, new_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);

	lv_obj_t *menu_bar = menu_bar_objects[upd->bar_index];
	if (menu_bar && lv_obj_is_valid(menu_bar) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_bar_set_value(menu_bar, upd->bar_value, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(menu_bar, new_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}

	bool show_val = bd ? bd->show_bar_value : false;
	int decimals = bd ? bd->decimals : 0;
	lv_obj_t *val_label = (upd->bar_index == 0) ? ui_Bar_1_Value : ui_Bar_2_Value;
	if (val_label && lv_obj_is_valid(val_label) && show_val) {
		char value_str[16];
		if (upd->is_timeout) {
			strcpy(value_str, "---");
		} else {
			if (decimals == 0) {
				snprintf(value_str, sizeof(value_str), "%d", (int)upd->final_value);
			} else {
				snprintf(value_str, sizeof(value_str), "%.*f", decimals, upd->final_value);
			}
		}
		lv_label_set_text(val_label, value_str);
	}

	free(upd);
}

// Immediate (no-alloc, no-async) bar update
void update_bar_ui_immediate(int bar_index, int32_t bar_value,
							 double final_value, int config_index) {
	(void)config_index; /* legacy parameter — now unused */
	lv_obj_t *bar_obj = (bar_index == 0) ? ui_Bar_1 : ui_Bar_2;
	if (bar_obj == NULL || !lv_obj_is_valid(bar_obj) ||
		lv_obj_get_screen(bar_obj) == NULL) {
		return;
	}
	bar_data_t *bd = _get_bar_data_by_slot((uint8_t)bar_index);
	lv_bar_set_value(bar_obj, bar_value, LV_ANIM_OFF);
	lv_color_t new_color;
	if (bd) {
		if (final_value < bd->bar_low) {
			new_color = bd->bar_low_color;
		} else if (final_value > bd->bar_high) {
			new_color = bd->bar_high_color;
		} else {
			new_color = bd->bar_in_range_color;
		}
	} else {
		new_color = THEME_COLOR_GREEN_BRIGHT;
	}
	lv_obj_set_style_bg_color(bar_obj, new_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);

	bool show_val = bd ? bd->show_bar_value : false;
	int decimals = bd ? bd->decimals : 0;
	lv_obj_t *val_label = (bar_index == 0) ? ui_Bar_1_Value : ui_Bar_2_Value;
	if (val_label && lv_obj_is_valid(val_label) && show_val) {
		char value_str[16];
		if (decimals == 0) {
			snprintf(value_str, sizeof(value_str), "%d", (int)final_value);
		} else {
			snprintf(value_str, sizeof(value_str), "%.*f", decimals, final_value);
		}
		lv_label_set_text(val_label, value_str);
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
	bar_data_t *bd1 = _get_bar_data_by_slot(0);
	int32_t b1_min = bd1 ? bd1->bar_min : 0;
	int32_t b1_max = bd1 ? bd1->bar_max : 100;
	if (b1_max <= b1_min) { b1_min = 0; b1_max = 100; }
	ui_Bar_1 = lv_bar_create(parent);
	lv_bar_set_range(ui_Bar_1, b1_min, b1_max);
	lv_bar_set_value(ui_Bar_1, b1_min, LV_ANIM_OFF);
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
	lv_label_set_text(ui_Bar_1_Label, (bd1 && bd1->label[0]) ? bd1->label : "BAR1");
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
	if (!(bd1 && bd1->show_bar_value))
		lv_obj_add_flag(ui_Bar_1_Value, LV_OBJ_FLAG_HIDDEN);

	bar_data_t *bd2 = _get_bar_data_by_slot(1);
	int32_t b2_min = bd2 ? bd2->bar_min : 0;
	int32_t b2_max = bd2 ? bd2->bar_max : 100;
	if (b2_max <= b2_min) { b2_min = 0; b2_max = 100; }
	ui_Bar_2 = lv_bar_create(parent);
	lv_bar_set_range(ui_Bar_2, b2_min, b2_max);
	lv_bar_set_value(ui_Bar_2, b2_min, LV_ANIM_OFF);
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
	lv_label_set_text(ui_Bar_2_Label, (bd2 && bd2->label[0]) ? bd2->label : "BAR2");
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
	if (!(bd2 && bd2->show_bar_value))
		lv_obj_add_flag(ui_Bar_2_Value, LV_OBJ_FLAG_HIDDEN);
}

uint64_t *widget_bar_get_last_can_time(uint8_t bar_idx) {
	return &last_bar_can_received[bar_idx & 1];
}

/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

static void _bar_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (!bd) return;

	double final_value = is_stale ? 0.0 : (double)value;
	int32_t bar_value = is_stale ? 0 : (int32_t)value;

	/* Update this widget's own bar via per-instance pointer */
	if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj)) {
		lv_bar_set_value(bd->bar_obj, bar_value, LV_ANIM_OFF);
		lv_color_t new_color;
		if (final_value < bd->bar_low)
			new_color = bd->bar_low_color;
		else if (final_value > bd->bar_high)
			new_color = bd->bar_high_color;
		else
			new_color = bd->bar_in_range_color;
		lv_obj_set_style_bg_color(bd->bar_obj, new_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}

	/* Update this widget's own value label */
	if (bd->value_obj && lv_obj_is_valid(bd->value_obj) && bd->show_bar_value) {
		char value_str[16];
		if (is_stale) {
			strcpy(value_str, "---");
		} else if (bd->decimals == 0) {
			snprintf(value_str, sizeof(value_str), "%d", (int)final_value);
		} else {
			snprintf(value_str, sizeof(value_str), "%.*f", bd->decimals, final_value);
		}
		lv_label_set_text(bd->value_obj, value_str);
	}

	/* Update menu bar preview if visible */
	uint8_t bar_index = bd->slot;
	if (bar_index < 2) {
		lv_obj_t *menu_bar = menu_bar_objects[bar_index];
		if (menu_bar && lv_obj_is_valid(menu_bar) && ui_MenuScreen &&
			lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
			lv_bar_set_value(menu_bar, bar_value, LV_ANIM_OFF);
			lv_color_t mc;
			if (final_value < bd->bar_low) mc = bd->bar_low_color;
			else if (final_value > bd->bar_high) mc = bd->bar_high_color;
			else mc = bd->bar_in_range_color;
			lv_obj_set_style_bg_color(menu_bar, mc,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
	}
}

/* ── _bar_create: create a single bar per slot, positioned by layout ──────── */
static void _bar_create(widget_t *w, lv_obj_t *parent) {
	bar_data_t *bd = (bar_data_t *)w->type_data;
	uint8_t slot = bd ? bd->slot : 0;

	int32_t b_min = bd ? bd->bar_min : 0;
	int32_t b_max = bd ? bd->bar_max : 100;
	if (b_max <= b_min) { b_min = 0; b_max = 100; }

	/* Create the bar LVGL object */
	lv_obj_t *bar = lv_bar_create(parent);
	lv_bar_set_range(bar, b_min, b_max);
	lv_bar_set_value(bar, b_min, LV_ANIM_OFF);
	lv_obj_set_width(bar, w->w);
	lv_obj_set_height(bar, w->h);
	lv_obj_set_align(bar, LV_ALIGN_CENTER);
	lv_obj_set_pos(bar, w->x, w->y);
	lv_obj_set_style_radius(bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(bar, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(bar, THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(bar, THEME_COLOR_GREEN_BRIGHT,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(bar, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

	/* Create the label above the bar */
	lv_obj_t *lbl = lv_label_create(parent);
	lv_obj_set_align(lbl, LV_ALIGN_CENTER);
	lv_obj_set_pos(lbl, w->x, w->y - 28);
	lv_label_set_text(lbl, (bd && bd->label[0]) ? bd->label : (slot == 0 ? "BAR1" : "BAR2"));
	lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(lbl, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Create the value label to the right of the bar */
	lv_obj_t *val = lv_label_create(parent);
	lv_obj_set_width(val, 80);
	lv_obj_set_height(val, LV_SIZE_CONTENT);
	lv_obj_set_align(val, LV_ALIGN_CENTER);
	lv_obj_set_pos(val, w->x + (w->w / 2) + 50, w->y - 28);
	lv_label_set_text(val, "---");
	lv_obj_set_style_text_color(val, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(val, THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	if (!(bd && bd->show_bar_value))
		lv_obj_add_flag(val, LV_OBJ_FLAG_HIDDEN);

	/* Store per-instance pointers for signal callback */
	if (bd) {
		bd->bar_obj = bar;
		bd->label_obj = lbl;
		bd->value_obj = val;
	}

	/* Assign to slot globals so existing code (RPM limiter, callbacks) works */
	if (slot == 0) {
		ui_Bar_1 = bar;
		ui_Bar_1_Label = lbl;
		ui_Bar_1_Value = val;
	} else {
		ui_Bar_2 = bar;
		ui_Bar_2_Label = lbl;
		ui_Bar_2_Value = val;
	}

	w->root = bar;

	/* Subscribe to signal if bound */
	if (bd && bd->signal_index >= 0)
		signal_subscribe(bd->signal_index, _bar_on_signal, w);
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
	bar_data_t *bd = (bar_data_t *)w->type_data;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	if (bd) {
		cJSON_AddNumberToObject(cfg, "slot", bd->slot);
		if (bd->label[0] != '\0')
			cJSON_AddStringToObject(cfg, "label", bd->label);
		cJSON_AddNumberToObject(cfg, "bar_min", bd->bar_min);
		cJSON_AddNumberToObject(cfg, "bar_max", bd->bar_max);
		cJSON_AddNumberToObject(cfg, "bar_low", bd->bar_low);
		cJSON_AddNumberToObject(cfg, "bar_high", bd->bar_high);
		cJSON_AddNumberToObject(cfg, "bar_low_color", (int)bd->bar_low_color.full);
		cJSON_AddNumberToObject(cfg, "bar_high_color", (int)bd->bar_high_color.full);
		cJSON_AddNumberToObject(cfg, "bar_in_range_color", (int)bd->bar_in_range_color.full);
		cJSON_AddBoolToObject(cfg, "show_bar_value", bd->show_bar_value);
		cJSON_AddBoolToObject(cfg, "invert_bar_value", bd->invert_bar_value);
		cJSON_AddBoolToObject(cfg, "fuel_sender", bd->fuel_sender);
		cJSON_AddNumberToObject(cfg, "fuel_sender_empty_v", bd->fuel_sender_empty_v);
		cJSON_AddNumberToObject(cfg, "fuel_sender_full_v", bd->fuel_sender_full_v);
		cJSON_AddNumberToObject(cfg, "fuel_sender_filter", bd->fuel_sender_filter);
		cJSON_AddNumberToObject(cfg, "decimals", bd->decimals);
		if (bd->signal_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "signal_name", bd->signal_name);
	} else {
		cJSON_AddNumberToObject(cfg, "slot", 0);
	}
}
static void _bar_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (!bd) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;
	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	if (cJSON_IsNumber(item)) {
		bd->slot = (uint8_t)item->valueint;
		w->slot = bd->slot;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(bd->label, item->valuestring, sizeof(bd->label) - 1);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_min");
	if (cJSON_IsNumber(item)) bd->bar_min = (int32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_max");
	if (cJSON_IsNumber(item)) bd->bar_max = (int32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_low");
	if (cJSON_IsNumber(item)) bd->bar_low = (int32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_high");
	if (cJSON_IsNumber(item)) bd->bar_high = (int32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_low_color");
	if (cJSON_IsNumber(item)) bd->bar_low_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_high_color");
	if (cJSON_IsNumber(item)) bd->bar_high_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_in_range_color");
	if (cJSON_IsNumber(item)) bd->bar_in_range_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_bar_value");
	if (cJSON_IsBool(item)) bd->show_bar_value = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "invert_bar_value");
	if (cJSON_IsBool(item)) bd->invert_bar_value = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "fuel_sender");
	if (cJSON_IsBool(item)) bd->fuel_sender = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "fuel_sender_empty_v");
	if (cJSON_IsNumber(item)) bd->fuel_sender_empty_v = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "fuel_sender_full_v");
	if (cJSON_IsNumber(item)) bd->fuel_sender_full_v = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "fuel_sender_filter");
	if (cJSON_IsNumber(item)) bd->fuel_sender_filter = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "decimals");
	if (cJSON_IsNumber(item)) bd->decimals = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring) {
		strncpy(bd->signal_name, item->valuestring, sizeof(bd->signal_name) - 1);
		bd->signal_name[sizeof(bd->signal_name) - 1] = '\0';
	}

	/* Resolve signal name → index */
	if (bd->signal_name[0] != '\0')
		bd->signal_index = signal_find_by_name(bd->signal_name);
}
static void _bar_destroy(widget_t *w) {
	free(w->type_data);
	free(w);
}

/* Default x offsets from screen centre for BAR1 and BAR2 */
static const int16_t s_bar_default_x[2] = {-240, 240};

widget_t *widget_bar_create_instance(uint8_t slot) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	bar_data_t *bd = heap_caps_calloc(1, sizeof(bar_data_t), MALLOC_CAP_SPIRAM);
	if (!bd) bd = calloc(1, sizeof(bar_data_t));
	if (!bd) { free(w); return NULL; }

	/* Defaults — actual config comes from _from_json() */
	bd->slot = slot & 1;
	snprintf(bd->label, sizeof(bd->label), "BAR%d", (slot & 1) + 1);
	bd->bar_max = 100;
	bd->bar_in_range_color = THEME_COLOR_GREEN_BRIGHT;
	bd->bar_low_color = THEME_COLOR_BLUE_DARK;
	bd->bar_high_color = THEME_COLOR_RED;
	bd->signal_index = -1;

	w->type = WIDGET_BAR;
	w->slot = slot & 1;
	w->x = s_bar_default_x[slot & 1];
	w->y = 209;
	w->w = 300;
	w->h = 30;
	w->type_data = bd;
	snprintf(w->id, sizeof(w->id), "bar_%u", slot & 1);

	w->create = _bar_create;
	w->resize = _bar_resize;
	w->open_settings = _bar_open_settings;
	w->to_json = _bar_to_json;
	w->from_json = _bar_from_json;
	w->destroy = _bar_destroy;

	return w;
}

uint8_t widget_bar_get_slot(const widget_t *w) {
	if (!w || w->type != WIDGET_BAR || !w->type_data) return 0;
	return ((const bar_data_t *)w->type_data)->slot;
}

bool widget_bar_has_signal(const widget_t *w) {
	if (!w || w->type != WIDGET_BAR || !w->type_data) return false;
	return ((const bar_data_t *)w->type_data)->signal_index >= 0;
}
