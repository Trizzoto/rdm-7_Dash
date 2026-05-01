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

#ifdef __cplusplus
}
#endif
