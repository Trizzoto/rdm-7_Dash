/*
 * widget_shape_panel.c -- Decorative rectangle / rounded-rect widget.
 *
 * Purely visual: no data binding, no signal subscription.
 * Used as backgrounds, dividers, or grouping elements.
 */
#include "widget_shape_panel.h"
#include "widget_rules.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "widget_types.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_shape_panel";

/* ── Default values ─────────────────────────────────────────────────────── */

#define DEF_BG_COLOR      0x1A1A1A
#define DEF_BG_OPA        255
#define DEF_BORDER_COLOR  0x2E2F2E
#define DEF_BORDER_WIDTH  0
#define DEF_BORDER_RADIUS 10
#define DEF_SHADOW_WIDTH  0
#define DEF_SHADOW_COLOR  0x000000
#define DEF_SHADOW_OPA    128
#define DEF_SHADOW_OFS_X  0
#define DEF_SHADOW_OFS_Y  0
#define DEF_W             150
#define DEF_H             80

/* ── Helpers ────────────────────────────────────────────────────────────── */

static inline uint32_t _color_to_u32(lv_color_t c) {
    return (uint32_t)c.full;
}

static inline lv_color_t _u32_to_color(uint32_t v) {
    lv_color_t c;
    c.ch.red   = (v >> 11) & 0x1F;
    c.ch.green = (v >> 5)  & 0x3F;
    c.ch.blue  = v & 0x1F;
    return c;
}

/* ── vtable: create ─────────────────────────────────────────────────────── */

