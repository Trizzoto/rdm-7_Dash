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
#include "data/channel_manager.h"
#include "signal.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "widget_types.h"

#include <math.h>
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

/* ── Color-alert defaults (mirror widget_bar). Alerts OFF by default so
 * existing layouts are unaffected. Thresholds come from the bound channel. */
#define ARC_DEFAULT_ALERTS_ENABLED  false
#define ARC_DEFAULT_ALERT_LOW       0.0f
#define ARC_DEFAULT_ALERT_HIGH      100.0f
#define ARC_DEFAULT_LOW_COLOR      0x0000FF
#define ARC_DEFAULT_HIGH_COLOR     0xFF0000

/* ── Tick / value-line / anchor / reverse defaults (meter-parity). All
 * default so existing layouts/perf are unchanged: ticks + value-line OFF,
 * anchor + reverse OFF. The numeric tick defaults mirror widget_meter. */
#define ARC_DEFAULT_SHOW_TICKS         false
#define ARC_DEFAULT_MINOR_TICK_COUNT   21
#define ARC_DEFAULT_MAJOR_TICK_EVERY   5
#define ARC_DEFAULT_MINOR_TICK_LENGTH  10
#define ARC_DEFAULT_MINOR_TICK_WIDTH   2
#define ARC_DEFAULT_MAJOR_TICK_LENGTH  15
#define ARC_DEFAULT_MAJOR_TICK_WIDTH   4
#define ARC_DEFAULT_MINOR_TICK_COLOR   0x9E9E9E
#define ARC_DEFAULT_MAJOR_TICK_COLOR   0xFFFFFF
#define ARC_DEFAULT_TICKS_OUTSIDE      false
#define ARC_DEFAULT_SHOW_TICK_LABELS   true
#define ARC_DEFAULT_LABEL_GAP          10
#define ARC_DEFAULT_TICK_LABEL_COLOR   0xFFFFFF
#define ARC_DEFAULT_TICK_LABEL_DIVISOR 1
#define ARC_DEFAULT_SHOW_VALUE_LINE    false
#define ARC_DEFAULT_VALUE_LINE_WIDTH   4
#define ARC_DEFAULT_VALUE_LINE_COLOR   0xFFFFFF
#define ARC_DEFAULT_VALUE_LINE_R_MOD   (-10)
#define ARC_DEFAULT_ANCHOR_VALUE       50.0f
#define ARC_DEFAULT_ANCHOR_POSITION    50

/* Forward declarations */
static void _arc_on_signal(float value, bool is_stale, void *user_data);
static void _arc_apply_night_mode(widget_t *w, bool active);
static void _arc_night_cb(bool active, void *user_data);
static void _arc_apply_fill_color(arc_data_t *d, bool active);
static void _arc_flash_timer_cb(lv_timer_t *t);
static void _arc_update_value_label(arc_data_t *d, float value);
static void _arc_recompute_value(widget_t *w, float value, bool is_stale);
static void _arc_build_overlay(arc_data_t *d, lv_obj_t *cont,
                               lv_coord_t ow, lv_coord_t oh, bool night_active);
static void _arc_drive_value_needle(arc_data_t *d, float value);
static void _arc_tick_draw_cb(lv_event_t *e);

/* ── Helpers: mode detection ───────────────────────────────────────────── */

static bool _is_image_mode(const arc_data_t *d) {
    return d->arc_image[0] != '\0' && d->arc_image_full[0] != '\0';
}

static bool _is_static_image_mode(const arc_data_t *d) {
    return d->arc_image[0] != '\0' && d->arc_image_full[0] == '\0';
}

/* ── Helper: track inset for "ticks outside" mode ───────────────────────────
 * When show_ticks AND ticks_outside are both set, the tick overlay stays at the
 * full rim and the arc track is pushed INWARD by this many px on every side so
 * the track sits inside the tick ring. Returns 0 (no inset = current behavior)
 * otherwise. The inset is the longer tick length plus a gap (with extra room
 * for numeric labels when they're shown). STANDARD mode only — the inset is
 * applied to the arc + redline arc, never to the overlay meter. */
static int _arc_track_inset(const arc_data_t *d) {
    if (!d->show_ticks || !d->ticks_outside) return 0;
    int tl = d->major_tick_length > d->minor_tick_length
             ? d->major_tick_length : d->minor_tick_length;
    return tl + (d->show_tick_labels ? 16 : 4);   /* tick length + gap (+ label room) */
}

/* Clamp an inset arc dimension so a tiny widget never gets a non-positive
 * (or absurdly small) size — keep at least 20 px after the 2*inset subtract. */
static lv_coord_t _arc_inset_dim(lv_coord_t full, int inset) {
    lv_coord_t v = (lv_coord_t)(full - 2 * inset);
    if (v < 20) v = 20;
    return v;
}

/* ── Helpers: anchor + reverse value transform (ported from widget_meter) ──
 * Apply the anchor curve to a value, returning a value on a LINEAR
 * [signal_min, signal_max] axis whose pct lands `anchor_value` at
 * `anchor_position`% of the sweep — two linear segments. No-op when
 * anchor_enabled is false. Identical math to _meter_apply_anchor. */
static float _arc_apply_anchor(const arc_data_t *d, float v) {
    if (!d->anchor_enabled) return v;
    float lo = d->signal_min;
    float hi = d->signal_max;
    if (hi <= lo) return v;
    float a = d->anchor_value;
    if (a <= lo || a >= hi) return v;
    int32_t pos = d->anchor_position;
    if (pos <= 0)   pos = 0;
    if (pos >= 100) pos = 100;
    float range = hi - lo;
    float pivot = lo + (range * (float)pos) / 100.0f;
    if (v <= a) {
        float span = a - lo;
        if (span <= 0.0f) return lo;
        return lo + (v - lo) * (pivot - lo) / span;
    } else {
        float span = hi - a;
        if (span <= 0.0f) return hi;
        return pivot + (v - a) * (hi - pivot) / span;
    }
}

/* Apply anchor THEN reverse (same order as the meter) to a clamped value,
 * yielding the value to feed to the linear fill / image-clip / value-needle.
 * Clamps to [signal_min, signal_max] on the way out. */
static float _arc_transform_value(const arc_data_t *d, float v) {
    if (v < d->signal_min) v = d->signal_min;
    if (v > d->signal_max) v = d->signal_max;
    v = _arc_apply_anchor(d, v);
    if (d->reverse) v = d->signal_min + d->signal_max - v;
    return v;
}

/* ── Helpers: image-mode clip width update ─────────────────────────────── */

static void _update_image_clip(widget_t *w, float value) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d || !d->img_clip_obj) return;

    /* Anchor + reverse first, then linear pct. */
    float tv = _arc_transform_value(d, value);
    float range = d->signal_max - d->signal_min;
    if (range <= 0.0f) range = 100.0f;
    float pct = (tv - d->signal_min) / range;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    lv_coord_t clip_w = (lv_coord_t)(pct * (float)w->w);
    lv_obj_set_width(d->img_clip_obj, clip_w);
}

/* ── Helpers: standard-arc value update ────────────────────────────────── */

static void _update_arc_value(arc_data_t *d, float value) {
    if (!d || !d->arc_obj) return;

    /* Anchor + reverse first, then linear pct over the configured range. */
    float tv = _arc_transform_value(d, value);
    float range = d->signal_max - d->signal_min;
    if (range <= 0.0f) range = 100.0f;
    float pct = (tv - d->signal_min) / range;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    lv_arc_set_value(d->arc_obj, (int16_t)(pct * 100.0f));
}

/* Drive the overlay value-line needle to `value`. Applies the SAME anchor +
 * reverse transform as the fill so the needle and fill agree, then clamps and
 * pushes the (integer) value into the overlay meter. No-op when the overlay /
 * needle don't exist (ticks-only overlay, image mode, or feature off). */
