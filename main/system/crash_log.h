#pragma once

/**
 * crash_log — Track the previous boot's reset reason so panics are visible
 *             across reboots instead of disappearing into the void.
 *
 * Uses NVS (namespace "crash_log") to persist a small record:
 *   - last_reason   : esp_reset_reason_t from the previous boot
 *   - last_fw       : firmware version string at previous boot
 *   - last_uptime_s : seconds since boot at last clean shutdown
 *                     (0 if the previous boot crashed)
 *   - panic_count   : running total of crash-class reboots since flash
 *
 * Doesn't capture full backtraces — that needs ESP-IDF's coredump system,
 * which requires a partition table change. This is the minimum viable
 * "what happened last time" indicator.
 *
 * Surfaces results via ESP_LOGI at boot AND via the read API for the
 * Diagnostics screen / desktop app.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_reset_reason_t reason;        /* enum from esp_system.h */
    bool               was_crash;     /* PANIC / WDT / BROWNOUT etc */
    uint32_t           panic_count;   /* lifetime crashes since flash */
    uint32_t           last_uptime_s; /* 0 if crash, else seconds at clean shutdown */
    char               last_fw[16];   /* firmware version of previous boot */
} crash_log_record_t;

/**
 * @brief Read the previous boot's reset reason from ROM, persist a snapshot,
 *        and log a human-readable summary. Call once early in app_main, AFTER
 *        NVS is initialised.
 *
 * On crash-class reset reasons, this increments the lifetime panic_count.
 */
esp_err_t crash_log_init(void);

/**
 * @brief Get the saved snapshot of the previous boot. Filled in by
 *        crash_log_init() — safe to call any time after.
 */
const crash_log_record_t *crash_log_get(void);

/**
 * @brief Mark a clean shutdown — call right before `esp_restart()` or any
 *        graceful reboot path (OTA finish, settings reset, etc).
 *        Records current uptime so the next boot's record reflects that
 *        the previous run ended on purpose, not from a crash.
 */
void crash_log_mark_clean_shutdown(void);

#ifdef __cplusplus
}
#endif
