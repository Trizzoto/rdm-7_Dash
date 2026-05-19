/* ui_gear_setup.h — on-device modal for the CALCULATED_GEAR config.
 *
 * Mirrors the web editor's openGearSetup() flow so a user who never opens
 * Studio can still get gear display working from the dash alone.
 *
 * Auto-opens after the user applies the "RDM-7 / Internal" preset in the
 * ECU picker (the marker preset whose only purpose is to drive
 * CALCULATED_GEAR). Can also be opened directly from Device Settings.
 *
 * Persists via config_store_save_gear_cal(). Cancelling makes no changes.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional done callback. `saved` is true if the user pressed Save (config
 * was persisted), false on Cancel/close. ctx is the opaque pointer the
 * caller passed to open. cb may be NULL. */
typedef void (*ui_gear_setup_done_cb_t)(bool saved, void *ctx);

/* Open the gear-setup overlay. Existing NVS gear_cal config is loaded as
 * the starting state; first-time users get sensible defaults
 * (5 forward gears, generic ratio table, 4.11 final drive, 1.95 m wheel
 * circumference, RPM + VEHICLE_SPEED signal names). No-op if already open. */
void ui_gear_setup_open(ui_gear_setup_done_cb_t cb, void *ctx);

/* True while the overlay is on screen. */
bool ui_gear_setup_is_open(void);

#ifdef __cplusplus
}
#endif
