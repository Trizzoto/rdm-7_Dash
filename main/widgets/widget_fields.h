/**
 * widget_fields.h - Compile-time widget field metadata for the Inspector.
 *
 * The Inspector renders STYLE / DATA / RULES tabs from the data in
 * schema/widgets.schema.json. Rather than parse JSON at runtime (slow,
 * RAM-hungry), the schema is codegen'd into a static C array at build
 * time by tools/codegen_widget_inspector.py. The result lives in
 * widget_fields.gen.c next to this header.
 *
 * Each entry mirrors one schema field: name, label, type, category,
 * numeric range, defaults, select options, enable / group / inline
 * metadata for conditional UI.
 *
 * Per-widget apply hooks (Phase 3.2.x) write changes back into the
 * widget's type_data and call the appropriate lv_obj_set_style_* on
 * the live LVGL object for instant preview.
 */
#pragma once
#include "widgets/widget_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field types. Superset of schema "type" values - keep in lockstep with
 * SCHEMA_TYPE_TO_WF in tools/codegen_widget_inspector.py. */
typedef enum {
    WF_TYPE_TEXT = 0,       /* short single-line text */
    WF_TYPE_TEXTAREA,       /* multi-line text */
    WF_TYPE_NUMBER,         /* free numeric input */
    WF_TYPE_STEPPER,        /* integer with min/max/step */
    WF_TYPE_STEPPER_AUTO,   /* stepper with no fixed upper bound */
    WF_TYPE_SLIDER,         /* integer slider, no keypad */
    WF_TYPE_CHECKBOX,       /* on/off */
    WF_TYPE_SELECT,         /* dropdown / segmented */
    WF_TYPE_COLOR,          /* RGB565 colour */
    WF_TYPE_FONT,           /* "Family:size" string */
    WF_TYPE_IMAGE_PICKER,   /* LittleFS image path */
    WF_TYPE_CAN_ID,         /* CAN frame ID, hex input */
    WF_TYPE_COUNT,
} widget_field_type_t;

typedef enum {
    WF_CAT_DATA = 0,
    WF_CAT_APPEARANCE,
    WF_CAT_ALERTS,
    WF_CAT_THRESHOLDS,
    WF_CAT_COUNT,
} widget_field_category_t;

typedef struct {
    int32_t     value;
    const char *label;
} widget_field_option_t;

typedef struct {
    const char             *name;            /* schema name, e.g. "border_color" */
    const char             *label;           /* user-facing label */
    widget_field_type_t     type;
    widget_field_category_t category;
    /* Numeric range (NUMBER / STEPPER / STEPPER_AUTO / SLIDER). Zero if unused. */
    int32_t                 min_int;
    int32_t                 max_int;
    int32_t                 step_int;
    /* Defaults - one of these is meaningful depending on type. */
    int32_t                 default_int;     /* NUMBER/STEPPER/SLIDER/CHECKBOX/SELECT */
    float                   default_float;   /* NUMBER (when schema default is fractional) */
    uint32_t                default_color;   /* COLOR, packed 0xRRGGBB */
    const char             *default_str;     /* TEXT/TEXTAREA/FONT/IMAGE - NULL otherwise */
    /* SELECT options - NULL/0 unless type==SELECT. */
    const widget_field_option_t *options;
    uint8_t                 option_count;
    /* Behavioural metadata. NULL when unused. */
    const char             *enabled_by;      /* this field is gated by that field */
    const char             *group;           /* groups e.g. alerts {high,low} */
    const char             *inline_key;      /* same key = render on one row */
    bool                    night_overridable;
} widget_field_t;

typedef struct {
    widget_type_t              type;
    const char                *type_name;     /* "panel", "rpm_bar", ... */
    const widget_field_t      *fields;
    uint16_t                   field_count;
} widget_fields_def_t;

/* Generated arrays - defined in widget_fields.gen.c. */
extern const widget_fields_def_t WIDGET_FIELDS[];
extern const uint8_t             WIDGET_FIELDS_COUNT;

/* Locate the definition for a widget type. NULL if unknown. */
const widget_fields_def_t *widget_fields_for_type(widget_type_t t);

/* Locate a field by schema name. NULL if not found. */
const widget_field_t *widget_fields_find(const widget_fields_def_t *def,
                                          const char *name);

#ifdef __cplusplus
}
#endif
