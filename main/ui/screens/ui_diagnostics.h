#pragma once

/* System Diagnostics screen — read-only at-a-glance view of CAN bus, SD card,
 * WiFi, signals, and system health. Opened from Device Settings. */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show the diagnostics screen. The current screen at call time becomes the
 * "return screen" that we restore when the user hits Back. */
void diagnostics_ui_show(void);

/* Hide and destroy the diagnostics screen. Safe no-op if not active. */
void diagnostics_ui_hide(void);

bool diagnostics_ui_is_active(void);

#ifdef __cplusplus
}
#endif