static void _shape_panel_create(widget_t *w, lv_obj_t *parent) {
    shape_panel_data_t *sd = (shape_panel_data_t *)w->type_data;
    if (!sd) return;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w->w, w->h);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_pos(obj, w->x, w->y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);

    /* Background */
    lv_obj_set_style_bg_color(obj, sd->bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, sd->bg_opa, LV_PART_MAIN);

    /* Border */
    lv_obj_set_style_border_color(obj, sd->border_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, sd->border_width, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, sd->border_radius, LV_PART_MAIN);

    /* Shadow */
    lv_obj_set_style_shadow_width(obj, sd->shadow_width, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(obj, sd->shadow_color, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(obj, sd->shadow_opa, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_x(obj, sd->shadow_ofs_x, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(obj, sd->shadow_ofs_y, LV_PART_MAIN);

    w->root = obj;
}

/* ── vtable: resize ─────────────────────────────────────────────────────── */

static void _shape_panel_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    w->w = nw;
    w->h = nh;
}

/* ── vtable: open_settings ──────────────────────────────────────────────── */

static void _shape_panel_open_settings(widget_t *w) { (void)w; }

/* ── vtable: to_json (defaults-only serialization) ──────────────────────── */

static void _shape_panel_to_json(widget_t *w, cJSON *out) {
    shape_panel_data_t *sd = (shape_panel_data_t *)w->type_data;
    widget_base_to_json(w, out);
    if (!sd) return;

    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;

    uint32_t col;

    col = _color_to_u32(sd->bg_color);
    if (col != DEF_BG_COLOR)
        cJSON_AddNumberToObject(cfg, "bg_color", col);
    if (sd->bg_opa != DEF_BG_OPA)
        cJSON_AddNumberToObject(cfg, "bg_opa", sd->bg_opa);

    col = _color_to_u32(sd->border_color);
    if (col != DEF_BORDER_COLOR)
        cJSON_AddNumberToObject(cfg, "border_color", col);
    if (sd->border_width != DEF_BORDER_WIDTH)
        cJSON_AddNumberToObject(cfg, "border_width", sd->border_width);
    if (sd->border_radius != DEF_BORDER_RADIUS)
        cJSON_AddNumberToObject(cfg, "border_radius", sd->border_radius);

    if (sd->shadow_width != DEF_SHADOW_WIDTH)
        cJSON_AddNumberToObject(cfg, "shadow_width", sd->shadow_width);
    col = _color_to_u32(sd->shadow_color);
    if (col != DEF_SHADOW_COLOR)
        cJSON_AddNumberToObject(cfg, "shadow_color", col);
    if (sd->shadow_opa != DEF_SHADOW_OPA)
        cJSON_AddNumberToObject(cfg, "shadow_opa", sd->shadow_opa);
    if (sd->shadow_ofs_x != DEF_SHADOW_OFS_X)
        cJSON_AddNumberToObject(cfg, "shadow_ofs_x", sd->shadow_ofs_x);
    if (sd->shadow_ofs_y != DEF_SHADOW_OFS_Y)
        cJSON_AddNumberToObject(cfg, "shadow_ofs_y", sd->shadow_ofs_y);
}

/* ── vtable: from_json ──────────────────────────────────────────────────── */

static void _shape_panel_from_json(widget_t *w, cJSON *in) {
    shape_panel_data_t *sd = (shape_panel_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!sd) return;

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;

    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_color");
    if (cJSON_IsNumber(item)) sd->bg_color = _u32_to_color((uint32_t)item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_opa");
    if (cJSON_IsNumber(item)) sd->bg_opa = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "border_color");
    if (cJSON_IsNumber(item)) sd->border_color = _u32_to_color((uint32_t)item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "border_width");
    if (cJSON_IsNumber(item)) sd->border_width = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "border_radius");
    if (cJSON_IsNumber(item)) sd->border_radius = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_width");
    if (cJSON_IsNumber(item)) sd->shadow_width = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_color");
    if (cJSON_IsNumber(item)) sd->shadow_color = _u32_to_color((uint32_t)item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_opa");
    if (cJSON_IsNumber(item)) sd->shadow_opa = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_ofs_x");
    if (cJSON_IsNumber(item)) sd->shadow_ofs_x = (int8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "shadow_ofs_y");
    if (cJSON_IsNumber(item)) sd->shadow_ofs_y = (int8_t)item->valueint;
}

/* ── vtable: destroy ────────────────────────────────────────────────────── */

static void _shape_panel_destroy(widget_t *w) {
    if (!w) return;
    widget_rules_free(w);
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    if (w->type_data) free(w->type_data);
    free(w);
}

/* ── apply_overrides ────────────────────────────────────────────────────── */

static void _shape_panel_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    shape_panel_data_t *sd = (shape_panel_data_t *)w->type_data;
    if (!sd) return;

    lv_color_t bg = sd->bg_color;
    uint8_t bg_opa = sd->bg_opa;
    lv_color_t bdr = sd->border_color;
    uint8_t bdr_w = sd->border_width;
    uint8_t radius = sd->border_radius;
    lv_color_t shd = sd->shadow_color;
    uint8_t shd_w = sd->shadow_width;
    uint8_t shd_opa = sd->shadow_opa;

    for (uint8_t i = 0; i < count; i++) {
        const rule_override_t *o = &ov[i];
        if (strcmp(o->field_name, "bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            bg.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "bg_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
            bg_opa = (uint8_t)o->value.num;
        } else if (strcmp(o->field_name, "border_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            bdr.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "border_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
            bdr_w = (uint8_t)o->value.num;
        } else if (strcmp(o->field_name, "border_radius") == 0 && o->value_type == RULE_VAL_NUMBER) {
            radius = (uint8_t)o->value.num;
        } else if (strcmp(o->field_name, "shadow_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            shd.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "shadow_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
            shd_w = (uint8_t)o->value.num;
        } else if (strcmp(o->field_name, "shadow_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
            shd_opa = (uint8_t)o->value.num;
        }
    }

    lv_obj_set_style_bg_color(w->root, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(w->root, bg_opa, LV_PART_MAIN);
    lv_obj_set_style_border_color(w->root, bdr, LV_PART_MAIN);
    lv_obj_set_style_border_width(w->root, bdr_w, LV_PART_MAIN);
    lv_obj_set_style_radius(w->root, radius, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(w->root, shd, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(w->root, shd_w, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(w->root, shd_opa, LV_PART_MAIN);
}

/* ── Factory ────────────────────────────────────────────────────────────── */

widget_t *widget_shape_panel_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) {
        ESP_LOGE(TAG, "Failed to allocate widget_t");
        return NULL;
    }

    shape_panel_data_t *sd = heap_caps_calloc(1, sizeof(shape_panel_data_t), MALLOC_CAP_SPIRAM);
    if (!sd) sd = calloc(1, sizeof(shape_panel_data_t));
    if (!sd) { free(w); return NULL; }

    /* Set defaults */
    sd->bg_color      = lv_color_hex(DEF_BG_COLOR);
    sd->bg_opa        = DEF_BG_OPA;
    sd->border_color  = lv_color_hex(DEF_BORDER_COLOR);
    sd->border_width  = DEF_BORDER_WIDTH;
    sd->border_radius = DEF_BORDER_RADIUS;
    sd->shadow_width  = DEF_SHADOW_WIDTH;
    sd->shadow_color  = lv_color_hex(DEF_SHADOW_COLOR);
    sd->shadow_opa    = DEF_SHADOW_OPA;
    sd->shadow_ofs_x  = DEF_SHADOW_OFS_X;
    sd->shadow_ofs_y  = DEF_SHADOW_OFS_Y;

    w->type      = WIDGET_SHAPE_PANEL;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = DEF_W;
    w->h         = DEF_H;
    w->type_data = sd;
    snprintf(w->id, sizeof(w->id), "shape_panel_%u", slot);

    w->create        = _shape_panel_create;
    w->resize        = _shape_panel_resize;
    w->open_settings = _shape_panel_open_settings;
    w->to_json       = _shape_panel_to_json;
    w->from_json     = _shape_panel_from_json;
    w->destroy       = _shape_panel_destroy;
    w->apply_overrides = _shape_panel_apply_overrides;

    ESP_LOGI(TAG, "Created shape_panel instance slot=%u", slot);
    return w;
}
