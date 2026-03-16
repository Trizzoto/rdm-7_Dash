/*
 * widget_toggle.c -- Interactive toggle switch widget.
 *
 * Transmits CAN messages on toggle and optionally reads state from a signal.
 */
#include "widget_toggle.h"
#include "widget_rules.h"
#include "can/can_manager.h"
#include "signal.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "widget_types.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_toggle";

#define TOGGLE_DEFAULT_W  80
#define TOGGLE_DEFAULT_H  40

/* ── Default appearance values ──────────────────────────────────────────── */
#define DEF_ACTIVE_COLOR    0x00FF00
#define DEF_INACTIVE_COLOR  0x555555
#define DEF_LABEL_COLOR     0xFFFFFF
#define DEF_BORDER_RADIUS   5
#define DEF_ON_THRESHOLD    0.5f
#define DEF_TX_DLC          8

/* ── Forward declarations ───────────────────────────────────────────────── */
static void _toggle_create(widget_t *w, lv_obj_t *parent);
static void _toggle_resize(widget_t *w, uint16_t nw, uint16_t nh);
static void _toggle_open_settings(widget_t *w);
static void _toggle_to_json(widget_t *w, cJSON *out);
static void _toggle_from_json(widget_t *w, cJSON *in);
static void _toggle_destroy(widget_t *w);

/* ── Signal callback ────────────────────────────────────────────────────── */
static void _toggle_on_signal(float value, bool is_stale, void *user_data) {
    widget_t *w = (widget_t *)user_data;
    if (!w || !w->type_data || !w->root) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;

    if (is_stale || value < d->signal_on_threshold) {
        d->current_state = false;
        if (d->sw_obj && lv_obj_is_valid(d->sw_obj))
            lv_obj_clear_state(d->sw_obj, LV_STATE_CHECKED);
    } else {
        d->current_state = true;
        if (d->sw_obj && lv_obj_is_valid(d->sw_obj))
            lv_obj_add_state(d->sw_obj, LV_STATE_CHECKED);
    }
}

/* ── Toggle clicked event callback ──────────────────────────────────────── */
static void _toggle_clicked_cb(lv_event_t *e) {
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;

    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    d->current_state = checked;

    /* Transmit CAN frame if TX is configured */
    if (d->tx_can_id > 0) {
        if (checked) {
            can_transmit_frame(d->tx_can_id, d->tx_on_data, d->tx_on_dlc);
        } else {
            can_transmit_frame(d->tx_can_id, d->tx_off_data, d->tx_off_dlc);
        }
    }

    ESP_LOGI(TAG, "Toggle %s → %s", w->id, checked ? "ON" : "OFF");
}

/* ── Create ─────────────────────────────────────────────────────────────── */
static void _toggle_create(widget_t *w, lv_obj_t *parent) {
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    if (!d) return;

    /* Root container */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, w->w, w->h);
    lv_obj_set_align(cont, LV_ALIGN_CENTER);
    lv_obj_set_pos(cont, w->x, w->y);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cont, d->border_radius, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Switch */
    lv_obj_t *sw = lv_switch_create(cont);
    d->sw_obj = sw;

    /* Style the switch: unchecked background */
    lv_obj_set_style_bg_color(sw, d->inactive_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* Style the switch: checked indicator background */
    lv_obj_set_style_bg_color(sw, d->active_color,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* If a label is provided, place it beside the switch */
    if (d->label[0] != '\0') {
        lv_obj_t *lbl = lv_label_create(cont);
        lv_label_set_text(lbl, d->label);
        lv_obj_set_style_text_color(lbl, d->label_color,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_MID);
        d->label_obj = lbl;
        /* Place switch above center to leave room for label */
        lv_obj_set_align(sw, LV_ALIGN_TOP_MID);
    } else {
        d->label_obj = NULL;
        lv_obj_set_align(sw, LV_ALIGN_CENTER);
    }

    /* Apply initial state */
    if (d->current_state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    /* Event callback for user toggle interaction */
    lv_obj_add_event_cb(sw, _toggle_clicked_cb, LV_EVENT_VALUE_CHANGED, w);

    w->root = cont;

    /* Subscribe to signal after root is set */
    if (d->signal_index >= 0) {
        signal_subscribe(d->signal_index, _toggle_on_signal, w);
    }
}

/* ── Resize ─────────────────────────────────────────────────────────────── */
static void _toggle_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    w->w = nw;
    w->h = nh;
}

/* ── Open settings (stub) ───────────────────────────────────────────────── */
static void _toggle_open_settings(widget_t *w) { (void)w; }

/* ── Helper: check if all bytes in array are zero ───────────────────────── */
static bool _data_is_zero(const uint8_t *data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        if (data[i] != 0) return false;
    }
    return true;
}

