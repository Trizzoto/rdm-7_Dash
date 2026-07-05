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
#include "lvgl.h"

#include <float.h>
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
    /* Free any per-signal value-label maps from the previous layout before
     * zeroing the array — these are PSRAM allocations owned by the signal. */
    for (uint16_t i = 0; i < s_signal_count; i++) {
        if (s_signals[i].value_map) {
            heap_caps_free(s_signals[i].value_map);
            s_signals[i].value_map = NULL;
            s_signals[i].value_map_count = 0;
        }
    }
    memset(s_signals, 0, MAX_SIGNALS * sizeof(signal_t));
    s_signal_count = 0;
}

/* ── Registration ───────────────────────────────────────────────────────── */

int16_t signal_register(const char *name, uint32_t can_id,
                        uint8_t start, uint8_t len,
                        float scale, float offset,
                        bool is_signed, uint8_t endian,
                        const char *unit)
{
    return signal_register_with_source(name, can_id, start, len,
                                       scale, offset, is_signed, endian,
                                       unit, SIGNAL_SOURCE_CAN);
}

int16_t signal_register_with_source(const char *name, uint32_t can_id,
                                    uint8_t start, uint8_t len,
                                    float scale, float offset,
                                    bool is_signed, uint8_t endian,
                                    const char *unit,
                                    signal_source_t source)
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
    int16_t existing = signal_find_by_name(name);
    if (existing >= 0) {
        /* Expected on cross-layout switches: signals MERGE across layout
         * loads (the registry is NOT reset on the happy path — see
         * layout_manager.c _instantiate_widgets), so a name registered by
         * an earlier layout is still present when a later layout re-declares
         * it.
         *
         * Previously the first registration won and the duplicate was
         * dropped (return -1). That silently shadowed the correct decode:
         * if a splash/boot layout serialized a frozen snapshot of the whole
         * registry and was loaded before the dashboard, the dashboard's
         * authoritative decode params were discarded and the signal decoded
         * garbage / never matched a frame (survived reboot). Fix: UPSERT —
         * overwrite the existing slot's decode params with the freshly
         * loaded layout's values (latest-layout-wins) while leaving the
         * slot's subscribers, peak/min/session stats, value_map and
         * test_locked intact so existing channel bindings (resolved by
         * index) stay attached. */
        signal_t *cur = &s_signals[existing];
        cur->can_id     = can_id;
        cur->bit_start  = start;
        cur->bit_length = len;
        cur->scale      = scale;
        cur->offset     = offset;
        cur->is_signed  = is_signed;
        cur->endian     = endian;
        cur->source     = (uint8_t)source;
        if (unit && unit[0] != '\0')
            safe_strncpy(cur->unit, unit, sizeof(cur->unit));
        else
            cur->unit[0] = '\0';
        /* Reset decode freshness so the next frame re-evaluates and fires
         * notify_subscribers — mirrors channel_source_apply.c's in-place
         * patch. Without this a still-fresh slot whose current_value happens
         * to match the new decode could swallow the first update. */
        cur->is_stale       = true;
        cur->last_update_ms = 0;
        /* Release any test lock. Preserving it here while resetting the
         * slot to stale created a ZOMBIE: dispatch kept skipping the signal
         * (locked) and the staleness sweep kept skipping it too, so one
         * forgotten editor Test Value + any layout/channel reload (the
         * editor autosaves constantly) left the signal permanently dead
         * showing "--" with no visible reason. Test locks are ephemeral
         * editor state — a reload releases them. */
        if (cur->test_locked) {
            ESP_LOGI(TAG, "signal '%s': releasing test lock on re-register", name);
            cur->test_locked  = false;
            cur->test_lock_ms = 0;
        }
        ESP_LOGD(TAG, "signal '%s' already registered — updating decode params", name);
        return existing;
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
    sig->source     = (uint8_t)source;
    sig->is_stale   = true; /* stale until the first frame arrives */

    /* Unit string */
    if (unit && unit[0] != '\0')
        safe_strncpy(sig->unit, unit, sizeof(sig->unit));
    else
        sig->unit[0] = '\0';

    /* Peak/min tracking */
    sig->peak_value      = -FLT_MAX;
    sig->min_value       = FLT_MAX;
    sig->session_peak    = -FLT_MAX;
    sig->session_min     = FLT_MAX;
    sig->tracking_active = true;

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

/* ── Value-label map ───────────────────────────────────────────────────── */

bool signal_set_value_map(int16_t signal_index,
                          const signal_value_label_t *entries,
                          uint8_t count)
{
    if (!s_signals || signal_index < 0 || signal_index >= (int16_t)s_signal_count)
        return false;
    signal_t *sig = &s_signals[signal_index];

    /* Free any prior map. signal_registry_reset() zeroes the array so we
     * don't free across resets — value_map ptrs live only while the layout
     * is active. Layout reload calls reset before _load_signals so this is
     * the only path that allocates them. */
    if (sig->value_map) {
        heap_caps_free(sig->value_map);
        sig->value_map = NULL;
        sig->value_map_count = 0;
    }
    if (!entries || count == 0) return true;

    if (count > SIGNAL_VALUE_MAP_MAX) count = SIGNAL_VALUE_MAP_MAX;
    sig->value_map = heap_caps_calloc(count, sizeof(signal_value_label_t),
                                       MALLOC_CAP_SPIRAM);
    if (!sig->value_map) {
        ESP_LOGE(TAG, "value_map alloc failed for '%s' (%u entries)",
                 sig->name, count);
        return false;
    }
    memcpy(sig->value_map, entries, (size_t)count * sizeof(signal_value_label_t));
    sig->value_map_count = count;
    return true;
}

const char *signal_value_lookup_label(int16_t signal_index, float value)
{
    if (!s_signals || signal_index < 0 || signal_index >= (int16_t)s_signal_count)
        return NULL;
    signal_t *sig = &s_signals[signal_index];
    if (!sig->value_map || sig->value_map_count == 0) return NULL;

    /* Match on |decoded - entry| < epsilon so both integer-coded signals
     * (gear 0..7) and decimal-coded signals (lambda 0.85 / 1.0 / 1.15)
     * resolve cleanly. Epsilon also absorbs the tiny float drift that
     * raw*scale+offset introduces — e.g. decoder returns 6.9999998f when
     * the bit field is literally 7. */
    for (uint8_t i = 0; i < sig->value_map_count; i++) {
        float d = value - sig->value_map[i].value;
        if (d < 0.0f) d = -d;
        if (d < SIGNAL_VALUE_MAP_EPSILON) return sig->value_map[i].label;
    }
    return NULL;
}

void signal_format_value(int16_t signal_index, float value,
                         uint8_t decimals, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    if (signal_index >= 0) {
        const char *lbl = signal_value_lookup_label(signal_index, value);
        if (lbl) {
            /* Single-line copy; truncates safely if label is longer than buf. */
            size_t n = strlen(lbl);
            if (n >= cap) n = cap - 1;
            memcpy(buf, lbl, n);
            buf[n] = '\0';
            return;
        }
    }
    if (decimals == 0) snprintf(buf, cap, "%d", (int)value);
    else               snprintf(buf, cap, "%.*f", decimals, (double)value);
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

bool signal_unsubscribe(int16_t signal_index, signal_update_cb_t cb,
                        void *user_data)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count) {
        return false;
    }

    signal_t *sig = &s_signals[signal_index];

    for (uint8_t i = 0; i < sig->subscriber_count; i++) {
        if (sig->subscribers[i].cb == cb &&
            sig->subscribers[i].user_data == user_data) {
            /* Shift remaining subscribers down */
            for (uint8_t j = i; j < sig->subscriber_count - 1; j++) {
                sig->subscribers[j] = sig->subscribers[j + 1];
            }
            sig->subscriber_count--;
            memset(&sig->subscribers[sig->subscriber_count], 0,
                   sizeof(signal_subscriber_t));
            return true;
        }
    }
    return false;
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

/* Self-expiring dispatch pause. Used by the boot curtain reveal so live CAN
 * updates don't force expensive widget redraws mid-animation. Deadline-based
 * rather than a bool so a missed "resume" (animation killed, screen torn
 * down mid-sweep) can never wedge dispatch permanently. */
static uint64_t s_dispatch_pause_until_ms = 0;

void signal_dispatch_pause_ms(uint32_t ms)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_dispatch_pause_until_ms = (ms > 0) ? now + ms : 0;
}

