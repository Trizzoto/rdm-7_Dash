/*
 * widget_arc.c -- Arc widget with optional signal binding, image mode, and
 *                 RPM-bar-style polish (redline, limiter flash, value text).
 *
 * Supports three rendering modes:
 *   1. Image mode -- track + fill images with clip-based reveal (left to right)
 *   2. Static image mode -- track image only, no fill (decorative)
 *   3. Standard arc mode -- LVGL arc with full polish:
 *        - Background track + moving fill indicator
 *        - Redline zone marker (static red arc from threshold to max)
 *        - In-zone recolor (fill goes red while value >= threshold)
 *        - Limiter effect (flash or solid color while value >= limiter_value)
 *        - Centered value text overlay (font / decimals / unit suffix)
 *
 * Field defaults match widget_rpm_bar where it makes sense so users can
 * swap one for the other without re-dialing every knob.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_arc";

#define ARC_DEFAULT_W              200
#define ARC_DEFAULT_H              200
#define ARC_DEFAULT_START          135
#define ARC_DEFAULT_END             45
#define ARC_DEFAULT_WIDTH           10
#define ARC_DEFAULT_COLOR          0x00FF00
#define ARC_DEFAULT_BG_COLOR       0x333333
#define ARC_DEFAULT_BG_WIDTH        10
#define ARC_DEFAULT_ROUNDED         false
#define ARC_DEFAULT_SIG_MIN         0.0f
#define ARC_DEFAULT_SIG_MAX         100.0f
#define ARC_DEFAULT_REDLINE         80.0f
#define ARC_DEFAULT_REDLINE_COLOR  0xFF0000
#define ARC_DEFAULT_LIMITER_VAL     90.0f
#define ARC_DEFAULT_LIMITER_COLOR  0xFF0000
#define ARC_DEFAULT_FLASH_MS        200
#define ARC_DEFAULT_VALUE_COLOR    0xFFFFFF

/* Forward declarations */
static void _arc_on_signal(float value, bool is_stale, void *user_data);
static void _arc_apply_night_mode(widget_t *w, bool active);
static void _arc_night_cb(bool active, void *user_data);
static void _arc_apply_fill_color(arc_data_t *d, bool active);
static void _arc_flash_timer_cb(lv_timer_t *t);
static void _arc_update_value_label(arc_data_t *d, float value);
static void _arc_recompute_value(widget_t *w, float value, bool is_stale);

/* ── Helpers: mode detection ───────────────────────────────────────────── */

static bool _is_image_mode(const arc_data_t *d) {
    return d->arc_image[0] != '\0' && d->arc_image_full[0] != '\0';
}

static bool _is_static_image_mode(const arc_data_t *d) {
    return d->arc_image[0] != '\0' && d->arc_image_full[0] == '\0';
}

/* ── Helpers: image-mode clip width update ─────────────────────────────── */

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

/* ── Helpers: standard-arc value update ────────────────────────────────── */

static void _update_arc_value(arc_data_t *d, float value) {
    if (!d || !d->arc_obj) return;

    float range = d->signal_max - d->signal_min;
    if (range <= 0.0f) range = 100.0f;
    float pct = (value - d->signal_min) / range;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    lv_arc_set_value(d->arc_obj, (int16_t)(pct * 100.0f));
}

/* Recolor the indicator arc based on whether we're in the redline / limiter
 * zone and (for flash effect) the current flash phase. `active` is the
 * current night-mode state — used to pick night-overridden colors. */
static void _arc_apply_fill_color(arc_data_t *d, bool active) {
    if (!d || !d->arc_obj || !lv_obj_is_valid(d->arc_obj)) return;

    /* Pick the base "normal" fill color (with night override if set). */
    lv_color_t normal = NIGHT_PICK_COLOR(active, d->night, arc_color, d->arc_color);
    lv_color_t redline = NIGHT_PICK_COLOR(active, d->night, redline_color, d->redline_color);
    lv_color_t fill = normal;

    /* Order of precedence:
     *   1. Limiter Solid  → limiter_color overrides everything
     *   2. Limiter Flash  → toggle between normal and limiter_color
     *   3. Redline zone   → recolor with redline_color (if recolor_fill on)
     *   4. Default        → normal arc_color
     * Whichever wins gets applied to LV_PART_INDICATOR. */
    if (d->in_limiter && d->limiter_effect == 2) {
        fill = d->limiter_color;
    } else if (d->in_limiter && d->limiter_effect == 1) {
        fill = d->flash_phase ? d->limiter_color : normal;
    } else if (d->redline_enabled && d->redline_recolor_fill) {
        /* "In zone" detection re-uses the in_limiter result when limiter
         * is at the same threshold; otherwise check threshold directly. */
        if (d->_cached_value >= d->redline_threshold) {
            fill = redline;
        }
    }

    lv_obj_set_style_arc_color(d->arc_obj, fill, LV_PART_INDICATOR);
}

