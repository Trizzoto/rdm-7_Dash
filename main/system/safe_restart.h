/*
 * safe_restart.h — reboot without the other core faulting on the way down.
 *
 * WHY THIS EXISTS
 *
 * esp_restart() ends in esp_restart_noos(), and on the ESP32-S3 that function
 * disables the cache BEFORE it stalls the other core
 * (esp-idf 5.3.1, components/esp_system/port/soc/esp32s3/system_internal.c:
 * Cache_Disable_ICache() at line 105, "Reset and stall the other CPU" at 108).
 * esp_restart() suspends only the calling core's scheduler (vTaskSuspendAll),
 * so for that window the other core is still running ordinary tasks out of
 * flash/PSRAM-mapped memory with no cache behind them. The first instruction it
 * fetches raises "Cache disabled but cached memory region accessed".
 *
 * On this dash the other core is core 1, running LVGL, and the window is wide
 * enough to hit often: measured 2 crashes in 6 API reboots, both firmware
 * before and after the Sept 2026 display work, so this is long-standing and
 * not a regression. Every one of those turned a deliberate restart into a
 * logged panic — which is what crash_log_mark_clean_shutdown() was added to
 * prevent, and could not, because the panic happens after it.
 *
 * IDF is aware of the failure mode: the same function moves the current core's
 * stack out of PSRAM a few lines earlier to avoid exactly this error. It just
 * does not extend the courtesy to the other core.
 *
 * Stalling the other core ourselves first closes the window: a stalled core
 * fetches nothing, so it cannot fault when the cache goes.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Stall the other core, then restart. Drop-in for esp_restart().
 *
 * Does not return. Shutdown handlers still run (WiFi/BT stop and the like),
 * exactly as with esp_restart() — they run on the calling core with the other
 * one held, which is the same position esp_restart_noos() puts them in a few
 * microseconds later anyway.
 *
 * Callers that want the reboot recorded as deliberate should still call
 * crash_log_mark_clean_shutdown() first; this only stops the *panic*, it does
 * not classify the restart.
 */
void rdm_safe_restart(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
