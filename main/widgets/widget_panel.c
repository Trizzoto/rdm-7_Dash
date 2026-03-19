#include "widget_panel.h"
#include "widget_rules.h"
#include "can/can_decode.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/dashboard.h"
#include "widget_registry.h"
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

static uint64_t last_panel_can_received[8] = {0};

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
/* ── Helper: look up panel_data_t by slot via registry ─────────────────── */
static panel_data_t *_lookup_panel_data(uint8_t slot) {
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_PANEL, slot);
	return w ? (panel_data_t *)w->type_data : NULL;
}

/* ── Public setters for panel warning thresholds (called from widget_warning.c) ── */
void widget_panel_set_warning_high(uint8_t slot, float threshold, bool enabled) {
	panel_data_t *pd = _lookup_panel_data(slot);
	if (!pd) return;
	pd->warning_high_threshold = threshold;
	pd->warning_high_enabled = enabled;
}

void widget_panel_set_warning_low(uint8_t slot, float threshold, bool enabled) {
	panel_data_t *pd = _lookup_panel_data(slot);
	if (!pd) return;
	pd->warning_low_threshold = threshold;
	pd->warning_low_enabled = enabled;
}

void widget_panel_set_warning_high_color(uint8_t slot, lv_color_t color) {
	panel_data_t *pd = _lookup_panel_data(slot);
	if (!pd) return;
	pd->warning_high_color = color;
}

