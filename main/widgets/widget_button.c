/*
 * widget_button.c -- Push-button widget that transmits CAN messages.
 *
 * Supports momentary (press/release) and latching (toggle on/off) modes.
 * Visual feedback via a separate pressed-state background color.
 */
#include "widget_button.h"
#include "widget_image.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "can/can_decode.h"
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

#define DEF_LABEL          "BTN"
#define DEF_TX_CAN_ID      0
#define DEF_TX_BIT_START   0
#define DEF_TX_BIT_LENGTH  1
#define DEF_TX_ENDIAN      1
#define DEF_TX_RATE_HZ     0
#define DEF_TX_SEND_REL    false
#define DEF_LATCH          false
#define DEF_BG_COLOR       0x333333
#define DEF_TEXT_COLOR    0xFFFFFF
#define DEF_PRESSED_COLOR 0x555555
#define DEF_BORDER_RADIUS 5
#define DEF_LABEL_ALIGN   1   /* center */
#define DEF_SHOW_LABEL    true

static lv_text_align_t _to_lv_align(uint8_t a) {
    if (a == 0) return LV_TEXT_ALIGN_LEFT;
    if (a == 2) return LV_TEXT_ALIGN_RIGHT;
    return LV_TEXT_ALIGN_CENTER;
}

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void _btn_start_tx_timer(widget_t *w);
static void _btn_stop_tx_timer(button_data_t *d);
static void _button_apply_night_mode(widget_t *w, bool active);
static void _button_night_cb(bool active, void *user_data);

/* ── LVGL callbacks ───────────────────────────────────────────────────────── */

static void _btn_send(button_data_t *d, bool on) {
    if (d->tx_can_id == 0) return;
    uint8_t frame[8] = {0};
    uint32_t val = on ? (d->tx_bit_length >= 32 ? 0xFFFFFFFFu : ((1u << d->tx_bit_length) - 1u)) : 0u;
    can_pack_bits(frame, d->tx_bit_start, d->tx_bit_length, val, d->tx_endian);
    esp_err_t err = can_transmit_frame(d->tx_can_id, frame, 8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX failed (id=0x%03X): %s",
                 (unsigned)d->tx_can_id, esp_err_to_name(err));
    }
}

