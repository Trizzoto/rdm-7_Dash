#include "widget_bar.h"
#include "widget_image.h"
#include "widget_rules.h"
#include "data/channel_manager.h"
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

static inline bool _bar_has_track_image(const bar_data_t *bd) {
	return bd && bd->bar_image[0] != '\0';
}
static inline bool _bar_has_fill_image(const bar_data_t *bd) {
	return bd && bd->bar_image_full[0] != '\0';
}

/* Max tick objects we can hold (must match bar_data_t::tick_objs[60]). With
 * tick_side == Both we draw two per logical tick, so the per-side count is
 * clamped to 30. */
#define BAR_MAX_TICK_OBJS  60

/* Delete every live tick sibling and reset the bookkeeping. Tick objects are
 * children of the bar's parent (siblings of root / label / value), so they are
 * deleted explicitly here exactly like the label/value siblings in
 * _bar_destroy — lv_obj_clean(screen) on a rebuild already frees them, but any
 * in-place rebuild (resize / inspector edit) must tear down the old set first
 * to avoid orphaned objects. */
static void _bar_free_ticks(bar_data_t *bd) {
	if (!bd) return;
	for (uint8_t i = 0; i < bd->tick_obj_count; i++) {
		if (bd->tick_objs[i] && lv_obj_is_valid(bd->tick_objs[i]))
			lv_obj_del(bd->tick_objs[i]);
		bd->tick_objs[i] = NULL;
	}
	bd->tick_obj_count = 0;
}

/* Create one tick rectangle at center-origin (x,y), pushed to the foreground so
 * the bar fill never paints over it. Mirrors update_rpm_lines() tick styling:
 * radius 0, solid bg, no border, no padding, non-interactive. Returns the new
 * object (or NULL on alloc / cap failure). */
static lv_obj_t *_bar_make_tick(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t tw, lv_coord_t th, lv_color_t col) {
	lv_obj_t *t = lv_obj_create(parent);
	if (!t) return NULL;
	lv_obj_set_size(t, tw, th);
	lv_obj_set_align(t, LV_ALIGN_CENTER);
	lv_obj_set_pos(t, x, y);
	lv_obj_set_style_radius(t, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(t, col, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(t, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_move_foreground(t);
	return t;
}

/* (Re)build the tick marks for a bar widget. Tears down any existing ticks,
 * then — if show_ticks — lays tick_count rectangles evenly across the bar
 * width (w->w) in center-origin coordinates. tick_side controls vertical
 * placement above (Top), below (Bottom), or both edges (Both) of the bar
 * body. Called only on create / resize / inspector edit (NOT per signal tick),
 * respecting the paint-memo pattern. */
static void _bar_build_ticks(widget_t *w) {
	if (!w) return;
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (!bd) return;

	_bar_free_ticks(bd);

	if (!bd->show_ticks || bd->tick_count == 0) return;

	/* Parent of the bar = parent the siblings live on. Fall back to the bar
	 * object's parent (image-mode bars may not have bd->bar_obj). */
	lv_obj_t *anchor = bd->bar_obj ? bd->bar_obj
	                 : (bd->img_bg_obj ? bd->img_bg_obj : w->root);
	if (!anchor || !lv_obj_is_valid(anchor)) return;
	lv_obj_t *parent = lv_obj_get_parent(anchor);
	if (!parent || !lv_obj_is_valid(parent)) return;

	lv_coord_t tw = (lv_coord_t)(bd->tick_width  ? bd->tick_width  : 1);
	lv_coord_t th = (lv_coord_t)(bd->tick_length ? bd->tick_length : 1);
	uint8_t count = bd->tick_count;

	/* Per-side cap so "Both" (2 objects per tick) can't overflow tick_objs[60]. */
	bool both = (bd->tick_side == 2);
	uint8_t per_side_cap = both ? (BAR_MAX_TICK_OBJS / 2) : BAR_MAX_TICK_OBJS;
	if (count > per_side_cap) count = per_side_cap;

	/* Even spacing across the full bar width: tick i sits at
	 * left_edge + i/(count-1) * width. A single tick is centered. */
	lv_coord_t left = w->x - (w->w / 2);
	lv_coord_t span = w->w;
	/* Vertical placement: half the bar height plus half the tick height puts
	 * the tick flush against the top / bottom edge of the bar body. */
	lv_coord_t y_top = w->y - (w->h / 2) - (th / 2);
	lv_coord_t y_bot = w->y + (w->h / 2) + (th / 2);

	for (uint8_t i = 0; i < count; i++) {
		lv_coord_t x;
		if (count == 1)
			x = w->x;
		else
			x = left + (lv_coord_t)(((int32_t)i * span) / (count - 1));

		if (bd->tick_side == 0 || bd->tick_side == 2) {   /* Top / Both */
			if (bd->tick_obj_count < BAR_MAX_TICK_OBJS) {
				lv_obj_t *t = _bar_make_tick(parent, x, y_top, tw, th, bd->tick_color);
				if (t) bd->tick_objs[bd->tick_obj_count++] = t;
			}
		}
		if (bd->tick_side == 1 || bd->tick_side == 2) {   /* Bottom / Both */
			if (bd->tick_obj_count < BAR_MAX_TICK_OBJS) {
				lv_obj_t *t = _bar_make_tick(parent, x, y_bot, tw, th, bd->tick_color);
				if (t) bd->tick_objs[bd->tick_obj_count++] = t;
			}
		}
	}
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

/* Apply the user-configured anchor curve. Returns the "virtual" data value
 * that, when fed to the linear bar fill calculation, lands at the desired
 * non-linear position. Two linear segments split at (anchor_value,
 * anchor_position%): values in [min..anchor] map to [0..anchor_pos%] of
 * the bar, values in [anchor..max] map to [anchor_pos%..100%]. Anchor at
 * midpoint with position=50 collapses to linear pass-through. */
static float _bar_apply_anchor(const bar_data_t *bd, float v) {
	if (!bd || !bd->anchor_enabled) return v;
	float mn = (float)bd->bar_min;
	float mx = (float)bd->bar_max;
	if (mx <= mn) return v;
	float a  = (float)bd->anchor_value;
	float ap = (float)bd->anchor_position;   /* 0..100 */
	if (ap < 0.0f)   ap = 0.0f;
	if (ap > 100.0f) ap = 100.0f;
	float pct;
	if (v <= a) {
		pct = (a > mn) ? ((v - mn) / (a - mn) * ap) : 0.0f;
	} else {
		float hp = 100.0f - ap;
		pct = (mx > a) ? (ap + (v - a) / (mx - a) * hp) : 100.0f;
	}
	if (pct < 0.0f)   pct = 0.0f;
	if (pct > 100.0f) pct = 100.0f;
	return mn + (pct / 100.0f) * (mx - mn);
}

/* Forward decl — used by widget_bar_sync_range to re-push the current value. */
static void _bar_on_signal(float value, bool is_stale, void *user_data);

void widget_bar_sync_range(bar_data_t *bd) {
	if (!bd) return;
	if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj)) {
		int32_t scale = _bar_resolution_scale(bd);
		int32_t lo = lroundf(bd->bar_min * (float)scale);
		int32_t hi = lroundf(bd->bar_max * (float)scale);
		if (hi <= lo) { lo = 0; hi = 100 * scale; }
		lv_bar_set_range(bd->bar_obj, lo, hi);
	}
	/* Range / decimals just changed → the cached scaled value (standard mode)
	 * or clip width (image mode) no longer maps to the right fill. Drop the
	 * paint memo and re-push the current value NOW: a parked/steady signal
	 * produces no further tick to self-heal (signal.c only notifies on a value
	 * change), so without this the fill would freeze at the old scale. */
	bd->_pc_valid = false;
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_BAR, bd->slot);
	if (w && bd->signal_index >= 0) {
		signal_t *sig = signal_get_by_index((uint16_t)bd->signal_index);
		if (sig) _bar_on_signal(sig->current_value, sig->is_stale, w);
	}
}

/* ── Helpers: look up bar_data_t by slot or value_id via registry ──────── */
static bar_data_t *_lookup_bar_data(uint8_t slot) {
	widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_BAR, slot);
	return w ? (bar_data_t *)w->type_data : NULL;
}


