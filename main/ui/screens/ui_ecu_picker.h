/**
 * ui_ecu_picker.h - full-screen ECU selector overlay.
 *
 * Shared by first_run_wizard (as step 2 of 3) and device_settings
 * ("ECU" row). Presents the list from ecu_presets.c plus a "Custom"
 * option for users with a non-listed ECU. On confirm, persists the
 * choice to NVS and applies the preset to the named layout.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback invoked after the picker closes.
 * @param applied  true if the user picked an ECU (preset was applied),
 *                 false if they skipped / chose Custom.
 * @param ctx      Opaque user context from ecu_picker_open().
 */
typedef void (*ecu_picker_done_cb_t)(bool applied, void *ctx);

/**
 * Open the picker overlay on the top layer.
 * @param layout_name  Layout to apply the preset to (typically "default").
 * @param allow_skip   If true, shows a "Skip / Custom" button in addition
 *                     to Apply. First-run wizard uses true; Device Settings
 *                     uses true but with a different label ("Cancel").
 * @param cb           Optional completion callback.
 * @param ctx          Passed to cb.
 */
void ecu_picker_open(const char *layout_name, bool allow_skip,
                     ecu_picker_done_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
