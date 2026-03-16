/**
 * widget_rules.c -- Conditional rules evaluation engine.
 *
 * Rules are per-widget and allow overriding widget config fields based on
 * signal conditions. Each rule watches a signal and, when the condition
 * matches, applies a set of field overrides via the widget's apply_overrides
 * vtable function.
 */
#include "widget_rules.h"
#include "signal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "widget_rules";

/* ── Operator string mapping ──────────────────────────────────────────── */

static rule_operator_t _op_from_str(const char *s)
{
    if (!s) return RULE_OP_EQ;
    if (strcmp(s, ">")  == 0) return RULE_OP_GT;
    if (strcmp(s, "<")  == 0) return RULE_OP_LT;
    if (strcmp(s, ">=") == 0) return RULE_OP_GTE;
    if (strcmp(s, "<=") == 0) return RULE_OP_LTE;
    if (strcmp(s, "==") == 0) return RULE_OP_EQ;
    if (strcmp(s, "!=") == 0) return RULE_OP_NEQ;
    if (strcmp(s, "range") == 0) return RULE_OP_RANGE;
    ESP_LOGW(TAG, "Unknown operator '%s', defaulting to ==", s);
    return RULE_OP_EQ;
}

static const char *_op_to_str(rule_operator_t op)
{
    switch (op) {
    case RULE_OP_GT:    return ">";
    case RULE_OP_LT:    return "<";
    case RULE_OP_GTE:   return ">=";
    case RULE_OP_LTE:   return "<=";
    case RULE_OP_EQ:    return "==";
    case RULE_OP_NEQ:   return "!=";
    case RULE_OP_RANGE: return "range";
    }
    return "==";
}

/* ── Value type string mapping ────────────────────────────────────────── */

static rule_value_type_t _type_from_str(const char *s)
{
    if (!s) return RULE_VAL_NUMBER;
    if (strcmp(s, "number") == 0) return RULE_VAL_NUMBER;
    if (strcmp(s, "color")  == 0) return RULE_VAL_COLOR;
    if (strcmp(s, "bool")   == 0) return RULE_VAL_BOOL;
    if (strcmp(s, "string") == 0) return RULE_VAL_STRING;
    return RULE_VAL_NUMBER;
}

static const char *_type_to_str(rule_value_type_t t)
{
    switch (t) {
    case RULE_VAL_NUMBER: return "number";
    case RULE_VAL_COLOR:  return "color";
    case RULE_VAL_BOOL:   return "bool";
    case RULE_VAL_STRING: return "string";
    }
    return "number";
}

/* ── Rule evaluation callback ─────────────────────────────────────────── */

