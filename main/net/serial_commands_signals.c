/**
 * serial_commands_signals.c — live signal serial JSON-RPC handlers.
 *
 * Methods: signal.values, signal.inject, signal.simulate.
 */
#include "serial_commands_internal.h"
#include "system/rdm_lv_async.h"

#include "cJSON.h"
#include "widgets/signal.h"
#include "widgets/signal_sim.h"
#include "lvgl.h"

/* LVGL mutex (defined in main.c) */
extern bool rdm_lvgl_lock(int timeout_ms);
extern void rdm_lvgl_unlock(void);

/* ── signal.values ───────────────────────────────────────────────────────── */

void _handle_signal_values(int id, cJSON *params)
{
    (void)params;
    if (!rdm_lvgl_lock(500)) {
        _send_error(id, "LVGL busy");
        return;
    }

    uint16_t count = signal_get_count();
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "signals");
    for (uint16_t i = 0; i < count; i++) {
        signal_t *sig = signal_get_by_index(i);
        if (!sig || sig->name[0] == '\0') continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", sig->name);
        cJSON_AddNumberToObject(obj, "value", sig->current_value);
        cJSON_AddBoolToObject(obj, "stale", sig->is_stale);
        cJSON_AddNumberToObject(obj, "can_id", sig->can_id);
        cJSON_AddItemToArray(arr, obj);
    }
    rdm_lvgl_unlock();
    _send_response(id, r, NULL);
}

/* ── signal.inject ───────────────────────────────────────────────────────── */

void _handle_signal_inject(int id, cJSON *params)
{
    /* Supports single {"name":"RPM","value":3000} or batch {"signals":[...]} */
    cJSON *batch = cJSON_GetObjectItem(params, "signals");
    if (cJSON_IsArray(batch)) {
        int n = cJSON_GetArraySize(batch);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(batch, i);
            cJSON *n_item = cJSON_GetObjectItem(item, "name");
            cJSON *v_item = cJSON_GetObjectItem(item, "value");
            if (cJSON_IsString(n_item) && cJSON_IsNumber(v_item)) {
                signal_inject_test_value(n_item->valuestring,
                                         (float)v_item->valuedouble);
            }
        }
    } else {
        cJSON *n_item = cJSON_GetObjectItem(params, "name");
        cJSON *v_item = cJSON_GetObjectItem(params, "value");
        if (cJSON_IsString(n_item) && cJSON_IsNumber(v_item)) {
            signal_inject_test_value(n_item->valuestring,
                                     (float)v_item->valuedouble);
        }
    }
    _send_ok(id);
}

/* ── signal.simulate ─────────────────────────────────────────────────────── */

void _handle_signal_simulate(int id, cJSON *params)
{
    cJSON *enable = cJSON_GetObjectItem(params, "enable");
    if (cJSON_IsBool(enable)) {
        if (cJSON_IsTrue(enable))
            rdm_async_call((lv_async_cb_t)signal_sim_start, NULL);
        else
            rdm_async_call((lv_async_cb_t)signal_sim_stop, NULL);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "active", signal_sim_is_active());
    _send_response(id, r, NULL);
}
