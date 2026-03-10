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

/* ─── Widget type enum ──────────────────────────────────────────────────── */

typedef enum {
    WIDGET_PANEL     = 0,
    WIDGET_RPM_BAR   = 1,
    WIDGET_BAR       = 2,
    WIDGET_INDICATOR = 3,
    WIDGET_WARNING   = 4,
    WIDGET_TEXT      = 5,
    WIDGET_METER     = 6,
    WIDGET_TYPE_COUNT
} widget_type_t;

/* ─── Forward declaration ───────────────────────────────────────────────── */

typedef struct widget_t widget_t;

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
    widget_create_fn        create;
    widget_resize_fn        resize;
    widget_open_settings_fn open_settings;
    widget_to_json_fn       to_json;
    widget_from_json_fn     from_json;
    widget_destroy_fn       destroy;
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

#ifdef __cplusplus
}
#endif
