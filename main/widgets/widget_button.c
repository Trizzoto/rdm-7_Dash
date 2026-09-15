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
#include "ui/menu/edit_mode.h"
#include "can/can_emu.h"
#include "storage/config_store.h"
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
#define DEF_TX_RATE_HZ     10
#define DEF_LATCH          false
#define DEF_REMEMBER       false

/* Number of OFF frames sent back-to-back when a momentary releases or a latch
 * clears. A single dropped frame on a busy bus could otherwise strand a
 * driven output ON; a short burst makes the OFF self-healing without adding a
 * permanent heartbeat. */
#define TX_OFF_BURST       3
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
static void _btn_set_pressed_visual(button_data_t *d, bool pressed_state);
static void _btn_activate(widget_t *w);
static void _btn_deactivate(widget_t *w);
static void _button_apply_night_mode(widget_t *w, bool active);
static void _button_night_cb(bool active, void *user_data);

/* ── LVGL callbacks ───────────────────────────────────────────────────────── */

/* The raw tx_* output. Frames are built and sent by can_emu (per CAN ID, so
 * two buttons on one frame keep each other's bits), on its own 10 ms clock:
 * ON now and every 1/tx_rate_hz while active, and TX_OFF_BURST OFF frames
 * when it stops — a single dropped frame on a busy bus can't strand a driven
 * output ON. */
static void _btn_output(widget_t *w, bool on) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (d->tx_can_id == 0) return;
    can_emu_legacy_set(w, d->tx_can_id, d->tx_bit_start, d->tx_bit_length,
                       d->tx_endian, d->tx_rate_hz, on);
}

/* Persist (or forget) the latch state for this output when remember_state is
 * on. Keyed by the output it drives, so it survives reboots and layout edits. */
static void _btn_persist_latch(button_data_t *d) {
    if (!d->remember_state || d->tx_can_id == 0) return;
    config_store_save_widget_latch(d->tx_can_id, d->tx_bit_start, d->latch_state);
}

/* ── Activate / deactivate (shared by momentary + latch) ─────────────────────
 *
 * activate:   assert ON now and (re)start the keepalive timer so the frame
 *             keeps going out at tx_rate_hz while the output is held. Acts like
 *             a CAN keypad — the receiver sees a steady stream, not one frame.
 * deactivate: stop the keepalive and drive OFF (burst). Only emits OFF if we
 *             were actually asserting, so taps in Edit Mode (which never
 *             activate) put nothing on the bus. */
static void _btn_activate(widget_t *w) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d || d->tx_active) return;
    d->tx_active = true;
    _btn_output(w, true);
}

static void _btn_deactivate(widget_t *w) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d) return;
    if (d->tx_active) {
        d->tx_active = false;
        _btn_output(w, false);
    }
}

/* The device control this button presses, if any. A latch control keeps its
 * own state in the engine (and remembers it across power-off when the device
 * says so); anything else is held while the finger is down — or, on a latch
 * button, until the next press. */
static void _btn_control(button_data_t *d, bool down) {
    if (!d->control[0]) return;
    if (can_emu_ref_is_latch(d->control)) {
        if (d->latch) can_emu_set_latch(d->control, down);
        else if (down) can_emu_press(d->control);   /* each tap flips it */
        return;
    }
    if (down && !d->control_held) d->control_held = can_emu_press(d->control);
    else if (!down && d->control_held) { can_emu_release(d->control); d->control_held = false; }
}

/* Called from create() for latch buttons: restore the persisted on/off state
 * (when remember_state is set) and, if ON, re-assert the output + keepalive so
 * a CAN-controlled load comes back to its last commanded state after a reboot.
 * A latch control's state lives in can_emu, so the button just shows it. */
static void _btn_restore_latch(widget_t *w) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d || !d->latch) return;
    if (d->control[0] && can_emu_ref_is_latch(d->control)) {
        d->latch_state = can_emu_is_active(d->control);
    } else if (d->remember_state && d->tx_can_id != 0) {
        bool on = false;
        if (config_store_load_widget_latch(d->tx_can_id, d->tx_bit_start, &on) == ESP_OK)
            d->latch_state = on;
    }
    _btn_set_pressed_visual(d, d->latch_state);
    if (d->latch_state) {
        _btn_activate(w);
        if (d->control[0] && !can_emu_ref_is_latch(d->control)) _btn_control(d, true);
    }
}

/* Toggle pressed/normal visual state for image-mode and btn-mode buttons.
 * For two-image mode: swaps visibility between img_obj and pressed_img_obj.
 * For single-image mode: applies a dim overlay on press as fallback feedback.
 * For btn_obj mode: not called — LVGL handles LV_STATE_PRESSED natively. */
