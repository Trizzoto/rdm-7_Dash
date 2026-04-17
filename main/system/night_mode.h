/*
 * night_mode.h — Single source of truth for the runtime "is night mode active"
 *                state. Widgets, brightness control, and the dashboard subscribe
 *                here; settings UI / CAN signal triggers / time-based rules call
 *                the setter to change state.
 *
 * Three sources can flip night mode on/off:
 *   1. Manual toggle from device settings (drives night_mode_config.manual_active)
 *   2. CAN signal binding configured per-layout (e.g. headlight-on signal)
 *   3. (Future) Time-of-day based on RTC
 *
 * Whichever source is most recent wins — there's no priority logic in v1.
 *
 * On state change, registered subscribers fire (max NIGHT_MODE_MAX_SUBSCRIBERS).
 * Subscribers run on the LVGL task because they touch widget LVGL objects.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIGHT_MODE_MAX_SUBSCRIBERS 64

/* Subscriber callback: fires on state change with the new active state. */
typedef void (*night_mode_change_cb_t)(bool active, void *user_data);

/* Initialise the module. Call once during boot, after layout_manager_init() and
 * after signal subsystem is up. Idempotent. */
void night_mode_init(void);

/* Set the runtime active flag. Fires all subscribers if state changed. Also
 * updates the `__NIGHT_MODE` internal signal (0/1). Safe to call from any task
 * — internally posts to the LVGL task via lv_async_call. */
void night_mode_set_active(bool active);

/* Current state. */
bool night_mode_is_active(void);

/* Register a subscriber that fires on state changes. Returns true on success
 * or false if the subscriber table is full. user_data is passed back to the cb. */
bool night_mode_subscribe(night_mode_change_cb_t cb, void *user_data);

/* Unregister a subscriber by (cb, user_data) pair. */
void night_mode_unsubscribe(night_mode_change_cb_t cb, void *user_data);

/* Drop all subscribers — used by dashboard re-init to avoid stale pointers
 * to deleted widgets. */
void night_mode_clear_subscribers(void);

#ifdef __cplusplus
}
#endif