/* ── to_json (defaults-only serialization) ──────────────────────────────── */
static void _toggle_to_json(widget_t *w, cJSON *out) {
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    widget_base_to_json(w, out);
    if (!d) return;

    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;

    cJSON_AddNumberToObject(cfg, "slot", w->slot);

    if (d->label[0] != '\0')
        cJSON_AddStringToObject(cfg, "label", d->label);

    if (d->signal_name[0] != '\0')
        cJSON_AddStringToObject(cfg, "signal_name", d->signal_name);

    if (d->signal_on_threshold != DEF_ON_THRESHOLD)
        cJSON_AddNumberToObject(cfg, "signal_on_threshold", d->signal_on_threshold);

    /* CAN TX */
    if (d->tx_can_id > 0)
        cJSON_AddNumberToObject(cfg, "tx_can_id", d->tx_can_id);

    if (d->tx_on_dlc != DEF_TX_DLC)
        cJSON_AddNumberToObject(cfg, "tx_on_dlc", d->tx_on_dlc);

    if (d->tx_off_dlc != DEF_TX_DLC)
        cJSON_AddNumberToObject(cfg, "tx_off_dlc", d->tx_off_dlc);

    /* TX data arrays: only write if non-zero */
    if (!_data_is_zero(d->tx_on_data, d->tx_on_dlc)) {
        cJSON *arr = cJSON_AddArrayToObject(cfg, "tx_on_data");
        for (int i = 0; i < d->tx_on_dlc; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(d->tx_on_data[i]));
    }

    if (!_data_is_zero(d->tx_off_data, d->tx_off_dlc)) {
        cJSON *arr = cJSON_AddArrayToObject(cfg, "tx_off_data");
        for (int i = 0; i < d->tx_off_dlc; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(d->tx_off_data[i]));
    }

    /* Appearance: only write if different from defaults */
    if (lv_color_to16(d->active_color) != lv_color_to16(lv_color_hex(DEF_ACTIVE_COLOR)))
        cJSON_AddNumberToObject(cfg, "active_color", lv_color_to16(d->active_color));

    if (lv_color_to16(d->inactive_color) != lv_color_to16(lv_color_hex(DEF_INACTIVE_COLOR)))
        cJSON_AddNumberToObject(cfg, "inactive_color", lv_color_to16(d->inactive_color));

    if (lv_color_to16(d->label_color) != lv_color_to16(lv_color_hex(DEF_LABEL_COLOR)))
        cJSON_AddNumberToObject(cfg, "label_color", lv_color_to16(d->label_color));

    if (d->border_radius != DEF_BORDER_RADIUS)
        cJSON_AddNumberToObject(cfg, "border_radius", d->border_radius);
}

/* ── from_json ──────────────────────────────────────────────────────────── */
static void _toggle_from_json(widget_t *w, cJSON *in) {
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!d) return;

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;

    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
    if (cJSON_IsNumber(item)) w->slot = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "label");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(d->label, item->valuestring, sizeof(d->label) - 1);
        d->label[sizeof(d->label) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(d->signal_name, item->valuestring, sizeof(d->signal_name) - 1);
        d->signal_name[sizeof(d->signal_name) - 1] = '\0';
        d->signal_index = signal_find_by_name(d->signal_name);
    }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_on_threshold");
    if (cJSON_IsNumber(item)) d->signal_on_threshold = (float)item->valuedouble;

    /* CAN TX */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_can_id");
    if (cJSON_IsNumber(item)) d->tx_can_id = (uint32_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_on_dlc");
    if (cJSON_IsNumber(item)) d->tx_on_dlc = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_off_dlc");
    if (cJSON_IsNumber(item)) d->tx_off_dlc = (uint8_t)item->valueint;

    /* TX data arrays */
    cJSON *arr;
    arr = cJSON_GetObjectItemCaseSensitive(cfg, "tx_on_data");
    if (cJSON_IsArray(arr)) {
        int i = 0;
        cJSON *byte_val;
        cJSON_ArrayForEach(byte_val, arr) {
            if (i < 8 && cJSON_IsNumber(byte_val))
                d->tx_on_data[i++] = (uint8_t)byte_val->valueint;
        }
    }

    arr = cJSON_GetObjectItemCaseSensitive(cfg, "tx_off_data");
    if (cJSON_IsArray(arr)) {
        int i = 0;
        cJSON *byte_val;
        cJSON_ArrayForEach(byte_val, arr) {
            if (i < 8 && cJSON_IsNumber(byte_val))
                d->tx_off_data[i++] = (uint8_t)byte_val->valueint;
        }
    }

    /* Appearance */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "active_color");
    if (cJSON_IsNumber(item)) d->active_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "inactive_color");
    if (cJSON_IsNumber(item)) d->inactive_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_color");
    if (cJSON_IsNumber(item)) d->label_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "border_radius");
    if (cJSON_IsNumber(item)) d->border_radius = (uint8_t)item->valueint;
}

/* ── Destroy ────────────────────────────────────────────────────────────── */
static void _toggle_destroy(widget_t *w) {
    if (!w) return;
    widget_rules_free(w);
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    if (d) free(d);
    free(w);
}

/* ── Factory ────────────────────────────────────────────────────────────── */
widget_t *widget_toggle_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) return NULL;

    toggle_data_t *d = heap_caps_calloc(1, sizeof(toggle_data_t), MALLOC_CAP_SPIRAM);
    if (!d) d = calloc(1, sizeof(toggle_data_t));
    if (!d) { free(w); return NULL; }

    /* Set defaults */
    d->signal_index        = -1;
    d->signal_on_threshold = DEF_ON_THRESHOLD;
    d->tx_can_id           = 0;
    d->tx_on_dlc           = DEF_TX_DLC;
    d->tx_off_dlc          = DEF_TX_DLC;
    d->active_color        = lv_color_hex(DEF_ACTIVE_COLOR);
    d->inactive_color      = lv_color_hex(DEF_INACTIVE_COLOR);
    d->label_color         = lv_color_hex(DEF_LABEL_COLOR);
    d->border_radius       = DEF_BORDER_RADIUS;
    d->current_state       = false;

    w->type      = WIDGET_TOGGLE;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = TOGGLE_DEFAULT_W;
    w->h         = TOGGLE_DEFAULT_H;
    w->type_data = d;
    snprintf(w->id, sizeof(w->id), "toggle_%u", slot);

    w->create        = _toggle_create;
    w->resize        = _toggle_resize;
    w->open_settings = _toggle_open_settings;
    w->to_json       = _toggle_to_json;
    w->from_json     = _toggle_from_json;
    w->destroy       = _toggle_destroy;

    return w;
}
