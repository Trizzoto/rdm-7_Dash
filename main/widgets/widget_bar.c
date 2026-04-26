#include "widget_bar.h"
#include "widget_image.h"
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

static const char *TAG = "widget_bar";

/* Default x offsets from screen centre for BAR1 and BAR2 (60% of half-width) */
static const int16_t s_bar_default_x[2] = {
	-(int16_t)(SCREEN_ORIGIN_X * 3 / 5),
	 (int16_t)(SCREEN_ORIGIN_X * 3 / 5)
};

/* Helper: returns true when both image names are set (image-based bar mode) */
static inline bool _bar_is_image_mode(const bar_data_t *bd) {
	return bd && bd->bar_image[0] != '\0' && bd->bar_image_full[0] != '\0';
}

/* Decimals drive the bar's *internal* resolution. A user bar_min/bar_max of
 * 0..1 with decimals=2 yields an internal LVGL range of 0..100, so a live
 * value of 0.85 fills 85% of the bar instead of snapping to the 0 or 1 end.
 * Clamped to [0..4] matching the dropdown range in config_modal.c. */
static inline int32_t _bar_resolution_scale(const bar_data_t *bd) {
	if (!bd || bd->decimals == 0) return 1;
	uint8_t d = bd->decimals > 4 ? 4 : bd->decimals;
	int32_t s = 1;
	while (d--) s *= 10;
	return s;
}

void widget_bar_sync_range(bar_data_t *bd) {
	if (!bd || !bd->bar_obj || !lv_obj_is_valid(bd->bar_obj)) return;
	int32_t scale = _bar_resolution_scale(bd);
	int32_t lo = bd->bar_min * scale;
	int32_t hi = bd->bar_max * scale;
	if (hi <= lo) { lo = 0; hi = 100 * scale; }
	lv_bar_set_range(bd->bar_obj, lo, hi);
}

/* ── Helpers: look up bar_data_t by slot or value_id via registry ──────── */
static bar_data_t *_lookup_bar_data(uint8_t slot) {
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_BAR, slot);
	return w ? (bar_data_t *)w->type_data : NULL;
}

static bar_data_t *_lookup_bar_data_by_value_id(uint8_t value_id) {
	uint8_t slot = (value_id == BAR1_VALUE_ID) ? 0 : 1;
	return _lookup_bar_data(slot);
}

uint64_t last_bar_can_received[2] = {0, 0};

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
		bar_data_t *bd = _lookup_bar_data_by_value_id(value_id);
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
	bar_data_t *bd = _lookup_bar_data_by_value_id(value_id);
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
	bar_data_t *bd = _lookup_bar_data_by_value_id(value_id);
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
	bar_data_t *bd = _lookup_bar_data_by_value_id(value_id);
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
	bar_data_t *bd = _lookup_bar_data_by_value_id(value_id);
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
	bar_data_t *bd = _lookup_bar_data_by_value_id(value_id);
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

