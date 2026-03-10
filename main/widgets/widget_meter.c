/*
 * widget_meter.c — Analog sweeping gauge for a value slot.
 *
 * Binds to a value slot (0–12). Receives raw int32_t via:
 *     w->update(w, &raw_value);
 * No double-precision math; only int32_t and clamping.
 */
#include "widget_meter.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "signal.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widget_types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_meter";

#define METER_DEFAULT_W 140
#define METER_DEFAULT_H 140

static void _meter_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !w->root || !lv_obj_is_valid(w->root)) return;
	if (is_stale) {
		lv_meter_set_indicator_value(md->meter, md->needle, md->min);
		if (md->value_label && lv_obj_is_valid(md->value_label))
			lv_label_set_text(md->value_label, "---");
		return;
	}
	int32_t v = (int32_t)value;
	if (v < md->min) v = md->min;
	if (v > md->max) v = md->max;
	lv_meter_set_indicator_value(md->meter, md->needle, v);
	if (md->value_label && lv_obj_is_valid(md->value_label)) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", (int)v);
		lv_label_set_text(md->value_label, buf);
	}
}

static void _meter_create(widget_t *w, lv_obj_t *parent) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md) {
		ESP_LOGE(TAG, "_meter_create: missing meter_data");
		return;
	}

	ESP_LOGI(TAG, "_meter_create: calling lv_meter_create, parent=%p",
			 (void *)parent);
	lv_obj_t *m = lv_meter_create(parent);
	if (!m) {
		ESP_LOGE(TAG, "_meter_create: lv_meter_create failed");
		return;
	}
	ESP_LOGI(TAG, "_meter_create: meter created OK, m=%p", (void *)m);

	lv_obj_set_size(m, (lv_coord_t)w->w, (lv_coord_t)w->h);
	lv_obj_set_align(m, LV_ALIGN_CENTER);
	lv_obj_set_pos(m, w->x, w->y);

	ESP_LOGI(TAG, "_meter_create: calling lv_meter_add_scale");
	lv_meter_scale_t *scale = lv_meter_add_scale(m);
	uint32_t angle_range =
		(360 + (md->end_angle % 360) - (md->start_angle % 360)) % 360;
	if (angle_range == 0 && md->start_angle != md->end_angle) {
		angle_range = 360;
	}
	ESP_LOGI(TAG, "_meter_create: angle_range=%u start=%d end=%d min=%d max=%d",
			 (unsigned)angle_range, (int)md->start_angle, (int)md->end_angle,
			 (int)md->min, (int)md->max);

	ESP_LOGI(TAG, "_meter_create: calling lv_meter_set_scale_range");
	lv_meter_set_scale_range(m, scale, md->min, md->max, angle_range,
							 (int32_t)md->start_angle);
	ESP_LOGI(TAG, "_meter_create: calling lv_meter_set_scale_ticks");
	lv_meter_set_scale_ticks(m, scale, 21, 2, 10,
							 lv_palette_main(LV_PALETTE_GREY));
	ESP_LOGI(TAG, "_meter_create: calling lv_meter_set_scale_major_ticks");
	lv_meter_set_scale_major_ticks(m, scale, 5, 4, 15, lv_color_white(), 10);

	ESP_LOGI(TAG, "_meter_create: calling lv_meter_add_needle_line");
	lv_meter_indicator_t *needle =
		lv_meter_add_needle_line(m, scale, 4, lv_color_white(), -10);

	ESP_LOGI(TAG, "_meter_create: calling lv_meter_set_indicator_value");
	md->meter = m;
	md->scale = scale;
	md->needle = needle;

	lv_meter_set_indicator_value(m, needle, md->min);

	/* Add digital value label in the center */
	md->value_label = lv_label_create(m);
	lv_label_set_text(md->value_label, "0");
	const lv_font_t *mtr_val_font = widget_resolve_font(md->value_font);
	lv_obj_set_style_text_font(md->value_label, mtr_val_font ? mtr_val_font : THEME_FONT_DASH_RPM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(md->value_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(md->value_label, LV_ALIGN_CENTER, 0, 20);

	/* Add ID label (e.g. "RPM") below the value */
	md->id_label = lv_label_create(m);
	lv_label_set_text(md->id_label, md->signal_name[0] != '\0' ? md->signal_name : w->id);
	const lv_font_t *mtr_lbl_font = widget_resolve_font(md->label_font);
	lv_obj_set_style_text_font(md->id_label, mtr_lbl_font ? mtr_lbl_font : THEME_FONT_SMALL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(md->id_label, THEME_COLOR_TEXT_MUTED,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(md->id_label, LV_ALIGN_CENTER, 0, 45);

	w->root = m;

	/* Subscribe to signal if bound */
	if (md->signal_index >= 0)
		signal_subscribe(md->signal_index, _meter_on_signal, w);

	ESP_LOGI(TAG, "_meter_create: DONE");
}

static void _meter_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, (lv_coord_t)nw, (lv_coord_t)nh);
	w->w = nw;
	w->h = nh;
}

static void _meter_open_settings(widget_t *w) { (void)w; }

static void _meter_to_json(widget_t *w, cJSON *out) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	widget_base_to_json(w, out);
	if (!md)
		return;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg)
		return;
	cJSON_AddNumberToObject(cfg, "slot", md->value_idx);
	cJSON_AddNumberToObject(cfg, "min", md->min);
	cJSON_AddNumberToObject(cfg, "max", md->max);
	cJSON_AddNumberToObject(cfg, "start_angle", md->start_angle);
	cJSON_AddNumberToObject(cfg, "end_angle", md->end_angle);
	if (md->label_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "label_font", md->label_font);
	if (md->value_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "value_font", md->value_font);
	if (md->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", md->signal_name);
}

