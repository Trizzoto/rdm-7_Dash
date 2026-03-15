#pragma once
#include "widget_types.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Subscribe rule signals for a widget. Call after widget create() completes.
 * Resolves signal names, subscribes the rule evaluation callback. */
void widget_rules_subscribe(widget_t *w);

/* Parse "rules" array from a widget's config JSON into w->rules.
 * Call from widget from_json() or from layout_manager. */
void widget_rules_from_json(widget_t *w, const cJSON *config);

/* Serialize w->rules into a "rules" array in the config JSON.
 * Call from widget to_json(). */
void widget_rules_to_json(const widget_t *w, cJSON *config);

/* Free the rules array. Call from widget destroy(). */
void widget_rules_free(widget_t *w);

#ifdef __cplusplus
}
#endif
