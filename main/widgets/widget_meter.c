/*
 * widget_meter.c — Analog sweeping gauge for a value slot.
 *
 * Binds to a value slot (0–12). Receives raw int32_t via:
 *     w->update(w, &raw_value);
 * No double-precision math; only int32_t and clamping.
 */
#include "widget_meter.h"
#include "widget_image.h"
#include "widget_rules.h"
#include "system/night_mode.h"
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
	if (!md->meter || !md->needle) return;
	if (is_stale) {
		lv_meter_set_indicator_value(md->meter, md->needle, md->min);
		return;
	}
	int32_t v = (int32_t)value;
	if (v < md->min) v = md->min;
	if (v > md->max) v = md->max;
	lv_meter_set_indicator_value(md->meter, md->needle, v);
}

/* Forward declarations — used by _meter_create / _meter_destroy below. */
static void _meter_apply_night_mode(widget_t *w, bool active);
static void _meter_night_cb(bool active, void *user_data);

static void _meter_create(widget_t *w, lv_obj_t *parent) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md) {
		ESP_LOGE(TAG, "_meter_create: missing meter_data");
		return;
	}

	ESP_LOGD(TAG, "_meter_create: calling lv_meter_create, parent=%p",
			 (void *)parent);
	lv_obj_t *m = lv_meter_create(parent);
	if (!m) {
		ESP_LOGE(TAG, "_meter_create: lv_meter_create failed");
		return;
	}
	ESP_LOGD(TAG, "_meter_create: meter created OK, m=%p", (void *)m);

	lv_obj_set_size(m, (lv_coord_t)w->w, (lv_coord_t)w->h);
	lv_obj_set_align(m, LV_ALIGN_CENTER);
	lv_obj_set_pos(m, w->x, w->y);
	lv_obj_set_style_bg_color(m, md->meter_bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(m, md->meter_bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Always set border explicitly to override theme defaults */
	lv_obj_set_style_border_width(m, md->border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (md->border_width > 0) {
		lv_obj_set_style_border_color(m, md->border_color, LV_PART_MAIN | LV_STATE_DEFAULT);
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
			ESP_LOGD(TAG, "Meter background image '%s' loaded", md->bg_image_name);
		}
	}

	ESP_LOGD(TAG, "_meter_create: calling lv_meter_add_scale");
	lv_meter_scale_t *scale = lv_meter_add_scale(m);
	uint32_t angle_range =
		(360 + (md->end_angle % 360) - (md->start_angle % 360)) % 360;
	if (angle_range == 0 && md->start_angle != md->end_angle) {
		angle_range = 360;
	}
	ESP_LOGD(TAG, "_meter_create: angle_range=%u start=%d end=%d min=%d max=%d",
			 (unsigned)angle_range, (int)md->start_angle, (int)md->end_angle,
			 (int)md->min, (int)md->max);

	ESP_LOGD(TAG, "_meter_create: calling lv_meter_set_scale_range");
	lv_meter_set_scale_range(m, scale, md->min, md->max, angle_range,
							 (int32_t)md->start_angle);
	ESP_LOGD(TAG, "_meter_create: calling lv_meter_set_scale_ticks");
	if (md->minor_tick_count < 2) md->minor_tick_count = 2;
	if (md->major_tick_every < 1) md->major_tick_every = 1;
	lv_meter_set_scale_ticks(m, scale, md->minor_tick_count, md->minor_tick_width,
							 md->minor_tick_length, md->minor_tick_color);
	ESP_LOGD(TAG, "_meter_create: calling lv_meter_set_scale_major_ticks");
	lv_meter_set_scale_major_ticks(m, scale, md->major_tick_every, md->major_tick_width,
								   md->major_tick_length, md->major_tick_color, md->label_gap);

	/* Tick label font and color — always set color to override theme defaults */
	lv_obj_set_style_text_color(m, md->tick_label_color, LV_PART_TICKS);
	if (md->tick_label_font[0] != '\0') {
		const lv_font_t *tfont = widget_resolve_font(md->tick_label_font);
		if (tfont) {
			lv_obj_set_style_text_font(m, tfont, LV_PART_TICKS);
		}
	}
	/* Hide tick labels if disabled */
	if (!md->show_tick_labels) {
		lv_obj_set_style_text_opa(m, LV_OPA_TRANSP, LV_PART_TICKS);
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
				ESP_LOGD(TAG, "_meter_create: needle scale offset=%d", md->needle_angle_offset);
			}
			ESP_LOGD(TAG, "_meter_create: using needle image '%s' pivot(%d,%d)",
					 md->needle_image_name, md->needle_pivot_x, md->needle_pivot_y);
			needle = lv_meter_add_needle_img(m, needle_target_scale, md->needle_img_dsc,
											  md->needle_pivot_x, md->needle_pivot_y);
		} else {
			ESP_LOGW(TAG, "Needle image '%s' failed to load, falling back to line", md->needle_image_name);
			needle = lv_meter_add_needle_line(m, scale, md->needle_width, md->needle_color, md->needle_r_mod);
		}
	} else {
		ESP_LOGD(TAG, "_meter_create: using line needle");
		needle = lv_meter_add_needle_line(m, scale, md->needle_width, md->needle_color, md->needle_r_mod);
	}

	/* Needle center ball styling */
	if (md->needle_ball_size == 0) {
		lv_obj_set_style_size(m, 0, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_INDICATOR);
	} else {
		lv_obj_set_style_size(m, md->needle_ball_size, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(m, md->needle_ball_color, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_INDICATOR);
	}

	ESP_LOGD(TAG, "_meter_create: calling lv_meter_set_indicator_value");
	md->meter = m;
	md->scale = scale;
	md->needle = needle;

	lv_meter_set_indicator_value(m, needle, md->min);

	w->root = m;

	/* Subscribe to signal if bound */
	if (md->signal_index >= 0)
		signal_subscribe(md->signal_index, _meter_on_signal, w);

	/* Subscribe rules (safe no-op if no rules defined) */
	widget_rules_subscribe(w);

	/* Subscribe to night-mode changes if any night override is set. */
	if (md->night.has_minor_tick_color  || md->night.has_major_tick_color ||
	    md->night.has_needle_color      || md->night.has_needle_ball_color ||
	    md->night.has_border_color      || md->night.has_meter_bg_color ||
	    md->night.has_tick_label_color  || md->night.has_needle_image_name ||
	    md->night.has_bg_image_name) {
		night_mode_subscribe(_meter_night_cb, w);
		_meter_apply_night_mode(w, night_mode_is_active());
	}

	ESP_LOGD(TAG, "_meter_create: DONE");
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
	if (md->needle_ball_size != 10)
		cJSON_AddNumberToObject(cfg, "needle_ball_size", md->needle_ball_size);
	if (md->needle_ball_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "needle_ball_color", (int)md->needle_ball_color.full);
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
	if (md->tick_label_color.full != lv_color_white().full)
		cJSON_AddNumberToObject(cfg, "tick_label_color", (int)md->tick_label_color.full);
	if (!md->show_tick_labels)
		cJSON_AddBoolToObject(cfg, "show_tick_labels", false);

	/* Rules */
	widget_rules_to_json(w, cfg);

	/* Night-mode overrides — emit only fields that have an override set */
	{
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, md->night, minor_tick_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, major_tick_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, needle_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, needle_ball_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, border_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, meter_bg_color);
		NIGHT_SERIALIZE_COLOR(n, md->night, tick_label_color);
		NIGHT_SERIALIZE_IMAGE(n, md->night, needle_image_name);
		NIGHT_SERIALIZE_IMAGE(n, md->night, bg_image_name);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
	}
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
	if (md->minor_tick_count < 2) md->minor_tick_count = 2;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_every");
	if (cJSON_IsNumber(ap)) md->major_tick_every = (uint8_t)ap->valueint;
	if (md->major_tick_every < 1) md->major_tick_every = 1;
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
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_ball_size");
	if (cJSON_IsNumber(ap)) md->needle_ball_size = (uint8_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "needle_ball_color");
	if (cJSON_IsNumber(ap)) md->needle_ball_color.full = (uint32_t)ap->valueint;
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
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_color");
	if (cJSON_IsNumber(ap)) md->tick_label_color.full = (uint32_t)ap->valueint;
	ap = cJSON_GetObjectItemCaseSensitive(cfg, "show_tick_labels");
	if (cJSON_IsBool(ap)) md->show_tick_labels = cJSON_IsTrue(ap);

	/* Rules */
	widget_rules_from_json(w, cfg);

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, md->night, minor_tick_color);
		NIGHT_PARSE_COLOR(night, md->night, major_tick_color);
		NIGHT_PARSE_COLOR(night, md->night, needle_color);
		NIGHT_PARSE_COLOR(night, md->night, needle_ball_color);
		NIGHT_PARSE_COLOR(night, md->night, border_color);
		NIGHT_PARSE_COLOR(night, md->night, meter_bg_color);
		NIGHT_PARSE_COLOR(night, md->night, tick_label_color);
		NIGHT_PARSE_IMAGE(night, md->night, needle_image_name);
		NIGHT_PARSE_IMAGE(night, md->night, bg_image_name);
	}
}

