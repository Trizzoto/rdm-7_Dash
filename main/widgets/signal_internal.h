#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the internal signal injection timer (LVGL timer, 500 ms).
 * Must be called from the LVGL task after signal_registry_init().
 */
void signal_internal_start(void);

/**
 * Stop and delete the timer.
 */
void signal_internal_stop(void);

#ifdef __cplusplus
}
#endif
