/*
 * widget_text.c — Text widget displaying live CAN value strings.
 *
 * Binds to a value slot (0-12). Receives text_update_t via w->update(w, data)
 * from the CAN dispatch loop. Uses the pre-formatted value_str directly;
 * no heap allocation, no global string arrays.
 */
#include "widget_text.h"
#include "widget_dispatcher.h"
#include "widget_types.h"
#include "ui/theme.h"
#include "cJSON.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_text";

#define TEXT_DEFAULT_W 100
#define TEXT_DEFAULT_H 24

/* ── Vtable implementation ───────────────────────────────────────────────── */

static void _text_create(widget_t *w, lv_obj_t *parent) {
	lv_obj_t *label = lv_label_create(parent);
	if (!label) {
		ESP_LOGE(TAG, "_text_create: lv_label_create failed");
		return;
	}
	lv_obj_set_size(label, (lv_coord_t)w->w, (lv_coord_t)w->h);
	lv_obj_set_pos(label, w->x, w->y);
	lv_label_set_text(label, "---");
	lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
	w->root = label;
}

static void _text_update(widget_t *w, void *data) {
	text_update_t *tud = (text_update_t *)data;
	if (!tud || !w || !w->root || !lv_obj_is_valid(w->root))
		return;
	uint8_t bound_idx = (uint8_t)(uintptr_t)w->type_data;
	if (tud->value_idx != bound_idx)
		return;
	lv_label_set_text(w->root, tud->value_str);
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
	uint8_t value_idx = (uint8_t)(uintptr_t)w->type_data;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (cfg)
		cJSON_AddNumberToObject(cfg, "slot", value_idx);
}

static void _text_from_json(widget_t *w, cJSON *in) {
	widget_base_from_json(w, in);
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (cfg) {
		cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
		if (cJSON_IsNumber(slot_item)) {
			uint8_t idx = (uint8_t)slot_item->valueint;
			if (idx < 13)
				w->type_data = (void *)(uintptr_t)idx;
		}
	}
}

static void _text_destroy(widget_t *w) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	free(w);
}

/* ── Factory ─────────────────────────────────────────────────────────────── */

widget_t *widget_text_create_instance(uint8_t value_idx) {
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w)
		return NULL;

	w->type = WIDGET_TEXT;
	w->x = 0;
	w->y = 0;
	w->w = TEXT_DEFAULT_W;
	w->h = TEXT_DEFAULT_H;
	w->type_data = (void *)(uintptr_t)(value_idx < 13 ? value_idx : 0);
	snprintf(w->id, sizeof(w->id), "text_%u", value_idx < 13 ? value_idx : 0);

	w->create = _text_create;
	w->update = _text_update;
	w->resize = _text_resize;
	w->open_settings = _text_open_settings;
	w->to_json = _text_to_json;
	w->from_json = _text_from_json;
	w->destroy = _text_destroy;

	return w;
}
