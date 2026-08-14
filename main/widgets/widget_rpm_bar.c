#include "widget_rpm_bar.h"
#include "widget_rules.h"
#include "data/channel_manager.h"
#include "screen_config.h"
#include "esp_heap_caps.h"
#include "signal.h"
#include "system/night_mode.h"
#include "can/can_decode.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "ui/dashboard.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widget_bar.h"
#include "widget_panel.h"
#include "widget_registry.h"
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
/* Right-half mirror bar — only created for fill_dir 2 (Center→Out) and
 * 3 (Edges→In). NULL for the single-bar modes (0 = L→R, 1 = R→L). It is a
 * child of s_rpm_container, so the destroy cascade frees it; we only NULL the
 * pointer ourselves. set_rpm_value / _apply_limiter_effect / resize all mirror
 * onto it when present. */
static lv_obj_t *rpm_bar_gauge2 = NULL;

/* Live container dimensions — drive proportional scaling of the bar gauge,
 * Panel9 colour swatch, redline zone, and tick marks/labels. Defaults to the
 * stock 800x55 layout; updated by widget_rpm_bar_create() and _rpm_bar_resize(). */
static int s_container_w = 800;
static int s_container_h = 55;

/* Pick the closest pre-compiled Fugaz face for the requested pixel height.
 * The dashboard ships compiled bitmap fonts at 14/17/28/56 px (no TTF fallback
 * for Fugaz), so we bucket to the nearest. */
static const lv_font_t *_pick_tick_font(int desired_px) {
	if (desired_px >= 42) return &ui_font_fugaz_56;
	if (desired_px >= 22) return &ui_font_fugaz_28;
	if (desired_px >= 15) return &ui_font_fugaz_17;
	return &ui_font_fugaz_14;
}

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
	rpm_bar_gauge2 = NULL;   /* freed with the screen; drop the dangling ptr */
}

/* ── Helper: look up rpm_bar_data_t via registry (singleton, slot 0) ──── */
static rpm_bar_data_t *_lookup_rpm_bar_data(void) {
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_RPM_BAR, 0);
	return w ? (rpm_bar_data_t *)w->type_data : NULL;
}

static int current_canbus_rpm = 0; // Store the current CAN bus RPM value

// CAN timeout tracking
static bool rpm_color_needs_update = false;
static lv_color_t new_rpm_color;

/* ── Limiter flash state ──────────────────────────────────────────────────
 * The flash effect (limiter_effect == 1) is driven by a single periodic
 * timer that toggles s_flash_state. set_rpm_value() calls _apply_limiter_effect()
 * which paints the bar in either bar_color (off-flash) or limiter_color (on-flash
 * / solid). The timer is recreated whenever flash_speed_ms changes.
 *
 * limiter_effect values:
 *   0 = None      — _apply_limiter_effect repaints the normal bar_color
 *   1 = Bar Flash — bar toggles between bar_color and limiter_color at flash_speed_ms
 *   2 = Bar Solid — bar stays solid limiter_color (no animation) */
static bool          s_flash_state    = false;       /* current flash phase */
static lv_timer_t   *s_flash_timer    = NULL;        /* shared LVGL timer */
static uint16_t      s_flash_timer_ms = 0;           /* current period the timer was created with */

static void _apply_limiter_effect(void);

static void _rpm_flash_timer_cb(lv_timer_t *timer) {
    (void)timer;
    s_flash_state = !s_flash_state;
    _apply_limiter_effect();
}

/* (Re)create the flash timer if `desired_ms` differs from the active period.
 * Pass 0 to tear it down. */
static void _ensure_flash_timer(uint16_t desired_ms) {
    if (desired_ms == s_flash_timer_ms) return;
    if (s_flash_timer) {
        lv_timer_del(s_flash_timer);
        s_flash_timer    = NULL;
        s_flash_timer_ms = 0;
    }
    if (desired_ms > 0) {
        s_flash_timer = lv_timer_create(_rpm_flash_timer_cb, desired_ms, NULL);
        s_flash_timer_ms = desired_ms;
    }
}
void rpm_gauge_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);
	rpm_gauge_max = 3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update the RPM bar gauge range to match the new max
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		lv_bar_set_range(rpm_bar_gauge, 0, rpm_gauge_max);
	}

	/* Build ticks into the rpm bar's own container, not lv_scr_act() — see
	 * the matching note in _rpm_bar_on_channel_changed. */
	if (s_rpm_container && lv_obj_is_valid(s_rpm_container)) {
		update_rpm_lines(s_rpm_container);
		update_redline_position();
	}
}

void rpm_redline_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);
	rpm_redline_value =
		3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	update_redline_position();
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
		rpm_bar_data_t *rd = _lookup_rpm_bar_data();
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

// RPM Limiter Effect event callback. Dropdown options map 1:1 to enum:
//   0 = None, 1 = Bar Flash, 2 = Bar Solid
void rpm_limiter_effect_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (rd) {
		rd->limiter_effect = (uint8_t)(selected > 2 ? 0 : selected);
		/* Tear down or rebuild the flash timer based on the new effect.
		 * Solid mode (2) doesn't need the timer; only flash mode (1) does. */
		_ensure_flash_timer(rd->limiter_effect == 1 ? rd->flash_speed_ms : 0);
		_apply_limiter_effect();
	}
}

// RPM Flash Speed event callback. Dropdown index → flash period in ms.
void rpm_flash_speed_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	/* Options (50ms steps): 50, 100, 150, 200, ... 1000  → 20 entries */
	uint16_t ms = (uint16_t)(50 + selected * 50);
	if (ms < 50)   ms = 50;
	if (ms > 1000) ms = 1000;
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (rd) {
		rd->flash_speed_ms = ms;
		if (rd->limiter_effect == 1) _ensure_flash_timer(ms);
	}
}

void rpm_limiter_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);

	int32_t rpm_value =
		3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update configuration
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (rd) rd->limiter_value = rpm_value;
}

void rpm_limiter_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
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
		rpm_bar_data_t *rd = _lookup_rpm_bar_data();
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
	rpm_bar_data_t *rd_cw = _lookup_rpm_bar_data();
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
		rpm_bar_data_t *rd = _lookup_rpm_bar_data();
		if (rd) rd->limiter_color = selected_limiter_custom_color;
	}

	// Apply the new limiter color immediately (bar repaints on next frame).
	_apply_limiter_effect();

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
	rpm_bar_data_t *rd_lc = _lookup_rpm_bar_data();
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

	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	uint8_t fill_dir = rd ? rd->fill_dir : 0;

	if (fill_dir == 0) {
		/* Classic mode — map RPM onto the extended bar range so the fill
		 * reaches the very right edge at gauge_max (legacy 782.5/765 hack). */
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge) && rpm_gauge_max > 0) {
			const float bar_extension_ratio = 782.5f / 765.0f;
			int32_t extended_rpm_max =
				(int32_t)(rpm_gauge_max * bar_extension_ratio);
			int32_t scaled_rpm = (rpm * extended_rpm_max) / rpm_gauge_max;
			lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
		}
	} else {
		/* R→L (1) / Center→Out (2) / Edges→In (3): plain [0,gauge_max] range,
		 * same value on both halves — each half's base_dir picks the direction. */
		int32_t v = rpm;
		if (rpm_gauge_max > 0 && v > rpm_gauge_max) v = rpm_gauge_max;
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge))
			lv_bar_set_value(rpm_bar_gauge, v, LV_ANIM_OFF);
		if (rpm_bar_gauge2 && lv_obj_is_valid(rpm_bar_gauge2))
			lv_bar_set_value(rpm_bar_gauge2, v, LV_ANIM_OFF);
	}

	// Limiter overlay: repaint the bar based on whether we crossed the limiter.
	_apply_limiter_effect();
}

/* Repaint the bar's background according to limiter_effect + current RPM.
 *
 *   - effect 0 (None):       bar stays bar_color regardless of RPM
 *   - effect 1 (Bar Flash):  if RPM >= limiter_value, toggle between bar_color
 *                            and limiter_color driven by s_flash_state. Below
 *                            the threshold, bar reverts to bar_color.
 *   - effect 2 (Bar Solid):  if RPM >= limiter_value, bar goes solid
 *                            limiter_color. Below the threshold, bar reverts.
 *
 * Safe to call from any context that already holds the LVGL mutex (set_rpm_value
 * is called from update_rpm_ui which runs on the LVGL task; the flash timer
 * callback also runs on the LVGL task). */
/* Memoization cache for _apply_limiter_effect. Per the LVGL v8 style-
 * invalidation pitfall (see MEMORY.md): lv_obj_set_style_* unconditionally
 * invalidates the target object regardless of whether the new value
 * matches the old one. _apply_limiter_effect is called once PER FRAME
 * from set_rpm_value(); without memo we invalidate the entire 800×55
 * rpm_bar every refresh tick (4.5 MB/s of pixel pumping for no visible
 * change), which then forces LVGL to chunk the area into 8 partial
 * flushes that hammer the cross-FB sync queue. That sync queue overload
 * is what makes the tap-to-show-chrome tear (chrome lives inside this
 * widget's vertical zone). Caching the visible state and skipping style
 * writes when nothing changed kills both the bandwidth waste and the
 * tap-tear. */
static struct {
	bool       valid;          /* cleared on widget create/destroy */
	lv_color_t fill;
	bool       grad_active;
	uint16_t   grad_first_color;  /* hash proxy for gradient identity */
	lv_color_t panel9_color;
} s_paint_cache = {0};

/* Public reset: call from create_instance/destroy or any path that
 * changes config in a way the cache can't detect (e.g. new gradient
 * stops array swapped in). */
static void _invalidate_paint_cache(void) { s_paint_cache.valid = false; }

/* Paint one bar's indicator (solid fill, or the multi-stop gradient when
 * grad_active). Factored out so the mirror modes (fill_dir 2/3) can paint both
 * halves identically. See the long note in _apply_limiter_effect for why
 * dsc.dir must be cleared on the solid path. */
