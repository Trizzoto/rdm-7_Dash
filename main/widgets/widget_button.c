/*
 * widget_button.c -- Momentary push-button widget that transmits CAN messages.
 *
 * Sends a CAN frame on press (always, when configured) and optionally a
 * second frame on release.  Visual feedback via a separate pressed-state
 * background color.
 */
#include "widget_button.h"
#include "widget_rules.h"
#include "can/can_manager.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "widget_types.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_button";

#define BUTTON_DEFAULT_W 100
#define BUTTON_DEFAULT_H  40

/* ── Defaults ─────────────────────────────────────────────────────────────── */

#define DEF_LABEL        "BTN"
#define DEF_TX_CAN_ID    0
#define DEF_PRESS_DLC    8
#define DEF_RELEASE_DLC  0
#define DEF_BG_COLOR     0x333333
#define DEF_TEXT_COLOR    0xFFFFFF
#define DEF_PRESSED_COLOR 0x555555
#define DEF_BORDER_RADIUS 5

/* ── LVGL callbacks ───────────────────────────────────────────────────────── */

static void _btn_pressed_cb(lv_event_t *e) {
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    button_data_t *d = (button_data_t *)w->type_data;

    if (d->tx_can_id > 0 && d->tx_press_dlc > 0) {
        esp_err_t err = can_transmit_frame(d->tx_can_id, d->tx_press_data, d->tx_press_dlc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Press TX failed (id=0x%03X): %s",
                     (unsigned)d->tx_can_id, esp_err_to_name(err));
        }
    }
}

static void _btn_released_cb(lv_event_t *e) {
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    button_data_t *d = (button_data_t *)w->type_data;

    if (d->tx_can_id > 0 && d->tx_release_dlc > 0) {
        esp_err_t err = can_transmit_frame(d->tx_can_id, d->tx_release_data, d->tx_release_dlc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Release TX failed (id=0x%03X): %s",
                     (unsigned)d->tx_can_id, esp_err_to_name(err));
        }
    }
}

/* ── vtable: create ───────────────────────────────────────────────────────── */

static void _button_create(widget_t *w, lv_obj_t *parent) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d) return;

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w->w, w->h);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_set_pos(btn, w->x, w->y);

    /* Normal state style */
    lv_obj_set_style_bg_color(btn, d->bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, d->border_radius, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Pressed state style */
    lv_obj_set_style_bg_color(btn, d->pressed_color, LV_PART_MAIN | LV_STATE_PRESSED);

    /* Label */
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_align(lbl, LV_ALIGN_CENTER);
    lv_label_set_text(lbl, d->label);
    lv_obj_set_style_text_color(lbl, d->text_color, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Resolve custom font */
    if (d->font[0] != '\0') {
        const lv_font_t *f = widget_resolve_font(d->font);
        if (f) {
            lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }

    /* Event callbacks */
    lv_obj_add_event_cb(btn, _btn_pressed_cb,  LV_EVENT_PRESSED,  w);
    lv_obj_add_event_cb(btn, _btn_released_cb, LV_EVENT_RELEASED, w);

    d->btn_obj   = btn;
    d->label_obj = lbl;
    w->root      = btn;
}

/* ── vtable: resize ───────────────────────────────────────────────────────── */

static void _button_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    w->w = nw;
    w->h = nh;
}

/* ── vtable: open_settings ────────────────────────────────────────────────── */

static void _button_open_settings(widget_t *w) { (void)w; }

/* ── vtable: to_json ──────────────────────────────────────────────────────── */

static bool _data_array_nonzero(const uint8_t *data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        if (data[i] != 0) return true;
    }
    return false;
}

static cJSON *_make_byte_array(const uint8_t *data, uint8_t len) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (uint8_t i = 0; i < len; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(data[i]));
    }
    return arr;
}

