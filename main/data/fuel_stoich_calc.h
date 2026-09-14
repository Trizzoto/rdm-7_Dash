#pragma once

/*
 * fuel_stoich_calc.h — the arithmetic behind the dash's air-fuel ratio.
 *
 * Header-only and dependency-free on purpose: no LVGL, no NVS, no channel
 * store. The runtime half (main/data/fuel_stoich.c) owns WHICH fuel is in the
 * tank; this file only says what that fuel's stoichiometric ratio is, so the
 * sums can be checked on a host (tests/native/test_fuel_stoich.c).
 *
 * Why this exists at all: an ECU that reports lambda is telling you something
 * fuel-independent — λ 1.00 is "exactly enough air" on any fuel. AFR is not:
 * that same λ 1.00 is 14.7 on petrol, 9.8 on E85, 6.4 on methanol. The dash
 * used to convert λ -> AFR with a hardcoded 14.7, so an E85 car read
 * petrol-equivalent AFR — "14.7" at stoich while its tuning software said 9.8.
 * Nothing was calculated wrong, but it disagreed with the laptop, which is the
 * one comparison a tuner actually makes.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	FUEL_PETROL   = 0,   /* 14.7 — the default, identical to before this existed */
	FUEL_E10      = 1,
	FUEL_E85      = 2,
	FUEL_E100     = 3,
	FUEL_METHANOL = 4,
	FUEL_FLEX     = 5,   /* live, from the ethanol_pct channel */
	FUEL__COUNT
} fuel_mode_t;

/* Stoichiometric AFR of the pure fuels, by mass. */
#define FUEL_AFR_PETROL    14.7f
#define FUEL_AFR_ETHANOL    9.0f
#define FUEL_AFR_METHANOL   6.4f

/* Densities (kg/L) for turning a VOLUME blend into a MASS fraction. */
#define FUEL_RHO_ETHANOL    0.789f
#define FUEL_RHO_PETROL     0.745f

/*
 * Stoichiometric AFR of a petrol/ethanol blend, given ethanol percent BY
 * VOLUME — which is what flex-fuel sensors, OBD2 PID 0x52, Link's "ETHANOL %"
 * and Haltech's fuel composition all report.
 *
 * AFR mixes by MASS, not volume, so the volume fraction is converted first.
 * Interpolating linearly by volume instead gives 9.86 for E85; by mass it
 * gives 9.81, which is the figure tuners quote. Deriving the fixed E10 / E85 /
 * E100 presets from this same function (fuel_stoich_for_mode) means a Flex
 * car reading 85% and a car set to "E85" can never disagree.
 *
 * Out-of-range and non-finite input clamps to the nearest pure fuel rather
 * than producing a ratio outside [9.0, 14.7].
 */
static inline float fuel_stoich_for_ethanol(float pct) {
	if (!isfinite(pct) || pct <= 0.0f) return FUEL_AFR_PETROL;
	if (pct >= 100.0f) return FUEL_AFR_ETHANOL;
	float v  = pct / 100.0f;
	float me = FUEL_RHO_ETHANOL * v;
	float mg = FUEL_RHO_PETROL  * (1.0f - v);
	float w  = me / (me + mg);                  /* ethanol mass fraction */
	return w * FUEL_AFR_ETHANOL + (1.0f - w) * FUEL_AFR_PETROL;
}

/* The fixed stoich for a mode. FLEX has no fixed value — it returns petrol,
 * which is the runtime's fallback until an ethanol reading has arrived. */
static inline float fuel_stoich_for_mode(fuel_mode_t m) {
	switch (m) {
	case FUEL_E10:      return fuel_stoich_for_ethanol(10.0f);
	case FUEL_E85:      return fuel_stoich_for_ethanol(85.0f);
	case FUEL_E100:     return fuel_stoich_for_ethanol(100.0f);
	case FUEL_METHANOL: return FUEL_AFR_METHANOL;
	case FUEL_PETROL:
	case FUEL_FLEX:
	default:            return FUEL_AFR_PETROL;
	}
}

/* Stable API/NVS-independent key for a mode, and back. The key — not the enum
 * value — is what crosses the web API, so reordering the enum later cannot
 * silently reinterpret a stored or requested fuel. */
static inline const char *fuel_mode_key(fuel_mode_t m) {
	switch (m) {
	case FUEL_PETROL:   return "petrol";
	case FUEL_E10:      return "e10";
	case FUEL_E85:      return "e85";
	case FUEL_E100:     return "e100";
	case FUEL_METHANOL: return "methanol";
	case FUEL_FLEX:     return "flex";
	default:            return "petrol";
	}
}

static inline bool fuel_mode_from_key(const char *key, fuel_mode_t *out) {
	if (!key || !out) return false;
	for (int i = 0; i < FUEL__COUNT; ++i) {
		if (strcmp(key, fuel_mode_key((fuel_mode_t)i)) == 0) {
			*out = (fuel_mode_t)i;
			return true;
		}
	}
	return false;
}

#ifdef __cplusplus
}
#endif
