/*
 * widget_banner.c — Threshold-driven alert banner across the screen.
 *
 * Subscribes to a single signal, evaluates a comparator (gt/lt/gte/lte/
 * eq/neq/range/always) against either a single threshold or a range,
 * and shows/hides the bar accordingly. Acts as a dashboard-wide
 * "shouty" warning — the bar takes the full screen width by default
 * so it's hard to miss when it fires.
 *
 * The visibility model is intentionally NOT routed through the rules
 * engine because we want a self-contained widget that's discoverable
 * from the palette without the user having to know that rules exist.
 * Stylistically the bar is just a single rounded-rect container with
 * a label inside; appearance fields are all live-mutable through the
 * inspector.
 */
#include "widget_banner.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "ui/theme.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "signal.h"
#include "lvgl.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_banner";

/* ── Defaults ───────────────────────────────────────────────────────────── */

#define DEF_BG_COLOR       0xFF0000        /* fire-engine red */
#define DEF_BG_OPA         128             /* ~half opacity */
#define DEF_TEXT_COLOR     0x000000
#define DEF_BORDER_WIDTH   0
#define DEF_BORDER_COLOR   0x000000
#define DEF_RADIUS         0
#define DEF_ALIGN          1               /* center */
#define DEF_OP             BANNER_OP_GT
#define DEF_THRESHOLD      0.0f
#define DEF_TEXT           "WARNING"

/* Banner default geometry: full screen width, 80 px tall, anchored 40 px
 * up from the bottom of the canvas so the user sees a strip of dashboard
 * behind it on first drop. Caller can move it via the standard widget
 * drag handles. */
#define DEF_W              800
#define DEF_H              80
#define DEF_X              0
#define DEF_Y              200             /* centre of an 80px strip at y∈[160,240], i.e. 40 px above the 480-tall screen's bottom */

/* ── Forward decls ──────────────────────────────────────────────────────── */
static void _banner_apply_visibility(widget_t *w);
static void _banner_night_cb(bool active, void *user_data);
static void _banner_apply_night_mode(widget_t *w, bool active);

/* ── Helpers ────────────────────────────────────────────────────────────── */

static inline lv_color_t _u32_to_color(uint32_t v) {
	return lv_color_hex(v);
}

/* Evaluate banner_data_t's condition against `value`. Stale signals
 * are treated as "no condition met" so a CAN dropout doesn't leave a
 * stale warning latched. */
static bool _banner_eval(const banner_data_t *bd, float value, bool is_stale) {
	if (!bd) return false;
	if (bd->op == BANNER_OP_ALWAYS) return true;
	if (is_stale) return false;
	switch (bd->op) {
	case BANNER_OP_GT:    return value >  bd->threshold;
	case BANNER_OP_LT:    return value <  bd->threshold;
	case BANNER_OP_GTE:   return value >= bd->threshold;
	case BANNER_OP_LTE:   return value <= bd->threshold;
	case BANNER_OP_EQ:    return fabsf(value - bd->threshold) <  0.001f;
	case BANNER_OP_NEQ:   return fabsf(value - bd->threshold) >= 0.001f;
	case BANNER_OP_RANGE: return value >= bd->range_min && value <= bd->range_max;
	default:              return false;
	}
}

static void _banner_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	banner_data_t *bd = (banner_data_t *)w->type_data;
	if (!bd) return;
	bool active = _banner_eval(bd, value, is_stale);
	if (active == bd->condition_active) return;   /* skip unchanged */
	bd->condition_active = active;
	_banner_apply_visibility(w);
}