/* Mirror a stop array end-for-end: order reversed, pos p becomes 100-p. */
static void _grad_stops_reverse(const gradient_stops_t *src, gradient_stops_t *out) {
	out->count = src->count;
	for (uint8_t i = 0; i < src->count; i++) {
		const gradient_stop_t *s = &src->stops[src->count - 1 - i];
		out->stops[i].pos   = (uint8_t)(100 - s->pos);
		out->stops[i].color = s->color;
	}
}

/* Does this bar fill from its RIGHT edge? LV_GRAD_DIR_HOR always lays stop[0]
 * at an object's LEFT edge, so any right-filling bar needs the stops mirrored
 * or the gradient runs backwards against the fill.
 *
 * That is three of the four modes' worth of bug in one place: R→L put the
 * first stop at the end of the fill, and each mirror mode got it right on one
 * half and backwards on the other — which is why Center Out looked like the
 * colours were "a bit mixed up" rather than plainly wrong. Reading it off
 * base_dir keeps the rule in one place instead of a fill_dir/half matrix. */
static bool _rpm_bar_fills_rtl(lv_obj_t *bar) {
	if (!bar || !lv_obj_is_valid(bar)) return false;
	return lv_obj_get_style_base_dir(bar, LV_PART_MAIN) == LV_BASE_DIR_RTL;
}

