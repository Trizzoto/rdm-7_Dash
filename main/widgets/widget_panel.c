#include "widget_panel.h"
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
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/dashboard.h"
#include "widget_types.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "signal.h"

/* Async update payload for lv_async_call(update_panel_ui, ...) */
typedef struct {
	uint8_t panel_index;
	char    value_str[32];
	double  final_value;
} panel_update_t;

uint64_t last_panel_can_received[8] = {0};

/* Shared LVGL styles — initialised by init_styles() / init_common_style() */
lv_style_t box_style;
lv_style_t common_style;

static const lv_coord_t label_positions[8][2] = {
	{-312, -54}, {-146, -54}, {-312, 54}, {-146, 54},
	{146, -54},	 {312, -54},  {146, 54},  {312, 54}};
static const lv_coord_t value_positions[8][2] = {
	{-312, -17}, {-146, -17}, {-312, 91}, {-146, 91},
	{146, -17},	 {312, -17},  {146, 91},  {312, 91}};
static const lv_coord_t box_positions[8][2] = {
	{-312, -26}, {-146, -26}, {-312, 82}, {-146, 82},
	{146, -26},	 {312, -26},  {146, 82},  {312, 82}};
/* ── panel_data_t: per-instance state replacing raw slot cast ────────────── */
typedef struct {
	uint8_t    slot;
	char       label[64];
	char       custom_text[32];
	uint8_t    decimals;
	bool       warning_high_enabled;
	float      warning_high_threshold;
	lv_color_t warning_high_color;
	bool       warning_low_enabled;
	float      warning_low_threshold;
	lv_color_t warning_low_color;
	char       signal_name[32];
	int16_t    signal_index;
	/* LVGL object pointers (runtime only) */
	lv_obj_t  *box;
	lv_obj_t  *header_label;
	lv_obj_t  *value_label;
	lv_obj_t  *custom_text_label;
} panel_data_t;

/* ── Helper: look up panel_data_t by slot ────────────────────────────────── */
static panel_data_t *_get_panel_data_by_slot(uint8_t slot) {
	if (slot >= 8) return NULL;
	widget_t **widgets = dashboard_get_widgets();
	uint8_t count = dashboard_get_widget_count();
	for (uint8_t i = 0; i < count; i++) {
		if (widgets[i] && widgets[i]->type == WIDGET_PANEL) {
			panel_data_t *pd = (panel_data_t *)widgets[i]->type_data;
			if (pd && pd->slot == slot) return pd;
		}
	}
	return NULL;
}

/* ── Public setters for panel warning thresholds (called from widget_warning.c) ── */
void widget_panel_set_warning_high(uint8_t slot, float threshold, bool enabled) {
	panel_data_t *pd = _get_panel_data_by_slot(slot);
	if (!pd) return;
	pd->warning_high_threshold = threshold;
	pd->warning_high_enabled = enabled;
}

void widget_panel_set_warning_low(uint8_t slot, float threshold, bool enabled) {
	panel_data_t *pd = _get_panel_data_by_slot(slot);
	if (!pd) return;
	pd->warning_low_threshold = threshold;
	pd->warning_low_enabled = enabled;
}

void widget_panel_set_warning_high_color(uint8_t slot, lv_color_t color) {
	panel_data_t *pd = _get_panel_data_by_slot(slot);
	if (!pd) return;
	pd->warning_high_color = color;
}

void widget_panel_set_warning_low_color(uint8_t slot, lv_color_t color) {
	panel_data_t *pd = _get_panel_data_by_slot(slot);
	if (!pd) return;
	pd->warning_low_color = color;
}

