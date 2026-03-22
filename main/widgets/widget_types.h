/**
 * widget_types.h — Phase 2 widget abstraction layer.
 *
 * Defines the common widget_t interface that every widget module implements.
 * The existing Phase 0F create/update/deinit functions are left untouched;
 * this layer is purely additive.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── String helper ─────────────────────────────────────────────────────── */

/** Safe strncpy that always null-terminates.
 *  @param dst       Destination buffer.
 *  @param src       Source string (NULL-safe — writes empty string).
 *  @param dst_size  Total size of dst buffer (including null terminator). */
static inline void safe_strncpy(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (src) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/* ─── Widget type enum ──────────────────────────────────────────────────── */

typedef enum {
    WIDGET_PANEL       = 0,
    WIDGET_RPM_BAR     = 1,
    WIDGET_BAR         = 2,
    WIDGET_INDICATOR   = 3,
    WIDGET_WARNING     = 4,
    WIDGET_TEXT        = 5,
    WIDGET_METER       = 6,
    WIDGET_IMAGE       = 7,
    WIDGET_SHAPE_PANEL = 8,
    WIDGET_ARC         = 9,
    WIDGET_TOGGLE      = 10,
    WIDGET_BUTTON      = 11,
    WIDGET_SHIFT_LIGHT = 12,
    WIDGET_TYPE_COUNT
} widget_type_t;

/* ─── Forward declaration ───────────────────────────────────────────────── */

typedef struct widget_t widget_t;

/* ─── Conditional rules ─────────────────────────────────────────────────── */

#define MAX_WIDGET_RULES     16
#define MAX_RULE_OVERRIDES   16
#define RULE_FIELD_NAME_LEN 32

typedef enum {
    RULE_OP_GT = 0,     /* >  */
    RULE_OP_LT,         /* <  */
    RULE_OP_GTE,        /* >= */
    RULE_OP_LTE,        /* <= */
    RULE_OP_EQ,         /* == */
    RULE_OP_NEQ,        /* != */
    RULE_OP_RANGE,      /* between min and max */
} rule_operator_t;

typedef enum {
    RULE_VAL_NUMBER = 0,
    RULE_VAL_COLOR,
    RULE_VAL_BOOL,
    RULE_VAL_STRING,
} rule_value_type_t;

typedef struct {
    char              field_name[RULE_FIELD_NAME_LEN];
    rule_value_type_t value_type;
    union {
        float    num;
        uint32_t color;
        bool     flag;
        char     str[32];
    } value;
} rule_override_t;

typedef struct {
    char             signal_name[32];
    int16_t          signal_index;      /* resolved at from_json time */
    rule_operator_t  op;
    float            threshold;         /* for non-range ops */
    float            range_min;         /* for RULE_OP_RANGE */
    float            range_max;
    rule_override_t  overrides[MAX_RULE_OVERRIDES];
    uint8_t          override_count;
    bool             is_active;         /* runtime: last evaluation result */
} widget_rule_t;

/* ─── Function pointer typedefs ─────────────────────────────────────────── */

/** Called once to build LVGL objects on the given parent. */
typedef void (*widget_create_fn)       (widget_t *w, lv_obj_t *parent);

/** Resize the widget's root container.  Phase 3 will apply to LVGL objects. */
typedef void (*widget_resize_fn)       (widget_t *w, uint16_t new_w, uint16_t new_h);

/** Open the settings modal for this widget. */
typedef void (*widget_open_settings_fn)(widget_t *w);

/** Serialise position/size and widget-specific fields into the supplied JSON
 *  object (caller owns the cJSON node). */
typedef void (*widget_to_json_fn)      (widget_t *w, cJSON *out);

/** Deserialise from a cJSON object.  Sets w->x/y/w/h and any type_data
 *  fields.  LVGL repositioning is deferred to Phase 3. */
typedef void (*widget_from_json_fn)    (widget_t *w, cJSON *in);

/** Destroy the widget: delete LVGL objects if owned, then free(w). */
typedef void (*widget_destroy_fn)      (widget_t *w);

/** Apply rule overrides to the widget's live LVGL objects. */
typedef void (*widget_apply_overrides_fn)(widget_t *w, const rule_override_t *overrides, uint8_t count);

/* ─── Core widget struct ─────────────────────────────────────────────────── */

struct widget_t {
    widget_type_t           type;       /**< Which widget type this is.        */
    lv_obj_t               *root;       /**< Top-level LVGL container object.  */
    int16_t                 x, y;       /**< Layout position (pixels).         */
    uint16_t                w, h;       /**< Layout size (pixels).             */
    char                    id[16];     /**< Instance identifier string.       */
    uint8_t                 slot;       /**< Slot index (e.g. panel 0-7).      */
    void                   *type_data;  /**< Per-instance type-specific data.  */

    /* vtable */
    widget_create_fn           create;
    widget_resize_fn           resize;
    widget_open_settings_fn    open_settings;
    widget_to_json_fn          to_json;
    widget_from_json_fn        from_json;
    widget_destroy_fn          destroy;
    widget_apply_overrides_fn  apply_overrides;  /**< NULL if widget ignores rules. */

    /* Conditional rules (heap-allocated, NULL if no rules) */
    widget_rule_t *rules;
    uint8_t        rule_count;
};

/* ─── Size constraints ───────────────────────────────────────────────────── */

typedef struct {
    uint16_t min_w, min_h;
    uint16_t max_w, max_h;
} widget_size_constraints_t;

/** Per-type minimum/maximum dimensions.  Indexed by widget_type_t. */
extern const widget_size_constraints_t widget_constraints[WIDGET_TYPE_COUNT];

/* ─── Shared helpers (implemented in widget_types.c) ────────────────────── */

/** Return a short ASCII name for a widget type (e.g. "panel", "rpm_bar"). */
const char *widget_type_name(widget_type_t type);

/** Resolve a font name string to an lv_font_t pointer.
 *  Returns the theme default body font if name is NULL/empty/unrecognised. */
const lv_font_t *widget_resolve_font(const char *name);

/** Write the base fields (type, id, x, y, w, h) into an existing JSON object. */
void widget_base_to_json(const widget_t *w, cJSON *out);

/** Read the base fields from a JSON object into *w.  Unknown keys are ignored. */
void widget_base_from_json(widget_t *w, const cJSON *in);

/* ─── Widget capability queries ─────────────────────────────────────────── */

/** Return a pointer to the signal_name[32] buffer inside type_data, or NULL
 *  if this widget type has no signal binding. */
char *widget_get_signal_name_buf(widget_t *w);

/** Return a pointer to the signal_index field inside type_data, or NULL
 *  if this widget type has no signal binding. */
int16_t *widget_get_signal_index_ptr(widget_t *w);

/** Return a pointer to the label buffer inside type_data, or NULL
 *  if this widget type has no label field. */
char *widget_get_label_buf(widget_t *w);

/** Return true if the widget type supports alert/threshold configuration
 *  (currently WIDGET_PANEL and WIDGET_BAR only). */
bool widget_has_alert_support(widget_t *w);

/** Return true if the widget type needs a data source / signal configuration
 *  in the config modal.  Image, toggle, and button do not. */
bool widget_needs_data_source(widget_t *w);

#ifdef __cplusplus
}
#endif
