/**
 * widget_types.c — Phase 2 widget type system implementation.
 *
 * Contains:
 *   - Per-type size constraints table
 *   - widget_type_name() lookup
 *   - widget_base_to_json() / widget_base_from_json() shared helpers
 */
#include "widget_types.h"
#include <stdio.h>
#include <string.h>

/* ─── Size constraints table ────────────────────────────────────────────── */
/*
 * All values in pixels.  Designed for an 800×480 display.
 * Phase 3 drag-and-drop will enforce these limits when the user resizes.
 *
 * Order must match widget_type_t enum exactly.
 */
const widget_size_constraints_t widget_constraints[WIDGET_TYPE_COUNT] = {
    /* WIDGET_PANEL     */ { .min_w =  80, .min_h =  40, .max_w = 250, .max_h = 130 },
    /* WIDGET_RPM_BAR   */ { .min_w = 300, .min_h =  30, .max_w = 800, .max_h =  80 },
    /* WIDGET_SPEED     */ { .min_w =  60, .min_h =  30, .max_w = 200, .max_h =  80 },
    /* WIDGET_GEAR      */ { .min_w =  50, .min_h =  50, .max_w = 130, .max_h = 130 },
    /* WIDGET_BAR       */ { .min_w = 120, .min_h =  15, .max_w = 450, .max_h =  50 },
    /* WIDGET_INDICATOR */ { .min_w =  30, .min_h =  30, .max_w =  80, .max_h =  80 },
    /* WIDGET_WARNING   */ { .min_w =  18, .min_h =  18, .max_w =  60, .max_h =  60 },
    /* WIDGET_TEXT      */ { .min_w =  40, .min_h =  20, .max_w = 400, .max_h = 100 },
    /* WIDGET_METER     */ { .min_w =  80, .min_h =  80, .max_w = 200, .max_h = 200 },
};

/* ─── Type name lookup ───────────────────────────────────────────────────── */

const char *widget_type_name(widget_type_t type)
{
    static const char *const names[WIDGET_TYPE_COUNT] = {
        "panel",
        "rpm_bar",
        "speed",
        "gear",
        "bar",
        "indicator",
        "warning",
        "text",
        "meter",
    };
    if ((unsigned)type >= (unsigned)WIDGET_TYPE_COUNT) return "unknown";
    return names[type];
}

/* ─── Shared JSON helpers ────────────────────────────────────────────────── */

void widget_base_to_json(const widget_t *w, cJSON *out)
{
    if (!w || !out) return;
    cJSON_AddStringToObject(out, "type", widget_type_name(w->type));
    cJSON_AddStringToObject(out, "id",   w->id);
    cJSON_AddNumberToObject(out, "x",    w->x);
    cJSON_AddNumberToObject(out, "y",    w->y);
    cJSON_AddNumberToObject(out, "w",    w->w);
    cJSON_AddNumberToObject(out, "h",    w->h);
}

void widget_base_from_json(widget_t *w, const cJSON *in)
{
    if (!w || !in) return;
    const cJSON *item;

    if ((item = cJSON_GetObjectItemCaseSensitive(in, "x")) && cJSON_IsNumber(item))
        w->x = (int16_t)item->valuedouble;
    if ((item = cJSON_GetObjectItemCaseSensitive(in, "y")) && cJSON_IsNumber(item))
        w->y = (int16_t)item->valuedouble;
    if ((item = cJSON_GetObjectItemCaseSensitive(in, "w")) && cJSON_IsNumber(item))
        w->w = (uint16_t)item->valuedouble;
    if ((item = cJSON_GetObjectItemCaseSensitive(in, "h")) && cJSON_IsNumber(item))
        w->h = (uint16_t)item->valuedouble;
    if ((item = cJSON_GetObjectItemCaseSensitive(in, "id")) && cJSON_IsString(item))
        strncpy(w->id, item->valuestring, sizeof(w->id) - 1);
}