/* Flash timer fires every flash_speed_ms while value >= limiter_value AND
 * limiter_effect == 1. Toggles flash_phase + repaints. */
static void _arc_flash_timer_cb(lv_timer_t *t) {
    widget_t *w = (widget_t *)t->user_data;
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;
    d->flash_phase = !d->flash_phase;
    _arc_apply_fill_color(d, night_mode_is_active());
}

/* Start / stop the flash timer based on current state. Idempotent. */
static void _arc_update_flash_state(widget_t *w) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    bool want_flash = d->in_limiter && d->limiter_effect == 1;
    if (want_flash && !d->flash_timer) {
        uint32_t period = d->flash_speed_ms ? d->flash_speed_ms : ARC_DEFAULT_FLASH_MS;
        d->flash_timer = lv_timer_create(_arc_flash_timer_cb, period, w);
        d->flash_phase = true;
    } else if (!want_flash && d->flash_timer) {
        lv_timer_del(d->flash_timer);
        d->flash_timer = NULL;
        d->flash_phase = false;
    } else if (want_flash && d->flash_timer) {
        /* Already running — only restart if period changed. */
        if (d->flash_timer->period != d->flash_speed_ms) {
            lv_timer_set_period(d->flash_timer, d->flash_speed_ms);
        }
    }
}

/* Compose the value-text label content using value_decimals + value_unit.
 * Skips work when show_value is false or label doesn't exist. */
static void _arc_update_value_label(arc_data_t *d, float value) {
    if (!d || !d->value_label || !lv_obj_is_valid(d->value_label)) return;
    char buf[32];
    if (d->value_decimals == 0) {
        snprintf(buf, sizeof(buf), "%d%s%s",
                 (int)value,
                 d->value_unit[0] ? " " : "",
                 d->value_unit);
    } else {
        snprintf(buf, sizeof(buf), "%.*f%s%s",
                 (int)d->value_decimals, (double)value,
                 d->value_unit[0] ? " " : "",
                 d->value_unit);
    }
    lv_label_set_text(d->value_label, buf);
}

/* Central per-tick logic. Cache value, update LVGL arc, update label,
 * re-evaluate zone state, repaint fill. Called from the signal callback
 * and from places that need a forced repaint (resize, night swap). */
static void _arc_recompute_value(widget_t *w, float value, bool is_stale) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    if (is_stale) {
        /* Stale: collapse fill to 0 and clear limiter state. */
        d->_cached_value = d->signal_min;
        d->in_limiter   = false;
        if (_is_image_mode(d) && d->img_clip_obj) {
            lv_obj_set_width(d->img_clip_obj, 0);
        } else {
            _update_arc_value(d, d->signal_min);
        }
        _arc_update_value_label(d, d->signal_min);
        _arc_update_flash_state(w);
        _arc_apply_fill_color(d, night_mode_is_active());
        return;
    }

    d->_cached_value = value;

    if (_is_image_mode(d)) {
        _update_image_clip(w, value);
    } else {
        _update_arc_value(d, value);
    }

    /* Update limiter latch + flash timer. */
    bool new_in_limiter = (d->limiter_effect != 0) && (value >= d->limiter_value);
    if (new_in_limiter != d->in_limiter) {
        d->in_limiter = new_in_limiter;
        _arc_update_flash_state(w);
    }

    _arc_update_value_label(d, value);
    _arc_apply_fill_color(d, night_mode_is_active());
}

/* ── Signal callback ───────────────────────────────────────────────────── */

