/**
 * remote_touch.h — Virtual LVGL input device driven by the web UI.
 *
 * When the browser-side Live Control toggle is on, mouse / trackpad events
 * on the Live Preview image are POSTed to /api/touch. The web handler calls
 * remote_touch_set() which feeds a virtual LVGL input device that coexists
 * with the real GT911 touch — either input source can drive the dashboard.
 *
 * Thread-safe: remote_touch_set() may be called from any task; state is
 * latched under a mutex and consumed on the LVGL task via the indev read_cb.
 *
 * Coordinates are in device pixels (0..SCREEN_W-1, 0..SCREEN_H-1).
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise the virtual input device. Call once, on the LVGL task,
 *  AFTER the real touch indev has been registered (order doesn't strictly
 *  matter to LVGL — but it keeps the logs tidy). */
void remote_touch_init(lv_disp_t *disp);

/** Latch a new pointer state. `pressed` = true for down/move, false for up.
 *  Safe to call from any task. */
void remote_touch_set(int16_t x, int16_t y, bool pressed);

/** Globally enable/disable remote touch input. When disabled, the virtual
 *  indev reports "released" regardless of what remote_touch_set() was last
 *  told — prevents a stale click from firing when the user toggles off. */
void remote_touch_set_enabled(bool on);

/** Query current enable state (for /api/touch GET / UI sync). */
bool remote_touch_is_enabled(void);

#ifdef __cplusplus
}
#endif
