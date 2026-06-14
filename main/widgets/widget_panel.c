#include "widget_panel.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "data/channel_manager.h"
#include <float.h>  /* FLT_MAX for peak/min sentinels */
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
	bool stale = (strcmp(value_str, "--") == 0);
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

	/* Baseline colors must honour night mode: when no warning is active, the
	 * label/value/border should fall back to the per-widget night override
	 * (if any) instead of the day theme. Otherwise every CAN frame this path
	 * runs would repaint over _panel_apply_night_mode and "merge" the look. */
	bool night_on = night_mode_is_active();
	lv_color_t base_label = pd
		? NIGHT_PICK_COLOR(night_on, pd->night, label_color, pd->label_color)
		: THEME_COLOR_TEXT_PRIMARY;
	lv_color_t base_value = pd
		? NIGHT_PICK_COLOR(night_on, pd->night, value_color, pd->value_color)
		: THEME_COLOR_TEXT_PRIMARY;
	lv_color_t base_border = pd
		? NIGHT_PICK_COLOR(night_on, pd->night, border_color, pd->border_color)
		: THEME_COLOR_PANEL;

	if (ui_Label[i] && lv_obj_is_valid(ui_Label[i]))
		lv_obj_set_style_text_color(ui_Label[i],
			al ? wc : base_label,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]))
		lv_obj_set_style_text_color(ui_Value[i],
			av ? wc : base_value,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) &&
		lv_obj_get_screen(ui_Box[i]) != NULL) {
		lv_obj_set_style_border_color(ui_Box[i],
			ap ? wc : base_border,
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
			ap ? wc : base_border,
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
		lv_label_set_text(ui_Value[i], "--");
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

/* Forward decl: the channel-changed handler below re-points this signal
 * subscription, but the definition lives further down (after the factory). */
static void _panel_on_signal(float value, bool is_stale, void *user_data);

/* Channel-changed listener: fires when the user edits a bound channel's
 * thresholds, range, signal binding, etc. Re-snapshot the relevant
 * fields and invalidate so the next signal callback paints with new
 * threshold values. */
static void _panel_on_channel_changed(channel_t *c, void *user_data) {
	if (!c || !user_data) return;
	widget_t *w = (widget_t *)user_data;
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;

	/* Snapshot channel → widget legacy fields. Render code reads the
	 * widget's fields directly, so this is how channel edits propagate. */
	safe_strncpy(pd->signal_name, c->signal_name, sizeof(pd->signal_name));
	/* Re-point our own signal subscription when the channel re-resolves to a
	 * different underlying signal. The signal registry keys subscribers by the
	 * index they attached to at create, so copying the new index alone would
	 * leave _panel_on_signal bound to the OLD signal → the panel freezes on
	 * its last value. Mirror the inspector signal-swap path (unsub old / sub
	 * new). Safe here: the channel-changed callback runs under rdm_lvgl_lock. */
	int16_t new_idx = c->signal_index;
	if (new_idx != pd->signal_index) {
		if (pd->signal_index >= 0)
			signal_unsubscribe(pd->signal_index, _panel_on_signal, w);
		pd->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _panel_on_signal, w);
	}
	/* Threshold VALUES + enabled are owned by the channel. When the channel
	 * has no warn set, the alert reverts to inactive (so we don't paint a
	 * stale/always-on warning). Colours stay widget-owned (below). */
	if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
		pd->warning_high_enabled = true;
		pd->warning_high_threshold = (float)c->high_warn;
	} else {
		pd->warning_high_enabled = false;
	}
	if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW) {
		pd->warning_low_enabled = true;
		pd->warning_low_threshold = (float)c->low_warn;
	} else {
		pd->warning_low_enabled = false;
	}
	/* Alert COLOURS are widget-owned (Widget settings → ALERTS) and are never
	 * driven by the channel — the channel-side colour pickers were removed, so
	 * adopting channel colours here just clobbered the user's choice with a
	 * stale captured value (the "always red" bug). Only thresholds / range /
	 * signal sync from the channel. */

	/* Re-render the value now: a display-unit (or decimals) change must show
	 * immediately, but the signal only re-notifies on a value CHANGE, so a
	 * constant/frozen signal would otherwise keep the old unit until the
	 * value next moves. Re-format the channel's current value in the new
	 * unit (paint-memo lets it through since the string differs). */
	_panel_on_signal(c->current_value, c->is_stale, w);
	if (pd->box && lv_obj_is_valid(pd->box)) lv_obj_invalidate(pd->box);
}

/* ── Unit-suffix helpers ────────────────────────────────────────────────── */
/* The bound channel's display unit, or NULL when not shown / unavailable. */
static const char *_panel_unit_str(const panel_data_t *pd) {
	if (!pd->show_unit || !pd->channel) return NULL;
	const char *u = ((const channel_t *)pd->channel)->units_display;
	return (u && u[0]) ? u : NULL;
}
/* A smaller font for the unit derived from the value font: small ~0.5x,
 * medium ~0.72x. Falls back to theme fonts when value_font is the theme
 * default ("Family:size" is the dynamic-TTF form the size is parsed from). */
static const lv_font_t *_panel_unit_font(const char *value_font, uint8_t size_mode) {
	if (value_font && value_font[0]) {
		const char *colon = strrchr(value_font, ':');
		if (colon) {
			int sz = atoi(colon + 1);
			if (sz > 0) {
				int nsz = (size_mode == 0) ? (sz * 50) / 100 : (sz * 72) / 100;
				if (nsz < 8) nsz = 8;
				size_t fl = (size_t)(colon - value_font);
				if (fl < 40) {
					char b[48];
					memcpy(b, value_font, fl);
					snprintf(b + fl, sizeof(b) - fl, ":%d", nsz);
					const lv_font_t *f = widget_resolve_font(b);
					if (f) return f;
				}
			}
		}
	}
	return (size_mode == 0) ? THEME_FONT_SMALL : THEME_FONT_MEDIUM;
}

