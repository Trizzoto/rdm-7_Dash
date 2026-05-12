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
#include "widgets/widget_types.h"
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

/* ─── Selection + drag (active while armed) ───────────────────────────────── */

/** Currently-selected widget, or NULL if none. Cheap; just returns a pointer
 *  from the module's static state. */
widget_t *edit_mode_get_selected(void);

/** Replace selection. Pass NULL to deselect. Re-uses the ring on lv_layer_top.
 *  No-op if not armed. */
void edit_mode_select(widget_t *w);


/** Refresh the selection ring's position/size against the current widget.
 *  Call this after the widget's root has moved or resized. */
void edit_mode_refresh_selection(void);

/* ─── Event callbacks (registered per widget by dashboard.c) ──────────────── */
/*
 * Wiring contract:
 *   - LV_EVENT_PRESSED   → edit_mode_widget_pressed_cb   (user_data = widget_t*)
 *   - LV_EVENT_PRESSING  → edit_mode_widget_pressing_cb  (user_data = widget_t*)
 *   - LV_EVENT_RELEASED  → edit_mode_widget_released_cb  (user_data = widget_t*)
 *
 * All three bail unless Edit Mode is armed, so registering them is cheap
 * even when the user is in live mode.
 */
void edit_mode_widget_pressed_cb(lv_event_t *e);
void edit_mode_widget_pressing_cb(lv_event_t *e);
void edit_mode_widget_released_cb(lv_event_t *e);

/** Registered on ui_Screen3 background — deselects on empty-area press.
 *  When the press hits a widget, the widget intercepts and this never fires. */
void edit_mode_screen_pressed_cb(lv_event_t *e);

/** Re-foreground the editor chrome (top + bottom toolbar) on the active
 *  screen. Call after a layout reapply (undo/redo/duplicate) so the freshly
 *  created widget objects don't end up drawing over the toolbars.
 *  No-op when no chrome is currently up. */
void edit_mode_refresh_zorder(void);

/** Called by external editors (the Inspector, etc.) after they've directly
 *  mutated the currently-selected widget. Refreshes the selection chrome
 *  to the widget's new bounds, flushes any in-progress undo session, pushes
 *  a fresh snapshot to capture the external edit as its own undo step, and
 *  schedules a layout save. No-op when not armed. */
void edit_mode_commit_external_edit(void);

#ifdef __cplusplus
}
#endif