void signal_dispatch_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if (!s_signals || !data) return;

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (now_ms < s_dispatch_pause_until_ms) return;

    for (uint16_t i = 0; i < s_signal_count; i++) {
        signal_t *sig = &s_signals[i];

        /* Skip non-CAN-decoded signals. OBD2- and internal-sourced signals are
         * registered with can_id=0 / bit_length=0 and are fed by their own
         * producers, never by bus decode. Without this, a stray frame with CAN
         * ID 0x000 (a legal, highest-priority ID) would match every one of them
         * and force it to its decode offset, marking it fresh. */
        if (sig->can_id == 0 || sig->bit_length == 0) continue;

        if (sig->can_id != can_id) continue;

        /* Test-lock gate: the web editor / Properties panel has injected a
         * manual test value and asked the firmware to hold it. Drop this
         * frame so the test value stays pinned while real bus traffic
         * arrives. Cleared via signal_set_test_lock(name, false) or on
         * layout reload. */
        if (sig->test_locked) continue;

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

        /* Update peak/min tracking */
        if (sig->tracking_active) {
            if (decoded > sig->peak_value) sig->peak_value = decoded;
            if (decoded < sig->min_value)  sig->min_value  = decoded;
            if (decoded > sig->session_peak) sig->session_peak = decoded;
            if (decoded < sig->session_min)  sig->session_min  = decoded;
        }

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
        ESP_LOGD(TAG, "signal_inject_test_value: '%s' not found", name);
        return;
    }

    signal_t *sig = &s_signals[idx];
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    bool was_stale = sig->is_stale;
    bool changed   = (value != sig->current_value);

    sig->current_value  = value;
    sig->is_stale       = false;
    sig->last_update_ms = now_ms;
    /* Re-arm the test-lock TTL: while the editor keeps injecting (slider
     * drags re-send), the lock stays alive; once injects stop, the lock
     * expires and live CAN data resumes (see signal_check_staleness). */
    sig->test_lock_ms   = now_ms;

    /* Update peak/min tracking — skip when the simulator is driving OR
     * the signal is currently test-locked by the user. The sim's
     * triangle-wave sweep would pin peaks at the extremes of its range;
     * manually-injected test values are fake by definition and shouldn't
     * corrupt the recorded peaks either. Replay is still allowed through
     * this branch: it plays back real logged data that the user probably
     * DOES want reflected in the peak/min stats. */
    if (sig->tracking_active && !signal_sim_is_active() && !sig->test_locked) {
        if (value > sig->peak_value) sig->peak_value = value;
        if (value < sig->min_value)  sig->min_value  = value;
        if (value > sig->session_peak) sig->session_peak = value;
        if (value < sig->session_min)  sig->session_min  = value;
    }

    /* Match signal_dispatch_frame: only repaint widgets when the value
     * actually changed or the signal was stale. Cuts redundant LVGL
     * invalidations when the sim (or replay) re-injects the same value. */
    if (was_stale || changed) {
        notify_subscribers(sig);
    }
}