static void _banner_apply_visibility(widget_t *w) {
	banner_data_t *bd = (banner_data_t *)w->type_data;
	if (!bd || !bd->bar || !lv_obj_is_valid(bd->bar)) return;
	if (bd->condition_active) {
		lv_obj_clear_flag(bd->bar, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(bd->bar, LV_OBJ_FLAG_HIDDEN);
	}
}

/* ── vtable: create ─────────────────────────────────────────────────────── */

static void _banner_create(widget_t *w, lv_obj_t *parent) {
	banner_data_t *bd = (banner_data_t *)w->type_data;
	if (!bd) return;

	/* Bar container — full geometry from widget_t, styled per type_data. */
	lv_obj_t *bar = lv_obj_create(parent);
	lv_obj_set_size(bar, w->w, w->h);
	lv_obj_set_align(bar, LV_ALIGN_CENTER);
	lv_obj_set_pos(bar, w->x, w->y);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(bar, 8, LV_PART_MAIN);

	bool night_active = night_mode_is_active();
	lv_color_t bg_col   = NIGHT_PICK_COLOR(night_active, bd->night, bg_color,     bd->bg_color);
	lv_color_t txt_col  = NIGHT_PICK_COLOR(night_active, bd->night, text_color,   bd->text_color);
	lv_color_t bdr_col  = NIGHT_PICK_COLOR(night_active, bd->night, border_color, bd->border_color);

	lv_obj_set_style_bg_color(bar, bg_col, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(bar, bd->bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar, bd->radius, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar, bd->border_width, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (bd->border_width > 0) {
		lv_obj_set_style_border_color(bar, bdr_col, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Label centred inside the bar. lv_label is one of the cheapest LVGL
	 * objects and resizes itself to its content — we keep it size-content
	 * + aligned so the user's text_align choice controls horizontal
	 * positioning without needing extra LVGL flex container plumbing. */
	lv_obj_t *lbl = lv_label_create(bar);
	lv_label_set_text(lbl, bd->text[0] ? bd->text : DEF_TEXT);
	lv_obj_set_style_text_color(lbl, txt_col, LV_PART_MAIN | LV_STATE_DEFAULT);
	const lv_font_t *resolved = widget_resolve_font(bd->font);
	lv_obj_set_style_text_font(lbl, resolved ? resolved : THEME_FONT_BODY,
	                            LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_text_align_t lv_a = (bd->text_align == 0) ? LV_TEXT_ALIGN_LEFT :
	                       (bd->text_align == 2) ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
	lv_obj_set_style_text_align(lbl, lv_a, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl, w->w - 16);   /* leave space for the 8 px pad */
	lv_obj_center(lbl);

	bd->bar = bar;
	bd->label = lbl;
	w->root = bar;

	/* Default to hidden — condition evaluation will reveal it. ALWAYS-on
	 * mode is the one exception: show immediately so the user gets the
	 * preview without needing a signal to fire. */
	bd->condition_active = (bd->op == BANNER_OP_ALWAYS);
	_banner_apply_visibility(w);

	/* Subscribe to the bound signal — initial eval pulls current value
	 * synchronously so the banner state matches reality even if the
	 * signal is steady-state at create time. */
	if (bd->signal_index >= 0) {
		signal_subscribe(bd->signal_index, _banner_on_signal, w);
		signal_t *sig = signal_get_by_index((uint16_t)bd->signal_index);
		if (sig) {
			_banner_on_signal(sig->current_value, sig->is_stale, w);
		}
	}

	/* Night-mode subscribe only if any override is set — keeps the
	 * subscriber list lean for layouts that don't care about night. */
	if (bd->night.has_bg_color || bd->night.has_text_color || bd->night.has_border_color) {
		night_mode_subscribe(_banner_night_cb, w);
	}
}

/* ── vtable: resize ─────────────────────────────────────────────────────── */

static void _banner_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	banner_data_t *bd = (banner_data_t *)w->type_data;
	if (bd && bd->bar && lv_obj_is_valid(bd->bar)) {
		lv_obj_set_size(bd->bar, nw, nh);
	}
	if (bd && bd->label && lv_obj_is_valid(bd->label)) {
		lv_obj_set_width(bd->label, nw - 16);
		lv_obj_center(bd->label);
	}
	w->w = nw;
	w->h = nh;
}

/* ── vtable: open_settings ──────────────────────────────────────────────── */

static void _banner_open_settings(widget_t *w) { (void)w; }

/* ── vtable: to_json ────────────────────────────────────────────────────── */

static void _banner_to_json(widget_t *w, cJSON *out) {
	banner_data_t *bd = (banner_data_t *)w->type_data;
	widget_base_to_json(w, out);
	if (!bd) return;

	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;

	if (bd->signal_name[0])
		cJSON_AddStringToObject(cfg, "signal_name", bd->signal_name);
	if (bd->text[0])
		cJSON_AddStringToObject(cfg, "text", bd->text);
	if (bd->font[0])
		cJSON_AddStringToObject(cfg, "font", bd->font);
	if (bd->op != DEF_OP)
		cJSON_AddNumberToObject(cfg, "op", bd->op);
	if (bd->threshold != DEF_THRESHOLD)
		cJSON_AddNumberToObject(cfg, "threshold", bd->threshold);
	if (bd->range_min != 0.0f)
		cJSON_AddNumberToObject(cfg, "range_min", bd->range_min);
	if (bd->range_max != 0.0f)
		cJSON_AddNumberToObject(cfg, "range_max", bd->range_max);
	if (bd->text_align != DEF_ALIGN)
		cJSON_AddNumberToObject(cfg, "text_align", bd->text_align);
	if (lv_color_to32(bd->bg_color) != lv_color_to32(lv_color_hex(DEF_BG_COLOR)))
		cJSON_AddNumberToObject(cfg, "bg_color", (int)bd->bg_color.full);
	if (bd->bg_opa != DEF_BG_OPA)
		cJSON_AddNumberToObject(cfg, "bg_opa", bd->bg_opa);
	if (lv_color_to32(bd->text_color) != lv_color_to32(lv_color_hex(DEF_TEXT_COLOR)))
		cJSON_AddNumberToObject(cfg, "text_color", (int)bd->text_color.full);
	if (bd->border_width != DEF_BORDER_WIDTH)
		cJSON_AddNumberToObject(cfg, "border_width", bd->border_width);
	if (lv_color_to32(bd->border_color) != lv_color_to32(lv_color_hex(DEF_BORDER_COLOR)))
		cJSON_AddNumberToObject(cfg, "border_color", (int)bd->border_color.full);
	if (bd->radius != DEF_RADIUS)
		cJSON_AddNumberToObject(cfg, "radius", bd->radius);

	{
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, bd->night, bg_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, text_color);
		NIGHT_SERIALIZE_COLOR(n, bd->night, border_color);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
	}
}

/* ── vtable: from_json ──────────────────────────────────────────────────── */

static void _banner_from_json(widget_t *w, cJSON *in) {
	banner_data_t *bd = (banner_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!bd) return;

	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;
	cJSON *item;

	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring) {
		strncpy(bd->signal_name, item->valuestring, sizeof(bd->signal_name) - 1);
		bd->signal_name[sizeof(bd->signal_name) - 1] = '\0';
		bd->signal_index = signal_find_by_name(bd->signal_name);
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "text");
	if (cJSON_IsString(item) && item->valuestring) {
		strncpy(bd->text, item->valuestring, sizeof(bd->text) - 1);
		bd->text[sizeof(bd->text) - 1] = '\0';
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "font");
	if (cJSON_IsString(item) && item->valuestring) {
		strncpy(bd->font, item->valuestring, sizeof(bd->font) - 1);
		bd->font[sizeof(bd->font) - 1] = '\0';
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "op");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 0 || v > BANNER_OP_ALWAYS) v = DEF_OP;
		bd->op = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "threshold");
	if (cJSON_IsNumber(item)) bd->threshold = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "range_min");
	if (cJSON_IsNumber(item)) bd->range_min = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "range_max");
	if (cJSON_IsNumber(item)) bd->range_max = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "text_align");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		bd->text_align = (v < 0 || v > 2) ? DEF_ALIGN : (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_color");
	if (cJSON_IsNumber(item)) bd->bg_color.full = (uint16_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_opa");
	if (cJSON_IsNumber(item)) bd->bg_opa = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "text_color");
	if (cJSON_IsNumber(item)) bd->text_color.full = (uint16_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "border_width");
	if (cJSON_IsNumber(item)) bd->border_width = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "border_color");
	if (cJSON_IsNumber(item)) bd->border_color.full = (uint16_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "radius");
	if (cJSON_IsNumber(item)) bd->radius = (uint8_t)item->valueint;

	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, bd->night, bg_color);
		NIGHT_PARSE_COLOR(night, bd->night, text_color);
		NIGHT_PARSE_COLOR(night, bd->night, border_color);
	}
}

