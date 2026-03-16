/*
 * signal.c — Phase 1 signal registry.
 *
 * Owns a PSRAM-backed array of signal_t descriptors.  Each signal decodes
 * its CAN bit-field exactly once per frame and pushes the result to all
 * registered subscribers.
 *
 * Threading: signal_dispatch_frame() and signal_check_timeouts() MUST be
 * called with the LVGL mutex held (i.e. from can_process_queued_frames()).
 * Subscriber callbacks therefore run on the LVGL task and may call LVGL
 * APIs directly.
 */

#include "widgets/signal.h"
#include "widgets/signal_sim.h"
#include "widgets/widget_types.h"
#include "can/can_decode.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "signal";

static signal_t *s_signals     = NULL;
static uint16_t  s_signal_count = 0;

/* ── Registry lifecycle ─────────────────────────────────────────────────── */

void signal_registry_init(void)
{
    if (s_signals) return; /* idempotent */

    s_signals = heap_caps_calloc(MAX_SIGNALS, sizeof(signal_t),
                                 MALLOC_CAP_SPIRAM);
    if (!s_signals) {
        ESP_LOGE(TAG, "Failed to allocate signal registry in PSRAM (%u bytes)",
                 (unsigned)(MAX_SIGNALS * sizeof(signal_t)));
        return;
    }
    s_signal_count = 0;
    ESP_LOGI(TAG, "Signal registry ready (%u slots, %u bytes each)",
             MAX_SIGNALS, (unsigned)sizeof(signal_t));
}

void signal_registry_reset(void)
{
    if (!s_signals) return;
    memset(s_signals, 0, MAX_SIGNALS * sizeof(signal_t));
    s_signal_count = 0;
}

/* ── Registration ───────────────────────────────────────────────────────── */

int16_t signal_register(const char *name, uint32_t can_id,
                        uint8_t start, uint8_t len,
                        float scale, float offset,
                        bool is_signed, uint8_t endian)
{
    if (!s_signals) {
        ESP_LOGE(TAG, "signal_registry_init() not called");
        return -1;
    }
    if (!name || name[0] == '\0') {
        ESP_LOGE(TAG, "signal_register: name must not be empty");
        return -1;
    }
    if (s_signal_count >= MAX_SIGNALS) {
        ESP_LOGE(TAG, "Signal registry full (max %u)", MAX_SIGNALS);
        return -1;
    }
    if (signal_find_by_name(name) >= 0) {
        ESP_LOGW(TAG, "Duplicate signal name '%s' rejected", name);
        return -1;
    }

    signal_t *sig = &s_signals[s_signal_count];
    memset(sig, 0, sizeof(signal_t));

    safe_strncpy(sig->name, name, sizeof(sig->name));
    sig->can_id     = can_id;
    sig->bit_start  = start;
    sig->bit_length = len;
    sig->scale      = scale;
    sig->offset     = offset;
    sig->is_signed  = is_signed;
    sig->endian     = endian;
    sig->is_stale   = true; /* stale until the first frame arrives */

    int16_t idx = (int16_t)s_signal_count;
    s_signal_count++;

    ESP_LOGD(TAG, "Registered signal[%d] '%s' CAN 0x%03lX bits %u+%u",
             idx, sig->name, (unsigned long)can_id, start, len);
    return idx;
}

int16_t signal_find_by_name(const char *name)
{
    if (!s_signals || !name) return -1;
    for (uint16_t i = 0; i < s_signal_count; i++) {
        if (strncmp(s_signals[i].name, name, sizeof(s_signals[i].name)) == 0)
            return (int16_t)i;
    }
    return -1;
}

signal_t *signal_get_by_index(uint16_t index)
{
    if (!s_signals || index >= s_signal_count) return NULL;
    return &s_signals[index];
}

uint16_t signal_get_count(void)
{
    return s_signal_count;
}

/* ── Subscription ───────────────────────────────────────────────────────── */

bool signal_subscribe(int16_t signal_index, signal_update_cb_t cb,
                      void *user_data)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count || !cb) {
        return false;
    }

    signal_t *sig = &s_signals[signal_index];

    if (sig->subscriber_count >= MAX_SIGNAL_SUBSCRIBERS) {
        ESP_LOGW(TAG, "Signal '%s' subscriber list full (%u max)",
                 sig->name, MAX_SIGNAL_SUBSCRIBERS);
        return false;
    }

    sig->subscribers[sig->subscriber_count].cb        = cb;
    sig->subscribers[sig->subscriber_count].user_data = user_data;
    sig->subscriber_count++;
    return true;
}

/* ── Internal: push to all subscribers ─────────────────────────────────── */

static void notify_subscribers(signal_t *sig)
{
    for (uint8_t i = 0; i < sig->subscriber_count; i++) {
        if (sig->subscribers[i].cb) {
            sig->subscribers[i].cb(sig->current_value, sig->is_stale,
                                   sig->subscribers[i].user_data);
        }
    }
}

/* ── Dispatch ───────────────────────────────────────────────────────────── */

void signal_dispatch_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if (!s_signals || !data) return;

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    for (uint16_t i = 0; i < s_signal_count; i++) {
        signal_t *sig = &s_signals[i];

        if (sig->can_id != can_id) continue;

        /* Guard: ensure the frame carries enough bytes for this signal.
         * can_extract_bits uses the same formula internally, so this
         * prevents reading uninitialised bytes. */
        uint8_t end_byte = (uint8_t)((sig->bit_start + sig->bit_length - 1) / 8);
        if (dlc <= end_byte) {
            ESP_LOGD(TAG, "Signal '%s': frame too short (dlc=%u, need>%u)",
                     sig->name, dlc, end_byte);
            continue;
        }

        int64_t raw = can_extract_bits(data, sig->bit_start, sig->bit_length,
                                       sig->endian, sig->is_signed);
        float decoded = (float)raw * sig->scale + sig->offset;

        sig->last_update_ms = now_ms;

        /* Only notify subscribers when the value actually changes or the
         * signal was previously stale — avoids redundant LVGL updates on
         * every duplicate CAN frame. */
        bool was_stale = sig->is_stale;
        sig->is_stale  = false;

        if (was_stale || decoded != sig->current_value) {
            sig->current_value = decoded;
            notify_subscribers(sig);
        }
    }
}

/* ── Test value injection ───────────────────────────────────────────────── */

void signal_inject_test_value(const char *name, float value)
{
    if (!s_signals || !name) return;

    int16_t idx = signal_find_by_name(name);
    if (idx < 0) {
        ESP_LOGW(TAG, "signal_inject_test_value: '%s' not found", name);
        return;
    }

    signal_t *sig = &s_signals[idx];
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    sig->current_value  = value;
    sig->is_stale       = false;
    sig->last_update_ms = now_ms;
    notify_subscribers(sig);
}

/* ── Timeout checking ───────────────────────────────────────────────────── */

void signal_check_timeouts(uint64_t current_time_ms)
{
    if (!s_signals) return;
    if (signal_sim_is_active()) return; /* sim keeps signals fresh */

    for (uint16_t i = 0; i < s_signal_count; i++) {
        signal_t *sig = &s_signals[i];

        /* Skip: already stale (already notified) or never received. */
        if (sig->is_stale || sig->last_update_ms == 0) continue;

        if (current_time_ms - sig->last_update_ms > SIGNAL_TIMEOUT_MS) {
            sig->is_stale = true;
            notify_subscribers(sig);
        }
    }
}
