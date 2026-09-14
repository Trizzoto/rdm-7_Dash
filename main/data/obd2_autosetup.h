/*
 * obd2_autosetup.h — set OBD2 up for a car without asking, or offer it
 * (ADR-0073, ADR-0074).
 *
 * Some cars are read through two doors at once: a factory ECU broadcasts part
 * of what a dash wants over CAN, and answers OBD2 for the rest. A Ford Falcon
 * BA/BF/FG broadcasts RPM, speed, coolant and throttle, but not ignition
 * timing, oil or fuel pressure, lambda or fuel trims — and an FG not intake
 * temperature or MAP either. OBD2 has those.
 *
 * Two modes, chosen by the preset:
 *
 *  SETUP — a preset that is known to want OBD2 (a Falcon). The discovery scan
 *    runs in the background and every reading the car answers that fills a
 *    channel nothing else feeds is bound. If the car doesn't answer yet
 *    (ignition off, dash on a bench) the setup stays owed — persisted — and is
 *    retried, including after a reboot, until the car answers once.
 *
 *  CHECK — any other preset. The same scan, once (with a few retries in the
 *    same session), but nothing is bound: if the car also answers OBD2 and
 *    some readings would fill channels the ECU leaves empty, that list is kept
 *    as an OFFER (persisted) for the channels editor, Device Settings and
 *    Studio to show — "your car also answers OBD2; these N could come from
 *    it". Some aftermarket ECUs and many factory ones answer; a race ECU that
 *    doesn't is left alone after the session's tries.
 *
 * Every function here runs on the LVGL task (the scan's callback is delivered
 * there, and the channel set is read under the LVGL mutex) — or with the LVGL
 * lock held — except obd2_autosetup_boot(), which may be called from anywhere.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "channel_source_apply.h"
#include "../can/obd2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool      check;      /* a CHECK attempt (offer only), not a SETUP      */
	bool      answered;   /* the car replied to the discovery scan          */
	uint8_t   readings;   /* PIDs it reported (bitmask PIDs not counted)     */
	size_t    offered;    /* of those, readings with a channel to go to      */
	size_t    fresh;      /* of those, channels nothing feeds yet            */
	size_t    bound;      /* channels this pass connected (SETUP only)       */
	esp_err_t err;        /* channel_apply_obd2() result; ESP_OK otherwise   */
	bool      will_retry; /* not answered, and another attempt is booked     */
} obd2_autosetup_result_t;

typedef void (*obd2_autosetup_cb_t)(const obd2_autosetup_result_t *r, void *user);

/** Does a car on this ECU preset want OBD2 set up behind it without asking? */
bool obd2_autosetup_ecu_wants(const char *make, const char *version);

/** Call after an ECU preset is applied as the car's whole setup. SETUP for a
 *  preset that wants it; otherwise cancels a setup still owed from a previous
 *  car and runs a CHECK, so an OBD2 offer can be made. */
void obd2_autosetup_for_ecu(const char *make, const char *version);

/** SETUP now, for any car. `cb` (may be NULL) hears every attempt's outcome
 *  until another start or listen replaces it. */
void obd2_autosetup_start(obd2_autosetup_cb_t cb, void *user);

/** CHECK now, for any car, and tell `cb` — for a screen that wants to know
 *  whether OBD2 is there before anyone has picked anything (the setup
 *  wizard's "No ECU detected" card). A SETUP already owed is left running and
 *  only gains the listener. */
void obd2_autosetup_check(obd2_autosetup_cb_t cb, void *user);

/** Forget a setup still owed, stop a check, and drop any offer. */
void obd2_autosetup_cancel(void);

/** Hear the outcome of attempts from now on (replaces any listener). */
void obd2_autosetup_listen(obd2_autosetup_cb_t cb, void *user);

/** Drop the listener if it is still `cb` — for a screen that is closing. */
void obd2_autosetup_forget(obd2_autosetup_cb_t cb);

/** A SETUP is owed to this car (persisted). */
bool obd2_autosetup_pending(void);

/** A scan for a SETUP or CHECK is on the bus right now. */
bool obd2_autosetup_running(void);

/** A CHECK is under way (scanning or waiting to retry). */
bool obd2_autosetup_checking(void);

/** The standing offer, resolved against the channel set as it is NOW: rows
 *  whose channel nothing feeds yet. Returns the count written to `out`; 0 when
 *  there is no offer or everything in it has since been set up (which retires
 *  the offer). `readings` (may be NULL) receives how many PIDs the car
 *  answered. LVGL task or lock held. */
size_t obd2_autosetup_offer(obd2_channel_match_t *out, size_t max_out,
                            uint8_t *readings);

/** The offer's raw answer, shaped as a scan result, so a scan sheet can show
 *  it without scanning again. False when there is no offer. */
bool obd2_autosetup_offer_scan(obd2_scan_result_t *out);

/** "Not now": forget the offer. The scan is still one tap away. */
void obd2_autosetup_dismiss_offer(void);

/** Resume a setup owed from before this boot and reload a standing offer.
 *  Safe from any task. */
void obd2_autosetup_boot(void);

#ifdef __cplusplus
}
#endif