/* ── vtable: destroy ────────────────────────────────────────────────────── */

static void _banner_destroy(widget_t *w) {
	if (!w) return;
	banner_data_t *bd = (banner_data_t *)w->type_data;
	if (bd && bd->signal_index >= 0)
		signal_unsubscribe(bd->signal_index, _banner_on_signal, w);
	night_mode_unsubscribe(_banner_night_cb, w);
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	if (w->type_data) free(w->type_data);
	free(w);
}

/* ── apply_overrides ────────────────────────────────────────────────────── */

static void _banner_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	banner_data_t *bd = (banner_data_t *)w->type_data;
	if (!bd) return;

	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bd->bg_color.full = (uint16_t)o->value.color;
			lv_obj_set_style_bg_color(bd->bar, bd->bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (strcmp(o->field_name, "bg_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
			int v = (int)o->value.num; if (v < 0) v = 0; if (v > 255) v = 255;
			bd->bg_opa = (uint8_t)v;
			lv_obj_set_style_bg_opa(bd->bar, bd->bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (strcmp(o->field_name, "text_color") == 0 && o->value_type == RULE_VAL_COLOR) {
			bd->text_color.full = (uint16_t)o->value.color;
			if (bd->label && lv_obj_is_valid(bd->label))
				lv_obj_set_style_text_color(bd->label, bd->text_color,
					LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
}

/* ── Night mode ─────────────────────────────────────────────────────────── */

static void _banner_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	banner_data_t *bd = (banner_data_t *)w->type_data;
	if (!bd) return;
	lv_color_t bg_col  = NIGHT_PICK_COLOR(active, bd->night, bg_color,     bd->bg_color);
	lv_color_t txt_col = NIGHT_PICK_COLOR(active, bd->night, text_color,   bd->text_color);
	lv_color_t bdr_col = NIGHT_PICK_COLOR(active, bd->night, border_color, bd->border_color);
	if (bd->bar && lv_obj_is_valid(bd->bar)) {
		lv_obj_set_style_bg_color(bd->bar, bg_col, LV_PART_MAIN | LV_STATE_DEFAULT);
		if (bd->border_width > 0)
			lv_obj_set_style_border_color(bd->bar, bdr_col, LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (bd->label && lv_obj_is_valid(bd->label))
		lv_obj_set_style_text_color(bd->label, txt_col, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void _banner_night_cb(bool active, void *user_data) {
	_banner_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector get / set ────────────────────────────────────────────────── */

static bool _banner_inspector_get(const widget_t *w, const char *name,
                                   widget_field_value_t *out) {
	if (!w || w->type != WIDGET_BANNER || !w->type_data || !name || !out) return false;
	const banner_data_t *bd = (const banner_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0)  { out->str = bd->signal_name; return true; }
	if (strcmp(name, "text") == 0)         { out->str = bd->text;        return true; }
	if (strcmp(name, "font") == 0)         { out->str = bd->font;        return true; }
	if (strcmp(name, "op") == 0)           { out->i = bd->op;            return true; }
	if (strcmp(name, "threshold") == 0)    { out->i = (int)bd->threshold; return true; }
	if (strcmp(name, "range_min") == 0)    { out->i = (int)bd->range_min; return true; }
	if (strcmp(name, "range_max") == 0)    { out->i = (int)bd->range_max; return true; }
	if (strcmp(name, "text_align") == 0)   { out->i = bd->text_align;    return true; }
	if (strcmp(name, "bg_color") == 0)     { out->color = lv_color_to32(bd->bg_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "bg_opa") == 0)       { out->i = bd->bg_opa;        return true; }
	if (strcmp(name, "text_color") == 0)   { out->color = lv_color_to32(bd->text_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "border_width") == 0) { out->i = bd->border_width;  return true; }
	if (strcmp(name, "border_color") == 0) { out->color = lv_color_to32(bd->border_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "radius") == 0)       { out->i = bd->radius;        return true; }
	return false;
}

static bool _banner_inspector_set(widget_t *w, const char *name,
                                   const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_BANNER || !w->type_data || !name || !in) return false;
	banner_data_t *bd = (banner_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0 && in->str) {
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;
		if (bd->signal_index >= 0)
			signal_unsubscribe(bd->signal_index, _banner_on_signal, w);
		strncpy(bd->signal_name, in->str, sizeof(bd->signal_name) - 1);
		bd->signal_name[sizeof(bd->signal_name) - 1] = '\0';
		bd->signal_index = new_idx;
		if (new_idx >= 0) {
			signal_subscribe(new_idx, _banner_on_signal, w);
			signal_t *sig = signal_get_by_index((uint16_t)new_idx);
			if (sig) _banner_on_signal(sig->current_value, sig->is_stale, w);
		}
		return true;
	}
	if (strcmp(name, "text") == 0 && in->str) {
		strncpy(bd->text, in->str, sizeof(bd->text) - 1);
		bd->text[sizeof(bd->text) - 1] = '\0';
		if (bd->label && lv_obj_is_valid(bd->label))
			lv_label_set_text(bd->label, bd->text[0] ? bd->text : DEF_TEXT);
		return true;
	}
	if (strcmp(name, "font") == 0 && in->str) {
		strncpy(bd->font, in->str, sizeof(bd->font) - 1);
		bd->font[sizeof(bd->font) - 1] = '\0';
		if (bd->label && lv_obj_is_valid(bd->label)) {
			const lv_font_t *f = widget_resolve_font(bd->font);
			lv_obj_set_style_text_font(bd->label, f ? f : THEME_FONT_BODY,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		return true;
	}
	if (strcmp(name, "op") == 0) {
		int v = in->i;
		if (v < 0 || v > BANNER_OP_ALWAYS) v = DEF_OP;
		bd->op = (uint8_t)v;
		/* Re-evaluate immediately so the banner reflects the new op
		 * against the latest known signal value — no need to wait for
		 * the next CAN frame. */
		if (bd->signal_index >= 0) {
			signal_t *sig = signal_get_by_index((uint16_t)bd->signal_index);
			if (sig) _banner_on_signal(sig->current_value, sig->is_stale, w);
		} else if (bd->op == BANNER_OP_ALWAYS) {
			bd->condition_active = true;
			_banner_apply_visibility(w);
		}
		return true;
	}
	if (strcmp(name, "threshold") == 0) { bd->threshold = (float)in->i;
		if (bd->signal_index >= 0) {
			signal_t *sig = signal_get_by_index((uint16_t)bd->signal_index);
			if (sig) _banner_on_signal(sig->current_value, sig->is_stale, w);
		}
		return true;
	}
	if (strcmp(name, "range_min") == 0) { bd->range_min = (float)in->i; return true; }
	if (strcmp(name, "range_max") == 0) { bd->range_max = (float)in->i; return true; }
	if (strcmp(name, "text_align") == 0) {
		int v = in->i; if (v < 0 || v > 2) v = DEF_ALIGN;
		bd->text_align = (uint8_t)v;
		if (bd->label && lv_obj_is_valid(bd->label)) {
			lv_text_align_t lv_a = (v == 0) ? LV_TEXT_ALIGN_LEFT :
			                        (v == 2) ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
			lv_obj_set_style_text_align(bd->label, lv_a, LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		return true;
	}
	if (strcmp(name, "bg_color") == 0) {
		bd->bg_color = lv_color_hex(in->color);
		if (bd->bar && lv_obj_is_valid(bd->bar))
			lv_obj_set_style_bg_color(bd->bar, bd->bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "bg_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		bd->bg_opa = (uint8_t)v;
		if (bd->bar && lv_obj_is_valid(bd->bar))
			lv_obj_set_style_bg_opa(bd->bar, bd->bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "text_color") == 0) {
		bd->text_color = lv_color_hex(in->color);
		if (bd->label && lv_obj_is_valid(bd->label))
			lv_obj_set_style_text_color(bd->label, bd->text_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "border_width") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 20) v = 20;
		bd->border_width = (uint8_t)v;
		if (bd->bar && lv_obj_is_valid(bd->bar)) {
			lv_obj_set_style_border_width(bd->bar, bd->border_width,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			if (bd->border_width > 0) {
				lv_obj_set_style_border_color(bd->bar, bd->border_color,
					LV_PART_MAIN | LV_STATE_DEFAULT);
				lv_obj_set_style_border_opa(bd->bar, LV_OPA_COVER,
					LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		}
		return true;
	}
	if (strcmp(name, "border_color") == 0) {
		bd->border_color = lv_color_hex(in->color);
		if (bd->bar && lv_obj_is_valid(bd->bar) && bd->border_width > 0)
			lv_obj_set_style_border_color(bd->bar, bd->border_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "radius") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 200) v = 200;
		bd->radius = (uint8_t)v;
		if (bd->bar && lv_obj_is_valid(bd->bar))
			lv_obj_set_style_radius(bd->bar, bd->radius, LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	return false;
}

/* ── Factory ────────────────────────────────────────────────────────────── */

widget_t *widget_banner_create_instance(uint8_t slot) {
	(void)slot;
	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w) {
		ESP_LOGE(TAG, "Failed to allocate widget_t");
		return NULL;
	}
	banner_data_t *bd = heap_caps_calloc(1, sizeof(banner_data_t), MALLOC_CAP_SPIRAM);
	if (!bd) bd = calloc(1, sizeof(banner_data_t));
	if (!bd) { free(w); return NULL; }

	bd->signal_index = -1;
	bd->op           = DEF_OP;
	bd->threshold    = DEF_THRESHOLD;
	bd->text_align   = DEF_ALIGN;
	strncpy(bd->text, DEF_TEXT, sizeof(bd->text) - 1);
	bd->bg_color     = _u32_to_color(DEF_BG_COLOR);
	bd->bg_opa       = DEF_BG_OPA;
	bd->text_color   = _u32_to_color(DEF_TEXT_COLOR);
	bd->border_color = _u32_to_color(DEF_BORDER_COLOR);
	bd->border_width = DEF_BORDER_WIDTH;
	bd->radius       = DEF_RADIUS;

	w->type      = WIDGET_BANNER;
	w->slot      = 0;
	w->x         = DEF_X;
	w->y         = DEF_Y;
	w->w         = DEF_W;
	w->h         = DEF_H;
	w->type_data = bd;
	snprintf(w->id, sizeof(w->id), "banner_0");

	w->create           = _banner_create;
	w->resize           = _banner_resize;
	w->open_settings    = _banner_open_settings;
	w->to_json          = _banner_to_json;
	w->from_json        = _banner_from_json;
	w->destroy          = _banner_destroy;
	w->apply_overrides  = _banner_apply_overrides;
	w->apply_night_mode = _banner_apply_night_mode;
	w->inspector_get    = _banner_inspector_get;
	w->inspector_set    = _banner_inspector_set;

	ESP_LOGI(TAG, "Created banner instance slot=%u", slot);
	return w;
}
