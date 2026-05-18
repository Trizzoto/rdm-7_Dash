/*
 * widget_line.c -- Decorative line widget.
 *
 * Renders a horizontal, vertical, or diagonal line inside its bounding box
 * using a transparent lv_obj_t root with a custom draw callback. Supports
 * color, thickness, opacity, rounded ends, and dash gap. Night-mode color
 * override and signal-driven rule overrides are both supported.
 */
#include "widget_line.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_line";

/* ── Defaults ───────────────────────────────────────────────────────────── */

#define DEF_LINE_COLOR  0xFFFFFF
#define DEF_LINE_OPA    255
#define DEF_LINE_WIDTH  4
#define DEF_ROUNDED     false
#define DEF_ORIENT      LINE_ORIENT_HORIZONTAL
#define DEF_DASH_GAP    0
#define DEF_W           200
#define DEF_H           4

/* ── Helpers ────────────────────────────────────────────────────────────── */

static inline uint32_t _color_to_u32(lv_color_t c) { return (uint32_t)c.full; }

static inline lv_color_t _u32_to_color(uint32_t v) {
    lv_color_t c;
    c.ch.red   = (v >> 11) & 0x1F;
    c.ch.green = (v >> 5)  & 0x3F;
    c.ch.blue  = v & 0x1F;
    return c;
}

static line_orientation_t _orient_from_str(const char *s) {
    if (!s) return LINE_ORIENT_HORIZONTAL;
    if (strcmp(s, "vertical")     == 0) return LINE_ORIENT_VERTICAL;
    if (strcmp(s, "diagonal_fwd") == 0) return LINE_ORIENT_DIAG_FWD;
    if (strcmp(s, "diagonal_bwd") == 0) return LINE_ORIENT_DIAG_BWD;
    return LINE_ORIENT_HORIZONTAL;
}

static const char *_orient_to_str(line_orientation_t o) {
    switch (o) {
    case LINE_ORIENT_VERTICAL:  return "vertical";
    case LINE_ORIENT_DIAG_FWD:  return "diagonal_fwd";
    case LINE_ORIENT_DIAG_BWD:  return "diagonal_bwd";
    default:                    return "horizontal";
    }
}

/* ── Draw callback ──────────────────────────────────────────────────────── */

static void _line_draw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_END) return;

    widget_t      *w        = (widget_t *)lv_event_get_user_data(e);
    line_data_t   *ld       = (line_data_t *)w->type_data;
    lv_obj_t      *obj      = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t x1c = coords.x1, y1c = coords.y1;
    int32_t x2c = coords.x2, y2c = coords.y2;
    int32_t pw  = x2c - x1c;
    int32_t ph  = y2c - y1c;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color      = ld->line_color;
    dsc.opa        = ld->line_opa;
    dsc.width      = ld->line_width;
    dsc.dash_gap   = ld->dash_gap;
    dsc.dash_width = ld->dash_gap > 0 ? ld->dash_gap * 2 : 0;
    dsc.round_start = ld->rounded ? 1 : 0;
    dsc.round_end   = ld->rounded ? 1 : 0;

    lv_point_t p1, p2;
    switch (ld->orientation) {
    case LINE_ORIENT_VERTICAL:
        p1 = (lv_point_t){x1c + pw / 2, y1c};
        p2 = (lv_point_t){x1c + pw / 2, y2c};
        break;
    case LINE_ORIENT_DIAG_FWD:
        p1 = (lv_point_t){x1c, y2c};
        p2 = (lv_point_t){x2c, y1c};
        break;
    case LINE_ORIENT_DIAG_BWD:
        p1 = (lv_point_t){x1c, y1c};
        p2 = (lv_point_t){x2c, y2c};
        break;
    default: /* horizontal */
        p1 = (lv_point_t){x1c, y1c + ph / 2};
        p2 = (lv_point_t){x2c, y1c + ph / 2};
        break;
    }

    lv_draw_line(draw_ctx, &dsc, &p1, &p2);
}

/* Forward declarations */
static void _line_apply_night_mode(widget_t *w, bool active);
static void _line_night_cb(bool active, void *user_data);

/* ── vtable: create ─────────────────────────────────────────────────────── */

