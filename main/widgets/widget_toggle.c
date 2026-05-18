/*
 * widget_toggle.c -- Interactive toggle switch widget.
 *
 * Transmits CAN messages on toggle and optionally reads state from a signal.
 */
#include "widget_toggle.h"
#include "widget_image.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "ui/menu/edit_mode.h"
#include "can/can_decode.h"
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
#define DEF_ACTIVE_OPA      255
#define DEF_INACTIVE_OPA    100
#define DEF_ON_THRESHOLD    0.5f
#define DEF_MOMENTARY       false
#define DEF_TX_BIT_START    0
#define DEF_TX_BIT_LENGTH   1
#define DEF_TX_ENDIAN       1
#define DEF_TX_RATE_HZ      10
#define DEF_LABEL_ALIGN     1   /* center */
#define DEF_SHOW_LABEL      true

static lv_text_align_t _to_lv_align(uint8_t a) {
    if (a == 0) return LV_TEXT_ALIGN_LEFT;
    if (a == 2) return LV_TEXT_ALIGN_RIGHT;
    return LV_TEXT_ALIGN_CENTER;
}

/* ── Forward declarations ───────────────────────────────────────────────── */
static void _toggle_create(widget_t *w, lv_obj_t *parent);
static void _toggle_resize(widget_t *w, uint16_t nw, uint16_t nh);
static void _toggle_open_settings(widget_t *w);
static void _toggle_to_json(widget_t *w, cJSON *out);
static void _toggle_from_json(widget_t *w, cJSON *in);
static void _toggle_destroy(widget_t *w);
static void _toggle_apply_night_mode(widget_t *w, bool active);
static void _toggle_night_cb(bool active, void *user_data);
static void _toggle_momentary_pressed_cb(lv_event_t *e);
static void _toggle_momentary_released_cb(lv_event_t *e);

/* ── Helper: apply image styling based on current state ─────────────────── */
static void _toggle_apply_image_state(toggle_data_t *d) {
    if (!d->img_obj || !lv_obj_is_valid(d->img_obj)) return;
    if (d->current_state) {
        lv_obj_set_style_img_recolor(d->img_obj, d->active_color,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(d->img_obj, LV_OPA_COVER,
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_opa(d->img_obj, d->active_opa,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_img_recolor(d->img_obj, d->inactive_color,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(d->img_obj, LV_OPA_COVER,
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_opa(d->img_obj, d->inactive_opa,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

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

    /* Update image styling if in image mode */
    _toggle_apply_image_state(d);
}

/* ── Periodic TX timer ─────────────────────────────────────────────────── */

static void _toggle_tx_timer_cb(lv_timer_t *t) {
    widget_t *w = (widget_t *)t->user_data;
    if (!w || !w->type_data) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    if (!d->current_state || d->tx_can_id == 0) return;
    uint8_t frame[8] = {0};
    uint32_t val = (d->tx_bit_length >= 32) ? 0xFFFFFFFFu : ((1u << d->tx_bit_length) - 1u);
    can_pack_bits(frame, d->tx_bit_start, d->tx_bit_length, val, d->tx_endian);
    can_transmit_frame(d->tx_can_id, frame, 8);
}

static void _toggle_start_tx_timer(widget_t *w) {
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    if (!d || d->tx_timer || d->tx_can_id == 0 || d->tx_rate_hz == 0) return;
    uint32_t period = 1000 / d->tx_rate_hz;
    d->tx_timer = lv_timer_create(_toggle_tx_timer_cb, period, w);
}

static void _toggle_stop_tx_timer(toggle_data_t *d) {
    if (d->tx_timer) {
        lv_timer_del(d->tx_timer);
        d->tx_timer = NULL;
    }
}

/* ── Toggle clicked event callback ──────────────────────────────────────── */
static void _toggle_clicked_cb(lv_event_t *e) {
    /* Suspend state changes + TX while Edit Mode is armed. */
    if (edit_mode_is_armed()) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;

    bool checked;
    if (d->img_obj) {
        /* Image mode: manually toggle state */
        d->current_state = !d->current_state;
        checked = d->current_state;
    } else {
        /* Switch mode: read state from the switch widget */
        lv_obj_t *sw = lv_event_get_target(e);
        checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
        d->current_state = checked;
    }

    /* Transmit CAN frame if TX is configured.
     * ON = all bits set, OFF = 0. */
    if (d->tx_can_id > 0) {
        uint8_t frame[8] = {0};
        uint32_t val = checked ? (d->tx_bit_length >= 32 ? 0xFFFFFFFFu : ((1u << d->tx_bit_length) - 1u)) : 0u;
        can_pack_bits(frame, d->tx_bit_start, d->tx_bit_length, val, d->tx_endian);
        can_transmit_frame(d->tx_can_id, frame, 8);
    }

    /* Start/stop periodic TX timer based on toggle state */
    if (checked)
        _toggle_start_tx_timer(w);
    else
        _toggle_stop_tx_timer(d);

    /* Update image styling if in image mode */
    _toggle_apply_image_state(d);

    ESP_LOGI(TAG, "Toggle %s → %s", w->id, checked ? "ON" : "OFF");
}

/* ── Momentary press/release callbacks ──────────────────────────────────── */

static void _toggle_momentary_pressed_cb(lv_event_t *e) {
    if (edit_mode_is_armed()) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;

    d->current_state = true;
    if (d->sw_obj && lv_obj_is_valid(d->sw_obj))
        lv_obj_add_state(d->sw_obj, LV_STATE_CHECKED);

    if (d->tx_can_id > 0) {
        uint8_t frame[8] = {0};
        uint32_t val = (d->tx_bit_length >= 32) ? 0xFFFFFFFFu : ((1u << d->tx_bit_length) - 1u);
        can_pack_bits(frame, d->tx_bit_start, d->tx_bit_length, val, d->tx_endian);
        can_transmit_frame(d->tx_can_id, frame, 8);
    }
    _toggle_start_tx_timer(w);
    _toggle_apply_image_state(d);
}

static void _toggle_momentary_released_cb(lv_event_t *e) {
    if (edit_mode_is_armed()) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;

    _toggle_stop_tx_timer(d);
    d->current_state = false;
    if (d->sw_obj && lv_obj_is_valid(d->sw_obj))
        lv_obj_clear_state(d->sw_obj, LV_STATE_CHECKED);

    if (d->tx_can_id > 0) {
        uint8_t frame[8] = {0};
        can_pack_bits(frame, d->tx_bit_start, d->tx_bit_length, 0u, d->tx_endian);
        can_transmit_frame(d->tx_can_id, frame, 8);
    }
    _toggle_apply_image_state(d);
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
    lv_obj_set_style_radius(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Image mode: if image_name is set, create an image instead of a switch */
    if (d->image_name[0] != '\0') {
        lv_img_dsc_t *dsc = rdm_image_load(d->image_name);
        d->img_dsc = dsc;
        if (dsc) {
            lv_obj_t *img = lv_img_create(cont);
            lv_img_set_src(img, dsc);
            lv_obj_set_align(img, LV_ALIGN_CENTER);
            d->img_obj = img;

            /* Apply initial color tint and opacity based on state */
            _toggle_apply_image_state(d);

            /* Make the image clickable to toggle state */
            lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
            if (d->momentary) {
                lv_obj_add_event_cb(img, _toggle_momentary_pressed_cb,  LV_EVENT_PRESSED,  w);
                lv_obj_add_event_cb(img, _toggle_momentary_released_cb, LV_EVENT_RELEASED, w);
            } else {
                lv_obj_add_event_cb(img, _toggle_clicked_cb, LV_EVENT_CLICKED, w);
            }
        } else {
            /* Image not found: show placeholder label */
            lv_obj_t *lbl = lv_label_create(cont);
            lv_label_set_text(lbl, d->image_name);
            lv_obj_set_align(lbl, LV_ALIGN_CENTER);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        /* Label overlay on image (optional) */
        if (d->show_label && d->label[0] != '\0') {
            lv_obj_t *lbl = lv_label_create(cont);
            lv_label_set_text(lbl, d->label);
            lv_obj_set_style_text_color(lbl, d->label_color,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_width(lbl, w->w);
            lv_obj_set_style_text_align(lbl, _to_lv_align(d->label_align), LV_PART_MAIN | LV_STATE_DEFAULT);
            if (d->font[0] != '\0') {
                const lv_font_t *f = widget_resolve_font(d->font);
                if (f) lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
            if (d->label_x != 0 || d->label_y != 0) {
                lv_obj_set_align(lbl, LV_ALIGN_CENTER);
                lv_obj_set_pos(lbl, d->label_x, d->label_y);
            } else {
                lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_MID);
            }
            d->label_obj = lbl;
        } else {
            d->label_obj = NULL;
        }
        d->sw_obj = NULL;
    } else {
        /* Switch mode (original behavior) */
        lv_obj_t *sw = lv_switch_create(cont);
        d->sw_obj = sw;
        d->img_obj = NULL;
        d->img_dsc = NULL;

        /* Style the switch: unchecked background */
        lv_obj_set_style_bg_color(sw, d->inactive_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        /* Style the switch: checked indicator background */
        lv_obj_set_style_bg_color(sw, d->active_color,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);

        /* If a label is provided and visible, place it beside the switch */
        if (d->show_label && d->label[0] != '\0') {
            lv_obj_t *lbl = lv_label_create(cont);
            lv_label_set_text(lbl, d->label);
            lv_obj_set_style_text_color(lbl, d->label_color,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_width(lbl, w->w);
            lv_obj_set_style_text_align(lbl, _to_lv_align(d->label_align), LV_PART_MAIN | LV_STATE_DEFAULT);
            if (d->font[0] != '\0') {
                const lv_font_t *f = widget_resolve_font(d->font);
                if (f) lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
            if (d->label_x != 0 || d->label_y != 0) {
                lv_obj_set_align(lbl, LV_ALIGN_CENTER);
                lv_obj_set_pos(lbl, d->label_x, d->label_y);
            } else {
                lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_MID);
            }
            d->label_obj = lbl;
            /* Place switch above center to leave room for label */
            if (d->label_x == 0 && d->label_y == 0) {
                lv_obj_set_align(sw, LV_ALIGN_TOP_MID);
            } else {
                lv_obj_set_align(sw, LV_ALIGN_CENTER);
            }
        } else {
            d->label_obj = NULL;
            lv_obj_set_align(sw, LV_ALIGN_CENTER);
        }

        /* Apply initial state */
        if (d->current_state) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }

        /* Event callback for user toggle interaction */
        if (d->momentary) {
            lv_obj_add_event_cb(sw, _toggle_momentary_pressed_cb,  LV_EVENT_PRESSED,  w);
            lv_obj_add_event_cb(sw, _toggle_momentary_released_cb, LV_EVENT_RELEASED, w);
        } else {
            lv_obj_add_event_cb(sw, _toggle_clicked_cb, LV_EVENT_VALUE_CHANGED, w);
        }
    }

    w->root = cont;

    /* Subscribe to signal after root is set */
    if (d->signal_index >= 0) {
        signal_subscribe(d->signal_index, _toggle_on_signal, w);
    }

    /* Subscribe to night-mode changes if any night override is set, and apply
     * current state immediately so the widget renders correctly even if it
     * was created while night-mode is already active. */
    if (d->night.has_active_color || d->night.has_inactive_color ||
        d->night.has_label_color  || d->night.has_image_name) {
        night_mode_subscribe(_toggle_night_cb, w);
        _toggle_apply_night_mode(w, night_mode_is_active());
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

    if (d->momentary != DEF_MOMENTARY)
        cJSON_AddBoolToObject(cfg, "momentary", d->momentary);

    /* CAN TX */
    if (d->tx_can_id != 0)
        cJSON_AddNumberToObject(cfg, "tx_can_id", d->tx_can_id);

    if (d->tx_bit_start != DEF_TX_BIT_START)
        cJSON_AddNumberToObject(cfg, "tx_bit_start", d->tx_bit_start);

    if (d->tx_bit_length != DEF_TX_BIT_LENGTH)
        cJSON_AddNumberToObject(cfg, "tx_bit_length", d->tx_bit_length);

    if (d->tx_endian != DEF_TX_ENDIAN)
        cJSON_AddNumberToObject(cfg, "tx_endian", d->tx_endian);

    if (d->tx_rate_hz != DEF_TX_RATE_HZ)
        cJSON_AddNumberToObject(cfg, "tx_rate_hz", d->tx_rate_hz);

    /* Appearance: only write if different from defaults */
    if (d->active_color.full != lv_color_hex(DEF_ACTIVE_COLOR).full)
        cJSON_AddNumberToObject(cfg, "active_color", (int)d->active_color.full);

    if (d->inactive_color.full != lv_color_hex(DEF_INACTIVE_COLOR).full)
        cJSON_AddNumberToObject(cfg, "inactive_color", (int)d->inactive_color.full);

    if (d->label_color.full != lv_color_hex(DEF_LABEL_COLOR).full)
        cJSON_AddNumberToObject(cfg, "label_color", (int)d->label_color.full);

    if (d->font[0] != '\0')
        cJSON_AddStringToObject(cfg, "font", d->font);
    if (d->label_align != DEF_LABEL_ALIGN)
        cJSON_AddNumberToObject(cfg, "label_align", d->label_align);
    if (d->label_x != 0)
        cJSON_AddNumberToObject(cfg, "label_x", d->label_x);
    if (d->label_y != 0)
        cJSON_AddNumberToObject(cfg, "label_y", d->label_y);
    if (d->show_label != DEF_SHOW_LABEL)
        cJSON_AddBoolToObject(cfg, "show_label", d->show_label);

    /* Image mode fields */
    if (d->image_name[0] != '\0')
        cJSON_AddStringToObject(cfg, "image_name", d->image_name);

    if (d->active_opa != DEF_ACTIVE_OPA)
        cJSON_AddNumberToObject(cfg, "active_opa", d->active_opa);

    if (d->inactive_opa != DEF_INACTIVE_OPA)
        cJSON_AddNumberToObject(cfg, "inactive_opa", d->inactive_opa);

    /* Night-mode overrides — emit only fields that have an override set */
    {
        cJSON *n = cJSON_CreateObject();
        NIGHT_SERIALIZE_COLOR(n, d->night, active_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, inactive_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, label_color);
        NIGHT_SERIALIZE_IMAGE(n, d->night, image_name);
        if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
        else cJSON_Delete(n);
    }
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
        safe_strncpy(d->label, item->valuestring, sizeof(d->label));
    }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->signal_name, item->valuestring, sizeof(d->signal_name));
        d->signal_index = signal_find_by_name(d->signal_name);
    }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_on_threshold");
    if (cJSON_IsNumber(item)) d->signal_on_threshold = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "momentary");
    if (cJSON_IsBool(item)) d->momentary = cJSON_IsTrue(item);

    /* CAN TX */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_can_id");
    if (cJSON_IsNumber(item)) d->tx_can_id = (uint32_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_bit_start");
    if (cJSON_IsNumber(item)) d->tx_bit_start = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_bit_length");
    if (cJSON_IsNumber(item)) { d->tx_bit_length = (uint8_t)item->valueint; if (d->tx_bit_length > 32) d->tx_bit_length = 32; if (d->tx_bit_length == 0) d->tx_bit_length = 1; }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_endian");
    if (cJSON_IsNumber(item)) d->tx_endian = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_rate_hz");
    if (cJSON_IsNumber(item)) { d->tx_rate_hz = (uint8_t)item->valueint; if (d->tx_rate_hz > 50) d->tx_rate_hz = 50; }

    /* Appearance */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "active_color");
    if (cJSON_IsNumber(item)) d->active_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "inactive_color");
    if (cJSON_IsNumber(item)) d->inactive_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_color");
    if (cJSON_IsNumber(item)) d->label_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "font");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->font, item->valuestring, sizeof(d->font));
    }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_align");
    if (cJSON_IsNumber(item)) d->label_align = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_x");
    if (cJSON_IsNumber(item)) d->label_x = (int16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_y");
    if (cJSON_IsNumber(item)) d->label_y = (int16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "show_label");
    if (cJSON_IsBool(item)) d->show_label = cJSON_IsTrue(item);

    /* Image mode fields */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "image_name");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->image_name, item->valuestring, sizeof(d->image_name));
    }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "active_opa");
    if (cJSON_IsNumber(item)) d->active_opa = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "inactive_opa");
    if (cJSON_IsNumber(item)) d->inactive_opa = (uint8_t)item->valueint;

    /* Night-mode overrides */
    cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
    if (cJSON_IsObject(night)) {
        NIGHT_PARSE_COLOR(night, d->night, active_color);
        NIGHT_PARSE_COLOR(night, d->night, inactive_color);
        NIGHT_PARSE_COLOR(night, d->night, label_color);
        NIGHT_PARSE_IMAGE(night, d->night, image_name);
    }
}

