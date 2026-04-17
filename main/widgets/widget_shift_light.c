/**
 * widget_shift_light.c -- Shift light widget for racing dashboards.
 */
#include "widget_shift_light.h"
#include "widget_rules.h"
#include "system/night_mode.h"
#include "signal.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

static const char __attribute__((unused)) *TAG = "widget_shl";

/* SHL_DEFAULT_W defined in widget_shift_light.h via screen_config.h */
#define SHL_DEFAULT_H               30
#define SHL_DEFAULT_LED_COUNT        8
#define SHL_DEFAULT_RANGE_MIN     4000.0f
#define SHL_DEFAULT_RANGE_MAX     7000.0f
#define SHL_DEFAULT_FLASH_THRESH  7200.0f
#define SHL_DEFAULT_COLOR_LOW     0x00FF00
#define SHL_DEFAULT_COLOR_MID     0xFFFF00
#define SHL_DEFAULT_COLOR_HIGH    0xFF0000
#define SHL_DEFAULT_COLOR_OFF     0x212121
#define SHL_DEFAULT_LED_SPACING      2
#define SHL_DEFAULT_BORDER_RADIUS    2
#define SHL_DEFAULT_LED_WIDTH        0      /* 0 = auto-fit to container */
#define SHL_DEFAULT_LED_HEIGHT       0      /* 0 = auto-fit to container */
#define SHL_DEFAULT_FILL_MODE        0      /* 0 = left-to-right, 1 = outside-in */
#define SHL_DEFAULT_THRESH_MID    0.5f     /* Position where color changes low→mid */
#define SHL_DEFAULT_THRESH_HIGH   0.8f     /* Position where color changes mid→high */
#define SHL_FLASH_PERIOD_MS        200

static void _update_led_layout(widget_t *w) {
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    if (!d || !w->root || !lv_obj_is_valid(w->root)) return;

    lv_coord_t cw = (lv_coord_t)w->w;
    lv_coord_t ch = (lv_coord_t)w->h;

    int count = d->led_count;
    if (count < 1) count = 1;

    bool vertical = (ch > cw);
    int total_spacing = (count - 1) * d->led_spacing;

    int led_w, led_h;
    if (vertical) {
        led_w = d->led_width  > 0 ? d->led_width  : cw;
        led_h = d->led_height > 0 ? d->led_height : (ch - total_spacing) / count;
    } else {
        led_w = d->led_width  > 0 ? d->led_width  : (cw - total_spacing) / count;
        led_h = d->led_height > 0 ? d->led_height : ch;
    }
    if (led_w < 1) led_w = 1;
    if (led_h < 1) led_h = 1;

    if (vertical) {
        int strip_h = count * led_h + total_spacing;
        int y_ofs = (ch - strip_h) / 2;
        if (y_ofs < 0) y_ofs = 0;
        int x_ofs = (cw - led_w) / 2;
        if (x_ofs < 0) x_ofs = 0;

        for (int i = 0; i < count; i++) {
            if (!d->leds[i]) continue;
            /* Bottom-to-top: LED 0 at the bottom */
            int y = y_ofs + (count - 1 - i) * (led_h + d->led_spacing);
            lv_obj_set_pos(d->leds[i], x_ofs, y);
            lv_obj_set_size(d->leds[i], led_w, led_h);
            lv_obj_set_style_radius(d->leds[i], d->border_radius, LV_PART_MAIN);
        }
    } else {
        int strip_w = count * led_w + total_spacing;
        int x_ofs = (cw - strip_w) / 2;
        if (x_ofs < 0) x_ofs = 0;
        int y_ofs = (ch - led_h) / 2;
        if (y_ofs < 0) y_ofs = 0;

        for (int i = 0; i < count; i++) {
            if (!d->leds[i]) continue;
            int x = x_ofs + i * (led_w + d->led_spacing);
            lv_obj_set_pos(d->leds[i], x, y_ofs);
            lv_obj_set_size(d->leds[i], led_w, led_h);
            lv_obj_set_style_radius(d->leds[i], d->border_radius, LV_PART_MAIN);
        }
    }
}

static lv_color_t _led_color_for_pos(shift_light_data_t *d, int index, int fill_step) {
    if (d->led_count <= 1) return d->color_low;
    /* In outside-in mode, color is based on fill order (activation sequence)
     * rather than physical position, so edges light green and center lights red. */
    int basis = (d->fill_mode == 1) ? fill_step : index;
    float pos = (float)basis / (float)(d->led_count - 1);
    if (pos < d->threshold_mid)  return d->color_low;
    if (pos < d->threshold_high) return d->color_mid;
    return d->color_high;
}