static void _meter_destroy(widget_t *w) {
	if (!w)
		return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (md && md->signal_index >= 0)
		signal_unsubscribe(md->signal_index, _meter_on_signal, w);
	night_mode_unsubscribe(_meter_night_cb, w);
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
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

static void _meter_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !md->meter) return;

	lv_color_t bg = md->meter_bg_color;
	lv_color_t nc = md->needle_color;
	lv_color_t nbc = md->needle_ball_color;
	lv_color_t bc = md->border_color;

	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "meter_bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bg.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "needle_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			nc.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "needle_ball_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			nbc.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "border_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bc.full = (uint16_t)o->value.color;
		}
	}

	lv_obj_set_style_bg_color(md->meter, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(md->meter, nbc, LV_PART_INDICATOR);
	if (md->border_width > 0)
		lv_obj_set_style_border_color(md->meter, bc, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Needle color can only be applied to line needles (not image needles).
	 * LVGL v8 doesn't expose a direct API for line needle color after creation,
	 * so we store it for potential future use. */
	(void)nc;
}

/* Re-apply colors based on current night-mode state. Only runtime-mutable
 * style properties are updated here — tick colors and line needle color are
 * baked in at lv_meter_set_scale_ticks() / lv_meter_add_needle_line() in
 * LVGL v8 and cannot be changed without re-creating the scale. Likewise for
 * image swaps. TODO: full re-create path for tick + needle + image swap. */
static void _meter_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !md->meter) return;

	lv_color_t bg   = NIGHT_PICK_COLOR(active, md->night, meter_bg_color,   md->meter_bg_color);
	lv_color_t bdr  = NIGHT_PICK_COLOR(active, md->night, border_color,     md->border_color);
	lv_color_t nbc  = NIGHT_PICK_COLOR(active, md->night, needle_ball_color, md->needle_ball_color);
	lv_color_t tlc  = NIGHT_PICK_COLOR(active, md->night, tick_label_color, md->tick_label_color);

	lv_obj_set_style_bg_color(md->meter, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (md->border_width > 0) {
		lv_obj_set_style_border_color(md->meter, bdr,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (md->needle_ball_size > 0) {
		lv_obj_set_style_bg_color(md->meter, nbc, LV_PART_INDICATOR);
	}
	lv_obj_set_style_text_color(md->meter, tlc, LV_PART_TICKS);
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _meter_night_cb(bool active, void *user_data) {
	_meter_apply_night_mode((widget_t *)user_data, active);
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
	md->needle_ball_size = 10;
	md->needle_ball_color = lv_color_white();
	/* Tick label defaults */
	md->tick_label_color = lv_color_white();
	md->show_tick_labels = true;
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
	w->apply_night_mode = _meter_apply_night_mode;

	return w;
}