static void bar_low_color_wheel_ok_event_cb(lv_event_t *e) {
	bar_data_t *bd = _lookup_bar_data_by_value_id(bar_low_color_value_id);
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
	bar_data_t *bd = _lookup_bar_data_by_value_id(bar_high_color_value_id);
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
	bar_data_t *bd = _lookup_bar_data_by_value_id(bar_in_range_color_value_id);
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
	bar_data_t *bd_low = _lookup_bar_data_by_value_id(value_id);
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
	bar_data_t *bd_high = _lookup_bar_data_by_value_id(value_id);
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
	bar_data_t *bd_ir = _lookup_bar_data_by_value_id(value_id);
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

	bar_data_t *bd = _lookup_bar_data(upd->bar_index);
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
	bar_data_t *bd = _lookup_bar_data((uint8_t)bar_index);
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
	bar_data_t *bd1 = _lookup_bar_data(0);
	int32_t s1 = _bar_resolution_scale(bd1);
	int32_t b1_min = (bd1 ? bd1->bar_min : 0)   * s1;
	int32_t b1_max = (bd1 ? bd1->bar_max : 100) * s1;
	if (b1_max <= b1_min) { b1_min = 0; b1_max = 100 * s1; }
	ui_Bar_1 = lv_bar_create(parent);
	lv_bar_set_range(ui_Bar_1, b1_min, b1_max);
	lv_bar_set_value(ui_Bar_1, b1_min, LV_ANIM_OFF);
	lv_obj_set_width(ui_Bar_1, 300);
	lv_obj_set_height(ui_Bar_1, 30);
	lv_obj_set_x(ui_Bar_1, s_bar_default_x[0]);
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
	lv_obj_set_x(ui_Bar_1_Label, s_bar_default_x[0]);
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

	bar_data_t *bd2 = _lookup_bar_data(1);
	int32_t s2 = _bar_resolution_scale(bd2);
	int32_t b2_min = (bd2 ? bd2->bar_min : 0)   * s2;
	int32_t b2_max = (bd2 ? bd2->bar_max : 100) * s2;
	if (b2_max <= b2_min) { b2_min = 0; b2_max = 100 * s2; }
	ui_Bar_2 = lv_bar_create(parent);
	lv_bar_set_range(ui_Bar_2, b2_min, b2_max);
	lv_bar_set_value(ui_Bar_2, b2_min, LV_ANIM_OFF);
	lv_obj_set_width(ui_Bar_2, 300);
	lv_obj_set_height(ui_Bar_2, 30);
	lv_obj_set_x(ui_Bar_2, s_bar_default_x[1]);
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
	lv_obj_set_x(ui_Bar_2_Label, s_bar_default_x[1]);
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
	/* Scale by 10^decimals so the standard-mode LVGL bar has fractional
	 * resolution within bar_min..bar_max. Image-mode recomputes fill %
	 * directly from the unscaled float value below, so it ignores this. */
	int32_t scale = _bar_resolution_scale(bd);
	int32_t bar_value = is_stale ? 0 : (int32_t)(value * (float)scale);

	/* ── Image-based bar mode ── */
	if (_bar_is_image_mode(bd) && bd->img_clip_obj && lv_obj_is_valid(bd->img_clip_obj)) {
		int32_t range = bd->bar_max - bd->bar_min;
		int32_t pct = 0;
		if (range > 0 && !is_stale) {
			double clamped = final_value;
			if (clamped < bd->bar_min) clamped = bd->bar_min;
			if (clamped > bd->bar_max) clamped = bd->bar_max;
			if (bd->invert_bar_value)
				pct = (int32_t)(((bd->bar_max - clamped) * 100) / range);
			else
				pct = (int32_t)(((clamped - bd->bar_min) * 100) / range);
		}
		lv_coord_t clip_w = (lv_coord_t)((pct * w->w) / 100);
		lv_obj_set_width(bd->img_clip_obj, clip_w);
	} else if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj)) {
		/* ── Standard LVGL bar mode ── */
		lv_bar_set_value(bd->bar_obj, bar_value, LV_ANIM_OFF);
		bool night_active = night_mode_is_active();
		lv_color_t low_col  = NIGHT_PICK_COLOR(night_active, bd->night, bar_low_color,      bd->bar_low_color);
		lv_color_t high_col = NIGHT_PICK_COLOR(night_active, bd->night, bar_high_color,     bd->bar_high_color);
		lv_color_t in_col   = NIGHT_PICK_COLOR(night_active, bd->night, bar_in_range_color, bd->bar_in_range_color);
		lv_color_t new_color;
		if (final_value < bd->bar_low)
			new_color = low_col;
		else if (final_value > bd->bar_high)
			new_color = high_col;
		else
			new_color = in_col;
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

/* Forward declarations — used by _bar_create / _bar_destroy. */
static void _bar_apply_night_mode(widget_t *w, bool active);
static void _bar_night_cb(bool active, void *user_data);

/* ── _bar_create: create a single bar per slot, positioned by layout ──────── */
static void _bar_create(widget_t *w, lv_obj_t *parent) {
	bar_data_t *bd = (bar_data_t *)w->type_data;
	uint8_t slot = bd ? bd->slot : 0;

	/* Internal LVGL range is bar_min/bar_max scaled by 10^decimals so a
	 * value of e.g. 0.85 on a 0..1 bar (decimals=2) fills 85/100 of the
	 * bar, not snaps to 0 or 1. _bar_on_signal applies the same scale. */
	int32_t scale = _bar_resolution_scale(bd);
	int32_t b_min = (bd ? bd->bar_min : 0)   * scale;
	int32_t b_max = (bd ? bd->bar_max : 100) * scale;
	if (b_max <= b_min) { b_min = 0; b_max = 100 * scale; }

	lv_obj_t *bar = NULL; /* only set in standard (non-image) mode */

	if (_bar_is_image_mode(bd)) {
		/* ── Image-based bar mode ──────────────────────────────────── */
		bd->bar_img_dsc = rdm_image_load(bd->bar_image);
		bd->bar_img_full_dsc = rdm_image_load(bd->bar_image_full);
		safe_strncpy(bd->current_bar_image, bd->bar_image, sizeof(bd->current_bar_image));
		safe_strncpy(bd->current_bar_image_full, bd->bar_image_full, sizeof(bd->current_bar_image_full));

		if (bd->bar_img_dsc && bd->bar_img_full_dsc) {
			/* Background (track) image */
			lv_obj_t *bg = lv_img_create(parent);
			lv_img_set_src(bg, bd->bar_img_dsc);
			lv_obj_set_size(bg, w->w, w->h);
			lv_obj_set_align(bg, LV_ALIGN_CENTER);
			lv_obj_set_pos(bg, w->x, w->y);
			lv_img_set_zoom(bg, (uint16_t)(256 * w->w / bd->bar_img_dsc->header.w));
			bd->img_bg_obj = bg;

			/* Clipping container — same position, overflow hidden */
			lv_obj_t *clip = lv_obj_create(parent);
			lv_obj_set_size(clip, 0, w->h); /* starts at 0 width */
			lv_obj_set_align(clip, LV_ALIGN_CENTER);
			/* Position clip left-aligned with the bar area */
			lv_obj_set_pos(clip, w->x - (w->w / 2), w->y);
			lv_obj_set_style_bg_opa(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_width(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_pad_all(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_radius(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
			lv_obj_set_style_clip_corner(clip, true, LV_PART_MAIN | LV_STATE_DEFAULT);
			/* Use LEFT alignment so width grows rightward from left edge */
			lv_obj_set_align(clip, LV_ALIGN_TOP_LEFT);
			/* Convert center-based coords to top-left coords */
			lv_coord_t abs_left = SCREEN_ORIGIN_X + w->x - (w->w / 2);
			lv_coord_t abs_top = SCREEN_ORIGIN_Y + w->y - (w->h / 2);
			lv_obj_set_pos(clip, abs_left, abs_top);
			bd->img_clip_obj = clip;

			/* Fill image inside clip container — left-aligned */
			lv_obj_t *fill = lv_img_create(clip);
			lv_img_set_src(fill, bd->bar_img_full_dsc);
			lv_obj_set_align(fill, LV_ALIGN_TOP_LEFT);
			lv_obj_set_pos(fill, 0, 0);
			lv_img_set_zoom(fill, (uint16_t)(256 * w->w / bd->bar_img_full_dsc->header.w));
			bd->img_full_obj = fill;

			w->root = bg;
		} else {
			/* Failed to load one or both images — fall back to standard bar */
			ESP_LOGW(TAG, "Failed to load bar images, falling back to standard bar");
			if (bd->bar_img_dsc) { rdm_image_free(bd->bar_img_dsc); bd->bar_img_dsc = NULL; }
			if (bd->bar_img_full_dsc) { rdm_image_free(bd->bar_img_full_dsc); bd->bar_img_full_dsc = NULL; }
			goto standard_bar;
		}
	} else {
standard_bar:
		/* ── Standard LVGL bar mode ────────────────────────────────── */
		bar = lv_bar_create(parent);
		lv_bar_set_range(bar, b_min, b_max);
		lv_bar_set_value(bar, b_min, LV_ANIM_OFF);
		lv_obj_set_width(bar, w->w);
		lv_obj_set_height(bar, w->h);
		lv_obj_set_align(bar, LV_ALIGN_CENTER);
		lv_obj_set_pos(bar, w->x, w->y);
		lv_obj_set_style_radius(bar, bd->bar_radius, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(bar, bd->bar_bg_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(bar, bd->bar_border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(bar, bd->bar_border_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(bar, bd->indicator_radius, LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(bar, THEME_COLOR_GREEN_BRIGHT,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(bar, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

		bd->bar_obj = bar;
		w->root = bar;
	}

	/* Create the label above the bar */
	lv_obj_t *lbl = lv_label_create(parent);
	lv_obj_set_align(lbl, LV_ALIGN_CENTER);
	lv_obj_set_pos(lbl, w->x, w->y - 28);
	lv_label_set_text(lbl, (bd && bd->label[0]) ? bd->label : (slot == 0 ? "BAR1" : "BAR2"));
	lv_obj_set_style_text_color(lbl, bd->label_color,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *bar_lbl_font = bd ? widget_resolve_font(bd->label_font) : NULL;
	lv_obj_set_style_text_font(lbl, bar_lbl_font ? bar_lbl_font : THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Create the value label to the right of the bar */
	lv_obj_t *val = lv_label_create(parent);
	lv_obj_set_width(val, 80);
	lv_obj_set_height(val, LV_SIZE_CONTENT);
	lv_obj_set_align(val, LV_ALIGN_CENTER);
	lv_obj_set_pos(val, w->x + (w->w / 2) + 50, w->y - 28);
	lv_label_set_text(val, "---");
	lv_obj_set_style_text_color(val, bd->value_color,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *bar_val_font = bd ? widget_resolve_font(bd->value_font) : NULL;
	lv_obj_set_style_text_font(val, bar_val_font ? bar_val_font : THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	if (!(bd && bd->show_bar_value))
		lv_obj_add_flag(val, LV_OBJ_FLAG_HIDDEN);

	/* Store per-instance pointers for signal callback */
	if (bd) {
		bd->label_obj = lbl;
		bd->value_obj = val;
	}

	/* Assign to slot globals so existing code (RPM limiter, callbacks) works */
	if (slot == 0) {
		ui_Bar_1 = bar; /* NULL in image mode — that's fine */
		ui_Bar_1_Label = lbl;
		ui_Bar_1_Value = val;
	} else {
		ui_Bar_2 = bar;
		ui_Bar_2_Label = lbl;
		ui_Bar_2_Value = val;
	}

	/* Subscribe to signal if bound */
	if (bd && bd->signal_index >= 0)
		signal_subscribe(bd->signal_index, _bar_on_signal, w);

	/* Subscribe to night-mode changes if any night override is set, and apply
	 * current state immediately so the widget renders correctly even if it
	 * was created while night-mode is already active. */
	if (bd && (bd->night.has_bar_low_color      || bd->night.has_bar_high_color    ||
	           bd->night.has_bar_in_range_color || bd->night.has_bar_bg_color      ||
	           bd->night.has_bar_border_color   || bd->night.has_label_color       ||
	           bd->night.has_value_color        || bd->night.has_bar_image         ||
	           bd->night.has_bar_image_full)) {
		night_mode_subscribe(_bar_night_cb, w);
		_bar_apply_night_mode(w, night_mode_is_active());
	}
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
		cJSON_AddNumberToObject(cfg, "decimals", bd->decimals);
		if (bd->label_font[0] != '\0')
			cJSON_AddStringToObject(cfg, "label_font", bd->label_font);
		if (bd->value_font[0] != '\0')
			cJSON_AddStringToObject(cfg, "value_font", bd->value_font);
		if (bd->signal_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "signal_name", bd->signal_name);
		/* Appearance overrides — only serialize non-default values */
		if (bd->bar_bg_color.full != THEME_COLOR_PANEL.full)
			cJSON_AddNumberToObject(cfg, "bar_bg_color", (int)bd->bar_bg_color.full);
		if (bd->bar_radius != 5)
			cJSON_AddNumberToObject(cfg, "bar_radius", bd->bar_radius);
		if (bd->bar_border_width != 2)
			cJSON_AddNumberToObject(cfg, "bar_border_width", bd->bar_border_width);
		if (bd->bar_border_color.full != THEME_COLOR_PANEL.full)
			cJSON_AddNumberToObject(cfg, "bar_border_color", (int)bd->bar_border_color.full);
		if (bd->indicator_radius != 5)
			cJSON_AddNumberToObject(cfg, "indicator_radius", bd->indicator_radius);
		if (bd->label_color.full != THEME_COLOR_TEXT_PRIMARY.full)
			cJSON_AddNumberToObject(cfg, "label_color", (int)bd->label_color.full);
		if (bd->value_color.full != THEME_COLOR_TEXT_PRIMARY.full)
			cJSON_AddNumberToObject(cfg, "value_color", (int)bd->value_color.full);
		/* Image-based bar fields — only serialize if set */
		if (bd->bar_image[0] != '\0')
			cJSON_AddStringToObject(cfg, "bar_image", bd->bar_image);
		if (bd->bar_image_full[0] != '\0')
			cJSON_AddStringToObject(cfg, "bar_image_full", bd->bar_image_full);
		/* Night-mode overrides — emit only fields that have an override set */
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, bd->night, bar_low_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, bar_high_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, bar_in_range_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, bar_bg_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, bar_border_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, label_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, value_color);
		NIGHT_SERIALIZE_IMAGE(n, bd->night, bar_image);
		NIGHT_SERIALIZE_IMAGE(n, bd->night, bar_image_full);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
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
		safe_strncpy(bd->label, item->valuestring, sizeof(bd->label));
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
	item = cJSON_GetObjectItemCaseSensitive(cfg, "decimals");
	if (cJSON_IsNumber(item)) bd->decimals = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label_font");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(bd->label_font, item->valuestring, sizeof(bd->label_font));
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "value_font");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(bd->value_font, item->valuestring, sizeof(bd->value_font));
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(bd->signal_name, item->valuestring, sizeof(bd->signal_name));
	}

	/* Appearance overrides */
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_bg_color");
	if (cJSON_IsNumber(item)) bd->bar_bg_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_radius");
	if (cJSON_IsNumber(item)) bd->bar_radius = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_border_width");
	if (cJSON_IsNumber(item)) bd->bar_border_width = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_border_color");
	if (cJSON_IsNumber(item)) bd->bar_border_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "indicator_radius");
	if (cJSON_IsNumber(item)) bd->indicator_radius = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label_color");
	if (cJSON_IsNumber(item)) bd->label_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "value_color");
	if (cJSON_IsNumber(item)) bd->value_color.full = (uint32_t)item->valueint;

	/* Image-based bar fields */
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_image");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(bd->bar_image, item->valuestring, sizeof(bd->bar_image));
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_image_full");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(bd->bar_image_full, item->valuestring, sizeof(bd->bar_image_full));

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, bd->night, bar_low_color);
		NIGHT_PARSE_COLOR(night, bd->night, bar_high_color);
		NIGHT_PARSE_COLOR(night, bd->night, bar_in_range_color);
		NIGHT_PARSE_COLOR(night, bd->night, bar_bg_color);
		NIGHT_PARSE_COLOR(night, bd->night, bar_border_color);
		NIGHT_PARSE_COLOR(night, bd->night, label_color);
		NIGHT_PARSE_COLOR(night, bd->night, value_color);
		NIGHT_PARSE_IMAGE(night, bd->night, bar_image);
		NIGHT_PARSE_IMAGE(night, bd->night, bar_image_full);
	}

	/* Resolve signal name → index */
	if (bd->signal_name[0] != '\0')
		bd->signal_index = signal_find_by_name(bd->signal_name);
}
static void _bar_destroy(widget_t *w) {
	bar_data_t *bd = (bar_data_t *)w->type_data;
	uint8_t slot = bd ? bd->slot : 0;
	if (bd && bd->signal_index >= 0)
		signal_unsubscribe(bd->signal_index, _bar_on_signal, w);
	night_mode_unsubscribe(_bar_night_cb, w);
	widget_rules_free(w);
	/* Label and value are siblings of root (children of parent), delete explicitly */
	if (bd && bd->label_obj && lv_obj_is_valid(bd->label_obj))
		lv_obj_del(bd->label_obj);
	if (bd && bd->value_obj && lv_obj_is_valid(bd->value_obj))
		lv_obj_del(bd->value_obj);
	/* In image mode, clip container is a sibling of root — delete it separately */
	if (bd && bd->img_clip_obj && lv_obj_is_valid(bd->img_clip_obj))
		lv_obj_del(bd->img_clip_obj);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	/* Clear global pointers so stale references are not used */
	if (slot == 0) {
		ui_Bar_1 = NULL;
		ui_Bar_1_Label = NULL;
		ui_Bar_1_Value = NULL;
	} else {
		ui_Bar_2 = NULL;
		ui_Bar_2_Label = NULL;
		ui_Bar_2_Value = NULL;
	}
	if (bd) {
		bd->label_obj = NULL;
		bd->value_obj = NULL;
		bd->bar_obj = NULL;
		rdm_image_free(bd->bar_img_dsc);
		rdm_image_free(bd->bar_img_full_dsc);
	}
	free(w->type_data);
	free(w);
}

/* ── apply_overrides: live style changes driven by conditional rules ───── */

static void _bar_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (!bd) return;

	/* Start from base bar_data_t values (restore defaults) */
	lv_color_t bar_bg   = bd->bar_bg_color;
	lv_color_t bar_bdr  = bd->bar_border_color;
	lv_coord_t bar_bdrw = (lv_coord_t)bd->bar_border_width;
	lv_color_t lbl_col  = bd->label_color;
	lv_color_t val_col  = bd->value_color;
	const char *lbl_font_name = bd->label_font;
	const char *val_font_name = bd->value_font;

	/* Apply active overrides on top */
	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "bar_bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bar_bg.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "bar_border_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bar_bdr.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "bar_border_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bar_bdrw = (lv_coord_t)o->value.num;
		} else if (strcmp(o->field_name, "label_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			lbl_col.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "value_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			val_col.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "label_font") == 0 && o->value_type == RULE_VAL_STRING) {
			lbl_font_name = o->value.str;
		} else if (strcmp(o->field_name, "value_font") == 0 && o->value_type == RULE_VAL_STRING) {
			val_font_name = o->value.str;
		}
	}

	/* Apply all styles (either overridden or restored to base) */
	if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj)) {
		lv_obj_set_style_bg_color(bd->bar_obj, bar_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(bd->bar_obj, bar_bdr, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(bd->bar_obj, bar_bdrw, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (bd->label_obj && lv_obj_is_valid(bd->label_obj)) {
		lv_obj_set_style_text_color(bd->label_obj, lbl_col, LV_PART_MAIN | LV_STATE_DEFAULT);
		const lv_font_t *lf = widget_resolve_font(lbl_font_name);
		lv_obj_set_style_text_font(bd->label_obj, lf ? lf : THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (bd->value_obj && lv_obj_is_valid(bd->value_obj)) {
		lv_obj_set_style_text_color(bd->value_obj, val_col, LV_PART_MAIN | LV_STATE_DEFAULT);
		const lv_font_t *vf = widget_resolve_font(val_font_name);
		lv_obj_set_style_text_font(bd->value_obj, vf ? vf : THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

/* Re-apply colors (and, if needed, swap bar images) based on current
 * night-mode state. Dynamic bar indicator color (low/high/in_range) is
 * selected per-value inside _bar_on_signal, which already calls
 * night_mode_is_active() — we don't need to recompute it here. */
static void _bar_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (!bd) return;

	lv_color_t bar_bg  = NIGHT_PICK_COLOR(active, bd->night, bar_bg_color,     bd->bar_bg_color);
	lv_color_t bar_bdr = NIGHT_PICK_COLOR(active, bd->night, bar_border_color, bd->bar_border_color);
	lv_color_t lbl_col = NIGHT_PICK_COLOR(active, bd->night, label_color,      bd->label_color);
	lv_color_t val_col = NIGHT_PICK_COLOR(active, bd->night, value_color,      bd->value_color);

	if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj)) {
		lv_obj_set_style_bg_color(bd->bar_obj, bar_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(bd->bar_obj, bar_bdr, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (bd->label_obj && lv_obj_is_valid(bd->label_obj)) {
		lv_obj_set_style_text_color(bd->label_obj, lbl_col, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (bd->value_obj && lv_obj_is_valid(bd->value_obj)) {
		lv_obj_set_style_text_color(bd->value_obj, val_col, LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Image swap — only if in image mode and the desired name differs from
	 * what's currently loaded. Reloading is relatively expensive so we avoid
	 * it when the override isn't set or resolves to the same name. */
	const char *want_bg   = (active && bd->night.has_bar_image)      ? bd->night.bar_image      : bd->bar_image;
	const char *want_full = (active && bd->night.has_bar_image_full) ? bd->night.bar_image_full : bd->bar_image_full;

	if (bd->img_bg_obj && lv_obj_is_valid(bd->img_bg_obj) && want_bg && want_bg[0] != '\0' &&
	    strncmp(bd->current_bar_image, want_bg, sizeof(bd->current_bar_image)) != 0) {
		lv_img_dsc_t *new_dsc = rdm_image_load(want_bg);
		if (new_dsc) {
			lv_img_set_src(bd->img_bg_obj, new_dsc);
			if (new_dsc->header.w > 0)
				lv_img_set_zoom(bd->img_bg_obj, (uint16_t)(256 * w->w / new_dsc->header.w));
			rdm_image_free(bd->bar_img_dsc);
			bd->bar_img_dsc = new_dsc;
			safe_strncpy(bd->current_bar_image, want_bg, sizeof(bd->current_bar_image));
		}
	}
	if (bd->img_full_obj && lv_obj_is_valid(bd->img_full_obj) && want_full && want_full[0] != '\0' &&
	    strncmp(bd->current_bar_image_full, want_full, sizeof(bd->current_bar_image_full)) != 0) {
		lv_img_dsc_t *new_dsc = rdm_image_load(want_full);
		if (new_dsc) {
			lv_img_set_src(bd->img_full_obj, new_dsc);
			if (new_dsc->header.w > 0)
				lv_img_set_zoom(bd->img_full_obj, (uint16_t)(256 * w->w / new_dsc->header.w));
			rdm_image_free(bd->bar_img_full_dsc);
			bd->bar_img_full_dsc = new_dsc;
			safe_strncpy(bd->current_bar_image_full, want_full, sizeof(bd->current_bar_image_full));
		}
	}
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _bar_night_cb(bool active, void *user_data) {
	_bar_apply_night_mode((widget_t *)user_data, active);
}

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
	bd->bar_bg_color = THEME_COLOR_PANEL;
	bd->bar_radius = 5;
	bd->bar_border_width = 2;
	bd->bar_border_color = THEME_COLOR_PANEL;
	bd->indicator_radius = 5;
	bd->label_color = THEME_COLOR_TEXT_PRIMARY;
	bd->value_color = THEME_COLOR_TEXT_PRIMARY;

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
	w->apply_overrides = _bar_apply_overrides;
	w->apply_night_mode = _bar_apply_night_mode;

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
