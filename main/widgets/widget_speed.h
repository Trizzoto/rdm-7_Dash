#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Create speed value label and KMH/MPH units label on parent. */
void widget_speed_create(lv_obj_t *parent);

/** Immediate speed UI update. */
void update_speed_ui_immediate(const char *speed_str);

/** Async speed UI update (lv_async_call compatible). */
void update_speed_ui(void *param);

/** Returns pointer to last_speed_can_received for use by dispatcher. */
uint64_t *widget_speed_get_last_can_time(void);

#ifdef __cplusplus
}
#endif