/* ── Destroy ────────────────────────────────────────────────────────────── */
static void _toggle_destroy(widget_t *w) {
    if (!w) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    if (d) {
        _toggle_stop_tx_timer(d);
        if (d->signal_index >= 0)
            signal_unsubscribe(d->signal_index, _toggle_on_signal, w);
    }
    night_mode_unsubscribe(_toggle_night_cb, w);
    widget_rules_free(w);
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    if (d) {
        rdm_image_free((lv_img_dsc_t *)d->img_dsc);
        free(d);
    }
    free(w);
}

/* ── Apply overrides (conditional rules) ────────────────────────────────── */
static void _toggle_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    if (!d) return;

    /* Start from base type_data values (restore defaults) */
    lv_color_t active   = d->active_color;
    lv_color_t inactive = d->inactive_color;
    lv_color_t lbl_col  = d->label_color;

    /* Apply active overrides on top */
    for (uint8_t i = 0; i < count; i++) {
        const rule_override_t *o = &ov[i];
        if (strcmp(o->field_name, "active_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            active.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "inactive_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            inactive.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "label_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            lbl_col.full = (uint16_t)o->value.color;
        }
    }

    /* Apply all styles (either overridden or restored to base) */
    if (d->sw_obj && lv_obj_is_valid(d->sw_obj)) {
        lv_obj_set_style_bg_color(d->sw_obj, inactive, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(d->sw_obj, active, LV_PART_INDICATOR | LV_STATE_CHECKED);
    }
    if (d->img_obj && lv_obj_is_valid(d->img_obj)) {
        /* Re-apply image tint with overridden colors */
        lv_color_t tint = d->current_state ? active : inactive;
        lv_obj_set_style_img_recolor(d->img_obj, tint,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(d->img_obj, LV_OPA_COVER,
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
        lv_obj_set_style_text_color(d->label_obj, lbl_col, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/* ── Night-mode apply ───────────────────────────────────────────────────── */
/* Re-apply colors and (where feasible) the image source based on current
 * night-mode state. Picks day-or-night for each overridable field and writes
 * to the LVGL objects. */
static void _toggle_apply_night_mode(widget_t *w, bool active) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    toggle_data_t *d = (toggle_data_t *)w->type_data;
    if (!d) return;

    lv_color_t active_c   = NIGHT_PICK_COLOR(active, d->night, active_color,   d->active_color);
    lv_color_t inactive_c = NIGHT_PICK_COLOR(active, d->night, inactive_color, d->inactive_color);
    lv_color_t lbl_c      = NIGHT_PICK_COLOR(active, d->night, label_color,    d->label_color);

    if (d->sw_obj && lv_obj_is_valid(d->sw_obj)) {
        lv_obj_set_style_bg_color(d->sw_obj, inactive_c, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(d->sw_obj, active_c, LV_PART_INDICATOR | LV_STATE_CHECKED);
    }
    if (d->img_obj && lv_obj_is_valid(d->img_obj)) {
        /* Swap image source if a night override image is set */
        const char *img_name = NIGHT_PICK_IMAGE(active, d->night, image_name, d->image_name);
        if (img_name && img_name[0] != '\0') {
            lv_img_dsc_t *new_dsc = rdm_image_load(img_name);
            if (new_dsc) {
                lv_img_set_src(d->img_obj, new_dsc);
                rdm_image_free((lv_img_dsc_t *)d->img_dsc);
                d->img_dsc = new_dsc;
            }
        }
        /* Re-apply tint with night-picked colors */
        lv_color_t tint = d->current_state ? active_c : inactive_c;
        lv_obj_set_style_img_recolor(d->img_obj, tint,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(d->img_obj, LV_OPA_COVER,
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
        lv_obj_set_style_text_color(d->label_obj, lbl_c, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _toggle_night_cb(bool active, void *user_data) {
    _toggle_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Live preview paths:
 *   - Image-mode colour / opacity changes  -> _toggle_apply_image_state(d)
 *   - Switch-mode colour changes           -> lv_obj_set_style_bg_color(sw_obj)
 *   - Label text / font / align            -> direct on d->label_obj
 *   - image_name and momentary             -> persisted only (rebuild required;
 *                                             momentary flips the event-binding) */

static bool _toggle_inspector_get(const widget_t *w, const char *name,
                                  widget_field_value_t *out) {
	if (!w || w->type != WIDGET_TOGGLE || !w->type_data || !name || !out) return false;
	const toggle_data_t *d = (const toggle_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0)         { out->str = d->signal_name;  return true; }
	if (strcmp(name, "label") == 0)               { out->str = d->label;        return true; }
	if (strcmp(name, "font") == 0)                { out->str = d->font;         return true; }
	if (strcmp(name, "image_name") == 0)          { out->str = d->image_name;   return true; }
	if (strcmp(name, "show_label") == 0)          { out->b = d->show_label;     return true; }
	if (strcmp(name, "signal_on_threshold") == 0) { out->i = (int32_t)d->signal_on_threshold; return true; }
	if (strcmp(name, "tx_can_id") == 0)           { out->i = (int32_t)d->tx_can_id;    return true; }
	if (strcmp(name, "tx_bit_start") == 0)        { out->i = d->tx_bit_start;   return true; }
	if (strcmp(name, "tx_bit_length") == 0)       { out->i = d->tx_bit_length;  return true; }
	if (strcmp(name, "tx_endian") == 0)           { out->i = d->tx_endian;      return true; }
	if (strcmp(name, "tx_rate_hz") == 0)          { out->i = d->tx_rate_hz;     return true; }
	if (strcmp(name, "active_opa") == 0)          { out->i = d->active_opa;     return true; }
	if (strcmp(name, "inactive_opa") == 0)        { out->i = d->inactive_opa;   return true; }
	if (strcmp(name, "label_align") == 0)         { out->i = d->label_align;    return true; }
	if (strcmp(name, "label_x") == 0)             { out->i = d->label_x;        return true; }
	if (strcmp(name, "label_y") == 0)             { out->i = d->label_y;        return true; }
	if (strcmp(name, "active_color") == 0)        { out->color = lv_color_to32(d->active_color)   & 0xFFFFFF; return true; }
	if (strcmp(name, "inactive_color") == 0)      { out->color = lv_color_to32(d->inactive_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "label_color") == 0)         { out->color = lv_color_to32(d->label_color)    & 0xFFFFFF; return true; }
	return false;
}

static bool _toggle_inspector_set(widget_t *w, const char *name,
                                  const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_TOGGLE || !w->type_data || !name || !in) return false;
	toggle_data_t *d = (toggle_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0 && in->str) {
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;

		if (d->signal_index >= 0)
			signal_unsubscribe(d->signal_index, _toggle_on_signal, w);
		safe_strncpy(d->signal_name, in->str, sizeof(d->signal_name));
		d->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _toggle_on_signal, w);
		return true;
	}
	if (strcmp(name, "label") == 0 && in->str) {
		safe_strncpy(d->label, in->str, sizeof(d->label));
		if (d->label_obj && lv_obj_is_valid(d->label_obj))
			lv_label_set_text(d->label_obj, d->label);
		return true;
	}
	if (strcmp(name, "font") == 0 && in->str) {
		safe_strncpy(d->font, in->str, sizeof(d->font));
		if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
			const lv_font_t *f = widget_resolve_font(d->font);
			if (f) lv_obj_set_style_text_font(d->label_obj, f,
					LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		return true;
	}
	if (strcmp(name, "image_name") == 0 && in->str) {
		safe_strncpy(d->image_name, in->str, sizeof(d->image_name));
		return true;   /* switch <-> image flips object type — needs rebuild */
	}
	if (strcmp(name, "show_label") == 0) {
		d->show_label = in->b;
		if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
			if (d->show_label) lv_obj_clear_flag(d->label_obj, LV_OBJ_FLAG_HIDDEN);
			else               lv_obj_add_flag  (d->label_obj, LV_OBJ_FLAG_HIDDEN);
		}
		return true;
	}
	if (strcmp(name, "signal_on_threshold") == 0) {
		d->signal_on_threshold = (float)in->i;
		return true;
	}
	if (strcmp(name, "tx_can_id") == 0)     { d->tx_can_id     = (uint32_t)in->i; return true; }
	if (strcmp(name, "tx_bit_start") == 0)  { d->tx_bit_start  = (uint8_t)in->i;  return true; }
	if (strcmp(name, "tx_bit_length") == 0) { d->tx_bit_length = (uint8_t)in->i;  return true; }
	if (strcmp(name, "tx_endian") == 0)     { d->tx_endian     = (uint8_t)in->i;  return true; }
	if (strcmp(name, "tx_rate_hz") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 50) v = 50;
		d->tx_rate_hz = (uint8_t)v;
		return true;
	}
	if (strcmp(name, "active_color") == 0) {
		d->active_color = lv_color_hex(in->color);
		if (d->sw_obj && lv_obj_is_valid(d->sw_obj))
			lv_obj_set_style_bg_color(d->sw_obj, d->active_color,
				LV_PART_INDICATOR | LV_STATE_CHECKED);
		_toggle_apply_image_state(d);
		return true;
	}
	if (strcmp(name, "inactive_color") == 0) {
		d->inactive_color = lv_color_hex(in->color);
		if (d->sw_obj && lv_obj_is_valid(d->sw_obj))
			lv_obj_set_style_bg_color(d->sw_obj, d->inactive_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		_toggle_apply_image_state(d);
		return true;
	}
	if (strcmp(name, "active_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		d->active_opa = (uint8_t)v;
		_toggle_apply_image_state(d);
		return true;
	}
	if (strcmp(name, "inactive_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		d->inactive_opa = (uint8_t)v;
		_toggle_apply_image_state(d);
		return true;
	}
	if (strcmp(name, "label_color") == 0) {
		d->label_color = lv_color_hex(in->color);
		if (d->label_obj && lv_obj_is_valid(d->label_obj))
			lv_obj_set_style_text_color(d->label_obj, d->label_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "label_align") == 0) {
		uint8_t a = (uint8_t)in->i; if (a > 2) a = 1;
		d->label_align = a;
		if (d->label_obj && lv_obj_is_valid(d->label_obj))
			lv_obj_set_style_text_align(d->label_obj, _to_lv_align(a),
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "label_x") == 0) {
		d->label_x = (int16_t)in->i;
		if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
			lv_obj_set_align(d->label_obj, LV_ALIGN_CENTER);
			lv_obj_set_pos(d->label_obj, d->label_x, d->label_y);
		}
		return true;
	}
	if (strcmp(name, "label_y") == 0) {
		d->label_y = (int16_t)in->i;
		if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
			lv_obj_set_align(d->label_obj, LV_ALIGN_CENTER);
			lv_obj_set_pos(d->label_obj, d->label_x, d->label_y);
		}
		return true;
	}
	return false;
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
    d->momentary           = DEF_MOMENTARY;
    d->tx_can_id           = 0;
    d->tx_bit_start        = DEF_TX_BIT_START;
    d->tx_bit_length       = DEF_TX_BIT_LENGTH;
    d->tx_endian           = DEF_TX_ENDIAN;
    d->tx_rate_hz          = DEF_TX_RATE_HZ;
    d->active_color        = lv_color_hex(DEF_ACTIVE_COLOR);
    d->inactive_color      = lv_color_hex(DEF_INACTIVE_COLOR);
    d->label_color         = lv_color_hex(DEF_LABEL_COLOR);
    d->label_align         = DEF_LABEL_ALIGN;
    d->show_label          = DEF_SHOW_LABEL;
    d->active_opa          = DEF_ACTIVE_OPA;
    d->inactive_opa        = DEF_INACTIVE_OPA;
    d->current_state       = false;

    w->type      = WIDGET_TOGGLE;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = TOGGLE_DEFAULT_W;
    w->h         = TOGGLE_DEFAULT_H;
    w->type_data = d;
    snprintf(w->id, sizeof(w->id), "toggle_%u", slot);

    w->create           = _toggle_create;
    w->resize           = _toggle_resize;
    w->open_settings    = _toggle_open_settings;
    w->to_json          = _toggle_to_json;
    w->from_json        = _toggle_from_json;
    w->destroy          = _toggle_destroy;
    w->apply_overrides  = _toggle_apply_overrides;
    w->apply_night_mode = _toggle_apply_night_mode;
    w->inspector_get    = _toggle_inspector_get;
    w->inspector_set    = _toggle_inspector_set;

    return w;
}