static void _line_create(widget_t *w, lv_obj_t *parent) {
    line_data_t *ld = (line_data_t *)w->type_data;
    if (!ld) return;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w->w, w->h);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_pos(obj, w->x, w->y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);

    /* Fully transparent base */
    lv_obj_set_style_bg_opa(obj,      LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0,            LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0,            LV_PART_MAIN);

    lv_obj_add_event_cb(obj, _line_draw_cb, LV_EVENT_DRAW_MAIN_END, w);

    w->root = obj;

    if (ld->night.has_line_color) {
        night_mode_subscribe(_line_night_cb, w);
        _line_apply_night_mode(w, night_mode_is_active());
    }
}

/* ── vtable: resize ─────────────────────────────────────────────────────── */

static void _line_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    w->w = nw;
    w->h = nh;
}

/* ── vtable: open_settings ──────────────────────────────────────────────── */

static void _line_open_settings(widget_t *w) { (void)w; }

/* ── vtable: to_json ────────────────────────────────────────────────────── */

static void _line_to_json(widget_t *w, cJSON *out) {
    line_data_t *ld = (line_data_t *)w->type_data;
    widget_base_to_json(w, out);
    if (!ld) return;

    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;

    uint32_t col = _color_to_u32(ld->line_color);
    if (col != DEF_LINE_COLOR)
        cJSON_AddNumberToObject(cfg, "line_color", col);
    if (ld->line_opa != DEF_LINE_OPA)
        cJSON_AddNumberToObject(cfg, "line_opa", ld->line_opa);
    if (ld->line_width != DEF_LINE_WIDTH)
        cJSON_AddNumberToObject(cfg, "line_width", ld->line_width);
    if (ld->rounded != DEF_ROUNDED)
        cJSON_AddBoolToObject(cfg, "rounded", ld->rounded);
    if (ld->orientation != DEF_ORIENT)
        cJSON_AddStringToObject(cfg, "orientation", _orient_to_str(ld->orientation));
    if (ld->dash_gap != DEF_DASH_GAP)
        cJSON_AddNumberToObject(cfg, "dash_gap", ld->dash_gap);

    {
        cJSON *n = cJSON_CreateObject();
        NIGHT_SERIALIZE_COLOR(n, ld->night, line_color);
        if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
        else cJSON_Delete(n);
    }
}

/* ── vtable: from_json ──────────────────────────────────────────────────── */

static void _line_from_json(widget_t *w, cJSON *in) {
    line_data_t *ld = (line_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!ld) return;

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;

    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "line_color");
    if (cJSON_IsNumber(item)) ld->line_color = _u32_to_color((uint32_t)item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "line_opa");
    if (cJSON_IsNumber(item)) ld->line_opa = (lv_opa_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "line_width");
    if (cJSON_IsNumber(item)) ld->line_width = (uint8_t)LV_CLAMP(1, item->valueint, 30);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "rounded");
    if (cJSON_IsBool(item)) ld->rounded = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "orientation");
    if (cJSON_IsString(item)) ld->orientation = _orient_from_str(item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "dash_gap");
    if (cJSON_IsNumber(item)) ld->dash_gap = (uint8_t)LV_CLAMP(0, item->valueint, 40);

    cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
    if (cJSON_IsObject(night)) {
        NIGHT_PARSE_COLOR(night, ld->night, line_color);
    }
}

/* ── vtable: destroy ────────────────────────────────────────────────────── */

static void _line_destroy(widget_t *w) {
    if (!w) return;
    night_mode_unsubscribe(_line_night_cb, w);
    widget_rules_free(w);
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    if (w->type_data) free(w->type_data);
    free(w);
}

/* ── apply_overrides ────────────────────────────────────────────────────── */

static void _line_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    line_data_t *ld = (line_data_t *)w->type_data;
    if (!ld) return;

    bool changed = false;
    for (uint8_t i = 0; i < count; i++) {
        const rule_override_t *o = &ov[i];
        if (strcmp(o->field_name, "line_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            ld->line_color.full = (uint16_t)o->value.color;
            changed = true;
        } else if (strcmp(o->field_name, "line_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
            ld->line_opa = (lv_opa_t)o->value.num;
            changed = true;
        }
    }
    if (changed) lv_obj_invalidate(w->root);
}

/* ── Night mode ─────────────────────────────────────────────────────────── */

static void _line_apply_night_mode(widget_t *w, bool active) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    line_data_t *ld = (line_data_t *)w->type_data;
    if (!ld) return;

    ld->line_color = NIGHT_PICK_COLOR(active, ld->night, line_color, ld->line_color);
    lv_obj_invalidate(w->root);
}