static void _arc_on_signal(float value, bool is_stale, void *user_data) {
    widget_t *w = (widget_t *)user_data;
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    _arc_recompute_value(w, value, is_stale);
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

/* ── Helper: configure one LVGL arc with given angles/widths/colors ─── */
/* Shared between the main indicator arc and the redline zone arc — both
 * use the same lv_arc_create + style chain. The redline arc only differs
 * in the angle range (subset of the sweep) and color. */
static void _configure_arc(lv_obj_t *obj, int16_t start, int16_t end,
                            uint8_t bg_w, lv_color_t bg_c,
                            uint8_t fg_w, lv_color_t fg_c,
                            bool rounded) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_mode(obj, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(obj, start, end);
    lv_arc_set_angles(obj, start, end);
    lv_arc_set_range(obj, 0, 100);
    lv_arc_set_value(obj, 100);
    lv_obj_set_style_arc_color(obj, bg_c, LV_PART_MAIN);
    lv_obj_set_style_arc_width(obj, bg_w, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(obj, rounded, LV_PART_MAIN);
    lv_obj_set_style_arc_color(obj, fg_c, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(obj, fg_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(obj, rounded, LV_PART_INDICATOR);
    /* Hide the knob */
    lv_obj_set_style_pad_all(obj, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_KNOB);
    /* Remove default bg fill */
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
}

/* Convert a value-domain threshold to an angle along the sweep, used to
 * position the redline zone marker. */
static int16_t _value_to_angle(const arc_data_t *d, float value) {
    float range = d->signal_max - d->signal_min;
    if (range <= 0.0f) range = 100.0f;
    float pct = (value - d->signal_min) / range;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    /* Sweep is from start_angle to end_angle going clockwise (LVGL
     * convention). Wrap if end < start. */
    int32_t sweep = (360 + d->end_angle - d->start_angle) % 360;
    if (sweep == 0 && d->start_angle != d->end_angle) sweep = 360;
    return (int16_t)(d->start_angle + (int32_t)(pct * (float)sweep));
}

/* ── Create: standard arc mode (now with redline + value text) ────────── */

static void _arc_create_standard(widget_t *w, lv_obj_t *parent) {
    arc_data_t *d = (arc_data_t *)w->type_data;

    /* All three children (main arc, redline arc, value label) live inside a
     * transparent container so the widget acts as a single hit target +
     * resize unit. */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, w->w, w->h);
    lv_obj_set_align(cont, LV_ALIGN_CENTER);
    lv_obj_set_pos(cont, w->x, w->y);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);

    /* Main moving arc. */
    lv_obj_t *obj = lv_arc_create(cont);
    lv_obj_set_size(obj, w->w, w->h);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    _configure_arc(obj, d->start_angle, d->end_angle,
                    d->bg_arc_width, d->bg_arc_color,
                    d->arc_width, d->arc_color, d->rounded_ends);
    /* Initial value: 0 if signal-bound (will update on first tick),
     * else 100 (decorative full-fill). */
    lv_arc_set_value(obj, d->signal_index >= 0 ? 0 : 100);
    d->arc_obj = obj;

    /* Redline zone marker — separate arc spanning [threshold_angle..end_angle].
     * Drawn ON TOP of the main arc so it stays visible regardless of fill
     * progress. Background is transparent (track shows through); the
     * indicator part carries the red colour at the configured width. */
    if (d->redline_enabled) {
        int16_t rstart = _value_to_angle(d, d->redline_threshold);
        int16_t rend   = d->end_angle;
        uint8_t rw     = d->redline_arc_width > 0 ? d->redline_arc_width
                                                  : d->arc_width;
        lv_obj_t *robj = lv_arc_create(cont);
        lv_obj_set_size(robj, w->w, w->h);
        lv_obj_set_align(robj, LV_ALIGN_CENTER);
        _configure_arc(robj, rstart, rend,
                        0, lv_color_black(),
                        rw, d->redline_color, d->rounded_ends);
        lv_arc_set_value(robj, 100);
        /* Bg arc transparent so we only see the red indicator slice. */
        lv_obj_set_style_arc_opa(robj, LV_OPA_TRANSP, LV_PART_MAIN);
        d->redline_arc_obj = robj;
    }

    /* Value text overlay — centered label with optional font / unit. */
    if (d->show_value) {
        lv_obj_t *lbl = lv_label_create(cont);
        lv_label_set_text(lbl, "0");
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_set_y(lbl, d->value_y_offset);
        lv_obj_set_style_text_color(lbl,
            NIGHT_PICK_COLOR(false, d->night, value_color, d->value_color),
            LV_PART_MAIN);
        if (d->value_font[0] != '\0') {
            const lv_font_t *f = widget_resolve_font(d->value_font);
            if (f) lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN);
        }
        d->value_label = lbl;
    }

    w->root = cont;
}

/* ── Vtable: create ────────────────────────────────────────────────────── */

static void _arc_create(widget_t *w, lv_obj_t *parent) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    /* Reset runtime pointers in case this is a re-create (layout reload). */
    d->arc_obj         = NULL;
    d->redline_arc_obj = NULL;
    d->value_label     = NULL;
    d->flash_timer     = NULL;
    d->flash_phase     = false;
    d->in_limiter      = false;
    d->_cached_value   = d->signal_min;

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
        d->night.has_value_color || d->night.has_redline_color ||
        d->night.has_arc_image || d->night.has_arc_image_full) {
        night_mode_subscribe(_arc_night_cb, w);
        _arc_apply_night_mode(w, night_mode_is_active());
    }

    /* If a signal is bound and already has a fresh value, snap to it so the
     * widget renders correctly on first paint without waiting for the next
     * signal tick. (Matches widget_meter init behaviour.) */
    if (d->signal_index >= 0) {
        signal_t *sig = signal_get_by_index((uint16_t)d->signal_index);
        if (sig && !sig->is_stale) {
            _arc_recompute_value(w, sig->current_value, false);
        }
    }
}

