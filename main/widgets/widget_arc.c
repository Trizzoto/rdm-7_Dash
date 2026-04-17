/*
 * widget_arc.c -- Arc widget with optional signal binding and image mode.
 *
 * Supports three modes:
 *   1. Static arc (no signal) -- decorative foreground + background arc
 *   2. Signal-bound arc -- indicator value driven by signal percentage
 *   3. Image mode -- track + fill images with clip-based reveal
 *
 * When both arc_image and arc_image_full are set, the LVGL arc is replaced
 * with two images: the track image as background and the fill image
 * progressively revealed left-to-right via a clipping container whose
 * width is set proportional to the signal value.
 */
#include "widget_arc.h"
#include "widget_image.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "signal.h"
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
#define ARC_DEFAULT_SIG_MIN     0.0f
#define ARC_DEFAULT_SIG_MAX     100.0f

/* Forward declarations */
static void _arc_on_signal(float value, bool is_stale, void *user_data);

/* ── Helper: check if arc is in image mode ─────────────────────────────── */

static bool _is_image_mode(const arc_data_t *d) {
    return d->arc_image[0] != '\0' && d->arc_image_full[0] != '\0';
}

static bool _is_static_image_mode(const arc_data_t *d) {
    return d->arc_image[0] != '\0' && d->arc_image_full[0] == '\0';
}

/* ── Helper: update clip width from signal value ───────────────────────── */

static void _update_image_clip(widget_t *w, float value) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d || !d->img_clip_obj) return;

    float range = d->signal_max - d->signal_min;
    if (range <= 0.0f) range = 100.0f;
    float pct = (value - d->signal_min) / range;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    lv_coord_t clip_w = (lv_coord_t)(pct * (float)w->w);
    lv_obj_set_width(d->img_clip_obj, clip_w);
}

/* ── Helper: update standard arc value from signal ─────────────────────── */

static void _update_arc_value(widget_t *w, float value) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d || !d->arc_obj) return;

    float range = d->signal_max - d->signal_min;
    if (range <= 0.0f) range = 100.0f;
    float pct = (value - d->signal_min) / range;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    lv_arc_set_value(d->arc_obj, (int16_t)(pct * 100.0f));
}

/* ── Signal callback ───────────────────────────────────────────────────── */

static void _arc_on_signal(float value, bool is_stale, void *user_data) {
    widget_t *w = (widget_t *)user_data;
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    if (is_stale) {
        if (_is_image_mode(d) && d->img_clip_obj) {
            lv_obj_set_width(d->img_clip_obj, 0);
        } else if (d->arc_obj) {
            lv_arc_set_value(d->arc_obj, 0);
        }
        return;
    }

    if (_is_image_mode(d)) {
        _update_image_clip(w, value);
    } else if (d->arc_obj) {
        _update_arc_value(w, value);
    }
}

/* ── Create: image mode ────────────────────────────────────────────────── */

