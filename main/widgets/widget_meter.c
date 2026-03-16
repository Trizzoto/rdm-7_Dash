/*
 * widget_meter.c — Analog sweeping gauge for a value slot.
 *
 * Binds to a value slot (0–12). Receives raw int32_t via:
 *     w->update(w, &raw_value);
 * No double-precision math; only int32_t and clamping.
 */
#include "widget_meter.h"
#include "widget_rules.h"
#include "widget_image.h"
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
		return;
	}
	int32_t v = (int32_t)value;
	if (v < md->min) v = md->min;
	if (v > md->max) v = md->max;
	lv_meter_set_indicator_value(md->meter, md->needle, v);
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
	lv_obj_set_style_bg_color(m, md->meter_bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(m, md->meter_bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (md->border_width > 0) {
		lv_obj_set_style_border_color(m, md->border_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(m, md->border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(m, md->border_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	/* Scale padding — always set to override theme default.
	 * 0 = ticks flush with edge, positive = ticks pushed inward */
	lv_obj_set_style_pad_top(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Background image */
	if (md->bg_image_name[0] != '\0') {
		md->bg_img_dsc = rdm_image_load(md->bg_image_name);
		if (md->bg_img_dsc) {
			lv_obj_set_style_bg_img_src(m, md->bg_img_dsc, LV_PART_MAIN | LV_STATE_DEFAULT);
			ESP_LOGI(TAG, "Meter background image '%s' loaded", md->bg_image_name);
		}
	}

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
	lv_meter_set_scale_ticks(m, scale, md->minor_tick_count, md->minor_tick_width,
							 md->minor_tick_length, md->minor_tick_color);
	ESP_LOGI(TAG, "_meter_create: calling lv_meter_set_scale_major_ticks");
	lv_meter_set_scale_major_ticks(m, scale, md->major_tick_every, md->major_tick_width,
								   md->major_tick_length, md->major_tick_color, md->label_gap);

	/* Tick label font */
	if (md->tick_label_font[0] != '\0') {
		const lv_font_t *tfont = widget_resolve_font(md->tick_label_font);
		if (tfont) {
			lv_obj_set_style_text_font(m, tfont, LV_PART_TICKS);
		}
	}

	/* Needle: use image if configured, otherwise line.
	 * When needle_angle_offset != 0 AND using an image needle, create a
	 * second scale with the rotated origin so the needle sweeps offset
	 * from the tick marks. */
	lv_meter_indicator_t *needle;
	lv_meter_scale_t *needle_target_scale = scale; /* default: same scale as ticks */

	if (md->needle_image_name[0] != '\0') {
		md->needle_img_dsc = rdm_image_load(md->needle_image_name);
		if (md->needle_img_dsc) {
			/* Create separate needle scale if angle offset is set */
			if (md->needle_angle_offset != 0) {
				lv_meter_scale_t *ns = lv_meter_add_scale(m);
				lv_meter_set_scale_range(m, ns, md->min, md->max, angle_range,
										 (int32_t)(md->start_angle + md->needle_angle_offset));
				lv_meter_set_scale_ticks(m, ns, 0, 0, 0, lv_color_black());
				md->needle_scale = ns;
				needle_target_scale = ns;
				ESP_LOGI(TAG, "_meter_create: needle scale offset=%d", md->needle_angle_offset);
			}
			ESP_LOGI(TAG, "_meter_create: using needle image '%s' pivot(%d,%d)",
					 md->needle_image_name, md->needle_pivot_x, md->needle_pivot_y);
			needle = lv_meter_add_needle_img(m, needle_target_scale, md->needle_img_dsc,
											  md->needle_pivot_x, md->needle_pivot_y);
		} else {
			ESP_LOGW(TAG, "Needle image '%s' failed to load, falling back to line", md->needle_image_name);
			needle = lv_meter_add_needle_line(m, scale, md->needle_width, md->needle_color, md->needle_r_mod);
		}
	} else {
		ESP_LOGI(TAG, "_meter_create: using line needle");
		needle = lv_meter_add_needle_line(m, scale, md->needle_width, md->needle_color, md->needle_r_mod);
	}

	ESP_LOGI(TAG, "_meter_create: calling lv_meter_set_indicator_value");
	md->meter = m;
	md->scale = scale;
	md->needle = needle;

	lv_meter_set_indicator_value(m, needle, md->min);

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
	if (md->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", md->signal_name);

	/* Appearance overrides — only serialize non-default values */
	if (md->minor_tick_count != 21)
		cJSON_AddNumberToObject(cfg, "minor_tick_count", md->minor_tick_count);
	if (md->major_tick_every != 5)
		cJSON_AddNumberToObject(cfg, "major_tick_every", md->major_tick_every);
	if (md->minor_tick_width != 2)
		cJSON_AddNumberToObject(cfg, "minor_tick_width", md->minor_tick_width);
	if (md->minor_tick_length != 10)
		cJSON_AddNumberToObject(cfg, "minor_tick_length", md->minor_tick_length);
	if (md->major_tick_width != 4)
		cJSON_AddNumberToObject(cfg, "major_tick_width", md->major_tick_width);
	if (md->major_tick_length != 15)
		cJSON_AddNumberToObject(cfg, "major_tick_length", md->major_tick_length);
	if (md->minor_tick_color.full != lv_palette_main(LV_PALETTE_GREY).full)
		cJSON_AddNumberToObject(cfg, "minor_tick_color", (int)md->minor_tick_color.full);
	if (md->major_tick_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "major_tick_color", (int)md->major_tick_color.full);
	if (md->needle_width != 4)
		cJSON_AddNumberToObject(cfg, "needle_width", md->needle_width);
	if (md->needle_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "needle_color", (int)md->needle_color.full);
	if (md->needle_r_mod != -10)
		cJSON_AddNumberToObject(cfg, "needle_r_mod", md->needle_r_mod);
	if (md->needle_image_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "needle_image_name", md->needle_image_name);
	if (md->needle_pivot_x != 0)
		cJSON_AddNumberToObject(cfg, "needle_pivot_x", md->needle_pivot_x);
	if (md->needle_pivot_y != 0)
		cJSON_AddNumberToObject(cfg, "needle_pivot_y", md->needle_pivot_y);
	if (md->needle_angle_offset != 0)
		cJSON_AddNumberToObject(cfg, "needle_angle_offset", md->needle_angle_offset);
	if (md->bg_image_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "bg_image_name", md->bg_image_name);
	if (md->border_width != 0)
		cJSON_AddNumberToObject(cfg, "border_width", md->border_width);
	if (md->border_color.full != lv_color_black().full)
		cJSON_AddNumberToObject(cfg, "border_color", (int)md->border_color.full);
	if (md->border_opa != 255)
		cJSON_AddNumberToObject(cfg, "border_opa", md->border_opa);
	if (md->meter_bg_color.full != lv_color_hex(0x3D3D3D).full)
		cJSON_AddNumberToObject(cfg, "meter_bg_color", (int)md->meter_bg_color.full);
	if (md->meter_bg_opa != 255)
		cJSON_AddNumberToObject(cfg, "meter_bg_opa", md->meter_bg_opa);
	if (md->scale_padding != 0)
		cJSON_AddNumberToObject(cfg, "scale_padding", md->scale_padding);
	if (md->label_gap != 10)
		cJSON_AddNumberToObject(cfg, "label_gap", md->label_gap);
	if (md->tick_label_font[0] != '\0')
		cJSON_AddStringToObject(cfg, "tick_label_font", md->tick_label_font);
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
	cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(sig_item) && sig_item->valuestring) {
		safe_strncpy(md->signal_name, sig_item->valuestring, sizeof(md->signal_name));
	}

	/* Resolve signal name → index */
	if (md->signal_name[0] != '\0')
		md->signal_index = signal_find_by_name(md->signal_name);

	/* Appearance overrides */
	cJSON *ap;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_count");
	if (cJSON_IsNumber(ap)) md->minor_tick_count = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_every");
	if (cJSON_IsNumber(ap)) md->major_tick_every = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_width");
	if (cJSON_IsNumber(ap)) md->minor_tick_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_length");
	if (cJSON_IsNumber(ap)) md->minor_tick_length = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_width");
	if (cJSON_IsNumber(ap)) md->major_tick_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_length");
	if (cJSON_IsNumber(ap)) md->major_tick_length = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_color");
	if (cJSON_IsNumber(ap)) md->minor_tick_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_color");
	if (cJSON_IsNumber(ap)) md->major_tick_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_width");
	if (cJSON_IsNumber(ap)) md->needle_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_color");
	if (cJSON_IsNumber(ap)) md->needle_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_r_mod");
	if (cJSON_IsNumber(ap)) md->needle_r_mod = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_image_name");
	if (cJSON_IsString(ap) && ap->valuestring) {
		safe_strncpy(md->needle_image_name, ap->valuestring, sizeof(md->needle_image_name));
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_pivot_x");
	if (cJSON_IsNumber(ap)) md->needle_pivot_x = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_pivot_y");
	if (cJSON_IsNumber(ap)) md->needle_pivot_y = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_angle_offset");
	if (cJSON_IsNumber(ap)) md->needle_angle_offset = (int16_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "bg_image_name");
	if (cJSON_IsString(ap) && ap->valuestring) {
		safe_strncpy(md->bg_image_name, ap->valuestring, sizeof(md->bg_image_name));
	}
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "border_width");
	if (cJSON_IsNumber(ap)) md->border_width = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "border_color");
	if (cJSON_IsNumber(ap)) md->border_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "border_opa");
	if (cJSON_IsNumber(ap)) md->border_opa = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "meter_bg_color");
	if (cJSON_IsNumber(ap)) md->meter_bg_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "meter_bg_opa");
	if (cJSON_IsNumber(ap)) md->meter_bg_opa = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "scale_padding");
	if (cJSON_IsNumber(ap)) md->scale_padding = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "label_gap");
	if (cJSON_IsNumber(ap)) md->label_gap = (int8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_font");
	if (cJSON_IsString(ap) && ap->valuestring) {
		safe_strncpy(md->tick_label_font, ap->valuestring, sizeof(md->tick_label_font));
	}
}

static void _meter_destroy(widget_t *w) {
	if (!w)
		return;
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (md) {
		rdm_image_free(md->needle_img_dsc);
		rdm_image_free(md->bg_img_dsc);
		free(md);
	}
	free(w);
}

uint8_t widget_meter_get_value_idx(const widget_t *w) {
	if (!w || w->type != WIDGET_METER || !w->type_data)
		return 0;
	const meter_data_t *md = (const meter_data_t *)w->type_data;
	return md->value_idx < 13 ? md->value_idx : 0;
}

/* ── apply_overrides: live style changes driven by conditional rules ───── */

static void _meter_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md) return;
	lv_obj_t *m = md->meter;
	if (!m) return;

	/* Start from base meter_data_t values (restore defaults) */
	lv_color_t bg_color = md->meter_bg_color;
	uint8_t bg_opa = md->meter_bg_opa;
	lv_color_t bdr_color = md->border_color;
	lv_coord_t bdr_width = (lv_coord_t)md->border_width;
	uint8_t bdr_opa = md->border_opa;
	lv_color_t ndl_color = md->needle_color;
	lv_color_t minor_tick_c = md->minor_tick_color;
	lv_color_t major_tick_c = md->major_tick_color;
	bool ticks_changed = false;

	/* Apply active overrides on top */
	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "meter_bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bg_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "meter_bg_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bg_opa = (uint8_t)o->value.num;
		} else if (strcmp(o->field_name, "border_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bdr_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "border_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bdr_width = (lv_coord_t)o->value.num;
		} else if (strcmp(o->field_name, "border_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
			bdr_opa = (uint8_t)o->value.num;
		} else if (strcmp(o->field_name, "needle_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			ndl_color.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "minor_tick_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			minor_tick_c.full = (uint16_t)o->value.color;
			ticks_changed = true;
		} else if (strcmp(o->field_name, "major_tick_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			major_tick_c.full = (uint16_t)o->value.color;
			ticks_changed = true;
		}
	}

	/* Apply all styles (either overridden or restored to base) */
	lv_obj_set_style_bg_color(m, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(m, bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(m, bdr_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(m, bdr_width, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(m, bdr_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_line_color(m, ndl_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	/* Re-apply tick colors via scale API (not style-based in LVGL v8) */
	if (ticks_changed || count == 0) {
		if (md->scale) {
			lv_meter_set_scale_ticks(m, md->scale, md->minor_tick_count,
				md->minor_tick_width, md->minor_tick_length, minor_tick_c);
			lv_meter_set_scale_major_ticks(m, md->scale, md->major_tick_every,
				md->major_tick_width, md->major_tick_length, major_tick_c, md->label_gap);
			lv_obj_invalidate(m);
		}
	}
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

	/* Tick defaults */
	md->minor_tick_count = 21;
	md->major_tick_every = 5;
	md->minor_tick_width = 2;
	md->minor_tick_length = 10;
	md->major_tick_width = 4;
	md->major_tick_length = 15;
	md->minor_tick_color = lv_palette_main(LV_PALETTE_GREY);
	md->major_tick_color = lv_color_white();
	/* Needle defaults */
	md->needle_width = 4;
	md->needle_color = lv_color_white();
	md->needle_r_mod = -10;
	/* Border defaults */
	md->border_color = lv_color_black();
	md->border_width = 0;
	md->border_opa = 255;
	/* Background defaults */
	md->meter_bg_color = lv_color_hex(0x3D3D3D);
	md->meter_bg_opa = 255;
	/* Scale layout defaults */
	md->scale_padding = 0;
	md->label_gap = 10;

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
	w->apply_overrides = _meter_apply_overrides;

	return w;
}