uint64_t last_bar_can_received[2] = {0, 0};

/* Global LVGL object definitions (declared extern in widget_bar.h) */
lv_obj_t *ui_Bar_1_Label = NULL;
lv_obj_t *ui_Bar_2_Label = NULL;
lv_obj_t *ui_Bar_1_Value = NULL;
lv_obj_t *ui_Bar_2_Value = NULL;

void bar_range_input_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		const char *txt = lv_textarea_get_text(textarea);
		uint8_t slot = *(uint8_t *)lv_event_get_user_data(e);
		bar_data_t *bd = _lookup_bar_data(slot);
		if (!bd) return;

		bool is_min = lv_obj_get_user_data(textarea) != NULL;
		float value = (float)atof(txt);

		if (is_min) bd->bar_min = value;
		else        bd->bar_max = value;
		/* Re-apply the scaled range (10^decimals) so fractional bounds keep
		 * their LVGL resolution instead of snapping to whole units. */
		widget_bar_sync_range(bd);
	}
}


// Forward declaration for color wheel popup
void create_rpm_color_wheel_popup(void);
void create_limiter_color_wheel_popup(void);



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
	int16_t sig_idx = bd ? bd->signal_index : -1;
	lv_obj_t *val_label = (upd->bar_index == 0) ? ui_Bar_1_Value : ui_Bar_2_Value;
	if (val_label && lv_obj_is_valid(val_label) && show_val) {
		char value_str[16];
		if (upd->is_timeout) {
			strcpy(value_str, "---");
		} else {
			signal_format_value(sig_idx, (float)upd->final_value,
								(uint8_t)decimals, value_str, sizeof(value_str));
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
	int16_t sig_idx = bd ? bd->signal_index : -1;
	lv_obj_t *val_label = (bar_index == 0) ? ui_Bar_1_Value : ui_Bar_2_Value;
	if (val_label && lv_obj_is_valid(val_label) && show_val) {
		char value_str[16];
		signal_format_value(sig_idx, (float)final_value,
							(uint8_t)decimals, value_str, sizeof(value_str));
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
	int32_t b1_min = lroundf((bd1 ? bd1->bar_min : 0.0f)   * (float)s1);
	int32_t b1_max = lroundf((bd1 ? bd1->bar_max : 100.0f) * (float)s1);
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
	int32_t b2_min = lroundf((bd2 ? bd2->bar_min : 0.0f)   * (float)s2);
	int32_t b2_max = lroundf((bd2 ? bd2->bar_max : 100.0f) * (float)s2);
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

/* Forward decl: defined below, but needed by _bar_on_channel_changed which
 * re-subscribes it when the channel's signal source is rebound at runtime. */
static void _bar_on_signal(float value, bool is_stale, void *user_data);

/* Channel-changed listener: re-snapshot channel fields and invalidate.
 * The next _bar_on_signal call will pick up the new bar_min/max/low/high
 * via the widget's live fields. */
static void _bar_on_channel_changed(channel_t *c, void *user_data) {
	if (!c || !user_data) return;
	widget_t *w = (widget_t *)user_data;
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (!bd) return;

	safe_strncpy(bd->signal_name, c->signal_name, sizeof(bd->signal_name));
	/* Re-point our own signal subscription when the channel's source index
	 * actually moved. _bar_on_signal is keyed in the registry by the index it
	 * attached to at create; without this the gauge stays bound to the OLD
	 * signal (or nothing) and freezes on its last value after a runtime rebind.
	 * Mirrors the inspector rebind path (_bar_inspector_set / "signal_name"). */
	int16_t new_idx = c->signal_index;
	if (new_idx != bd->signal_index) {
		if (bd->signal_index >= 0)
			signal_unsubscribe(bd->signal_index, _bar_on_signal, w);
		bd->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _bar_on_signal, w);
	}
	bd->bar_min = c->min;
	bd->bar_max = c->max;
	/* Channel owns the alert thresholds. When a side has no warn set, park it
	 * at the range edge so that alert can never fire (reverts to inactive)
	 * instead of leaving a stale 0 that paints the bar permanently in-alert. */
	bd->bar_low  = (c->low_warn  != CHANNEL_THRESHOLD_UNSET_LOW)  ? c->low_warn  : bd->bar_min;
	bd->bar_high = (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) ? c->high_warn : bd->bar_max;
	/* Bar zone colours are widget-owned (Widget settings → ALERTS) — never
	 * driven by the channel. Only range/thresholds/signal sync above. */

	/* Bounds changed → the cached scaled display value / color buckets are
	 * stale; force the next tick to re-apply everything. */
	bd->_pc_valid = false;
	if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj))
		lv_obj_invalidate(bd->bar_obj);
}