static void _arc_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    /* Also resize the arc child(ren) so they fill the new container. */
    if (d) {
        if (d->arc_obj && lv_obj_is_valid(d->arc_obj))
            lv_obj_set_size(d->arc_obj, nw, nh);
        if (d->redline_arc_obj && lv_obj_is_valid(d->redline_arc_obj))
            lv_obj_set_size(d->redline_arc_obj, nw, nh);
    }
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

    /* Redline zone */
    if (d->redline_enabled)
        cJSON_AddBoolToObject(cfg, "redline_enabled", true);
    if (d->redline_threshold != ARC_DEFAULT_REDLINE)
        cJSON_AddNumberToObject(cfg, "redline_threshold", (double)d->redline_threshold);
    if (d->redline_color.full != lv_color_hex(ARC_DEFAULT_REDLINE_COLOR).full)
        cJSON_AddNumberToObject(cfg, "redline_color", (int)d->redline_color.full);
    if (d->redline_arc_width != 0)
        cJSON_AddNumberToObject(cfg, "redline_arc_width", d->redline_arc_width);
    if (!d->redline_recolor_fill)
        cJSON_AddBoolToObject(cfg, "redline_recolor_fill", false);

    /* Limiter */
    if (d->limiter_effect != 0)
        cJSON_AddNumberToObject(cfg, "limiter_effect", d->limiter_effect);
    if (d->limiter_value != ARC_DEFAULT_LIMITER_VAL)
        cJSON_AddNumberToObject(cfg, "limiter_value", (double)d->limiter_value);
    if (d->limiter_color.full != lv_color_hex(ARC_DEFAULT_LIMITER_COLOR).full)
        cJSON_AddNumberToObject(cfg, "limiter_color", (int)d->limiter_color.full);
    if (d->flash_speed_ms != ARC_DEFAULT_FLASH_MS)
        cJSON_AddNumberToObject(cfg, "flash_speed_ms", d->flash_speed_ms);

    /* Value text */
    if (d->show_value)
        cJSON_AddBoolToObject(cfg, "show_value", true);
    if (d->value_font[0] != '\0')
        cJSON_AddStringToObject(cfg, "value_font", d->value_font);
    if (d->value_color.full != lv_color_hex(ARC_DEFAULT_VALUE_COLOR).full)
        cJSON_AddNumberToObject(cfg, "value_color", (int)d->value_color.full);
    if (d->value_y_offset != 0)
        cJSON_AddNumberToObject(cfg, "value_y_offset", d->value_y_offset);
    if (d->value_decimals != 0)
        cJSON_AddNumberToObject(cfg, "value_decimals", d->value_decimals);
    if (d->value_unit[0] != '\0')
        cJSON_AddStringToObject(cfg, "value_unit", d->value_unit);

    /* Rules */
    widget_rules_to_json(w, cfg);

    /* Night-mode overrides — emit only fields that have an override set */
    {
        cJSON *n = cJSON_CreateObject();
        NIGHT_SERIALIZE_COLOR(n, d->night, arc_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, bg_arc_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, value_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, redline_color);
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

    /* Redline */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_enabled");
    if (cJSON_IsBool(item)) d->redline_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_threshold");
    if (cJSON_IsNumber(item)) d->redline_threshold = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_color");
    if (cJSON_IsNumber(item)) d->redline_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_arc_width");
    if (cJSON_IsNumber(item)) d->redline_arc_width = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "redline_recolor_fill");
    if (cJSON_IsBool(item)) d->redline_recolor_fill = cJSON_IsTrue(item);

    /* Limiter */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_effect");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v < 0) v = 0;
        if (v > 2) v = 2;
        d->limiter_effect = (uint8_t)v;
    }
    item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_value");
    if (cJSON_IsNumber(item)) d->limiter_value = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_color");
    if (cJSON_IsNumber(item)) d->limiter_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "flash_speed_ms");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v < 50)   v = 50;
        if (v > 1000) v = 1000;
        d->flash_speed_ms = (uint16_t)v;
    }

    /* Value text */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "show_value");
    if (cJSON_IsBool(item)) d->show_value = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_font");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(d->value_font, item->valuestring, sizeof(d->value_font));
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_color");
    if (cJSON_IsNumber(item)) d->value_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_y_offset");
    if (cJSON_IsNumber(item)) d->value_y_offset = (int16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_decimals");
    if (cJSON_IsNumber(item)) d->value_decimals = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_unit");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(d->value_unit, item->valuestring, sizeof(d->value_unit));

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
        NIGHT_PARSE_COLOR(night, d->night, value_color);
        NIGHT_PARSE_COLOR(night, d->night, redline_color);
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

    if (d && d->flash_timer) {
        lv_timer_del(d->flash_timer);
        d->flash_timer = NULL;
    }

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

    /* If limiter/redline is currently active, _arc_apply_fill_color owns the
     * indicator color — skip overriding it so the rule doesn't fight the
     * limiter flash. Background + width can still be applied. */
    bool zone_owns_fill = (d->in_limiter && d->limiter_effect != 0) ||
                          (d->redline_enabled && d->redline_recolor_fill &&
                           d->_cached_value >= d->redline_threshold);
    if (!zone_owns_fill) {
        lv_obj_set_style_arc_color(d->arc_obj, fg, LV_PART_INDICATOR);
    }
    lv_obj_set_style_arc_width(d->arc_obj, fg_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d->arc_obj, bg, LV_PART_MAIN);
    lv_obj_set_style_arc_width(d->arc_obj, bg_w, LV_PART_MAIN);
}

