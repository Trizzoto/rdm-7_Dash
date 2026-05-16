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

/* ── Encoded (service, pid) tuples ──────────────────────────────────────
 *
 * Polled-PID storage encodes service + PID into a single uint32 so the
 * polling backend can disambiguate same-byte PIDs that mean different
 * things in different services (e.g. Mode 01 PID 0x21 = DTC distance
 * vs Mode 21 PID 0x21 = Toyota ATF temp), AND handle Mode 22 16-bit PIDs
 * (Ford, GM, VW, newer Toyota).
 *
 * Three encoding ranges (decoded by value):
 *   value <= 0xFF      → legacy Mode 01 bare byte (service=1, pid=value)
 *   0x100..0xFFFF      → 8-bit PID: (service<<8) | pid  — Mode 01/21
 *   value > 0xFFFF     → 16-bit PID: (service<<16) | pid — Mode 22
 *
 * Encoders pick the smallest valid form so existing 8-bit PIDs serialise
 * to the same shape as before (no JSON bloat for the common case). */

static inline uint32_t obd2_encode_pid(uint8_t service, uint16_t pid) {
    if (service == 0) service = 0x01;
    if (pid <= 0xFF) {
        return (uint32_t)(((uint32_t)service << 8) | pid);
    }
    return (uint32_t)(((uint32_t)service << 16) | pid);
}

static inline uint8_t obd2_decode_service(uint32_t encoded) {
    if (encoded <= 0xFF) return 0x01;
    if (encoded <= 0xFFFF) {
        uint8_t s = (uint8_t)(encoded >> 8);
        return s ? s : 0x01;
    }
    uint8_t s = (uint8_t)(encoded >> 16);
    return s ? s : 0x01;
}

static inline uint16_t obd2_decode_pid(uint32_t encoded) {
    if (encoded <= 0xFF)   return (uint16_t)encoded;
    if (encoded <= 0xFFFF) return (uint16_t)(encoded & 0xFF);
    return (uint16_t)(encoded & 0xFFFF);
}

typedef enum {
    OBD2_TIER_FAST = 0,   /* RPM, MAP, speed — polled every cycle (~10 Hz). */
    OBD2_TIER_SLOW = 1,   /* temps, voltages, slow vars — every Nth cycle. */
} obd2_tier_t;

/* A single sub-field within a packed PID response. Used for Mode 21/22
 * PIDs that return multiple values in one response — common on Toyota
 * (Mode 21 PID 0x80 packs RPM + speed + temps in one ISO-TP frame).
 * byte_offset is relative to the start of the data payload (after the
 * response service echo byte and the PID echo byte). */
typedef struct {
    const char *signal_name;
    const char *unit;
    uint8_t     byte_offset;
    uint8_t     bytes;        /* 1, 2, 4 */
    bool        is_signed;
    float       scale;
    float       offset;
} obd2_subfield_t;

/* Definition of a single decodable PID.
 *
 * Two decode modes:
 *  - Single-value (legacy / most Mode 01 PIDs): use `bytes/scale/offset`
 *    with the implicit raw = (bytes=1) A or (bytes=2) A*256 + B.
 *  - Packed multi-value: set `sub_fields` + `sub_field_count`. One
 *    request → N signal_set_external_value calls, one per sub-field.
 *
 * `signal_name` is the registry key widgets bind to (matches the ECU
 * preset normalized names where applicable: RPM, COOLANT_TEMP, etc.).
 * For packed PIDs, `signal_name` is unused (each sub-field carries its
 * own name) but `human_name` still labels the row in the picker.
 *
 * `service` selects the OBD2 service byte for the request frame:
 *   0x01 = Mode 01 (standard J1979 live data, single-frame)
 *   0x21 = Mode 21 (KWP-derived, used by Toyota/Honda — often multi-frame)
 *   0x22 = Mode 22 (UDS, used by Ford/GM/VW — 16-bit PIDs, future)
 *   0    = treated as 0x01 (backwards-compat default)
 *
 * `request_id` overrides the default broadcast 0x7DF when set non-zero —
 * use 0x7E0 to address the engine ECU specifically (avoids NRCs from
 * other ECUs that don't handle this PID).
 *
 * `category` is a free-form UI grouping hint. NULL = ungrouped. */
typedef struct {
    uint16_t     pid;             /* 8-bit for Mode 01/21; 16-bit for Mode 22 */
    const char  *signal_name;
    const char  *human_name;     /* UI label, e.g. "Engine RPM" */
    const char  *unit;
    uint8_t      bytes;          /* 1 or 2 (single-value mode) */
    float        scale;
    float        offset;
    obd2_tier_t  tier;
    bool         default_enabled; /* in the 30 starter set */
    bool         suggested_filler; /* shown in "Suggested" group for supplemental mode */
    uint8_t      service;         /* 0/0x01 = Mode 01, 0x21 = Mode 21, 0x22 = Mode 22 */
    const char  *category;        /* UI group, or NULL */
    const obd2_subfield_t *sub_fields; /* packed-decode sub-fields; NULL → single-value */
    uint8_t      sub_field_count;
    uint32_t     request_id;      /* 0 → use OBD2_REQUEST_ID_BROADCAST */
} obd2_pid_def_t;

/* Helper: resolve the effective service byte (translates 0 → 0x01). */
static inline uint8_t obd2_def_service(const obd2_pid_def_t *def) {
    return (def && def->service) ? def->service : 0x01;
}

/* Static PID table — see obd2_pids.c */
extern const obd2_pid_def_t OBD2_PIDS[];
extern const int OBD2_PIDS_COUNT;