static void _arc_drive_value_needle(arc_data_t *d, float value) {
    if (!d || !d->tick_meter || !d->value_needle) return;
    if (!lv_obj_is_valid(d->tick_meter)) return;
    float tv = _arc_transform_value(d, value);
    lv_meter_set_indicator_value(d->tick_meter, d->value_needle,
                                  (int32_t)lroundf(tv));
}

/* Recolor the indicator arc based on whether we're in the redline / limiter
 * zone and (for flash effect) the current flash phase. `active` is the
 * current night-mode state — used to pick night-overridden colors. */
static void _arc_apply_fill_color(arc_data_t *d, bool active) {
    if (!d || !d->arc_obj || !lv_obj_is_valid(d->arc_obj)) return;

    /* Pick the base "normal" fill color.
     *   - If a widget_rule has overridden arc_color (cached in
     *     _rule_arc_color), it wins over both the default and the night
     *     override. The rule is the strongest non-zone signal of intent.
     *   - Otherwise: night override if active, else default arc_color. */
    lv_color_t normal = d->_rule_arc_color_set
        ? d->_rule_arc_color
        : NIGHT_PICK_COLOR(active, d->night, arc_color, d->arc_color);
    /* Multi-stop value-walked gradient: sample stops at t and replace
     * the normal colour wholesale. Stops are absolute (don't inherit
     * from arc_color / rule overrides / night) — see widget_arc.h
     * for rationale. Rule overrides and night-mode still apply when
     * no gradient is configured (count == 0). */
    if (d->grad_stops.count >= 2 && d->signal_max > d->signal_min) {
        float t = (d->_cached_value - d->signal_min) /
                  (d->signal_max - d->signal_min);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        normal.full = gradient_stops_sample(&d->grad_stops, t);
    }
    lv_color_t redline = NIGHT_PICK_COLOR(active, d->night, redline_color, d->redline_color);
    lv_color_t fill = normal;

    /* Order of precedence (highest first):
     *   1. Limiter Solid  → limiter_color overrides everything
     *   2. Limiter Flash  → toggle between normal and limiter_color
     *   3. Color alerts   → arc_low_color below arc_low / arc_high_color above
     *                       arc_high (thresholds owned by the bound channel)
     *   4. Redline zone   → recolor with redline_color (if recolor_fill on)
     *   5. Default        → normal arc_color
     * (A rule-overridden arc_color is already folded into `normal` above, so it
     *  outranks redline/normal but yields to limiter/alerts — same as the
     *  rule-fg behaviour the rest of this function assumes.)
     * Alerts sit ABOVE redline so a configured low/high alert wins on the high
     * side instead of the redline fill-recolor. Whichever branch wins gets
     * applied to LV_PART_INDICATOR. */
    if (d->in_limiter && d->limiter_effect == 2) {
        fill = d->limiter_color;
    } else if (d->in_limiter && d->limiter_effect == 1) {
        fill = d->flash_phase ? d->limiter_color : normal;
    } else if (d->arc_alerts_enabled &&
               (d->_cached_value <= d->arc_low || d->_cached_value >= d->arc_high)) {
        if (d->_cached_value <= d->arc_low)
            fill = NIGHT_PICK_COLOR(active, d->night, arc_low_color, d->arc_low_color);
        else
            fill = NIGHT_PICK_COLOR(active, d->night, arc_high_color, d->arc_high_color);
    } else if (d->redline_enabled && d->redline_recolor_fill) {
        /* "In zone" detection re-uses the in_limiter result when limiter
         * is at the same threshold; otherwise check threshold directly. */
        if (d->_cached_value >= d->redline_threshold) {
            fill = redline;
        }
    }

    /* Skip the write when the computed fill is unchanged — LVGL v8
     * set_style_arc_color invalidates the whole arc ring every call. */
    if (d->_last_fill_valid && d->_last_fill.full == fill.full) return;
    lv_obj_set_style_arc_color(d->arc_obj, fill, LV_PART_INDICATOR);
    d->_last_fill = fill;
    d->_last_fill_valid = true;
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
    /* Try the signal's value→label map first (gear positions, drive
     * modes, etc.). On a hit we skip the unit suffix — labels like "N"
     * or "Sport" stand alone. Numeric fallback keeps the existing unit
     * concatenation behaviour. */
    const char *lbl = signal_value_lookup_label(d->signal_index, value);
    if (lbl) {
        snprintf(buf, sizeof(buf), "%s", lbl);
    } else if (d->value_decimals == 0) {
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
    /* Skip the realloc + invalidate when the rendered string is unchanged —
     * lv_label_set_text invalidates the label bbox before any content diff. */
    if (d->_last_label_valid && strcmp(d->_last_label, buf) == 0) return;
    lv_label_set_text(d->value_label, buf);
    safe_strncpy(d->_last_label, buf, sizeof(d->_last_label));
    d->_last_label_valid = true;
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
        _arc_drive_value_needle(d, d->signal_min);
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
    _arc_drive_value_needle(d, value);

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

/* Channel-changed listener — snapshot channel fields and invalidate. */
static void _arc_on_channel_changed(channel_t *c, void *user_data) {
    if (!c || !user_data) return;
    widget_t *w = (widget_t *)user_data;
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d) return;
    safe_strncpy(d->signal_name, c->signal_name, sizeof(d->signal_name));
    /* Re-point our own signal subscription when the channel re-binds to a new
       source — copying the index alone leaves _arc_on_signal attached to the
       OLD index, so the gauge would freeze on its last value. Mirrors the
       inspector signal_name rebind path. Safe: runs under the LVGL mutex. */
    int16_t new_idx = c->signal_index;
    if (new_idx != d->signal_index) {
        if (d->signal_index >= 0)
            signal_unsubscribe(d->signal_index, _arc_on_signal, w);
        d->signal_index = new_idx;
        if (new_idx >= 0)
            signal_subscribe(new_idx, _arc_on_signal, w);
    }
    d->signal_min = (float)c->min;
    d->signal_max = (float)c->max;
    if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
        d->redline_enabled = true;
        d->redline_threshold = (float)c->high_warn;
    } else {
        d->redline_enabled = false;
    }
    /* Channel owns the alert thresholds (same as the bar). When a side has no
     * warn set, park it at the range edge so that alert can never fire
     * (reverts to inactive) instead of leaving a stale value mid-range. Alert
     * COLOURS stay widget-owned — never driven by the channel. */
    d->arc_low  = (c->low_warn  != CHANNEL_THRESHOLD_UNSET_LOW)  ? (float)c->low_warn  : d->signal_min;
    d->arc_high = (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) ? (float)c->high_warn : d->signal_max;
    /* Redline colour is widget-owned — never driven by the channel. */
    /* Thresholds moved → re-apply the fill so the alert recolor reflects the
     * new buckets immediately (a parked signal produces no further tick). */
    _arc_apply_fill_color(d, night_mode_is_active());
    if (w->root && lv_obj_is_valid(w->root)) lv_obj_invalidate(w->root);
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
 * position the redline zone marker. Honours reverse: with reverse on, high
 * values sit at the START of the sweep, so the pct is mirrored (1-pct) to
 * keep the marker aligned with the (mirrored) fill. Anchor is intentionally
 * NOT applied here — the redline marker arc spans a fixed angular slice and
 * we only need the threshold's angular position; threshold compares elsewhere
 * use the raw value. */
static int16_t _value_to_angle(const arc_data_t *d, float value) {
    float range = d->signal_max - d->signal_min;
    if (range <= 0.0f) range = 100.0f;
    float pct = (value - d->signal_min) / range;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    if (d->reverse) pct = 1.0f - pct;

    /* Sweep is from start_angle to end_angle going clockwise (LVGL
     * convention). Wrap if end < start. */
    int32_t sweep = (360 + d->end_angle - d->start_angle) % 360;
    if (sweep == 0 && d->start_angle != d->end_angle) sweep = 360;
    return (int16_t)(d->start_angle + (int32_t)(pct * (float)sweep));
}

/* Compute the value to DISPLAY at a major tick whose linear (LVGL) value is
 * `linear_v`, given the tick's fractional position `pct` (0..100) along the
 * sweep. Ported from widget_meter's relabel logic:
 *   - No anchor / no reverse → the linear value is already what we want.
 *   - Anchor and/or reverse on → recompute from pct so the printed number
 *     matches the WARPED fill. The overlay scale is laid out linearly by
 *     LVGL, but the fill / needle are driven through _arc_transform_value
 *     (anchor THEN reverse). To label the warped axis we invert that order:
 *     mirror pct first when reverse is on, then run the (forward) anchor
 *     curve over the pct to get the value the user would read at that spot.
 * The anchor curve here mirrors _meter_value_for_angle_pct. */
static float _arc_value_for_tick(const arc_data_t *d, float linear_v, int32_t pct) {
    if (!d->anchor_enabled && !d->reverse) return linear_v;
    if (d->signal_max <= d->signal_min) return d->signal_min;
    if (d->reverse) pct = 100 - pct;
    if (pct <= 0)   return d->signal_min;
    if (pct >= 100) return d->signal_max;
    if (!d->anchor_enabled) {
        return d->signal_min +
               (d->signal_max - d->signal_min) * (float)pct / 100.0f;
    }
    int32_t ap = d->anchor_position;
    if (ap < 0)   ap = 0;
    if (ap > 100) ap = 100;
    if (pct <= ap) {
        if (ap == 0) return d->signal_min;
        return d->signal_min +
               (d->anchor_value - d->signal_min) * (float)pct / (float)ap;
    }
    int32_t hp = 100 - ap;
    if (hp == 0) return d->signal_max;
    return d->anchor_value +
           (d->signal_max - d->anchor_value) * (float)(pct - ap) / (float)hp;
}

/* DRAW_PART_BEGIN hook on the overlay lv_meter — relabels the major-tick
 * numeric labels using tick_label_divisor / value_decimals and the same
 * anchor / reverse warp the fill uses, so the printed numbers line up with the
 * warped fill (mirrors widget_meter's LV_METER_DRAW_PART_TICK relabel). Only
 * touches TICK parts that carry a label; everything else passes through.
 *
 * The overlay scale range is the RAW signal_min..signal_max (no value_scale),
 * so dsc->value is already the real value at the tick — no /value_scale needed
 * (unlike the meter, whose scale is value*10^decimals). */
static void _arc_tick_draw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_PART_BEGIN) return;
    arc_data_t *d = (arc_data_t *)lv_event_get_user_data(e);
    if (!d) return;
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (!dsc || dsc->type != LV_METER_DRAW_PART_TICK) return;
    /* Only major ticks carry a label_dsc + text buffer; minor ticks have none. */
    if (dsc->label_dsc == NULL || dsc->text == NULL) return;

    /* Value the user should read at this tick (anchor / reverse aware). */
    float shown = dsc->value;
    if (d->anchor_enabled || d->reverse) {
        uint16_t total = (d->minor_tick_count > 1)
                         ? (uint16_t)(d->minor_tick_count - 1) : 1;
        int32_t pct = (int32_t)(((int64_t)dsc->id * 100) / (int64_t)total);
        shown = _arc_value_for_tick(d, (float)dsc->value, pct);
    }

    uint16_t div = d->tick_label_divisor > 0 ? d->tick_label_divisor : 1;
    float display = shown / (float)div;
    if (d->value_decimals > 0) {
        lv_snprintf(dsc->text, 16, "%.*f", (int)d->value_decimals, display);
    } else {
        lv_snprintf(dsc->text, 16, "%d", (int)lroundf(display));
    }
}