static void _btn_set_pressed_visual(button_data_t *d, bool pressed_state) {
    /* A latched-on normal button used to look exactly like an off one once
     * the finger lifted. CHECKED carries pressed_color while it is on. */
    if (d->latch && d->btn_obj && lv_obj_is_valid(d->btn_obj)) {
        if (pressed_state) lv_obj_add_state(d->btn_obj, LV_STATE_CHECKED);
        else               lv_obj_clear_state(d->btn_obj, LV_STATE_CHECKED);
    }
    if (d->img_obj && lv_obj_is_valid(d->img_obj)) {
        if (d->pressed_img_obj && lv_obj_is_valid(d->pressed_img_obj)) {
            if (pressed_state) {
                lv_obj_add_flag(d->img_obj, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(d->pressed_img_obj, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(d->img_obj, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(d->pressed_img_obj, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            /* Single image: semitransparent pressed_color overlay as feedback */
            lv_obj_set_style_img_recolor(d->img_obj,
                pressed_state ? d->pressed_color : lv_color_black(),
                LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_img_recolor_opa(d->img_obj,
                pressed_state ? 100 : 0,
                LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void _btn_pressed_cb(lv_event_t *e) {
    /* Suspend CAN TX while Edit Mode is armed — the user is positioning /
     * inspecting widgets, not driving the rig. */
    if (edit_mode_is_armed()) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    button_data_t *d = (button_data_t *)w->type_data;

    if (d->latch) {
        d->latch_state = !d->latch_state;
        _btn_set_pressed_visual(d, d->latch_state);
        if (d->latch_state) _btn_activate(w);
        else                _btn_deactivate(w);
        _btn_control(d, d->latch_state);
        _btn_persist_latch(d);
    } else {
        _btn_set_pressed_visual(d, true);
        _btn_activate(w);
        _btn_control(d, true);
    }
}

/* Fires on RELEASED (lifted on the button) AND PRESS_LOST (finger slid off the
 * button, or Edit Mode armed mid-press). A momentary MUST drive OFF on both —
 * missing the PRESS_LOST path is exactly what left the bit stuck ON before.
 * Deliberately NOT gated on edit_mode: if we asserted ON before edit mode was
 * armed, we still have to turn it off. _btn_deactivate is a no-op (no bus
 * traffic) when we never asserted, so Edit-Mode taps stay silent. */
static void _btn_release_or_lost(lv_event_t *e) {
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w || !w->type_data) return;
    button_data_t *d = (button_data_t *)w->type_data;
    if (d->latch) return;            /* latch holds until the next press */
    _btn_set_pressed_visual(d, false);
    _btn_deactivate(w);
    _btn_control(d, false);
}

/* ── vtable: create ───────────────────────────────────────────────────────── */

static void _button_create(widget_t *w, lv_obj_t *parent) {
    button_data_t *d = (button_data_t *)w->type_data;
    if (!d) return;

    /* Image mode: transparent container holds images + catches events */
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

        /* Normal-state image */
        lv_img_dsc_t *dsc = rdm_image_load(d->image_name);
        d->img_dsc = dsc;
        if (dsc) {
            lv_obj_t *img = lv_img_create(cont);
            lv_img_set_src(img, dsc);
            lv_obj_set_align(img, LV_ALIGN_CENTER);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
            d->img_obj = img;
        }

        /* Pressed-state image (hidden until button is pressed/latched) */
        if (d->pressed_image_name[0] != '\0') {
            lv_img_dsc_t *pdsc = rdm_image_load(d->pressed_image_name);
            d->pressed_img_dsc = pdsc;
            if (pdsc) {
                lv_obj_t *pimg = lv_img_create(cont);
                lv_img_set_src(pimg, pdsc);
                lv_obj_set_align(pimg, LV_ALIGN_CENTER);
                lv_obj_clear_flag(pimg, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(pimg, LV_OBJ_FLAG_HIDDEN);
                d->pressed_img_obj = pimg;
            }
        }

        /* Container is the single event target — images are visual only.
         * Bind PRESS_LOST as well as RELEASED so a finger sliding off still
         * turns a momentary OFF. */
        lv_obj_add_event_cb(cont, _btn_pressed_cb,      LV_EVENT_PRESSED,    w);
        lv_obj_add_event_cb(cont, _btn_release_or_lost, LV_EVENT_RELEASED,   w);
        lv_obj_add_event_cb(cont, _btn_release_or_lost, LV_EVENT_PRESS_LOST, w);

        /* Label (optional, floats on top of images) */
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
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            d->label_obj = lbl;
        } else {
            d->label_obj = NULL;
        }

        d->btn_obj = NULL;
        w->root = cont;

        /* Restore latch state (incl. persisted, if remember_state) and re-assert
         * the output when reloading a latched-on button. */
        _btn_restore_latch(w);

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

    /* Pressed state style — and a latch that is on looks pressed */
    lv_obj_set_style_bg_color(btn, d->pressed_color, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, d->pressed_color, LV_PART_MAIN | LV_STATE_CHECKED);

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

    /* Event callbacks. PRESS_LOST mirrors RELEASED so a finger sliding off the
     * button still drives a momentary OFF (was the stuck-on bug). */
    lv_obj_add_event_cb(btn, _btn_pressed_cb,      LV_EVENT_PRESSED,    w);
    lv_obj_add_event_cb(btn, _btn_release_or_lost, LV_EVENT_RELEASED,   w);
    lv_obj_add_event_cb(btn, _btn_release_or_lost, LV_EVENT_PRESS_LOST, w);

    d->btn_obj   = btn;
    d->img_obj   = NULL;
    d->img_dsc   = NULL;
    w->root      = btn;

    /* Restore latch state (incl. persisted) and re-assert if latched-on. */
    _btn_restore_latch(w);

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

    if (d->control[0] != '\0')
        cJSON_AddStringToObject(cfg, "control", d->control);

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

    if (d->latch != DEF_LATCH)
        cJSON_AddBoolToObject(cfg, "latch", d->latch);

    if (d->remember_state != DEF_REMEMBER)
        cJSON_AddBoolToObject(cfg, "remember_state", d->remember_state);

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
    if (d->pressed_image_name[0] != '\0')
        cJSON_AddStringToObject(cfg, "pressed_image_name", d->pressed_image_name);

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

    item = cJSON_GetObjectItemCaseSensitive(cfg, "control");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->control, item->valuestring, sizeof(d->control));
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

    item = cJSON_GetObjectItemCaseSensitive(cfg, "latch");
    if (cJSON_IsBool(item)) d->latch = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(cfg, "remember_state");
    if (cJSON_IsBool(item)) d->remember_state = cJSON_IsTrue(item);

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

    item = cJSON_GetObjectItemCaseSensitive(cfg, "pressed_image_name");
    if (cJSON_IsString(item) && item->valuestring) {
        safe_strncpy(d->pressed_image_name, item->valuestring, sizeof(d->pressed_image_name));
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
        /* Don't strand a driven output ON when the widget goes away (layout
         * reload, delete). Exception: a persisted latch stays asserted so a
         * hot-reload doesn't blink the load off — the recreated widget restores
         * it from NVS. */
        can_emu_legacy_forget(w, d->tx_active && !(d->latch && d->remember_state));
        /* Nor a device control held by a finger that no longer has a button. */
        if (d->control_held) { can_emu_release(d->control); d->control_held = false; }
        rdm_image_free((lv_img_dsc_t *)d->img_dsc);
        rdm_image_free((lv_img_dsc_t *)d->pressed_img_dsc);
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
        lv_obj_set_style_bg_color(d->btn_obj, pressed, LV_PART_MAIN | LV_STATE_CHECKED);
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
        lv_obj_set_style_bg_color(d->btn_obj, pressed, LV_PART_MAIN | LV_STATE_CHECKED);
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

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Button is TX-only — no signal_name field, so the DATA tab in the inspector
 * shows TX fields without the SIGNAL card. image_name swaps require a
 * rebuild (button mode vs image mode use different LVGL objects). */

static bool _button_inspector_get(const widget_t *w, const char *name,
                                  widget_field_value_t *out) {
	if (!w || w->type != WIDGET_BUTTON || !w->type_data || !name || !out) return false;
	const button_data_t *d = (const button_data_t *)w->type_data;

	if (strcmp(name, "label") == 0)           { out->str = d->label;       return true; }
	if (strcmp(name, "control") == 0)         { out->str = d->control;     return true; }
	if (strcmp(name, "font") == 0)            { out->str = d->font;        return true; }
	if (strcmp(name, "image_name") == 0)      { out->str = d->image_name;  return true; }
	if (strcmp(name, "show_label") == 0)      { out->b = d->show_label;    return true; }
	if (strcmp(name, "latch") == 0)           { out->b = d->latch;          return true; }
	if (strcmp(name, "remember_state") == 0)  { out->b = d->remember_state;  return true; }
	if (strcmp(name, "tx_can_id") == 0)       { out->i = (int32_t)d->tx_can_id;  return true; }
	if (strcmp(name, "tx_bit_start") == 0)    { out->i = d->tx_bit_start;  return true; }
	if (strcmp(name, "tx_bit_length") == 0)   { out->i = d->tx_bit_length; return true; }
	if (strcmp(name, "tx_endian") == 0)       { out->i = d->tx_endian;     return true; }
	if (strcmp(name, "tx_rate_hz") == 0)      { out->i = d->tx_rate_hz;    return true; }
	if (strcmp(name, "border_radius") == 0)   { out->i = d->border_radius; return true; }
	if (strcmp(name, "label_align") == 0)     { out->i = d->label_align;   return true; }
	if (strcmp(name, "label_x") == 0)         { out->i = d->label_x;       return true; }
	if (strcmp(name, "label_y") == 0)         { out->i = d->label_y;       return true; }
	if (strcmp(name, "bg_color") == 0)        { out->color = lv_color_to32(d->bg_color)      & 0xFFFFFF; return true; }
	if (strcmp(name, "text_color") == 0)      { out->color = lv_color_to32(d->text_color)    & 0xFFFFFF; return true; }
	if (strcmp(name, "pressed_color") == 0)   { out->color = lv_color_to32(d->pressed_color) & 0xFFFFFF; return true; }
	return false;
}

static bool _button_inspector_set(widget_t *w, const char *name,
                                  const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_BUTTON || !w->type_data || !name || !in) return false;
	button_data_t *d = (button_data_t *)w->type_data;

	if (strcmp(name, "label") == 0 && in->str) {
		safe_strncpy(d->label, in->str, sizeof(d->label));
		if (d->label_obj && lv_obj_is_valid(d->label_obj))
			lv_label_set_text(d->label_obj, d->label);
		return true;
	}
	if (strcmp(name, "control") == 0 && in->str) {
		if (d->control_held) { can_emu_release(d->control); d->control_held = false; }
		safe_strncpy(d->control, in->str, sizeof(d->control));
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
		return true;   /* normal-vs-image button needs rebuild */
	}
	if (strcmp(name, "show_label") == 0) {
		d->show_label = in->b;
		if (d->label_obj && lv_obj_is_valid(d->label_obj)) {
			if (d->show_label) lv_obj_clear_flag(d->label_obj, LV_OBJ_FLAG_HIDDEN);
			else               lv_obj_add_flag  (d->label_obj, LV_OBJ_FLAG_HIDDEN);
		}
		return true;
	}
	if (strcmp(name, "latch") == 0)           { d->latch          = in->b; return true; }
	if (strcmp(name, "remember_state") == 0) {
		d->remember_state = in->b;
		/* Capture the current latch state right away so enabling "remember"
		 * on an already-on button records it. */
		if (d->remember_state && d->latch) _btn_persist_latch(d);
		return true;
	}
	if (strcmp(name, "tx_can_id") == 0)       { d->tx_can_id       = (uint32_t)in->i; return true; }
	if (strcmp(name, "tx_bit_start") == 0)    { d->tx_bit_start    = (uint8_t)in->i;  return true; }
	if (strcmp(name, "tx_bit_length") == 0)   { d->tx_bit_length   = (uint8_t)in->i;  return true; }
	if (strcmp(name, "tx_endian") == 0)       { d->tx_endian       = (uint8_t)in->i;  return true; }
	if (strcmp(name, "tx_rate_hz") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 50) v = 50;
		d->tx_rate_hz = (uint8_t)v;
		return true;
	}
	if (strcmp(name, "border_radius") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 100) v = 100;
		d->border_radius = (uint8_t)v;
		if (d->btn_obj && lv_obj_is_valid(d->btn_obj))
			lv_obj_set_style_radius(d->btn_obj, d->border_radius,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "bg_color") == 0) {
		d->bg_color = lv_color_hex(in->color);
		if (d->btn_obj && lv_obj_is_valid(d->btn_obj))
			lv_obj_set_style_bg_color(d->btn_obj, d->bg_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "pressed_color") == 0) {
		d->pressed_color = lv_color_hex(in->color);
		if (d->btn_obj && lv_obj_is_valid(d->btn_obj))
			lv_obj_set_style_bg_color(d->btn_obj, d->pressed_color,
				LV_PART_MAIN | LV_STATE_PRESSED);
		return true;
	}
	if (strcmp(name, "text_color") == 0) {
		d->text_color = lv_color_hex(in->color);
		if (d->label_obj && lv_obj_is_valid(d->label_obj))
			lv_obj_set_style_text_color(d->label_obj, d->text_color,
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
    d->latch            = DEF_LATCH;
    d->remember_state   = DEF_REMEMBER;
    d->latch_state      = false;
    d->tx_active        = false;
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
    w->inspector_get    = _button_inspector_get;
    w->inspector_set    = _button_inspector_set;

    ESP_LOGI(TAG, "Created button instance slot=%u", slot);
    return w;
}