/* create vtable adapter: creates a single panel box for the given slot.
 * Only panels present in the JSON layout will be created. */
static void _panel_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;
	uint8_t slot = pd->slot;

	const char *display_str;
	char buf[32];
	if (is_stale) {
		display_str = "--";
	} else {
		/* channel_format_display_value renders in the channel's display unit
		 * when it differs from native (e.g. coolant in °F), else defers to
		 * signal_format_value so a value→label map (gear/mode) still applies.
		 * The warning thresholds below compare the NATIVE value, unchanged. */
		channel_format_display_value((const channel_t *)pd->channel,
		                             pd->signal_index, value, pd->decimals,
		                             buf, sizeof(buf));
		/* Full-size unit: append inline to the value ("40°C" / "482 kPa"); no
		 * space before a degree unit, a space before the rest. Small/medium
		 * units are a separate label (positioned below), so don't append. */
		if (pd->show_unit && pd->unit_size == 2 && pd->channel) {
			const char *u = ((const channel_t *)pd->channel)->units_display;
			if (u && u[0]) {
				size_t len = strnlen(buf, sizeof(buf));
				bool deg = ((unsigned char)u[0] == 0xC2 &&
				            (unsigned char)u[1] == 0xB0);
				if (len < sizeof(buf) - 1)
					snprintf(buf + len, sizeof(buf) - len, "%s%s",
					         deg ? "" : " ", u);
			}
		}
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

	/* Early-out: if the rendered string AND the warning-state bitmap
	 * are both unchanged since the last call, there is nothing to
	 * update. Signals can fire at hundreds of Hz while the displayed
	 * integer only changes a few times per second — this gate collapses
	 * a storm of redundant lv_label_set_text / set_style calls into the
	 * handful of actual transitions. Also stops clobbering the style
	 * overrides applied by widget_rules so the rules system no longer
	 * needs to re-assert on every single signal update. */
	uint8_t warn_state = ((uint8_t)apply_label      ) |
	                     ((uint8_t)apply_value << 1 ) |
	                     ((uint8_t)apply_panel << 2 ) |
	                     ((uint8_t)(is_stale ? 1 : 0) << 7);
	bool text_changed  = (strncmp(pd->last_display, display_str,
	                              sizeof(pd->last_display)) != 0);
	bool state_changed = (pd->last_warn_state != warn_state);
	/* Early-out collapses the per-tick restyle storm — BUT only when there is
	 * no peak line to maintain. With show_peak on we must fall through to the
	 * peak block below so a session-peak reset (which doesn't notify us) or a
	 * just-enabled peak label still re-renders; the value/warn writes stay
	 * individually gated on text_changed/state_changed, and the peak block has
	 * its own strcmp gate, so falling through stays cheap. */
	if (!text_changed && !state_changed && pd->show_peak == 0) {
		return;
	}
	/* Cache the display string. Use memcpy(size-1) + manual null terminator
	 * — GCC's -Wstringop-truncation false-flags safe_strncpy here because
	 * it cannot prove display_str's length at compile time. */
	if (text_changed) {
		size_t copy_len = strnlen(display_str, sizeof(pd->last_display) - 1);
		memcpy(pd->last_display, display_str, copy_len);
		pd->last_display[copy_len] = '\0';
	}
	pd->last_warn_state = warn_state;

	/* Update this widget's own LVGL objects directly (per-instance pointers).
	 * Style sets in LVGL v8 unconditionally invalidate regardless of whether
	 * the prop value actually changed (see lv_obj_style.c: set_local_style_prop
	 * just calls lv_obj_refresh_style), and the panel box is the biggest
	 * rect on the widget. So we gate the style writes on an actual state
	 * transition — dropping ~5 box/label invalidations per tick per panel
	 * down to 1 (the label text change) in the common no-alert sim path.
	 * border_width / border_opa are never runtime-animated; they're set
	 * once in _panel_create and not touched here. */
	if (text_changed && pd->value_label && lv_obj_is_valid(pd->value_label)) {
		lv_label_set_text(pd->value_label, display_str);
		/* The small/medium unit lives in a flex row beside the value, so LVGL
		 * keeps it adjacent automatically when the value text resizes. */
	}
	if (state_changed) {
		if (pd->value_label && lv_obj_is_valid(pd->value_label)) {
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
		}
	}

	/* Update menu panel preview if the menu screen is active. Same gating
	 * strategy as above — text on change, border color on state transition. */
	bool menu_active = (slot < 8 && ui_MenuScreen &&
	                    lv_obj_is_valid(ui_MenuScreen) &&
	                    lv_scr_act() == ui_MenuScreen);
	if (menu_active && text_changed && menu_panel_value_labels[slot] &&
		lv_obj_is_valid(menu_panel_value_labels[slot])) {
		lv_label_set_text(menu_panel_value_labels[slot], display_str);
	}
	if (menu_active && state_changed && menu_panel_boxes[slot] &&
		lv_obj_is_valid(menu_panel_boxes[slot])) {
		lv_obj_set_style_border_color(menu_panel_boxes[slot],
			apply_panel ? warn_color : THEME_COLOR_PANEL,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Peak hold display. Reads peak_value/min_value from the signal layer
	 * (always tracked) and renders into a small label below the value. We
	 * only update when show_peak is non-zero AND we have a valid signal,
	 * which keeps the cost essentially zero for the common case. */
	if (pd->show_peak != 0 && pd->peak_label &&
	    lv_obj_is_valid(pd->peak_label) && pd->signal_index >= 0) {
		signal_t *sig = signal_get_by_index(pd->signal_index);
		char pk_buf[40];
		/* Panel widgets show SESSION peaks (since boot), not the all-time
		 * persisted ones. The Peaks screen still displays the persisted
		 * values — this is so a driver sees today's high RPM, not a peak
		 * captured weeks ago that has no relevance to the current run. */
		if (!sig || (sig->session_peak == -FLT_MAX && sig->session_min == FLT_MAX)) {
			pk_buf[0] = '\0';  /* no data yet */
		} else {
			float pk = sig->session_peak;
			float mn = sig->session_min;
			int dec = pd->decimals;
			switch (pd->show_peak) {
				case 1: /* MAX */
					if (pk == -FLT_MAX) snprintf(pk_buf, sizeof(pk_buf), "MAX -");
					else if (dec == 0)  snprintf(pk_buf, sizeof(pk_buf), "MAX %d", (int)pk);
					else                snprintf(pk_buf, sizeof(pk_buf), "MAX %.*f", dec, (double)pk);
					break;
				case 2: /* MIN */
					if (mn == FLT_MAX)  snprintf(pk_buf, sizeof(pk_buf), "MIN -");
					else if (dec == 0)  snprintf(pk_buf, sizeof(pk_buf), "MIN %d", (int)mn);
					else                snprintf(pk_buf, sizeof(pk_buf), "MIN %.*f", dec, (double)mn);
					break;
				case 3: /* both */ {
					char a[16], b[16];
					if (mn == FLT_MAX) snprintf(a, sizeof(a), "-");
					else if (dec == 0) snprintf(a, sizeof(a), "%d", (int)mn);
					else               snprintf(a, sizeof(a), "%.*f", dec, (double)mn);
					if (pk == -FLT_MAX) snprintf(b, sizeof(b), "-");
					else if (dec == 0)  snprintf(b, sizeof(b), "%d", (int)pk);
					else                snprintf(b, sizeof(b), "%.*f", dec, (double)pk);
					snprintf(pk_buf, sizeof(pk_buf), "%s/%s", a, b);
					break;
				}
				default: pk_buf[0] = '\0';
			}
		}
		/* Session peaks move rarely — skip the realloc + label invalidate
		 * when the rendered peak string is unchanged. */
		if (strcmp(pd->last_peak, pk_buf) != 0) {
			lv_label_set_text(pd->peak_label, pk_buf);
			safe_strncpy(pd->last_peak, pk_buf, sizeof(pd->last_peak));
		}
	}
}

/* Forward declarations — used by _panel_create / _panel_destroy below but
 * defined further down (after _panel_apply_overrides). */
static void _panel_apply_night_mode(widget_t *w, bool active);
static void _panel_night_cb(bool active, void *user_data);

static void _panel_create(widget_t *w, lv_obj_t *parent) {
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;
	uint8_t slot = pd->slot;

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

	/* Header + value alignment: the labels are near-full-box width and aligned
	 * LV_ALIGN_CENTER, so the text_align alone places the text at the box's
	 * left / centre / right edge (used for MoTeC-style left & right columns). */
	lv_text_align_t ta = (pd->text_align == 0) ? LV_TEXT_ALIGN_LEFT
	                   : (pd->text_align == 2) ? LV_TEXT_ALIGN_RIGHT
	                   : LV_TEXT_ALIGN_CENTER;

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
	lv_obj_set_style_text_align(hdr, ta, 0);
	lv_obj_set_width(hdr, w->w - 10);
	lv_label_set_long_mode(hdr, LV_LABEL_LONG_CLIP);
	lv_obj_set_x(hdr, 0);
	lv_obj_set_y(hdr, pd->label_y_offset);
	lv_obj_set_align(hdr, LV_ALIGN_CENTER);

	/* Value label */
	lv_obj_t *val = lv_label_create(box);
	lv_label_set_text(val, "--");
	lv_obj_set_style_text_color(val, pd->value_color,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(val, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *val_font = widget_resolve_font(pd->value_font);
	lv_obj_set_style_text_font(val, val_font ? val_font : THEME_FONT_DASH_VALUE,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(val, ta, 0);
	lv_obj_set_width(val, w->w - 15);
	lv_label_set_long_mode(val, LV_LABEL_LONG_CLIP);
	lv_obj_set_x(val, 0);
	lv_obj_set_y(val, pd->value_y_offset);
	lv_obj_set_align(val, LV_ALIGN_CENTER);

	/* Small/medium unit suffix: a separate, smaller label sitting just to the
	 * right of the value text. (Full-size units are appended inline in the
	 * value text instead — see _panel_on_signal.) Value + unit live in a
	 * transparent flex row so LVGL keeps them adjacent on every layout pass —
	 * an earlier align_to() approach baked the unit's position once at create,
	 * before the content-width value had settled, and pinned it to the box's
	 * left edge until a value change happened to re-trigger it. */
	pd->unit_label = NULL;
	pd->value_row  = NULL;
	{
		const char *unit_str = _panel_unit_str(pd);
		if (unit_str && pd->unit_size < 2) {
			lv_obj_t *row = lv_obj_create(box);
			lv_obj_remove_style_all(row);
			lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
			lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
			/* main axis: pack left; cross axis: END so the smaller unit sits on
			 * the value's baseline rather than its vertical centre. */
			lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
			                      LV_FLEX_ALIGN_START);
			lv_obj_set_style_pad_column(row, 3, 0);

			/* value becomes a content-width flex item; flex drives its pos */
			lv_obj_set_width(val, LV_SIZE_CONTENT);
			lv_obj_set_parent(val, row);
			lv_obj_set_align(val, LV_ALIGN_TOP_LEFT);
			lv_obj_set_pos(val, 0, 0);

			lv_obj_t *us = lv_label_create(row);
			lv_label_set_text(us, unit_str);
			lv_obj_set_style_text_font(us,
				_panel_unit_font(pd->value_font, pd->unit_size),
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_text_color(us, pd->value_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_label_set_long_mode(us, LV_LABEL_LONG_CLIP);
			/* lift the unit a hair off the bottom so a degree glyph clears the
			 * value's descenders/baseline cleanly. */
			lv_obj_set_style_pad_bottom(us, 2, 0);

			/* place the whole row within the box per text_align */
			lv_align_t va = (pd->text_align == 0) ? LV_ALIGN_LEFT_MID
			              : (pd->text_align == 2) ? LV_ALIGN_RIGHT_MID
			              : LV_ALIGN_CENTER;
			lv_obj_align(row, va, 0, pd->value_y_offset);

			pd->value_row  = row;
			pd->unit_label = us;
		}
	}

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

	/* Peak/min display (under the value, in muted body font). Created
	 * always but hidden when show_peak == 0; toggling visibility is cheaper
	 * than tearing down and rebuilding the label when the user changes the
	 * setting from the config modal. */
	lv_obj_t *pk = lv_label_create(box);
	lv_label_set_text(pk, "");
	lv_obj_set_style_text_color(pk, THEME_COLOR_TEXT_MUTED,
	                             LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(pk, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *pk_font = widget_resolve_font(pd->peak_font);
	lv_obj_set_style_text_font(pk, pk_font ? pk_font : THEME_FONT_TINY,
	                            LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Follow the panel's text alignment (left/centre/right) like the header and
	 * value, so the peak hugs the same edge in MoTeC-style columns; peak_x/y
	 * offsets fine-tune on top of that. */
	lv_obj_set_style_text_align(pk, ta, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_width(pk, w->w - 10);
	lv_label_set_long_mode(pk, LV_LABEL_LONG_CLIP);
	lv_obj_set_x(pk, pd->peak_x_offset);
	lv_obj_set_y(pk, pd->peak_y_offset);
	lv_obj_set_align(pk, LV_ALIGN_CENTER);
	if (pd->show_peak == 0)
		lv_obj_add_flag(pk, LV_OBJ_FLAG_HIDDEN);

	/* Store per-instance pointers so signal callback uses the right objects */
	pd->box = box;
	pd->header_label = hdr;
	pd->value_label = val;
	pd->custom_text_label = ctxt;
	pd->peak_label = pk;

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

	/* Subscribe to channel-changed events when bound. */
	if (pd->channel) {
		channel_manager_subscribe((channel_t *)pd->channel,
		                           _panel_on_channel_changed, w);
	}

	/* Subscribe to night-mode changes if any night override is set, and apply
	 * current state immediately so the widget renders correctly even if it
	 * was created while night-mode is already active. */
	if (pd->night.has_border_color || pd->night.has_bg_color ||
	    pd->night.has_label_color  || pd->night.has_value_color) {
		night_mode_subscribe(_panel_night_cb, w);
		_panel_apply_night_mode(w, night_mode_is_active());
	}
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
	/* Decimals: persist only as a per-widget override — omit when it matches
	 * the bound channel's decimals (the default), so it follows the channel. */
	if (!pd->channel || pd->decimals != ((channel_t *)pd->channel)->decimals)
		cJSON_AddNumberToObject(cfg, "decimals", pd->decimals);
	if (pd->show_peak != 0)
		cJSON_AddNumberToObject(cfg, "show_peak", pd->show_peak);
	/* Peak placement/font — defaults-only. */
	if (pd->peak_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "peak_font", pd->peak_font);
	if (pd->peak_x_offset != 0)
		cJSON_AddNumberToObject(cfg, "peak_x_offset", pd->peak_x_offset);
	if (pd->peak_y_offset != 31)
		cJSON_AddNumberToObject(cfg, "peak_y_offset", pd->peak_y_offset);
	/* Alert COLOURS + apply-flags are widget-owned styling and DO round-trip
	 * in the layout. The threshold VALUES + enabled state live on the bound
	 * channel (channels.json) and are intentionally NOT persisted here — so
	 * warning levels stick to the channel setup and don't travel when a layout
	 * is shared. Emit defaults-only (colour untouched == 0). */
	if (pd->warning_high_color.full != THEME_COLOR_RED.full ||
	    !pd->warning_high_apply_label || !pd->warning_high_apply_value ||
	    pd->warning_high_apply_panel) {
		cJSON_AddNumberToObject(cfg, "warning_high_color", (int)pd->warning_high_color.full);
		cJSON_AddBoolToObject(cfg, "warning_high_apply_label", pd->warning_high_apply_label);
		cJSON_AddBoolToObject(cfg, "warning_high_apply_value", pd->warning_high_apply_value);
		cJSON_AddBoolToObject(cfg, "warning_high_apply_panel", pd->warning_high_apply_panel);
	}
	if (pd->warning_low_color.full != THEME_COLOR_BLUE_DARK.full ||
	    !pd->warning_low_apply_label || !pd->warning_low_apply_value ||
	    pd->warning_low_apply_panel) {
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
	/* v14 channel binding — only emit when explicitly bound */
	if (pd->channel_id[0] != '\0')
		cJSON_AddStringToObject(cfg, "channel", pd->channel_id);
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
	if (pd->text_align != 1)
		cJSON_AddNumberToObject(cfg, "text_align", pd->text_align);
	if (pd->show_unit)
		cJSON_AddBoolToObject(cfg, "show_unit", true);
	if (pd->unit_size != 2)
		cJSON_AddNumberToObject(cfg, "unit_size", pd->unit_size);
	if (pd->custom_text_x_offset != 41)
		cJSON_AddNumberToObject(cfg, "custom_text_x_offset", pd->custom_text_x_offset);
	if (pd->custom_text_y_offset != 32)
		cJSON_AddNumberToObject(cfg, "custom_text_y_offset", pd->custom_text_y_offset);
	/* Night-mode overrides — emit only fields that have an override set */
	{
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, pd->night, border_color);
		NIGHT_SERIALIZE_COLOR(n, pd->night, bg_color);
		NIGHT_SERIALIZE_COLOR(n, pd->night, label_color);
		NIGHT_SERIALIZE_COLOR(n, pd->night, value_color);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
	}
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
	bool decimals_overridden = cJSON_IsNumber(item);
	if (decimals_overridden) pd->decimals = (uint8_t)item->valueint;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_peak");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 0 || v > 3) v = 0;
		pd->show_peak = (uint8_t)v;
	}

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
	item = cJSON_GetObjectItemCaseSensitive(cfg, "text_align");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint; if (v < 0 || v > 2) v = 1;
		pd->text_align = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_unit");
	if (cJSON_IsBool(item)) pd->show_unit = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "unit_size");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint; if (v < 0 || v > 2) v = 2;
		pd->unit_size = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "custom_text_x_offset");
	if (cJSON_IsNumber(item)) pd->custom_text_x_offset = (int8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "custom_text_y_offset");
	if (cJSON_IsNumber(item)) pd->custom_text_y_offset = (int8_t)item->valueint;

	/* Peak placement/font. peak_y_offset back-compat: layouts saved before this
	 * field existed positioned the peak at value_y_offset + 22, so when the key
	 * is absent derive it from the (already-parsed) value offset to keep the
	 * exact same look; new layouts carry an explicit value. */
	item = cJSON_GetObjectItemCaseSensitive(cfg, "peak_font");
	if (cJSON_IsString(item))
		safe_strncpy(pd->peak_font, item->valuestring, sizeof(pd->peak_font));
	item = cJSON_GetObjectItemCaseSensitive(cfg, "peak_x_offset");
	if (cJSON_IsNumber(item)) pd->peak_x_offset = (int8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "peak_y_offset");
	if (cJSON_IsNumber(item)) pd->peak_y_offset = (int8_t)item->valueint;
	else                      pd->peak_y_offset = (int8_t)(pd->value_y_offset + 22);

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, pd->night, border_color);
		NIGHT_PARSE_COLOR(night, pd->night, bg_color);
		NIGHT_PARSE_COLOR(night, pd->night, label_color);
		NIGHT_PARSE_COLOR(night, pd->night, value_color);
	}

	/* Resolve signal name → index */
	if (pd->signal_name[0] != '\0')
		pd->signal_index = signal_find_by_name(pd->signal_name);

	/* ── v14 channel binding + backwards-compat migration ─────────
	 * See widget_meter.c for the pattern documentation.
	 * Empty-signal channel falls through to the legacy path so
	 * record_legacy_widget repopulates it from the widget's own signal. */
	cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(cfg, "channel");
	if (cJSON_IsString(ch_item) && ch_item->valuestring && ch_item->valuestring[0] != '\0')
		safe_strncpy(pd->channel_id, ch_item->valuestring, sizeof(pd->channel_id));
	channel_t *bound_c = pd->channel_id[0] ? channel_manager_get(pd->channel_id) : NULL;
	if (bound_c && bound_c->signal_index >= 0) {
		pd->channel = bound_c;
		safe_strncpy(pd->signal_name, bound_c->signal_name, sizeof(pd->signal_name));
		pd->signal_index = bound_c->signal_index;
		/* Decimals default to the channel; a per-widget override in the layout
		 * wins (config modal → Display → Decimals). */
		if (!decimals_overridden) pd->decimals = bound_c->decimals;
		/* Channel owns the threshold values. When it has a warn it's
		 * authoritative; when it doesn't but this widget carried a legacy
		 * per-widget threshold (layouts saved before thresholds moved to the
		 * channel), migrate that value UP into the channel so the alert is
		 * preserved and the channel becomes the single source of truth. With
		 * neither, the alert is inactive. */
		bool _ch_dirty = false;
		if (bound_c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
			pd->warning_high_enabled = true;
			pd->warning_high_threshold = (float)bound_c->high_warn;
		} else if (pd->warning_high_enabled) {
			bound_c->high_warn = pd->warning_high_threshold;
			_ch_dirty = true;
		} else {
			pd->warning_high_enabled = false;
		}
		if (bound_c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW) {
			pd->warning_low_enabled = true;
			pd->warning_low_threshold = (float)bound_c->low_warn;
		} else if (pd->warning_low_enabled) {
			bound_c->low_warn = pd->warning_low_threshold;
			_ch_dirty = true;
		} else {
			pd->warning_low_enabled = false;
		}
		if (_ch_dirty) channel_manager_mark_dirty();
		/* Alert colours stay widget-owned — never overridden by the channel. */
	} else if (pd->signal_name[0] != '\0') {
		/* v13 legacy path — self-report data so the channel registry
		 * auto-populates on first v14 boot. */
		legacy_widget_data_t legacy = {
			.signal_name = pd->signal_name,
			.min = INT32_MIN,   /* panel doesn't carry display range */
			.max = INT32_MIN,
			.high_warn = pd->warning_high_enabled ? pd->warning_high_threshold : (float)INT32_MIN,
			.color_normal = CHANNEL_USE_DEFAULT_COLOR,
			.color_high_warn = pd->warning_high_enabled ?
				(lv_color_to32(pd->warning_high_color) & 0xFFFFFF) : CHANNEL_USE_DEFAULT_COLOR,
		};
		channel_t *c = channel_manager_record_legacy_widget(&legacy);
		if (c) {
			pd->channel = c;
			if (!decimals_overridden) pd->decimals = c->decimals;
			/* Also record the low_warn separately — record_legacy_widget
			 * only takes high-side thresholds in v1. */
			if (pd->warning_low_enabled && c->low_warn == CHANNEL_THRESHOLD_UNSET_LOW) {
				c->low_warn = pd->warning_low_threshold;
				if (pd->warning_low_enabled)
					c->color_low_warn = lv_color_to32(pd->warning_low_color) & 0xFFFFFF;
				channel_manager_mark_dirty();
			}
			/* Adopt the channel's warns into the widget so the alert reflects
			 * the channel (single source of truth) — the signal-bound legacy
			 * path must sync channel→widget just like the explicit-channel
			 * path, otherwise a panel bound only by signal name never picks up
			 * its channel's thresholds (the "panel alerts not working" bug). */
			if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
				pd->warning_high_enabled = true;
				pd->warning_high_threshold = (float)c->high_warn;
			}
			if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW) {
				pd->warning_low_enabled = true;
				pd->warning_low_threshold = (float)c->low_warn;
			}
		}
	}
}
static void _panel_destroy(widget_t *w) {
	if (w) {
		panel_data_t *pd = (panel_data_t *)w->type_data;
		if (pd && pd->signal_index >= 0)
			signal_unsubscribe(pd->signal_index, _panel_on_signal, w);
		if (pd && pd->channel) {
			channel_manager_unsubscribe((channel_t *)pd->channel,
			                             _panel_on_channel_changed, w);
			pd->channel = NULL;
		}
		night_mode_unsubscribe(_panel_night_cb, w);
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

/* Re-apply colors based on current night-mode state. Called by night_mode
 * subscribers — this is just a thin wrapper that picks day-or-night value
 * for each overridable field and writes to the LVGL objects. */
static void _panel_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	panel_data_t *pd = (panel_data_t *)w->type_data;
	if (!pd) return;

	lv_color_t bg     = NIGHT_PICK_COLOR(active, pd->night, bg_color,     pd->bg_color);
	lv_color_t bdr    = NIGHT_PICK_COLOR(active, pd->night, border_color, pd->border_color);
	lv_color_t lblc   = NIGHT_PICK_COLOR(active, pd->night, label_color,  pd->label_color);
	lv_color_t valc   = NIGHT_PICK_COLOR(active, pd->night, value_color,  pd->value_color);

	if (pd->box && lv_obj_is_valid(pd->box)) {
		lv_obj_set_style_bg_color(pd->box, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(pd->box, bdr, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (pd->header_label && lv_obj_is_valid(pd->header_label)) {
		lv_obj_set_style_text_color(pd->header_label, lblc, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (pd->value_label && lv_obj_is_valid(pd->value_label)) {
		lv_obj_set_style_text_color(pd->value_label, valc, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _panel_night_cb(bool active, void *user_data) {
	_panel_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector hooks (Phase 3.5) ────────────────────────────────────────
 *
 * Read / write Panel fields by schema name. Used by the on-device
 * Inspector to render the STYLE / DATA tabs from schema/widgets.schema.json
 * without hand-coded UI for each field. Live-preview is built in: the set
 * hook writes BOTH the type_data field AND the matching LVGL property on
 * the panel's existing LVGL objects, so the user sees the edit land
 * immediately.
 *
 * Schema names match widgets.schema.json. Fields not handled here fall
 * through to false, and the Inspector skips them (they'll need a follow-up
 * commit to wire). Alert fields (warning_*) are deferred until the RULES
 * tab lands. */

#include <string.h>

static bool _panel_inspector_get(const widget_t *w, const char *name,
								 widget_field_value_t *out) {
	if (!w || w->type != WIDGET_PANEL || !w->type_data || !name || !out) return false;
	const panel_data_t *pd = (const panel_data_t *)w->type_data;

	if (strcmp(name, "label") == 0)                { out->str = pd->label;        return true; }
	if (strcmp(name, "custom_text") == 0)          { out->str = pd->custom_text;  return true; }
	if (strcmp(name, "decimals") == 0)             { out->i = pd->decimals;       return true; }
	if (strcmp(name, "show_peak") == 0)            { out->i = pd->show_peak;      return true; }
	if (strcmp(name, "peak_font") == 0)            { out->str = pd->peak_font;    return true; }
	if (strcmp(name, "peak_x_offset") == 0)        { out->i = pd->peak_x_offset;  return true; }
	if (strcmp(name, "peak_y_offset") == 0)        { out->i = pd->peak_y_offset;  return true; }
	if (strcmp(name, "signal_name") == 0)          { out->str = pd->signal_name;  return true; }
	if (strcmp(name, "label_font") == 0)           { out->str = pd->label_font;   return true; }
	if (strcmp(name, "value_font") == 0)           { out->str = pd->value_font;   return true; }
	if (strcmp(name, "border_radius") == 0)        { out->i = pd->border_radius;  return true; }
	if (strcmp(name, "border_width") == 0)         { out->i = pd->border_width;   return true; }
	if (strcmp(name, "border_color") == 0)         { out->color = lv_color_to32(pd->border_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "bg_color") == 0)             { out->color = lv_color_to32(pd->bg_color)     & 0xFFFFFF; return true; }
	if (strcmp(name, "bg_opa") == 0)               { out->i = pd->bg_opa;         return true; }
	if (strcmp(name, "label_color") == 0)          { out->color = lv_color_to32(pd->label_color)  & 0xFFFFFF; return true; }
	if (strcmp(name, "value_color") == 0)          { out->color = lv_color_to32(pd->value_color)  & 0xFFFFFF; return true; }
	if (strcmp(name, "label_y_offset") == 0)       { out->i = pd->label_y_offset; return true; }
	if (strcmp(name, "value_y_offset") == 0)       { out->i = pd->value_y_offset; return true; }
	if (strcmp(name, "text_align") == 0)           { out->i = pd->text_align;     return true; }
	if (strcmp(name, "show_unit") == 0)            { out->b = pd->show_unit;      return true; }
	if (strcmp(name, "unit_size") == 0)            { out->i = pd->unit_size;      return true; }
	if (strcmp(name, "custom_text_x_offset") == 0) { out->i = pd->custom_text_x_offset; return true; }
	if (strcmp(name, "custom_text_y_offset") == 0) { out->i = pd->custom_text_y_offset; return true; }
	return false;
}

static bool _panel_inspector_set(widget_t *w, const char *name,
								 const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_PANEL || !w->type_data || !name || !in) return false;
	panel_data_t *pd = (panel_data_t *)w->type_data;

	lv_obj_t *lbl = (w->slot < 13) ? ui_Label[w->slot] : NULL;
	lv_obj_t *val = (w->slot < 13) ? ui_Value[w->slot] : NULL;
	lv_obj_t *unit = pd->custom_text_label;

	if (strcmp(name, "decimals") == 0) {
		pd->decimals = (uint8_t)in->i;
		pd->last_peak[0] = '\0';  /* re-render peak with new precision */
		return true;
	}
	if (strcmp(name, "show_peak") == 0) {
		pd->show_peak = (uint8_t)in->i;
		pd->last_peak[0] = '\0';  /* re-render peak in the new mode */
		if (pd->peak_label && lv_obj_is_valid(pd->peak_label)) {
			if (pd->show_peak == 0)
				lv_obj_add_flag(pd->peak_label, LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_clear_flag(pd->peak_label, LV_OBJ_FLAG_HIDDEN);
		}
		return true;
	}
	if (strcmp(name, "peak_font") == 0) {
		safe_strncpy(pd->peak_font, in->str ? in->str : "", sizeof(pd->peak_font));
		if (pd->peak_label && lv_obj_is_valid(pd->peak_label)) {
			const lv_font_t *f = widget_resolve_font(pd->peak_font);
			lv_obj_set_style_text_font(pd->peak_label, f ? f : THEME_FONT_TINY, 0);
		}
		return true;
	}
	if (strcmp(name, "peak_x_offset") == 0) {
		pd->peak_x_offset = (int8_t)in->i;
		if (pd->peak_label && lv_obj_is_valid(pd->peak_label))
			lv_obj_set_x(pd->peak_label, pd->peak_x_offset);
		return true;
	}
	if (strcmp(name, "peak_y_offset") == 0) {
		pd->peak_y_offset = (int8_t)in->i;
		if (pd->peak_label && lv_obj_is_valid(pd->peak_label))
			lv_obj_set_y(pd->peak_label, pd->peak_y_offset);
		return true;
	}
	if (strcmp(name, "border_radius") == 0) {
		pd->border_radius = (uint8_t)in->i;
		if (w->root && lv_obj_is_valid(w->root))
			lv_obj_set_style_radius(w->root, pd->border_radius, 0);
		return true;
	}
	if (strcmp(name, "border_width") == 0) {
		pd->border_width = (uint8_t)in->i;
		if (w->root && lv_obj_is_valid(w->root))
			lv_obj_set_style_border_width(w->root, pd->border_width, 0);
		return true;
	}
	if (strcmp(name, "border_color") == 0) {
		pd->border_color = lv_color_hex(in->color);
		if (w->root && lv_obj_is_valid(w->root))
			lv_obj_set_style_border_color(w->root, pd->border_color, 0);
		return true;
	}
	if (strcmp(name, "bg_color") == 0) {
		pd->bg_color = lv_color_hex(in->color);
		if (w->root && lv_obj_is_valid(w->root))
			lv_obj_set_style_bg_color(w->root, pd->bg_color, 0);
		return true;
	}
	if (strcmp(name, "bg_opa") == 0) {
		pd->bg_opa = (uint8_t)in->i;
		if (w->root && lv_obj_is_valid(w->root))
			lv_obj_set_style_bg_opa(w->root, pd->bg_opa, 0);
		return true;
	}
	if (strcmp(name, "label_color") == 0) {
		pd->label_color = lv_color_hex(in->color);
		if (lbl && lv_obj_is_valid(lbl))
			lv_obj_set_style_text_color(lbl, pd->label_color, 0);
		return true;
	}
	if (strcmp(name, "value_color") == 0) {
		pd->value_color = lv_color_hex(in->color);
		if (val && lv_obj_is_valid(val))
			lv_obj_set_style_text_color(val, pd->value_color, 0);
		if (pd->unit_label && lv_obj_is_valid(pd->unit_label))
			lv_obj_set_style_text_color(pd->unit_label, pd->value_color, 0);
		return true;
	}
	if (strcmp(name, "label_y_offset") == 0) {
		pd->label_y_offset = (int8_t)in->i;
		if (lbl && lv_obj_is_valid(lbl))
			lv_obj_set_y(lbl, pd->label_y_offset);
		return true;
	}
	if (strcmp(name, "value_y_offset") == 0) {
		pd->value_y_offset = (int8_t)in->i;
		/* In small/medium-unit mode the value lives inside the flex row, so
		 * move the row (the positioned object); otherwise move the value. */
		lv_obj_t *vpos = (pd->value_row && lv_obj_is_valid(pd->value_row))
		                 ? pd->value_row : val;
		if (vpos && lv_obj_is_valid(vpos))
			lv_obj_set_y(vpos, pd->value_y_offset);
		/* Peak has its own peak_y_offset now — it no longer rides on the value. */
		return true;
	}
	if (strcmp(name, "text_align") == 0) {
		int v = in->i; if (v < 0 || v > 2) v = 1;
		pd->text_align = (uint8_t)v;
		lv_text_align_t ta = (v == 0) ? LV_TEXT_ALIGN_LEFT
		                   : (v == 2) ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
		if (pd->header_label && lv_obj_is_valid(pd->header_label))
			lv_obj_set_style_text_align(pd->header_label, ta, 0);
		if (pd->value_label && lv_obj_is_valid(pd->value_label))
			lv_obj_set_style_text_align(pd->value_label, ta, 0);
		if (pd->peak_label && lv_obj_is_valid(pd->peak_label))
			lv_obj_set_style_text_align(pd->peak_label, ta, 0);
		/* small/medium unit: the value is content-width inside the flex row, so
		 * text_align has no effect there — re-align the row within the box. */
		if (pd->value_row && lv_obj_is_valid(pd->value_row)) {
			lv_align_t va = (v == 0) ? LV_ALIGN_LEFT_MID
			              : (v == 2) ? LV_ALIGN_RIGHT_MID : LV_ALIGN_CENTER;
			lv_obj_align(pd->value_row, va, 0, pd->value_y_offset);
		}
		return true;
	}
	if (strcmp(name, "show_unit") == 0) {
		pd->show_unit = in->b;
		pd->last_display[0] = '\0';   /* force value re-render on next tick */
		return true;
	}
	if (strcmp(name, "unit_size") == 0) {
		int v = in->i; if (v < 0 || v > 2) v = 2;
		pd->unit_size = (uint8_t)v;
		pd->last_display[0] = '\0';   /* full<->small/medium changes layout; web rebuilds */
		return true;
	}
	if (strcmp(name, "custom_text_x_offset") == 0) {
		pd->custom_text_x_offset = (int8_t)in->i;
		if (unit && lv_obj_is_valid(unit))
			lv_obj_set_x(unit, pd->custom_text_x_offset);
		return true;
	}
	if (strcmp(name, "custom_text_y_offset") == 0) {
		pd->custom_text_y_offset = (int8_t)in->i;
		if (unit && lv_obj_is_valid(unit))
			lv_obj_set_y(unit, pd->custom_text_y_offset);
		return true;
	}
	if (strcmp(name, "label") == 0 && in->str) {
		strncpy(pd->label, in->str, sizeof(pd->label) - 1);
		pd->label[sizeof(pd->label) - 1] = '\0';
		if (lbl && lv_obj_is_valid(lbl)) lv_label_set_text(lbl, pd->label);
		return true;
	}
	if (strcmp(name, "custom_text") == 0 && in->str) {
		strncpy(pd->custom_text, in->str, sizeof(pd->custom_text) - 1);
		pd->custom_text[sizeof(pd->custom_text) - 1] = '\0';
		if (unit && lv_obj_is_valid(unit)) lv_label_set_text(unit, pd->custom_text);
		return true;
	}
	if (strcmp(name, "signal_name") == 0 && in->str) {
		/* Signal swap: unsubscribe from old, look up new, subscribe to new.
		 * Auto-fills custom_text from the new signal's intrinsic unit if
		 * the widget doesn't have one set, so a fresh widget picks up "°C"
		 * without the user typing it. */
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;   /* unknown name */

		if (pd->signal_index >= 0)
			signal_unsubscribe(pd->signal_index, _panel_on_signal, w);

		strncpy(pd->signal_name, in->str, sizeof(pd->signal_name) - 1);
		pd->signal_name[sizeof(pd->signal_name) - 1] = '\0';
		pd->signal_index = new_idx;

		if (new_idx >= 0)
			signal_subscribe(new_idx, _panel_on_signal, w);

		if (pd->custom_text[0] == '\0' && new_idx >= 0) {
			signal_t *s = signal_get_by_index((uint16_t)new_idx);
			if (s && s->unit[0] != '\0') {
				strncpy(pd->custom_text, s->unit, sizeof(pd->custom_text) - 1);
				pd->custom_text[sizeof(pd->custom_text) - 1] = '\0';
				if (unit && lv_obj_is_valid(unit))
					lv_label_set_text(unit, pd->custom_text);
			}
		}
		return true;
	}
	/* label_font / value_font and warning_* deferred. */
	return false;
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

	pd->slot = slot;
	pd->signal_index = -1;
	/* Warning "apply to" defaults: label + value coloured, panel border not */
	pd->warning_high_apply_label = true;
	pd->warning_high_apply_value = true;
	pd->warning_high_apply_panel = false;
	pd->warning_low_apply_label = true;
	pd->warning_low_apply_value = true;
	pd->warning_low_apply_panel = false;
	/* Sensible default alert colours so a channel-driven warning is never
	 * painted in the calloc'd black. User overrides via Widget settings →
	 * ALERTS. Thresholds come from the channel; colours are widget-owned. */
	pd->warning_high_color = THEME_COLOR_RED;
	pd->warning_low_color = THEME_COLOR_BLUE_DARK;
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
	pd->text_align = 1;   /* center (back-compat default) */
	pd->show_unit = false;
	pd->unit_size = 2;    /* full (inline) */
	pd->custom_text_x_offset = 41;
	pd->custom_text_y_offset = 32;
	pd->peak_font[0] = '\0';   /* tiny theme font */
	pd->peak_x_offset = 0;
	pd->peak_y_offset = 31;    /* legacy: value_y_offset(9) + 22 */

	w->type = WIDGET_PANEL;
	w->slot = pd->slot;
	w->x = (pd->slot < 8) ? s_panel_default_x[pd->slot] : 0;
	w->y = (pd->slot < 8) ? s_panel_default_y[pd->slot] : 0;
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
	w->apply_night_mode = _panel_apply_night_mode;
	w->inspector_get = _panel_inspector_get;
	w->inspector_set = _panel_inspector_set;

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