static void _rule_signal_cb(float value, bool is_stale, void *user_data)
{
    (void)value;   /* Don't use -- each rule reads its own signal's current value */
    (void)is_stale;
    widget_t *w = (widget_t *)user_data;
    if (!w || !w->rules || w->rule_count == 0) return;
    if (!w->root) return;

    bool any_changed = false;

    for (uint8_t i = 0; i < w->rule_count; i++) {
        widget_rule_t *r = &w->rules[i];
        bool was_active = r->is_active;
        bool match = false;

        /* Read this rule's own signal value (not the triggering signal) */
        if (r->signal_index < 0) {
            match = false;
        } else {
            signal_t *sig = signal_get_by_index((uint16_t)r->signal_index);
            if (!sig || sig->is_stale) {
                match = false;
            } else {
                float v = sig->current_value;
                switch (r->op) {
                case RULE_OP_GT:    match = (v > r->threshold);  break;
                case RULE_OP_LT:    match = (v < r->threshold);  break;
                case RULE_OP_GTE:   match = (v >= r->threshold); break;
                case RULE_OP_LTE:   match = (v <= r->threshold); break;
                case RULE_OP_EQ:    match = (v == r->threshold); break;
                case RULE_OP_NEQ:   match = (v != r->threshold); break;
                case RULE_OP_RANGE: match = (v >= r->range_min && v <= r->range_max); break;
                }
            }
        }

        r->is_active = match;
        if (match != was_active) any_changed = true;
    }

    if (any_changed && w->apply_overrides) {
        /* Collect active overrides -- last matching rule wins per field */
        rule_override_t merged[MAX_WIDGET_RULES * MAX_RULE_OVERRIDES];
        uint8_t count = 0;

        for (uint8_t i = 0; i < w->rule_count; i++) {
            if (!w->rules[i].is_active) continue;
            for (uint8_t j = 0; j < w->rules[i].override_count; j++) {
                bool replaced = false;
                for (uint8_t k = 0; k < count; k++) {
                    if (strcmp(merged[k].field_name,
                              w->rules[i].overrides[j].field_name) == 0) {
                        merged[k] = w->rules[i].overrides[j];
                        replaced = true;
                        break;
                    }
                }
                if (!replaced && count < (uint8_t)(sizeof(merged) / sizeof(merged[0]))) {
                    merged[count++] = w->rules[i].overrides[j];
                }
            }
        }

        w->apply_overrides(w, merged, count);
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void widget_rules_subscribe(widget_t *w)
{
    if (!w || !w->rules || w->rule_count == 0) return;

    /* Track which signal indices we have already subscribed to avoid
     * duplicate subscriptions when multiple rules watch the same signal. */
    int16_t subscribed[MAX_WIDGET_RULES];
    uint8_t sub_count = 0;

    for (uint8_t i = 0; i < w->rule_count; i++) {
        widget_rule_t *r = &w->rules[i];

        /* Resolve signal name to index */
        if (r->signal_name[0] != '\0') {
            r->signal_index = signal_find_by_name(r->signal_name);
        }

        if (r->signal_index < 0) {
            ESP_LOGW(TAG, "Rule %u on '%s': signal '%s' not found",
                     i, w->id, r->signal_name);
            continue;
        }

        /* Check if already subscribed to this signal */
        bool already = false;
        for (uint8_t s = 0; s < sub_count; s++) {
            if (subscribed[s] == r->signal_index) {
                already = true;
                break;
            }
        }

        if (!already) {
            if (signal_subscribe(r->signal_index, _rule_signal_cb, w)) {
                if (sub_count < MAX_WIDGET_RULES) {
                    subscribed[sub_count++] = r->signal_index;
                }
                ESP_LOGI(TAG, "Rule subscribed '%s' signal '%s' (idx %d)",
                         w->id, r->signal_name, r->signal_index);
            } else {
                ESP_LOGW(TAG, "Rule subscribe failed for '%s' signal '%s'",
                         w->id, r->signal_name);
            }
        }
    }
}

void widget_rules_from_json(widget_t *w, const cJSON *config)
{
    if (!w || !config) return;

    const cJSON *rules_arr = cJSON_GetObjectItemCaseSensitive(config, "rules");
    if (!cJSON_IsArray(rules_arr)) return;

    int arr_size = cJSON_GetArraySize(rules_arr);
    if (arr_size <= 0) return;

    uint8_t count = (uint8_t)(arr_size > MAX_WIDGET_RULES ? MAX_WIDGET_RULES : arr_size);
    w->rules = calloc(count, sizeof(widget_rule_t));
    if (!w->rules) {
        ESP_LOGE(TAG, "Failed to allocate %u rules for '%s'", count, w->id);
        return;
    }
    w->rule_count = count;

    for (uint8_t i = 0; i < count; i++) {
        const cJSON *rule_obj = cJSON_GetArrayItem(rules_arr, (int)i);
        if (!cJSON_IsObject(rule_obj)) continue;

        widget_rule_t *r = &w->rules[i];
        r->signal_index = -1;
        r->is_active = false;

        /* signal_name */
        const cJSON *sn = cJSON_GetObjectItemCaseSensitive(rule_obj, "signal_name");
        if (cJSON_IsString(sn) && sn->valuestring) {
            safe_strncpy(r->signal_name, sn->valuestring, sizeof(r->signal_name));
        }

        /* operator */
        const cJSON *op_item = cJSON_GetObjectItemCaseSensitive(rule_obj, "op");
        if (cJSON_IsString(op_item) && op_item->valuestring)
            r->op = _op_from_str(op_item->valuestring);

        /* threshold */
        const cJSON *th = cJSON_GetObjectItemCaseSensitive(rule_obj, "threshold");
        if (cJSON_IsNumber(th))
            r->threshold = (float)th->valuedouble;

        /* range bounds */
        const cJSON *rmin = cJSON_GetObjectItemCaseSensitive(rule_obj, "range_min");
        if (cJSON_IsNumber(rmin))
            r->range_min = (float)rmin->valuedouble;
        const cJSON *rmax = cJSON_GetObjectItemCaseSensitive(rule_obj, "range_max");
        if (cJSON_IsNumber(rmax))
            r->range_max = (float)rmax->valuedouble;

        /* overrides array */
        const cJSON *ov_arr = cJSON_GetObjectItemCaseSensitive(rule_obj, "overrides");
        if (cJSON_IsArray(ov_arr)) {
            int ov_size = cJSON_GetArraySize(ov_arr);
            uint8_t ov_count = (uint8_t)(ov_size > MAX_RULE_OVERRIDES
                                         ? MAX_RULE_OVERRIDES : ov_size);
            r->override_count = ov_count;

            for (uint8_t j = 0; j < ov_count; j++) {
                const cJSON *ov_obj = cJSON_GetArrayItem(ov_arr, (int)j);
                if (!cJSON_IsObject(ov_obj)) continue;

                rule_override_t *ov = &r->overrides[j];

                const cJSON *field = cJSON_GetObjectItemCaseSensitive(ov_obj, "field");
                if (cJSON_IsString(field) && field->valuestring) {
                    safe_strncpy(ov->field_name, field->valuestring,
                            sizeof(ov->field_name));
                }

                const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(ov_obj, "type");
                if (cJSON_IsString(type_item) && type_item->valuestring)
                    ov->value_type = _type_from_str(type_item->valuestring);

                const cJSON *val = cJSON_GetObjectItemCaseSensitive(ov_obj, "value");
                switch (ov->value_type) {
                case RULE_VAL_NUMBER:
                    if (cJSON_IsNumber(val))
                        ov->value.num = (float)val->valuedouble;
                    break;
                case RULE_VAL_COLOR:
                    if (cJSON_IsNumber(val))
                        ov->value.color = (uint32_t)val->valueint;
                    break;
                case RULE_VAL_BOOL:
                    if (cJSON_IsBool(val))
                        ov->value.flag = cJSON_IsTrue(val);
                    break;
                case RULE_VAL_STRING:
                    if (cJSON_IsString(val) && val->valuestring) {
                        safe_strncpy(ov->value.str, val->valuestring,
                                sizeof(ov->value.str));
                    }
                    break;
                }
            }
        }
    }

    ESP_LOGI(TAG, "Parsed %u rules for widget '%s'", count, w->id);
}

void widget_rules_to_json(const widget_t *w, cJSON *config)
{
    if (!w || !config || !w->rules || w->rule_count == 0) return;

    cJSON *rules_arr = cJSON_AddArrayToObject(config, "rules");
    if (!rules_arr) return;

    for (uint8_t i = 0; i < w->rule_count; i++) {
        const widget_rule_t *r = &w->rules[i];
        cJSON *rule_obj = cJSON_CreateObject();
        if (!rule_obj) continue;

        cJSON_AddStringToObject(rule_obj, "signal_name", r->signal_name);
        cJSON_AddStringToObject(rule_obj, "op", _op_to_str(r->op));

        if (r->op == RULE_OP_RANGE) {
            cJSON_AddNumberToObject(rule_obj, "range_min", (double)r->range_min);
            cJSON_AddNumberToObject(rule_obj, "range_max", (double)r->range_max);
        } else {
            cJSON_AddNumberToObject(rule_obj, "threshold", (double)r->threshold);
        }

        if (r->override_count > 0) {
            cJSON *ov_arr = cJSON_AddArrayToObject(rule_obj, "overrides");
            if (ov_arr) {
                for (uint8_t j = 0; j < r->override_count; j++) {
                    const rule_override_t *ov = &r->overrides[j];
                    cJSON *ov_obj = cJSON_CreateObject();
                    if (!ov_obj) continue;

                    cJSON_AddStringToObject(ov_obj, "field", ov->field_name);
                    cJSON_AddStringToObject(ov_obj, "type",
                                            _type_to_str(ov->value_type));

                    switch (ov->value_type) {
                    case RULE_VAL_NUMBER:
                        cJSON_AddNumberToObject(ov_obj, "value",
                                                (double)ov->value.num);
                        break;
                    case RULE_VAL_COLOR:
                        cJSON_AddNumberToObject(ov_obj, "value",
                                                (double)ov->value.color);
                        break;
                    case RULE_VAL_BOOL:
                        cJSON_AddBoolToObject(ov_obj, "value", ov->value.flag);
                        break;
                    case RULE_VAL_STRING:
                        cJSON_AddStringToObject(ov_obj, "value", ov->value.str);
                        break;
                    }

                    cJSON_AddItemToArray(ov_arr, ov_obj);
                }
            }
        }

        cJSON_AddItemToArray(rules_arr, rule_obj);
    }
}

void widget_rules_free(widget_t *w)
{
    if (!w) return;
    free(w->rules);
    w->rules = NULL;
    w->rule_count = 0;
}