static void _button_to_json(widget_t *w, cJSON *out) {
    button_data_t *d = (button_data_t *)w->type_data;
    widget_base_to_json(w, out);
    if (!d) return;

    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;

    cJSON_AddNumberToObject(cfg, "slot", w->slot);

    /* Label -- always write (no sensible "empty" default) */
    if (strcmp(d->label, DEF_LABEL) != 0)
        cJSON_AddStringToObject(cfg, "label", d->label);

    /* CAN TX config */
    if (d->tx_can_id != DEF_TX_CAN_ID)
        cJSON_AddNumberToObject(cfg, "tx_can_id", d->tx_can_id);
    if (d->tx_press_dlc != DEF_PRESS_DLC)
        cJSON_AddNumberToObject(cfg, "tx_press_dlc", d->tx_press_dlc);
    if (_data_array_nonzero(d->tx_press_data, 8)) {
        cJSON *arr = _make_byte_array(d->tx_press_data, 8);
        if (arr) cJSON_AddItemToObject(cfg, "tx_press_data", arr);
    }
    if (d->tx_release_dlc != DEF_RELEASE_DLC)
        cJSON_AddNumberToObject(cfg, "tx_release_dlc", d->tx_release_dlc);
    if (d->tx_release_dlc > 0) {
        cJSON *arr = _make_byte_array(d->tx_release_data, 8);
        if (arr) cJSON_AddItemToObject(cfg, "tx_release_data", arr);
    }

    /* Appearance -- defaults-only */
    if (lv_color_to16(d->bg_color) != lv_color_to16(lv_color_hex(DEF_BG_COLOR)))
        cJSON_AddNumberToObject(cfg, "bg_color", lv_color_to16(d->bg_color));
    if (lv_color_to16(d->text_color) != lv_color_to16(lv_color_hex(DEF_TEXT_COLOR)))
        cJSON_AddNumberToObject(cfg, "text_color", lv_color_to16(d->text_color));
    if (lv_color_to16(d->pressed_color) != lv_color_to16(lv_color_hex(DEF_PRESSED_COLOR)))
        cJSON_AddNumberToObject(cfg, "pressed_color", lv_color_to16(d->pressed_color));
    if (d->border_radius != DEF_BORDER_RADIUS)
        cJSON_AddNumberToObject(cfg, "border_radius", d->border_radius);
    if (d->font[0] != '\0')
        cJSON_AddStringToObject(cfg, "font", d->font);
}

/* ── vtable: from_json ────────────────────────────────────────────────────── */

static void _parse_byte_array(cJSON *arr, uint8_t *dst, uint8_t max_len) {
    if (!cJSON_IsArray(arr)) return;
    int count = cJSON_GetArraySize(arr);
    if (count > max_len) count = max_len;
    for (int i = 0; i < count; i++) {
        cJSON *elem = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsNumber(elem))
            dst[i] = (uint8_t)elem->valueint;
    }
}

static void _button_from_json(widget_t *w, cJSON *in) {
    button_data_t *d = (button_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!d) return;

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;

    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
    if (cJSON_IsNumber(item)) w->slot = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "label");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->label, item->valuestring, sizeof(d->label));
    }

    /* CAN TX */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_can_id");
    if (cJSON_IsNumber(item)) d->tx_can_id = (uint32_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_press_dlc");
    if (cJSON_IsNumber(item)) { d->tx_press_dlc = (uint8_t)item->valueint; if (d->tx_press_dlc > 8) d->tx_press_dlc = 8; }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_press_data");
    _parse_byte_array(item, d->tx_press_data, 8);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_release_dlc");
    if (cJSON_IsNumber(item)) { d->tx_release_dlc = (uint8_t)item->valueint; if (d->tx_release_dlc > 8) d->tx_release_dlc = 8; }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_release_data");
    _parse_byte_array(item, d->tx_release_data, 8);

    /* Appearance */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_color");
    if (cJSON_IsNumber(item)) d->bg_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "text_color");
    if (cJSON_IsNumber(item)) d->text_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "pressed_color");
    if (cJSON_IsNumber(item)) d->pressed_color = lv_color_hex(item->valueint);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "border_radius");
    if (cJSON_IsNumber(item)) d->border_radius = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "font");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->font, item->valuestring, sizeof(d->font));
    }
}

/* ── vtable: destroy ──────────────────────────────────────────────────────── */

static void _button_destroy(widget_t *w) {
    if (!w) return;
    widget_rules_free(w);
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    if (w->type_data) free(w->type_data);
    free(w);
}

/* ── Factory ──────────────────────────────────────────────────────────────── */

widget_t *widget_button_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) return NULL;

    button_data_t *d = heap_caps_calloc(1, sizeof(button_data_t), MALLOC_CAP_SPIRAM);
    if (!d) d = calloc(1, sizeof(button_data_t));
    if (!d) { free(w); return NULL; }

    /* Set defaults */
    safe_strncpy(d->label, DEF_LABEL, sizeof(d->label));
    d->tx_can_id       = DEF_TX_CAN_ID;
    d->tx_press_dlc    = DEF_PRESS_DLC;
    d->tx_release_dlc  = DEF_RELEASE_DLC;
    d->bg_color        = lv_color_hex(DEF_BG_COLOR);
    d->text_color      = lv_color_hex(DEF_TEXT_COLOR);
    d->pressed_color   = lv_color_hex(DEF_PRESSED_COLOR);
    d->border_radius   = DEF_BORDER_RADIUS;
    /* d->font left as "" (calloc zeroed) */

    w->type      = WIDGET_BUTTON;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = BUTTON_DEFAULT_W;
    w->h         = BUTTON_DEFAULT_H;
    w->type_data = d;
    snprintf(w->id, sizeof(w->id), "button_%u", slot);

    w->create        = _button_create;
    w->resize        = _button_resize;
    w->open_settings = _button_open_settings;
    w->to_json       = _button_to_json;
    w->from_json     = _button_from_json;
    w->destroy       = _button_destroy;

    ESP_LOGI(TAG, "Created button instance slot=%u", slot);
    return w;
}
