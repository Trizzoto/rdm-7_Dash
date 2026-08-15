/**
 * ecu_presets.h - ECU auto-configuration presets.
 *
 * An ECU preset is a pre-canned mapping from normalized signal names
 * (RPM, MAP, TPS, COOLANT_TEMP, ...) to the CAN decode parameters for a
 * specific ECU's default broadcast stream. When the user picks an ECU in
 * the first-run wizard or Device Settings, the active layout's signals[]
 * array is replaced with the preset's mapping. Widget signal_name bindings
 * are preserved, so widgets automatically show live data.
 *
 * All preset scales/offsets output metric units (kPa, degC, km/h, lambda).
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Normalized signal slot ids. Keep in lockstep with ECU_SIGNAL_NAMES[]
 * in ecu_presets.c. Widget signal_name strings must match these keys. */
typedef enum {
    ECU_SIG_RPM = 0,
    ECU_SIG_MAP,
    ECU_SIG_THROTTLE,
    ECU_SIG_COOLANT_TEMP,
    ECU_SIG_INTAKE_AIR_TEMP,
    ECU_SIG_LAMBDA,
    ECU_SIG_OIL_TEMP,
    ECU_SIG_OIL_PRESSURE,
    ECU_SIG_FUEL_PRESSURE,
    ECU_SIG_IGNITION,
    ECU_SIG_VEHICLE_SPEED,
    ECU_SIG_GEAR,
    ECU_SIG_BATTERY_VOLTAGE,
    ECU_SIG_FUEL_TRIM,
    ECU_SIG_EGT,
    /* Chassis / driver-aid signals — most factory presets won't broadcast
     * these (left SIG_UNSUPPORTED) but Ford FG does. Added as optional
     * standard slots so future OEM presets (Holden/HSV, Subaru WRX, etc.)
     * can wire them up without bespoke per-preset machinery. */
    ECU_SIG_BOOST,
    ECU_SIG_FUEL_LEVEL,
    ECU_SIG_PARK_BRAKE,
    ECU_SIG_YAW_RATE,
    ECU_SIG_LATERAL_G,
    ECU_SIG__COUNT,
} ecu_signal_slot_t;

/* Per-signal decode row. can_id==0 marks the slot as unsupported by the
 * ECU's default broadcast (e.g. Honda ECUs don't broadcast oil temp). The
 * apply function writes unsupported slots as unbound so the user can add
 * a custom signal later without collision.
 *
 * value_map_csv: optional value→label table for enum-coded signals
 *   (gear position, drive mode, cruise state, …). Format is
 *   "v1=label1,v2=label2,…" — e.g. "0=N,1=1,2=2,3=3,4=4,5=5,6=6,7=P".
 *   Labels are trimmed and capped at SIGNAL_VALUE_LABEL_MAX-1 chars.
 *   NULL or empty = no map; widgets fall through to numeric formatting.
 *   Applied at ecu_preset_apply_to_layout() time into the signal's
 *   "value_map" JSON array, so every widget bound to the slot picks
 *   up the labels automatically. */
typedef struct {
    uint32_t can_id;       /* 0 = unsupported */
    uint8_t  bit_start;
    uint8_t  bit_length;
    float    scale;
    float    offset;
    bool     is_signed;
    uint8_t  endian;       /* 0 = Motorola (BE), 1 = Intel (LE) */
    const char *unit;      /* metric unit string, or "" */
    uint8_t  decimals;     /* display decimal places for panel/bar/text widgets */
    const char *value_map_csv;  /* optional "v=label,v=label,…" enum map */
    /* Multiplexed frames — several payloads share one can_id and a frame
     * index inside the message says which is which (Link Generic Dash: byte 0
     * on 0x3E8). mux_bit_length 0 = not multiplexed, which is what every other
     * preset leaves these at by omitting them. Kept in lockstep with the
     * preconfig rows in preset_picker_data.c. */
    uint8_t  mux_bit_start;
    uint8_t  mux_bit_length;
    uint16_t mux_value;
} ecu_signal_row_t;

typedef struct {
    const char *make;      /* e.g. "ECU Master" */
    const char *version;   /* e.g. "Black/Classic" */
    const char *display;   /* picker label, e.g. "ECU Master Black / Classic" */
    ecu_signal_row_t rows[ECU_SIG__COUNT];
} ecu_preset_t;

/* NULL-name sentinel terminated. */
extern const ecu_preset_t ECU_PRESETS[];
extern const int ECU_PRESETS_COUNT;

/* Return the normalized signal name (e.g. "RPM") for a slot. */
const char *ecu_signal_slot_name(ecu_signal_slot_t slot);