/**
 * Map a fill order index (0 = first to light) to physical LED position.
 * left-to-right: 0,1,2,...,N-1
 * outside-in:    0, N-1, 1, N-2, 2, N-3, ...  (pairs light simultaneously)
 */
static int _fill_order(shift_light_data_t *d, int fill_idx) {
    if (d->fill_mode == 0 || d->led_count <= 1) return fill_idx;
    /* Outside-in symmetric: pairs light at the same time.
     * fill_idx 0 → pos 0 (left outer), fill_idx 1 → pos N-1 (right outer),
     * fill_idx 2 → pos 1 (left inner), fill_idx 3 → pos N-2 (right inner), etc. */
    int pair = fill_idx / 2;
    if (fill_idx % 2 == 0)
        return pair;                        /* left side */
    else
        return d->led_count - 1 - pair;     /* right side */
}

static void _shift_light_on_signal(float value, bool is_stale, void *user_data) {
    widget_t *w = (widget_t *)user_data;
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    if (!w->root || !lv_obj_is_valid(w->root)) return;

    if (is_stale) {
        for (int i = 0; i < d->led_count; i++) {
            if (d->leds[i])
                lv_obj_set_style_bg_color(d->leds[i], d->color_off, LV_PART_MAIN);
        }
        d->active_count = 0;
        return;
    }

    float range = d->range_max - d->range_min;
    float normalized = (value - d->range_min) / (range > 0 ? range : 1.0f);
    if (normalized < 0) normalized = 0;
    if (normalized > 1) normalized = 1;
    d->active_count = (uint8_t)(normalized * d->led_count + 0.5f);

    /* Outside-in mode: LEDs light in pairs (both outermost, then both next-inner).
     * Round active_count up to the nearest even number so both members of a
     * pair always activate together. */
    if (d->fill_mode == 1 && d->active_count % 2 != 0 && d->active_count < d->led_count)
        d->active_count++;

    bool flashing = (value >= d->flash_threshold);

    if (!flashing) {
        /* Build maps: which LEDs are active, and what fill step each position has */
        bool active[16] = {false};
        int  step_of[16] = {0};   /* fill step for each physical position */
        for (int f = 0; f < d->led_count; f++) {
            int phys = _fill_order(d, f);
            step_of[phys] = f;
            if (f < d->active_count)
                active[phys] = true;
        }

        for (int i = 0; i < d->led_count; i++) {
            if (!d->leds[i]) continue;
            if (active[i]) {
                lv_color_t color = _led_color_for_pos(d, i, step_of[i]);
                lv_obj_set_style_bg_color(d->leds[i], color, LV_PART_MAIN);
            } else {
                lv_obj_set_style_bg_color(d->leds[i], d->color_off, LV_PART_MAIN);
            }
        }
    }
}

static void _flash_timer_cb(lv_timer_t *timer) {
    widget_t *w = (widget_t *)timer->user_data;
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    if (!w->root || !lv_obj_is_valid(w->root)) return;

    signal_t *sig = (d->signal_index >= 0) ? signal_get_by_index(d->signal_index) : NULL;
    bool should_flash = sig && !sig->is_stale && sig->current_value >= d->flash_threshold;

    if (should_flash) {
        d->flash_state = !d->flash_state;
        for (int i = 0; i < d->led_count; i++) {
            if (!d->leds[i]) continue;
            if (d->flash_state)
                lv_obj_set_style_bg_color(d->leds[i], d->color_high, LV_PART_MAIN);
            else
                lv_obj_set_style_bg_color(d->leds[i], d->color_off, LV_PART_MAIN);
        }
    }
}

/* Forward declarations — night mode hooks used in _shl_create / _shl_destroy */
static void _shl_apply_night_mode(widget_t *w, bool active);
static void _shl_night_cb(bool active, void *user_data);