void widget_panel_set_warning_low_color(uint8_t slot, lv_color_t color) {
	panel_data_t *pd = _lookup_panel_data(slot);
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

	if (menu_panel_value_labels[i] &&
		lv_obj_is_valid(menu_panel_value_labels[i]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_panel_value_labels[i], update->value_str);
	}

	panel_data_t *pd = _lookup_panel_data(i);

	/* Determine warning state and apply-to flags */
	lv_color_t warn_color = {0};
	bool a_label = false, a_value = false, a_panel = false;
	bool is_stale = (strcmp(update->value_str, "---") == 0);
	if (!is_stale && pd && pd->warning_high_enabled &&
		update->final_value > pd->warning_high_threshold) {
		warn_color = pd->warning_high_color;
		a_label = pd->warning_high_apply_label;
		a_value = pd->warning_high_apply_value;
		a_panel = pd->warning_high_apply_panel;
	} else if (!is_stale && pd && pd->warning_low_enabled &&
			   update->final_value < pd->warning_low_threshold) {
		warn_color = pd->warning_low_color;
		a_label = pd->warning_low_apply_label;
		a_value = pd->warning_low_apply_value;
		a_panel = pd->warning_low_apply_panel;
	}

	/* Apply label color */
	if (ui_Label[i] && lv_obj_is_valid(ui_Label[i]))
		lv_obj_set_style_text_color(ui_Label[i],
			a_label ? warn_color : THEME_COLOR_TEXT_PRIMARY,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Apply value color */
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]))
		lv_obj_set_style_text_color(ui_Value[i],
			a_value ? warn_color : THEME_COLOR_TEXT_PRIMARY,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Apply panel border */
	if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) &&
		lv_obj_get_screen(ui_Box[i]) != NULL) {
		lv_obj_set_style_border_color(ui_Box[i],
			a_panel ? warn_color : THEME_COLOR_PANEL,
			LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(ui_Box[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(ui_Box[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Menu preview panel border */
	if (menu_panel_boxes[i] && lv_obj_is_valid(menu_panel_boxes[i]) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		lv_obj_set_style_border_color(menu_panel_boxes[i],
			a_panel ? warn_color : THEME_COLOR_PANEL,
			LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(menu_panel_boxes[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(menu_panel_boxes[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	free(update);
}

// Immediate (no-alloc, no-async) panel update
void update_panel_ui_immediate(uint8_t i, const char *value_str,
							   double final_value) {
	if (i >= 8)
		return;
	panel_data_t *pd = _lookup_panel_data(i);
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]) &&
		lv_obj_get_screen(ui_Value[i]) != NULL) {
		lv_label_set_text(ui_Value[i], value_str);
	}
	if (menu_panel_value_labels[i] &&
		lv_obj_is_valid(menu_panel_value_labels[i]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_panel_value_labels[i], value_str);
	}

	/* Determine warning state and apply-to flags */
	lv_color_t wc = {0};
	bool al = false, av = false, ap = false;
	bool stale = (strcmp(value_str, "---") == 0);
	if (!stale && pd && pd->warning_high_enabled &&
		final_value > pd->warning_high_threshold) {
		wc = pd->warning_high_color;
		al = pd->warning_high_apply_label;
		av = pd->warning_high_apply_value;
		ap = pd->warning_high_apply_panel;
	} else if (!stale && pd && pd->warning_low_enabled &&
			   final_value < pd->warning_low_threshold) {
		wc = pd->warning_low_color;
		al = pd->warning_low_apply_label;
		av = pd->warning_low_apply_value;
		ap = pd->warning_low_apply_panel;
	}

	if (ui_Label[i] && lv_obj_is_valid(ui_Label[i]))
		lv_obj_set_style_text_color(ui_Label[i],
			al ? wc : THEME_COLOR_TEXT_PRIMARY,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]))
		lv_obj_set_style_text_color(ui_Value[i],
			av ? wc : THEME_COLOR_TEXT_PRIMARY,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) &&
		lv_obj_get_screen(ui_Box[i]) != NULL) {
		lv_obj_set_style_border_color(ui_Box[i],
			ap ? wc : THEME_COLOR_PANEL,
			LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(ui_Box[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(ui_Box[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (menu_panel_boxes[i] && lv_obj_is_valid(menu_panel_boxes[i]) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		lv_obj_set_style_border_color(menu_panel_boxes[i],
			ap ? wc : THEME_COLOR_PANEL,
			LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(menu_panel_boxes[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(menu_panel_boxes[i], 255,
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
			panel_data_t *fpd = _lookup_panel_data(i);
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

		/* ── Custom unit text — also a child of ui_Box[i] ───────────── */
		ui_CustomText[i] = lv_label_create(ui_Box[i]);
		{
			panel_data_t *fpd2 = _lookup_panel_data(i);
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

	const char *display_str;
	char buf[32];
	if (is_stale) {
		display_str = "---";
	} else {
		if (pd->decimals == 0)
			snprintf(buf, sizeof(buf), "%d", (int)value);
		else
			snprintf(buf, sizeof(buf), "%.*f", pd->decimals, (double)value);
		display_str = buf;
	}

	double final_value = is_stale ? 0.0 : (double)value;

	/* Determine active warning color (if any) */
	lv_color_t warn_color = {0};
	bool apply_label = false, apply_value = false, apply_panel = false;
	if (!is_stale && pd->warning_high_enabled &&
		final_value > pd->warning_high_threshold) {
		warn_color = pd->warning_high_color;
		apply_label = pd->warning_high_apply_label;
		apply_value = pd->warning_high_apply_value;
		apply_panel = pd->warning_high_apply_panel;
	} else if (!is_stale && pd->warning_low_enabled &&
			   final_value < pd->warning_low_threshold) {
		warn_color = pd->warning_low_color;
		apply_label = pd->warning_low_apply_label;
		apply_value = pd->warning_low_apply_value;
		apply_panel = pd->warning_low_apply_panel;
	}

	/* Update this widget's own LVGL objects directly (per-instance pointers).
	 * This is the authoritative path — avoids cross-talk via global arrays
	 * if two panels share a slot due to misconfiguration. */
	if (pd->value_label && lv_obj_is_valid(pd->value_label)) {
		lv_label_set_text(pd->value_label, display_str);
		lv_color_t val_color = apply_value ? warn_color : pd->value_color;
		lv_obj_set_style_text_color(pd->value_label, val_color,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (pd->header_label && lv_obj_is_valid(pd->header_label)) {
		lv_color_t lbl_color = apply_label ? warn_color : pd->label_color;
		lv_obj_set_style_text_color(pd->header_label, lbl_color,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (pd->box && lv_obj_is_valid(pd->box)) {
		lv_color_t bdr_color = apply_panel ? warn_color : pd->border_color;
		lv_obj_set_style_border_color(pd->box, bdr_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(pd->box, pd->border_width,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(pd->box, 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Update menu panel preview if the menu screen is active */
	if (slot < 8 && menu_panel_value_labels[slot] &&
		lv_obj_is_valid(menu_panel_value_labels[slot]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_panel_value_labels[slot], display_str);
	}
	if (slot < 8 && menu_panel_boxes[slot] &&
		lv_obj_is_valid(menu_panel_boxes[slot]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_obj_set_style_border_color(menu_panel_boxes[slot],
			apply_panel ? warn_color : THEME_COLOR_PANEL,
			LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(menu_panel_boxes[slot], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(menu_panel_boxes[slot], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

static void _panel_create(widget_t *w, lv_obj_t *parent) {
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;
	uint8_t slot = pd->slot;
	if (slot >= 8)
		return;

	/* Create single box for this slot */
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_set_size(box, w->w, w->h);
	lv_obj_set_pos(box, w->x, w->y);
	lv_obj_set_align(box, LV_ALIGN_CENTER);
	lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_clip_corner(box, true,
								 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(box, pd->border_radius, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(box, pd->bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(box, pd->bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_clip_corner(box, false, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(box, pd->border_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(box, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(box, pd->border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_post(box, true, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_width(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_pad(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(box, screen3_touch_event_cb, LV_EVENT_PRESSED,
						NULL);
	lv_obj_add_event_cb(box, screen3_touch_event_cb,
						LV_EVENT_RELEASED, NULL);

	/* Header label */
	lv_obj_t *hdr = lv_label_create(box);
	lv_label_set_text(hdr, pd->label);
	lv_obj_set_style_text_color(hdr, pd->label_color,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(hdr, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *lbl_font = widget_resolve_font(pd->label_font);
	lv_obj_set_style_text_font(hdr, lbl_font ? lbl_font : THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(hdr, w->w - 10);
	lv_label_set_long_mode(hdr, LV_LABEL_LONG_CLIP);
	lv_obj_set_x(hdr, 0);
	lv_obj_set_y(hdr, pd->label_y_offset);
	lv_obj_set_align(hdr, LV_ALIGN_CENTER);

	/* Value label */
	lv_obj_t *val = lv_label_create(box);
	lv_label_set_text(val, "---");
	lv_obj_set_style_text_color(val, pd->value_color,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(val, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *val_font = widget_resolve_font(pd->value_font);
	lv_obj_set_style_text_font(val, val_font ? val_font : THEME_FONT_DASH_VALUE,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(val, w->w - 15);
	lv_label_set_long_mode(val, LV_LABEL_LONG_CLIP);
	lv_obj_set_x(val, 0);
	lv_obj_set_y(val, pd->value_y_offset);
	lv_obj_set_align(val, LV_ALIGN_CENTER);

	/* Custom unit text */
	lv_obj_t *ctxt = lv_label_create(box);
	lv_label_set_text(ctxt, pd->custom_text);
	lv_obj_set_style_text_color(ctxt, THEME_COLOR_TEXT_MUTED,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ctxt, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ctxt, THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ctxt, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_width(ctxt, 60);
	lv_label_set_long_mode(ctxt, LV_LABEL_LONG_CLIP);
	lv_obj_set_x(ctxt, pd->custom_text_x_offset);
	lv_obj_set_y(ctxt, pd->custom_text_y_offset);
	lv_obj_set_align(ctxt, LV_ALIGN_CENTER);
	if (strlen(pd->custom_text) == 0)
		lv_obj_add_flag(ctxt, LV_OBJ_FLAG_HIDDEN);

	/* Store per-instance pointers so signal callback uses the right objects */
	pd->box = box;
	pd->header_label = hdr;
	pd->value_label = val;
	pd->custom_text_label = ctxt;

	/* Also assign to global arrays for backward compat (config modal, RPM limiter) */
	if (slot < 8) {
		ui_Box[slot] = box;
		ui_Label[slot] = hdr;
		ui_Value[slot] = val;
		ui_CustomText[slot] = ctxt;
	}

	w->root = box;

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
		cJSON_AddBoolToObject(cfg, "warning_high_apply_label", pd->warning_high_apply_label);
		cJSON_AddBoolToObject(cfg, "warning_high_apply_value", pd->warning_high_apply_value);
		cJSON_AddBoolToObject(cfg, "warning_high_apply_panel", pd->warning_high_apply_panel);
	}
	if (pd->warning_low_enabled) {
		cJSON_AddBoolToObject(cfg, "warning_low_enabled", true);
		cJSON_AddNumberToObject(cfg, "warning_low_threshold", pd->warning_low_threshold);
		cJSON_AddNumberToObject(cfg, "warning_low_color", (int)pd->warning_low_color.full);
		cJSON_AddBoolToObject(cfg, "warning_low_apply_label", pd->warning_low_apply_label);
		cJSON_AddBoolToObject(cfg, "warning_low_apply_value", pd->warning_low_apply_value);
		cJSON_AddBoolToObject(cfg, "warning_low_apply_panel", pd->warning_low_apply_panel);
	}
	if (pd->label_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "label_font", pd->label_font);
	if (pd->value_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "value_font", pd->value_font);
	if (pd->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", pd->signal_name);
	/* Appearance overrides — only serialize non-default values */
	if (pd->border_radius != 7)
		cJSON_AddNumberToObject(cfg, "border_radius", pd->border_radius);
	if (pd->border_width != 3)
		cJSON_AddNumberToObject(cfg, "border_width", pd->border_width);
	if (pd->border_color.full != THEME_COLOR_PANEL.full)
		cJSON_AddNumberToObject(cfg, "border_color", (int)pd->border_color.full);
	if (pd->bg_color.full != THEME_COLOR_BG.full)
		cJSON_AddNumberToObject(cfg, "bg_color", (int)pd->bg_color.full);
	if (pd->bg_opa != 255)
		cJSON_AddNumberToObject(cfg, "bg_opa", pd->bg_opa);
	if (pd->label_color.full != THEME_COLOR_TEXT_PRIMARY.full)
		cJSON_AddNumberToObject(cfg, "label_color", (int)pd->label_color.full);
	if (pd->value_color.full != THEME_COLOR_TEXT_PRIMARY.full)
		cJSON_AddNumberToObject(cfg, "value_color", (int)pd->value_color.full);
	if (pd->label_y_offset != -28)
		cJSON_AddNumberToObject(cfg, "label_y_offset", pd->label_y_offset);
	if (pd->value_y_offset != 9)
		cJSON_AddNumberToObject(cfg, "value_y_offset", pd->value_y_offset);
	if (pd->custom_text_x_offset != 41)
		cJSON_AddNumberToObject(cfg, "custom_text_x_offset", pd->custom_text_x_offset);
	if (pd->custom_text_y_offset != 32)
		cJSON_AddNumberToObject(cfg, "custom_text_y_offset", pd->custom_text_y_offset);
}
static void _panel_from_json(widget_t *w, cJSON *in) {
	panel_data_t *pd = (panel_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!pd) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;

	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	if (cJSON_IsNumber(item)) {
		pd->slot = (uint8_t)item->valueint;
		w->slot = pd->slot;
	}

	item = cJSON_GetObjectItemCaseSensitive(cfg, "label");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(pd->label, item->valuestring, sizeof(pd->label));

	item = cJSON_GetObjectItemCaseSensitive(cfg, "custom_text");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(pd->custom_text, item->valuestring, sizeof(pd->custom_text));

	item = cJSON_GetObjectItemCaseSensitive(cfg, "decimals");
	if (cJSON_IsNumber(item)) pd->decimals = (uint8_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_enabled");
	if (cJSON_IsBool(item)) pd->warning_high_enabled = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_threshold");
	if (cJSON_IsNumber(item)) pd->warning_high_threshold = (float)item->valuedouble;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_color");
	if (cJSON_IsNumber(item)) pd->warning_high_color.full = (uint32_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_apply_label");
	if (cJSON_IsBool(item)) pd->warning_high_apply_label = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_apply_value");
	if (cJSON_IsBool(item)) pd->warning_high_apply_value = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_high_apply_panel");
	if (cJSON_IsBool(item)) pd->warning_high_apply_panel = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_enabled");
	if (cJSON_IsBool(item)) pd->warning_low_enabled = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_threshold");
	if (cJSON_IsNumber(item)) pd->warning_low_threshold = (float)item->valuedouble;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_color");
	if (cJSON_IsNumber(item)) pd->warning_low_color.full = (uint32_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_apply_label");
	if (cJSON_IsBool(item)) pd->warning_low_apply_label = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_apply_value");
	if (cJSON_IsBool(item)) pd->warning_low_apply_value = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "warning_low_apply_panel");
	if (cJSON_IsBool(item)) pd->warning_low_apply_panel = cJSON_IsTrue(item);

	item = cJSON_GetObjectItemCaseSensitive(cfg, "label_font");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(pd->label_font, item->valuestring, sizeof(pd->label_font));
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "value_font");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(pd->value_font, item->valuestring, sizeof(pd->value_font));
	}

	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(pd->signal_name, item->valuestring, sizeof(pd->signal_name));

	/* Appearance overrides */
	item = cJSON_GetObjectItemCaseSensitive(cfg, "border_radius");
	if (cJSON_IsNumber(item)) pd->border_radius = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "border_width");
	if (cJSON_IsNumber(item)) pd->border_width = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "border_color");
	if (cJSON_IsNumber(item)) pd->border_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_color");
	if (cJSON_IsNumber(item)) pd->bg_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_opa");
	if (cJSON_IsNumber(item)) pd->bg_opa = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label_color");
	if (cJSON_IsNumber(item)) pd->label_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "value_color");
	if (cJSON_IsNumber(item)) pd->value_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "label_y_offset");
	if (cJSON_IsNumber(item)) pd->label_y_offset = (int8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "value_y_offset");
	if (cJSON_IsNumber(item)) pd->value_y_offset = (int8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "custom_text_x_offset");
	if (cJSON_IsNumber(item)) pd->custom_text_x_offset = (int8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "custom_text_y_offset");
	if (cJSON_IsNumber(item)) pd->custom_text_y_offset = (int8_t)item->valueint;

	/* Resolve signal name → index */
	if (pd->signal_name[0] != '\0')
		pd->signal_index = signal_find_by_name(pd->signal_name);
}
static void _panel_destroy(widget_t *w) {
	if (w) {
		panel_data_t *pd = (panel_data_t *)w->type_data;
		if (pd && pd->signal_index >= 0)
			signal_unsubscribe(pd->signal_index, _panel_on_signal, w);
		widget_rules_free(w);
		if (w->root && lv_obj_is_valid(w->root))
			lv_obj_del(w->root);
		w->root = NULL;
		free(w->type_data);
		free(w);
	}
}

static void _panel_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;

	/* Start from base panel_data_t values (restore defaults) */
	lv_color_t bg_color      = pd->bg_color;
	uint8_t    bg_opa        = pd->bg_opa;
	lv_color_t bdr_color     = pd->border_color;
	uint8_t    bdr_width     = pd->border_width;
	uint8_t    bdr_radius    = pd->border_radius;
	lv_color_t label_color   = pd->label_color;
	lv_color_t value_color   = pd->value_color;
	const char *lbl_font_name = pd->label_font;
	const char *val_font_name = pd->value_font;

	/* Apply active overrides on top */
	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bg_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "bg_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bg_opa = (uint8_t)o->value.num;
		} else if (strcmp(o->field_name, "border_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bdr_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "border_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bdr_width = (uint8_t)o->value.num;
		} else if (strcmp(o->field_name, "border_radius") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bdr_radius = (uint8_t)o->value.num;
		} else if (strcmp(o->field_name, "label_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			label_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "value_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			value_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "label_font") == 0 && o->value_type == RULE_VAL_STRING) {
			lbl_font_name = o->value.str;
		} else if (strcmp(o->field_name, "value_font") == 0 && o->value_type == RULE_VAL_STRING) {
			val_font_name = o->value.str;
		}
	}

	/* Apply all styles (either overridden or restored to base) */
	if (pd->box && lv_obj_is_valid(pd->box)) {
		lv_obj_set_style_bg_color(pd->box, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(pd->box, bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(pd->box, bdr_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(pd->box, bdr_width, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(pd->box, bdr_radius, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (pd->header_label && lv_obj_is_valid(pd->header_label)) {
		lv_obj_set_style_text_color(pd->header_label, label_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		const lv_font_t *lf = widget_resolve_font(lbl_font_name);
		lv_obj_set_style_text_font(pd->header_label,
			lf ? lf : THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (pd->value_label && lv_obj_is_valid(pd->value_label)) {
		lv_obj_set_style_text_color(pd->value_label, value_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		const lv_font_t *vf = widget_resolve_font(val_font_name);
		lv_obj_set_style_text_font(pd->value_label,
			vf ? vf : THEME_FONT_DASH_VALUE, LV_PART_MAIN | LV_STATE_DEFAULT);
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
	/* Warning "apply to" defaults: label + value coloured, panel border not */
	pd->warning_high_apply_label = true;
	pd->warning_high_apply_value = true;
	pd->warning_high_apply_panel = false;
	pd->warning_low_apply_label = true;
	pd->warning_low_apply_value = true;
	pd->warning_low_apply_panel = false;
	/* Defaults — actual config comes from _from_json() when loading layouts */
	snprintf(pd->label, sizeof(pd->label), "Panel %u", pd->slot + 1);
	pd->border_radius = 7;
	pd->border_width = 3;
	pd->border_color = THEME_COLOR_PANEL;
	pd->bg_color = THEME_COLOR_BG;
	pd->bg_opa = 255;
	pd->label_color = THEME_COLOR_TEXT_PRIMARY;
	pd->value_color = THEME_COLOR_TEXT_PRIMARY;
	pd->label_y_offset = -28;
	pd->value_y_offset = 9;
	pd->custom_text_x_offset = 41;
	pd->custom_text_y_offset = 32;

	w->type = WIDGET_PANEL;
	w->slot = pd->slot;
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
	w->apply_overrides = _panel_apply_overrides;

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
