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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ────────────────────────────────────────────────────────────── */

/* Bumped 128 → 200. The signal array is PSRAM-backed so the extra ~14 KB
 * is free, and the headroom matters: signals MERGE across ECU/layout
 * switches (see layout_manager.c), so a user who tries several ECU
 * presets accumulates their unique signal names. At 128 the registry
 * could fill, which silently broke OBD2 gap-fill (no slot left to
 * register FUEL_LEVEL etc.). The wizard now also registers only
 * canonical-mapped ECU signals to keep accumulation small, but the
 * cap raise gives margin for layouts authored with many raw signals. */
#define MAX_SIGNALS            200
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

/* ── Value-label map ───────────────────────────────────────────────────── */

/* Maps a discrete decoded value to a display label. Lives on the signal so
 * every widget rendering that signal picks up the labels for free (gear
 * positions, drive modes, cruise state, boolean flags, lambda bands,
 * threshold codes, …). Authored in the layout JSON as:
 *
 *   "value_map": [ { "v": 0,     "label": "N"      },
 *                  { "v": 1,     "label": "1"      },
 *                  { "v": 0.85,  "label": "Rich"   },
 *                  { "v": 1.0,   "label": "Stoich" }, … ]
 *
 * Lookup matches on |value - entry.v| < SIGNAL_VALUE_MAP_EPSILON, which
 * tolerates the float drift introduced by CAN decode (raw * scale +
 * offset) while still cleanly distinguishing entries spaced >= 0.001
 * apart. Signals whose physical values aren't enum-coded (RPM, temps,
 * etc.) simply leave the map empty and fall through to numeric format. */
#define SIGNAL_VALUE_LABEL_MAX   12      /* chars, incl. NUL */
#define SIGNAL_VALUE_MAP_MAX     32      /* max entries per signal */
#define SIGNAL_VALUE_MAP_EPSILON 0.001f  /* match tolerance */

typedef struct {
    float    value;
    char     label[SIGNAL_VALUE_LABEL_MAX];
} signal_value_label_t;

/* ── Signal descriptor ─────────────────────────────────────────────────── */

/* Provenance of a signal — what kind of source produces its value.
 *
 *   CAN      — ECU broadcasts the value on the bus. Most layout-loaded
 *              signals fall here. can_id/bit_start/length are meaningful.
 *   OBD2     — We poll the value via Mode 01 / Mode 21 / Mode 22. Polled
 *              by obd2.c, NOT received as a broadcast frame. can_id is 0.
 *   INTERNAL — Synthesized on-device (CALCULATED_GEAR, FUEL_SENDER_V,
 *              DTC_COUNT, dimmer wire input, etc). No CAN traffic at all.
 *
 * Drives the on-device OBD2 picker's "in preset" logic without name
 * heuristics, lets the web Channels source picker classify Bound signals,
 * and lets the Custom Signals editor filter out ECU-preset rows by
 * provenance instead of name matching. */
typedef enum {
    SIGNAL_SOURCE_CAN      = 0,
    SIGNAL_SOURCE_OBD2     = 1,
    SIGNAL_SOURCE_INTERNAL = 2,
} signal_source_t;

