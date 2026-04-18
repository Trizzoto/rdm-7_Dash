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
	int32_t v;
	if (is_stale) {
		v = md->min;
	} else {
		v = (int32_t)value;
		if (v < md->min) v = md->min;
		if (v > md->max) v = md->max;
	}
	lv_meter_set_indicator_value(md->meter, md->needle, v);
	/* Drive the night meter in lock-step so the swap looks continuous. */
	if (md->night_meter && md->night_needle && lv_obj_is_valid(md->night_meter)) {
		lv_meter_set_indicator_value(md->night_meter, md->night_needle, v);
	}
}

/* Forward declarations — used by _meter_create / _meter_destroy below. */
static void _meter_apply_night_mode(widget_t *w, bool active);
static void _meter_night_cb(bool active, void *user_data);

/* True when at least one night-mode override touches a property baked in at
 * creation time (LVGL v8 can't mutate these live). When true, _meter_create
 * builds a sibling "night meter" with the night values pre-baked, and
 * apply_night_mode toggles visibility instead of mutating styles. */
static inline bool _meter_needs_night_meter(const meter_data_t *md) {
	return md->night.has_minor_tick_color ||
	       md->night.has_major_tick_color ||
	       md->night.has_needle_color     ||
	       md->night.has_needle_image_name ||
	       md->night.has_bg_image_name;
}

/* Build a single meter (day or night). When `use_night` is true, any field
 * with a corresponding night override picks the night value; otherwise the
 * day value is used. The output pointers (`*out_meter`, `*out_scale`,
 * `*out_needle`, `*out_needle_scale`) receive the created LVGL handles.
 * `*out_needle_img_dsc` and `*out_bg_img_dsc` receive any loaded image
 * descriptors so the caller can free them on destroy. */