static void _arc_create_image_mode(widget_t *w, lv_obj_t *parent) {
    arc_data_t *d = (arc_data_t *)w->type_data;

    /* Create a transparent container as root */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, w->w, w->h);
    lv_obj_set_align(cont, LV_ALIGN_CENTER);
    lv_obj_set_pos(cont, w->x, w->y);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);

    /* Load track (background) image */
    d->arc_img_dsc = rdm_image_load(d->arc_image);
    if (d->arc_img_dsc) {
        d->img_bg_obj = lv_img_create(cont);
        lv_img_set_src(d->img_bg_obj, d->arc_img_dsc);
        lv_obj_set_align(d->img_bg_obj, LV_ALIGN_CENTER);
    } else {
        ESP_LOGW(TAG, "Failed to load track image '%s'", d->arc_image);
    }

    /* Load fill image */
    d->arc_img_full_dsc = rdm_image_load(d->arc_image_full);
    if (d->arc_img_full_dsc) {
        /* Create clip container -- starts at width 0 (empty) */
        d->img_clip_obj = lv_obj_create(cont);
        lv_obj_set_size(d->img_clip_obj, 0, w->h);
        lv_obj_set_align(d->img_clip_obj, LV_ALIGN_LEFT_MID);
        lv_obj_clear_flag(d->img_clip_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(d->img_clip_obj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(d->img_clip_obj, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(d->img_clip_obj, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(d->img_clip_obj, 0, LV_PART_MAIN);

        /* Full image inside the clip container, aligned to left so it
         * gets progressively revealed as clip container width grows */
        d->img_full_obj = lv_img_create(d->img_clip_obj);
        lv_img_set_src(d->img_full_obj, d->arc_img_full_dsc);
        lv_obj_set_align(d->img_full_obj, LV_ALIGN_LEFT_MID);
    } else {
        ESP_LOGW(TAG, "Failed to load fill image '%s'", d->arc_image_full);
    }

    w->root = cont;
}

/* ── Create: static image mode (track only, no fill) ───────────────────── */

static void _arc_create_static_image(widget_t *w, lv_obj_t *parent) {
    arc_data_t *d = (arc_data_t *)w->type_data;

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, w->w, w->h);
    lv_obj_set_align(cont, LV_ALIGN_CENTER);
    lv_obj_set_pos(cont, w->x, w->y);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);

    d->arc_img_dsc = rdm_image_load(d->arc_image);
    if (d->arc_img_dsc) {
        d->img_bg_obj = lv_img_create(cont);
        lv_img_set_src(d->img_bg_obj, d->arc_img_dsc);
        lv_obj_set_align(d->img_bg_obj, LV_ALIGN_CENTER);
    } else {
        lv_obj_t *lbl = lv_label_create(cont);
        lv_label_set_text(lbl, d->arc_image);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), LV_PART_MAIN);
    }

    w->root = cont;
}

/* ── Create: standard arc mode ─────────────────────────────────────────── */

static void _arc_create_standard(widget_t *w, lv_obj_t *parent) {
    arc_data_t *d = (arc_data_t *)w->type_data;

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

    /* Foreground/indicator arc (LV_PART_INDICATOR) */
    lv_arc_set_range(obj, 0, 100);
    if (d->signal_index >= 0) {
        /* Signal-bound: start at 0, signal callback will update */
        lv_arc_set_value(obj, 0);
    } else {
        /* Static: fill completely */
        lv_arc_set_value(obj, 100);
    }
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

/* ── Vtable: create ────────────────────────────────────────────────────── */

/* Forward declarations — night mode hooks used in _arc_create / _arc_destroy */
static void _arc_apply_night_mode(widget_t *w, bool active);
static void _arc_night_cb(bool active, void *user_data);

static void _arc_create(widget_t *w, lv_obj_t *parent) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    if (_is_image_mode(d)) {
        _arc_create_image_mode(w, parent);
    } else if (_is_static_image_mode(d)) {
        _arc_create_static_image(w, parent);
    } else {
        _arc_create_standard(w, parent);
    }

    /* Subscribe to signal after w->root is set */
    if (d->signal_index >= 0)
        signal_subscribe(d->signal_index, _arc_on_signal, w);

    /* Subscribe rules (safe no-op if no rules defined) */
    widget_rules_subscribe(w);

    /* Subscribe to night-mode changes if any night override is set, and apply
     * current state immediately so the widget renders correctly even if it
     * was created while night-mode is already active. */
    if (d->night.has_arc_color || d->night.has_bg_arc_color ||
        d->night.has_arc_image || d->night.has_arc_image_full) {
        night_mode_subscribe(_arc_night_cb, w);
        _arc_apply_night_mode(w, night_mode_is_active());
    }
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

    /* Standard arc fields -- defaults-only */
    if (d->start_angle != ARC_DEFAULT_START)
        cJSON_AddNumberToObject(cfg, "start_angle", d->start_angle);
    if (d->end_angle != ARC_DEFAULT_END)
        cJSON_AddNumberToObject(cfg, "end_angle", d->end_angle);
    if (d->arc_width != ARC_DEFAULT_WIDTH)
        cJSON_AddNumberToObject(cfg, "arc_width", d->arc_width);
    if (d->arc_color.full != lv_color_hex(ARC_DEFAULT_COLOR).full)
        cJSON_AddNumberToObject(cfg, "arc_color", (int)d->arc_color.full);
    if (d->bg_arc_color.full != lv_color_hex(ARC_DEFAULT_BG_COLOR).full)
        cJSON_AddNumberToObject(cfg, "bg_arc_color", (int)d->bg_arc_color.full);
    if (d->bg_arc_width != ARC_DEFAULT_BG_WIDTH)
        cJSON_AddNumberToObject(cfg, "bg_arc_width", d->bg_arc_width);
    if (d->rounded_ends != ARC_DEFAULT_ROUNDED)
        cJSON_AddBoolToObject(cfg, "rounded_ends", d->rounded_ends);

    /* Signal binding */
    if (d->signal_name[0] != '\0')
        cJSON_AddStringToObject(cfg, "signal_name", d->signal_name);
    if (d->signal_min != ARC_DEFAULT_SIG_MIN)
        cJSON_AddNumberToObject(cfg, "signal_min", (double)d->signal_min);
    if (d->signal_max != ARC_DEFAULT_SIG_MAX)
        cJSON_AddNumberToObject(cfg, "signal_max", (double)d->signal_max);

    /* Image mode */
    if (d->arc_image[0] != '\0')
        cJSON_AddStringToObject(cfg, "arc_image", d->arc_image);
    if (d->arc_image_full[0] != '\0')
        cJSON_AddStringToObject(cfg, "arc_image_full", d->arc_image_full);

    /* Rules */
    widget_rules_to_json(w, cfg);

    /* Night-mode overrides — emit only fields that have an override set */
    {
        cJSON *n = cJSON_CreateObject();
        NIGHT_SERIALIZE_COLOR(n, d->night, arc_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, bg_arc_color);
        NIGHT_SERIALIZE_IMAGE(n, d->night, arc_image);
        NIGHT_SERIALIZE_IMAGE(n, d->night, arc_image_full);
        if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
        else cJSON_Delete(n);
    }
}

