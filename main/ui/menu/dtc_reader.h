/**
 * dtc_reader.h — On-device "Read Trouble Codes" modal.
 *
 * Opens from Device Settings. Shows three sets of DTCs from the vehicle:
 *   - Stored (Mode 03)    — confirmed faults, may have lit the MIL
 *   - Pending (Mode 07)   — detected but not yet "matured" / not confirmed
 *   - Permanent (Mode 0A) — federal regs, survive a Mode 04 clear
 *
 * Three tabs flip between buckets; refresh polls all three on open.
 * "Clear Codes" button issues Mode 04 (with two-tap confirmation) —
 * removes stored + pending. Permanent codes only clear themselves once
 * the underlying self-tests pass.
 *
 * Each code shows its 5-char ID (e.g. "P0420") plus a description from
 * the offline obd2_dtc_db when available, raw code only otherwise.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the DTC reader. Idempotent. */
void dtc_reader_open(void);

/* Close the modal and tear down all LVGL objects. */
void dtc_reader_close(void);

/* True while the modal is open. */
bool dtc_reader_is_open(void);

#ifdef __cplusplus
}
#endif
