/* menu_screen.c — Phase 0B rebuild */
#include "menu_screen.h"
#include "../callbacks/ui_callbacks.h"
#include "../config/config_controls.h"
#include "../screens/ui_Screen3.h"
#include "../settings/settings_panel.h"
#include "../theme.h"
#include "../ui.h"
#include "config_modal.h"
#include "device_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gear_config.h"
#include "lvgl.h"
#include "preset_picker.h"
#include "storage/config_store.h"
#include <stdio.h>
#include <string.h>

/* Externs not already covered by the headers above */
extern void reconfigure_can_filter(void);
/* Removed limiter demo extern */
extern char previous_values[13][64];
extern lv_obj_t *keyboard;
extern lv_obj_t *ui_Value[];

/* Global previewer references */
lv_obj_t *custom_gear_config_button = NULL;
lv_obj_t *menu_rpm_value_label = NULL;
lv_obj_t *menu_speed_value_label = NULL;
lv_obj_t *menu_speed_units_label = NULL;
lv_obj_t *menu_gear_value_label = NULL;
lv_obj_t *menu_gear_icon = NULL;
lv_obj_t *menu_panel_value_labels[8] = {NULL};
lv_obj_t *menu_panel_boxes[8] = {NULL};
lv_obj_t *menu_panel_labels[8] = {NULL};
lv_obj_t *menu_bar_objects[2] = {NULL};
lv_obj_t *menu_bar_labels[2] = {NULL};

/* ── private helpers ──────────────────────────────────────────── */

static void delete_old_screen_cb(lv_timer_t *t) {
	lv_obj_t *s = (lv_obj_t *)t->user_data;
	if (s) {
		destroy_preconfig_menu();
		lv_obj_del(s);
	}
}

static void clear_menu_refs(void) {
	menu_speed_value_label = menu_speed_units_label = NULL;
	menu_gear_value_label = menu_gear_icon = NULL;
	for (int i = 0; i < 8; i++)
		menu_panel_value_labels[i] = menu_panel_boxes[i] =
			menu_panel_labels[i] = NULL;
	for (int i = 0; i < 2; i++)
		menu_bar_objects[i] = menu_bar_labels[i] = NULL;
}

static void do_screen_transition(lv_obj_t *old, lv_obj_t *btn) {
	ui_Screen3_screen_init();
	if (!ui_Screen3) {
		lv_obj_clear_state(btn, LV_STATE_DISABLED);
		return;
	}
	lv_scr_load_anim(ui_Screen3, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
	lv_timer_t *dt = lv_timer_create(delete_old_screen_cb, 300, old);
	lv_timer_set_repeat_count(dt, 1);
	clear_menu_refs();
}

static lv_obj_t *make_panel_lbl(lv_obj_t *box, const char *txt,
								const lv_font_t *fnt, int y, int w) {
	lv_obj_t *l = lv_label_create(box);
	lv_label_set_text(l, txt);
	lv_obj_set_style_text_color(l, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(l, fnt, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(l, w);
	lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
	lv_obj_set_x(l, 0);
	lv_obj_set_y(l, y);
	lv_obj_set_align(l, LV_ALIGN_CENTER);
	return l;
}

/* ── public API ───────────────────────────────────────────────── */

void close_menu_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	menu_rpm_value_label = NULL;
	menu_speed_value_label = NULL;
	menu_speed_units_label = NULL;
	menu_gear_value_label = NULL;

	lv_obj_t *old = lv_scr_act();
	lv_obj_t *btn = lv_event_get_target(e);
	lv_obj_add_state(btn, LV_STATE_DISABLED);

	custom_gear_section_flush_to_config();

	lv_obj_t *ind = lv_label_create(old);
	lv_label_set_text(ind, "Saving...");
	lv_obj_align(ind, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_text_color(ind, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_refr_now(NULL);

	config_store_save_values(values_config, MAX_VALUES);
	rebuild_can_dispatch();
	reconfigure_can_filter();
	vTaskDelay(pdMS_TO_TICKS(50));
	lv_obj_del(ind);
	do_screen_transition(old, btn);
}

void cancel_menu_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	menu_rpm_value_label = NULL;
	menu_speed_value_label = NULL;
	menu_speed_units_label = NULL;
	menu_gear_value_label = NULL;

	lv_obj_t *old = lv_scr_act();
	lv_obj_t *btn = lv_event_get_target(e);
	lv_obj_add_state(btn, LV_STATE_DISABLED);

	lv_obj_t *ind = lv_label_create(old);
	lv_label_set_text(ind, "Canceling...");
	lv_obj_align(ind, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_text_color(ind, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_refr_now(NULL);

	config_store_load_values(values_config, MAX_VALUES);
	vTaskDelay(pdMS_TO_TICKS(200));
	lv_obj_del(ind);
	do_screen_transition(old, btn);
}

void load_menu_screen_for_value(uint8_t value_id) {
	current_value_id = value_id;
	destroy_preconfig_menu();
	custom_gear_config_button = NULL;

	ui_MenuScreen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(ui_MenuScreen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(ui_MenuScreen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(ui_MenuScreen, LV_OBJ_FLAG_SCROLLABLE);

	/* All widget types now use the unified tabbed modal */
	config_modal_open(ui_MenuScreen, value_id);
	lv_scr_load(ui_MenuScreen);
}

void create_menu_objects(lv_obj_t *parent, uint8_t value_id) {
	uint8_t idx = value_id - 1;

	menu_panel_boxes[idx] = lv_obj_create(parent);
	lv_obj_set_size(menu_panel_boxes[idx], 155, 92);
	lv_obj_set_align(menu_panel_boxes[idx], LV_ALIGN_TOP_LEFT);
	lv_obj_set_pos(menu_panel_boxes[idx], 5, 5);
	lv_obj_clear_flag(menu_panel_boxes[idx], LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_style(menu_panel_boxes[idx], get_box_style(),
					 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(menu_panel_boxes[idx], 3,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(menu_panel_boxes[idx], THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	menu_panel_labels[idx] =
		make_panel_lbl(menu_panel_boxes[idx], label_texts[idx],
					   THEME_FONT_DASH_LABEL, -28, 145);

	const char *cur = (ui_Value[idx] && lv_obj_is_valid(ui_Value[idx]))
						  ? lv_label_get_text(ui_Value[idx])
					  : (strlen(previous_values[idx]) > 0)
						  ? previous_values[idx]
						  : "0";
	menu_panel_value_labels[idx] = make_panel_lbl(
		menu_panel_boxes[idx], cur, THEME_FONT_DASH_VALUE, 9, 140);

	create_config_controls(parent, value_id);
}

void custom_gear_config_btn_event_cb(lv_event_t *e) {
	(void)e;
	create_custom_gear_config_menu();
}
