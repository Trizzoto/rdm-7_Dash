/*
 * widget_text.c — Text widget displaying live CAN value strings.
 *
 * Binds to a value slot (0-12). Receives text_update_t via w->update(w, data)
 * from the CAN dispatch loop. Uses the pre-formatted value_str directly;
 * no heap allocation, no global string arrays.
 */
#include "widget_text.h"
#include "widget_rules.h"
#include "widget_types.h"
#include "ui/theme.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "signal.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *TAG = "widget_text";

#define TEXT_DEFAULT_W 100
#define TEXT_DEFAULT_H 24
#define TEXT_DEFAULT_COLOR lv_color_white()

/* ── Vtable implementation ───────────────────────────────────────────────── */

static void _text_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	if (!w->root || !lv_obj_is_valid(w->root)) return;
	text_data_t *td = (text_data_t *)w->type_data;
	if (is_stale) {
		lv_label_set_text(w->root, "---");
		return;
	}
	char buf[32];
	if (!td || td->decimals == 0)
		snprintf(buf, sizeof(buf), "%d", (int)value);
	else
		snprintf(buf, sizeof(buf), "%.*f", td->decimals, (double)value);
	lv_label_set_text(w->root, buf);
}

static void _text_create(widget_t *w, lv_obj_t *parent) {
	lv_obj_t *label = lv_label_create(parent);
	if (!label) {
		ESP_LOGE(TAG, "_text_create: lv_label_create failed");
		return;
	}
	lv_obj_set_size(label, (lv_coord_t)w->w, (lv_coord_t)w->h);
	lv_obj_set_align(label, LV_ALIGN_CENTER);
	lv_obj_set_pos(label, w->x, w->y);
	lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
	text_data_t *td = (text_data_t *)w->type_data;
	lv_obj_set_style_text_color(label, td ? td->text_color : TEXT_DEFAULT_COLOR, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *resolved = td ? widget_resolve_font(td->font) : NULL;
	lv_obj_set_style_text_font(label, resolved ? resolved : THEME_FONT_BODY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Show static text if no signal bound, otherwise "---" until signal arrives */
	if (td && td->signal_index < 0 && td->static_text[0] != '\0')
		lv_label_set_text(label, td->static_text);
	else
		lv_label_set_text(label, "---");
	w->root = label;

	/* Subscribe to signal if bound */
	if (td && td->signal_index >= 0)
		signal_subscribe(td->signal_index, _text_on_signal, w);
}

static void _text_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, (lv_coord_t)nw, (lv_coord_t)nh);
	w->w = nw;
	w->h = nh;
}

static void _text_open_settings(widget_t *w) {
	(void)w;
}

static void _text_to_json(widget_t *w, cJSON *out) {
	widget_base_to_json(w, out);
	text_data_t *td = (text_data_t *)w->type_data;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	if (td) {
		cJSON_AddNumberToObject(cfg, "slot", td->value_idx);
		cJSON_AddNumberToObject(cfg, "decimals", td->decimals);
		if (td->static_text[0] != '\0')
			cJSON_AddStringToObject(cfg, "static_text", td->static_text);
		if (td->signal_name[0] != '\0')
			cJSON_AddStringToObject(cfg, "signal_name", td->signal_name);
		if (td->font[0] != '\0')
			cJSON_AddStringToObject(cfg, "font", td->font);
		if (td->text_color.full != TEXT_DEFAULT_COLOR.full)
			cJSON_AddNumberToObject(cfg, "text_color", td->text_color.full);
	}
}

static void _text_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
	text_data_t *td = (text_data_t *)w->type_data;
	if (!td) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;
	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
	if (cJSON_IsNumber(item)) {
		uint8_t idx = (uint8_t)item->valueint;
		if (idx < 13) td->value_idx = idx;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "decimals");
	if (cJSON_IsNumber(item)) td->decimals = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "static_text");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(td->static_text, item->valuestring, sizeof(td->static_text));
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(td->signal_name, item->valuestring, sizeof(td->signal_name));
	}

	item = cJSON_GetObjectItemCaseSensitive(cfg, "font");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(td->font, item->valuestring, sizeof(td->font));
	}

	item = cJSON_GetObjectItemCaseSensitive(cfg, "text_color");
	if (cJSON_IsNumber(item)) td->text_color.full = (uint16_t)item->valuedouble;

	/* Resolve signal name → index */
	if (td->signal_name[0] != '\0')
		td->signal_index = signal_find_by_name(td->signal_name);
}

static void _text_destroy(widget_t *w) {
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	free(w->type_data);
	free(w);
}

/* ── Rule overrides ──────────────────────────────────────────────────────── */

static void _text_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	text_data_t *td = (text_data_t *)w->type_data;
	if (!td) return;

	/* Restore base value */
	lv_color_t c = td->text_color;

	/* Overlay active overrides */
	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "text_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			c.full = (uint16_t)o->value.color;
		}
	}

	/* Apply to LVGL object */
	lv_obj_set_style_text_color(w->root, c, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ── Factory ─────────────────────────────────────────────────────────────── */

widget_t *widget_text_create_instance(uint8_t value_idx) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	text_data_t *td = heap_caps_calloc(1, sizeof(text_data_t), MALLOC_CAP_SPIRAM);
	if (!td) td = calloc(1, sizeof(text_data_t));
	if (!td) { free(w); return NULL; }

	td->value_idx = value_idx < 13 ? value_idx : 0;
	td->signal_index = -1;
	td->text_color = TEXT_DEFAULT_COLOR;

	w->type = WIDGET_TEXT;
	w->slot = td->value_idx;
	w->x = 0;
	w->y = 0;
	w->w = TEXT_DEFAULT_W;
	w->h = TEXT_DEFAULT_H;
	w->type_data = td;
	snprintf(w->id, sizeof(w->id), "text_%u", td->value_idx);

	w->create = _text_create;
	w->resize = _text_resize;
	w->open_settings = _text_open_settings;
	w->to_json = _text_to_json;
	w->from_json = _text_from_json;
	w->destroy = _text_destroy;
	w->apply_overrides = _text_apply_overrides;

	return w;
}

uint8_t widget_text_get_value_idx(const widget_t *w) {
	if (!w || w->type != WIDGET_TEXT || !w->type_data) return 0;
	return ((const text_data_t *)w->type_data)->value_idx;
}

bool widget_text_has_signal(const widget_t *w) {
	if (!w || w->type != WIDGET_TEXT || !w->type_data) return false;
	return ((const text_data_t *)w->type_data)->signal_index >= 0;
}
