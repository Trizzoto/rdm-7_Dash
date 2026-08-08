#pragma once
/**
 * can_forward.h — forward the fuel-sender analog input over CAN.
 *
 * A small LVGL-timer transmitter that periodically packs the fuel reading
 * (calibrated level or raw volts, see fuel_forward_config_t in
 * storage/config_store.h) into an 8-byte CAN frame and puts it on the bus,
 * so an ECU can consume the dash's analog input as if it were its own.
 *
 * Threading: everything here runs on the LVGL task (timer callback), same
 * as the OBD2 poller and the button/toggle TX widgets. can_forward_apply()
 * may be called from any task — it defers the timer swap via rdm_async_call.
 */
#include "storage/config_store.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load persisted config from NVS and start transmitting if enabled.
 * Must run on the LVGL task (creates an lv_timer). Idempotent — safe to
 * call again on layout reloads; the timer is only created once. */
void can_forward_init(void);

/** Apply a new config live (restarts the timer at the new rate, or stops
 * it when disabled). Safe from any task. Does NOT persist — callers save
 * via config_store_save_fuel_forward() themselves. */
void can_forward_apply(const fuel_forward_config_t *cfg);

/** Snapshot current config + health counters for the status endpoint.
 * Any pointer may be NULL. tx_ok/tx_fail are cumulative since boot. */
void can_forward_get_status(fuel_forward_config_t *cfg,
                            uint32_t *tx_ok, uint32_t *tx_fail);

#ifdef __cplusplus
}
#endif