/* ── External value push (OBD2, internal synthesis, etc.) ──────────────── */

void signal_set_external_value(const char *name, float value)
{
    if (!s_signals || !name) return;

    int16_t idx = signal_find_by_name(name);
    if (idx < 0) {
        ESP_LOGD(TAG, "signal_set_external_value: '%s' not found", name);
        return;
    }

    signal_t *sig = &s_signals[idx];

    /* Test-lock gate: matches signal_dispatch_frame — if the user has
     * pinned a manual value, external sources don't overwrite it. */
    if (sig->test_locked) return;

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    bool was_stale = sig->is_stale;
    bool changed   = (value != sig->current_value);

    /* Peak/min tracking — unlike inject, external pushes always feed peaks
     * (they're real data from OBD2 / internal sensors). Still gate on sim
     * being inactive so sim-only sweeps don't corrupt history. */
    if (sig->tracking_active && !signal_sim_is_active()) {
        if (value > sig->peak_value) sig->peak_value = value;
        if (value < sig->min_value)  sig->min_value  = value;
        if (value > sig->session_peak) sig->session_peak = value;
        if (value < sig->session_min)  sig->session_min  = value;
    }

    sig->current_value  = value;
    sig->is_stale       = false;
    sig->last_update_ms = now_ms;

    if (was_stale || changed) {
        notify_subscribers(sig);
    }
}

/* ── Timeout checking ───────────────────────────────────────────────────── */

