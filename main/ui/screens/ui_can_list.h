#pragma once

/* Live CAN ID viewer - scrollable table of every CAN ID seen on the bus
 * with its current data bytes, rolling Hz, and DLC. Uses the row-cache
 * pattern (build once, refresh in place) so the S3 doesn't have to redraw
 * the whole list every tick. Opened from System Diagnostics. */

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void can_list_ui_show(void);
void can_list_ui_hide(void);
bool can_list_ui_is_active(void);

/* Embedded variant: build the live CAN table (column header + scrollable
 * list) into an existing container instead of taking over the whole screen.
 * The caller owns `parent` and its geometry; the table fills it via flex.
 * Used by the Device Settings "CAN Bus" popup. Mutually exclusive with
 * can_list_ui_show() — they share module state, so never have both live.
 * Call can_list_ui_embed_stop() before the parent is deleted to tear down
 * the refresh timer and drop the cached row pointers. */
void can_list_ui_embed(lv_obj_t *parent);
void can_list_ui_embed_stop(void);

#ifdef __cplusplus
}
#endif