static void _bar_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (!bd) return;

	double final_value = is_stale ? 0.0 : (double)value;
	/* Apply anchor curve once for the FILL POSITION calculation. Threshold
	 * checks below still use the real (unwarped) final_value so warning
	 * colors trigger at the configured data thresholds. */
	float fill_value = is_stale ? 0.0f : _bar_apply_anchor(bd, value);
	/* Scale by 10^decimals so the standard-mode LVGL bar has fractional
	 * resolution within bar_min..bar_max. Image-mode recomputes fill %
	 * directly from the unscaled float value below, so it ignores this. */
	int32_t scale = _bar_resolution_scale(bd);
	int32_t bar_value = is_stale ? 0 : (int32_t)(fill_value * (float)scale);

	/* ── Fill-image bar mode (clip container controls fill width) ── */
	if (bd->img_clip_obj && lv_obj_is_valid(bd->img_clip_obj)) {
		float range = bd->bar_max - bd->bar_min;
		int32_t pct = 0;
		if (range > 0.0f && !is_stale) {
			double clamped = (double)fill_value;
			if (clamped < bd->bar_min) clamped = bd->bar_min;
			if (clamped > bd->bar_max) clamped = bd->bar_max;
			if (bd->invert_bar_value)
				pct = (int32_t)(((bd->bar_max - clamped) * 100.0) / range);
			else
				pct = (int32_t)(((clamped - bd->bar_min) * 100.0) / range);
		}
		lv_coord_t clip_w = (lv_coord_t)((pct * w->w) / 100);
		if (!bd->_pc_valid || bd->_pc_clip_w != clip_w) {
			lv_obj_set_width(bd->img_clip_obj, clip_w);
			bd->_pc_clip_w = clip_w;
		}
	} else if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj)) {
		/* ── Standard LVGL bar mode ── */
		/* Invert is implemented by reflecting the value about the midpoint:
		 * (min + max - value) keeps the bar filling left→right but maps a
		 * "full" reading to an empty fill and vice versa. We do this on the
		 * already-scaled bar_value because the LVGL bar's range is also
		 * scaled (see _bar_resolution_scale). Stale → 0 anyway, no flip. */
		int32_t display_value = bar_value;
		if (bd->invert_bar_value && !is_stale) {
			int32_t scaled_min = lroundf(bd->bar_min * (float)scale);
			int32_t scaled_max = lroundf(bd->bar_max * (float)scale);
			display_value = scaled_min + scaled_max - bar_value;
		}
		/* Value-gate: lv_bar_set_value (LV_ANIM_OFF) invalidates the WHOLE
		 * bar bbox every call — the dominant per-frame cost. Skip it when the
		 * scaled display value is unchanged (common for slow / low-decimal
		 * signals), which removes the whole-bar dirty rect entirely. */
		if (!bd->_pc_valid || bd->_pc_display_value != display_value) {
			lv_bar_set_value(bd->bar_obj, display_value, LV_ANIM_OFF);
			bd->_pc_display_value = display_value;
		}
		bool night_active = night_mode_is_active();
		lv_color_t low_col  = NIGHT_PICK_COLOR(night_active, bd->night, bar_low_color,      bd->bar_low_color);
		lv_color_t high_col = NIGHT_PICK_COLOR(night_active, bd->night, bar_high_color,     bd->bar_high_color);
		lv_color_t in_col   = NIGHT_PICK_COLOR(night_active, bd->night, bar_in_range_color, bd->bar_in_range_color);
		lv_color_t new_color;
		bool in_range = false;
		if (final_value < bd->bar_low)
			new_color = low_col;
		else if (final_value > bd->bar_high)
			new_color = high_col;
		else { new_color = in_col; in_range = true; }
		/* Color + gradient memo: new_color is night-resolved, so a night
		 * toggle that changes the resolved color re-fires here on the next
		 * tick. grad identity = (active flag, first-stop color). */
		bool grad_now = in_range && bd->grad_stops.count >= 2;
		uint16_t grad_first_now =
			(bd->grad_stops.count >= 1) ? bd->grad_stops.stops[0].color : 0;
		if (!bd->_pc_valid || bd->_pc_bg.full != new_color.full ||
		    bd->_pc_grad_active != grad_now || bd->_pc_grad_first != grad_first_now) {
		lv_obj_set_style_bg_color(bd->bar_obj, new_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		/* Multi-stop horizontal gradient on the indicator, in-range only.
		 * Uses LVGL's native lv_grad_dsc_t (cap raised to 8 stops in
		 * sdkconfig) — LVGL renders the gradient INTO the indicator
		 * rectangle directly, so as the bar fills from 0 → 100 % the
		 * gradient grows with it without the centering/tiling artifacts
		 * that bg_img has. Low/high alert states still paint solid over
		 * the top via the bg_color set just above.
		 *
		 * lv_obj_set_style_bg_grad stores the dsc POINTER, not a copy —
		 * the descriptor must outlive the style. We keep it in
		 * bar_data_t.grad_lv_dsc so it survives every redraw. */
		if (in_range && bd->grad_stops.count >= 2 &&
		    gradient_stops_to_lv_grad_dsc(&bd->grad_stops, &bd->grad_lv_dsc,
		                                  LV_GRAD_DIR_HOR)) {
			lv_obj_set_style_bg_grad(bd->bar_obj, &bd->grad_lv_dsc,
			                         LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else {
			/* MUST clear dsc.dir as well — LVGL's draw_dsc init checks
			 * grad->dir on the descriptor before falling back to the
			 * bg_grad_dir property, so leaving HOR here would keep the
			 * gradient painting over the alert colour. */
			bd->grad_lv_dsc.dir = LV_GRAD_DIR_NONE;
			lv_obj_set_style_bg_grad_dir(bd->bar_obj, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
			bd->_pc_bg = new_color;
			bd->_pc_grad_active = grad_now;
			bd->_pc_grad_first = grad_first_now;
		}
	}

	/* Update this widget's own value label */
	if (bd->value_obj && lv_obj_is_valid(bd->value_obj) && bd->show_bar_value) {
		char value_str[16];
		if (is_stale) {
			strcpy(value_str, "---");
		} else {
			/* signal_format_value honours any value-label map on the
			 * bound signal (gear positions, modes, etc.); falls back to
			 * the existing decimals-aware numeric format otherwise. */
			signal_format_value(bd->signal_index, (float)final_value,
								bd->decimals, value_str, sizeof(value_str));
		}
		/* Skip the realloc + label invalidate when the string is unchanged. */
		if (!bd->_pc_valid || strcmp(bd->_pc_value_str, value_str) != 0) {
			lv_label_set_text(bd->value_obj, value_str);
			safe_strncpy(bd->_pc_value_str, value_str, sizeof(bd->_pc_value_str));
		}
	}

	/* All gated writes for this tick are done — trust the memo next frame. */
	bd->_pc_valid = true;

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
	int32_t b_min = lroundf((bd ? bd->bar_min : 0.0f)   * (float)scale);
	int32_t b_max = lroundf((bd ? bd->bar_max : 100.0f) * (float)scale);
	if (b_max <= b_min) { b_min = 0; b_max = 100 * scale; }

	bool has_track = _bar_has_track_image(bd);
	bool has_fill  = _bar_has_fill_image(bd);

	/* ── Step 1: track background ───────────────────────────────────── */
	if (has_track) {
		bd->bar_img_dsc = rdm_image_load(bd->bar_image);
		safe_strncpy(bd->current_bar_image, bd->bar_image, sizeof(bd->current_bar_image));
		if (bd->bar_img_dsc) {
			lv_obj_t *bg = lv_img_create(parent);
			lv_img_set_src(bg, bd->bar_img_dsc);
			lv_obj_set_size(bg, w->w, w->h);
			lv_obj_set_align(bg, LV_ALIGN_CENTER);
			lv_obj_set_pos(bg, w->x, w->y);
			if (bd->bar_img_dsc->header.w > 0)
				lv_img_set_zoom(bg, (uint16_t)(256 * w->w / bd->bar_img_dsc->header.w));
			lv_obj_set_style_img_opa(bg, bd->bar_bg_opa, 0);
			bd->img_bg_obj = bg;
			w->root = bg;
		} else {
			ESP_LOGW(TAG, "Failed to load track image '%s', using color track", bd->bar_image);
			has_track = false;
		}
	}
	if (!has_track && has_fill) {
		/* Fill image but no track image — plain styled lv_obj as background */
		lv_obj_t *track_bg = lv_obj_create(parent);
		lv_obj_set_size(track_bg, w->w, w->h);
		lv_obj_set_align(track_bg, LV_ALIGN_CENTER);
		lv_obj_set_pos(track_bg, w->x, w->y);
		lv_obj_set_style_bg_color(track_bg, bd->bar_bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(track_bg, bd->bar_bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(track_bg, bd->bar_border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(track_bg, bd->bar_border_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(track_bg, bd->bar_radius, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(track_bg, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_clear_flag(track_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		bd->img_bg_obj = track_bg;
		w->root = track_bg;
	}

	/* ── Step 2: fill indicator ─────────────────────────────────────── */
	if (has_fill) {
		bd->bar_img_full_dsc = rdm_image_load(bd->bar_image_full);
		safe_strncpy(bd->current_bar_image_full, bd->bar_image_full, sizeof(bd->current_bar_image_full));
		if (bd->bar_img_full_dsc) {
			lv_obj_t *clip = lv_obj_create(parent);
			lv_obj_set_size(clip, 0, w->h);
			lv_obj_set_style_bg_opa(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_width(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_pad_all(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_radius(clip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
			lv_obj_set_style_clip_corner(clip, true, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_align(clip, LV_ALIGN_TOP_LEFT);
			lv_coord_t abs_left = SCREEN_ORIGIN_X + w->x - (w->w / 2);
			lv_coord_t abs_top  = SCREEN_ORIGIN_Y + w->y - (w->h / 2);
			lv_obj_set_pos(clip, abs_left, abs_top);
			bd->img_clip_obj = clip;

			lv_obj_t *fill_img = lv_img_create(clip);
			lv_img_set_src(fill_img, bd->bar_img_full_dsc);
			lv_obj_set_align(fill_img, LV_ALIGN_TOP_LEFT);
			lv_obj_set_pos(fill_img, 0, 0);
			if (bd->bar_img_full_dsc->header.w > 0)
				lv_img_set_zoom(fill_img, (uint16_t)(256 * w->w / bd->bar_img_full_dsc->header.w));
			bd->img_full_obj = fill_img;
		} else {
			ESP_LOGW(TAG, "Failed to load fill image '%s', using color fill", bd->bar_image_full);
			has_fill = false;
		}
	}

	if (!has_fill) {
		/* Standard lv_bar for fill indicator */
		lv_obj_t *bar = lv_bar_create(parent);
		lv_bar_set_range(bar, b_min, b_max);
		lv_bar_set_value(bar, b_min, LV_ANIM_OFF);
		lv_obj_set_width(bar, w->w);
		lv_obj_set_height(bar, w->h);
		lv_obj_set_align(bar, LV_ALIGN_CENTER);
		lv_obj_set_pos(bar, w->x, w->y);
		lv_obj_set_style_radius(bar, bd->bar_radius, LV_PART_MAIN | LV_STATE_DEFAULT);
		if (has_track) {
			/* Track image is the visual background — make lv_bar body transparent */
			lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
		} else {
			lv_obj_set_style_bg_color(bar, bd->bar_bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_opa(bar, bd->bar_bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_width(bar, bd->bar_border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_border_color(bar, bd->bar_border_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		lv_obj_set_style_pad_all(bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(bar, bd->indicator_radius, LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(bar, THEME_COLOR_GREEN_BRIGHT,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(bar, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
		bd->bar_obj = bar;
		if (!w->root) w->root = bar;
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
	/* Apply Show Label preference. Default is true so existing layouts keep
	 * their label rendered; user can hide it for a clean value-only bar. */
	if (bd && !bd->show_bar_label)
		lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);

	/* Value label sits at the bar's right end, vertically centered with the
	 * bar fill, right-aligned so the digits hug the inside of the right edge.
	 * Width 60px is enough for "1234" / "75.3" style readouts; the label is
	 * raised to the foreground so the bar's coloured fill doesn't paint over
	 * it when the bar is near full. The previous position (+50px past the
	 * right edge, above the bar) put BAR2's label entirely off-screen — show
	 * value silently did nothing for the right-side bar. */
	#define BAR_VALUE_W      60
	#define BAR_VALUE_PAD_R  3   /* gap between digits and bar's right edge */
	lv_obj_t *val = lv_label_create(parent);
	lv_obj_set_width(val, BAR_VALUE_W);
	lv_obj_set_height(val, LV_SIZE_CONTENT);
	lv_obj_set_align(val, LV_ALIGN_CENTER);
	lv_obj_set_pos(val,
		w->x + (w->w / 2) - (BAR_VALUE_W / 2) - BAR_VALUE_PAD_R,
		w->y);
	lv_label_set_text(val, "---");
	lv_obj_set_style_text_color(val, bd->value_color,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *bar_val_font = bd ? widget_resolve_font(bd->value_font) : NULL;
	lv_obj_set_style_text_font(val, bar_val_font ? bar_val_font : THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_move_foreground(val);
	if (!(bd && bd->show_bar_value))
		lv_obj_add_flag(val, LV_OBJ_FLAG_HIDDEN);

	/* Store per-instance pointers for signal callback */
	if (bd) {
		bd->label_obj = lbl;
		bd->value_obj = val;
	}

	/* Tick marks (optional) — siblings overlaid on the bar, built once here
	 * and only rebuilt on resize / inspector edit (never per signal tick). */
	bd->tick_obj_count = 0;
	if (bd->show_ticks)
		_bar_build_ticks(w);

	/* Assign to slot globals so existing code (RPM limiter, callbacks) works */
	if (slot == 0) {
		ui_Bar_1 = bd ? bd->bar_obj : NULL;
		ui_Bar_1_Label = lbl;
		ui_Bar_1_Value = val;
	} else {
		ui_Bar_2 = bd ? bd->bar_obj : NULL;
		ui_Bar_2_Label = lbl;
		ui_Bar_2_Value = val;
	}

	/* Subscribe to signal if bound */
	if (bd && bd->signal_index >= 0)
		signal_subscribe(bd->signal_index, _bar_on_signal, w);

	if (bd->channel)
		channel_manager_subscribe((channel_t *)bd->channel,
		                           _bar_on_channel_changed, w);
	(void)0;

	/* Subscribe to night-mode changes if any night override is set, and apply
	 * current state immediately so the widget renders correctly even if it
	 * was created while night-mode is already active. */
	if (bd && (bd->night.has_bar_low_color      || bd->night.has_bar_high_color    ||
	           bd->night.has_bar_in_range_color || bd->night.has_bar_bg_color      ||
	           bd->night.has_bar_border_color   || bd->night.has_label_color       ||
	           bd->night.has_value_color        || bd->night.has_tick_color        ||
	           bd->night.has_bar_image          || bd->night.has_bar_image_full)) {
		night_mode_subscribe(_bar_night_cb, w);
		_bar_apply_night_mode(w, night_mode_is_active());
	}
}
static void _bar_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, nw, nh);
	w->w = nw;
	w->h = nh;
	/* Keep the label + value glued to their relative positions when the bar
	 * grows / shrinks live in the editor. Without this the value label stays
	 * pinned to the original right-edge x and drifts off the bar end. */
	bar_data_t *bd = (bar_data_t *)w->type_data;
	if (bd && bd->label_obj && lv_obj_is_valid(bd->label_obj))
		lv_obj_set_pos(bd->label_obj, w->x, w->y - 28);
	if (bd && bd->value_obj && lv_obj_is_valid(bd->value_obj))
		lv_obj_set_pos(bd->value_obj,
			w->x + (w->w / 2) - 33,   /* BAR_VALUE_W/2 + BAR_VALUE_PAD_R */
			w->y);
	/* Tick spacing/placement depends on w->w / w->h — rebuild against the new
	 * geometry (free + recreate, so spacing recomputes cleanly). */
	if (bd && bd->show_ticks)
		_bar_build_ticks(w);
	else if (bd)
		_bar_free_ticks(bd);
	/* Geometry changed — repaint cleanly on the next tick. */
	if (bd) bd->_pc_valid = false;
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
		if (bd->anchor_enabled)
			cJSON_AddBoolToObject(cfg, "anchor_enabled", true);
		if (bd->anchor_enabled || bd->anchor_value != (bd->bar_min + bd->bar_max) / 2)
			cJSON_AddNumberToObject(cfg, "anchor_value", bd->anchor_value);
		if (bd->anchor_enabled || bd->anchor_position != 50)
			cJSON_AddNumberToObject(cfg, "anchor_position", bd->anchor_position);
		/* Alert thresholds (bar_low / bar_high) live on the bound channel
		 * (channels.json) and are intentionally NOT persisted here, so they
		 * stick to the channel setup and don't travel when a layout is shared.
		 * Alert COLOURS are widget-owned styling and do round-trip. */
		cJSON_AddNumberToObject(cfg, "bar_low_color", (int)bd->bar_low_color.full);
		cJSON_AddNumberToObject(cfg, "bar_high_color", (int)bd->bar_high_color.full);
		cJSON_AddNumberToObject(cfg, "bar_in_range_color", (int)bd->bar_in_range_color.full);
		/* Multi-stop gradient. Old 2-stop fields (bar_grad_enabled /
		 * bar_grad_end_color) are dropped on save — they're only read
		 * back in _bar_from_json for legacy-layout migration. */
		{
			cJSON *gs = gradient_stops_to_json(&bd->grad_stops);
			if (gs) cJSON_AddItemToObject(cfg, "grad_stops", gs);
		}
		cJSON_AddBoolToObject(cfg, "show_bar_value", bd->show_bar_value);
		/* Defaults-only: emit show_bar_label only when the user has hidden it.
		 * Saves a few bytes in the common path (default true). */
		if (!bd->show_bar_label)
			cJSON_AddBoolToObject(cfg, "show_bar_label", false);
		cJSON_AddBoolToObject(cfg, "invert_bar_value", bd->invert_bar_value);
		/* Decimals: per-widget override only — omit when it matches the bound
			 * channel's decimals so it follows the channel by default. */
			if (!bd->channel || bd->decimals != ((channel_t *)bd->channel)->decimals)
				cJSON_AddNumberToObject(cfg, "decimals", bd->decimals);
		if (bd->label_font[0] != '\0')
			cJSON_AddStringToObject(cfg, "label_font", bd->label_font);
		if (bd->value_font[0] != '\0')
			cJSON_AddStringToObject(cfg, "value_font", bd->value_font);
		if (bd->signal_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "signal_name", bd->signal_name);
		if (bd->channel_id[0] != '\0')
			cJSON_AddStringToObject(cfg, "channel", bd->channel_id);
		/* Appearance overrides — only serialize non-default values */
		if (bd->bar_bg_color.full != THEME_COLOR_PANEL.full)
			cJSON_AddNumberToObject(cfg, "bar_bg_color", (int)bd->bar_bg_color.full);
		if (bd->bar_bg_opa != 255)
			cJSON_AddNumberToObject(cfg, "bar_bg_opa", bd->bar_bg_opa);
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
		/* Tick marks — defaults-only. Defaults: show_ticks=false, tick_count=5,
		 * tick_length=6, tick_width=2, tick_color=TEXT_PRIMARY, tick_side=2. */
		if (bd->show_ticks)
			cJSON_AddBoolToObject(cfg, "show_ticks", true);
		if (bd->tick_count != 5)
			cJSON_AddNumberToObject(cfg, "tick_count", bd->tick_count);
		if (bd->tick_length != 6)
			cJSON_AddNumberToObject(cfg, "tick_length", bd->tick_length);
		if (bd->tick_width != 2)
			cJSON_AddNumberToObject(cfg, "tick_width", bd->tick_width);
		if (bd->tick_color.full != THEME_COLOR_TEXT_PRIMARY.full)
			cJSON_AddNumberToObject(cfg, "tick_color", (int)bd->tick_color.full);
		if (bd->tick_side != 2)
			cJSON_AddNumberToObject(cfg, "tick_side", bd->tick_side);
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
		NIGHT_SERIALIZE_COLOR(n, bd->night, tick_color);
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
	if (cJSON_IsNumber(item)) bd->bar_min = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_max");
	if (cJSON_IsNumber(item)) bd->bar_max = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_enabled");
	if (cJSON_IsBool(item)) bd->anchor_enabled = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_value");
	if (cJSON_IsNumber(item)) bd->anchor_value = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_position");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 0)   v = 0;
		if (v > 100) v = 100;
		bd->anchor_position = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_low");
	if (cJSON_IsNumber(item)) bd->bar_low = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_high");
	if (cJSON_IsNumber(item)) bd->bar_high = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_low_color");
	if (cJSON_IsNumber(item)) bd->bar_low_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_high_color");
	if (cJSON_IsNumber(item)) bd->bar_high_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_in_range_color");
	if (cJSON_IsNumber(item)) bd->bar_in_range_color.full = (uint32_t)item->valueint;
	/* Prefer new N-stop array; fall back to the legacy 2-stop fields
	 * so layouts saved before this revision keep their gradient. */
	const cJSON *gs_arr = cJSON_GetObjectItemCaseSensitive(cfg, "grad_stops");
	if (!gradient_stops_from_json(gs_arr, &bd->grad_stops)) {
		const cJSON *ge  = cJSON_GetObjectItemCaseSensitive(cfg, "bar_grad_enabled");
		const cJSON *gec = cJSON_GetObjectItemCaseSensitive(cfg, "bar_grad_end_color");
		if (cJSON_IsBool(ge) && cJSON_IsTrue(ge) && cJSON_IsNumber(gec)) {
			gradient_stops_install_legacy_2stop(&bd->grad_stops,
				bd->bar_in_range_color.full, (uint16_t)gec->valueint);
		}
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_bar_value");
	if (cJSON_IsBool(item)) bd->show_bar_value = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_bar_label");
	if (cJSON_IsBool(item)) bd->show_bar_label = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "invert_bar_value");
	if (cJSON_IsBool(item)) bd->invert_bar_value = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "decimals");
	bool decimals_overridden = cJSON_IsNumber(item);
	if (decimals_overridden) bd->decimals = (uint8_t)item->valueint;
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
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bar_bg_opa");
	if (cJSON_IsNumber(item)) bd->bar_bg_opa = (uint8_t)item->valueint;
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

	/* Tick marks */
	item = cJSON_GetObjectItemCaseSensitive(cfg, "show_ticks");
	if (cJSON_IsBool(item)) bd->show_ticks = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_count");
	if (cJSON_IsNumber(item)) bd->tick_count = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_length");
	if (cJSON_IsNumber(item)) bd->tick_length = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_width");
	if (cJSON_IsNumber(item)) bd->tick_width = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_color");
	if (cJSON_IsNumber(item)) bd->tick_color.full = (uint32_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_side");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 0) v = 0;
		if (v > 2) v = 2;
		bd->tick_side = (uint8_t)v;
	}

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
		NIGHT_PARSE_COLOR(night, bd->night, tick_color);
		NIGHT_PARSE_IMAGE(night, bd->night, bar_image);
		NIGHT_PARSE_IMAGE(night, bd->night, bar_image_full);
	}

	/* Resolve signal name → index */
	if (bd->signal_name[0] != '\0')
		bd->signal_index = signal_find_by_name(bd->signal_name);

	/* ── v14 channel binding + backwards-compat migration ───────── */
	cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(cfg, "channel");
	/* Stash channel id (if any) but treat an empty-signal channel as a
	 * miss so the legacy fallback can repopulate it from the widget's
	 * own signal field. Happens on cross-layout switches where
	 * resolve_signals cleared the stale binding before widgets load. */
	if (cJSON_IsString(ch_item) && ch_item->valuestring && ch_item->valuestring[0] != '\0')
		safe_strncpy(bd->channel_id, ch_item->valuestring, sizeof(bd->channel_id));
	channel_t *bound_c = bd->channel_id[0] ? channel_manager_get(bd->channel_id) : NULL;
	if (bound_c && bound_c->signal_index >= 0) {
		bd->channel = bound_c;
		safe_strncpy(bd->signal_name, bound_c->signal_name, sizeof(bd->signal_name));
		bd->signal_index = bound_c->signal_index;
		/* Decimals default to the channel; a per-widget override in the layout
		 * wins (config modal → Display → Decimals). */
		if (!decimals_overridden) bd->decimals = bound_c->decimals;
		bd->bar_min = bound_c->min;
		bd->bar_max = bound_c->max;
		/* Channel owns the alert thresholds. When a side has a warn it's
		 * authoritative; when it doesn't but the widget carried a legacy
		 * threshold inside the range (older layouts), migrate it UP into the
		 * channel. Otherwise park at the range edge (alert inactive). */
		bool _ch_dirty = false;
		if (bound_c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
			bd->bar_high = bound_c->high_warn;
		} else if (bd->bar_high > bd->bar_min && bd->bar_high < bd->bar_max) {
			bound_c->high_warn = bd->bar_high;   /* real threshold, strictly in range */
			_ch_dirty = true;
		} else {
			bd->bar_high = bd->bar_max;          /* edge / unset → high alert inactive */
		}
		if (bound_c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW) {
			bd->bar_low = bound_c->low_warn;
		} else if (bd->bar_low > bd->bar_min && bd->bar_low < bd->bar_max) {
			bound_c->low_warn = bd->bar_low;
			_ch_dirty = true;
		} else {
			bd->bar_low = bd->bar_min;
		}
		if (_ch_dirty) channel_manager_mark_dirty();
		/* Bar zone colours stay widget-owned — never overridden by the channel. */
	} else if (bd->signal_name[0] != '\0') {
		legacy_widget_data_t legacy = {
			.signal_name = bd->signal_name,
			.min = bd->bar_min,
			.max = bd->bar_max,
			.high_warn = bd->bar_high,
			.color_normal = CHANNEL_USE_DEFAULT_COLOR,
			.color_high_warn = lv_color_to32(bd->bar_high_color) & 0xFFFFFF,
		};
		channel_t *c = channel_manager_record_legacy_widget(&legacy);
		if (c) {
			bd->channel = c;
			if (!decimals_overridden) bd->decimals = c->decimals;
			/* Migrate the widget's legacy alert thresholds UP into the channel
			 * when the channel doesn't already define them (a real threshold is
			 * one inside the range; an edge value means "no alert"). Never
			 * clobber an existing channel warn — that's the source of truth. */
			if (c->high_warn == CHANNEL_THRESHOLD_UNSET_HIGH &&
			    bd->bar_high > bd->bar_min && bd->bar_high < bd->bar_max) {
				c->high_warn = bd->bar_high;
				c->color_high_warn = lv_color_to32(bd->bar_high_color) & 0xFFFFFF;
				channel_manager_mark_dirty();
			}
			if (c->low_warn == CHANNEL_THRESHOLD_UNSET_LOW &&
			    bd->bar_low > bd->bar_min && bd->bar_low < bd->bar_max) {
				c->low_warn = bd->bar_low;
				c->color_low_warn = lv_color_to32(bd->bar_low_color) & 0xFFFFFF;
				channel_manager_mark_dirty();
			}
			/* Adopt the channel's thresholds (single source of truth); a side
			 * with no channel warn parks at the range edge = alert inactive. */
			bd->bar_high = (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) ? c->high_warn : bd->bar_max;
			bd->bar_low  = (c->low_warn  != CHANNEL_THRESHOLD_UNSET_LOW)  ? c->low_warn  : bd->bar_min;
		}
	}
}
static void _bar_destroy(widget_t *w) {
	bar_data_t *bd = (bar_data_t *)w->type_data;
	uint8_t slot = bd ? bd->slot : 0;
	if (bd && bd->signal_index >= 0)
		signal_unsubscribe(bd->signal_index, _bar_on_signal, w);
	if (bd && bd->channel) {
		channel_manager_unsubscribe((channel_t *)bd->channel,
		                             _bar_on_channel_changed, w);
		bd->channel = NULL;
	}
	night_mode_unsubscribe(_bar_night_cb, w);
	widget_rules_free(w);
	/* Label and value are siblings of root (children of parent), delete explicitly */
	if (bd && bd->label_obj && lv_obj_is_valid(bd->label_obj))
		lv_obj_del(bd->label_obj);
	if (bd && bd->value_obj && lv_obj_is_valid(bd->value_obj))
		lv_obj_del(bd->value_obj);
	/* Tick marks are siblings of root too — delete explicitly + NULL them. */
	if (bd) _bar_free_ticks(bd);
	/* Clip container is always a sibling of root — delete explicitly */
	if (bd && bd->img_clip_obj && lv_obj_is_valid(bd->img_clip_obj))
		lv_obj_del(bd->img_clip_obj);
	/* In track-image + color-fill mode, bar_obj is a sibling of root */
	if (bd && bd->bar_obj && lv_obj_is_valid(bd->bar_obj) && (lv_obj_t *)bd->bar_obj != w->root)
		lv_obj_del(bd->bar_obj);
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
	/* Fill-image mode: plain track lv_obj stored in img_bg_obj */
	if (!_bar_has_track_image(bd) && bd->img_bg_obj && lv_obj_is_valid(bd->img_bg_obj)) {
		lv_obj_set_style_bg_color(bd->img_bg_obj, bar_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(bd->img_bg_obj, bar_bdr, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(bd->img_bg_obj, bar_bdrw, LV_PART_MAIN | LV_STATE_DEFAULT);
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

	/* Night palette changed — the per-value indicator color is night-resolved
	 * inside _bar_on_signal, so drop the memo to guarantee it re-applies. */
	bd->_pc_valid = false;

	lv_color_t bar_bg  = NIGHT_PICK_COLOR(active, bd->night, bar_bg_color,     bd->bar_bg_color);
	lv_color_t bar_bdr = NIGHT_PICK_COLOR(active, bd->night, bar_border_color, bd->bar_border_color);
	lv_color_t lbl_col = NIGHT_PICK_COLOR(active, bd->night, label_color,      bd->label_color);
	lv_color_t val_col = NIGHT_PICK_COLOR(active, bd->night, value_color,      bd->value_color);
	lv_color_t tick_col = NIGHT_PICK_COLOR(active, bd->night, tick_color,      bd->tick_color);

	if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj)) {
		lv_obj_set_style_bg_color(bd->bar_obj, bar_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(bd->bar_obj, bar_bdr, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (!_bar_has_track_image(bd) && bd->img_bg_obj && lv_obj_is_valid(bd->img_bg_obj)) {
		lv_obj_set_style_bg_color(bd->img_bg_obj, bar_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(bd->img_bg_obj, bar_bdr, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (bd->label_obj && lv_obj_is_valid(bd->label_obj)) {
		lv_obj_set_style_text_color(bd->label_obj, lbl_col, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (bd->value_obj && lv_obj_is_valid(bd->value_obj)) {
		lv_obj_set_style_text_color(bd->value_obj, val_col, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	/* Ticks are plain bg_color objects — a normal style write recolours them
	 * (no dual-object pattern needed). */
	for (uint8_t i = 0; i < bd->tick_obj_count; i++) {
		if (bd->tick_objs[i] && lv_obj_is_valid(bd->tick_objs[i]))
			lv_obj_set_style_bg_color(bd->tick_objs[i], tick_col,
				LV_PART_MAIN | LV_STATE_DEFAULT);
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

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Schema names match schema/widgets.schema.json. The set hook writes BOTH
 * the type_data field AND the matching LVGL property on the live objects so
 * the user sees the edit land immediately through the translucent dock.
 *
 * Image fields (bar_image / bar_image_full) write through to type_data but
 * skip live LVGL preview — swapping the underlying lv_img source would need
 * to coordinate with the existing track / fill / clip object tree, which
 * differs depending on whether the bar started in colour or image mode. The
 * change lands on the next dashboard rebuild. */

static bool _bar_inspector_get(const widget_t *w, const char *name,
                               widget_field_value_t *out) {
	if (!w || w->type != WIDGET_BAR || !w->type_data || !name || !out) return false;
	const bar_data_t *bd = (const bar_data_t *)w->type_data;

	if (strcmp(name, "label") == 0)              { out->str = bd->label;              return true; }
	if (strcmp(name, "signal_name") == 0)        { out->str = bd->signal_name;        return true; }
	if (strcmp(name, "label_font") == 0)         { out->str = bd->label_font;         return true; }
	if (strcmp(name, "value_font") == 0)         { out->str = bd->value_font;         return true; }
	if (strcmp(name, "bar_image") == 0)          { out->str = bd->bar_image;          return true; }
	if (strcmp(name, "bar_image_full") == 0)     { out->str = bd->bar_image_full;     return true; }
	if (strcmp(name, "bar_min") == 0)            { out->i = bd->bar_min;              return true; }
	if (strcmp(name, "bar_max") == 0)            { out->i = bd->bar_max;              return true; }
	if (strcmp(name, "decimals") == 0)           { out->i = bd->decimals;             return true; }
	if (strcmp(name, "show_bar_value") == 0)     { out->b = bd->show_bar_value;       return true; }
	if (strcmp(name, "invert_bar_value") == 0)   { out->b = bd->invert_bar_value;     return true; }
	if (strcmp(name, "show_bar_label") == 0)     { out->b = bd->show_bar_label;       return true; }
	if (strcmp(name, "anchor_enabled") == 0)     { out->b = bd->anchor_enabled;       return true; }
	if (strcmp(name, "anchor_value") == 0)       { out->i = bd->anchor_value;         return true; }
	if (strcmp(name, "anchor_position") == 0)    { out->i = bd->anchor_position;      return true; }
	if (strcmp(name, "bar_radius") == 0)         { out->i = bd->bar_radius;           return true; }
	if (strcmp(name, "bar_border_width") == 0)   { out->i = bd->bar_border_width;     return true; }
	if (strcmp(name, "indicator_radius") == 0)   { out->i = bd->indicator_radius;     return true; }
	if (strcmp(name, "bar_in_range_color") == 0) { out->color = lv_color_to32(bd->bar_in_range_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "bar_bg_color") == 0)       { out->color = lv_color_to32(bd->bar_bg_color)       & 0xFFFFFF; return true; }
	if (strcmp(name, "bar_border_color") == 0)   { out->color = lv_color_to32(bd->bar_border_color)   & 0xFFFFFF; return true; }
	if (strcmp(name, "label_color") == 0)        { out->color = lv_color_to32(bd->label_color)        & 0xFFFFFF; return true; }
	if (strcmp(name, "value_color") == 0)        { out->color = lv_color_to32(bd->value_color)        & 0xFFFFFF; return true; }
	if (strcmp(name, "show_ticks") == 0)         { out->b = bd->show_ticks;           return true; }
	if (strcmp(name, "tick_count") == 0)         { out->i = bd->tick_count;           return true; }
	if (strcmp(name, "tick_length") == 0)        { out->i = bd->tick_length;          return true; }
	if (strcmp(name, "tick_width") == 0)         { out->i = bd->tick_width;           return true; }
	if (strcmp(name, "tick_side") == 0)          { out->i = bd->tick_side;            return true; }
	if (strcmp(name, "tick_color") == 0)         { out->color = lv_color_to32(bd->tick_color)         & 0xFFFFFF; return true; }
	if (strcmp(name, "bar_low") == 0)            { out->i = bd->bar_low;              return true; }
	if (strcmp(name, "bar_high") == 0)           { out->i = bd->bar_high;             return true; }
	if (strcmp(name, "bar_low_color") == 0)      { out->color = lv_color_to32(bd->bar_low_color)      & 0xFFFFFF; return true; }
	if (strcmp(name, "bar_high_color") == 0)     { out->color = lv_color_to32(bd->bar_high_color)     & 0xFFFFFF; return true; }
	/* bar_alerts_enabled is derived — the bar has no dedicated enable flag,
	 * the runtime treats "both thresholds == 0" as alerts off. */
	if (strcmp(name, "bar_alerts_enabled") == 0) {
		out->b = (bd->bar_low != 0 || bd->bar_high != 0);
		return true;
	}
	return false;
}

static bool _bar_inspector_set(widget_t *w, const char *name,
                               const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_BAR || !w->type_data || !name || !in) return false;
	bar_data_t *bd = (bar_data_t *)w->type_data;

	if (strcmp(name, "label") == 0 && in->str) {
		safe_strncpy(bd->label, in->str, sizeof(bd->label));
		if (bd->label_obj && lv_obj_is_valid(bd->label_obj))
			lv_label_set_text(bd->label_obj, bd->label);
		return true;
	}
	if (strcmp(name, "signal_name") == 0 && in->str) {
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;

		if (bd->signal_index >= 0)
			signal_unsubscribe(bd->signal_index, _bar_on_signal, w);
		safe_strncpy(bd->signal_name, in->str, sizeof(bd->signal_name));
		bd->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _bar_on_signal, w);
		return true;
	}
	if (strcmp(name, "label_font") == 0 && in->str) {
		safe_strncpy(bd->label_font, in->str, sizeof(bd->label_font));
		const lv_font_t *f = widget_resolve_font(bd->label_font);
		if (bd->label_obj && lv_obj_is_valid(bd->label_obj))
			lv_obj_set_style_text_font(bd->label_obj,
				f ? f : THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "value_font") == 0 && in->str) {
		safe_strncpy(bd->value_font, in->str, sizeof(bd->value_font));
		const lv_font_t *f = widget_resolve_font(bd->value_font);
		if (bd->value_obj && lv_obj_is_valid(bd->value_obj))
			lv_obj_set_style_text_font(bd->value_obj,
				f ? f : THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "bar_image") == 0 && in->str) {
		safe_strncpy(bd->bar_image, in->str, sizeof(bd->bar_image));
		return true;   /* live swap requires rebuild — applied on next reload */
	}
	if (strcmp(name, "bar_image_full") == 0 && in->str) {
		safe_strncpy(bd->bar_image_full, in->str, sizeof(bd->bar_image_full));
		return true;   /* see bar_image */
	}
	if (strcmp(name, "bar_min") == 0) {
		bd->bar_min = (int32_t)in->i;
		widget_bar_sync_range(bd);
		return true;
	}
	if (strcmp(name, "bar_max") == 0) {
		bd->bar_max = (int32_t)in->i;
		widget_bar_sync_range(bd);
		return true;
	}
	if (strcmp(name, "decimals") == 0) {
		bd->decimals = (uint8_t)in->i;
		widget_bar_sync_range(bd);
		return true;
	}
	if (strcmp(name, "show_bar_value") == 0) {
		bd->show_bar_value = in->b;
		if (bd->value_obj && lv_obj_is_valid(bd->value_obj)) {
			if (bd->show_bar_value) lv_obj_clear_flag(bd->value_obj, LV_OBJ_FLAG_HIDDEN);
			else                    lv_obj_add_flag  (bd->value_obj, LV_OBJ_FLAG_HIDDEN);
		}
		return true;
	}
	if (strcmp(name, "invert_bar_value") == 0) {
		bd->invert_bar_value = in->b;
		return true;   /* picked up by the next _bar_on_signal call */
	}
	if (strcmp(name, "show_bar_label") == 0) {
		bd->show_bar_label = in->b;
		if (bd->label_obj && lv_obj_is_valid(bd->label_obj)) {
			if (bd->show_bar_label) lv_obj_clear_flag(bd->label_obj, LV_OBJ_FLAG_HIDDEN);
			else                    lv_obj_add_flag  (bd->label_obj, LV_OBJ_FLAG_HIDDEN);
		}
		return true;
	}
	if (strcmp(name, "anchor_enabled") == 0) {
		bd->anchor_enabled = in->b;
		return true;
	}
	if (strcmp(name, "anchor_value") == 0) {
		bd->anchor_value = (int32_t)in->i;
		return true;
	}
	if (strcmp(name, "anchor_position") == 0) {
		int v = in->i;
		if (v < 0)   v = 0;
		if (v > 100) v = 100;
		bd->anchor_position = (uint8_t)v;
		return true;
	}
	if (strcmp(name, "bar_radius") == 0) {
		bd->bar_radius = (uint8_t)in->i;
		if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj))
			lv_obj_set_style_radius(bd->bar_obj, bd->bar_radius, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "bar_border_width") == 0) {
		bd->bar_border_width = (uint8_t)in->i;
		if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj))
			lv_obj_set_style_border_width(bd->bar_obj, bd->bar_border_width,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "indicator_radius") == 0) {
		bd->indicator_radius = (uint8_t)in->i;
		if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj))
			lv_obj_set_style_radius(bd->bar_obj, bd->indicator_radius,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "bar_bg_color") == 0) {
		bd->bar_bg_color = lv_color_hex(in->color);
		if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj))
			lv_obj_set_style_bg_color(bd->bar_obj, bd->bar_bg_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		/* In track-image-less + fill-image mode, the styled lv_obj track lives
		 * on bd->img_bg_obj — mirror the colour there too. */
		if (!_bar_has_track_image(bd) && bd->img_bg_obj && lv_obj_is_valid(bd->img_bg_obj))
			lv_obj_set_style_bg_color(bd->img_bg_obj, bd->bar_bg_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "bar_border_color") == 0) {
		bd->bar_border_color = lv_color_hex(in->color);
		if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj))
			lv_obj_set_style_border_color(bd->bar_obj, bd->bar_border_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		if (!_bar_has_track_image(bd) && bd->img_bg_obj && lv_obj_is_valid(bd->img_bg_obj))
			lv_obj_set_style_border_color(bd->img_bg_obj, bd->bar_border_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "bar_in_range_color") == 0) {
		bd->bar_in_range_color = lv_color_hex(in->color);
		/* Set indicator immediately for preview; runtime _bar_on_signal will
		 * pick the appropriate low / high / in-range colour on the next CAN
		 * frame regardless. */
		if (bd->bar_obj && lv_obj_is_valid(bd->bar_obj))
			lv_obj_set_style_bg_color(bd->bar_obj, bd->bar_in_range_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "label_color") == 0) {
		bd->label_color = lv_color_hex(in->color);
		if (bd->label_obj && lv_obj_is_valid(bd->label_obj))
			lv_obj_set_style_text_color(bd->label_obj, bd->label_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "value_color") == 0) {
		bd->value_color = lv_color_hex(in->color);
		if (bd->value_obj && lv_obj_is_valid(bd->value_obj))
			lv_obj_set_style_text_color(bd->value_obj, bd->value_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	/* Tick edits all rebuild the tick objects so spacing / geometry / colour
	 * land live. _bar_build_ticks tears down the old set and recreates from
	 * the current fields; a no-show_ticks state just frees them. */
	if (strcmp(name, "show_ticks") == 0) {
		bd->show_ticks = in->b;
		if (bd->show_ticks) _bar_build_ticks(w);
		else                _bar_free_ticks(bd);
		return true;
	}
	if (strcmp(name, "tick_count") == 0) {
		bd->tick_count = (uint8_t)in->i;
		if (bd->show_ticks) _bar_build_ticks(w);
		return true;
	}
	if (strcmp(name, "tick_length") == 0) {
		bd->tick_length = (uint8_t)in->i;
		if (bd->show_ticks) _bar_build_ticks(w);
		return true;
	}
	if (strcmp(name, "tick_width") == 0) {
		bd->tick_width = (uint8_t)in->i;
		if (bd->show_ticks) _bar_build_ticks(w);
		return true;
	}
	if (strcmp(name, "tick_side") == 0) {
		int v = in->i;
		if (v < 0) v = 0;
		if (v > 2) v = 2;
		bd->tick_side = (uint8_t)v;
		if (bd->show_ticks) _bar_build_ticks(w);
		return true;
	}
	if (strcmp(name, "tick_color") == 0) {
		bd->tick_color = lv_color_hex(in->color);
		if (bd->show_ticks) _bar_build_ticks(w);
		return true;
	}
	if (strcmp(name, "bar_low") == 0) {
		bd->bar_low = (int32_t)in->i;
		return true;
	}
	if (strcmp(name, "bar_high") == 0) {
		bd->bar_high = (int32_t)in->i;
		return true;
	}
	if (strcmp(name, "bar_low_color") == 0) {
		bd->bar_low_color = lv_color_hex(in->color);
		return true;
	}
	if (strcmp(name, "bar_high_color") == 0) {
		bd->bar_high_color = lv_color_hex(in->color);
		return true;
	}
	if (strcmp(name, "bar_alerts_enabled") == 0) {
		/* Synthesised toggle: turning alerts OFF clears both thresholds so the
		 * runtime stops applying low / high colours. Turning ON is a no-op —
		 * the user must still pick the threshold values themselves. */
		if (!in->b) {
			bd->bar_low  = 0;
			bd->bar_high = 0;
		}
		return true;
	}
	return false;
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
	bd->anchor_value    = 50;   /* midpoint, matches default range 0..100 */
	bd->anchor_position = 50;
	bd->anchor_enabled  = false; /* off by default — pure linear */
	bd->bar_in_range_color = THEME_COLOR_GREEN_BRIGHT;
	bd->bar_low_color = THEME_COLOR_BLUE_DARK;
	bd->bar_high_color = THEME_COLOR_RED;
	/* grad_stops zero-initialised by calloc — count=0 means no gradient,
	 * render path falls back to the solid bar_in_range_color. */
	bd->signal_index = -1;
	bd->bar_bg_color = THEME_COLOR_PANEL;
	bd->bar_bg_opa = 255;
	bd->bar_radius = 5;
	bd->bar_border_width = 2;
	bd->bar_border_color = THEME_COLOR_PANEL;
	bd->indicator_radius = 5;
	bd->label_color = THEME_COLOR_TEXT_PRIMARY;
	bd->value_color = THEME_COLOR_TEXT_PRIMARY;
	bd->show_bar_label = true;        /* show the text label above the bar by default */
	/* Tick-mark defaults (off by default so existing layouts are unaffected) */
	bd->show_ticks  = false;
	bd->tick_count  = 5;
	bd->tick_length = 6;
	bd->tick_width  = 2;
	bd->tick_color  = THEME_COLOR_TEXT_PRIMARY;   /* 0xE8E8E8 */
	bd->tick_side   = 2;                            /* Both */

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
	w->inspector_get = _bar_inspector_get;
	w->inspector_set = _bar_inspector_set;

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