/* ── Night-mode apply ───────────────────────────────────────────────────── */
/* Re-apply arc colors (and image swaps, where feasible) based on current
 * night-mode state. Image swap behaviour:
 *   - In standard arc mode (no images): arc_color, bg_arc_color,
 *     value_color and redline_color all apply.
 *   - In image mode: we attempt to reload the track/fill image descriptors
 *     from the night override names; LVGL source pointers are updated. */
static void _arc_apply_night_mode(widget_t *w, bool active) {
    if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;

    /* Colors — apply to standard arc mode */
    if (d->arc_obj && lv_obj_is_valid(d->arc_obj)) {
        lv_color_t bg = NIGHT_PICK_COLOR(active, d->night, bg_arc_color, d->bg_arc_color);
        lv_obj_set_style_arc_color(d->arc_obj, bg, LV_PART_MAIN);
        /* Foreground / indicator picks colour via _arc_apply_fill_color so
         * limiter / redline state stays consistent. */
        _arc_apply_fill_color(d, active);
    }
    if (d->redline_arc_obj && lv_obj_is_valid(d->redline_arc_obj)) {
        lv_color_t rc = NIGHT_PICK_COLOR(active, d->night, redline_color, d->redline_color);
        lv_obj_set_style_arc_color(d->redline_arc_obj, rc, LV_PART_INDICATOR);
    }
    if (d->value_label && lv_obj_is_valid(d->value_label)) {
        lv_color_t vc = NIGHT_PICK_COLOR(active, d->night, value_color, d->value_color);
        lv_obj_set_style_text_color(d->value_label, vc, LV_PART_MAIN);
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

    /* Standard arc defaults */
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

    /* Redline defaults — disabled by default; threshold/color match
     * widget_rpm_bar so users muscle-memory carries over. */
    d->redline_enabled       = false;
    d->redline_threshold     = ARC_DEFAULT_REDLINE;
    d->redline_color         = lv_color_hex(ARC_DEFAULT_REDLINE_COLOR);
    d->redline_arc_width     = 0;     /* 0 = follow arc_width */
    d->redline_recolor_fill  = true;

    /* Limiter defaults */
    d->limiter_effect = 0;
    d->limiter_value  = ARC_DEFAULT_LIMITER_VAL;
    d->limiter_color  = lv_color_hex(ARC_DEFAULT_LIMITER_COLOR);
    d->flash_speed_ms = ARC_DEFAULT_FLASH_MS;
    d->flash_timer    = NULL;
    d->flash_phase    = false;
    d->in_limiter     = false;

    /* Value text defaults */
    d->show_value     = false;
    d->value_color    = lv_color_hex(ARC_DEFAULT_VALUE_COLOR);
    d->value_y_offset = 0;
    d->value_decimals = 0;
    /* arc_image, arc_image_full, signal_name, value_font, value_unit
     * zeroed by calloc */

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