static void _shl_create(widget_t *w, lv_obj_t *parent) {
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    if (!d) return;

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, w->w, w->h);
    lv_obj_set_align(cont, LV_ALIGN_CENTER);
    lv_obj_set_pos(cont, w->x, w->y);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);

    w->root = cont;

    for (int i = 0; i < d->led_count && i < 16; i++) {
        lv_obj_t *led = lv_obj_create(cont);
        lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(led, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(led, d->color_off, LV_PART_MAIN);
        lv_obj_set_style_border_width(led, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(led, d->border_radius, LV_PART_MAIN);
        lv_obj_set_style_pad_all(led, 0, LV_PART_MAIN);
        d->leds[i] = led;
    }

    _update_led_layout(w);

    if (d->signal_index >= 0)
        signal_subscribe(d->signal_index, _shift_light_on_signal, w);

    d->flash_timer = lv_timer_create(_flash_timer_cb, d->flash_speed, w);
    if (d->flash_timer)
        lv_timer_set_repeat_count(d->flash_timer, -1);

    /* Subscribe to night-mode changes if any night override is set, and apply
     * current state immediately so the widget renders correctly even if it
     * was created while night-mode is already active. */
    if (d->night.has_color_low  || d->night.has_color_mid ||
        d->night.has_color_high || d->night.has_color_off) {
        night_mode_subscribe(_shl_night_cb, w);
        _shl_apply_night_mode(w, night_mode_is_active());
    }
}

static void _shl_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    w->w = nw;
    w->h = nh;
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    _update_led_layout(w);
}

static void _shl_open_settings(widget_t *w) { (void)w; }

static void _shl_to_json(widget_t *w, cJSON *out) {
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    widget_base_to_json(w, out);
    if (!d) return;

    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;

    if (d->signal_name[0])
        cJSON_AddStringToObject(cfg, "signal_name", d->signal_name);

    if (d->led_count != SHL_DEFAULT_LED_COUNT)
        cJSON_AddNumberToObject(cfg, "led_count", d->led_count);
    if (d->range_min != SHL_DEFAULT_RANGE_MIN)
        cJSON_AddNumberToObject(cfg, "range_min", d->range_min);
    if (d->range_max != SHL_DEFAULT_RANGE_MAX)
        cJSON_AddNumberToObject(cfg, "range_max", d->range_max);
    if (d->flash_threshold != SHL_DEFAULT_FLASH_THRESH)
        cJSON_AddNumberToObject(cfg, "flash_threshold", d->flash_threshold);
    if (d->flash_speed != SHL_FLASH_PERIOD_MS)
        cJSON_AddNumberToObject(cfg, "flash_speed", d->flash_speed);
    if (d->color_low.full != lv_color_hex(SHL_DEFAULT_COLOR_LOW).full)
        cJSON_AddNumberToObject(cfg, "color_low", (int)d->color_low.full);
    if (d->color_mid.full != lv_color_hex(SHL_DEFAULT_COLOR_MID).full)
        cJSON_AddNumberToObject(cfg, "color_mid", (int)d->color_mid.full);
    if (d->color_high.full != lv_color_hex(SHL_DEFAULT_COLOR_HIGH).full)
        cJSON_AddNumberToObject(cfg, "color_high", (int)d->color_high.full);
    if (d->color_off.full != lv_color_hex(SHL_DEFAULT_COLOR_OFF).full)
        cJSON_AddNumberToObject(cfg, "color_off", (int)d->color_off.full);
    if (d->led_spacing != SHL_DEFAULT_LED_SPACING)
        cJSON_AddNumberToObject(cfg, "led_spacing", d->led_spacing);
    if (d->border_radius != SHL_DEFAULT_BORDER_RADIUS)
        cJSON_AddNumberToObject(cfg, "border_radius", d->border_radius);
    if (d->led_width != SHL_DEFAULT_LED_WIDTH)
        cJSON_AddNumberToObject(cfg, "led_width", d->led_width);
    if (d->led_height != SHL_DEFAULT_LED_HEIGHT)
        cJSON_AddNumberToObject(cfg, "led_height", d->led_height);
    if (d->fill_mode != SHL_DEFAULT_FILL_MODE)
        cJSON_AddNumberToObject(cfg, "fill_mode", d->fill_mode);
    if (d->threshold_mid != SHL_DEFAULT_THRESH_MID)
        cJSON_AddNumberToObject(cfg, "threshold_mid", (double)d->threshold_mid);
    if (d->threshold_high != SHL_DEFAULT_THRESH_HIGH)
        cJSON_AddNumberToObject(cfg, "threshold_high", (double)d->threshold_high);

    /* Night-mode overrides — emit only fields that have an override set */
    {
        cJSON *n = cJSON_CreateObject();
        NIGHT_SERIALIZE_COLOR(n, d->night, color_low);
        NIGHT_SERIALIZE_COLOR(n, d->night, color_mid);
        NIGHT_SERIALIZE_COLOR(n, d->night, color_high);
        NIGHT_SERIALIZE_COLOR(n, d->night, color_off);
        if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
        else cJSON_Delete(n);
    }
}

