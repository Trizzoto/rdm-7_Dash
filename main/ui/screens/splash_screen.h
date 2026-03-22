#pragma once

#include "cJSON.h"
#include "lvgl.h"
#include "widgets/widget_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Show the splash screen at boot. Loads custom layout from _splash.json
 *  if available, otherwise shows default RDM logo. Starts a 900 ms timer
 *  that auto-transitions to the dashboard. */
void show_splash_screen(void);

/* ── Splash edit mode (used by web editor) ────────────────────────────── */

/** Enter splash edit mode: cancel any boot timer, create a splash screen
 *  from `_splash.json` (or blank), and make it the active screen.
 *  Called from web server via lv_async_call. */
void splash_screen_enter_edit_mode(void);

/** Exit splash edit mode: reload the dashboard (Screen3). */
void splash_screen_exit_edit_mode(void);

/** Apply a preview layout JSON to the splash screen without saving.
 *  Must be called on the LVGL task. */
void splash_screen_apply_preview(cJSON *root);

/** Get the current splash widgets (for GET /api/layout/current). */
widget_t **splash_screen_get_widgets(void);
uint8_t splash_screen_get_widget_count(void);

/** Check whether a custom splash layout file exists on LittleFS. */
bool splash_screen_has_custom(void);

/** Check whether splash edit mode is currently active. */
bool splash_screen_is_edit_mode(void);

/** Get the bare name of the active splash (e.g. "Default", "Racing"). */
const char *splash_screen_get_active_name(void);

/** Set the active splash name in the module's memory (does NOT write NVS). */
void splash_screen_set_active_name(const char *name);

#ifdef __cplusplus
}
#endif