static void _btn_update_visual(button_data_t *d) {
    if (d->img_obj && lv_obj_is_valid(d->img_obj)) {
        lv_obj_set_style_img_recolor(d->img_obj,
            d->latch_state ? d->pressed_color : d->bg_color,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        return;
    }
    if (!d->btn_obj || !lv_obj_is_valid(d->btn_obj)) return;
    lv_obj_set_style_bg_color(d->btn_obj,
        d->latch_state ? d->pressed_color : d->bg_color,
        LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void _btn_pressed_cb(lv_event_t *e) {
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    button_data_t *d = (button_data_t *)w->type_data;

    if (d->latch) {
        d->latch_state = !d->latch_state;
        _btn_send(d, d->latch_state);
        _btn_update_visual(d);
        /* Start/stop periodic TX based on latch state */
        if (d->latch_state)
            _btn_start_tx_timer(w);
        else
            _btn_stop_tx_timer(d);
    } else {
        _btn_send(d, true);
        _btn_start_tx_timer(w);
    }
}

static void _btn_released_cb(lv_event_t *e) {
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    button_data_t *d = (button_data_t *)w->type_data;

    if (!d->latch) {
        _btn_stop_tx_timer(d);
        if (d->tx_send_release)
            _btn_send(d, false);
    }
}

/* ── Periodic TX timer ────────────────────────────────────────────────────── */

static void _btn_tx_timer_cb(lv_timer_t *t) {
    widget_t *w = (widget_t *)t->user_data;
    if (!w || !w->type_data) return;
    button_data_t *d = (button_data_t *)w->type_data;
    _btn_send(d, true);
}

static void _btn_start_tx_timer(widget_t *w) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d || d->tx_timer || d->tx_can_id == 0 || d->tx_rate_hz == 0) return;
    uint32_t period = 1000 / d->tx_rate_hz;
    d->tx_timer = lv_timer_create(_btn_tx_timer_cb, period, w);
}

static void _btn_stop_tx_timer(button_data_t *d) {
    if (d->tx_timer) {
        lv_timer_del(d->tx_timer);
        d->tx_timer = NULL;
    }
}

/* ── vtable: create ───────────────────────────────────────────────────────── */

static void _button_create(widget_t *w, lv_obj_t *parent) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d) return;

    /* Image mode: create a container with an image instead of lv_btn */
    if (d->image_name[0] != '\0') {
        lv_obj_t *cont = lv_obj_create(parent);
        lv_obj_set_size(cont, w->w, w->h);
        lv_obj_set_align(cont, LV_ALIGN_CENTER);
        lv_obj_set_pos(cont, w->x, w->y);
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_img_dsc_t *dsc = rdm_image_load(d->image_name);
        d->img_dsc = dsc;
        if (dsc) {
            lv_obj_t *img = lv_img_create(cont);
            lv_img_set_src(img, dsc);
            lv_obj_set_align(img, LV_ALIGN_CENTER);
            d->img_obj = img;

            /* Recolor with bg_color by default, pressed_color on press/latch */
            lv_obj_set_style_img_recolor(img, d->bg_color,
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER,
                                             LV_PART_MAIN | LV_STATE_DEFAULT);

            lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(img, _btn_pressed_cb,  LV_EVENT_PRESSED,  w);
            lv_obj_add_event_cb(img, _btn_released_cb, LV_EVENT_RELEASED, w);
        }

        /* Label (optional, on top of image) */
        if (d->show_label && d->label[0] != '\0') {
            lv_obj_t *lbl = lv_label_create(cont);
            lv_obj_set_align(lbl, LV_ALIGN_CENTER);
            if (d->label_x != 0 || d->label_y != 0)
                lv_obj_set_pos(lbl, d->label_x, d->label_y);
            lv_label_set_text(lbl, d->label);
            lv_obj_set_style_text_color(lbl, d->text_color, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_width(lbl, w->w);
            lv_obj_set_style_text_align(lbl, _to_lv_align(d->label_align), LV_PART_MAIN | LV_STATE_DEFAULT);
            if (d->font[0] != '\0') {
                const lv_font_t *f = widget_resolve_font(d->font);
                if (f) lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
            d->label_obj = lbl;
        } else {
            d->label_obj = NULL;
        }

        d->btn_obj = NULL;
        w->root = cont;

        /* Subscribe to night-mode changes if any night override is set */
        if (d->night.has_bg_color || d->night.has_text_color ||
            d->night.has_pressed_color || d->night.has_image_name) {
            night_mode_subscribe(_button_night_cb, w);
            _button_apply_night_mode(w, night_mode_is_active());
        }
        return;
    }

    /* Normal button mode */
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
    if (d->show_label) {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        if (d->label_x != 0 || d->label_y != 0)
            lv_obj_set_pos(lbl, d->label_x, d->label_y);
        lv_label_set_text(lbl, d->label);
        lv_obj_set_style_text_color(lbl, d->text_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(lbl, w->w);
        lv_obj_set_style_text_align(lbl, _to_lv_align(d->label_align), LV_PART_MAIN | LV_STATE_DEFAULT);

        if (d->font[0] != '\0') {
            const lv_font_t *f = widget_resolve_font(d->font);
            if (f) lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        d->label_obj = lbl;
    } else {
        d->label_obj = NULL;
    }

    /* Event callbacks */
    lv_obj_add_event_cb(btn, _btn_pressed_cb,  LV_EVENT_PRESSED,  w);
    lv_obj_add_event_cb(btn, _btn_released_cb, LV_EVENT_RELEASED, w);

    d->btn_obj   = btn;
    d->img_obj   = NULL;
    d->img_dsc   = NULL;
    w->root      = btn;

    /* Subscribe to night-mode changes if any night override is set */
    if (d->night.has_bg_color || d->night.has_text_color ||
        d->night.has_pressed_color || d->night.has_image_name) {
        night_mode_subscribe(_button_night_cb, w);
        _button_apply_night_mode(w, night_mode_is_active());
    }
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

    if (d->tx_bit_start != DEF_TX_BIT_START)
        cJSON_AddNumberToObject(cfg, "tx_bit_start", d->tx_bit_start);

    if (d->tx_bit_length != DEF_TX_BIT_LENGTH)
        cJSON_AddNumberToObject(cfg, "tx_bit_length", d->tx_bit_length);

    if (d->tx_endian != DEF_TX_ENDIAN)
        cJSON_AddNumberToObject(cfg, "tx_endian", d->tx_endian);

    if (d->tx_rate_hz != DEF_TX_RATE_HZ)
        cJSON_AddNumberToObject(cfg, "tx_rate_hz", d->tx_rate_hz);

    if (d->tx_send_release != DEF_TX_SEND_REL)
        cJSON_AddBoolToObject(cfg, "tx_send_release", d->tx_send_release);

    if (d->latch != DEF_LATCH)
        cJSON_AddBoolToObject(cfg, "latch", d->latch);

    /* Appearance -- defaults-only */
    if (d->bg_color.full != lv_color_hex(DEF_BG_COLOR).full)
        cJSON_AddNumberToObject(cfg, "bg_color", (int)d->bg_color.full);
    if (d->text_color.full != lv_color_hex(DEF_TEXT_COLOR).full)
        cJSON_AddNumberToObject(cfg, "text_color", (int)d->text_color.full);
    if (d->pressed_color.full != lv_color_hex(DEF_PRESSED_COLOR).full)
        cJSON_AddNumberToObject(cfg, "pressed_color", (int)d->pressed_color.full);
    if (d->border_radius != DEF_BORDER_RADIUS)
        cJSON_AddNumberToObject(cfg, "border_radius", d->border_radius);
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
    if (d->image_name[0] != '\0')
        cJSON_AddStringToObject(cfg, "image_name", d->image_name);

    /* Night-mode overrides — emit only fields that have an override set */
    {
        cJSON *n = cJSON_CreateObject();
        NIGHT_SERIALIZE_COLOR(n, d->night, bg_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, text_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, pressed_color);
        NIGHT_SERIALIZE_IMAGE(n, d->night, image_name);
        if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
        else cJSON_Delete(n);
    }
}

/* ── vtable: from_json ────────────────────────────────────────────────────── */

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

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_bit_start");
    if (cJSON_IsNumber(item)) d->tx_bit_start = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_bit_length");
    if (cJSON_IsNumber(item)) { d->tx_bit_length = (uint8_t)item->valueint; if (d->tx_bit_length > 32) d->tx_bit_length = 32; if (d->tx_bit_length == 0) d->tx_bit_length = 1; }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_endian");
    if (cJSON_IsNumber(item)) d->tx_endian = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_rate_hz");
    if (cJSON_IsNumber(item)) { d->tx_rate_hz = (uint8_t)item->valueint; if (d->tx_rate_hz > 50) d->tx_rate_hz = 50; }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "tx_send_release");
    if (cJSON_IsBool(item)) d->tx_send_release = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "latch");
    if (cJSON_IsBool(item)) d->latch = cJSON_IsTrue(item);

    /* Appearance */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "bg_color");
    if (cJSON_IsNumber(item)) d->bg_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "text_color");
    if (cJSON_IsNumber(item)) d->text_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "pressed_color");
    if (cJSON_IsNumber(item)) d->pressed_color.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "border_radius");
    if (cJSON_IsNumber(item)) d->border_radius = (uint8_t)item->valueint;

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

    item = cJSON_GetObjectItemCaseSensitive(cfg, "image_name");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->image_name, item->valuestring, sizeof(d->image_name));
    }

    /* Night-mode overrides */
    cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
    if (cJSON_IsObject(night)) {
        NIGHT_PARSE_COLOR(night, d->night, bg_color);
        NIGHT_PARSE_COLOR(night, d->night, text_color);
        NIGHT_PARSE_COLOR(night, d->night, pressed_color);
        NIGHT_PARSE_IMAGE(night, d->night, image_name);
    }
}

