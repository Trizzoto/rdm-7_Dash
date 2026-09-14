/*
 * obd2_autosetup.h — set OBD2 up for a car without asking (ADR-0073).
 *
 * Some cars are read through two doors at once: a factory ECU broadcasts part
 * of what a dash wants over CAN, and answers OBD2 for the rest. A Ford Falcon
 * BA/BF/FG broadcasts RPM, speed, coolant and throttle, but not ignition
 * timing, oil or fuel pressure, lambda or fuel trims — and an FG not intake
 * temperature or MAP either. OBD2 has those.
 *
 * That used to be a wizard step every customer saw, most of them on a
 * standalone ECU with nothing to gain from it. Now choosing such a car's
 * preset is enough: this runs the discovery scan in the background, takes
 * every reading the car answers that fills a channel nothing else feeds, and
 * binds it. If the car doesn't answer yet (ignition off, dash on a bench) the
 * setup stays owed — persisted — and is retried, including after a reboot,
 * until the car answers once.
 *
 * Every function here runs on the LVGL task (the scan's callback is delivered
 * there, and the channel set is read under the LVGL mutex), except
 * obd2_autosetup_boot(), which may be called from anywhere.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool      answered;   /* the car replied to the discovery scan        */
	uint8_t   readings;   /* PIDs it reported (bitmask PIDs not counted)   */
	size_t    offered;    /* of those, readings with a channel to go to    */
	size_t    bound;      /* channels this pass connected                  */
	esp_err_t err;        /* channel_apply_obd2() result; ESP_OK otherwise */
	bool      will_retry; /* not answered, and another attempt is booked   */
} obd2_autosetup_result_t;

typedef void (*obd2_autosetup_cb_t)(const obd2_autosetup_result_t *r, void *user);

/** Does a car on this ECU preset want OBD2 filling in behind it? */
bool obd2_autosetup_ecu_wants(const char *make, const char *version);

/** Call after an ECU preset is applied as the car's whole setup. Arms the
 *  automatic setup for a preset that wants it (and starts it now), and
 *  cancels any setup still owed otherwise — a car that has moved to a
 *  standalone ECU is not scanned on every boot for ever. */
void obd2_autosetup_for_ecu(const char *make, const char *version);

/** Arm and attempt now, for any car. `cb` (may be NULL) hears the outcome of
 *  every attempt until another start replaces it. Starting while a scan is
 *  already running just re-targets the listener. */
void obd2_autosetup_start(obd2_autosetup_cb_t cb, void *user);

/** Forget a setup still owed, and stop retrying. */
void obd2_autosetup_cancel(void);

/** Hear the outcome of attempts from now on (replaces any listener) — for a
 *  screen that wants to show a setup it did not start, like the wizard after
 *  applying a Falcon preset. */
void obd2_autosetup_listen(obd2_autosetup_cb_t cb, void *user);

/** Drop the listener if it is still `cb` — for a screen that is closing. */
void obd2_autosetup_forget(obd2_autosetup_cb_t cb);

/** A setup is owed to this car (persisted). */
bool obd2_autosetup_pending(void);

/** A scan for it is on the bus right now. */
bool obd2_autosetup_running(void);

/** Resume a setup owed from before this boot. Safe from any task. */
void obd2_autosetup_boot(void);

#ifdef __cplusplus
}
#endif