void update_panel_ui(void *param) {
	panel_update_t *update = (panel_update_t *)param;
	if (!update)
		return;

	uint8_t i = update->panel_index;
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]) &&
		lv_obj_get_screen(ui_Value[i]) != NULL) {
		lv_label_set_text(ui_Value[i], update->value_str);
	}

	// Also update menu preview if it exists, is valid, and menu is visible
	if (menu_panel_value_labels[i] &&
		lv_obj_is_valid(menu_panel_value_labels[i]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_panel_value_labels[i], update->value_str);
	}

	// Look up panel_data_t for threshold checks
	panel_data_t *pd = _get_panel_data_by_slot(i);

	// Also update menu panel box border effects if menu is visible
	if (menu_panel_boxes[i] && lv_obj_is_valid(menu_panel_boxes[i]) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		// Apply same border logic as main screen panels
		if (strcmp(update->value_str, "---") == 0) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_high_enabled &&
				   update->final_value > pd->warning_high_threshold) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  pd->warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_low_enabled &&
				   update->final_value < pd->warning_low_threshold) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  pd->warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Ensure border is visible
		lv_obj_set_style_border_width(menu_panel_boxes[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(menu_panel_boxes[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Update border color based on thresholds
	if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) &&
		lv_obj_get_screen(ui_Box[i]) != NULL) {
		// Special case: if showing "---", always use default grey color
		if (strcmp(update->value_str, "---") == 0) {
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_high_enabled &&
				   update->final_value > pd->warning_high_threshold) {
			lv_obj_set_style_border_color(ui_Box[i],
										  pd->warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_low_enabled &&
				   update->final_value < pd->warning_low_threshold) {
			lv_obj_set_style_border_color(ui_Box[i],
										  pd->warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Ensure border is visible
		lv_obj_set_style_border_width(ui_Box[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(ui_Box[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	free(update);
}

// Immediate (no-alloc, no-async) panel update
void update_panel_ui_immediate(uint8_t i, const char *value_str,
							   double final_value) {
	if (i >= 8)
		return;
	panel_data_t *pd = _get_panel_data_by_slot(i);
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]) &&
		lv_obj_get_screen(ui_Value[i]) != NULL) {
		lv_label_set_text(ui_Value[i], value_str);
	}
	if (menu_panel_value_labels[i] &&
		lv_obj_is_valid(menu_panel_value_labels[i]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_panel_value_labels[i], value_str);
	}
	if (menu_panel_boxes[i] && lv_obj_is_valid(menu_panel_boxes[i]) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		if (strcmp(value_str, "---") == 0) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_high_enabled &&
				   final_value > pd->warning_high_threshold) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  pd->warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_low_enabled &&
				   final_value < pd->warning_low_threshold) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  pd->warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		lv_obj_set_style_border_width(menu_panel_boxes[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(menu_panel_boxes[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) &&
		lv_obj_get_screen(ui_Box[i]) != NULL) {
		if (strcmp(value_str, "---") == 0) {
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_high_enabled &&
				   final_value > pd->warning_high_threshold) {
			lv_obj_set_style_border_color(ui_Box[i],
										  pd->warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (pd && pd->warning_low_enabled &&
				   final_value < pd->warning_low_threshold) {
			lv_obj_set_style_border_color(ui_Box[i],
										  pd->warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		lv_obj_set_style_border_width(ui_Box[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(ui_Box[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}
void apply_common_roller_styles(lv_obj_t *roller) {
	// Set text color for dall items
	lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_SELECTED);
	lv_obj_set_style_bg_color(roller, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(roller, 0, LV_PART_SELECTED);
	lv_obj_set_style_radius(roller, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// Initialize styles
void init_styles(void) {
	// Box Style
	lv_style_init(&box_style);
	lv_style_set_radius(&box_style, 7);
	lv_style_set_bg_color(&box_style,
						  THEME_COLOR_BG); // Black background
	lv_style_set_bg_opa(&box_style, 255);  // Full opacity for black background
	lv_style_set_clip_corner(&box_style, false);
	lv_style_set_border_color(&box_style, THEME_COLOR_PANEL);
	lv_style_set_border_opa(&box_style, 255);
	lv_style_set_border_width(&box_style, 3);
	lv_style_set_border_post(&box_style, true); // Ensure border is drawn on top
	lv_style_set_outline_width(&box_style, 0);	// Remove black outline
	lv_style_set_outline_pad(&box_style, 0);
}

void init_common_style(void) {
	lv_style_init(&common_style);
	lv_style_set_radius(&common_style, 7);
	lv_style_set_pad_all(&common_style, 8); // 7px padding on all sides
	lv_style_set_bg_color(&common_style,
						  THEME_COLOR_TEXT_PRIMARY); // White background
	lv_style_set_bg_opa(&common_style, LV_OPA_COVER);
	lv_style_set_border_color(&common_style,
							  THEME_COLOR_TEXT_MUTED); // Light gray border
	lv_style_set_border_width(&common_style, 1);
	lv_style_set_text_color(&common_style, lv_color_black()); // Black text
	lv_style_set_text_font(&common_style,
						   THEME_FONT_SMALL); // Common font
}

// Getter function for common_style to allow access from other files
lv_style_t *get_common_style(void) { return &common_style; }

lv_style_t *get_box_style(void) { return &box_style; }

// Shared panel helper used by widget_rpm_bar.c
lv_obj_t *create_panel(lv_obj_t *parent, int width, int height, int x, int y,
					   int radius, lv_color_t bg_color, int transform_angle) {
	lv_obj_t *panel = lv_obj_create(parent);
	lv_obj_set_size(panel, width, height);
	lv_obj_set_pos(panel, x, y);
	lv_obj_set_align(panel, LV_ALIGN_CENTER);
	lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(panel, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(panel, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (transform_angle != 0) {
		lv_obj_set_style_transform_angle(panel, transform_angle,
										 LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	return panel;
}
/////////////////////////////////////////////	ITEM CREATION
////////////////////////////////////////////////

void create_transparent_click_zone(lv_obj_t *parent, lv_obj_t *target_label,
								   uint8_t value_id) {
	lv_obj_t *click_zone = lv_obj_create(parent);

	// Check if this is a bar (BAR1_VALUE_ID=12 or BAR2_VALUE_ID=13) and adjust
	// size accordingly
	if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
		// For bars, create a click zone that covers the full bar width and
		// height
		lv_obj_set_size(click_zone, 300, 30); // Match the bar dimensions
	} else {
		// For other elements, use the standard 60x60 size
		lv_obj_set_size(click_zone, 60, 60);
	}

	lv_obj_align_to(click_zone, target_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_clear_flag(click_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_flag(click_zone, LV_OBJ_FLAG_CLICKABLE);

	// Add touch events for quick tap detection (to show menu button)
	lv_obj_add_event_cb(click_zone, screen3_touch_event_cb, LV_EVENT_PRESSED,
						NULL);
	lv_obj_add_event_cb(click_zone, screen3_touch_event_cb, LV_EVENT_RELEASED,
						NULL);

	// Allocate memory to store value_id and pass it to the event callback
	uint8_t *id_ptr = lv_mem_alloc(sizeof(uint8_t));
	*id_ptr = value_id;
	lv_obj_add_event_cb(click_zone, value_long_press_event_cb,
						LV_EVENT_LONG_PRESSED, id_ptr);
	lv_obj_add_event_cb(click_zone, free_value_id_event_cb, LV_EVENT_DELETE,
						id_ptr);
}
void init_boxes_and_arcs(void) {
	for (uint8_t i = 0; i < 8; i++) {
		// Create Box
		ui_Box[i] = lv_obj_create(ui_Screen3);
		lv_obj_set_size(ui_Box[i], 155, 92);
		lv_obj_set_pos(ui_Box[i], box_positions[i][0], box_positions[i][1]);
		lv_obj_set_align(ui_Box[i], LV_ALIGN_CENTER);
		lv_obj_clear_flag(ui_Box[i], LV_OBJ_FLAG_SCROLLABLE);
		// Enable content clipping so children don't overflow
		lv_obj_set_style_clip_corner(ui_Box[i], true,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_add_style(ui_Box[i], &box_style,
						 LV_PART_MAIN | LV_STATE_DEFAULT);

		// Add touch events to boxes for quick tap detection (to show menu
		// button)
		lv_obj_add_event_cb(ui_Box[i], screen3_touch_event_cb, LV_EVENT_PRESSED,
							NULL);
		lv_obj_add_event_cb(ui_Box[i], screen3_touch_event_cb,
							LV_EVENT_RELEASED, NULL);
	}
}

void widget_panel_create(lv_obj_t *parent) {
	init_boxes_and_arcs();
	for (uint8_t i = 0; i < 8; i++) {
		/* ── Header label (already inside box) ──────────────────────── */
		ui_Label[i] = lv_label_create(ui_Box[i]);
		{
			panel_data_t *fpd = _get_panel_data_by_slot(i);
			lv_label_set_text(ui_Label[i], fpd ? fpd->label : "---");
		}
		lv_obj_set_style_text_color(ui_Label[i], THEME_COLOR_TEXT_PRIMARY,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_opa(ui_Label[i], 255,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(ui_Label[i], THEME_FONT_DASH_LABEL,
								   LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_align(ui_Label[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(ui_Label[i], 145);
		lv_label_set_long_mode(ui_Label[i], LV_LABEL_LONG_CLIP);
		/* relative_y: label_positions - box_positions ≈ -28 for all slots */
		lv_coord_t relative_y = label_positions[i][1] - box_positions[i][1];
		lv_obj_set_x(ui_Label[i], 0);
		lv_obj_set_y(ui_Label[i], relative_y);
		lv_obj_set_align(ui_Label[i], LV_ALIGN_CENTER);

		/* ── Value label — now a child of ui_Box[i] ─────────────────── *
		 * Relative position: value_positions - box_positions ≈ (0, +9). *
		 * This means the label moves with the box automatically when the *
		 * layout manager repositions ui_Box.                             */
		ui_Value[i] = lv_label_create(ui_Box[i]);
		lv_label_set_text(ui_Value[i], "---");
		strcpy(previous_values[i], "---");
		lv_obj_set_style_text_color(ui_Value[i], THEME_COLOR_TEXT_PRIMARY,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_opa(ui_Value[i], 255,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(ui_Value[i], THEME_FONT_DASH_VALUE,
								   LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_align(ui_Value[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(ui_Value[i], 140);
		lv_label_set_long_mode(ui_Value[i], LV_LABEL_LONG_CLIP);
		/* Relative y inside 155×92 box: value_pos.y - box_pos.y */
		lv_coord_t val_rel_y = value_positions[i][1] - box_positions[i][1];
		lv_obj_set_x(ui_Value[i], 0);
		lv_obj_set_y(ui_Value[i], val_rel_y);
		lv_obj_set_align(ui_Value[i], LV_ALIGN_CENTER);

		/* Click zone lives inside the box so it tracks with it */
		create_transparent_click_zone(ui_Box[i], ui_Value[i], i + 1);

		/* ── Custom unit text — also a child of ui_Box[i] ───────────── */
		ui_CustomText[i] = lv_label_create(ui_Box[i]);
		{
			panel_data_t *fpd2 = _get_panel_data_by_slot(i);
			const char *ct = (fpd2 && fpd2->custom_text[0]) ? fpd2->custom_text : "";
			lv_label_set_text(ui_CustomText[i], ct);
			lv_obj_set_style_text_color(ui_CustomText[i], THEME_COLOR_TEXT_MUTED,
										LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_text_opa(ui_CustomText[i], 255,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_text_font(ui_CustomText[i], THEME_FONT_BODY,
									   LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_text_align(ui_CustomText[i], LV_TEXT_ALIGN_RIGHT,
										LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_width(ui_CustomText[i], 60);
			lv_label_set_long_mode(ui_CustomText[i], LV_LABEL_LONG_CLIP);
			lv_obj_set_x(ui_CustomText[i], 41);
			lv_obj_set_y(ui_CustomText[i], 32);
			lv_obj_set_align(ui_CustomText[i], LV_ALIGN_CENTER);
			if (ct[0] == '\0')
				lv_obj_add_flag(ui_CustomText[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

uint64_t *widget_panel_get_last_can_time(uint8_t idx) {
	return &last_panel_can_received[idx & 7];
}

/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

/* Panel sizes are fixed at 155×92; positions match box_positions[] above.
 * Pixel offsets are relative to screen centre (LV_ALIGN_CENTER). */
static const int16_t s_panel_default_x[8] = {-312, -146, -312, -146,
											 146,  312,	 146,  312};
static const int16_t s_panel_default_y[8] = {-26, -26, 82, 82,
											 -26, -26, 82, 82};

/* create vtable adapter: creates a single panel box for the given slot.
 * Only panels present in the JSON layout will be created. */
static void _panel_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;
	uint8_t slot = pd->slot;
	if (slot >= 8) return;
	if (is_stale) {
		update_panel_ui_immediate(slot, "---", 0.0);
		return;
	}
	char buf[32];
	if (pd->decimals == 0)
		snprintf(buf, sizeof(buf), "%d", (int)value);
	else
		snprintf(buf, sizeof(buf), "%.*f", pd->decimals, (double)value);
	update_panel_ui_immediate(slot, buf, (double)value);
}

static void _panel_create(widget_t *w, lv_obj_t *parent) {
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;
	uint8_t slot = pd->slot;
	if (slot >= 8)
		return;

	/* Create single box for this slot */
	ui_Box[slot] = lv_obj_create(parent);
	lv_obj_set_size(ui_Box[slot], w->w, w->h);
	lv_obj_set_pos(ui_Box[slot], w->x, w->y);
	lv_obj_set_align(ui_Box[slot], LV_ALIGN_CENTER);
	lv_obj_clear_flag(ui_Box[slot], LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_clip_corner(ui_Box[slot], true,
								 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_style(ui_Box[slot], &box_style,
					 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(ui_Box[slot], screen3_touch_event_cb, LV_EVENT_PRESSED,
						NULL);
	lv_obj_add_event_cb(ui_Box[slot], screen3_touch_event_cb,
						LV_EVENT_RELEASED, NULL);

	/* Header label */
	ui_Label[slot] = lv_label_create(ui_Box[slot]);
	lv_label_set_text(ui_Label[slot], pd->label);
	lv_obj_set_style_text_color(ui_Label[slot], THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Label[slot], 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Label[slot], THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_Label[slot], LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(ui_Label[slot], w->w - 10);
	lv_label_set_long_mode(ui_Label[slot], LV_LABEL_LONG_CLIP);
	lv_coord_t relative_y = label_positions[slot][1] - box_positions[slot][1];
	lv_obj_set_x(ui_Label[slot], 0);
	lv_obj_set_y(ui_Label[slot], relative_y);
	lv_obj_set_align(ui_Label[slot], LV_ALIGN_CENTER);

	/* Value label */
	ui_Value[slot] = lv_label_create(ui_Box[slot]);
	lv_label_set_text(ui_Value[slot], "---");
	strcpy(previous_values[slot], "---");
	lv_obj_set_style_text_color(ui_Value[slot], THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Value[slot], 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Value[slot], THEME_FONT_DASH_VALUE,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_Value[slot], LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(ui_Value[slot], w->w - 15);
	lv_label_set_long_mode(ui_Value[slot], LV_LABEL_LONG_CLIP);
	lv_coord_t val_rel_y = value_positions[slot][1] - box_positions[slot][1];
	lv_obj_set_x(ui_Value[slot], 0);
	lv_obj_set_y(ui_Value[slot], val_rel_y);
	lv_obj_set_align(ui_Value[slot], LV_ALIGN_CENTER);

	/* Click zone */
	create_transparent_click_zone(ui_Box[slot], ui_Value[slot], slot + 1);

	/* Custom unit text */
	ui_CustomText[slot] = lv_label_create(ui_Box[slot]);
	lv_label_set_text(ui_CustomText[slot], pd->custom_text);
	lv_obj_set_style_text_color(ui_CustomText[slot], THEME_COLOR_TEXT_MUTED,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_CustomText[slot], 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_CustomText[slot], THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_CustomText[slot], LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_width(ui_CustomText[slot], 60);
	lv_label_set_long_mode(ui_CustomText[slot], LV_LABEL_LONG_CLIP);
	lv_obj_set_x(ui_CustomText[slot], 41);
	lv_obj_set_y(ui_CustomText[slot], 32);
	lv_obj_set_align(ui_CustomText[slot], LV_ALIGN_CENTER);
	if (strlen(pd->custom_text) == 0)
		lv_obj_add_flag(ui_CustomText[slot], LV_OBJ_FLAG_HIDDEN);

	w->root = ui_Box[slot];

	/* Subscribe to signal if bound */
	if (pd->signal_index >= 0)
		signal_subscribe(pd->signal_index, _panel_on_signal, w);
}

static void _panel_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (!w->root || !lv_obj_is_valid(w->root))
		return;
	lv_obj_set_size(w->root, nw, nh);
	w->w = nw;
	w->h = nh;
}
static void _panel_open_settings(widget_t *w) {
	(void)w; /* triggered via LVGL long-press */
}
static void _panel_to_json(widget_t *w, cJSON *out) {
	panel_data_t *pd = (panel_data_t *)w->type_data;
	widget_base_to_json(w, out);
	if (!pd) return;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	cJSON_AddNumberToObject(cfg, "slot", pd->slot);
	cJSON_AddStringToObject(cfg, "label", pd->label);
	cJSON_AddStringToObject(cfg, "custom_text", pd->custom_text);
	cJSON_AddNumberToObject(cfg, "decimals", pd->decimals);
	if (pd->warning_high_enabled) {
		cJSON_AddBoolToObject(cfg, "warning_high_enabled", true);
		cJSON_AddNumberToObject(cfg, "warning_high_threshold", pd->warning_high_threshold);
		cJSON_AddNumberToObject(cfg, "warning_high_color", (int)pd->warning_high_color.full);
	}
	if (pd->warning_low_enabled) {
		cJSON_AddBoolToObject(cfg, "warning_low_enabled", true);
		cJSON_AddNumberToObject(cfg, "warning_low_threshold", pd->warning_low_threshold);
		cJSON_AddNumberToObject(cfg, "warning_low_color", (int)pd->warning_low_color.full);
	}
	if (pd->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", pd->signal_name);
}
static void _panel_from_json(widget_t *w, cJSON *in) {
	panel_data_t *pd = (panel_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!pd) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;

	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	if (cJSON_IsNumber(item)) pd->slot = (uint8_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "label");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(pd->label, item->valuestring, sizeof(pd->label) - 1);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "custom_text");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(pd->custom_text, item->valuestring, sizeof(pd->custom_text) - 1);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "decimals");
	if (cJSON_IsNumber(item)) pd->decimals = (uint8_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_enabled");
	if (cJSON_IsBool(item)) pd->warning_high_enabled = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_threshold");
	if (cJSON_IsNumber(item)) pd->warning_high_threshold = (float)item->valuedouble;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_color");
	if (cJSON_IsNumber(item)) pd->warning_high_color.full = (uint32_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_enabled");
	if (cJSON_IsBool(item)) pd->warning_low_enabled = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_threshold");
	if (cJSON_IsNumber(item)) pd->warning_low_threshold = (float)item->valuedouble;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_color");
	if (cJSON_IsNumber(item)) pd->warning_low_color.full = (uint32_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(pd->signal_name, item->valuestring, sizeof(pd->signal_name) - 1);

	/* Resolve signal name → index */
	if (pd->signal_name[0] != '\0')
		pd->signal_index = signal_find_by_name(pd->signal_name);
}
static void _panel_destroy(widget_t *w) {
	if (w) {
		free(w->type_data);
		free(w);
	}
}

widget_t *widget_panel_create_instance(uint8_t slot) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	/* Allocate per-instance data in PSRAM, fall back to internal RAM */
	panel_data_t *pd = heap_caps_calloc(1, sizeof(panel_data_t), MALLOC_CAP_SPIRAM);
	if (!pd)
		pd = calloc(1, sizeof(panel_data_t));
	if (!pd) {
		free(w);
		return NULL;
	}

	pd->slot = slot < 8 ? slot : 0;
	pd->signal_index = -1;
	/* Defaults — actual config comes from _from_json() when loading layouts */
	snprintf(pd->label, sizeof(pd->label), "Panel %u", pd->slot + 1);

	w->type = WIDGET_PANEL;
	w->x = s_panel_default_x[pd->slot];
	w->y = s_panel_default_y[pd->slot];
	w->w = 155;
	w->h = 92;
	w->type_data = pd;
	snprintf(w->id, sizeof(w->id), "panel_%u", slot);

	w->create = _panel_create;
	w->resize = _panel_resize;
	w->open_settings = _panel_open_settings;
	w->to_json = _panel_to_json;
	w->from_json = _panel_from_json;
	w->destroy = _panel_destroy;

	return w;
}

uint8_t widget_panel_get_slot(const widget_t *w) {
	if (!w || w->type != WIDGET_PANEL || !w->type_data) return 0;
	return ((const panel_data_t *)w->type_data)->slot;
}

bool widget_panel_has_signal(const widget_t *w) {
	if (!w || w->type != WIDGET_PANEL || !w->type_data) return false;
	return ((const panel_data_t *)w->type_data)->signal_index >= 0;
}
