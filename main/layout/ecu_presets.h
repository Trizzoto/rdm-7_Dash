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
 * a custom signal later without collision. */
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

#ifdef __cplusplus
}
#endif
