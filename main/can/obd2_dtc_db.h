/**
 * obd2_dtc_db.h — Offline DTC code → human description lookup.
 *
 * Static table of the most-cited generic SAE J2012 Powertrain DTCs.
 * Manufacturer-specific codes (P1xxx, P2xxx after the generic ranges,
 * P3xxx, C1xxx, B1xxx, U1xxx) are NOT in this table — they vary per
 * vehicle and would bloat the binary. The Code Reader modal falls
 * back to showing just the code with no description for unknowns.
 *
 * Sized to fit in flash, not RAM: ~100 entries × ~50 bytes each ≈
 * 5 KB rodata. No runtime memory cost beyond the pointer lookup.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Look up a DTC code (e.g. "P0420") and return its description, or
 * NULL if the code isn't in the offline database. Lifetime is static
 * (the description lives in flash) so the caller doesn't need to copy.
 *
 * Lookup is linear (the table is small enough that a binary-search
 * tree isn't worth the extra code). */
const char *obd2_dtc_lookup(const char *code);

#ifdef __cplusplus
}
#endif