static void _shl_from_json(widget_t *w, cJSON *in) {
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    widget_base_from_json(w, in);
    if (!d) return;

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cfg) return;

    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(d->signal_name, item->valuestring, sizeof(d->signal_name));

    item = cJSON_GetObjectItemCaseSensitive(cfg, "led_count");
    if (cJSON_IsNumber(item)) {
        d->led_count = (uint8_t)item->valueint;
        if (d->led_count < 4) d->led_count = 4;
        if (d->led_count > 16) d->led_count = 16;
    }

    item = cJSON_GetObjectItemCaseSensitive(cfg, "range_min");
    if (cJSON_IsNumber(item)) d->range_min = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "range_max");
    if (cJSON_IsNumber(item)) d->range_max = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "flash_threshold");
    if (cJSON_IsNumber(item)) d->flash_threshold = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "flash_speed");
    if (cJSON_IsNumber(item)) d->flash_speed = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "color_low");
    if (cJSON_IsNumber(item)) d->color_low.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "color_mid");
    if (cJSON_IsNumber(item)) d->color_mid.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "color_high");
    if (cJSON_IsNumber(item)) d->color_high.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "color_off");
    if (cJSON_IsNumber(item)) d->color_off.full = (uint16_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "led_spacing");
    if (cJSON_IsNumber(item)) d->led_spacing = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "border_radius");
    if (cJSON_IsNumber(item)) d->border_radius = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "led_width");
    if (cJSON_IsNumber(item)) d->led_width = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "led_height");
    if (cJSON_IsNumber(item)) d->led_height = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "fill_mode");
    if (cJSON_IsNumber(item)) d->fill_mode = (uint8_t)item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "threshold_mid");
    if (cJSON_IsNumber(item)) d->threshold_mid = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(cfg, "threshold_high");
    if (cJSON_IsNumber(item)) d->threshold_high = (float)item->valuedouble;

    if (d->signal_name[0])
        d->signal_index = signal_find_by_name(d->signal_name);

    /* Night-mode overrides */
    cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
    if (cJSON_IsObject(night)) {
        NIGHT_PARSE_COLOR(night, d->night, color_low);
        NIGHT_PARSE_COLOR(night, d->night, color_mid);
        NIGHT_PARSE_COLOR(night, d->night, color_high);
        NIGHT_PARSE_COLOR(night, d->night, color_off);
    }
}

static void _shl_destroy(widget_t *w) {
    if (!w) return;
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;

    if (d) {
        if (d->signal_index >= 0)
            signal_unsubscribe(d->signal_index, _shift_light_on_signal, w);

        if (d->flash_timer) {
            lv_timer_del(d->flash_timer);
            d->flash_timer = NULL;
        }
    }

    night_mode_unsubscribe(_shl_night_cb, w);
    widget_rules_free(w);

    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;

    if (d) free(d);
    free(w);
}

static void _shl_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    if (!d) return;

    lv_color_t c_low  = d->color_low;
    lv_color_t c_mid  = d->color_mid;
    lv_color_t c_high = d->color_high;
    lv_color_t c_off  = d->color_off;

    for (uint8_t i = 0; i < count; i++) {
        const rule_override_t *o = &ov[i];
        if (o->value_type == RULE_VAL_COLOR) {
            if (strcmp(o->field_name, "color_low") == 0)  c_low.full  = (uint16_t)o->value.color;
            else if (strcmp(o->field_name, "color_mid") == 0)  c_mid.full  = (uint16_t)o->value.color;
            else if (strcmp(o->field_name, "color_high") == 0) c_high.full = (uint16_t)o->value.color;
            else if (strcmp(o->field_name, "color_off") == 0)  c_off.full  = (uint16_t)o->value.color;
        }
    }

    /* Build fill step map for correct color assignment in outside-in mode */
    bool active[16] = {false};
    int  step_of[16] = {0};
    for (int f = 0; f < d->led_count; f++) {
        int phys = _fill_order(d, f);
        step_of[phys] = f;
        if (f < d->active_count)
            active[phys] = true;
    }

    for (int i = 0; i < d->led_count; i++) {
        if (!d->leds[i]) continue;
        if (active[i]) {
            int basis = (d->fill_mode == 1) ? step_of[i] : i;
            float pos = (d->led_count > 1) ? (float)basis / (float)(d->led_count - 1) : 0;
            lv_color_t color;
            if (pos < d->threshold_mid) color = c_low;
            else if (pos < d->threshold_high) color = c_mid;
            else color = c_high;
            lv_obj_set_style_bg_color(d->leds[i], color, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(d->leds[i], c_off, LV_PART_MAIN);
        }
    }
}

