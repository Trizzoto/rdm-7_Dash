/**
 * crash_log.c — see crash_log.h.
 *
 * NVS layout (namespace "crash_log"):
 *   u8     last_reason     — esp_reset_reason_t from previous boot
 *   u32    panic_count     — lifetime crash-class reboot count
 *   u32    last_uptime_s   — 0 if previous boot crashed, else clean-shutdown uptime
 *   str    last_fw         — firmware version string at previous boot
 *
 * Sequence:
 *   crash_log_init() reads esp_reset_reason() on the current boot, treats
 *   that as "what caused the previous reset", saves it to NVS, and logs it.
 *   Then writes the current firmware version to last_fw for the next boot.
 *   crash_log_mark_clean_shutdown() updates last_uptime_s before a planned
 *   esp_restart() so the next boot can tell the previous run was clean.
 */
#include "system/crash_log.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "version.h"

static const char *TAG = "crash_log";
static const char *NVS_NS = "crash_log";

static crash_log_record_t s_record = {0};

static bool _is_crash_reason(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

static const char *_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_UNKNOWN:    return "UNKNOWN";
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    default:                 return "?";
    }
}

esp_err_t crash_log_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    s_record.reason    = reason;
    s_record.was_crash = _is_crash_reason(reason);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s) — crash log disabled for this run",
                 esp_err_to_name(err));
        /* still log the reason even without NVS persistence */
        ESP_LOGI(TAG, "Previous boot ended with: %s", _reason_str(reason));
        return err;
    }

    /* Read the saved snapshot from the prior run — what the previous
       crash_log_init wrote at the end of its own execution. */
    uint32_t panic_count = 0;
    uint32_t last_uptime_s = 0;
    size_t fw_len = sizeof(s_record.last_fw);

    nvs_get_u32(h, "panic_count",   &panic_count);
    nvs_get_u32(h, "last_uptime_s", &last_uptime_s);
    err = nvs_get_str(h, "last_fw", s_record.last_fw, &fw_len);
    if (err != ESP_OK) {
        s_record.last_fw[0] = '\0';
    }
    s_record.last_uptime_s = last_uptime_s;

    /* Increment crash counter if this boot was caused by a crash. */
    if (s_record.was_crash) {
        panic_count++;
        ESP_LOGW(TAG, "Previous boot CRASHED: reason=%s, previous fw=%s, "
                 "lifetime panic_count=%lu",
                 _reason_str(reason),
                 s_record.last_fw[0] ? s_record.last_fw : "(unknown)",
                 (unsigned long)panic_count);
    } else {
        ESP_LOGI(TAG, "Previous boot ended cleanly: reason=%s, previous fw=%s, "
                 "uptime=%lus, lifetime panic_count=%lu",
                 _reason_str(reason),
                 s_record.last_fw[0] ? s_record.last_fw : "(unknown)",
                 (unsigned long)last_uptime_s,
                 (unsigned long)panic_count);
    }
    s_record.panic_count = panic_count;

    /* Persist for next boot. last_uptime_s defaults to 0 — if we crash,
       that's what stays. mark_clean_shutdown() overwrites it on planned reboots. */
    nvs_set_u8(h,  "last_reason",   (uint8_t)reason);
    nvs_set_u32(h, "panic_count",   panic_count);
    nvs_set_u32(h, "last_uptime_s", 0);
    nvs_set_str(h, "last_fw",       FIRMWARE_VERSION);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

const crash_log_record_t *crash_log_get(void)
{
    return &s_record;
}

void crash_log_mark_clean_shutdown(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    nvs_set_u32(h, "last_uptime_s", uptime_s);
    nvs_commit(h);
    nvs_close(h);
}
