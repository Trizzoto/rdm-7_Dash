/**
 * edit_mode.h — Dashboard Edit Mode arming state + UI pill.
 *
 * Live mode (default): widgets behave normally — Button widgets transmit
 * CAN, Toggle widgets flip state, long-press does nothing. Tap-on-dashboard
 * reveals the Menu pill (top-right) plus the grey Edit Mode pill alongside
 * it for 6 seconds.
 *
 * Edit mode (armed): tapping Edit Mode pill flips this state. Live widget
 * handlers suspend (Button doesn't TX, Toggle doesn't toggle), long-press on
 * a widget opens its config modal, and a red banner pins to the bottom of
 * the screen until exit. Exits on user tap of the pill, layout reload, or
 * reboot — no idle timeout.
 *
 * This module owns just the state flag, the pill button, and the banner.
 * Widget click handlers query `edit_mode_is_armed()` and bail. Long-press
 * registration in dashboard.c gates on the same flag.
 */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True when Edit Mode is currently armed. O(1), safe from any thread that
 *  holds the LVGL mutex (read-only access to a static bool). */
bool edit_mode_is_armed(void);

/** Enter Edit Mode: restyle pill to red "Exit Edit Mode", hide the blue
 *  Menu button, show the bottom banner. Idempotent. */
void edit_mode_enter(void);

/** Exit Edit Mode: restore pill styling and hide it, delete banner.
 *  Idempotent — safe to call from dashboard reload paths even when not
 *  currently armed. */
void edit_mode_exit(void);

/** Create the grey-translucent "Edit Mode" pill anchored top-right of the
 *  parent, sitting just left of the existing ui_Menu_Button. Caller does
 *  not own the returned handle; lifetime tracks the parent screen.
 *  Returns NULL on alloc failure. Pill is created hidden — first short tap
 *  on the dashboard reveals it via edit_mode_show_pill(). */
lv_obj_t *edit_mode_create_pill(lv_obj_t *parent);

/** Reveal the pill. Called from the dashboard short-tap handler in lockstep
 *  with the Menu button's reveal. Becomes a no-op if the pill hasn't been
 *  created yet. When armed, the pill is already visible and this is a no-op. */
void edit_mode_show_pill(void);

/** Hide the pill. Called from the 6-second auto-hide timer alongside the
 *  Menu button. When armed, the pill stays pinned and this is a no-op. */
void edit_mode_hide_pill(void);

#ifdef __cplusplus
}
#endif
