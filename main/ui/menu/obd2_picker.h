/**
 * obd2_picker.h — On-device modal for configuring OBD2 polled signals.
 *
 * Opens from Device Settings (button under the ECU dropdown). One modal
 * serves three contexts:
 *   - Primary OBD2 preset: edit the full polled-PID list.
 *   - Native preset with supplemental OBD2: edit the gap-filler list,
 *     with "provided by preset" PIDs shown but uncheckable.
 *   - Native preset without OBD2 yet: same UI, all checkboxes available.
 *
 * The Scan Vehicle button triggers obd2_discovery_start; once complete,
 * "Supported" badges appear next to PIDs the car responded to.
 *
 * On Save, the modal writes the new enabled list to the active layout's
 * `obd2_pids` array via ecu_preset_save_obd2_pids and calls obd2_start
 * to restart polling. The dashboard reloads to pick up signal-name changes.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the picker modal on the top layer. Reads the current state from
 *  the active layout JSON + signal registry. Idempotent — calling while
 *  the modal is open is a no-op. */
void obd2_picker_open(void);

/** Close the modal, freeing all LVGL objects. Safe to call when closed. */
void obd2_picker_close(void);

/** True while the modal is open. */
bool obd2_picker_is_open(void);

#ifdef __cplusplus
}
#endif
