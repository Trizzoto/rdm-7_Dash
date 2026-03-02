#include "widget_speed.h"
#include "widget_dispatcher.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "can/can_decode.h"
#include "can/can_dispatch.h"
#include "storage/config_store.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/menu/menu_screen.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/callbacks/ui_callbacks.h"

uint64_t last_speed_can_received = 0;

void update_speed_ui(void *param) {
	speed_update_t *s_upd = (speed_update_t *)param;

	if (ui_Speed_Value == NULL || lv_obj_get_screen(ui_Speed_Value) == NULL) {
		free(s_upd);
		return;
	}

	lv_label_set_text(ui_Speed_Value, s_upd->speed_str);

	// Also update menu preview if it exists, is valid, and menu is visible
	if (menu_speed_value_label && lv_obj_is_valid(menu_speed_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_speed_value_label, s_upd->speed_str);
	}

	free(s_upd);
}

// Immediate speed update
void update_speed_ui_immediate(const char *speed_str) {
	if (ui_Speed_Value == NULL || lv_obj_get_screen(ui_Speed_Value) == NULL) {
		return;
	}
	lv_label_set_text(ui_Speed_Value, speed_str);
	if (menu_speed_value_label && lv_obj_is_valid(menu_speed_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_speed_value_label, speed_str);
	}
}

void widget_speed_create(lv_obj_t *parent)
{
    ui_Speed_Value = lv_label_create(parent);
    lv_obj_set_width(ui_Speed_Value, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Speed_Value, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Speed_Value, LV_ALIGN_CENTER);
    lv_obj_set_x(ui_Speed_Value, 0);
    lv_obj_set_y(ui_Speed_Value, 30);
    lv_label_set_text(ui_Speed_Value, "---");
    strcpy(previous_values[SPEED_VALUE_ID - 1], "---");
    lv_obj_set_style_text_color(ui_Speed_Value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Speed_Value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Speed_Value, THEME_FONT_DASH_SPEED, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Kmh = lv_label_create(parent);
    lv_obj_set_width(ui_Kmh, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Kmh, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Kmh, 37);
    lv_obj_set_y(ui_Kmh, 64);
    lv_obj_set_align(ui_Kmh, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Kmh, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
    lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Kmh, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Kmh, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
}

uint64_t *widget_speed_get_last_can_time(void) { return &last_speed_can_received; }