/**
 * Map a normalized ECU signal name (one of ECU_SIGNAL_NAMES) to its
 * canonical channel id (e.g. "RPM" → "rpm", "MAP" → "manifold_pressure").
 * Returns NULL when the name doesn't match any normalized ECU slot OR
 * the slot has no canonical mapping yet. Used by the v14 channel
 * registry's legacy-widget migrator to bind widgets whose signal_name
 * is one of the dash's standard ECU vocabulary.
 *
 * Cheap O(N) scan, N=20. Called once per widget at layout-load time. */
const char *ecu_signal_name_to_canonical(const char *signal_name);

/**
 * Reverse of ECU_SIGNAL_CANONICAL[]: canonical channel id ("rpm",
 * "coolant_temp", ...) → ECU signal slot. Returns ECU_SIG__COUNT if
 * the channel has no normalized slot (e.g. EGT cylinder N, custom
 * channels). Cheap O(N), N=20. */
ecu_signal_slot_t ecu_slot_from_canonical_id(const char *canonical_id);

/**
 * Write a single preset's slot row into a layout's signals[] array.
 * Targets the canonical signal name (ECU_SIGNAL_NAMES[slot]) — if an
 * entry with that name exists in signals[], its decode params are
 * overwritten; otherwise a new entry is appended.
 *
 * Used by the source picker: when the user picks "Haltech RPM" for
 * the rpm channel, this rewrites layout.signals[].rpm with Haltech's
 * CAN id / bit start / scale / offset / etc. The channel keeps its
 * binding to signal name "rpm" — only the decode changes.
 *
 * Does NOT touch the layout's `ecu` / `ecu_version` fields — those
 * track the *bulk* preset chosen via the layout-level picker. A
 * per-channel source override leaves them alone.
 *
 * Triggers layout reload (dashboard_init) on success so the new
 * decode params take effect immediately. */
esp_err_t ecu_preset_apply_slot_to_layout(const char *layout_name,
                                          const ecu_preset_t *preset,
                                          ecu_signal_slot_t slot);

/**
 * Generic helper: write a single signal definition into the named
 * layout's signals[] array. Creates the entry if it doesn't exist;
 * otherwise wipes the existing decode params before re-adding so
 * stale fields (decimals, value_map, etc.) don't leak through. Persists
 * to disk.
 *
 * Used by ecu_preset_apply_slot_to_layout (ECU bulk slot apply) AND by
 * the web channels source picker's preconfig binding path. Single
 * apply path means both callers get the same JSON shape, the same
 * cJSON allocation lifetime handling, and the same disk persistence
 * semantics.
 *
 * @param layout_name  Layout to rewrite (typically "default").
 * @param signal_name  Registry name to bind under (e.g. "RPM").
 * @param can_id       CAN ID (0 for OBD2/internal signals).
 * @param bit_start    Start bit (0-63).
 * @param bit_length   Length in bits (1-64).
 * @param scale        Linear scale factor.
 * @param offset       Linear offset.
 * @param is_signed    Signed two's-complement decode.
 * @param endian       0 = Motorola/big, 1 = Intel/little.
 * @param unit         Display unit, or NULL to omit.
 * @param decimals     Display decimals, or -1 to omit.
 * @return ESP_OK on success.
 */
esp_err_t ecu_preset_write_signal_to_layout(const char *layout_name,
                                            const char *signal_name,
                                            uint32_t can_id,
                                            uint8_t bit_start,
                                            uint8_t bit_length,
                                            float scale, float offset,
                                            bool is_signed, uint8_t endian,
                                            const char *unit,
                                            int decimals);

/* ── Batched layout signal writer ─────────────────────────────────────
 *
 * Calling ecu_preset_write_signal_to_layout() in a loop does a full
 * read-parse-modify-serialize-write of the layout file PER signal — for a
 * 68-signal preset that's 68 full-file LittleFS writes, which on-device
 * stalls the LVGL task for seconds and overflows the CAN RX queue.
 *
 * This writer opens the layout ONCE, accumulates any number of signal
 * upserts in memory, and writes ONCE on commit. Lifetime:
 *   w = ecu_layout_writer_open("Time_Attack");   // read + parse once
 *   for (...) ecu_layout_writer_upsert(w, ...);   // in-memory, no disk
 *   ecu_layout_writer_commit(w);                  // one save, frees w
 * On any error path use ecu_layout_writer_abort(w) to free without saving.
 * open() returns NULL on read/parse/alloc failure — callers should treat a
 * NULL writer as "skip layout persistence" (channel-owned decode in
 * channels.json remains the authoritative copy under ADR 0005). */
typedef struct ecu_layout_writer ecu_layout_writer_t;

