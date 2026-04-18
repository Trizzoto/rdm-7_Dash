#pragma once

/* Peak / Min Values screen — scrollable table listing every registered signal
 * with its current value, recorded peak (max), and recorded min. Lets the
 * user reset all peaks from one place. Opened from Device Settings. */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void peaks_ui_show(void);
void peaks_ui_hide(void);
bool peaks_ui_is_active(void);

#ifdef __cplusplus
}
#endif
