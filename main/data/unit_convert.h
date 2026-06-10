#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
