/**
 * widget_types.c — Phase 2 widget type system implementation.
 *
 * Contains:
 *   - Per-type size constraints table
 *   - widget_type_name() lookup
 *   - widget_base_to_json() / widget_base_from_json() shared helpers
 */
#include "widget_types.h"
#include "font_manager.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "widget_panel.h"
#include "widget_rpm_bar.h"
#include "widget_bar.h"
#include "widget_text.h"
#include "widget_meter.h"
#include "widget_indicator.h"
#include "widget_warning.h"
#include "widget_toggle.h"
#include "widget_image.h"
#include "widget_shape_panel.h"
#include "widget_arc.h"
#include "widget_button.h"
#include "widget_shift_light.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Shared font resolver ──────────────────────────────────────────────── */

const lv_font_t *widget_resolve_font(const char *name) {
	if (!name || name[0] == '\0') return NULL;

	/* New format: "Family:size" (e.g. "Fugaz:28") */
	const char *colon = strchr(name, ':');
	if (colon) {
		char family[32];
		size_t flen = (size_t)(colon - name);
		if (flen >= sizeof(family)) flen = sizeof(family) - 1;
		memcpy(family, name, flen);
		family[flen] = '\0';
		uint16_t size = (uint16_t)atoi(colon + 1);
		if (size >= 8 && size <= 128) {
			const lv_font_t *f = font_manager_get(family, size);
			if (f) return f;
		}
		return NULL;
	}

	/* Legacy compiled font names — backward compatibility */
	/* Montserrat system fonts */
	if (strcmp(name, "montserrat_8") == 0)  return &lv_font_montserrat_8;
	if (strcmp(name, "montserrat_10") == 0) return &lv_font_montserrat_10;
	if (strcmp(name, "montserrat_12") == 0) return &lv_font_montserrat_12;
	if (strcmp(name, "montserrat_14") == 0) return &lv_font_montserrat_14;
	if (strcmp(name, "montserrat_16") == 0) return &lv_font_montserrat_16;
	if (strcmp(name, "montserrat_18") == 0) return &lv_font_montserrat_18;
	if (strcmp(name, "montserrat_20") == 0) return &lv_font_montserrat_20;
	if (strcmp(name, "montserrat_22") == 0) return &lv_font_montserrat_22;
	if (strcmp(name, "montserrat_24") == 0) return &lv_font_montserrat_24;
	/* Custom dashboard fonts */
	if (strcmp(name, "fugaz_14") == 0)      return &ui_font_fugaz_14;
	if (strcmp(name, "fugaz_17") == 0)      return &ui_font_fugaz_17;
	if (strcmp(name, "fugaz_28") == 0)      return &ui_font_fugaz_28;
	if (strcmp(name, "fugaz_56") == 0)      return &ui_font_fugaz_56;
	if (strcmp(name, "manrope_35_bold") == 0) return &ui_font_Manrope_35_BOLD;
	if (strcmp(name, "manrope_54_bold") == 0) return &ui_font_Manrope_54_BOLD;
	return NULL;
}

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
    /* WIDGET_BAR       */ { .min_w = 120, .min_h =  15, .max_w = 450, .max_h =  50 },
    /* WIDGET_INDICATOR */ { .min_w =  30, .min_h =  30, .max_w =  80, .max_h =  80 },
    /* WIDGET_WARNING   */ { .min_w =  18, .min_h =  18, .max_w =  60, .max_h =  60 },
    /* WIDGET_TEXT      */ { .min_w =  40, .min_h =  20, .max_w = 400, .max_h = 100 },
    /* WIDGET_METER     */ { .min_w =  80, .min_h =  80, .max_w = 800, .max_h = 800 },
    /* WIDGET_IMAGE     */ { .min_w =  10, .min_h =  10, .max_w = 800, .max_h = 480 },
    /* WIDGET_SHAPE_PANEL */ { .min_w = 10, .min_h = 10, .max_w = 800, .max_h = 480 },
    /* WIDGET_ARC       */ { .min_w =  30, .min_h =  30, .max_w = 800, .max_h = 800 },
    /* WIDGET_TOGGLE    */ { .min_w =  40, .min_h =  20, .max_w = 200, .max_h =  80 },
    /* WIDGET_BUTTON    */ { .min_w =  40, .min_h =  20, .max_w = 300, .max_h = 100 },
    /* WIDGET_SHIFT_LIGHT */ { .min_w = 100, .min_h = 15, .max_w = 800, .max_h =  60 },
};