static void _arc_from_json(widget_t *w, cJSON *in) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!d) return;

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;

    cJSON *item;

    /* Standard arc fields */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "start_angle");
    if (cJSON_IsNumber(item)) d->start_angle = (int16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "end_angle");
    if (cJSON_IsNumber(item)) d->end_angle = (int16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_width");
    if (cJSON_IsNumber(item)) d->arc_width = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_color");
    if (cJSON_IsNumber(item)) d->arc_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_arc_color");
    if (cJSON_IsNumber(item)) d->bg_arc_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_arc_width");
    if (cJSON_IsNumber(item)) d->bg_arc_width = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "rounded_ends");
    if (cJSON_IsBool(item)) d->rounded_ends = cJSON_IsTrue(item);

    /* Signal binding */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(d->signal_name, item->valuestring, sizeof(d->signal_name));

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_min");
    if (cJSON_IsNumber(item)) d->signal_min = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_max");
    if (cJSON_IsNumber(item)) d->signal_max = (float)item->valuedouble;

    /* Image mode */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_image");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(d->arc_image, item->valuestring, sizeof(d->arc_image));

    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_image_full");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(d->arc_image_full, item->valuestring, sizeof(d->arc_image_full));

    /* Resolve signal name to index */
    if (d->signal_name[0] != '\0')
        d->signal_index = signal_find_by_name(d->signal_name);

    /* Rules */
    widget_rules_from_json(w, cfg);

    /* Night-mode overrides */
    cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
    if (cJSON_IsObject(night)) {
        NIGHT_PARSE_COLOR(night, d->night, arc_color);
        NIGHT_PARSE_COLOR(night, d->night, bg_arc_color);
        NIGHT_PARSE_IMAGE(night, d->night, arc_image);
        NIGHT_PARSE_IMAGE(night, d->night, arc_image_full);
    }
}

static void _arc_destroy(widget_t *w) {
    if (!w) return;
    arc_data_t *d = (arc_data_t *)w->type_data;

    /* Unsubscribe signal before deleting LVGL objects */
    if (d && d->signal_index >= 0)
        signal_unsubscribe(d->signal_index, _arc_on_signal, w);

    night_mode_unsubscribe(_arc_night_cb, w);

    widget_rules_free(w);

    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;

    if (d) {
        rdm_image_free(d->arc_img_dsc);
        rdm_image_free(d->arc_img_full_dsc);
        free(d);
    }
    free(w);
}

