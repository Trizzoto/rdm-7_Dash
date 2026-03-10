/*
 * ui_Screen3.c — Dashboard coordinator.
 *
 * Owns LVGL object arrays and UI-level state shared across modules.
 * All widget rendering and signal dispatch are handled by the widget
 * system and signal registry.
 */

#include "ui/screens/ui_Screen3.h"
#include "device_id.h"
#include "device_settings.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/callbacks/ui_callbacks.h"
#include "ui/dashboard.h"
#include "ui/menu/menu_screen.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widgets/widget_bar.h"
#include "widgets/widget_indicator.h"
#include "widgets/widget_panel.h"
#include "widgets/widget_rpm_bar.h"
#include "widgets/widget_warning.h"
#include <stdio.h>
#include <string.h>

/* ── Global LVGL object arrays ──────────────────────────────────────────── */
lv_obj_t *ui_Label[13] = {NULL};
lv_obj_t *ui_Value[13] = {NULL};
lv_obj_t *ui_Box[8] = {NULL};
lv_obj_t *ui_CustomText[8] = {NULL};
lv_obj_t *config_bars[13] = {NULL};
lv_obj_t *ui_MenuScreen = NULL;
lv_obj_t *keyboard = NULL;
lv_obj_t *rpm_bar_gauge = NULL;
lv_obj_t *rpm_redline_zone = NULL;
lv_timer_t *menu_button_hide_timer = NULL;

/* ── Config form objects (used by config_controls / config_modal) ────────── */
lv_obj_t *g_label_input[MAX_VALUES];
lv_obj_t *g_can_id_input[MAX_VALUES];
lv_obj_t *g_endian_dropdown[MAX_VALUES];
lv_obj_t *g_bit_start_dropdown[MAX_VALUES];
lv_obj_t *g_bit_length_dropdown[MAX_VALUES];
lv_obj_t *g_scale_input[MAX_VALUES];
lv_obj_t *g_offset_input[MAX_VALUES];
lv_obj_t *g_decimals_dropdown[MAX_VALUES];
lv_obj_t *g_type_dropdown[MAX_VALUES];

int rpm_gauge_max = 7000;
int rpm_redline_value = 6000;
uint8_t current_value_id;
char value_offset_texts[13][64] = {"0", "0", "0", "0", "0", "0", "0",
								   "0", "0", "0", "0", "0", "0"};
char previous_values[13][64] = {0};
bool reset_can_tracking = false;

/* ── Coordinator-local state ────────────────────────────────────────────── */
static uint32_t touch_press_time = 0;
static uint32_t last_long_press_time = 0;
static lv_obj_t *ui_Setup_Menu_Screen = NULL;

/* ── Shared extern callbacks (defined elsewhere, used here) ─────────────── */
extern void device_settings_longpress_cb(lv_event_t *e);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Coordinator-level event callbacks
 * ═══════════════════════════════════════════════════════════════════════════
 */

void keyboard_ready_event_cb(lv_event_t *e) {
	lv_obj_t *kb = lv_event_get_target(e);
	lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

void value_long_press_event_cb(lv_event_t *e) {
	uint32_t now = lv_tick_get();
	if (now - last_long_press_time < 500)
		return;
	last_long_press_time = now;
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	current_value_id = value_id;
	load_menu_screen_for_value(value_id);
}

static void menu_button_hide_timer_cb(lv_timer_t *timer) {
	if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button))
		lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
	if (menu_button_hide_timer) {
		lv_timer_del(menu_button_hide_timer);
		menu_button_hide_timer = NULL;
	}
}

void screen3_touch_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		touch_press_time = lv_tick_get();
	} else if (code == LV_EVENT_RELEASED) {
		uint32_t dur = lv_tick_get() - touch_press_time;
		if (dur < 300 && ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button)) {
			lv_obj_clear_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
			if (menu_button_hide_timer)
				lv_timer_del(menu_button_hide_timer);
			menu_button_hide_timer =
				lv_timer_create(menu_button_hide_timer_cb, 6000, NULL);
			lv_timer_set_repeat_count(menu_button_hide_timer, 1);
		}
		touch_press_time = 0;
	}
}

static void setup_menu_close_btn_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	lv_obj_t *scr = ui_Setup_Menu_Screen;
	if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
		lv_scr_load(ui_Screen3);
		if (scr && lv_obj_is_valid(scr)) {
			lv_obj_del_async(scr);
			ui_Setup_Menu_Screen = NULL;
		}
	}
}