/* ── Overlay meter: ticks + value-line needle ──────────────────────────────
 * lv_arc has no tick API in LVGL v8, so when show_ticks and/or
 * show_value_line are set in STANDARD mode we drop a transparent lv_meter
 * sibling into the arc container. Its scale spans signal_min..signal_max over
 * the SAME angle span as the arc fill (computed from start_angle/end_angle the
 * same way the fill does), so ticks and the value-line align with the fill.
 *
 * The overlay is created sized + centered to match the arc, with its bg +
 * border transparent and its center indicator ball hidden. It's moved to the
 * BACK of the container (child index 0) so the arc fill renders ON TOP.
 *
 * Tick + value-line colours are baked into lv_meter at create time (v8
 * limitation), so night-mode picks the night colour here based on
 * `night_active`; _arc_apply_night_mode rebuilds the overlay when one of those
 * night overrides is set.
 *
 * Frees: the overlay is a child of `cont`, so the w->root delete cascade frees
 * it. Rebuild paths delete the old overlay first (see _arc_rebuild_overlay). */
static void _arc_build_overlay(arc_data_t *d, lv_obj_t *cont,
                               lv_coord_t ow, lv_coord_t oh, bool night_active) {
    if (!d || !cont) return;
    if (!d->show_ticks && !d->show_value_line) return;

    lv_obj_t *m = lv_meter_create(cont);
    if (!m) return;
    lv_obj_set_size(m, ow, oh);
    lv_obj_set_align(m, LV_ALIGN_CENTER);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    /* Transparent shell — only the tick ring (+ optional needle) shows. */
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(m, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(m, 0, LV_PART_MAIN);
    /* Hide the meter's center indicator ball. */
    lv_obj_set_style_size(m, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_INDICATOR);

    lv_meter_scale_t *scale = lv_meter_add_scale(m);
    /* Angle span: identical computation to _value_to_angle's sweep. */
    int32_t angle_range = (360 + (d->end_angle % 360) - (d->start_angle % 360)) % 360;
    if (angle_range == 0 && d->start_angle != d->end_angle) angle_range = 360;
    lv_meter_set_scale_range(m, scale,
                             (int32_t)lroundf(d->signal_min),
                             (int32_t)lroundf(d->signal_max),
                             angle_range, (int32_t)d->start_angle);

    /* Tick marks. When show_ticks is off but a value-line is wanted, the
     * scale still needs a (zero-width) tick setup so the needle's angle math
     * works — mirror the meter's "zero the widths" approach. */
    uint8_t mtc = d->minor_tick_count < 2 ? 2 : d->minor_tick_count;
    uint8_t mte = d->major_tick_every < 1 ? 1 : d->major_tick_every;
    lv_color_t mintc = NIGHT_PICK_COLOR(night_active, d->night, minor_tick_color, d->minor_tick_color);
    lv_color_t majtc = NIGHT_PICK_COLOR(night_active, d->night, major_tick_color, d->major_tick_color);
    uint8_t minor_w = d->show_ticks ? d->minor_tick_width  : 0;
    uint8_t minor_l = d->show_ticks ? d->minor_tick_length : 0;
    uint8_t major_w = d->show_ticks ? d->major_tick_width  : 0;
    uint8_t major_l = d->show_ticks ? d->major_tick_length : 0;
    lv_meter_set_scale_ticks(m, scale, mtc, minor_w, minor_l, mintc);
    /* Numeric tick labels (meter-parity). When ticks AND labels are both on,
     * draw the labels: pass the real label_gap, bake the label colour/font
     * into LV_PART_TICKS, and register a DRAW_PART_BEGIN relabel hook that
     * applies the divisor / decimals / anchor / reverse. Otherwise keep the
     * marks-only behaviour: gap 0 + transparent label opacity so the overlay
     * draws tick MARKS only (the arc has its own centered value label). */
    bool want_labels = d->show_ticks && d->show_tick_labels;
    if (want_labels) {
        lv_meter_set_scale_major_ticks(m, scale, mte, major_w, major_l, majtc,
                                       d->label_gap);
        lv_color_t tlc = NIGHT_PICK_COLOR(night_active, d->night,
                                          tick_label_color, d->tick_label_color);
        lv_obj_set_style_text_color(m, tlc, LV_PART_TICKS);
        if (d->tick_label_font[0] != '\0') {
            const lv_font_t *tfont = widget_resolve_font(d->tick_label_font);
            if (tfont) lv_obj_set_style_text_font(m, tfont, LV_PART_TICKS);
        }
        /* Relabel hook — fires per tick at DRAW_PART_BEGIN. Pass the
         * arc_data_t* so the cb can read divisor / decimals / anchor /
         * reverse without a widget_t round-trip. */
        lv_obj_add_event_cb(m, _arc_tick_draw_cb, LV_EVENT_DRAW_PART_BEGIN, d);
    } else {
        lv_meter_set_scale_major_ticks(m, scale, mte, major_w, major_l, majtc, 0);
        /* Suppress numeric tick labels entirely. */
        lv_obj_set_style_text_opa(m, LV_OPA_TRANSP, LV_PART_TICKS);
    }

    /* Value-line needle. */
    if (d->show_value_line) {
        lv_color_t vlc = NIGHT_PICK_COLOR(night_active, d->night, value_line_color, d->value_line_color);
        d->value_needle = lv_meter_add_needle_line(m, scale,
                                                    d->value_line_width,
                                                    vlc, d->value_line_r_mod);
        /* Snap to the bound signal's current value (anchor+reverse applied
         * inside _arc_drive_value_needle); fall back to signal_min. */
        float init = d->signal_min;
        if (d->signal_index >= 0) {
            signal_t *sig = signal_get_by_index((uint16_t)d->signal_index);
            if (sig && !sig->is_stale) init = sig->current_value;
        }
        float tv = _arc_transform_value(d, init);
        lv_meter_set_indicator_value(m, d->value_needle, (int32_t)lroundf(tv));
    }

    d->tick_meter = m;
    d->tick_scale = scale;

    /* Draw UNDER the arc fill: move the overlay to the back of the container
     * so the (later-created or already-created) arc renders on top. */
    lv_obj_move_background(m);
}

/* Tear down + rebuild the overlay meter in place. Used by the night-apply
 * path because tick / value-line colours are baked in at create time
 * (LVGL v8 has no live tick/needle recolor). The overlay is a single cheap
 * child, so a full delete + rebuild is fine. Re-asserts back-of-container
 * z-order so the arc fill stays on top. */
static void _arc_rebuild_overlay(widget_t *w, bool night_active) {
    arc_data_t *d = (arc_data_t *)w->type_data;
    if (!d || !w->root || !lv_obj_is_valid(w->root)) return;
    /* Only meaningful in standard mode (overlay never built in image modes). */
    if (!d->arc_obj) return;
    if (d->tick_meter && lv_obj_is_valid(d->tick_meter))
        lv_obj_del(d->tick_meter);
    d->tick_meter   = NULL;
    d->tick_scale   = NULL;
    d->value_needle = NULL;
    _arc_build_overlay(d, w->root, (lv_coord_t)w->w, (lv_coord_t)w->h, night_active);
    /* Push the freshly-built value-needle to the cached value. */
    _arc_drive_value_needle(d, d->_cached_value);
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

    /* Inset for "ticks outside" mode: the overlay meter stays at the full rim
     * (w->w × w->h) while the arc + redline arc are pushed inward so the track
     * sits inside the tick ring. 0 = current behavior. */
    int ins = _arc_track_inset(d);

    /* Main moving arc. */
    lv_obj_t *obj = lv_arc_create(cont);
    lv_obj_set_size(obj, _arc_inset_dim((lv_coord_t)w->w, ins),
                         _arc_inset_dim((lv_coord_t)w->h, ins));
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
        lv_obj_set_size(robj, _arc_inset_dim((lv_coord_t)w->w, ins),
                              _arc_inset_dim((lv_coord_t)w->h, ins));
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

    /* Overlay meter for ticks + value-line needle. Built AFTER the arc/redline/
     * label and then moved to the back (inside _arc_build_overlay) so the arc
     * fill renders on top of the tick ring. Night colour is picked here so a
     * widget created while night mode is already active bakes the right tick /
     * value-line colours. */
    _arc_build_overlay(d, cont, (lv_coord_t)w->w, (lv_coord_t)w->h,
                       night_mode_is_active());

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
    d->tick_meter      = NULL;
    d->tick_scale      = NULL;
    d->value_needle    = NULL;
    d->flash_timer     = NULL;
    d->flash_phase     = false;
    d->in_limiter      = false;
    d->_cached_value   = d->signal_min;
    d->_last_fill_valid  = false;  /* paint memo must not gate the first paint */
    d->_last_label_valid = false;  /* of a freshly (re)built arc_obj/value_label */

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

    if (d->channel)
        channel_manager_subscribe((channel_t *)d->channel,
                                   _arc_on_channel_changed, w);

    /* Subscribe rules (safe no-op if no rules defined) */
    widget_rules_subscribe(w);

    /* Subscribe to night-mode changes if any night override is set, and apply
     * current state immediately so the widget renders correctly even if it
     * was created while night-mode is already active. */
    if (d->night.has_arc_color || d->night.has_bg_arc_color ||
        d->night.has_value_color || d->night.has_redline_color ||
        d->night.has_arc_low_color || d->night.has_arc_high_color ||
        d->night.has_minor_tick_color || d->night.has_major_tick_color ||
        d->night.has_tick_label_color ||
        d->night.has_value_line_color ||
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
    /* Also resize the arc child(ren) so they fill the new container. The arc +
     * redline arc get the "ticks outside" inset; the overlay meter stays full
     * size so ticks remain at the rim. */
    if (d) {
        int ins = _arc_track_inset(d);
        lv_coord_t aw = _arc_inset_dim((lv_coord_t)nw, ins);
        lv_coord_t ah = _arc_inset_dim((lv_coord_t)nh, ins);
        if (d->arc_obj && lv_obj_is_valid(d->arc_obj))
            lv_obj_set_size(d->arc_obj, aw, ah);
        if (d->redline_arc_obj && lv_obj_is_valid(d->redline_arc_obj))
            lv_obj_set_size(d->redline_arc_obj, aw, ah);
        /* Overlay tick/value-line meter tracks the FULL container size so ticks
         * stay at the rim (radius is independent of the inset arc). The scale's
         * angle math is size-independent, so just resizing is enough — no
         * rebuild. */
        if (d->tick_meter && lv_obj_is_valid(d->tick_meter))
            lv_obj_set_size(d->tick_meter, nw, nh);
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
    {
        /* Multi-stop gradient. Emitted only when ≥2 stops are configured —
         * mirrors the defaults-only pattern used elsewhere. Old layouts'
         * grad_enabled/grad_end_color fields are dropped on save. */
        cJSON *gs = gradient_stops_to_json(&d->grad_stops);
        if (gs) cJSON_AddItemToObject(cfg, "grad_stops", gs);
    }
    if (d->bg_arc_color.full != lv_color_hex(ARC_DEFAULT_BG_COLOR).full)
        cJSON_AddNumberToObject(cfg, "bg_arc_color", (int)d->bg_arc_color.full);
    if (d->bg_arc_width != ARC_DEFAULT_BG_WIDTH)
        cJSON_AddNumberToObject(cfg, "bg_arc_width", d->bg_arc_width);
    if (d->rounded_ends != ARC_DEFAULT_ROUNDED)
        cJSON_AddBoolToObject(cfg, "rounded_ends", d->rounded_ends);

    /* Signal binding */
    if (d->signal_name[0] != '\0')
        cJSON_AddStringToObject(cfg, "signal_name", d->signal_name);
    if (d->channel_id[0] != '\0')
        cJSON_AddStringToObject(cfg, "channel", d->channel_id);
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

    /* Color alerts — defaults-only. arc_low / arc_high (the THRESHOLDS) live on
     * the bound channel (channels.json) and are intentionally NOT persisted
     * here, so they stick to the channel setup and don't travel when a layout
     * is shared (same as the bar's bar_low / bar_high). Alert COLOURS are
     * widget-owned styling and DO round-trip. */
    if (d->arc_alerts_enabled)
        cJSON_AddBoolToObject(cfg, "arc_alerts_enabled", true);
    if (d->arc_low_color.full != lv_color_hex(ARC_DEFAULT_LOW_COLOR).full)
        cJSON_AddNumberToObject(cfg, "arc_low_color", (int)d->arc_low_color.full);
    if (d->arc_high_color.full != lv_color_hex(ARC_DEFAULT_HIGH_COLOR).full)
        cJSON_AddNumberToObject(cfg, "arc_high_color", (int)d->arc_high_color.full);

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

    /* Ticks (overlay meter) — defaults-only; bool only when true. */
    if (d->show_ticks)
        cJSON_AddBoolToObject(cfg, "show_ticks", true);
    if (d->minor_tick_count != ARC_DEFAULT_MINOR_TICK_COUNT)
        cJSON_AddNumberToObject(cfg, "minor_tick_count", d->minor_tick_count);
    if (d->major_tick_every != ARC_DEFAULT_MAJOR_TICK_EVERY)
        cJSON_AddNumberToObject(cfg, "major_tick_every", d->major_tick_every);
    if (d->minor_tick_length != ARC_DEFAULT_MINOR_TICK_LENGTH)
        cJSON_AddNumberToObject(cfg, "minor_tick_length", d->minor_tick_length);
    if (d->minor_tick_width != ARC_DEFAULT_MINOR_TICK_WIDTH)
        cJSON_AddNumberToObject(cfg, "minor_tick_width", d->minor_tick_width);
    if (d->major_tick_length != ARC_DEFAULT_MAJOR_TICK_LENGTH)
        cJSON_AddNumberToObject(cfg, "major_tick_length", d->major_tick_length);
    if (d->major_tick_width != ARC_DEFAULT_MAJOR_TICK_WIDTH)
        cJSON_AddNumberToObject(cfg, "major_tick_width", d->major_tick_width);
    if (d->minor_tick_color.full != lv_color_hex(ARC_DEFAULT_MINOR_TICK_COLOR).full)
        cJSON_AddNumberToObject(cfg, "minor_tick_color", (int)d->minor_tick_color.full);
    if (d->major_tick_color.full != lv_color_hex(ARC_DEFAULT_MAJOR_TICK_COLOR).full)
        cJSON_AddNumberToObject(cfg, "major_tick_color", (int)d->major_tick_color.full);
    if (d->ticks_outside != ARC_DEFAULT_TICKS_OUTSIDE)
        cJSON_AddBoolToObject(cfg, "ticks_outside", d->ticks_outside);

    /* Numeric tick labels — default ON, so emit the bool only when FALSE. */
    if (!d->show_tick_labels)
        cJSON_AddBoolToObject(cfg, "show_tick_labels", false);
    if (d->label_gap != ARC_DEFAULT_LABEL_GAP)
        cJSON_AddNumberToObject(cfg, "label_gap", d->label_gap);
    if (d->tick_label_font[0] != '\0')
        cJSON_AddStringToObject(cfg, "tick_label_font", d->tick_label_font);
    if (d->tick_label_color.full != lv_color_hex(ARC_DEFAULT_TICK_LABEL_COLOR).full)
        cJSON_AddNumberToObject(cfg, "tick_label_color", (int)d->tick_label_color.full);
    if (d->tick_label_divisor != ARC_DEFAULT_TICK_LABEL_DIVISOR)
        cJSON_AddNumberToObject(cfg, "tick_label_divisor", d->tick_label_divisor);

    /* Value line (overlay needle) */
    if (d->show_value_line)
        cJSON_AddBoolToObject(cfg, "show_value_line", true);
    if (d->value_line_width != ARC_DEFAULT_VALUE_LINE_WIDTH)
        cJSON_AddNumberToObject(cfg, "value_line_width", d->value_line_width);
    if (d->value_line_color.full != lv_color_hex(ARC_DEFAULT_VALUE_LINE_COLOR).full)
        cJSON_AddNumberToObject(cfg, "value_line_color", (int)d->value_line_color.full);
    if (d->value_line_r_mod != ARC_DEFAULT_VALUE_LINE_R_MOD)
        cJSON_AddNumberToObject(cfg, "value_line_r_mod", d->value_line_r_mod);

    /* Anchor curve */
    if (d->anchor_enabled)
        cJSON_AddBoolToObject(cfg, "anchor_enabled", true);
    if (d->anchor_value != ARC_DEFAULT_ANCHOR_VALUE)
        cJSON_AddNumberToObject(cfg, "anchor_value", (double)d->anchor_value);
    if (d->anchor_position != ARC_DEFAULT_ANCHOR_POSITION)
        cJSON_AddNumberToObject(cfg, "anchor_position", d->anchor_position);

    /* Reverse */
    if (d->reverse)
        cJSON_AddBoolToObject(cfg, "reverse", true);

    /* Rules */
    widget_rules_to_json(w, cfg);

    /* Night-mode overrides — emit only fields that have an override set */
    {
        cJSON *n = cJSON_CreateObject();
        NIGHT_SERIALIZE_COLOR(n, d->night, arc_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, bg_arc_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, value_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, redline_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, arc_low_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, arc_high_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, minor_tick_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, major_tick_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, tick_label_color);
        NIGHT_SERIALIZE_COLOR(n, d->night, value_line_color);
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

    /* Multi-stop gradient: prefer the new grad_stops array; fall back to
     * the legacy 2-stop schema (grad_enabled + grad_end_color) so layouts
     * saved before this revision keep rendering with the same visual. */
    const cJSON *gs_arr = cJSON_GetObjectItemCaseSensitive(cfg, "grad_stops");
    if (!gradient_stops_from_json(gs_arr, &d->grad_stops)) {
        const cJSON *ge  = cJSON_GetObjectItemCaseSensitive(cfg, "grad_enabled");
        const cJSON *gec = cJSON_GetObjectItemCaseSensitive(cfg, "grad_end_color");
        if (cJSON_IsBool(ge) && cJSON_IsTrue(ge) && cJSON_IsNumber(gec)) {
            gradient_stops_install_legacy_2stop(&d->grad_stops,
                d->arc_color.full, (uint16_t)gec->valueint);
        }
    }

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

    /* Color alerts — enable flag + colours only. Thresholds (arc_low /
     * arc_high) are NOT read here; they're adopted from the bound channel
     * below (single source of truth, same as the bar). */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_alerts_enabled");
    if (cJSON_IsBool(item)) d->arc_alerts_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_low_color");
    if (cJSON_IsNumber(item)) d->arc_low_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "arc_high_color");
    if (cJSON_IsNumber(item)) d->arc_high_color.full = (uint16_t)item->valueint;

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

    /* Ticks (overlay lv_meter) */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "show_ticks");
    if (cJSON_IsBool(item)) d->show_ticks = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_count");
    if (cJSON_IsNumber(item)) d->minor_tick_count = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_every");
    if (cJSON_IsNumber(item)) d->major_tick_every = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_length");
    if (cJSON_IsNumber(item)) d->minor_tick_length = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_width");
    if (cJSON_IsNumber(item)) d->minor_tick_width = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_length");
    if (cJSON_IsNumber(item)) d->major_tick_length = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_width");
    if (cJSON_IsNumber(item)) d->major_tick_width = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "minor_tick_color");
    if (cJSON_IsNumber(item)) d->minor_tick_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "major_tick_color");
    if (cJSON_IsNumber(item)) d->major_tick_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "ticks_outside");
    if (cJSON_IsBool(item)) d->ticks_outside = cJSON_IsTrue(item);

    /* Numeric tick labels */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "show_tick_labels");
    if (cJSON_IsBool(item)) d->show_tick_labels = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "label_gap");
    if (cJSON_IsNumber(item)) d->label_gap = (int16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_font");
    if (cJSON_IsString(item) && item->valuestring)
        safe_strncpy(d->tick_label_font, item->valuestring, sizeof(d->tick_label_font));
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_color");
    if (cJSON_IsNumber(item)) d->tick_label_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "tick_label_divisor");
    if (cJSON_IsNumber(item)) {
        /* tick_label_divisor is stored as uint16_t. The schema advertises a
         * max of 100000, but that overflows uint16_t (65535) — clamp to the
         * storage range so a too-large value can't wrap to a tiny divisor. */
        int v = item->valueint;
        if (v < 1)     v = 1;
        if (v > 65535) v = 65535;
        d->tick_label_divisor = (uint16_t)v;
    }

    /* Value line (needle on the overlay meter) */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "show_value_line");
    if (cJSON_IsBool(item)) d->show_value_line = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_line_width");
    if (cJSON_IsNumber(item)) d->value_line_width = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_line_color");
    if (cJSON_IsNumber(item)) d->value_line_color.full = (uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "value_line_r_mod");
    if (cJSON_IsNumber(item)) d->value_line_r_mod = (int16_t)item->valueint;

    /* Anchor curve */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_enabled");
    if (cJSON_IsBool(item)) d->anchor_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_value");
    if (cJSON_IsNumber(item)) d->anchor_value = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(cfg, "anchor_position");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        d->anchor_position = (uint8_t)v;
    }

    /* Reverse */
    item = cJSON_GetObjectItemCaseSensitive(cfg, "reverse");
    if (cJSON_IsBool(item)) d->reverse = cJSON_IsTrue(item);

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
        NIGHT_PARSE_COLOR(night, d->night, arc_low_color);
        NIGHT_PARSE_COLOR(night, d->night, arc_high_color);
        NIGHT_PARSE_COLOR(night, d->night, minor_tick_color);
        NIGHT_PARSE_COLOR(night, d->night, major_tick_color);
        NIGHT_PARSE_COLOR(night, d->night, tick_label_color);
        NIGHT_PARSE_COLOR(night, d->night, value_line_color);
        NIGHT_PARSE_IMAGE(night, d->night, arc_image);
        NIGHT_PARSE_IMAGE(night, d->night, arc_image_full);
    }

    /* ── v14 channel binding + backwards-compat migration ────────
     * Empty-signal channel falls through to the legacy path so
     * record_legacy_widget repopulates it from the widget's own signal. */
    cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(cfg, "channel");
    if (cJSON_IsString(ch_item) && ch_item->valuestring && ch_item->valuestring[0] != '\0')
        safe_strncpy(d->channel_id, ch_item->valuestring, sizeof(d->channel_id));
    channel_t *bound_c = d->channel_id[0] ? channel_manager_get(d->channel_id) : NULL;
    if (bound_c && bound_c->signal_index >= 0) {
        d->channel = bound_c;
        safe_strncpy(d->signal_name, bound_c->signal_name, sizeof(d->signal_name));
        d->signal_index = bound_c->signal_index;
        d->signal_min = (float)bound_c->min;
        d->signal_max = (float)bound_c->max;
        if (bound_c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
            d->redline_enabled = true;
            d->redline_threshold = (float)bound_c->high_warn;
        } else {
            d->redline_enabled = false;
        }
        /* Channel owns the alert thresholds (single source of truth). A side
         * with no channel warn parks at the range edge = alert inactive. Same
         * pattern as the bar. Alert colours stay widget-owned. */
        d->arc_low  = (bound_c->low_warn  != CHANNEL_THRESHOLD_UNSET_LOW)  ? (float)bound_c->low_warn  : d->signal_min;
        d->arc_high = (bound_c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) ? (float)bound_c->high_warn : d->signal_max;
        /* Redline colour stays widget-owned — never overridden by the channel. */
    } else if (d->signal_name[0] != '\0') {
        legacy_widget_data_t legacy = {
            .signal_name = d->signal_name,
            .min = (int32_t)d->signal_min,
            .max = (int32_t)d->signal_max,
            .high_warn = d->redline_enabled ? (int32_t)d->redline_threshold : INT32_MIN,
            .color_normal = lv_color_to32(d->arc_color) & 0xFFFFFF,
            .color_high_warn = d->redline_enabled ?
                (lv_color_to32(d->redline_color) & 0xFFFFFF) : CHANNEL_USE_DEFAULT_COLOR,
        };
        channel_t *c = channel_manager_record_legacy_widget(&legacy);
        if (c) {
            d->channel = c;
            /* Adopt the channel's alert thresholds (single source of truth); a
             * side with no channel warn parks at the range edge = inactive. */
            d->arc_low  = (c->low_warn  != CHANNEL_THRESHOLD_UNSET_LOW)  ? (float)c->low_warn  : d->signal_min;
            d->arc_high = (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) ? (float)c->high_warn : d->signal_max;
        }
    }
}

