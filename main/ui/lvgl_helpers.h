#ifndef LVGL_HELPERS_H
#define LVGL_HELPERS_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Declare the LVGL mutex as extern
extern SemaphoreHandle_t lvgl_mux;

// LVGL mutex helpers (project-wide). All lv_* calls must hold this lock,
// except those already running on the LVGL task (timer/event/signal callbacks
// — the dispatcher acquires the lock for those). The mutex is recursive.
//
// Pass timeout_ms = -1 for portMAX_DELAY (block indefinitely),
// 0 for non-blocking (try-lock), or a positive value for ms timeout.
bool rdm_lvgl_lock(int timeout_ms);
void rdm_lvgl_unlock(void);

#endif // LVGL_HELPERS_H
