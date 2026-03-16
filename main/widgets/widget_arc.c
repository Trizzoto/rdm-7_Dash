/*
 * widget_arc.c -- Decorative arc/ring shape widget (no data binding).
 *
 * Purely visual decoration: renders a foreground and background arc
 * with configurable angles, widths, colors, and rounded ends.
 * No signal subscription or CAN integration.
 */
#include "widget_arc.h"
#include "widget_rules.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "widget_types.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_arc";

#define ARC_DEFAULT_W          200
#define ARC_DEFAULT_H          200
#define ARC_DEFAULT_START      135
#define ARC_DEFAULT_END         45
#define ARC_DEFAULT_WIDTH       10
#define ARC_DEFAULT_COLOR      0x00FF00
#define ARC_DEFAULT_BG_COLOR   0x333333
#define ARC_DEFAULT_BG_WIDTH    10
#define ARC_DEFAULT_ROUNDED     false

static void _arc_create(widget_t *w, lv_obj_t *parent) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    lv_obj_t *obj = lv_arc_create(parent);
    lv_obj_set_size(obj, w->w, w->h);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_pos(obj, w->x, w->y);

    /* Disable interactivity -- purely decorative */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_mode(obj, LV_ARC_MODE_NORMAL);

    /* Background arc (LV_PART_MAIN) */
    lv_arc_set_bg_angles(obj, d->start_angle, d->end_angle);
    lv_obj_set_style_arc_color(obj, d->bg_arc_color, LV_PART_MAIN);
    lv_obj_set_style_arc_width(obj, d->bg_arc_width, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(obj, d->rounded_ends, LV_PART_MAIN);

    /* Foreground/indicator arc (LV_PART_INDICATOR) -- fill completely */
    lv_arc_set_range(obj, 0, 100);
    lv_arc_set_value(obj, 100);
    lv_arc_set_angles(obj, d->start_angle, d->end_angle);
    lv_obj_set_style_arc_color(obj, d->arc_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(obj, d->arc_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(obj, d->rounded_ends, LV_PART_INDICATOR);

    /* Hide the knob */
    lv_obj_set_style_pad_all(obj, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_KNOB);

    /* Remove default background fill so only arcs are visible */
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);

    d->arc_obj = obj;
    w->root = obj;
}

static void _arc_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    w->w = nw;
    w->h = nh;
}

static void _arc_open_settings(widget_t *w) { (void)w; }

static void _arc_to_json(widget_t *w, cJSON *out) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    widget_base_to_json(w, out);
    if (!d) return;

    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;

    if (d->start_angle != ARC_DEFAULT_START)
        cJSON_AddNumberToObject(cfg, "start_angle", d->start_angle);
    if (d->end_angle != ARC_DEFAULT_END)
        cJSON_AddNumberToObject(cfg, "end_angle", d->end_angle);
    if (d->arc_width != ARC_DEFAULT_WIDTH)
        cJSON_AddNumberToObject(cfg, "arc_width", d->arc_width);
    if (lv_color_to16(d->arc_color) != lv_color_to16(lv_color_hex(ARC_DEFAULT_COLOR)))
        cJSON_AddNumberToObject(cfg, "arc_color", lv_color_to16(d->arc_color));
    if (lv_color_to16(d->bg_arc_color) != lv_color_to16(lv_color_hex(ARC_DEFAULT_BG_COLOR)))
        cJSON_AddNumberToObject(cfg, "bg_arc_color", lv_color_to16(d->bg_arc_color));
    if (d->bg_arc_width != ARC_DEFAULT_BG_WIDTH)
        cJSON_AddNumberToObject(cfg, "bg_arc_width", d->bg_arc_width);
    if (d->rounded_ends != ARC_DEFAULT_ROUNDED)
        cJSON_AddBoolToObject(cfg, "rounded_ends", d->rounded_ends);
}

static void _arc_from_json(widget_t *w, cJSON *in) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!d) return;

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;

    cJSON *item;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "start_angle");
    if (cJSON_IsNumber(item)) d->start_angle = (int16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "end_angle");
    if (cJSON_IsNumber(item)) d->end_angle = (int16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_width");
    if (cJSON_IsNumber(item)) d->arc_width = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_color");
    if (cJSON_IsNumber(item)) d->arc_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_arc_color");
    if (cJSON_IsNumber(item)) d->bg_arc_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_arc_width");
    if (cJSON_IsNumber(item)) d->bg_arc_width = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "rounded_ends");
    if (cJSON_IsBool(item)) d->rounded_ends = cJSON_IsTrue(item);
}

static void _arc_destroy(widget_t *w) {
    if (!w) return;
    widget_rules_free(w);
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (d) free(d);
    free(w);
}

/* ── apply_overrides ────────────────────────────────────────────────────── */

static void _arc_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d || !d->arc_obj) return;

    lv_color_t fg = d->arc_color;
    lv_color_t bg = d->bg_arc_color;
    uint8_t fg_w = d->arc_width;
    uint8_t bg_w = d->bg_arc_width;

    for (uint8_t i = 0; i < count; i++) {
        const rule_override_t *o = &ov[i];
        if (strcmp(o->field_name, "arc_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            fg.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "bg_arc_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            bg.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "arc_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
            fg_w = (uint8_t)o->value.num;
        } else if (strcmp(o->field_name, "bg_arc_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
            bg_w = (uint8_t)o->value.num;
        }
    }

    lv_obj_set_style_arc_color(d->arc_obj, fg, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(d->arc_obj, fg_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d->arc_obj, bg, LV_PART_MAIN);
    lv_obj_set_style_arc_width(d->arc_obj, bg_w, LV_PART_MAIN);
}

widget_t *widget_arc_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) return NULL;

    arc_data_t *d = heap_caps_calloc(1, sizeof(arc_data_t), MALLOC_CAP_SPIRAM);
    if (!d) d = calloc(1, sizeof(arc_data_t));
    if (!d) { free(w); return NULL; }

    /* Set defaults */
    d->start_angle  = ARC_DEFAULT_START;
    d->end_angle    = ARC_DEFAULT_END;
    d->arc_width    = ARC_DEFAULT_WIDTH;
    d->arc_color    = lv_color_hex(ARC_DEFAULT_COLOR);
    d->bg_arc_color = lv_color_hex(ARC_DEFAULT_BG_COLOR);
    d->bg_arc_width = ARC_DEFAULT_BG_WIDTH;
    d->rounded_ends = ARC_DEFAULT_ROUNDED;
    d->arc_obj      = NULL;

    w->type      = WIDGET_ARC;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = ARC_DEFAULT_W;
    w->h         = ARC_DEFAULT_H;
    w->type_data = d;
    snprintf(w->id, sizeof(w->id), "arc_%u", slot);

    w->create        = _arc_create;
    w->resize        = _arc_resize;
    w->open_settings = _arc_open_settings;
    w->to_json       = _arc_to_json;
    w->from_json     = _arc_from_json;
    w->destroy       = _arc_destroy;
    w->apply_overrides = _arc_apply_overrides;

    ESP_LOGI(TAG, "Created arc widget instance (slot %u)", slot);
    return w;
}