static void _arc_destroy(widget_t *w) {
    if (!w) return;
    arc_data_t *d = (arc_data_t *)w->type_data;

    /* Unsubscribe signal before deleting LVGL objects */
    if (d && d->signal_index >= 0)
        signal_unsubscribe(d->signal_index, _arc_on_signal, w);

    if (d && d->channel) {
        channel_manager_unsubscribe((channel_t *)d->channel,
                                     _arc_on_channel_changed, w);
        d->channel = NULL;
    }

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

    /* Walk the override list. We track arc_color separately because the
     * indicator paint is delegated to _arc_apply_fill_color (which knows
     * limiter / redline precedence). Background color + both widths are
     * applied directly here. */
    bool       rule_sets_fg = false;
    lv_color_t rule_fg = d->arc_color;
    lv_color_t bg = d->bg_arc_color;
    uint8_t    fg_w = d->arc_width;
    uint8_t    bg_w = d->bg_arc_width;

    for (uint8_t i = 0; i < count; i++) {
        const rule_override_t *o = &ov[i];
        if (strcmp(o->field_name, "arc_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            rule_fg.full = (uint16_t)o->value.color;
            rule_sets_fg = true;
        } else if (strcmp(o->field_name, "bg_arc_color") == 0 && o->value_type == RULE_VAL_COLOR) {
            bg.full = (uint16_t)o->value.color;
        } else if (strcmp(o->field_name, "arc_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
            fg_w = (uint8_t)o->value.num;
        } else if (strcmp(o->field_name, "bg_arc_width") == 0 && o->value_type == RULE_VAL_NUMBER) {
            bg_w = (uint8_t)o->value.num;
        }
    }

    /* Update the rule-fg cache that _arc_apply_fill_color reads. Setting
     * (or clearing) it here means the next call — including the one we
     * make below AND every future _arc_recompute_value tick — will pick
     * the right "normal" base. This is what fixes the previously-known
     * limitation: when a limiter or redline zone clears, the indicator
     * snaps back to the rule colour instead of to d->arc_color. */
    if (rule_sets_fg) {
        d->_rule_arc_color = rule_fg;
        d->_rule_arc_color_set = true;
    } else {
        d->_rule_arc_color_set = false;
    }

    /* Delegate the indicator paint — _arc_apply_fill_color honours
     * limiter solid / limiter flash phase / redline-recolor / rule-fg
     * precedence in one place. Background + widths apply directly. */
    _arc_apply_fill_color(d, night_mode_is_active());
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

    /* Overlay tick / value-line colours are baked into the overlay lv_meter at
     * create time (LVGL v8 has no live tick/needle recolor API). When a night
     * override touches one of those colours, rebuild the overlay with the
     * night-picked colours. The overlay is a single cheap child, so a full
     * delete + rebuild is acceptable here. Only runs when the overlay exists
     * (standard mode, ticks or value-line enabled) AND a baked night colour is
     * actually set — so plain day/night colour-less layouts pay nothing. */
    if (d->tick_meter &&
        (d->night.has_minor_tick_color ||
         d->night.has_major_tick_color ||
         d->night.has_tick_label_color ||
         d->night.has_value_line_color)) {
        _arc_rebuild_overlay(w, active);
    }
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _arc_night_cb(bool active, void *user_data) {
    _arc_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Schema covers the headline arc fields; redline/limiter/value-overlay live
 * in arc_data_t but aren't yet surfaced through the inspector schema, so
 * those stay out of the hook. Image swaps (arc_image / arc_image_full)
 * defer to the next layout reload because flipping between standard /
 * static-image / image modes changes the underlying LVGL object tree. */

static bool _arc_inspector_get(const widget_t *w, const char *name,
                               widget_field_value_t *out) {
	if (!w || w->type != WIDGET_ARC || !w->type_data || !name || !out) return false;
	const arc_data_t *d = (const arc_data_t *)w->type_data;

	if (strcmp(name, "signal_name") == 0)    { out->str = d->signal_name;    return true; }
	if (strcmp(name, "arc_image") == 0)      { out->str = d->arc_image;      return true; }
	if (strcmp(name, "arc_image_full") == 0) { out->str = d->arc_image_full; return true; }
	if (strcmp(name, "tick_label_font") == 0) { out->str = d->tick_label_font; return true; }
	if (strcmp(name, "show_tick_labels") == 0) { out->b = d->show_tick_labels; return true; }
	if (strcmp(name, "ticks_outside") == 0)  { out->b = d->ticks_outside;    return true; }
	if (strcmp(name, "label_gap") == 0)      { out->i = d->label_gap;        return true; }
	if (strcmp(name, "tick_label_divisor") == 0) { out->i = d->tick_label_divisor; return true; }
	if (strcmp(name, "tick_label_color") == 0) { out->color = lv_color_to32(d->tick_label_color) & 0xFFFFFF; return true; }
	if (strcmp(name, "start_angle") == 0)    { out->i = d->start_angle;      return true; }
	if (strcmp(name, "end_angle") == 0)      { out->i = d->end_angle;        return true; }
	if (strcmp(name, "signal_min") == 0)     { out->i = (int32_t)d->signal_min; return true; }
	if (strcmp(name, "signal_max") == 0)     { out->i = (int32_t)d->signal_max; return true; }
	if (strcmp(name, "arc_width") == 0)      { out->i = d->arc_width;        return true; }
	if (strcmp(name, "bg_arc_width") == 0)   { out->i = d->bg_arc_width;     return true; }
	if (strcmp(name, "rounded_ends") == 0)   { out->b = d->rounded_ends;     return true; }
	if (strcmp(name, "arc_color") == 0)      { out->color = lv_color_to32(d->arc_color)    & 0xFFFFFF; return true; }
	if (strcmp(name, "bg_arc_color") == 0)   { out->color = lv_color_to32(d->bg_arc_color) & 0xFFFFFF; return true; }
	/* Color alerts. Thresholds (arc_low / arc_high) are channel-owned and the
	 * web edits them through the channel API, but expose them read-back here so
	 * the inspector can display the current values (mirrors the bar). */
	if (strcmp(name, "arc_alerts_enabled") == 0) { out->b = d->arc_alerts_enabled; return true; }
	if (strcmp(name, "arc_low") == 0)        { out->i = (int32_t)d->arc_low;  return true; }
	if (strcmp(name, "arc_high") == 0)       { out->i = (int32_t)d->arc_high; return true; }
	if (strcmp(name, "arc_low_color") == 0)  { out->color = lv_color_to32(d->arc_low_color)  & 0xFFFFFF; return true; }
	if (strcmp(name, "arc_high_color") == 0) { out->color = lv_color_to32(d->arc_high_color) & 0xFFFFFF; return true; }
	/* Tick spacing is stored count-based (minor_tick_count / major_tick_every)
	 * but exposed to the inspector as VALUE-SPACING to match the meter. Derive
	 * the per-tick value step from the signal range and tick count. */
	if (strcmp(name, "minor_tick_step") == 0) {
		float range = d->signal_max - d->signal_min;
		int denom = d->minor_tick_count > 1 ? d->minor_tick_count - 1 : 1;
		out->i = (int32_t)lroundf(range / (float)denom);
		return true;
	}
	if (strcmp(name, "major_tick_step") == 0) {
		float range = d->signal_max - d->signal_min;
		int denom = d->minor_tick_count > 1 ? d->minor_tick_count - 1 : 1;
		float mstep = range / (float)denom;
		out->i = (int32_t)lroundf(mstep * (float)d->major_tick_every);
		return true;
	}
	return false;
}

static bool _arc_inspector_set(widget_t *w, const char *name,
                               const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_ARC || !w->type_data || !name || !in) return false;
	arc_data_t *d = (arc_data_t *)w->type_data;
	lv_obj_t *a = d->arc_obj;

	if (strcmp(name, "signal_name") == 0 && in->str) {
		int16_t new_idx = (in->str[0] != '\0') ? signal_find_by_name(in->str) : -1;
		if (in->str[0] != '\0' && new_idx < 0) return false;

		if (d->signal_index >= 0)
			signal_unsubscribe(d->signal_index, _arc_on_signal, w);
		safe_strncpy(d->signal_name, in->str, sizeof(d->signal_name));
		d->signal_index = new_idx;
		if (new_idx >= 0)
			signal_subscribe(new_idx, _arc_on_signal, w);
		return true;
	}
	if (strcmp(name, "arc_image") == 0 && in->str) {
		safe_strncpy(d->arc_image, in->str, sizeof(d->arc_image));
		return true;   /* mode flip — needs rebuild */
	}
	if (strcmp(name, "arc_image_full") == 0 && in->str) {
		safe_strncpy(d->arc_image_full, in->str, sizeof(d->arc_image_full));
		return true;
	}
	if (strcmp(name, "start_angle") == 0 || strcmp(name, "end_angle") == 0) {
		int v = in->i; v %= 360; if (v < 0) v += 360;
		if (strcmp(name, "start_angle") == 0) d->start_angle = (int16_t)v;
		else                                  d->end_angle   = (int16_t)v;
		if (a && lv_obj_is_valid(a)) {
			lv_arc_set_bg_angles(a, d->start_angle, d->end_angle);
			lv_arc_set_angles(a, d->start_angle, d->end_angle);
		}
		return true;
	}
	if (strcmp(name, "signal_min") == 0) { d->signal_min = (float)in->i; return true; }
	if (strcmp(name, "signal_max") == 0) { d->signal_max = (float)in->i; return true; }
	if (strcmp(name, "arc_width") == 0) {
		int v = in->i; if (v < 1) v = 1; if (v > 50) v = 50;
		d->arc_width = (uint8_t)v;
		if (a && lv_obj_is_valid(a))
			lv_obj_set_style_arc_width(a, d->arc_width, LV_PART_INDICATOR);
		return true;
	}
	if (strcmp(name, "bg_arc_width") == 0) {
		int v = in->i; if (v < 1) v = 1; if (v > 50) v = 50;
		d->bg_arc_width = (uint8_t)v;
		if (a && lv_obj_is_valid(a))
			lv_obj_set_style_arc_width(a, d->bg_arc_width, LV_PART_MAIN);
		return true;
	}
	if (strcmp(name, "rounded_ends") == 0) {
		d->rounded_ends = in->b;
		if (a && lv_obj_is_valid(a)) {
			lv_obj_set_style_arc_rounded(a, d->rounded_ends, LV_PART_MAIN);
			lv_obj_set_style_arc_rounded(a, d->rounded_ends, LV_PART_INDICATOR);
		}
		return true;
	}
	if (strcmp(name, "arc_color") == 0) {
		d->arc_color = lv_color_hex(in->color);
		/* Re-run the full fill precedence (gradient sample / rule / night /
		 * limiter / redline) so the edit lands correctly and immediately even
		 * on an unbound arc (no signal ticks to self-heal), and the memo stays
		 * coherent — _arc_apply_fill_color updates _last_fill itself. */
		if (a && lv_obj_is_valid(a))
			_arc_apply_fill_color(d, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "bg_arc_color") == 0) {
		d->bg_arc_color = lv_color_hex(in->color);
		if (a && lv_obj_is_valid(a))
			lv_obj_set_style_arc_color(a, d->bg_arc_color, LV_PART_MAIN);
		return true;
	}
	/* Tick-label fields. The label colour/font/gap are baked into the overlay
	 * lv_meter at create time and the relabel hook reads divisor straight off
	 * arc_data_t, so any edit rebuilds the overlay (cheap single child) to take
	 * effect live — same path night mode uses. */
	if (strcmp(name, "show_tick_labels") == 0) {
		d->show_tick_labels = in->b;
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	/* "Ticks outside track": the overlay (ticks) stays at the full rim, but the
	 * arc + redline arc must be re-inset live. Recompute the inset and resize
	 * both arcs (guard validity), then rebuild the overlay so everything
	 * refreshes — same path the other tick setters use. */
	if (strcmp(name, "ticks_outside") == 0) {
		d->ticks_outside = in->b;
		int ins = _arc_track_inset(d);
		lv_coord_t aw = _arc_inset_dim((lv_coord_t)w->w, ins);
		lv_coord_t ah = _arc_inset_dim((lv_coord_t)w->h, ins);
		if (d->arc_obj && lv_obj_is_valid(d->arc_obj))
			lv_obj_set_size(d->arc_obj, aw, ah);
		if (d->redline_arc_obj && lv_obj_is_valid(d->redline_arc_obj))
			lv_obj_set_size(d->redline_arc_obj, aw, ah);
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "label_gap") == 0) {
		int v = in->i; if (v < -150) v = -150; if (v > 150) v = 150;
		d->label_gap = (int16_t)v;
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "tick_label_font") == 0 && in->str) {
		safe_strncpy(d->tick_label_font, in->str, sizeof(d->tick_label_font));
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "tick_label_color") == 0) {
		d->tick_label_color = lv_color_hex(in->color);
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "tick_label_divisor") == 0) {
		int v = in->i; if (v < 1) v = 1; if (v > 65535) v = 65535;
		d->tick_label_divisor = (uint16_t)v;
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	/* VALUE-SPACING tick fields (mirror the meter). The struct stores
	 * minor_tick_count / major_tick_every; derive those from the entered
	 * value step and the signal range, then rebuild the overlay live. */
	if (strcmp(name, "minor_tick_step") == 0) {
		float step = (float)in->i;
		if (step <= 0) step = 1;
		float range = d->signal_max - d->signal_min;
		int32_t cnt = (int32_t)lroundf(range / step) + 1;
		if (cnt < 2) cnt = 2;
		if (cnt > 100) cnt = 100;
		d->minor_tick_count = (uint8_t)cnt;
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "major_tick_step") == 0) {
		float majstep = (float)in->i;
		float range = d->signal_max - d->signal_min;
		int denom = d->minor_tick_count > 1 ? d->minor_tick_count - 1 : 1;
		float minstep = range / (float)denom;
		if (minstep <= 0) minstep = 1;
		int32_t every = (int32_t)lroundf(majstep / minstep);
		if (every < 1) every = 1;
		if (every > 50) every = 50;
		d->major_tick_every = (uint8_t)every;
		_arc_rebuild_overlay(w, night_mode_is_active());
		return true;
	}
	/* Color alerts. Colours re-run the full fill precedence so the edit lands
	 * immediately (even on an unbound arc with no signal ticks) and the paint
	 * memo stays coherent — _arc_apply_fill_color updates _last_fill itself.
	 * Thresholds are channel-owned; the web edits them through the channel API,
	 * but accept a direct write here too (mirrors the bar). */
	if (strcmp(name, "arc_alerts_enabled") == 0) {
		d->arc_alerts_enabled = in->b;
		if (a && lv_obj_is_valid(a))
			_arc_apply_fill_color(d, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "arc_low") == 0) {
		d->arc_low = (float)in->i;
		if (a && lv_obj_is_valid(a))
			_arc_apply_fill_color(d, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "arc_high") == 0) {
		d->arc_high = (float)in->i;
		if (a && lv_obj_is_valid(a))
			_arc_apply_fill_color(d, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "arc_low_color") == 0) {
		d->arc_low_color = lv_color_hex(in->color);
		if (a && lv_obj_is_valid(a))
			_arc_apply_fill_color(d, night_mode_is_active());
		return true;
	}
	if (strcmp(name, "arc_high_color") == 0) {
		d->arc_high_color = lv_color_hex(in->color);
		if (a && lv_obj_is_valid(a))
			_arc_apply_fill_color(d, night_mode_is_active());
		return true;
	}
	return false;
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
    /* grad_stops zero-initialised by calloc — count=0 means no
     * gradient, render path falls back to arc_color. */
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

    /* Color-alert defaults — OFF by default. Thresholds come from the bound
     * channel; the defaults here only matter for an unbound arc. */
    d->arc_alerts_enabled    = ARC_DEFAULT_ALERTS_ENABLED;
    d->arc_low               = ARC_DEFAULT_ALERT_LOW;
    d->arc_high              = ARC_DEFAULT_ALERT_HIGH;
    d->arc_low_color         = lv_color_hex(ARC_DEFAULT_LOW_COLOR);
    d->arc_high_color        = lv_color_hex(ARC_DEFAULT_HIGH_COLOR);

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

    /* Ticks (overlay meter) defaults — OFF by default. */
    d->show_ticks         = ARC_DEFAULT_SHOW_TICKS;
    d->minor_tick_count   = ARC_DEFAULT_MINOR_TICK_COUNT;
    d->major_tick_every   = ARC_DEFAULT_MAJOR_TICK_EVERY;
    d->minor_tick_length  = ARC_DEFAULT_MINOR_TICK_LENGTH;
    d->minor_tick_width   = ARC_DEFAULT_MINOR_TICK_WIDTH;
    d->major_tick_length  = ARC_DEFAULT_MAJOR_TICK_LENGTH;
    d->major_tick_width   = ARC_DEFAULT_MAJOR_TICK_WIDTH;
    d->minor_tick_color   = lv_color_hex(ARC_DEFAULT_MINOR_TICK_COLOR);
    d->major_tick_color   = lv_color_hex(ARC_DEFAULT_MAJOR_TICK_COLOR);
    d->ticks_outside      = ARC_DEFAULT_TICKS_OUTSIDE;

    /* Numeric tick label defaults — labels ON (only drawn when ticks are on). */
    d->show_tick_labels   = ARC_DEFAULT_SHOW_TICK_LABELS;
    d->label_gap          = ARC_DEFAULT_LABEL_GAP;
    d->tick_label_color   = lv_color_hex(ARC_DEFAULT_TICK_LABEL_COLOR);
    d->tick_label_divisor = ARC_DEFAULT_TICK_LABEL_DIVISOR;
    /* tick_label_font zeroed by calloc. */

    /* Value-line (overlay needle) defaults — OFF by default. */
    d->show_value_line    = ARC_DEFAULT_SHOW_VALUE_LINE;
    d->value_line_width   = ARC_DEFAULT_VALUE_LINE_WIDTH;
    d->value_line_color   = lv_color_hex(ARC_DEFAULT_VALUE_LINE_COLOR);
    d->value_line_r_mod   = ARC_DEFAULT_VALUE_LINE_R_MOD;

    /* Anchor + reverse defaults — OFF by default. */
    d->anchor_enabled     = false;
    d->anchor_value       = ARC_DEFAULT_ANCHOR_VALUE;
    d->anchor_position    = ARC_DEFAULT_ANCHOR_POSITION;
    d->reverse            = false;

    /* Overlay runtime handles zeroed by calloc (tick_meter / tick_scale /
     * value_needle). */

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
    w->inspector_get   = _arc_inspector_get;
    w->inspector_set   = _arc_inspector_set;

    ESP_LOGI(TAG, "Created arc widget instance (slot %u)", slot);
    return w;
}