ecu_layout_writer_t *ecu_layout_writer_open(const char *layout_name);

void ecu_layout_writer_upsert(ecu_layout_writer_t *w,
                              const char *signal_name,
                              uint32_t can_id,
                              uint8_t bit_start, uint8_t bit_length,
                              float scale, float offset,
                              bool is_signed, uint8_t endian,
                              const char *unit, int decimals);

/* Save the accumulated layout once and free the writer. */
esp_err_t ecu_layout_writer_commit(ecu_layout_writer_t *w);

/* Free the writer WITHOUT saving (error/cancel path). */
void ecu_layout_writer_abort(ecu_layout_writer_t *w);

/* Find a preset by make+version strings. Returns NULL if not found. */
const ecu_preset_t *ecu_preset_find(const char *make, const char *version);

/**
 * Rewrite the signals[] array of the named layout to match the preset.
 * Also writes the make + version into the layout's "ecu" / "ecu_version"
 * fields. Widgets bindings (signal_name inside each widget config) are
 * preserved as-is.
 *
 * @param layout_name  Layout to rewrite (typically "default").
 * @param preset       Preset to apply.
 * @return ESP_OK on success.
 */
esp_err_t ecu_preset_apply_to_layout(const char *layout_name,
                                     const ecu_preset_t *preset);

/* True if the preset is the OBD2 Standard preset, which uses request/response
 * polling rather than a fixed broadcast bit-decode. The apply path stores the
 * default starter PID list in the layout JSON rather than rewriting signals[]
 * with bit-field rows. */
bool ecu_preset_is_obd2(const ecu_preset_t *preset);

/**
 * Replace the layout's `polled_pids` array with the given list. Used by the
 * OBD2 Signals picker after the user changes the enabled-PID set. Does NOT
 * touch the signals[] array, ecu/ecu_version fields, or widgets.
 *
 * @param layout_name  Layout to rewrite (typically "default").
 * @param pids         Encoded (service, pid) tuples — obd2_encode_pid().
 * @param count        Number of PIDs.
 * @return ESP_OK on success.
 */
esp_err_t ecu_preset_save_obd2_pids(const char *layout_name,
                                    const uint32_t *pids, uint8_t count);

/**
 * Read the layout's `polled_pids` array. Used by dashboard_init after a
 * layout load to know which PIDs to start polling. Falls back to the
 * legacy `obd2_pids` key for older layouts (bare bytes interpreted as
 * Mode 01).
 *
 * @param layout_name  Layout to read from.
 * @param out          Buffer to receive encoded (service, pid) tuples.
 * @param max          Capacity of @p out.
 * @param out_count    On return, number of PIDs written.
 * @return ESP_OK on success (may return zero PIDs).
 */
esp_err_t ecu_preset_read_obd2_pids(const char *layout_name,
                                    uint32_t *out, uint8_t max,
                                    uint8_t *out_count);

/* ── Live preset match scoring ──────────────────────────────────────────
 *
 * Continuous "is this preset receiving data right now?" indicator.
 *
 * The score is the percentage of the preset's unique broadcast CAN IDs
 * that the can_id_tracker has seen within the last ~2.5 seconds:
 *
 *     score = 100 * (preset_ids ∩ recently_seen_ids) / preset_id_count
 *
 * Returns 0..100. Returns 0 when:
 *   - The preset has no signals (e.g. the OBD2 placeholder), OR
 *   - No recent frames matched any of the preset's IDs, OR
 *   - can_id_tracker is empty (no CAN traffic at all).
 *
 * Live, not snapshot: plug in the loom, watch the score climb within
 * a couple of seconds; unplug, watch it fall back to 0. UIs that poll
 * this every few hundred ms see live updates. No need to run a separate
 * "Scan Car" first — the tracker is always on.
 *
 * Both UI sides (web /api/ecu/list and on-device ui_ecu_picker) use
 * this to draw the blue "detected" indicator and to gate Manual-mode
 * filtering at the configurable threshold.
 *
 * @param preset  Preset to score (use ecu_preset_find first).
 * @return        0..100. 0 means "no preset IDs currently broadcasting."
 */
int ecu_preset_match_score(const ecu_preset_t *preset);

/**
 * Threshold above which a preset is considered "detected on this bus."
 * Used by the picker UIs to draw the blue indicator and to gate the
 * Manual-mode filter. 30% is forgiving — typical broadcast presets
 * have 5-15 unique CAN IDs and not every ID is broadcast every cycle,
 * so a strict threshold (e.g. 100%) would false-negative real matches.
 */
#define ECU_PRESET_MATCH_THRESHOLD 30

#ifdef __cplusplus
}
#endif