static void _meter_from_json(widget_t *w, cJSON *in) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!md)
		return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg)
		return;
	cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	cJSON *min_item = cJSON_GetObjectItemCaseSensitive(cfg, "min");
	cJSON *max_item = cJSON_GetObjectItemCaseSensitive(cfg, "max");
	cJSON *sa_item = cJSON_GetObjectItemCaseSensitive(cfg, "start_angle");
	cJSON *ea_item = cJSON_GetObjectItemCaseSensitive(cfg, "end_angle");
	if (cJSON_IsNumber(slot_item)) {
		uint8_t idx = (uint8_t)slot_item->valueint;
		if (idx < 13)
			md->value_idx = idx;
	}
	if (cJSON_IsNumber(min_item))
		md->min = (int32_t)min_item->valueint;
	if (cJSON_IsNumber(max_item))
		md->max = (int32_t)max_item->valueint;
	if (cJSON_IsNumber(sa_item))
		md->start_angle = (int16_t)sa_item->valueint;
	if (cJSON_IsNumber(ea_item))
		md->end_angle = (int16_t)ea_item->valueint;
	cJSON *lf_item = cJSON_GetObjectItemCaseSensitive(cfg, "label_font");
	if (cJSON_IsString(lf_item) && lf_item->valuestring) {
		strncpy(md->label_font, lf_item->valuestring, sizeof(md->label_font) - 1);
		md->label_font[sizeof(md->label_font) - 1] = '\0';
	}
	cJSON *vf_item = cJSON_GetObjectItemCaseSensitive(cfg, "value_font");
	if (cJSON_IsString(vf_item) && vf_item->valuestring) {
		strncpy(md->value_font, vf_item->valuestring, sizeof(md->value_font) - 1);
		md->value_font[sizeof(md->value_font) - 1] = '\0';
	}
	cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(sig_item) && sig_item->valuestring) {
		strncpy(md->signal_name, sig_item->valuestring, sizeof(md->signal_name) - 1);
		md->signal_name[sizeof(md->signal_name) - 1] = '\0';
	}

	/* Resolve signal name → index */
	if (md->signal_name[0] != '\0')
		md->signal_index = signal_find_by_name(md->signal_name);
}

static void _meter_destroy(widget_t *w) {
	if (!w)
		return;
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	free(w->type_data);
	free(w);
}

uint8_t widget_meter_get_value_idx(const widget_t *w) {
	if (!w || w->type != WIDGET_METER || !w->type_data)
		return 0;
	const meter_data_t *md = (const meter_data_t *)w->type_data;
	return md->value_idx < 13 ? md->value_idx : 0;
}

widget_t *widget_meter_create_instance(uint8_t value_idx) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	meter_data_t *md = heap_caps_calloc(1, sizeof(meter_data_t), MALLOC_CAP_SPIRAM);
	if (!md) md = calloc(1, sizeof(meter_data_t));
	if (!md) {
		free(w);
		return NULL;
	}

	md->value_idx = (value_idx < 13) ? value_idx : 0;
	md->min = 0;
	md->max = 100;

	md->start_angle = 135;
	md->end_angle = 45;
	md->meter = NULL;
	md->scale = NULL;
	md->needle = NULL;
	md->signal_index = -1;

	w->type = WIDGET_METER;
	w->slot = md->value_idx;
	w->x = 0;
	w->y = 0;
	w->w = METER_DEFAULT_W;
	w->h = METER_DEFAULT_H;
	w->type_data = md;
	snprintf(w->id, sizeof(w->id), "meter_%u", md->value_idx);

	w->create = _meter_create;
	w->resize = _meter_resize;
	w->open_settings = _meter_open_settings;
	w->to_json = _meter_to_json;
	w->from_json = _meter_from_json;
	w->destroy = _meter_destroy;

	return w;
}
