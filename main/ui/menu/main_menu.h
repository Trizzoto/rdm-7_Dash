/*
 * main_menu.h — the launcher the dock's Setup button opens (ADR-0075).
 *
 * Eight tiles, each with a live status line: Layouts, Screen, Recording,
 * Live data, Channels, Your car, Connect, This dash. Tiles open a popup over
 * the launcher or a page (device_settings_open_page); every page's Back
 * returns here and Close returns to the dashboard.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Build the launcher and show it. @p return_screen is where Close goes
 *  (normally the dashboard). Replaces any menu screen currently shown. */
void main_menu_open(lv_obj_t *return_screen);

/** Where Close goes. NULL when no menu session is open. */
lv_obj_t *main_menu_return_screen(void);

/** Show @p screen in place of the menu screen on the glass and delete that
 *  one — the one navigation step every menu page uses. */
void main_menu_swap_to(lv_obj_t *screen);

/** Something that can change how the dashboard is built (CAN, channels…)
 *  was opened, so Close must rebuild the dashboard rather than just show it. */
void main_menu_mark_dirty(void);

/** Close handler for any menu screen: back to the dashboard. */
void main_menu_close_cb(lv_event_t *e);

/** Back handler for pages: back to the launcher. */
void main_menu_back_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif
