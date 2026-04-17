/*
 * widget_night_helpers.h — boilerplate-killing macros for night-mode overrides.
 *
 * Each widget that supports night-mode overrides defines a small
 * `<widget>_night_overrides_t` struct using NIGHT_FIELD_COLOR / NIGHT_FIELD_IMAGE
 * for each overridable field. Then the widget's _from_json / _to_json /
 * _apply_night_mode use the matching PARSE / SERIALIZE / APPLY macros to keep
 * the per-widget code tiny.
 *
 * Field naming convention: the struct member name MUST match the corresponding
 * field name in the regular widget data struct. So a panel's `value_color`
 * (in panel_data_t) has a night override stored as `night.value_color`
 * (in panel_night_overrides_t), and the JSON path is `config.night.value_color`.
 *
 * Example use in a widget header:
 *
 *   typedef struct {
 *       NIGHT_FIELD_COLOR(value_color)
 *       NIGHT_FIELD_COLOR(label_color)
 *       NIGHT_FIELD_COLOR(border_color)
 *       NIGHT_FIELD_IMAGE(image_name, 32)
 *   } panel_night_overrides_t;
 *
 *   typedef struct {
 *       // ... regular fields
 *       panel_night_overrides_t night;
 *   } panel_data_t;
 *
 * In the widget .c file:
 *
 *   // _from_json:
 *   cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
 *   if (night) {
 *       NIGHT_PARSE_COLOR(night, pd->night, value_color);
 *       NIGHT_PARSE_COLOR(night, pd->night, label_color);
 *       NIGHT_PARSE_IMAGE(night, pd->night, image_name);
 *   }
 *
 *   // _to_json:
 *   cJSON *n = cJSON_CreateObject();
 *   NIGHT_SERIALIZE_COLOR(n, pd->night, value_color);
 *   NIGHT_SERIALIZE_COLOR(n, pd->night, label_color);
 *   NIGHT_SERIALIZE_IMAGE(n, pd->night, image_name);
 *   if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
 *   else cJSON_Delete(n);
 *
 * Helper at the bottom: NIGHT_HAS_ANY_OVERRIDE(struct) returns true if any
 * has_X flag is set — used by widget_create() to decide whether to subscribe.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lvgl.h"
#include "cJSON.h"

/* ─── Field declaration macros (use inside a struct) ─────────────────────── */

#define NIGHT_FIELD_COLOR(name)        bool has_##name; lv_color_t name;
#define NIGHT_FIELD_IMAGE(name, sz)    bool has_##name; char       name[sz];

/* ─── JSON parse macros (use in _from_json) ──────────────────────────────── */

#define NIGHT_PARSE_COLOR(src_obj, dest, field)                                 \
    do {                                                                       \
        cJSON *_v = cJSON_GetObjectItemCaseSensitive((src_obj), #field);       \
        if (cJSON_IsNumber(_v)) {                                              \
            (dest).has_##field = true;                                         \
            (dest).field.full  = (uint32_t)_v->valueint;                       \
        }                                                                      \
    } while (0)

#define NIGHT_PARSE_IMAGE(src_obj, dest, field)                                 \
    do {                                                                       \
        cJSON *_v = cJSON_GetObjectItemCaseSensitive((src_obj), #field);       \
        if (cJSON_IsString(_v) && _v->valuestring) {                           \
            (dest).has_##field = true;                                         \
            size_t _sz = sizeof((dest).field);                                 \
            strncpy((dest).field, _v->valuestring, _sz - 1);                   \
            (dest).field[_sz - 1] = '\0';                                      \
        }                                                                      \
    } while (0)

/* ─── JSON serialize macros (use in _to_json) ────────────────────────────── */

#define NIGHT_SERIALIZE_COLOR(out_obj, src, field)                              \
    do {                                                                       \
        if ((src).has_##field) {                                               \
            cJSON_AddNumberToObject((out_obj), #field,                         \
                                    (int)(src).field.full);                    \
        }                                                                      \
    } while (0)

#define NIGHT_SERIALIZE_IMAGE(out_obj, src, field)                              \
    do {                                                                       \
        if ((src).has_##field) {                                               \
            cJSON_AddStringToObject((out_obj), #field, (src).field);           \
        }                                                                      \
    } while (0)

/* ─── Apply helpers ──────────────────────────────────────────────────────── */

/**
 * Pick day or night value for a color field given current night mode state
 * and the widget's day value. Returns the night value if active && has_X,
 * else the day value.
 */
#define NIGHT_PICK_COLOR(active, src, field, day_value)                         \
    (((active) && (src).has_##field) ? (src).field : (day_value))

/**
 * Pick day or night value for an image-name string field. Returns a const
 * char* to the night image name if active && has_X, else the day name.
 */
#define NIGHT_PICK_IMAGE(active, src, field, day_value)                         \
    (((active) && (src).has_##field) ? (src).field : (day_value))