/* ─── Type name lookup ───────────────────────────────────────────────────── */

const char *widget_type_name(widget_type_t type)
{
    static const char *const names[WIDGET_TYPE_COUNT] = {
        "panel",
        "rpm_bar",
        "bar",
        "indicator",
        "warning",
        "text",
        "meter",
        "image",
        "shape_panel",
        "arc",
        "toggle",
        "button",
        "shift_light",
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
        safe_strncpy(w->id, item->valuestring, sizeof(w->id));
}

/* ─── Widget capability queries ─────────────────────────────────────────── */

char *widget_get_signal_name_buf(widget_t *w)
{
    if (!w || !w->type_data) return NULL;
    switch (w->type) {
        case WIDGET_PANEL:     return ((panel_data_t *)w->type_data)->signal_name;
        case WIDGET_RPM_BAR:   return ((rpm_bar_data_t *)w->type_data)->signal_name;
        case WIDGET_BAR:       return ((bar_data_t *)w->type_data)->signal_name;
        case WIDGET_TEXT:      return ((text_data_t *)w->type_data)->signal_name;
        case WIDGET_METER:     return ((meter_data_t *)w->type_data)->signal_name;
        case WIDGET_INDICATOR: return ((indicator_data_t *)w->type_data)->signal_name;
        case WIDGET_WARNING:   return ((warning_data_t *)w->type_data)->signal_name;
        case WIDGET_TOGGLE:    return ((toggle_data_t *)w->type_data)->signal_name;
        case WIDGET_SHIFT_LIGHT: return ((shift_light_data_t *)w->type_data)->signal_name;
        case WIDGET_IMAGE:
        case WIDGET_SHAPE_PANEL:
        case WIDGET_ARC:
        case WIDGET_BUTTON:
        default:               return NULL;
    }
}

int16_t *widget_get_signal_index_ptr(widget_t *w)
{
    if (!w || !w->type_data) return NULL;
    switch (w->type) {
        case WIDGET_PANEL:     return &((panel_data_t *)w->type_data)->signal_index;
        case WIDGET_RPM_BAR:   return &((rpm_bar_data_t *)w->type_data)->signal_index;
        case WIDGET_BAR:       return &((bar_data_t *)w->type_data)->signal_index;
        case WIDGET_TEXT:      return &((text_data_t *)w->type_data)->signal_index;
        case WIDGET_METER:     return &((meter_data_t *)w->type_data)->signal_index;
        case WIDGET_INDICATOR: return &((indicator_data_t *)w->type_data)->signal_index;
        case WIDGET_WARNING:   return &((warning_data_t *)w->type_data)->signal_index;
        case WIDGET_TOGGLE:    return &((toggle_data_t *)w->type_data)->signal_index;
        case WIDGET_SHIFT_LIGHT: return &((shift_light_data_t *)w->type_data)->signal_index;
        case WIDGET_IMAGE:
        case WIDGET_SHAPE_PANEL:
        case WIDGET_ARC:
        case WIDGET_BUTTON:
        default:               return NULL;
    }
}

char *widget_get_label_buf(widget_t *w)
{
    if (!w || !w->type_data) return NULL;
    switch (w->type) {
        case WIDGET_PANEL:   return ((panel_data_t *)w->type_data)->label;
        case WIDGET_BAR:     return ((bar_data_t *)w->type_data)->label;
        case WIDGET_WARNING: return ((warning_data_t *)w->type_data)->label;
        case WIDGET_TOGGLE:  return ((toggle_data_t *)w->type_data)->label;
        case WIDGET_BUTTON:  return ((button_data_t *)w->type_data)->label;
        default:             return NULL;
    }
}

bool widget_has_alert_support(widget_t *w)
{
    if (!w) return false;
    return (w->type == WIDGET_PANEL || w->type == WIDGET_BAR);
}

bool widget_needs_data_source(widget_t *w)
{
    if (!w) return false;
    /* Image just displays a file, toggle/button only TX CAN data */
    return (w->type != WIDGET_IMAGE &&
            w->type != WIDGET_TOGGLE &&
            w->type != WIDGET_BUTTON &&
            w->type != WIDGET_SHAPE_PANEL);
}
