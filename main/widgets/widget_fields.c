/*
 * widget_fields.c - lookup helpers for the generated WIDGET_FIELDS array.
 *
 * The array itself lives in widget_fields.gen.c (codegen'd from
 * schema/widgets.schema.json). This file is hand-written so the helpers
 * can evolve without re-running the codegen.
 */
#include "widgets/widget_fields.h"
#include <string.h>

const widget_fields_def_t *widget_fields_for_type(widget_type_t t) {
    for (uint8_t i = 0; i < WIDGET_FIELDS_COUNT; i++) {
        if (WIDGET_FIELDS[i].type == t) return &WIDGET_FIELDS[i];
    }
    return NULL;
}

const widget_field_t *widget_fields_find(const widget_fields_def_t *def,
                                          const char *name) {
    if (!def || !name) return NULL;
    for (uint16_t i = 0; i < def->field_count; i++) {
        if (def->fields[i].name &&
            strcmp(def->fields[i].name, name) == 0) {
            return &def->fields[i];
        }
    }
    return NULL;
}