void signal_check_timeouts(uint64_t current_time_ms)
{
    if (!s_signals) return;
    if (signal_sim_is_active()) return; /* sim keeps signals fresh */

    for (uint16_t i = 0; i < s_signal_count; i++) {
        signal_t *sig = &s_signals[i];

        /* Test-locked signals are under manual user control — hold fresh
         * so the injected value keeps rendering without the stale badge.
         * BUT only while the editor is actively re-injecting: a forgotten
         * Test Value (user never clicked ×, closed the browser, drove off)
         * used to pin the signal dead forever. Expire the lock after the
         * TTL so live CAN data resumes on its own. last_update_ms reset
         * like signal_set_test_lock(false) — the next frame or the 2 s
         * timeout decides fresh-vs-stale honestly. */
        if (sig->test_locked) {
            if (current_time_ms - sig->test_lock_ms > SIGNAL_TEST_LOCK_TTL_MS) {
                ESP_LOGI(TAG, "signal '%s': test lock expired (no inject for %u s) — resuming live data",
                         sig->name, (unsigned)(SIGNAL_TEST_LOCK_TTL_MS / 1000));
                sig->test_locked    = false;
                sig->test_lock_ms   = 0;
                sig->last_update_ms = current_time_ms;
            }
            continue;
        }

        /* Skip: already stale (already notified) or never received. */
        if (sig->is_stale || sig->last_update_ms == 0) continue;

        if (current_time_ms - sig->last_update_ms > SIGNAL_TIMEOUT_MS) {
            sig->is_stale = true;
            notify_subscribers(sig);
        }
    }
}

/* ── Test lock ──────────────────────────────────────────────────────────── */

void signal_set_test_lock(const char *name, bool locked)
{
    if (!s_signals || !name) return;
    int16_t idx = signal_find_by_name(name);
    if (idx < 0) {
        ESP_LOGD(TAG, "signal_set_test_lock: '%s' not found", name);
        return;
    }
    signal_t *sig = &s_signals[idx];
    if (sig->test_locked == locked) return;
    sig->test_locked = locked;
    if (locked) {
        /* Arm the TTL even if the caller locks without injecting first —
         * a zero timestamp would expire the lock on the next sweep tick. */
        sig->test_lock_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    }
    if (!locked) {
        sig->test_lock_ms = 0;
        /* Reset last_update_ms so the next real CAN frame (or 2s timeout)
         * decides fresh-vs-stale; don't hold the pre-lock timestamp. */
        sig->last_update_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    }
    ESP_LOGI(TAG, "signal '%s' test lock %s", name, locked ? "ON" : "OFF");
}

void signal_clear_all_test_locks(void)
{
    if (!s_signals) return;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    uint16_t cleared = 0;
    for (uint16_t i = 0; i < s_signal_count; i++) {
        if (s_signals[i].test_locked) {
            s_signals[i].test_locked = false;
            s_signals[i].last_update_ms = now_ms;
            cleared++;
        }
    }
    if (cleared) ESP_LOGI(TAG, "cleared %u test locks", cleared);
}

/* ── Peak/min value tracking ───────────────────────────────────────────── */

void signal_reset_peaks(void)
{
    if (!s_signals) return;
    for (uint16_t i = 0; i < s_signal_count; i++) {
        s_signals[i].peak_value   = -FLT_MAX;
        s_signals[i].min_value    = FLT_MAX;
        s_signals[i].session_peak = -FLT_MAX;
        s_signals[i].session_min  = FLT_MAX;
    }
    /* Peaks are session-only (reset every boot, no NVS persistence), so
     * there's nothing to flush — the RAM clear above is the whole job. */
}

void signal_reset_peak(int16_t signal_index)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count) return;
    s_signals[signal_index].peak_value   = -FLT_MAX;
    s_signals[signal_index].min_value    = FLT_MAX;
    s_signals[signal_index].session_peak = -FLT_MAX;
    s_signals[signal_index].session_min  = FLT_MAX;
    /* Session-only peaks — no NVS persistence to flush. */
}

float signal_get_peak(int16_t signal_index)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count) return -FLT_MAX;
    return s_signals[signal_index].peak_value;
}

float signal_get_min(int16_t signal_index)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count) return FLT_MAX;
    return s_signals[signal_index].min_value;
}

float signal_get_session_peak(int16_t signal_index)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count) return -FLT_MAX;
    return s_signals[signal_index].session_peak;
}

float signal_get_session_min(int16_t signal_index)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count) return FLT_MAX;
    return s_signals[signal_index].session_min;
}

void signal_reset_session_peak(int16_t signal_index)
{
    if (!s_signals || signal_index < 0 ||
        (uint16_t)signal_index >= s_signal_count) return;
    s_signals[signal_index].session_peak = -FLT_MAX;
    s_signals[signal_index].session_min  = FLT_MAX;
}

void signal_reset_all_session_peaks(void)
{
    if (!s_signals) return;
    for (uint16_t i = 0; i < s_signal_count; i++) {
        s_signals[i].session_peak = -FLT_MAX;
        s_signals[i].session_min  = FLT_MAX;
    }
}