static void _meter_build_one(meter_data_t *md, lv_obj_t *parent, bool use_night,
                             lv_obj_t **out_meter,
                             lv_meter_scale_t **out_scale,
                             lv_meter_indicator_t **out_needle,
                             lv_meter_scale_t **out_needle_scale,
                             lv_img_dsc_t **out_needle_img_dsc,
                             lv_img_dsc_t **out_bg_img_dsc) {
	/* Pick effective values based on day/night */
	lv_color_t bg_color    = use_night ? NIGHT_PICK_COLOR(true, md->night, meter_bg_color,    md->meter_bg_color)    : md->meter_bg_color;
	lv_color_t bdr_color   = use_night ? NIGHT_PICK_COLOR(true, md->night, border_color,      md->border_color)      : md->border_color;
	lv_color_t mintc       = use_night ? NIGHT_PICK_COLOR(true, md->night, minor_tick_color,  md->minor_tick_color)  : md->minor_tick_color;
	lv_color_t majtc       = use_night ? NIGHT_PICK_COLOR(true, md->night, major_tick_color,  md->major_tick_color)  : md->major_tick_color;
	lv_color_t needle_c    = use_night ? NIGHT_PICK_COLOR(true, md->night, needle_color,      md->needle_color)      : md->needle_color;
	lv_color_t ball_c      = use_night ? NIGHT_PICK_COLOR(true, md->night, needle_ball_color, md->needle_ball_color) : md->needle_ball_color;
	lv_color_t tlc         = use_night ? NIGHT_PICK_COLOR(true, md->night, tick_label_color,  md->tick_label_color)  : md->tick_label_color;
	const char *needle_img = (use_night && md->night.has_needle_image_name) ? md->night.needle_image_name : md->needle_image_name;
	const char *bg_img     = (use_night && md->night.has_bg_image_name)     ? md->night.bg_image_name     : md->bg_image_name;

	lv_obj_t *m = lv_meter_create(parent);
	if (!m) { *out_meter = NULL; return; }

	/* Placeholder size — caller sets the real w/h after we return. */
	lv_obj_set_size(m, METER_DEFAULT_W, METER_DEFAULT_H);
	lv_obj_set_style_bg_color(m, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(m, md->meter_bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(m, md->border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (md->border_width > 0) {
		lv_obj_set_style_border_color(m, bdr_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(m, md->border_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	lv_obj_set_style_pad_top(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(m, md->scale_padding, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Background image */
	if (bg_img && bg_img[0] != '\0') {
		lv_img_dsc_t *bgdsc = rdm_image_load(bg_img);
		if (bgdsc) {
			lv_obj_set_style_bg_img_src(m, bgdsc, LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		*out_bg_img_dsc = bgdsc;
	}

	lv_meter_scale_t *scale = lv_meter_add_scale(m);
	uint32_t angle_range = (360 + (md->end_angle % 360) - (md->start_angle % 360)) % 360;
	if (angle_range == 0 && md->start_angle != md->end_angle) angle_range = 360;
	lv_meter_set_scale_range(m, scale, md->min, md->max, angle_range, (int32_t)md->start_angle);
	uint8_t mtc = md->minor_tick_count < 2 ? 2 : md->minor_tick_count;
	uint8_t mte = md->major_tick_every < 1 ? 1 : md->major_tick_every;
	lv_meter_set_scale_ticks(m, scale, mtc, md->minor_tick_width, md->minor_tick_length, mintc);
	lv_meter_set_scale_major_ticks(m, scale, mte, md->major_tick_width, md->major_tick_length, majtc, md->label_gap);

	/* Tick label color/font */
	lv_obj_set_style_text_color(m, tlc, LV_PART_TICKS);
	if (md->tick_label_font[0] != '\0') {
		const lv_font_t *tfont = widget_resolve_font(md->tick_label_font);
		if (tfont) lv_obj_set_style_text_font(m, tfont, LV_PART_TICKS);
	}
	if (!md->show_tick_labels) {
		lv_obj_set_style_text_opa(m, LV_OPA_TRANSP, LV_PART_TICKS);
	}

	/* Needle */
	lv_meter_indicator_t *needle;
	lv_meter_scale_t *needle_target_scale = scale;
	*out_needle_scale = NULL;
	if (needle_img && needle_img[0] != '\0') {
		lv_img_dsc_t *ndsc = rdm_image_load(needle_img);
		if (ndsc) {
			if (md->needle_angle_offset != 0) {
				lv_meter_scale_t *ns = lv_meter_add_scale(m);
				lv_meter_set_scale_range(m, ns, md->min, md->max, angle_range,
				                         (int32_t)(md->start_angle + md->needle_angle_offset));
				lv_meter_set_scale_ticks(m, ns, 0, 0, 0, lv_color_black());
				*out_needle_scale = ns;
				needle_target_scale = ns;
			}
			needle = lv_meter_add_needle_img(m, needle_target_scale, ndsc,
			                                  md->needle_pivot_x, md->needle_pivot_y);
			*out_needle_img_dsc = ndsc;
		} else {
			needle = lv_meter_add_needle_line(m, scale, md->needle_width, needle_c, md->needle_r_mod);
		}
	} else {
		needle = lv_meter_add_needle_line(m, scale, md->needle_width, needle_c, md->needle_r_mod);
	}

	/* Needle center ball */
	if (md->needle_ball_size == 0) {
		lv_obj_set_style_size(m, 0, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_INDICATOR);
	} else {
		lv_obj_set_style_size(m, md->needle_ball_size, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(m, ball_c, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_INDICATOR);
	}

	lv_meter_set_indicator_value(m, needle, md->min);

	*out_meter = m;
	*out_scale = scale;
	*out_needle = needle;
}

static void _meter_create(widget_t *w, lv_obj_t *parent) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md) {
		ESP_LOGE(TAG, "_meter_create: missing meter_data");
		return;
	}

	bool needs_night = _meter_needs_night_meter(md);

	/* When a night meter is needed, wrap both meters in a transparent
	 * container so long-press / click events have a stable hit target
	 * regardless of which meter is currently visible. Otherwise the day
	 * meter itself is the root (preserves existing behavior for layouts
	 * with no night override or only color-mutable night overrides). */
	lv_obj_t *meter_parent;
	lv_obj_t *cont = NULL;
	if (needs_night) {
		cont = lv_obj_create(parent);
		lv_obj_set_size(cont, (lv_coord_t)w->w, (lv_coord_t)w->h);
		lv_obj_set_align(cont, LV_ALIGN_CENTER);
		lv_obj_set_pos(cont, w->x, w->y);
		lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		meter_parent = cont;
	} else {
		meter_parent = parent;
	}

	/* Build day meter */
	lv_obj_t *m = NULL;
	lv_meter_scale_t *scale = NULL;
	lv_meter_indicator_t *needle = NULL;
	lv_meter_scale_t *needle_scale = NULL;
	_meter_build_one(md, meter_parent, false, &m, &scale, &needle, &needle_scale,
	                 &md->needle_img_dsc, &md->bg_img_dsc);
	if (!m) {
		ESP_LOGE(TAG, "_meter_create: lv_meter_create failed");
		if (cont) lv_obj_del(cont);
		return;
	}
	lv_obj_set_size(m, (lv_coord_t)w->w, (lv_coord_t)w->h);
	if (!cont) {
		lv_obj_set_align(m, LV_ALIGN_CENTER);
		lv_obj_set_pos(m, w->x, w->y);
	} else {
		lv_obj_set_align(m, LV_ALIGN_CENTER);
	}
	md->meter = m;
	md->scale = scale;
	md->needle = needle;
	md->needle_scale = needle_scale;

	/* Build night meter as sibling, hidden by default */
	if (needs_night) {
		lv_obj_t *nm = NULL;
		lv_meter_scale_t *nscale = NULL;
		lv_meter_indicator_t *nneedle = NULL;
		lv_meter_scale_t *nneedle_scale = NULL;
		_meter_build_one(md, meter_parent, true, &nm, &nscale, &nneedle, &nneedle_scale,
		                 &md->night_needle_img_dsc, &md->night_bg_img_dsc);
		if (nm) {
			lv_obj_set_size(nm, (lv_coord_t)w->w, (lv_coord_t)w->h);
			lv_obj_set_align(nm, LV_ALIGN_CENTER);
			lv_obj_add_flag(nm, LV_OBJ_FLAG_HIDDEN);
			md->night_meter = nm;
			md->night_scale = nscale;
			md->night_needle = nneedle;
			md->night_needle_scale = nneedle_scale;
		}
	}

	w->root = cont ? cont : m;

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

	ESP_LOGD(TAG, "_meter_create: DONE (night_meter=%p)", (void *)md->night_meter);
}

static void _meter_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, (lv_coord_t)nw, (lv_coord_t)nh);
	/* If root is the container, also resize the day meter (child of container).
	 * If root *is* the day meter, the size set above already covered it. */
	if (md && md->meter && lv_obj_is_valid(md->meter) && md->meter != w->root)
		lv_obj_set_size(md->meter, (lv_coord_t)nw, (lv_coord_t)nh);
	if (md && md->night_meter && lv_obj_is_valid(md->night_meter))
		lv_obj_set_size(md->night_meter, (lv_coord_t)nw, (lv_coord_t)nh);
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
	/* Deleting w->root cascades to children (container case kills both day +
	 * night meters). If root is the day meter directly, no night meter exists
	 * (we only wrap when needs_night is true). */
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	if (md) {
		rdm_image_free(md->needle_img_dsc);
		rdm_image_free(md->bg_img_dsc);
		rdm_image_free(md->night_needle_img_dsc);
		rdm_image_free(md->night_bg_img_dsc);
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

	/* Apply to whichever meter is currently visible (day or night). When a
	 * night meter exists and is visible, conditional-rule overrides should
	 * affect it, not the hidden day meter. */
	lv_obj_t *target = md->meter;
	if (md->night_meter && lv_obj_is_valid(md->night_meter) &&
	    !lv_obj_has_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN)) {
		target = md->night_meter;
	}
	lv_obj_set_style_bg_color(target, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(target, nbc, LV_PART_INDICATOR);
	if (md->border_width > 0)
		lv_obj_set_style_border_color(target, bc, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Needle color can only be applied to line needles (not image needles).
	 * LVGL v8 doesn't expose a direct API for line needle color after creation,
	 * so we store it for potential future use. */
	(void)nc;
}

/* Apply night-mode state. Two paths:
 *   A) night_meter exists: a sibling meter was built with all night values
 *      baked in (tick colors, needle color, needle/bg image). We just toggle
 *      visibility — instant swap, no live mutation needed.
 *   B) no night_meter: only runtime-mutable color overrides are configured
 *      (bg/border/ball/tick label). Mutate them on the day meter directly. */
static void _meter_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	meter_data_t *md = (meter_data_t *)w->type_data;
	if (!md || !md->meter) return;

	bool has_night_obj = md->night_meter && lv_obj_is_valid(md->night_meter);

	if (has_night_obj) {
		/* Path A: visibility swap. Both meters already have correct colors
		 * baked in for their respective state, so no live mutation needed. */
		if (active) {
			lv_obj_add_flag(md->meter, LV_OBJ_FLAG_HIDDEN);
			lv_obj_clear_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_clear_flag(md->meter, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(md->night_meter, LV_OBJ_FLAG_HIDDEN);
		}
		return;
	}

	/* Path B: live mutation on the day meter (no baked night overrides). */
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