static void menu_button_clicked_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button))
		lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
	if (menu_button_hide_timer) {
		lv_timer_del(menu_button_hide_timer);
		menu_button_hide_timer = NULL;
	}
	ui_Setup_Menu_Screen = lv_obj_create(NULL);
	lv_obj_clear_flag(ui_Setup_Menu_Screen, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ui_Setup_Menu_Screen, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Setup_Menu_Screen, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_t *cb = lv_btn_create(ui_Setup_Menu_Screen);
	lv_obj_set_size(cb, 100, 50);
	lv_obj_align(cb, LV_ALIGN_BOTTOM_MID, 0, -20);
	lv_obj_set_style_bg_color(cb, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cb, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_t *cl = lv_label_create(cb);
	lv_label_set_text(cl, "CLOSE");
	lv_obj_center(cl);
	lv_obj_add_event_cb(cb, setup_menu_close_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_scr_load(ui_Setup_Menu_Screen);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Screen initialisation
 * ═══════════════════════════════════════════════════════════════════════════
 */
void ui_Screen3_screen_init(void) {
	init_styles();
	init_common_style();

	ui_Screen3 = lv_obj_create(NULL);
	lv_obj_clear_flag(ui_Screen3, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ui_Screen3, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Screen3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(ui_Screen3, screen3_touch_event_cb, LV_EVENT_PRESSED,
						NULL);
	lv_obj_add_event_cb(ui_Screen3, screen3_touch_event_cb, LV_EVENT_RELEASED,
						NULL);

	static bool timers_created = false;
	if (!timers_created) {
		lv_timer_create(check_rpm_color_update, 500, NULL);
		lv_timer_create(check_warning_timeouts, 50, NULL);
		indicator_animation_timer =
			lv_timer_create(indicator_animation_timer_cb, 350, NULL);
		lv_timer_pause(indicator_animation_timer);
		timers_created = true;
	}

	/* Clear stale object pointers before re-creation */
	for (int i = 0; i < 13; i++) {
		ui_Label[i] = ui_Value[i] = NULL;
		if (i < 8) {
			ui_Box[i] = ui_CustomText[i] = NULL;
		}
		memset(previous_values[i], 0, sizeof(previous_values[i]));
	}
	rpm_bar_gauge = NULL;
	ui_RPM_Value = NULL;
	ui_RPM_Label = NULL;
	ui_Panel9 = NULL;
	ui_Bar_1 = NULL;
	ui_Bar_2 = NULL;
	/* Clear stale static pointers inside widget_rpm_bar module */
	widget_rpm_bar_clear_stale_pointers();
	reset_can_tracking = true;

	/* Initialise widget layer via layout manager (loads from LittleFS JSON,
	 * falls back to direct widget_X_create() if the file is unavailable). */
	dashboard_init(ui_Screen3);

	/* Click zones for special widgets — only installed if the widget exists */
	if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
		create_transparent_click_zone(ui_Screen3, ui_RPM_Value, RPM_VALUE_ID);
	if (ui_Bar_1 && lv_obj_is_valid(ui_Bar_1))
		create_transparent_click_zone(ui_Screen3, ui_Bar_1, BAR1_VALUE_ID);
	if (ui_Bar_2 && lv_obj_is_valid(ui_Bar_2))
		create_transparent_click_zone(ui_Screen3, ui_Bar_2, BAR2_VALUE_ID);

	/* Menu button (glassmorphism) */
	ui_Menu_Button = lv_btn_create(ui_Screen3);
	lv_obj_set_size(ui_Menu_Button, 90, 40);
	lv_obj_align(ui_Menu_Button, LV_ALIGN_CENTER, 0, 105);
	lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_color(ui_Menu_Button, THEME_COLOR_TEXT_PRIMARY,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Menu_Button, 60,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(ui_Menu_Button, THEME_COLOR_TEXT_PRIMARY,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_Menu_Button, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Menu_Button, 12,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_t *ml = lv_label_create(ui_Menu_Button);
	lv_label_set_text(ml, "MENU");
	lv_obj_set_style_text_color(ml, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ml);
	lv_obj_add_event_cb(ui_Menu_Button, menu_button_clicked_cb,
						LV_EVENT_CLICKED, NULL);

	/* RDM logo (long-press → device settings) */
	ui_RDM_Logo_Text = lv_img_create(ui_Screen3);
	lv_img_set_src(ui_RDM_Logo_Text, &ui_img_RDM_Light);
	lv_obj_set_x(ui_RDM_Logo_Text, 0);
	lv_obj_set_y(ui_RDM_Logo_Text, -65);
	lv_obj_set_align(ui_RDM_Logo_Text, LV_ALIGN_CENTER);
	lv_obj_add_flag(ui_RDM_Logo_Text,
					LV_OBJ_FLAG_ADV_HITTEST | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(ui_RDM_Logo_Text, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(ui_RDM_Logo_Text, device_settings_longpress_cb,
						LV_EVENT_LONG_PRESSED, NULL);
}

void ui_Screen3_preview_layout(cJSON *root) {
	/* Hot-reload: tear down current screen and rebuild from JSON.
	 * Must clear stale widget pointers BEFORE lv_obj_clean frees them. */
	widget_rpm_bar_clear_stale_pointers();
	for (int i = 0; i < 13; i++) {
		ui_Label[i] = ui_Value[i] = NULL;
		if (i < 8) {
			ui_Box[i] = ui_CustomText[i] = NULL;
		}
	}
	rpm_bar_gauge = NULL;
	rpm_redline_zone = NULL;
	ui_RPM_Value = NULL;
	ui_RPM_Label = NULL;
	ui_Panel9 = NULL;
	ui_Bar_1 = NULL;
	ui_Bar_2 = NULL;
	reset_can_tracking = true;

	if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
		lv_obj_clean(ui_Screen3);
	}
	dashboard_apply_layout_json(ui_Screen3, root);
}
