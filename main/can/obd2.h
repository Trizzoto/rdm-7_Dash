/**
 * obd2.h — On-board diagnostics (SAE J1979) polling + decode.
 *
 * Implements Mode 01 (live data) request/response over ISO 15765-4 CAN.
 * Polls an enabled list of standard PIDs at a fast/slow tier, decodes the
 * response, and pushes the value into the signal registry by name — so
 * widgets bind to OBD2 signals like any other.
 *
 * Runs entirely on the LVGL task (LVGL timer for poll cadence, RX hook
 * from can_process_queued_frames). No separate FreeRTOS task.
 *
 * Two modes of use:
 *   - Primary preset ("OBD2 Standard"): 30 starter PIDs enabled by default.
 *   - Supplemental on top of a native preset: small list of gap-filler
 *     PIDs (fuel level, ambient, odometer) polled at slow tier.
 *
 * Both modes share the same enabled_pids[] list — the module doesn't care
 * which case it's in.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard cap on simultaneously-enabled PIDs. Keeps round-robin cycle short
 * and bounds NVS footprint. 48 is well above the 30 starter set + room
 * for ~18 user additions from discovery. */
#define OBD2_MAX_ENABLED 48

/* Standard 11-bit OBD2 IDs (ISO 15765-4). */
#define OBD2_REQUEST_ID_BROADCAST 0x7DFu
#define OBD2_RESPONSE_ID_FIRST    0x7E8u
#define OBD2_RESPONSE_ID_LAST     0x7EFu

typedef enum {
    OBD2_TIER_FAST = 0,   /* RPM, MAP, speed — polled every cycle (~10 Hz). */
    OBD2_TIER_SLOW = 1,   /* temps, voltages, slow vars — every Nth cycle. */
} obd2_tier_t;

/* Definition of a single decodable PID.
 *
 * All J1979 standard PID decodes happen to be linear, so we encode them as
 * (scale, offset, byte_count) and let one shared formula do the work:
 *     value = raw * scale + offset
 * where raw = (bytes=1) A    or    (bytes=2) A*256 + B.
 *
 * `signal_name` is the registry key widgets bind to (matches the ECU
 * preset normalized names where applicable: RPM, COOLANT_TEMP, etc.).
 *
 * `service` selects the OBD2 service byte for the request frame:
 *   0x01 = Mode 01 (standard J1979 live data)
 *   0x22 = Mode 22 (manufacturer-specific PIDs, future)
 *   0    = treated as 0x01 (backwards-compat default for existing entries)
 *
 * `category` is a free-form UI grouping hint (e.g. "Engine", "Fuel",
 * "Transmission"). Picker UI can group by it once Mode 22 packs land
 * and the flat list becomes too long. NULL = "General" / ungrouped. */
typedef struct {
    uint8_t      pid;
    const char  *signal_name;
    const char  *human_name;     /* UI label, e.g. "Engine RPM" */
    const char  *unit;
    uint8_t      bytes;          /* 1 or 2 */
    float        scale;
    float        offset;
    obd2_tier_t  tier;
    bool         default_enabled; /* in the 30 starter set */
    bool         suggested_filler; /* shown in "Suggested" group for supplemental mode */
    uint8_t      service;         /* 0 or 0x01 = Mode 01; 0x22 = Mode 22 */
    const char  *category;        /* UI group, or NULL */
} obd2_pid_def_t;

/* Helper: resolve the effective service byte (translates 0 → 0x01). */
static inline uint8_t obd2_def_service(const obd2_pid_def_t *def) {
    return (def && def->service) ? def->service : 0x01;
}

/* Static PID table — see obd2_pids.c */
extern const obd2_pid_def_t OBD2_PIDS[];
extern const int OBD2_PIDS_COUNT;

/* Look up a definition by PID number. Returns NULL if no decoder available. */
const obd2_pid_def_t *obd2_pid_find(uint8_t pid);

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** Initialise internal state (idempotent). No polling starts yet. */
void obd2_init(void);

/** Start polling the given PID list. Registers each PID's signal in the
 *  signal registry if it's not already present. Stops any prior polling.
 *  Called from the ECU preset apply path and on layout load. */
void obd2_start(const uint8_t *enabled_pids, uint8_t count);

/** Stop polling and tear down the timer. Idempotent. */
void obd2_stop(void);

/** True if polling is currently active. */
bool obd2_is_running(void);

/* ── Enabled PID list ───────────────────────────────────────────────────── */

/** Copy the currently-enabled PID list into @p out. Returns count written. */
uint8_t obd2_get_enabled(uint8_t *out, uint8_t max);

/** Replace the enabled list and restart polling. Caller is responsible
 *  for persisting the new list to NVS / layout JSON. */
void obd2_set_enabled(const uint8_t *pids, uint8_t count);

/* ── CAN dispatch ───────────────────────────────────────────────────────── */

/** RX hook called by can_process_queued_frames for any frame in
 *  [0x7E8..0x7EF]. LVGL task only. */
void obd2_rx_handler(uint32_t can_id, const uint8_t *data, uint8_t dlc);

/* ── Discovery scan ─────────────────────────────────────────────────────── */

/* 7 bitmask blocks * 32 PIDs/block = 224 possible standard PIDs; 128 is a
 * comfortable cap that fits any real vehicle. */
#define OBD2_SCAN_MAX_PIDS 128

/** Result of a discovery scan. Lists every PID the vehicle's ECU(s) report
 *  as supported via the standard 0x00/0x20/0x40/.../0xE0 bitmasks. */
typedef struct {
    uint8_t pids[OBD2_SCAN_MAX_PIDS];
    uint8_t count;
    bool    completed;     /* false → scan timed out with partial / no data */
} obd2_scan_result_t;

typedef void (*obd2_scan_cb_t)(const obd2_scan_result_t *result, void *user);

/** Begin a discovery scan. Sends Mode 01 PID 0x00 (and chains 0x20/0x40/...
 *  if the car says they're supported). Completes within ~2 seconds.
 *  Result delivered via callback on the LVGL task. */
void obd2_discovery_start(obd2_scan_cb_t cb, void *user);

/** True while a scan is in progress. */
bool obd2_discovery_in_progress(void);

#ifdef __cplusplus
}
#endif