/* Look up a definition by PID byte alone — returns the FIRST match
 * regardless of service. Kept for compat with callers that don't
 * distinguish services (e.g. live-indicator polling). Use
 * obd2_pid_find_svc() for unambiguous lookups across modes.
 *
 * Both lookups walk built-in OBD2_PIDS first, then runtime custom
 * PIDs (registered via obd2_custom_add). */
const obd2_pid_def_t *obd2_pid_find(uint16_t pid);

/* Service-aware lookup: returns the def whose (service, pid) matches.
 * service=0 is treated as 0x01 (Mode 01). Falls back to first-match-by-byte
 * if no exact (service, pid) pair exists. */
const obd2_pid_def_t *obd2_pid_find_svc(uint8_t service, uint16_t pid);

/* ── Unified PID iteration ─────────────────────────────────────────────
 *
 * Pickers and other UIs need to walk every available PID (built-in plus
 * runtime custom). These helpers give that as a flat indexed list so
 * callers don't have to walk two arrays separately. Built-in entries
 * come first, custom entries after. */
uint8_t obd2_pid_total_count(void);
const obd2_pid_def_t *obd2_pid_at(uint8_t index);

/* ── Custom (user-defined) PIDs ───────────────────────────────────────
 *
 * Custom PIDs let users add forum-found / community-documented OBD2
 * signals at runtime without a firmware rebuild. They're stored in
 * layout JSON (`custom_pids` array) and registered here at layout load.
 * Internally each custom PID becomes a single-sub-field "packed" def
 * so the existing packed-decode path handles it uniformly. */

#define OBD2_MAX_CUSTOM_PIDS 32

/* Clear all currently-registered custom PIDs. Called at layout load
 * before parsing the new layout's custom_pids array. */
void obd2_custom_reset(void);

/* Register one custom PID. Strings are copied into internal storage so
 * the caller need not keep them alive after this call. Returns false
 * if the registry is full or required args are missing.
 *
 * @param service       OBD2 service byte (0 = 0x01 Mode 01).
 * @param pid           PID byte.
 * @param signal_name   Required. The registry key widgets bind to.
 * @param human_name    Optional; defaults to signal_name.
 * @param unit          Optional; defaults to "".
 * @param data_offset   Byte offset into the response payload (0 = start
 *                      right after the service+PID echoes).
 * @param data_bytes    1, 2, or 4.
 * @param scale         Linear decode: value = raw * scale + offset.
 * @param offset
 * @param is_signed     true → sign-extend `data_bytes` raw.
 * @param tier          OBD2_TIER_FAST or _SLOW.
 * @param category      Optional brand/grouping for the picker (e.g.
 *                      "Toyota Custom"). NULL = falls into "OBD2 /
 *                      Standard" or "(category) / Mode XX" by service.
 * @param request_id    Override request CAN ID (0 = broadcast 0x7DF).
 */
bool obd2_custom_add(uint8_t service, uint16_t pid,
                     const char *signal_name,
                     const char *human_name,
                     const char *unit,
                     uint8_t data_offset, uint8_t data_bytes,
                     float scale, float offset, bool is_signed,
                     obd2_tier_t tier,
                     const char *category,
                     uint32_t request_id);

/* Number of currently-registered custom PIDs. */
uint8_t obd2_custom_count(void);

/* ── One-shot test: send + capture single response ─────────────────────
 *
 * Powers the "Test" button in the custom-PID editor. Fires one
 * request matching (service, pid), waits up to 500 ms for a response,
 * decodes per the supplied parameters, and reports raw bytes + decoded
 * value via the callback. Coexists with normal polling — just observes
 * RX from any thread until the matching PID echo arrives or the
 * timeout fires.
 *
 * Only one test is in flight at a time; calling while another test is
 * pending invokes the callback with ok=false immediately. */

typedef void (*obd2_test_cb_t)(bool ok,
                                const uint8_t *raw_payload, uint8_t raw_len,
                                float decoded,
                                uint32_t elapsed_ms,
                                void *user);

void obd2_test_pid(uint8_t service, uint8_t pid, uint32_t request_id,
                   uint8_t data_offset, uint8_t data_bytes,
                   float scale, float offset, bool is_signed,
                   obd2_test_cb_t cb, void *user);

/* Indexed access into the custom-PID registry (0..custom_count-1). */
const obd2_pid_def_t *obd2_custom_at(uint8_t index);

/* Service-aware search restricted to the custom-PID registry.
 * Returns NULL if no match. Built-in OBD2_PIDS are NOT searched. */
const obd2_pid_def_t *obd2_custom_find_svc(uint8_t service, uint16_t pid);

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** Initialise internal state (idempotent). No polling starts yet. */
void obd2_init(void);

/** Start polling the given encoded (service, pid) list. Each uint32
 *  element is obd2_encode_pid(service, pid). Layout-encoded values
 *  <= 0xFF are treated as Mode 01 for back-compat. Registers each
 *  PID's signal in the registry if not already present. Stops any
 *  prior polling. Called from the ECU preset apply path and on layout
 *  load. */
void obd2_start(const uint32_t *enabled_pids, uint8_t count);

/** Stop polling and tear down the timer. Idempotent. */
void obd2_stop(void);

/** True if polling is currently active. */
bool obd2_is_running(void);

/* ── Enabled PID list ───────────────────────────────────────────────────── */

/** Copy the currently-enabled (encoded) PID list into @p out. Returns
 *  count written. Each entry is obd2_encode_pid(service, pid). */
uint8_t obd2_get_enabled(uint32_t *out, uint8_t max);

/** Replace the enabled list and restart polling. Caller is responsible
 *  for persisting the new list to NVS / layout JSON. */
void obd2_set_enabled(const uint32_t *pids, uint8_t count);

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
