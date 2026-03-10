#include "widget_speed.h"
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
#include "ui/dashboard.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helper: look up speed_data_t ──────────────────────────────────────── */
static speed_data_t *_get_speed_data(void) {
	widget_t **widgets = dashboard_get_widgets();
	uint8_t count = dashboard_get_widget_count();
	for (uint8_t i = 0; i < count; i++) {
		if (widgets[i] && widgets[i]->type == WIDGET_SPEED) {
			return (speed_data_t *)widgets[i]->type_data;
		}
	}
	return NULL;
}

/* Async update payload for lv_async_call(update_speed_ui, ...) */
typedef struct {
	char speed_str[32];
} speed_update_t;

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

void widget_speed_create(lv_obj_t *parent) {
	ui_Speed_Value = lv_label_create(parent);
	lv_obj_set_width(ui_Speed_Value, LV_SIZE_CONTENT);
	lv_obj_set_height(ui_Speed_Value, LV_SIZE_CONTENT);
	lv_obj_set_align(ui_Speed_Value, LV_ALIGN_CENTER);
	lv_obj_set_x(ui_Speed_Value, 0);
	lv_obj_set_y(ui_Speed_Value, 30);
	lv_label_set_text(ui_Speed_Value, "---");
	strcpy(previous_values[SPEED_VALUE_ID - 1], "---");
	lv_obj_set_style_text_color(ui_Speed_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Speed_Value, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Speed_Value, THEME_FONT_DASH_SPEED,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Kmh = lv_label_create(parent);
	lv_obj_set_width(ui_Kmh, LV_SIZE_CONTENT);
	lv_obj_set_height(ui_Kmh, LV_SIZE_CONTENT);
	lv_obj_set_x(ui_Kmh, 37);
	lv_obj_set_y(ui_Kmh, 64);
	lv_obj_set_align(ui_Kmh, LV_ALIGN_CENTER);
	speed_data_t *sd_lookup = _get_speed_data();
	lv_label_set_text(
		ui_Kmh, (sd_lookup && sd_lookup->use_mph) ? "mph" : "k/mh");
	lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Kmh, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Kmh, THEME_FONT_SMALL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
}

uint64_t *widget_speed_get_last_can_time(void) {
	return &last_speed_can_received;
}

bool widget_speed_get_use_mph(void) {
	speed_data_t *sd = _get_speed_data();
	return sd ? sd->use_mph : false;
}

/* ── Phase 2: widget_t factory ───────────────────────────────────────────── */

static void _speed_on_signal(float value, bool is_stale, void *user_data) {
	(void)user_data;
	if (is_stale) {
		update_speed_ui_immediate("---");
		return;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", (int)value);
	update_speed_ui_immediate(buf);
}

static void _speed_create(widget_t *w, lv_obj_t *parent) {
	widget_speed_create(parent);
	w->root = ui_Speed_Value;

	/* The ui_Kmh label is a sibling to ui_Speed_Value in widget_speed_create.
	 * Position it relative to the loaded JSON coordinates. */
	if (ui_Kmh && lv_obj_is_valid(ui_Kmh)) {
		lv_obj_set_x(ui_Kmh, w->x + 37);
		lv_obj_set_y(ui_Kmh, w->y + 34); /* 64 - 30 = 34 diff */
	}

	/* Subscribe to signal if bound */
	speed_data_t *sd = (speed_data_t *)w->type_data;
	if (sd && sd->signal_index >= 0)
		signal_subscribe(sd->signal_index, _speed_on_signal, w);
}
static void _speed_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	/* Phase 6 will implement font-tier switching */
	w->w = nw;
	w->h = nh;
}
static void _speed_open_settings(widget_t *w) { (void)w; }
static void _speed_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	speed_data_t *sd = (speed_data_t *)w->type_data;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	if (sd) {
		cJSON_AddBoolToObject(cfg, "use_mph", sd->use_mph);
		cJSON_AddNumberToObject(cfg, "decimals", sd->decimals);
		if (sd->signal_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "signal_name", sd->signal_name);
	}
}
static void _speed_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
	speed_data_t *sd = (speed_data_t *)w->type_data;
	if (!sd) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;
	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "use_mph");
	if (cJSON_IsBool(item)) {
		sd->use_mph = cJSON_IsTrue(item);
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "decimals");
	if (cJSON_IsNumber(item))
		sd->decimals = (uint8_t)item->valuedouble;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring) {
		strncpy(sd->signal_name, item->valuestring, sizeof(sd->signal_name) - 1);
		sd->signal_name[sizeof(sd->signal_name) - 1] = '\0';
	}

	/* Resolve signal name → index */
	if (sd->signal_name[0] != '\0')
		sd->signal_index = signal_find_by_name(sd->signal_name);
}
static void _speed_destroy(widget_t *w) {
	free(w->type_data);
	free(w);
}

widget_t *widget_speed_create_instance(void) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	speed_data_t *sd = heap_caps_calloc(1, sizeof(speed_data_t), MALLOC_CAP_SPIRAM);
	if (!sd) sd = calloc(1, sizeof(speed_data_t));
	if (!sd) { free(w); return NULL; }

	sd->use_mph = false;   /* default: km/h */
	sd->signal_index = -1;

	w->type = WIDGET_SPEED;
	w->slot = 0;
	w->x = 0;
	w->y = 30;
	w->w = 120;
	w->h = 50;
	w->type_data = sd;
	snprintf(w->id, sizeof(w->id), "speed_0");

	w->create = _speed_create;
	w->resize = _speed_resize;
	w->open_settings = _speed_open_settings;
	w->to_json = _speed_to_json;
	w->from_json = _speed_from_json;
	w->destroy = _speed_destroy;

	return w;
}

