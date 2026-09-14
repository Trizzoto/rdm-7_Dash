#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * unit_convert — convert a measured value between a "native" unit (what the
 * ECU/CAN frame transmits, e.g. kPa) and a user-chosen "display" unit (e.g.
 * psi). Channels store and compare values in their native unit; the conversion
 * applies only at presentation time.
 *
 * Linear conversions only (out = v * scale + offset). Unknown pairs and equal
 * units pass the value through unchanged, so a relabel without a known
 * conversion never silently scales (it just shows the same number).
 */

/* Convert @v from unit @from to unit @to. Returns @v unchanged when either
 * string is NULL/empty, when from == to, or when no conversion is known. */
float unit_convert(float v, const char *from, const char *to);

/* True if a (non-identity) conversion exists between @from and @to. Lets the
 * UI decide whether a display-unit choice is meaningful for a given channel. */
bool unit_convert_supported(const char *from, const char *to);

/* Enumerate the units convertible to/from @from (excluding @from itself).
 * Fills @out with up to @max pointers into the static conversion table
 * (no allocation; strings live for the program). Returns the count — 0
 * means the unit has no known conversions (UI should hide the picker). */
size_t unit_convert_targets(const char *from, const char **out, size_t max);

/*
 * Stoichiometric air-fuel ratio used for λ <-> AFR.
 *
 * λ is fuel-independent; AFR is not — the same λ 1.00 is 14.7 AFR on petrol,
 * 9.8 on E85 and 6.4 on methanol. So this one pair cannot be a fixed table
 * entry the way kPa -> psi is. The default is 14.7, which is exactly what the
 * table always used, so nothing changes until a fuel is chosen
 * (main/data/fuel_stoich.c owns that choice and pushes the value in here).
 *
 * Kept as a plain setter rather than a lookup into the fuel module so this
 * file stays dependency-free: three host test suites link it directly.
 *
 * Out-of-range or non-finite values are refused and leave the previous
 * stoich in place — a garbage ratio would silently mis-scale every AFR
 * readout on the dash. Returns true when the value was accepted.
 */
#define UNIT_STOICH_PETROL  14.7f
#define UNIT_STOICH_MIN      5.0f
#define UNIT_STOICH_MAX     20.0f
bool  unit_convert_set_stoich(float afr);
float unit_convert_get_stoich(void);

#ifdef __cplusplus
}
#endif