typedef struct {
    char     name[32];
    uint32_t can_id;
    uint8_t  bit_start;
    uint8_t  bit_length;
    float    scale;
    float    offset;
    bool     is_signed;
    uint8_t  endian;          /* 0 = Motorola (big), 1 = Intel (little) */
    uint8_t  source;          /* signal_source_t — provenance, NOT decoder */

    char     unit[8];           /* Display unit (e.g., "kPa", "°C") */

    /* Optional value→label map. NULL/empty = numeric display. */
    signal_value_label_t *value_map;
    uint8_t               value_map_count;

    /* Runtime state */
    float    current_value;
    float    peak_value;        /* Highest value seen (persisted across reboot) */
    float    min_value;         /* Lowest value seen (persisted across reboot) */
    float    session_peak;      /* Highest seen this boot only — never persisted */
    float    session_min;       /* Lowest seen this boot only — never persisted */
    bool     tracking_active;   /* Peak/min tracking enabled */
    bool     is_stale;
    bool     test_locked;       /* true = manual test value holds this signal.
                                   signal_dispatch_frame() drops matching CAN
                                   frames so the injected value sticks while
                                   the car is live on the bus. Cleared via
                                   signal_set_test_lock(name, false) or by
                                   layout reload (registry reset). */
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
 * Register a signal. If a signal with the same name already exists (signals
 * MERGE across layout loads — the registry is not reset on the happy path),
 * its decode params are UPDATED in place and the existing index is returned
 * (latest-layout-wins); subscribers and peak/min stats on that slot are
 * preserved.
 *
 * Defaults source to SIGNAL_SOURCE_CAN. Callers that produce values
 * via OBD2 polling or on-device synthesis should call the _with_source
 * variant below instead.
 *
 * @return Signal index (>= 0) on success, -1 on failure.
 */
int16_t signal_register(const char *name, uint32_t can_id,
                        uint8_t start, uint8_t len,
                        float scale, float offset,
                        bool is_signed, uint8_t endian,
                        const char *unit);

/**
 * Register a signal with explicit provenance. Same as signal_register()
 * but tags the registry entry's source field. Re-registering an existing
 * name updates that slot's decode params in place (latest-layout-wins,
 * subscribers/stats preserved) and returns the existing index.
 *
 * @return Signal index (>= 0) on success, -1 on failure.
 */
int16_t signal_register_with_source(const char *name, uint32_t can_id,
                                    uint8_t start, uint8_t len,
                                    float scale, float offset,
                                    bool is_signed, uint8_t endian,
                                    const char *unit,
                                    signal_source_t source);

/**
 * Look up a signal by name.
 *
 * @return Signal index (>= 0) if found, -1 otherwise.
 */
int16_t signal_find_by_name(const char *name);

/**
 * Attach a value→label map to a signal. Replaces any prior map. Pass
 * @p count == 0 (or @p entries == NULL) to clear. Entries beyond
 * SIGNAL_VALUE_MAP_MAX are dropped. The signal owns its copy after this
 * call — caller is free to discard @p entries. Called once at layout
 * load by the signals[] parser; safe to call again on re-load.
 *
 * @return true on success, false if signal_index is out of range.
 */
bool signal_set_value_map(int16_t signal_index,
                          const signal_value_label_t *entries,
                          uint8_t count);

/**
 * Look up a label for the given decoded value on this signal. Returns
 * NULL if no map is attached or no entry matches (caller should fall
 * back to numeric formatting). Lookup is exact integer-equality on
 * roundf(value); useful for gear positions, mode codes, boolean flags.
 */
const char *signal_value_lookup_label(int16_t signal_index, float value);

/**
 * Format @p value into @p buf using either the signal's value-label map
 * (if a matching entry exists) or numeric formatting `%.*f` with the
 * supplied @p decimals. Pass @p signal_index < 0 to skip the map check.
 *
 *   no map / no match    -> "123.4"  (decimals respected)
 *   map matches v=0      -> "N"
 *
 * Widget signal-callbacks should use this instead of inline snprintf so
 * value-label maps work uniformly across panel / bar / text.
 */
void signal_format_value(int16_t signal_index, float value,
                         uint8_t decimals, char *buf, size_t cap);

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

/**
 * Push a value into a signal from a non-CAN-bit-decode source (OBD2, internal
 * synthesis, etc.). Updates current_value, peaks, last_update_ms, and notifies
 * subscribers — same effect as a successful CAN dispatch for the signal.
 *
 * Distinct from signal_inject_test_value() so peak tracking and sim/test gating
 * stay separate. Peak/min tracking runs through this path (unlike inject which
 * is gated by sim_is_active + test_locked).
 *
 * No-op if the signal name is not found in the registry. MUST be called on
 * the LVGL task.
 */
void signal_set_external_value(const char *name, float value);

/**
 * Set or clear the "test lock" on a signal by name. While locked,
 * signal_dispatch_frame() drops any matching CAN frame so an injected
 * test value won't be overwritten by live bus traffic. The signal also
 * stays "fresh" (no 2s timeout) for the duration of the lock.
 *
 * Called from the web /api/signal/inject (lock=true) and /api/signal/clear
 * (lock=false) handlers.
 */
void signal_set_test_lock(const char *name, bool locked);

/** Clear test lock on every signal. Layout reload also clears locks. */
void signal_clear_all_test_locks(void);

/* ── Peak/min tracking ────────────────────────────────────────────────── */

/** Reset peak and min values for all signals. */
void signal_reset_peaks(void);

/** Reset peak and min values for a single signal. */
void signal_reset_peak(int16_t signal_index);

/** Get the peak (max) value recorded for a signal. Returns -FLT_MAX if none. */
float signal_get_peak(int16_t signal_index);

/** Get the min value recorded for a signal. Returns FLT_MAX if none. */
float signal_get_min(int16_t signal_index);

/** Session peak/min — used by panel widgets so a "Peak Hold" reading shows
 * what's happened during this drive. NOTE: peaks are session-only — all
 * peak/min state (including signal_get_peak/min below) resets every boot and
 * is NOT persisted to NVS. The "all-time" vs "session" distinction is now
 * purely about reset granularity within a single boot. */
float signal_get_session_peak(int16_t signal_index);
float signal_get_session_min(int16_t signal_index);
void  signal_reset_session_peak(int16_t signal_index);
void  signal_reset_all_session_peaks(void);

#ifdef __cplusplus
}
#endif