/* ── Night-mode apply ───────────────────────────────────────────────────── */
/* Re-apply LED colors based on current night-mode state. Picks day-or-night
 * value for each overridable color and re-renders the LED strip respecting
 * the current active_count and fill_mode. */
static void _shl_apply_night_mode(widget_t *w, bool active_state) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    shift_light_data_t *d = (shift_light_data_t *)w->type_data;
    if (!d) return;

    lv_color_t c_low  = NIGHT_PICK_COLOR(active_state, d->night, color_low,  d->color_low);
    lv_color_t c_mid  = NIGHT_PICK_COLOR(active_state, d->night, color_mid,  d->color_mid);
    lv_color_t c_high = NIGHT_PICK_COLOR(active_state, d->night, color_high, d->color_high);
    lv_color_t c_off  = NIGHT_PICK_COLOR(active_state, d->night, color_off,  d->color_off);

    /* Build fill step map matching _shl_apply_overrides / _shift_light_on_signal */
    bool led_active[16] = {false};
    int  step_of[16]    = {0};
    for (int f = 0; f < d->led_count; f++) {
        int phys = _fill_order(d, f);
        step_of[phys] = f;
        if (f < d->active_count)
            led_active[phys] = true;
    }

    for (int i = 0; i < d->led_count; i++) {
        if (!d->leds[i]) continue;
        if (led_active[i]) {
            int basis = (d->fill_mode == 1) ? step_of[i] : i;
            float pos = (d->led_count > 1) ? (float)basis / (float)(d->led_count - 1) : 0;
            lv_color_t color;
            if (pos < d->threshold_mid) color = c_low;
            else if (pos < d->threshold_high) color = c_mid;
            else color = c_high;
            lv_obj_set_style_bg_color(d->leds[i], color, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(d->leds[i], c_off, LV_PART_MAIN);
        }
    }
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _shl_night_cb(bool active, void *user_data) {
    _shl_apply_night_mode((widget_t *)user_data, active);
}

widget_t *widget_shift_light_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) return NULL;

    shift_light_data_t *d = heap_caps_calloc(1, sizeof(shift_light_data_t), MALLOC_CAP_SPIRAM);
    if (!d) d = calloc(1, sizeof(shift_light_data_t));
    if (!d) { free(w); return NULL; }

    d->signal_name[0]  = '\0';
    d->signal_index    = -1;
    d->led_count       = SHL_DEFAULT_LED_COUNT;
    d->range_min       = SHL_DEFAULT_RANGE_MIN;
    d->range_max       = SHL_DEFAULT_RANGE_MAX;
    d->flash_threshold = SHL_DEFAULT_FLASH_THRESH;
    d->flash_speed     = SHL_FLASH_PERIOD_MS;
    d->color_low       = lv_color_hex(SHL_DEFAULT_COLOR_LOW);
    d->color_mid       = lv_color_hex(SHL_DEFAULT_COLOR_MID);
    d->color_high      = lv_color_hex(SHL_DEFAULT_COLOR_HIGH);
    d->color_off       = lv_color_hex(SHL_DEFAULT_COLOR_OFF);
    d->led_spacing     = SHL_DEFAULT_LED_SPACING;
    d->border_radius   = SHL_DEFAULT_BORDER_RADIUS;
    d->led_width       = SHL_DEFAULT_LED_WIDTH;
    d->led_height      = SHL_DEFAULT_LED_HEIGHT;
    d->fill_mode       = SHL_DEFAULT_FILL_MODE;
    d->threshold_mid   = SHL_DEFAULT_THRESH_MID;
    d->threshold_high  = SHL_DEFAULT_THRESH_HIGH;
    d->flash_state     = false;
    d->active_count    = 0;
    d->flash_timer     = NULL;
    memset(d->leds, 0, sizeof(d->leds));

    w->type      = WIDGET_SHIFT_LIGHT;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = SHL_DEFAULT_W;
    w->h         = SHL_DEFAULT_H;
    w->type_data = d;
    snprintf(w->id, sizeof(w->id), "shl_%u", slot);

    w->create          = _shl_create;
    w->resize          = _shl_resize;
    w->open_settings   = _shl_open_settings;
    w->to_json         = _shl_to_json;
    w->from_json       = _shl_from_json;
    w->destroy         = _shl_destroy;
    w->apply_overrides = _shl_apply_overrides;
    w->apply_night_mode = _shl_apply_night_mode;

    return w;
}