static void _rpm_paint_indicator(lv_obj_t *bar, lv_color_t fill,
                                 bool grad_active, rpm_bar_data_t *rd) {
	if (!bar || !lv_obj_is_valid(bar)) return;
	lv_obj_set_style_bg_color(bar, fill, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	/* Two descriptors, not one: LVGL stores the POINTER, so the two halves of
	 * a mirror layout cannot share a buffer holding opposite stop orders. */
	bool ok = false;
	if (grad_active && rd) {
		const bool rev = _rpm_bar_fills_rtl(bar);
		lv_grad_dsc_t *dsc = rev ? &rd->grad_lv_dsc_rev : &rd->grad_lv_dsc;
		gradient_stops_t mirrored;
		const gradient_stops_t *stops = &rd->grad_stops;
		if (rev) {
			_grad_stops_reverse(&rd->grad_stops, &mirrored);
			stops = &mirrored;
		}
		if (gradient_stops_to_lv_grad_dsc(stops, dsc, LV_GRAD_DIR_HOR)) {
			lv_obj_set_style_bg_grad(bar, dsc,
			                         LV_PART_INDICATOR | LV_STATE_DEFAULT);
			ok = true;
		}
	}
	if (!ok) {
		/* Stand down the descriptor THIS bar's style actually points at, not
		 * a hardcoded one. lv_obj_init_draw_rect_dsc prefers the stored
		 * bg_grad POINTER over bg_grad_dir whenever that descriptor's own
		 * dir is not NONE (lv_obj_draw.c), so clearing bg_grad_dir alone
		 * does not stop the gradient. Only ever clearing grad_lv_dsc left
		 * every RIGHT-TO-LEFT filling bar painting its gradient right
		 * through the limiter, so that bar never flashed: the whole R->L
		 * bar (mode 1), the left half of Centre->Out (2), and the right
		 * half of Edges->In (3). */
		if (rd) {
			lv_grad_dsc_t *mine = _rpm_bar_fills_rtl(bar) ? &rd->grad_lv_dsc_rev
			                                              : &rd->grad_lv_dsc;
			mine->dir = LV_GRAD_DIR_NONE;
		}
		lv_obj_set_style_bg_grad_color(bar, fill,
		                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_NONE,
		                             LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}
}

static void _apply_limiter_effect(void) {
	if (!rpm_bar_gauge || !lv_obj_is_valid(rpm_bar_gauge)) return;

	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	lv_color_t base = rd ? rd->bar_color : THEME_COLOR_GREEN;
	uint8_t   effect = rd ? rd->limiter_effect : 0;
	int32_t   trigger = rd ? rd->limiter_value : INT32_MAX;
	lv_color_t lim_c = rd ? rd->limiter_color : THEME_COLOR_RED;

	bool over_limiter = (current_canbus_rpm >= trigger);

	/* Pick the colour for the FILLED portion (PART_INDICATOR). */
	lv_color_t fill = base;
	if (over_limiter) {
		if (effect == 1) {           /* Bar Flash */
			fill = s_flash_state ? lim_c : base;
		} else if (effect == 2) {    /* Bar Solid */
			fill = lim_c;
		}
	}

	bool grad_active = rd && rd->grad_stops.count >= 2 && !over_limiter;
	uint16_t grad_first_color = (grad_active && rd->grad_stops.count >= 1)
	                                 ? rd->grad_stops.stops[0].color
	                                 : 0;
	lv_color_t panel9_color = fill;
	if (grad_active && rd->grad_stops.count >= 1) {
		panel9_color.full = rd->grad_stops.stops[0].color;
	}

	/* Fast-path: cache hit means visible state is identical to what
	 * we last painted. Skip ALL style writes — under LVGL v8 each one
	 * would invalidate the bar even though nothing actually changes. */
	if (s_paint_cache.valid &&
	    s_paint_cache.fill.full == fill.full &&
	    s_paint_cache.grad_active == grad_active &&
	    s_paint_cache.grad_first_color == grad_first_color &&
	    s_paint_cache.panel9_color.full == panel9_color.full) {
		return;
	}

	/* Multi-stop gradient via LVGL's native lv_grad_dsc_t — suppressed
	 * while flashing or over-limiter so the alert visual stays solid
	 * and unambiguous. sdkconfig bumps LV_GRADIENT_MAX_STOPS to 8 so a
	 * full Photoshop-authored stops array passes through untruncated.
	 * Renders directly into the indicator rect, so as the fill grows
	 * the gradient grows with it (no bg_img centering artifact).
	 *
	 * lv_obj_set_style_bg_grad stores the dsc POINTER, not a copy — the
	 * descriptor must outlive the style. rpm_bar_data_t.grad_lv_dsc
	 * lives as long as the widget. Painted on both halves in the mirror
	 * modes (rpm_bar_gauge2) via _rpm_paint_indicator. */
	_rpm_paint_indicator(rpm_bar_gauge, fill, grad_active, rd);
	if (rpm_bar_gauge2)
		_rpm_paint_indicator(rpm_bar_gauge2, fill, grad_active, rd);

	/* PART_MAIN bg_color is set ONCE at create (widget_rpm_bar_create →
	 * lv_obj_set_style_bg_color(rpm_bar_gauge, THEME_COLOR_RPM_BAR_BG,
	 * LV_PART_MAIN)). Re-writing it per frame served no purpose and
	 * invalidated the entire bar — removed deliberately. */

	/* Panel9 — the square colour swatch on the left edge of the RPM bar.
	 * Tracks the bar's visible "starting" colour: when a gradient is
	 * active, that's the gradient's first stop (matches what the user
	 * sees painted at the leftmost pixel of the fill); otherwise it's
	 * the solid bar_color. Limiter state still wins regardless so the
	 * peripheral-vision warning cue stays unambiguous. */
	if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
		lv_obj_set_style_bg_color(ui_Panel9, panel9_color,
		                           LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Update the paint cache so next frame can fast-path. */
	s_paint_cache.valid = true;
	s_paint_cache.fill = fill;
	s_paint_cache.grad_active = grad_active;
	s_paint_cache.grad_first_color = grad_first_color;
	s_paint_cache.panel9_color = panel9_color;
}
void update_redline_position(void) {
	if (!rpm_redline_zone)
		return;

	/* Redline-zone overlay is the classic L→R bar's feature; keep it hidden
	 * for every other fill direction (mirror/RTL bake redline into art). */
	{
		rpm_bar_data_t *rd = _lookup_rpm_bar_data();
		if (rd && rd->fill_dir != 0) {
			lv_obj_add_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);
			return;
		}
	}

	// Calculate redline position as percentage of max RPM
	float redline_percentage = (float)rpm_redline_value / (float)rpm_gauge_max;

	// Clamp to prevent going beyond the bar
	if (redline_percentage > 1.0f)
		redline_percentage = 1.0f;
	if (redline_percentage < 0.0f)
		redline_percentage = 0.0f;

	/* Scale geometry to the live container size (defaults: 800-wide container,
	 * 765-px bar fill region). */
	float sx = (float)s_container_w / 800.0f;

	const lv_coord_t screen_width = (lv_coord_t)s_container_w;
	const lv_coord_t bar_width = (lv_coord_t)(765.0f * sx + 0.5f);
	const lv_coord_t screen_right_edge =
		screen_width / 2; // Right edge relative to center

	// Calculate redline zone dimensions - extends from right edge of
	// screen to redline position
	lv_coord_t redline_rpm_position =
		-(bar_width / 2) +
		(lv_coord_t)(redline_percentage * bar_width); // RPM position on bar
	lv_coord_t redline_width =
		screen_right_edge -
		redline_rpm_position; // From redline to right edge of screen

	// If redline is at or beyond max RPM, hide the zone
	if (redline_percentage >= 1.0f || redline_width <= 0) {
		lv_obj_add_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	// Show and position the redline zone
	lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_width(rpm_redline_zone, redline_width);
	// Position so it starts at redline RPM position and extends to
	// right edge
	lv_obj_set_x(rpm_redline_zone, redline_rpm_position + (redline_width / 2));
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
	/* Early-out: the signal system fires on every value change, but the
	 * bar and label only need re-rendering when the INTEGER rpm changes.
	 * Without this gate the sim (and noisy real CAN signals) forces a
	 * full bar invalidate + text reflow ~20 Hz, which alone is enough to
	 * drag dashboard FPS down. */
	static int s_last_rpm = -1;
	static char s_last_str[16] = "";
	if (rpm_value == s_last_rpm && rpm_str &&
	    strncmp(rpm_str, s_last_str, sizeof(s_last_str)) == 0) {
		return;
	}
	s_last_rpm = rpm_value;
	if (rpm_str) {
		size_t n = strlen(rpm_str);
		if (n >= sizeof(s_last_str)) n = sizeof(s_last_str) - 1;
		memcpy(s_last_str, rpm_str, n);
		s_last_str[n] = '\0';
	}
	if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
		lv_label_set_text(ui_RPM_Value, rpm_str);
	set_rpm_value(rpm_value);
	update_menu_rpm_value_text(rpm_value);
}
/* Apply the standard RPM-bar track + indicator styling to one lv_bar. Used for
 * the single bar (modes 0/1) and for each half in the mirror modes (2/3). */
static void _rpm_style_bar(lv_obj_t *bar, lv_color_t track_bg, lv_color_t indic) {
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE); /* pass touch to parent */
	lv_obj_set_style_radius(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(bar, track_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(bar, indic, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(bar, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_grad_color(bar, indic, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_NONE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

/* Size + position the bar(s) for the current container size and fill direction.
 * Shared by create_rpm_bar_gauge and _rpm_bar_resize so geometry stays in one
 * place. base_dir per mode controls which way the fill grows:
 *   mode 1  : single bar, RTL  → fill from the right.
 *   mode 2  : left half RTL + right half LTR → both grow OUT from the centre.
 *   mode 3  : left half LTR + right half RTL → both grow IN toward the centre. */
static void _rpm_layout_bars(int cw, int ch, uint8_t fill_dir) {
	if (fill_dir == 2 || fill_dir == 3) {
		int half = cw / 2;
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_size(rpm_bar_gauge, half, ch);
			lv_obj_align(rpm_bar_gauge, LV_ALIGN_LEFT_MID, 0, 0);
			lv_obj_set_style_base_dir(rpm_bar_gauge,
				fill_dir == 2 ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		if (rpm_bar_gauge2 && lv_obj_is_valid(rpm_bar_gauge2)) {
			lv_obj_set_size(rpm_bar_gauge2, cw - half, ch);
			lv_obj_align(rpm_bar_gauge2, LV_ALIGN_RIGHT_MID, 0, 0);
			lv_obj_set_style_base_dir(rpm_bar_gauge2,
				fill_dir == 2 ? LV_BASE_DIR_LTR : LV_BASE_DIR_RTL,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	} else if (fill_dir == 1) {
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_size(rpm_bar_gauge, cw, ch);
			lv_obj_align(rpm_bar_gauge, LV_ALIGN_CENTER, 0, 0);
			lv_obj_set_style_base_dir(rpm_bar_gauge, LV_BASE_DIR_RTL,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	} else {
		/* Mode 0 — legacy extended-width geometry (Panel9 + 20px nudge). */
		float sx = (float)cw / 800.0f;
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_size(rpm_bar_gauge, (lv_coord_t)(783.0f * sx + 0.5f), ch);
			lv_obj_align(rpm_bar_gauge, LV_ALIGN_TOP_MID,
						 (lv_coord_t)(20.0f * sx + 0.5f), 0);
			lv_obj_set_style_base_dir(rpm_bar_gauge, LV_BASE_DIR_LTR,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
}

void create_rpm_bar_gauge(lv_obj_t *container) {
	rpm_bar_data_t *rd_bar = _lookup_rpm_bar_data();
	lv_color_t saved_color = rd_bar ? rd_bar->bar_color : THEME_COLOR_GREEN;
	lv_color_t track_bg = rd_bar ? rd_bar->bar_bg_color : THEME_COLOR_RPM_BAR_BG;
	uint8_t fill_dir = rd_bar ? rd_bar->fill_dir : 0;
	bool mirror = (fill_dir == 2 || fill_dir == 3);

	/* Scale Panel9 geometry to the live container size (default 800x55). */
	float sy = (float)s_container_h / 55.0f;

	/* Panel9 — colour indicator square hugging the container's left edge.
	 * Only meaningful for the classic L→R bar; hidden (kept valid) for every
	 * other fill direction so the mirror/RTL fills span the full width. */
	int panel_sq = s_container_h;
	int panel_x  = -(s_container_w - panel_sq) / 2;
	ui_Panel9 = create_panel(container, panel_sq, panel_sq, panel_x, 0, 0, saved_color, 0);
	if (fill_dir != 0) lv_obj_add_flag(ui_Panel9, LV_OBJ_FLAG_HIDDEN);

	/* Range: mode 0 keeps the legacy extended-width hack so the fill reaches
	 * the very right edge; every other mode uses a plain [0,gauge_max]. */
	const float bar_extension_ratio = 782.5f / 765.0f;
	int32_t bar_max = mirror || fill_dir == 1
		? rpm_gauge_max
		: (int32_t)(rpm_gauge_max * bar_extension_ratio);

	rpm_bar_gauge = lv_bar_create(container);
	lv_bar_set_range(rpm_bar_gauge, 0, bar_max);
	lv_bar_set_value(rpm_bar_gauge, 0, LV_ANIM_OFF);
	_rpm_style_bar(rpm_bar_gauge, track_bg, saved_color);

	if (mirror) {
		rpm_bar_gauge2 = lv_bar_create(container);
		lv_bar_set_range(rpm_bar_gauge2, 0, bar_max);
		lv_bar_set_value(rpm_bar_gauge2, 0, LV_ANIM_OFF);
		_rpm_style_bar(rpm_bar_gauge2, track_bg, saved_color);
	} else {
		rpm_bar_gauge2 = NULL;
	}

	_rpm_layout_bars(s_container_w, s_container_h, fill_dir);

	/* Redline zone — inside container, center-relative y. Originally height=12
	 * with y=22 inside the 55px container; scale both so it stays anchored to
	 * the lower portion of the bar at any height. Hidden for non-default fill
	 * directions (it only makes sense for the L→R bar; bake redline into art
	 * or use the limiter effect for the mirror/RTL modes). */
	rpm_redline_zone = lv_obj_create(container);
	lv_obj_set_height(rpm_redline_zone, (lv_coord_t)(12.0f * sy + 0.5f));
	lv_obj_set_y(rpm_redline_zone, (lv_coord_t)(22.0f * sy + 0.5f));
	lv_obj_set_align(rpm_redline_zone, LV_ALIGN_CENTER);
	lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); /* pass touch to parent */
	lv_obj_set_style_radius(rpm_redline_zone, 0,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_redline_zone, THEME_COLOR_RED,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(rpm_redline_zone, 180,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(rpm_redline_zone, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	if (fill_dir != 0) lv_obj_add_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);

	/* The optional numeric RPM readout is not built here — _rpm_bar_create()
	 * calls _rpm_bar_sync_value_label() once the container is sized, so the
	 * create and live-edit paths share one code path. */
}

/* Create-or-update the numeric RPM readout to match the current rd fields.
 * Used by the inspector when show_rpm_value / rpm_value_font / rpm_value_color
 * change at runtime. Creates the label lazily inside the rpm container the
 * first time it's enabled; hides it (keeps the object) when disabled so a
 * later re-enable is cheap. Safe to call only on the LVGL task. */
static void _rpm_bar_sync_value_label(rpm_bar_data_t *rd) {
	if (!rd) return;
	if (!rd->show_rpm_value) {
		if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
			lv_obj_add_flag(ui_RPM_Value, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	if (!s_rpm_container || !lv_obj_is_valid(s_rpm_container)) return;

	float sx = (float)s_container_w / 800.0f;

	if (!ui_RPM_Value || !lv_obj_is_valid(ui_RPM_Value)) {
		ui_RPM_Value = lv_label_create(s_rpm_container);
		lv_label_set_text(ui_RPM_Value, "--");
		/* Home = bottom-centre of the bar, so the readout starts sitting just
		 * under the fill and horizontally centred, and the X/Y offsets nudge
		 * from there. It used to be dead centre with a +20*sx nudge inherited
		 * from the Panel9-era geometry, which left the number off to one side
		 * and on top of the fill. */
		lv_obj_set_align(ui_RPM_Value, LV_ALIGN_BOTTOM_MID);
		lv_obj_clear_flag(ui_RPM_Value, LV_OBJ_FLAG_CLICKABLE);
		rd->rpm_value_obj = ui_RPM_Value;
	}
	lv_obj_clear_flag(ui_RPM_Value, LV_OBJ_FLAG_HIDDEN);

	const lv_font_t *vfont = widget_resolve_font(rd->rpm_value_font);
	if (!vfont) vfont = THEME_FONT_DASH_RPM;
	lv_obj_set_style_text_font(ui_RPM_Value, vfont,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(ui_RPM_Value, rd->rpm_value_color,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_RPM_Value, LV_OPA_COVER,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Offsets are pure nudges off the bottom-centre home set at create. */
	lv_obj_set_pos(ui_RPM_Value, rd->rpm_value_x_offset, rd->rpm_value_y_offset);
}

/* ── On-device appearance config callbacks ─────────────────────────────────
 * rpm_bar's STYLE editing is a hand-written tab (build_rpm_settings_tab in
 * config_modal.c), NOT the schema-driven generated STYLE tab, so the new
 * appearance fields don't auto-appear on-device — these callbacks wire the
 * extra rows. Each mirrors the matching inspector_set branch so web + device
 * stay in lockstep. COLOR_OPTS dropdown layout: Green0 Cyan1 Yellow2 Orange3
 * Red4 Blue5 Purple6 Magenta7 Pink8 Custom9 (last opens nothing here — the
 * on-device flow has no per-field colour wheel for these, so Custom is a
 * no-op that leaves the current colour unchanged). */
static bool _rpm_color_from_opts_idx(uint16_t idx, lv_color_t *out) {
	switch (idx) {
	case 0: *out = THEME_COLOR_GREEN;   return true;
	case 1: *out = THEME_COLOR_CYAN;    return true;
	case 2: *out = THEME_COLOR_YELLOW;  return true;
	case 3: *out = THEME_COLOR_ORANGE;  return true;
	case 4: *out = THEME_COLOR_RED;     return true;
	case 5: *out = THEME_COLOR_BLUE;    return true;
	case 6: *out = THEME_COLOR_PURPLE;  return true;
	case 7: *out = THEME_COLOR_MAGENTA; return true;
	case 8: *out = THEME_COLOR_PINK;    return true;
	default: return false; /* Custom / out-of-range — leave unchanged */
	}
}

void rpm_show_ticks_switch_event_cb(lv_event_t *e) {
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (!rd) return;
	rd->show_ticks = on;
	if (s_rpm_container && lv_obj_is_valid(s_rpm_container))
		update_rpm_lines(s_rpm_container);
}

void rpm_tick_side_dropdown_event_cb(lv_event_t *e) {
	uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (!rd) return;
	rd->tick_side = (uint8_t)(sel > 2 ? 2 : sel);
	if (s_rpm_container && lv_obj_is_valid(s_rpm_container))
		update_rpm_lines(s_rpm_container);
}

void rpm_tick_color_dropdown_event_cb(lv_event_t *e) {
	uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (!rd) return;
	lv_color_t c;
	if (!_rpm_color_from_opts_idx(sel, &c)) return;
	rd->tick_color = c;
	if (s_rpm_container && lv_obj_is_valid(s_rpm_container))
		update_rpm_lines(s_rpm_container);
}

void rpm_bar_bg_color_dropdown_event_cb(lv_event_t *e) {
	uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (!rd) return;
	lv_color_t c;
	if (!_rpm_color_from_opts_idx(sel, &c)) return;
	rd->bar_bg_color = c;
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge))
		lv_obj_set_style_bg_color(rpm_bar_gauge, rd->bar_bg_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
}

void rpm_show_value_switch_event_cb(lv_event_t *e) {
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (!rd) return;
	rd->show_rpm_value = on;
	_rpm_bar_sync_value_label(rd);
}

void rpm_value_color_dropdown_event_cb(lv_event_t *e) {
	uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
	rpm_bar_data_t *rd = _lookup_rpm_bar_data();
	if (!rd) return;
	lv_color_t c;
	if (!_rpm_color_from_opts_idx(sel, &c)) return;
	rd->rpm_value_color = c;
	if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
		lv_obj_set_style_text_color(ui_RPM_Value, rd->rpm_value_color,
									LV_PART_MAIN | LV_STATE_DEFAULT);
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

	/* Per-instance appearance (colour / length / width / side / visibility).
	 * Falls back to the historical hardcoded look if the data struct can't be
	 * found (e.g. very early boot before the widget is registered). */
	rpm_bar_data_t *rd_ticks = _lookup_rpm_bar_data();
	bool       show_ticks  = rd_ticks ? rd_ticks->show_ticks  : true;
	uint8_t    tick_side   = rd_ticks ? rd_ticks->tick_side   : 2;
	uint8_t    tick_len_n  = rd_ticks ? rd_ticks->tick_length : 12;
	uint8_t    tick_wid_n  = rd_ticks ? rd_ticks->tick_width  : 3;
	/* Night-aware tick colour: a wholesale rebuild (which is how tick colour
	 * changes are applied, including on night-mode transitions) picks the
	 * night override when night mode is active and an override is set. */
	lv_color_t tick_color  = rd_ticks ? rd_ticks->tick_color  : THEME_COLOR_BG;
	if (rd_ticks)
		tick_color = NIGHT_PICK_COLOR(night_mode_is_active(), rd_ticks->night,
		                              tick_color, rd_ticks->tick_color);
	uint8_t    lbl_every   = rd_ticks ? rd_ticks->label_every : 1;
	if (tick_len_n < 1) tick_len_n = 1;
	if (tick_wid_n < 1) tick_wid_n = 1;
	if (lbl_every  < 1) lbl_every  = 1;

	/* Ticks hidden: bookkeeping already cleared above (old objects deleted),
	 * so just leave the parent free of tick marks. */
	if (!show_ticks) return;

	// Step in increments of 500 RPM for medium and main ticks
	int increments = 500;
	int num_lines = (rpm_gauge_max / increments) + 1; // Include 0 RPM

	// Ensure we don't exceed MAX_RPM_LINES per set
	if (num_lines > MAX_RPM_LINES) {
		num_lines = MAX_RPM_LINES;
	}

	/* Scale factors against the stock 800x55 layout. sx spreads the ticks
	 * across the wider/narrower bar; sy scales tick length, thickness, label
	 * font and vertical placement so a taller RPM bar gets visibly chunkier
	 * markings. */
	float sx = (float)s_container_w / 800.0f;
	float sy = (float)s_container_h / 55.0f;

	lv_coord_t bar_x = (lv_coord_t)(18.0f * sx + 0.5f);
	lv_coord_t bar_y_set1 = 0; // top row: anchored to container top edge
	lv_coord_t span_px     = (lv_coord_t)(765.0f * sx + 0.5f);

	/* Tallest tick (main) sets the bottom-row baseline so all bottom ticks end
	 * flush with the container's bottom edge. Main length is user-driven
	 * (tick_len_n, nominal 12) and still scaled by sy for taller bars. */
	lv_coord_t main_h = (lv_coord_t)((float)tick_len_n * sy + 0.5f);
	if (main_h < 1) main_h = 1;

	/* Tick label font: scale 17px nominal by sy, then snap to nearest preloaded face. */
	const lv_font_t *tick_font = _pick_tick_font((int)(17.0f * sy + 0.5f));
	lv_coord_t label_off       = (lv_coord_t)(7.0f * sy + 0.5f);

	/* Numbers follow the fill. The scale used to run 0-at-the-left in every
	 * mode while the fill went somewhere else entirely — Center Out grew
	 * outward from the middle under a scale that still counted up from the
	 * far left, and R→L filled backwards under the same scale.
	 *
	 *   0 (L→R)        0 .......... max
	 *   1 (R→L)      max .......... 0
	 *   2 (Center Out) max ... 0 ... max   each half counts out from the middle
	 *   3 (Edges In)     0 ... max ... 0   each half counts in from its edge
	 *
	 * The two mirror modes place every value TWICE, so they run the body
	 * once per half. 200-slot pool against 17 values at 8000 rpm — doubling
	 * is nowhere near it. */
	rpm_bar_data_t *rd_dir  = _lookup_rpm_bar_data();
	uint8_t         fdir    = rd_dir ? rd_dir->fill_dir : 0;
	bool            mirrored = (fdir == 2 || fdir == 3);
	int             halves   = mirrored ? 2 : 1;
	lv_coord_t      half_px  = span_px / 2;

	for (int i = 0; i < num_lines; i++) {
	  for (int h = 0; h < halves; h++) {
		// Current RPM value for the tick
		int rpm_value = i * increments;

		// Calculate the x position based on rpm_value (scaled span)
		lv_coord_t frac_full = (lv_coord_t)(((int64_t)rpm_value * span_px) / rpm_gauge_max);
		lv_coord_t frac_half = (lv_coord_t)(((int64_t)rpm_value * half_px) / rpm_gauge_max);
		lv_coord_t x_pos;
		if (fdir == 1) {
			x_pos = bar_x + span_px - frac_full;              /* max at the left  */
		} else if (fdir == 2) {                               /* 0 in the middle  */
			x_pos = (h == 0) ? bar_x + half_px - frac_half
			                 : bar_x + half_px + frac_half;
		} else if (fdir == 3) {                               /* 0 at both edges  */
			x_pos = (h == 0) ? bar_x + frac_half
			                 : bar_x + span_px - frac_half;
		} else {
			x_pos = bar_x + frac_full;
		}
		/* The halves meet in the middle: mode 2's zero and mode 3's max land
		 * on the same pixel from both sides. Draw that one once, or it gets a
		 * double-thick tick and two labels stacked on each other. */
		if (h == 1 && ((fdir == 2 && rpm_value == 0) ||
		               (fdir == 3 && frac_half * 2 >= span_px))) continue;

		// Decide which size line/tick to draw
		//    - Every 1000 RPM: main tick (3x12 nominal)
		//    - Every 500 RPM:  medium tick (2x8 nominal)
		lv_coord_t line_width;
		lv_coord_t line_height;
		bool add_label = false; // Only label the 1000s in the first set

		if ((rpm_value % 1000) == 0) {
			// Main tick — full user width/length
			line_width  = (lv_coord_t)((float)tick_wid_n * sy + 0.5f);
			line_height = main_h;
			/* Thin the NUMBERS only — the tick stays, so the scale keeps its
			 * rhythm while the text stops colliding. */
			add_label = ((rpm_value / 1000) % lbl_every) == 0;
		} else {
			// Medium tick (500 RPM) — keep the original 2:3 width / 8:12 length
			// proportion relative to the main tick so user sizing scales both.
			line_width  = (lv_coord_t)((float)tick_wid_n * (2.0f / 3.0f) * sy + 0.5f);
			line_height = (lv_coord_t)((float)tick_len_n * (8.0f / 12.0f) * sy + 0.5f);
		}
		if (line_width  < 1) line_width  = 1;
		if (line_height < 1) line_height = 1;

		lv_coord_t adjusted_x = x_pos - (line_width / 2);

		/* tick_side: 0=Top, 1=Bottom, 2=Both.
		 *   - Top row drawn when tick_side != 1 (Bottom-only).
		 *   - Bottom row drawn when tick_side != 0 (Top-only).
		 * The thousands label always anchors to whichever row exists (top
		 * preferred for the historical look), and is suppressed only when both
		 * rows are off — which can't happen for a valid tick_side. */
		bool draw_top    = (tick_side != 1);
		bool draw_bottom = (tick_side != 0);

		lv_obj_t *label_anchor = NULL;
		bool      anchor_is_top = false;

		// Create the first set of lines (top row)
		if (draw_top) {
			lv_obj_t *line_top = lv_obj_create(parent);
			lv_obj_set_size(line_top, line_width, line_height);
			lv_obj_set_style_radius(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_color(line_top, tick_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_opa(line_top, LV_OPA_COVER,
									LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_width(line_top, 0,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_pad_all(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_clear_flag(line_top, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

			// Position the line so it's centered horizontally on x_pos
			lv_obj_set_pos(line_top, adjusted_x, bar_y_set1);

			rpm_lines[num_rpm_lines] = line_top;
			num_rpm_lines++;

			label_anchor  = line_top;
			anchor_is_top = true;
			if (num_rpm_lines >= MAX_RPM_LINES * 2) break;
		}

		// Create the second set of lines (bottom row, flipped height)
		if (draw_bottom) {
			lv_obj_t *line_bottom = lv_obj_create(parent);
			lv_obj_set_size(line_bottom, line_width, line_height);
			lv_obj_set_style_radius(line_bottom, 0,
									LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_color(line_bottom, tick_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_opa(line_bottom, LV_OPA_COVER,
									LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_width(line_bottom, 0,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_pad_all(line_bottom, 0,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_clear_flag(line_bottom, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

			// Bottom row: anchor each tick to the container's bottom edge so
			// shorter ticks slide down — matches the original 55px layout where
			// every bottom tick ended at y=55.
			lv_obj_set_pos(line_bottom, adjusted_x,
						   s_container_h - line_height);

			rpm_lines[num_rpm_lines] = line_bottom;
			if (!label_anchor) label_anchor = line_bottom;
			num_rpm_lines++;
		}

		// Add a label for 1000 RPM ticks. Stored at the slot index of its
		// anchor tick so update_rpm_lines's delete loop frees it (rpm_labels[]
		// is indexed by line slot, only valid for i < MAX_RPM_LINES).
		if (add_label && label_anchor) {
			int label_slot = num_rpm_lines - 1; /* last pushed tick's slot */
			if (anchor_is_top) label_slot = num_rpm_lines - (draw_bottom ? 2 : 1);
			if (label_slot >= 0 && label_slot < MAX_RPM_LINES) {
				lv_obj_t *label = lv_label_create(parent);

				// Display the "thousands" place (e.g., "7" for 7000)
				char rpm_str[8];
				snprintf(rpm_str, sizeof(rpm_str), "%d", rpm_value / 1000);
				lv_label_set_text(label, rpm_str);

				// Style the label (font scales with container height)
				lv_obj_set_style_text_color(label, tick_color, LV_PART_MAIN);
				lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
				lv_obj_set_style_text_font(label, tick_font,
										   LV_PART_MAIN | LV_STATE_DEFAULT);

				/* Anchor the label INSIDE the container. A top-row tick sits at
				 * the container's top edge, so the label hangs below it. A
				 * bottom-only tick sits flush with the container's BOTTOM edge —
				 * hanging the label below it would push it past the bottom and
				 * LVGL clips it (the "Bottom = numbers vanish" bug). Place it
				 * above the tick instead so it stays visible. */
				if (anchor_is_top)
					lv_obj_align_to(label, label_anchor,
									LV_ALIGN_OUT_BOTTOM_MID, 0, label_off);
				else
					lv_obj_align_to(label, label_anchor,
									LV_ALIGN_OUT_TOP_MID, 0, -label_off);

				rpm_labels[label_slot] = label;
			}
		}

		// Stop if we exceed the maximum number of lines
		if (num_rpm_lines >= MAX_RPM_LINES * 2) {
			break;
		}
	  }
	  /* The inner break only leaves the half loop — stop the value loop too. */
	  if (num_rpm_lines >= MAX_RPM_LINES * 2) break;
	}
}

lv_obj_t *widget_rpm_bar_create(lv_obj_t *parent) {
	/* Create a transparent container that holds all RPM sub-components.
	 * This allows the whole RPM widget to be moved as a single unit. */
	lv_obj_t *container = lv_obj_create(parent);
	lv_obj_set_size(container, SCREEN_W, 55);
	lv_obj_set_align(container, LV_ALIGN_CENTER);
	lv_obj_set_pos(container, 0, -(int16_t)SCREEN_ORIGIN_Y + 55 / 2);
	lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Sync the container-dimension cache so create_rpm_bar_gauge / update_rpm_lines
	 * pick up the right scale factors on first build. _rpm_bar_create() will
	 * resize the container (and re-call _rpm_bar_resize) once the layout-loaded
	 * w/h are applied. */
	s_container_w = SCREEN_W;
	s_container_h = 55;

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

/* Forward decl: channel-changed handler re-subscribes this below its own def. */
static void _rpm_bar_on_signal(float value, bool is_stale, void *user_data);

/* Channel-changed listener — snapshot channel fields into widget. */
static void _rpm_bar_on_channel_changed(channel_t *c, void *user_data) {
	if (!c || !user_data) return;
	widget_t *w = (widget_t *)user_data;
	rpm_bar_data_t *rd = (rpm_bar_data_t *)w->type_data;
	if (!rd) return;
	safe_strncpy(rd->signal_name, c->signal_name, sizeof(rd->signal_name));
	/* Re-point our own signal subscription when the channel re-resolves to a
	 * different signal index. Updating rd->signal_index alone leaves the
	 * _rpm_bar_on_signal callback bound to the OLD index in the registry, so
	 * the gauge would freeze on its last value after a runtime re-bind. Mirror
	 * the inspector-set re-subscribe pattern (safe: this runs under lvgl lock). */
	int16_t new_idx = c->signal_index;
	if (new_idx != rd->signal_index) {
		if (rd->signal_index >= 0)
			signal_unsubscribe(rd->signal_index, _rpm_bar_on_signal, w);
		rd->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _rpm_bar_on_signal, w);
	}
	/* RPM is whole-number; channel range is float, so round to int. */
	if (c->max > 0) rd->gauge_max = (int32_t)lroundf(c->max);
	if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
		rd->redline = (int32_t)lroundf(c->high_warn);
	/* Mirror to the global render state (gauge range, tick marks and redline
	 * all read it), then refresh the gauge exactly as the old roller cb did so
	 * a live channel edit rescales the bar + ticks without needing a reload. */
	if (rd->gauge_max > 0) rpm_gauge_max = rd->gauge_max;
	rpm_redline_value = rd->redline;
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge))
		lv_bar_set_range(rpm_bar_gauge, 0, rpm_gauge_max);
	/* Rebuild tick marks into the rpm bar's OWN container — never lv_scr_act().
	 * A live channel edit can arrive while a modal (e.g. the on-device channels
	 * editor) is the active screen; parenting ticks to lv_scr_act() then builds
	 * them as children of the modal with center-origin coords, so they bleed on
	 * top of the modal and the dashboard. The container is the only correct
	 * parent. Guard for the rare window where it's been torn down. */
	if (s_rpm_container && lv_obj_is_valid(s_rpm_container)) {
		update_rpm_lines(s_rpm_container);
		update_redline_position();
	}
}

/* Smoothing: re-feed the eased value, bypassing the intercept (no recursion). */
static void _rpm_bar_smooth_apply(widget_t *w, float v) {
	rpm_bar_data_t *rd = w ? (rpm_bar_data_t *)w->type_data : NULL;
	if (!rd) return;
	rd->smooth_bypass = true;
	_rpm_bar_on_signal(v, false, w);
	rd->smooth_bypass = false;
}

static void _rpm_bar_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	rpm_bar_data_t *rd = w ? (rpm_bar_data_t *)w->type_data : NULL;
	/* Value smoothing: ease the fill at the refresh rate. */
	if (rd && rd->smooth.smoothing_ms != 0 && !rd->smooth_bypass) {
		if (is_stale) widget_smooth_reset(&rd->smooth);
		else { widget_smooth_set(&rd->smooth, value, false); return; }
	}
	if (is_stale) {
		update_rpm_ui_immediate("--", 0);
		return;
	}
	int rpm = (int)value;
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", rpm);
	update_rpm_ui_immediate(buf, rpm);
}

/* Forward declarations — used by _rpm_bar_create / _rpm_bar_destroy. */
static void _rpm_bar_apply_night_mode(widget_t *w, bool active);
static void _rpm_bar_night_cb(bool active, void *user_data);

static void _rpm_bar_resize(widget_t *w, uint16_t nw, uint16_t nh);

static void _rpm_bar_create(widget_t *w, lv_obj_t *parent) {
	lv_obj_t *container = widget_rpm_bar_create(parent);
	w->root = container;

	/* Apply layout-defined size and position (overrides hardcoded defaults) */
	lv_obj_set_size(container, w->w, w->h);
	lv_obj_set_pos(container, w->x, w->y);
	/* Re-scale internal objects + re-render ticks so labels/lines match the
	 * loaded size, not the 800x55 defaults baked into widget_rpm_bar_create. */
	if (w->w != SCREEN_W || w->h != 55) {
		_rpm_bar_resize(w, w->w, w->h);
	}

	/* Subscribe to signal if bound */
	rpm_bar_data_t *rbd = (rpm_bar_data_t *)w->type_data;

	/* Build the numeric readout when the loaded layout asks for it. The label
	 * is created lazily by _rpm_bar_sync_value_label(), which the inspector
	 * also calls on live edits; without this call a saved show_rpm_value
	 * renders nothing until the user toggles it again. Must run after
	 * _rpm_bar_resize() — the readout is positioned from s_container_w. */
	_rpm_bar_sync_value_label(rbd);

	if (rbd && rbd->signal_index >= 0)
		signal_subscribe(rbd->signal_index, _rpm_bar_on_signal, w);

	if (rbd && rbd->channel)
		channel_manager_subscribe((channel_t *)rbd->channel,
		                           _rpm_bar_on_channel_changed, w);

	/* Wire up optional value smoothing (eases the fill at refresh rate). */
	if (rbd)
		widget_smooth_config(&rbd->smooth, w, _rpm_bar_smooth_apply,
		                     (float)rbd->gauge_max);

	/* Subscribe to night-mode changes if any night override is set, and apply
	 * current state immediately so the widget renders correctly even if it
	 * was created while night-mode is already active. */
	if (rbd && (rbd->night.has_bar_color || rbd->night.has_limiter_color ||
	            rbd->night.has_tick_color || rbd->night.has_bar_bg_color ||
	            rbd->night.has_rpm_value_color)) {
		night_mode_subscribe(_rpm_bar_night_cb, w);
		_rpm_bar_apply_night_mode(w, night_mode_is_active());
	}

	/* Spin up the limiter flash timer if the loaded layout uses Bar Flash.
	 * For Bar Solid (effect=2) we skip the timer; _apply_limiter_effect()
	 * paints the static limiter colour on each set_rpm_value() call. */
	if (rbd && rbd->limiter_effect == 1)
		_ensure_flash_timer(rbd->flash_speed_ms ? rbd->flash_speed_ms : 200);
	else
		_ensure_flash_timer(0);
}
static void _rpm_bar_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	w->w = nw;
	w->h = nh;
	s_container_w = nw;
	s_container_h = nh;

	if (s_rpm_container && lv_obj_is_valid(s_rpm_container))
		lv_obj_set_size(s_rpm_container, nw, nh);

	float sx = (float)nw / 800.0f;
	float sy = (float)nh / 55.0f;

	rpm_bar_data_t *rd_rs = _lookup_rpm_bar_data();
	uint8_t fill_dir = rd_rs ? rd_rs->fill_dir : 0;

	/* Panel9 — only shown for the classic L→R bar (hidden otherwise); only
	 * track its square geometry in that mode. */
	if (fill_dir == 0 && ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
		int panel_sq = nh;
		int panel_x  = -((int)nw - panel_sq) / 2;
		lv_obj_set_size(ui_Panel9, panel_sq, panel_sq);
		lv_obj_set_pos(ui_Panel9, panel_x, 0);
	}

	/* RPM bar gauge(s) — size + position + base_dir per fill direction. */
	_rpm_layout_bars(nw, nh, fill_dir);

	/* Redline zone — height + vertical anchor scale with sy; width/x are
	 * recomputed by update_redline_position() against the new container width. */
	if (rpm_redline_zone && lv_obj_is_valid(rpm_redline_zone)) {
		lv_obj_set_height(rpm_redline_zone, (lv_coord_t)(12.0f * sy + 0.5f));
		lv_obj_set_y(rpm_redline_zone, (lv_coord_t)(22.0f * sy + 0.5f));
	}

	/* Numeric RPM readout — home is LV_ALIGN_BOTTOM_MID (set at create), so
	 * resizing only needs to re-apply the user's nudge. */
	if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
		rpm_bar_data_t *rd_v = _lookup_rpm_bar_data();
		lv_obj_set_pos(ui_RPM_Value,
		               rd_v ? rd_v->rpm_value_x_offset : 0,
		               rd_v ? rd_v->rpm_value_y_offset : 0);
	}

	/* Rebuild tick marks + labels at the new scale. */
	if (s_rpm_container && lv_obj_is_valid(s_rpm_container))
		update_rpm_lines(s_rpm_container);

	update_redline_position();
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
	{
		/* Multi-stop gradient. Old grad_enabled/grad_end_color fields
		 * are dropped on save — _rpm_bar_from_json still reads them
		 * for legacy-layout migration. */
		cJSON *gs = gradient_stops_to_json(&rd->grad_stops);
		if (gs) cJSON_AddItemToObject(cfg, "grad_stops", gs);
	}
	cJSON_AddNumberToObject(cfg, "limiter_effect", rd->limiter_effect);
	cJSON_AddNumberToObject(cfg, "limiter_value", rd->limiter_value);
	cJSON_AddNumberToObject(cfg, "limiter_color", (int)rd->limiter_color.full);
	cJSON_AddNumberToObject(cfg, "flash_speed", rd->flash_speed_ms);
	if (rd->smooth.smoothing_ms != 20)   /* default 20 ms — defaults-only emit */
		cJSON_AddNumberToObject(cfg, "smoothing_ms", rd->smooth.smoothing_ms);
	if (rd->fill_dir != 0)              /* default 0 (L→R) — defaults-only emit */
		cJSON_AddNumberToObject(cfg, "fill_dir", rd->fill_dir);

	/* ── Appearance — defaults-only emit (keeps untouched widgets empty so
	 * the 32 KB layout budget isn't burned). Defaults mirror the historical
	 * hardcoded look. ─────────────────────────────────────────────────── */
	if (!rd->show_ticks)            /* default true */
		cJSON_AddBoolToObject(cfg, "show_ticks", false);
	if (rd->tick_side != 2)         /* default 2 (Both) */
		cJSON_AddNumberToObject(cfg, "tick_side", rd->tick_side);
	if (rd->tick_length != 12)      /* default 12 */
		cJSON_AddNumberToObject(cfg, "tick_length", rd->tick_length);
	if (rd->tick_width != 3)        /* default 3 */
		cJSON_AddNumberToObject(cfg, "tick_width", rd->tick_width);
	if (rd->tick_color.full != THEME_COLOR_BG.full)
		cJSON_AddNumberToObject(cfg, "tick_color", (int)rd->tick_color.full);
	if (rd->bar_bg_color.full != THEME_COLOR_RPM_BAR_BG.full)
		cJSON_AddNumberToObject(cfg, "bar_bg_color", (int)rd->bar_bg_color.full);
	/* Numeric RPM readout (defaults: off / theme font / theme primary). The
	 * night override of rpm_value_color was already serialized; the base
	 * values were not — so enabling the readout or restyling it was lost on
	 * reload. Defaults-only emit keeps untouched widgets empty. */
	if (rd->label_every != 1)
		cJSON_AddNumberToObject(cfg, "label_every", rd->label_every);
	if (rd->show_rpm_value)         /* default false */
		cJSON_AddBoolToObject(cfg, "show_rpm_value", true);
	if (rd->rpm_value_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "rpm_value_font", rd->rpm_value_font);
	if (rd->rpm_value_color.full != THEME_COLOR_TEXT_PRIMARY.full)
		cJSON_AddNumberToObject(cfg, "rpm_value_color", (int)rd->rpm_value_color.full);
	if (rd->rpm_value_x_offset != 0)
		cJSON_AddNumberToObject(cfg, "rpm_value_x_offset", rd->rpm_value_x_offset);
	if (rd->rpm_value_y_offset != 0)
		cJSON_AddNumberToObject(cfg, "rpm_value_y_offset", rd->rpm_value_y_offset);

	if (rd->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", rd->signal_name);
	if (rd->channel_id[0] != '\0')
		cJSON_AddStringToObject(cfg, "channel", rd->channel_id);
	/* Night-mode overrides — emit only fields that have an override set */
	{
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, rd->night, bar_color);
		NIGHT_SERIALIZE_COLOR(n, rd->night, limiter_color);
		NIGHT_SERIALIZE_COLOR(n, rd->night, tick_color);
		NIGHT_SERIALIZE_COLOR(n, rd->night, bar_bg_color);
		NIGHT_SERIALIZE_COLOR(n, rd->night, rpm_value_color);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
	}
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
	item = cJSON_GetObjectItemCaseSensitive(cfg, "smoothing_ms");
	if (cJSON_IsNumber(item) && item->valueint >= 0)
		rd->smooth.smoothing_ms = (uint16_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "fill_dir");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 0 || v > 3) v = 0;
		rd->fill_dir = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_color");
	if (cJSON_IsNumber(item)) rd->bar_color.full = (uint32_t)item->valueint;
	/* Multi-stop gradient with legacy-2stop fallback. */
	const cJSON *gs_arr = cJSON_GetObjectItemCaseSensitive(cfg, "grad_stops");
	if (!gradient_stops_from_json(gs_arr, &rd->grad_stops)) {
		const cJSON *ge  = cJSON_GetObjectItemCaseSensitive(cfg, "grad_enabled");
		const cJSON *gec = cJSON_GetObjectItemCaseSensitive(cfg, "grad_end_color");
		if (cJSON_IsBool(ge) && cJSON_IsTrue(ge) && cJSON_IsNumber(gec)) {
			gradient_stops_install_legacy_2stop(&rd->grad_stops,
				rd->bar_color.full, (uint16_t)gec->valueint);
		}
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_effect");
	if (cJSON_IsNumber(item)) rd->limiter_effect = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_value");
	if (cJSON_IsNumber(item)) rd->limiter_value = (int32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_color");
	if (cJSON_IsNumber(item)) rd->limiter_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "flash_speed");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 50)   v = 50;
		if (v > 1000) v = 1000;
		rd->flash_speed_ms = (uint16_t)v;
	}

	/* ── Appearance — all default to the historical hardcoded look, so an
	 * older layout that omits these reproduces the previous render. ────── */
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label_every");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint; if (v < 1) v = 1; if (v > 10) v = 10;
		rd->label_every = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_ticks");
	if (cJSON_IsBool(item)) rd->show_ticks = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_side");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 0 || v > 2) v = 2;
		rd->tick_side = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_length");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 1)   v = 1;
		if (v > 255) v = 255;
		rd->tick_length = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_width");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 1)   v = 1;
		if (v > 255) v = 255;
		rd->tick_width = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_color");
	if (cJSON_IsNumber(item)) rd->tick_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_bg_color");
	if (cJSON_IsNumber(item)) rd->bar_bg_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_rpm_value");
	if (cJSON_IsBool(item)) rd->show_rpm_value = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "rpm_value_font");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(rd->rpm_value_font, item->valuestring, sizeof(rd->rpm_value_font));
	item = cJSON_GetObjectItemCaseSensitive(cfg, "rpm_value_x_offset");
	if (cJSON_IsNumber(item)) rd->rpm_value_x_offset = (int8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "rpm_value_y_offset");
	if (cJSON_IsNumber(item)) rd->rpm_value_y_offset = (int8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "rpm_value_color");
	if (cJSON_IsNumber(item)) rd->rpm_value_color.full = (uint32_t)item->valueint;
	/* Clamp to the current 3-value enum. Older firmware versions had a
	 * 7-value enum that included "circles" modes (now removed); the legacy
	 * migration ladder lived here until 2026-04-27 and was deleted because
	 * any field-deployed device has long since re-saved its layouts in the
	 * new shape. Any out-of-range value now silently degrades to None. */
	if (rd->limiter_effect > 2) rd->limiter_effect = 0;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(rd->signal_name, item->valuestring, sizeof(rd->signal_name));

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, rd->night, bar_color);
		NIGHT_PARSE_COLOR(night, rd->night, limiter_color);
		NIGHT_PARSE_COLOR(night, rd->night, tick_color);
		NIGHT_PARSE_COLOR(night, rd->night, bar_bg_color);
		NIGHT_PARSE_COLOR(night, rd->night, rpm_value_color);
	}

	/* Resolve signal name → index */
	if (rd->signal_name[0] != '\0')
		rd->signal_index = signal_find_by_name(rd->signal_name);

	/* ── v14 channel binding + backwards-compat migration ─────────
	 * Empty-signal channel falls through to the legacy path so
	 * record_legacy_widget repopulates it from the widget's own signal. */
	cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(cfg, "channel");
	if (cJSON_IsString(ch_item) && ch_item->valuestring && ch_item->valuestring[0] != '\0')
		safe_strncpy(rd->channel_id, ch_item->valuestring, sizeof(rd->channel_id));
	channel_t *bound_c = rd->channel_id[0] ? channel_manager_get(rd->channel_id) : NULL;
	if (bound_c && bound_c->signal_index >= 0) {
		rd->channel = bound_c;
		safe_strncpy(rd->signal_name, bound_c->signal_name, sizeof(rd->signal_name));
		rd->signal_index = bound_c->signal_index;
		if (bound_c->max > 0) rd->gauge_max = (int32_t)lroundf(bound_c->max);
		if (bound_c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
			rd->redline = (int32_t)lroundf(bound_c->high_warn);
		/* Channel is authoritative for gauge range + redline — mirror to the
		 * globals the gauge/tick/redline render helpers read, overriding any
		 * rpm_max/redline that came from JSON above. Guard gauge_max so a
		 * channel with max=0 can't zero the update_redline_position divisor. */
		if (rd->gauge_max > 0) rpm_gauge_max = rd->gauge_max;
		rpm_redline_value = rd->redline;
	} else if (rd->signal_name[0] != '\0') {
		legacy_widget_data_t legacy = {
			.signal_name = rd->signal_name,
			.min = 0,
			.max = rd->gauge_max,
			.high_warn = rd->redline,
			.color_normal = lv_color_to32(rd->bar_color) & 0xFFFFFF,
			.color_high_warn = lv_color_to32(rd->limiter_color) & 0xFFFFFF,
		};
		channel_t *c = channel_manager_record_legacy_widget(&legacy);
		if (c) rd->channel = c;
	}
}
static void _rpm_bar_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	rpm_bar_data_t *rd = (rpm_bar_data_t *)w->type_data;
	if (!rd) return;

	/* Restore defaults from type_data */
	lv_color_t bar_col = rd->bar_color;
	lv_color_t lim_col = rd->limiter_color;

	/* Overlay active overrides */
	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "bar_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			lv_color_t c; c.full = (uint16_t)o->value.color;
			bar_col = c;
		} else if (strcmp(o->field_name, "limiter_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			lv_color_t c; c.full = (uint16_t)o->value.color;
			lim_col = c;
		}
	}

	/* Apply bar indicator color (both halves in the mirror modes). */
	_rpm_paint_indicator(rpm_bar_gauge, bar_col, false, rd);
	if (rpm_bar_gauge2)
		_rpm_paint_indicator(rpm_bar_gauge2, bar_col, false, rd);

	/* Apply redline/limiter zone color */
	if (rpm_redline_zone && lv_obj_is_valid(rpm_redline_zone)) {
		lv_obj_set_style_bg_color(rpm_redline_zone, lim_col,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

static void _rpm_bar_destroy(widget_t *w) {
	if (w) {
		rpm_bar_data_t *rbd = (rpm_bar_data_t *)w->type_data;
		if (rbd) widget_smooth_free(&rbd->smooth);
		if (rbd && rbd->signal_index >= 0)
			signal_unsubscribe(rbd->signal_index, _rpm_bar_on_signal, w);
		if (rbd && rbd->channel) {
			channel_manager_unsubscribe((channel_t *)rbd->channel,
			                             _rpm_bar_on_channel_changed, w);
			rbd->channel = NULL;
		}
		night_mode_unsubscribe(_rpm_bar_night_cb, w);
		widget_rules_free(w);
		/* Tear down the shared limiter flash timer — it references nothing
		 * widget-specific but there's no point running it without a bar. */
		_ensure_flash_timer(0);
		/* Reset the per-frame paint cache — the next rpm_bar instance
		 * (different colors / gradient) must repaint at least once. */
		_invalidate_paint_cache();
		/* The numeric RPM readout is a child of w->root (the container), so
		 * lv_obj_del below frees it via the cascade. NULL the global +
		 * per-instance pointer so the update_rpm_ui paths don't touch a freed
		 * object — mirrors how ui_Screen3.c NULLs ui_RPM_Value on rebuild. */
		if (rbd) rbd->rpm_value_obj = NULL;
		ui_RPM_Value = NULL;
		/* rpm_bar_gauge2 is a child of w->root (the container) — freed by the
		 * cascade below; just drop our dangling pointer. */
		rpm_bar_gauge2 = NULL;
		if (w->root && lv_obj_is_valid(w->root))
			lv_obj_del(w->root);
		w->root = NULL;
		free(w->type_data);
		free(w);
	}
}

/* Re-apply colors based on current night-mode state. Uses the same global
 * LVGL objects (rpm_bar_gauge / rpm_redline_zone) that _rpm_bar_apply_overrides
 * writes to. */
static void _rpm_bar_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	rpm_bar_data_t *rd = (rpm_bar_data_t *)w->type_data;
	if (!rd) return;

	lv_color_t bar_col = NIGHT_PICK_COLOR(active, rd->night, bar_color,     rd->bar_color);
	lv_color_t lim_col = NIGHT_PICK_COLOR(active, rd->night, limiter_color, rd->limiter_color);
	lv_color_t bg_col  = NIGHT_PICK_COLOR(active, rd->night, bar_bg_color,  rd->bar_bg_color);
	lv_color_t val_col = NIGHT_PICK_COLOR(active, rd->night, rpm_value_color, rd->rpm_value_color);

	/* Indicator colour — both halves in the mirror modes. */
	_rpm_paint_indicator(rpm_bar_gauge, bar_col, false, rd);
	if (rpm_bar_gauge2)
		_rpm_paint_indicator(rpm_bar_gauge2, bar_col, false, rd);
	/* Track background (unfilled portion) — plain style write on each bar. */
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge))
		lv_obj_set_style_bg_color(rpm_bar_gauge, bg_col,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	if (rpm_bar_gauge2 && lv_obj_is_valid(rpm_bar_gauge2))
		lv_obj_set_style_bg_color(rpm_bar_gauge2, bg_col,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	if (rpm_redline_zone && lv_obj_is_valid(rpm_redline_zone)) {
		lv_obj_set_style_bg_color(rpm_redline_zone, lim_col,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	/* Numeric readout colour — plain style write. */
	if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
		lv_obj_set_style_text_color(ui_RPM_Value, val_col,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	/* Tick colour is baked into the per-tick objects at create, so a colour
	 * change requires a wholesale rebuild. update_rpm_lines() reads the
	 * night-picked tick colour itself, so just rebuild here if a tick
	 * override exists (skip the rebuild otherwise to avoid the per-transition
	 * teardown cost). */
	if (rd->night.has_tick_color && s_rpm_container &&
	    lv_obj_is_valid(s_rpm_container)) {
		update_rpm_lines(s_rpm_container);
	}
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _rpm_bar_night_cb(bool active, void *user_data) {
	_rpm_bar_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector get / set ──────────────────────────────────────────────────
 *
 * Schema names: schema/widgets.schema.json -> rpm_bar fields. Note two
 * field-name remappings:
 *   schema "rpm_max"     -> rpm_bar_data_t.gauge_max + legacy global rpm_gauge_max
 *   schema "flash_speed" -> rpm_bar_data_t.flash_speed_ms
 *
 * Live preview leans on the existing _apply_limiter_effect() and tick-line
 * rebuild helpers — same path the legacy on-device config callbacks use. */

static bool _rpm_bar_inspector_get(const widget_t *w, const char *name,
                                   widget_field_value_t *out) {
	if (!w || w->type != WIDGET_RPM_BAR || !w->type_data || !name || !out) return false;
	const rpm_bar_data_t *rd = (const rpm_bar_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0)    { out->str = rd->signal_name;   return true; }
	if (strcmp(name, "rpm_max") == 0)        { out->i = rd->gauge_max;       return true; }
	if (strcmp(name, "redline") == 0)        { out->i = rd->redline;         return true; }
	if (strcmp(name, "limiter_effect") == 0) { out->i = rd->limiter_effect;  return true; }
	if (strcmp(name, "limiter_value") == 0)  { out->i = rd->limiter_value;   return true; }
	if (strcmp(name, "flash_speed") == 0)    { out->i = rd->flash_speed_ms;  return true; }
	if (strcmp(name, "smoothing_ms") == 0)   { out->i = rd->smooth.smoothing_ms; return true; }
	if (strcmp(name, "fill_dir") == 0)       { out->i = rd->fill_dir;        return true; }
	if (strcmp(name, "bar_color") == 0)      { out->color = lv_color_to32(rd->bar_color)     & 0xFFFFFF; return true; }
	if (strcmp(name, "limiter_color") == 0)  { out->color = lv_color_to32(rd->limiter_color) & 0xFFFFFF; return true; }
	/* Appearance fields */
	if (strcmp(name, "show_ticks") == 0)     { out->b = rd->show_ticks;       return true; }
	if (strcmp(name, "tick_side") == 0)      { out->i = rd->tick_side;        return true; }
	if (strcmp(name, "tick_length") == 0)    { out->i = rd->tick_length;      return true; }
	if (strcmp(name, "tick_width") == 0)     { out->i = rd->tick_width;       return true; }
	if (strcmp(name, "tick_color") == 0)     { out->color = lv_color_to32(rd->tick_color)      & 0xFFFFFF; return true; }
	if (strcmp(name, "bar_bg_color") == 0)   { out->color = lv_color_to32(rd->bar_bg_color)    & 0xFFFFFF; return true; }
	if (strcmp(name, "show_rpm_value") == 0) { out->b = rd->show_rpm_value;   return true; }
	if (strcmp(name, "label_every") == 0) { out->i = rd->label_every; return true; }
	if (strcmp(name, "rpm_value_x_offset") == 0) { out->i = rd->rpm_value_x_offset; return true; }
	if (strcmp(name, "rpm_value_y_offset") == 0) { out->i = rd->rpm_value_y_offset; return true; }
	if (strcmp(name, "rpm_value_font") == 0) { out->str = rd->rpm_value_font; return true; }
	if (strcmp(name, "rpm_value_color") == 0){ out->color = lv_color_to32(rd->rpm_value_color) & 0xFFFFFF; return true; }
	return false;
}

static bool _rpm_bar_inspector_set(widget_t *w, const char *name,
                                   const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_RPM_BAR || !w->type_data || !name || !in) return false;
	rpm_bar_data_t *rd = (rpm_bar_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0 && in->str) {
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;

		if (rd->signal_index >= 0)
			signal_unsubscribe(rd->signal_index, _rpm_bar_on_signal, w);
		safe_strncpy(rd->signal_name, in->str, sizeof(rd->signal_name));
		rd->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _rpm_bar_on_signal, w);
		return true;
	}
	if (strcmp(name, "rpm_max") == 0) {
		int v = in->i;
		if (v < 1000)  v = 1000;
		if (v > 20000) v = 20000;
		rd->gauge_max = v;
		rpm_gauge_max = v;   /* mirror global used by tick / redline helpers */
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge))
			lv_bar_set_range(rpm_bar_gauge, 0, rpm_gauge_max);
		if (w->root && lv_obj_is_valid(w->root))
			update_rpm_lines(w->root);
		update_redline_position();
		return true;
	}
	if (strcmp(name, "redline") == 0) {
		int v = in->i;
		if (v < 0) v = 0;
		rd->redline = v;
		rpm_redline_value = v;
		update_redline_position();
		return true;
	}
	if (strcmp(name, "smoothing_ms") == 0) {
		int v = in->i;
		if (v < 0)   v = 0;
		if (v > 500) v = 500;
		rd->smooth.smoothing_ms = (uint16_t)v;
		if (v == 0) widget_smooth_reset(&rd->smooth);  /* snap to live on next sample */
		return true;
	}
	if (strcmp(name, "fill_dir") == 0) {
		int v = in->i;
		if (v < 0 || v > 3) v = 0;
		/* Structural change (single vs two-bar) — stored now, applied on the
		 * next layout reload (the web editor saves + hot-reloads after edits). */
		rd->fill_dir = (uint8_t)v;
		return true;
	}
	if (strcmp(name, "bar_color") == 0) {
		rd->bar_color = lv_color_hex(in->color);
		_apply_limiter_effect();   /* repaints with new bar / limiter colours */
		return true;
	}
	if (strcmp(name, "limiter_color") == 0) {
		rd->limiter_color = lv_color_hex(in->color);
		_apply_limiter_effect();
		return true;
	}
	if (strcmp(name, "limiter_effect") == 0) {
		uint8_t v = (uint8_t)in->i;
		if (v > 2) v = 0;
		rd->limiter_effect = v;
		_ensure_flash_timer(v == 1 ? (rd->flash_speed_ms ? rd->flash_speed_ms : 200) : 0);
		_apply_limiter_effect();
		return true;
	}
	if (strcmp(name, "limiter_value") == 0) {
		rd->limiter_value = (int32_t)in->i;
		_apply_limiter_effect();
		return true;
	}
	if (strcmp(name, "flash_speed") == 0) {
		int v = in->i;
		if (v < 50)   v = 50;
		if (v > 1000) v = 1000;
		rd->flash_speed_ms = (uint16_t)v;
		if (rd->limiter_effect == 1) _ensure_flash_timer(rd->flash_speed_ms);
		return true;
	}

	/* ── Appearance fields ────────────────────────────────────────────── */
	/* Tick visibility / geometry / colour all require a wholesale tick
	 * rebuild — update_rpm_lines reads rd->* directly. */
	if (strcmp(name, "show_ticks") == 0) {
		rd->show_ticks = in->b;
		if (w->root && lv_obj_is_valid(w->root)) update_rpm_lines(w->root);
		return true;
	}
	if (strcmp(name, "tick_side") == 0) {
		int v = in->i;
		if (v < 0 || v > 2) v = 2;
		rd->tick_side = (uint8_t)v;
		if (w->root && lv_obj_is_valid(w->root)) update_rpm_lines(w->root);
		return true;
	}
	if (strcmp(name, "tick_length") == 0) {
		int v = in->i;
		if (v < 1)   v = 1;
		if (v > 255) v = 255;
		rd->tick_length = (uint8_t)v;
		if (w->root && lv_obj_is_valid(w->root)) update_rpm_lines(w->root);
		return true;
	}
	if (strcmp(name, "tick_width") == 0) {
		int v = in->i;
		if (v < 1)   v = 1;
		if (v > 255) v = 255;
		rd->tick_width = (uint8_t)v;
		if (w->root && lv_obj_is_valid(w->root)) update_rpm_lines(w->root);
		return true;
	}
	if (strcmp(name, "tick_color") == 0) {
		rd->tick_color = lv_color_hex(in->color);
		if (w->root && lv_obj_is_valid(w->root)) update_rpm_lines(w->root);
		return true;
	}
	/* Bar track background — live style write on PART_MAIN. */
	if (strcmp(name, "bar_bg_color") == 0) {
		rd->bar_bg_color = lv_color_hex(in->color);
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge))
			lv_obj_set_style_bg_color(rpm_bar_gauge, rd->bar_bg_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	/* Numeric readout — create-or-show/hide + restyle. */
	if (strcmp(name, "show_rpm_value") == 0) {
		rd->show_rpm_value = in->b;
		_rpm_bar_sync_value_label(rd);
		return true;
	}
	if (strcmp(name, "label_every") == 0) {
		int v = in->i; if (v < 1) v = 1; if (v > 10) v = 10;
		rd->label_every = (uint8_t)v;
		update_rpm_lines(s_rpm_container);   /* rebuild the scale */
		return true;
	}
	if (strcmp(name, "rpm_value_x_offset") == 0) {
		rd->rpm_value_x_offset = (int8_t)in->i;
		_rpm_bar_sync_value_label(rd);   /* one place computes the position */
		return true;
	}
	if (strcmp(name, "rpm_value_y_offset") == 0) {
		rd->rpm_value_y_offset = (int8_t)in->i;
		_rpm_bar_sync_value_label(rd);
		return true;
	}
	if (strcmp(name, "rpm_value_font") == 0 && in->str) {
		safe_strncpy(rd->rpm_value_font, in->str, sizeof(rd->rpm_value_font));
		_rpm_bar_sync_value_label(rd);
		return true;
	}
	if (strcmp(name, "rpm_value_color") == 0) {
		rd->rpm_value_color = lv_color_hex(in->color);
		if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
			lv_obj_set_style_text_color(ui_RPM_Value, rd->rpm_value_color,
										LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	return false;
}

widget_t *widget_rpm_bar_create_instance(void) {
	/* Fresh widget — drop any cached paint state from a previous instance
	 * so the first _apply_limiter_effect call always writes through. */
	_invalidate_paint_cache();

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
	rd->smooth.smoothing_ms = 20;   /* default: gentle 20 ms glide (snappy, no lag) */

	/* Sensible defaults */
	rd->gauge_max = 8000;
	rd->redline = 6500;
	rd->bar_color = lv_color_hex(0x00FF00);  /* green */
	/* grad_stops zero-initialised by calloc — count=0 means no gradient,
	 * render path falls back to solid bar_color. */
	rd->limiter_effect = 0;
	rd->limiter_value = 7500;
	rd->limiter_color = lv_color_hex(0xFF0000);  /* red */
	rd->flash_speed_ms = 200;
	rd->fill_dir = 0;   /* Left → Right (classic single bar) */

	/* Appearance defaults — chosen so they reproduce the previously-hardcoded
	 * look exactly, keeping to_json empty for untouched widgets. */
	rd->show_ticks   = true;            /* ticks were always drawn before */
	rd->tick_side    = 2;               /* Both rows (top + bottom) */
	rd->label_every  = 1;               /* every thousand, the historical look */
	rd->tick_length  = 12;              /* matches the old 12px main-tick height */
	rd->tick_width   = 3;               /* matches the old 3px main-tick width */
	rd->tick_color   = THEME_COLOR_BG;  /* old hardcoded tick/label colour */
	rd->bar_bg_color = THEME_COLOR_RPM_BAR_BG; /* old hardcoded track bg */
	rd->show_rpm_value = false;         /* no numeric readout historically */
	rd->rpm_value_font[0] = '\0';       /* empty → THEME_FONT_DASH_RPM fallback */
	rd->rpm_value_color   = THEME_COLOR_TEXT_PRIMARY;
	rd->rpm_value_obj     = NULL;

	w->type_data = rd;
	w->type = WIDGET_RPM_BAR;
	w->slot = 0;
	/* RPM bar occupies full screen width at top.
	 * y = -SCREEN_ORIGIN_Y + 55/2 = -213 in center-origin coords. */
	w->x = 0;
	w->y = -(int16_t)SCREEN_ORIGIN_Y + 55 / 2;
	w->w = SCREEN_W;
	w->h = 55;
	snprintf(w->id, sizeof(w->id), "rpm_bar_0");

	w->create = _rpm_bar_create;
	w->resize = _rpm_bar_resize;
	w->open_settings = _rpm_bar_open_settings;
	w->to_json = _rpm_bar_to_json;
	w->from_json = _rpm_bar_from_json;
	w->destroy = _rpm_bar_destroy;
	w->apply_overrides = _rpm_bar_apply_overrides;
	w->apply_night_mode = _rpm_bar_apply_night_mode;
	w->inspector_get = _rpm_bar_inspector_get;
	w->inspector_set = _rpm_bar_inspector_set;

	return w;
}