/* ── apply_overrides ────────────────────────────────────────────────────── */

static void _arc_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    /* Overrides only apply to standard arc mode */
    if (!d->arc_obj) return;

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

/* ── Night-mode apply ───────────────────────────────────────────────────── */
/* Re-apply arc colors (and image swaps, where feasible) based on current
 * night-mode state. Image swap behaviour:
 *   - In standard arc mode (no images): only arc_color and bg_arc_color apply.
 *   - In image mode: we attempt to reload the track/fill image descriptors
 *     from the night override names; LVGL source pointers are updated. Colors
 *     are not meaningful in image mode (the arc LVGL object is not used). */
static void _arc_apply_night_mode(widget_t *w, bool active) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    /* Colors — apply to standard arc mode */
    if (d->arc_obj && lv_obj_is_valid(d->arc_obj)) {
        lv_color_t fg = NIGHT_PICK_COLOR(active, d->night, arc_color,    d->arc_color);
        lv_color_t bg = NIGHT_PICK_COLOR(active, d->night, bg_arc_color, d->bg_arc_color);
        lv_obj_set_style_arc_color(d->arc_obj, fg, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(d->arc_obj, bg, LV_PART_MAIN);
    }

    /* Image swap — only meaningful in image mode. Reload the descriptor
     * using the night-picked image name and swap the LVGL source. */
    if (d->img_bg_obj && lv_obj_is_valid(d->img_bg_obj)) {
        const char *bg_name = NIGHT_PICK_IMAGE(active, d->night, arc_image, d->arc_image);
        if (bg_name && bg_name[0] != '\0') {
            lv_img_dsc_t *new_dsc = rdm_image_load(bg_name);
            if (new_dsc) {
                lv_img_set_src(d->img_bg_obj, new_dsc);
                rdm_image_free(d->arc_img_dsc);
                d->arc_img_dsc = new_dsc;
            }
        }
    }
    if (d->img_full_obj && lv_obj_is_valid(d->img_full_obj)) {
        const char *full_name = NIGHT_PICK_IMAGE(active, d->night, arc_image_full, d->arc_image_full);
        if (full_name && full_name[0] != '\0') {
            lv_img_dsc_t *new_dsc = rdm_image_load(full_name);
            if (new_dsc) {
                lv_img_set_src(d->img_full_obj, new_dsc);
                rdm_image_free(d->arc_img_full_dsc);
                d->arc_img_full_dsc = new_dsc;
            }
        }
    }
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _arc_night_cb(bool active, void *user_data) {
    _arc_apply_night_mode((widget_t *)user_data, active);
}

widget_t *widget_arc_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) return NULL;

    arc_data_t *d = heap_caps_calloc(1, sizeof(arc_data_t), MALLOC_CAP_SPIRAM);
    if (!d) d = calloc(1, sizeof(arc_data_t));
    if (!d) { free(w); return NULL; }

    /* Set defaults */
    d->start_angle   = ARC_DEFAULT_START;
    d->end_angle     = ARC_DEFAULT_END;
    d->arc_width     = ARC_DEFAULT_WIDTH;
    d->arc_color     = lv_color_hex(ARC_DEFAULT_COLOR);
    d->bg_arc_color  = lv_color_hex(ARC_DEFAULT_BG_COLOR);
    d->bg_arc_width  = ARC_DEFAULT_BG_WIDTH;
    d->rounded_ends  = ARC_DEFAULT_ROUNDED;
    d->arc_obj       = NULL;
    d->signal_index  = -1;
    d->signal_min    = ARC_DEFAULT_SIG_MIN;
    d->signal_max    = ARC_DEFAULT_SIG_MAX;
    /* arc_image, arc_image_full, signal_name: zeroed by calloc */

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
    w->apply_night_mode = _arc_apply_night_mode;

    ESP_LOGI(TAG, "Created arc widget instance (slot %u)", slot);
    return w;
}