/* ── vtable: destroy ──────────────────────────────────────────────────────── */

static void _button_destroy(widget_t *w) {
    if (!w) return;
    if (w->type_data) {
        button_data_t *d = (button_data_t *)w->type_data;
        _btn_stop_tx_timer(d);
        rdm_image_free((lv_img_dsc_t *)d->img_dsc);
    }
    night_mode_unsubscribe(_button_night_cb, w);
    widget_rules_free(w);
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    if (w->type_data) free(w->type_data);
    free(w);
}

/* ── Apply overrides (conditional rules) ──────────────────────────────────── */

static void _button_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d) return;

    /* Start from base type_data values (restore defaults) */
    lv_color_t bg      = d->bg_color;
    lv_color_t txt     = d->text_color;
    lv_color_t pressed = d->pressed_color;

    /* Apply active overrides on top */
    for (uint8_t i = 0; i < count; i++) {
        const rule_override_t *o = &ov[i];
        if (strcmp(o->field_name, "bg_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            bg.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "text_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            txt.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "pressed_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            pressed.full = (uint16_t)o->value.color;
        }
    }

    /* Apply all styles (either overridden or restored to base) */
    if (d->btn_obj && lv_obj_is_valid(d->btn_obj)) {
        lv_obj_set_style_bg_color(d->btn_obj, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(d->btn_obj, pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    }
    if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
        lv_obj_set_style_text_color(d->label_obj, txt, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/* ── Night-mode apply ─────────────────────────────────────────────────────── */
/* Re-apply colors (and image, where feasible) based on current night-mode
 * state. bg_color goes on LV_STATE_DEFAULT and pressed_color on LV_STATE_PRESSED
 * so that LVGL's native state handling continues to work under night mode. */
static void _button_apply_night_mode(widget_t *w, bool active) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d) return;

    lv_color_t bg      = NIGHT_PICK_COLOR(active, d->night, bg_color,      d->bg_color);
    lv_color_t txt     = NIGHT_PICK_COLOR(active, d->night, text_color,    d->text_color);
    lv_color_t pressed = NIGHT_PICK_COLOR(active, d->night, pressed_color, d->pressed_color);

    if (d->btn_obj && lv_obj_is_valid(d->btn_obj)) {
        lv_obj_set_style_bg_color(d->btn_obj, bg,      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(d->btn_obj, pressed, LV_PART_MAIN | LV_STATE_PRESSED);
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
        /* Recolor tint follows latch state like _btn_update_visual does */
        lv_obj_set_style_img_recolor(d->img_obj,
            d->latch_state ? pressed : bg,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(d->img_obj, LV_OPA_COVER,
            LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
        lv_obj_set_style_text_color(d->label_obj, txt, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _button_night_cb(bool active, void *user_data) {
    _button_apply_night_mode((widget_t *)user_data, active);
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
    d->tx_can_id        = DEF_TX_CAN_ID;
    d->tx_bit_start     = DEF_TX_BIT_START;
    d->tx_bit_length    = DEF_TX_BIT_LENGTH;
    d->tx_endian        = DEF_TX_ENDIAN;
    d->tx_rate_hz       = DEF_TX_RATE_HZ;
    d->tx_send_release  = DEF_TX_SEND_REL;
    d->latch            = DEF_LATCH;
    d->latch_state      = false;
    d->bg_color        = lv_color_hex(DEF_BG_COLOR);
    d->text_color      = lv_color_hex(DEF_TEXT_COLOR);
    d->pressed_color   = lv_color_hex(DEF_PRESSED_COLOR);
    d->border_radius   = DEF_BORDER_RADIUS;
    d->label_align     = DEF_LABEL_ALIGN;
    d->show_label      = DEF_SHOW_LABEL;
    /* d->font, d->image_name left as "" (calloc zeroed) */

    w->type      = WIDGET_BUTTON;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = BUTTON_DEFAULT_W;
    w->h         = BUTTON_DEFAULT_H;
    w->type_data = d;
    snprintf(w->id, sizeof(w->id), "button_%u", slot);

    w->create           = _button_create;
    w->resize           = _button_resize;
    w->open_settings    = _button_open_settings;
    w->to_json          = _button_to_json;
    w->from_json        = _button_from_json;
    w->destroy          = _button_destroy;
    w->apply_overrides  = _button_apply_overrides;
    w->apply_night_mode = _button_apply_night_mode;

    ESP_LOGI(TAG, "Created button instance slot=%u", slot);
    return w;
}