static void _line_night_cb(bool active, void *user_data) {
    _line_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Line is a purely visual widget — no signal binding. The line is drawn in
 * a DRAW_MAIN_END custom callback, so live updates just need to invalidate
 * the root for the next redraw to pick up the new field. */

static bool _line_inspector_get(const widget_t *w, const char *name,
                                widget_field_value_t *out) {
	if (!w || w->type != WIDGET_LINE || !w->type_data || !name || !out) return false;
	const line_data_t *ld = (const line_data_t *)w->type_data;

	if (strcmp(name, "line_color") == 0)  { out->color = lv_color_to32(ld->line_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "line_width") == 0)  { out->i = ld->line_width;   return true; }
	if (strcmp(name, "line_opa") == 0)    { out->i = ld->line_opa;     return true; }
	if (strcmp(name, "rounded") == 0)     { out->b = ld->rounded;      return true; }
	if (strcmp(name, "orientation") == 0) { out->i = ld->orientation;  return true; }
	if (strcmp(name, "dash_gap") == 0)    { out->i = ld->dash_gap;     return true; }
	return false;
}

static bool _line_inspector_set(widget_t *w, const char *name,
                                const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_LINE || !w->type_data || !name || !in) return false;
	line_data_t *ld = (line_data_t *)w->type_data;

	if (strcmp(name, "line_color") == 0) {
		ld->line_color = lv_color_hex(in->color);
	} else if (strcmp(name, "line_width") == 0) {
		int v = in->i; if (v < 1) v = 1; if (v > 30) v = 30;
		ld->line_width = (uint8_t)v;
	} else if (strcmp(name, "line_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		ld->line_opa = (lv_opa_t)v;
	} else if (strcmp(name, "rounded") == 0) {
		ld->rounded = in->b;
	} else if (strcmp(name, "orientation") == 0) {
		int v = in->i; if (v < 0 || v > 3) v = 0;
		ld->orientation = (line_orientation_t)v;
	} else if (strcmp(name, "dash_gap") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 40) v = 40;
		ld->dash_gap = (uint8_t)v;
	} else {
		return false;
	}

	if (w->root && lv_obj_is_valid(w->root)) lv_obj_invalidate(w->root);
	return true;
}

/* ── Factory ────────────────────────────────────────────────────────────── */

widget_t *widget_line_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) {
        ESP_LOGE(TAG, "Failed to allocate widget_t");
        return NULL;
    }

    line_data_t *ld = heap_caps_calloc(1, sizeof(line_data_t), MALLOC_CAP_SPIRAM);
    if (!ld) ld = calloc(1, sizeof(line_data_t));
    if (!ld) { free(w); return NULL; }

    ld->line_color   = lv_color_hex(DEF_LINE_COLOR);
    ld->line_opa     = DEF_LINE_OPA;
    ld->line_width   = DEF_LINE_WIDTH;
    ld->rounded      = DEF_ROUNDED;
    ld->orientation  = DEF_ORIENT;
    ld->dash_gap     = DEF_DASH_GAP;

    w->type      = WIDGET_LINE;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = DEF_W;
    w->h         = DEF_H;
    w->type_data = ld;
    snprintf(w->id, sizeof(w->id), "line_%u", slot);

    w->create           = _line_create;
    w->resize           = _line_resize;
    w->open_settings    = _line_open_settings;
    w->to_json          = _line_to_json;
    w->from_json        = _line_from_json;
    w->destroy          = _line_destroy;
    w->apply_overrides  = _line_apply_overrides;
    w->apply_night_mode = _line_apply_night_mode;
    w->inspector_get    = _line_inspector_get;
    w->inspector_set    = _line_inspector_set;

    ESP_LOGI(TAG, "Created line instance slot=%u", slot);
    return w;
}
