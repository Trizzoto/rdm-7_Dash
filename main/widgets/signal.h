/**
 * signal.h — Signal registry for CAN-decoded values.
 *
 * Each signal describes a CAN bit-field that is decoded once per frame and
 * pushed to all registered subscribers via callbacks.
 *
 * Threading: signal_dispatch_frame() and signal_check_timeouts() MUST be
 * called with the LVGL mutex held (i.e. from can_process_queued_frames()).
 * Subscriber callbacks therefore run on the LVGL task and may call LVGL
 * APIs directly.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ────────────────────────────────────────────────────────────── */

#define MAX_SIGNALS            128
#define MAX_SIGNAL_SUBSCRIBERS   16
#define SIGNAL_TIMEOUT_MS     2000

/* ── Callback typedef ──────────────────────────────────────────────────── */

/**
 * Called when a signal's decoded value changes or when it becomes stale.
 *
 * @param value     Decoded physical value (raw * scale + offset).
 * @param is_stale  true if the signal has timed out (no CAN frame received
 *                  within SIGNAL_TIMEOUT_MS).
 * @param user_data Opaque pointer passed at subscription time.
 */
typedef void (*signal_update_cb_t)(float value, bool is_stale, void *user_data);

/* ── Subscriber slot ───────────────────────────────────────────────────── */

typedef struct {
    signal_update_cb_t cb;
    void              *user_data;
} signal_subscriber_t;

/* ── Signal descriptor ─────────────────────────────────────────────────── */

typedef struct {
    char     name[32];
    uint32_t can_id;
    uint8_t  bit_start;
    uint8_t  bit_length;
    float    scale;
    float    offset;
    bool     is_signed;
    uint8_t  endian;          /* 0 = Motorola (big), 1 = Intel (little) */

    char     unit[8];           /* Display unit (e.g., "kPa", "°C") */

    /* Runtime state */
    float    current_value;
    float    peak_value;        /* Highest value seen */
    float    min_value;         /* Lowest value seen */
    bool     tracking_active;   /* Peak/min tracking enabled */
    bool     is_stale;
    uint64_t last_update_ms;

    signal_subscriber_t subscribers[MAX_SIGNAL_SUBSCRIBERS];
    uint8_t             subscriber_count;
} signal_t;

/* ── Registry lifecycle ────────────────────────────────────────────────── */

/** Allocate the PSRAM-backed signal array.  Idempotent. */
void signal_registry_init(void);

/** Zero all signals and reset the count.  Called at the top of layout load. */
void signal_registry_reset(void);

/* ── Registration ──────────────────────────────────────────────────────── */

/**
 * Register a new signal.  Duplicate names are rejected.
 *
 * @return Signal index (>= 0) on success, -1 on failure.
 */
int16_t signal_register(const char *name, uint32_t can_id,
                        uint8_t start, uint8_t len,
                        float scale, float offset,
                        bool is_signed, uint8_t endian,
                        const char *unit);

/**
 * Look up a signal by name.
 *
 * @return Signal index (>= 0) if found, -1 otherwise.
 */
int16_t signal_find_by_name(const char *name);

/** Return a pointer to the signal at the given index, or NULL. */
signal_t *signal_get_by_index(uint16_t index);

/** Return the current number of registered signals. */
uint16_t signal_get_count(void);

/* ── Subscription ──────────────────────────────────────────────────────── */

/**
 * Subscribe to value changes on a signal.
 *
 * @param signal_index  Index returned by signal_register / signal_find_by_name.
 * @param cb            Callback invoked on value change or stale transition.
 * @param user_data     Opaque pointer forwarded to the callback.
 * @return true on success, false if index invalid or subscriber list full.
 */
bool signal_subscribe(int16_t signal_index, signal_update_cb_t cb,
                      void *user_data);

/**
 * Remove a previously registered subscription.
 *
 * Matches on both @p cb and @p user_data (same pair passed to
 * signal_subscribe).  Must be called before the user_data pointer is freed
 * to prevent use-after-free in signal callbacks.
 *
 * @return true if a matching subscription was found and removed.
 */
bool signal_unsubscribe(int16_t signal_index, signal_update_cb_t cb,
                        void *user_data);

/* ── Dispatch (call from LVGL task) ────────────────────────────────────── */

/**
 * Decode all signals matching @p can_id from the raw CAN frame and notify
 * subscribers whose value changed.
 */
void signal_dispatch_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc);

/**
 * Mark signals as stale if no CAN frame has been received within
 * SIGNAL_TIMEOUT_MS and notify subscribers.
 */
void signal_check_timeouts(uint64_t current_time_ms);

/**
 * Inject a test value into a signal by name.  Sets current_value,
 * clears stale, updates last_update_ms, and notifies subscribers.
 *
 * MUST be called on the LVGL task (same threading rules as
 * signal_dispatch_frame).
 */
void signal_inject_test_value(const char *name, float value);

/* ── Peak/min tracking ────────────────────────────────────────────────── */

/** Reset peak and min values for all signals. */
void signal_reset_peaks(void);

/** Reset peak and min values for a single signal. */
void signal_reset_peak(int16_t signal_index);

/** Get the peak (max) value recorded for a signal. Returns -FLT_MAX if none. */
float signal_get_peak(int16_t signal_index);

/** Get the min value recorded for a signal. Returns FLT_MAX if none. */
float signal_get_min(int16_t signal_index);

#ifdef __cplusplus
}
#endif
