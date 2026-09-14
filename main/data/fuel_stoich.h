#pragma once

/*
 * fuel_stoich — which fuel is in the tank, and therefore what λ 1.00 is in AFR.
 *
 * Owns the per-dash fuel choice (NVS) and pushes the matching stoichiometric
 * ratio into unit_convert(), which is where every λ <-> AFR conversion on the
 * dash happens. The arithmetic lives in fuel_stoich_calc.h.
 *
 * FLEX derives the ratio live from the ethanol_pct channel, because a flex car
 * is exactly the one a fixed setting gets wrong: its ethanol content moves with
 * every tank, and "E85" is wrong the day it is filled with a 60% blend.
 *
 * The default is PETROL, which reproduces the hardcoded 14.7 the dash always
 * used — so nothing changes on update for anyone who does not choose a fuel.
 * That matters for flex cars in particular: an automatic default would have
 * silently moved their AFR readout from petrol-equivalent to true-blend on an
 * OTA update, under a tuner who may have been working in petrol-equivalent on
 * purpose.
 */

#include <stdbool.h>
#include "esp_err.h"
#include "fuel_stoich_calc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the stored fuel, apply its ratio, and start the flex timer. LVGL task.
 * Idempotent — dashboard_init() calls it on every layout reload. */
void fuel_stoich_start(void);

fuel_mode_t fuel_stoich_get_mode(void);

/* Persist a new fuel and apply it. Safe from any task: the store happens here
 * and the apply (which notifies widgets) is deferred to the LVGL task. */
esp_err_t fuel_stoich_set_mode(fuel_mode_t mode);

/* The ratio in use right now. */
float fuel_stoich_current(void);

/* FLEX only: whether an ethanol reading has arrived this boot, and the last
 * one seen (NAN when none). Lets the UI say "waiting for the ethanol sensor"
 * instead of implying the fallback ratio is a measurement. */
bool  fuel_stoich_flex_has_reading(void);
float fuel_stoich_flex_ethanol_pct(void);

#ifdef __cplusplus
}
#endif
