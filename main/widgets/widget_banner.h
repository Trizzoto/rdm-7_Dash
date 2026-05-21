/*
 * widget_banner.h — Full-width threshold-driven alert banner.
 *
 * Sits across the screen (bottom by default, full width) and only renders
 * when a bound signal crosses a configurable threshold. Acts as a
 * dashboard-wide "shouty" warning that's hard to miss, without forcing
 * the user to wire up a rule on some other widget to drive visibility.
 *
 * The threshold model mirrors the rules engine's operator set (>, <, >=,
 * <=, ==, !=, range) so users get the same mental model. Single signal,
 * single condition — for multi-condition logic, layer multiple banners.
 *
 * All of bg / text colour / opacity / font / corner radius / alignment
 * is editable through the standard inspector, defaults-only emit.
 */
#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Operator enum — same shape and order as rule_operator_t so the
 * inspector dropdown lines up with the rules editor. Storage type is
 * uint8_t in the data struct to keep the JSON small. */
typedef enum {
    BANNER_OP_GT    = 0,   /* signal >  threshold */
    BANNER_OP_LT    = 1,   /* signal <  threshold */
    BANNER_OP_GTE   = 2,   /* signal >= threshold */
    BANNER_OP_LTE   = 3,   /* signal <= threshold */
    BANNER_OP_EQ    = 4,   /* signal == threshold (within 0.001) */
    BANNER_OP_NEQ   = 5,   /* signal != threshold (outside 0.001) */
    BANNER_OP_RANGE = 6,   /* range_min <= signal <= range_max */
    BANNER_OP_ALWAYS = 7,  /* no signal needed — banner is always on (test mode) */
} banner_operator_t;

/* ── Night-mode overrides for banner ──────────────────────────────────── */
typedef struct {
    NIGHT_FIELD_COLOR(bg_color)
    NIGHT_FIELD_COLOR(text_color)
    NIGHT_FIELD_COLOR(border_color)
} banner_night_overrides_t;

/* ── Per-instance state for banner widgets ────────────────────────────── */
typedef struct {
    /* Trigger */
    char     signal_name[32];
    int16_t  signal_index;          /* resolved at from_json */
    uint8_t  op;                    /* banner_operator_t */
    float    threshold;             /* single-value ops */
    float    range_min;             /* used when op == BANNER_OP_RANGE */
    float    range_max;             /* used when op == BANNER_OP_RANGE */
    /* Content */
    char     text[128];             /* the message rendered inside the bar */
    char     font[32];              /* "Family:size", empty = THEME_FONT_BODY */
    uint8_t  text_align;            /* 0=left, 1=center, 2=right (default 1) */
    /* Appearance */
    lv_color_t bg_color;            /* default ~0xFF0000 */
    uint8_t    bg_opa;              /* default 128 (~half) */
    lv_color_t text_color;          /* default 0x000000 */
    uint8_t    border_width;        /* default 0 */
    lv_color_t border_color;        /* default 0x000000 */
    uint8_t    radius;              /* default 0 — banner-flat by convention */
    /* Runtime — not serialised */
    bool       condition_active;
    /* Editor-time visibility override: when true the banner is shown
     * regardless of the configured condition. Lets the user style /
     * position a banner without having to inject the threshold signal
     * or flip the op to "always (test)". The override is per-widget
     * (multiple banners can be tested independently) and is reset on
     * every layout rebuild. */
    bool       test_override;
    lv_obj_t  *bar;                 /* the bg container */
    lv_obj_t  *label;
    /* Night overrides */
    banner_night_overrides_t night;
} banner_data_t;

/**
 * Factory: allocate + wire vtable. Slot is currently unused (a layout can
 * carry as many banners as fit under the 32 KB JSON budget).
 */
widget_t *widget_banner_create_instance(uint8_t slot);

/**
 * Editor-time "force-visible" override for the banner identified by
 * widget_id. Called by the /api/banner/test handler so Studio can show
 * the banner while the user is styling it, without needing the bound
 * signal to actually cross its threshold. active=false releases the
 * override and returns the banner to condition-driven visibility.
 *
 * Safe to call from any context that already holds the LVGL mutex
 * (the HTTP path dispatches via lv_async_call).
 */
void widget_banner_apply_test_state(const char *widget_id, bool active);

#ifdef __cplusplus
}
#endif
