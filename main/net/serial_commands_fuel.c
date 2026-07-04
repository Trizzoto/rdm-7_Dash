/**
 * serial_commands_fuel.c — fuel sender calibration serial JSON-RPC handlers.
 *
 * Methods: fuel.status, fuel.set-empty, fuel.set-full.
 */
#include "serial_commands_internal.h"

#include "cJSON.h"
#include "widgets/signal_internal.h"

/* ── Fuel calibration serial commands ───────────────────────────────────── */

void _handle_fuel_status(int id, cJSON *params)
{
    (void)params;
    fuel_cal_config_t fc;
    signal_internal_get_fuel_cal(&fc);
    float voltage = signal_internal_get_fuel_voltage();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "voltage", voltage);
    cJSON_AddNumberToObject(r, "empty_v", fc.empty_v);
    cJSON_AddNumberToObject(r, "full_v", fc.full_v);
    cJSON_AddNumberToObject(r, "full_value", fc.full_value);
    cJSON_AddBoolToObject(r, "enabled", fc.enabled);
    _send_response(id, r, NULL);
}

void _handle_fuel_set_empty(int id, cJSON *params)
{
    (void)params;
    float v = signal_internal_get_fuel_voltage();
    fuel_cal_config_t fc;
    signal_internal_get_fuel_cal(&fc);
    signal_internal_set_fuel_cal(v, fc.full_v, fc.full_value, fc.enabled);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "empty_v", v);
    _send_response(id, r, NULL);
}

void _handle_fuel_set_full(int id, cJSON *params)
{
    (void)params;
    float v = signal_internal_get_fuel_voltage();
    fuel_cal_config_t fc;
    signal_internal_get_fuel_cal(&fc);
    signal_internal_set_fuel_cal(fc.empty_v, v, fc.full_value, true);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "full_v", v);
    _send_response(id, r, NULL);
}
