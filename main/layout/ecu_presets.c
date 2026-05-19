#include "ecu_presets.h"

#include "can/can_id_tracker.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "layout_manager.h"
#include "obd2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ecu_presets";

/* Threshold for "this CAN ID was seen recently." Pulled out as a constant so
 * the match_score logic stays readable. Matches the signal staleness window
 * loosely — if a CAN ID hasn't sent a frame in 2.5 seconds the bus is either
 * off, the ECU has stopped broadcasting that message, or the user just
 * disconnected the loom. Slightly longer than the 2-second signal stale
 * window so a freshly-attached vehicle doesn't flicker the score. */
#define LIVE_ID_FRESH_WINDOW_US ((int64_t)2500000)

/* Sentinel ECU make/version for the OBD2 preset. ecu_preset_is_obd2()
 * checks these. The rows[] table is all SIG_UNSUPPORTED because OBD2
 * doesn't use the slot-based bit decode — apply_to_layout writes the
 * starter PID list into the layout's obd2_pids array instead. */
#define OBD2_MAKE     "OBD2"
#define OBD2_VERSION  "Standard"

/* Normalized signal names (must match default_layout.c + widget bindings). */
static const char *const ECU_SIGNAL_NAMES[ECU_SIG__COUNT] = {
    "RPM",
    "MAP",
    "THROTTLE",
    "COOLANT_TEMP",
    "INTAKE_AIR_TEMP",
    "LAMBDA",
    "OIL_TEMP",
    "OIL_PRESSURE",
    "FUEL_PRESSURE",
    "IGNITION",
    "VEHICLE_SPEED",
    "GEAR",
    "BATTERY_VOLTAGE",
    "FUEL_TRIM",
    "EGT",
    "BOOST",
    "FUEL_LEVEL",
    "PARK_BRAKE",
    "YAW_RATE",
    "LATERAL_G",
};

const char *ecu_signal_slot_name(ecu_signal_slot_t slot) {
    if (slot >= ECU_SIG__COUNT) return "";
    return ECU_SIGNAL_NAMES[slot];
}

/* ── Preset tables ─────────────────────────────────────────────────────
 *
 * All outputs are metric. °F->°C conversion baked into scale/offset:
 *   C = (F - 32) * 5/9  ->  scale_C = scale_F * 5/9, offset_C = -32*5/9 - 17.7778
 * AFR->Lambda: scale_λ = scale_AFR / 14.7
 * m/s->km/h:   scale_kmh = scale_ms * 3.6
 * Pressure expressed in kPa throughout.
 *
 * Unsupported slots use can_id=0 and are written unbound into the layout.
 */

/* [0]=can_id, then bit_start, bit_length, scale, offset, is_signed, endian, unit, decimals */
#define SIG_UNSUPPORTED { 0, 0, 0, 0.0f, 0.0f, false, 0, "", 0 }

const ecu_preset_t ECU_PRESETS[] = {
    /* ══════════════════════════════════════════════════════════════════
     * ECU Master Black / Classic - base 0x600, Intel LE
     * Source: designer2k2/EMUcan (GPL) + ECUMaster ADU app note.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "ECU Master",
        .version = "Black/Classic",
        .display = "ECU Master Black / Classic",
        .rows = {
            [ECU_SIG_RPM]             = { 0x600,  0, 16, 1.0f,       0.0f,    false, 1, "rpm",    0 },
            [ECU_SIG_MAP]             = { 0x600, 32, 16, 1.0f,       0.0f,    false, 1, "kPa",    0 },
            [ECU_SIG_THROTTLE]        = { 0x600, 16,  8, 0.5f,       0.0f,    false, 1, "%",      1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x602, 48, 16, 1.0f,       0.0f,    true,  1, "degC",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x600, 24,  8, 1.0f,       0.0f,    true,  1, "degC",   0 },
            [ECU_SIG_LAMBDA]          = { 0x603, 16,  8, 0.0078125f, 0.0f,    false, 1, "lambda", 2 },
            [ECU_SIG_OIL_TEMP]        = { 0x602, 24,  8, 1.0f,       0.0f,    false, 1, "degC",   0 },
            [ECU_SIG_OIL_PRESSURE]    = { 0x602, 32,  8, 6.25f,      0.0f,    false, 1, "kPa",    0 },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x602, 40,  8, 6.25f,      0.0f,    false, 1, "kPa",    0 },
            [ECU_SIG_IGNITION]        = { 0x603,  0,  8, 0.5f,       0.0f,    true,  1, "deg",    1 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x602,  0, 16, 1.0f,       0.0f,    false, 1, "km/h",   0 },
            [ECU_SIG_GEAR]            = { 0x604,  0,  8, 1.0f,       0.0f,    true,  1, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x604, 16, 16, 0.027f,     0.0f,    false, 1, "V",      1 },
            [ECU_SIG_FUEL_TRIM]       = { 0x603, 24,  8, 0.5f,       -100.0f, false, 1, "%",      1 },
            [ECU_SIG_EGT]             = { 0x603, 32, 16, 1.0f,       0.0f,    false, 1, "degC",   0 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * MegaSquirt MS3-Pro - base 0x5F0, Motorola BE
     * Source: official Megasquirt CAN Broadcast spec (2014-10-27).
     * Metric conversions baked in: degF->degC, m/s->km/h, AFR->lambda.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "MegaSquirt",
        .version = "MS3-Pro",
        .display = "MegaSquirt MS3-Pro",
        .rows = {
            [ECU_SIG_RPM]             = { 0x5F0, 48, 16, 1.0f,       0.0f,      false, 0, "rpm",    0 },
            [ECU_SIG_MAP]             = { 0x5F2, 16, 16, 0.1f,       0.0f,      true,  0, "kPa",    1 },
            [ECU_SIG_THROTTLE]        = { 0x5F3,  0, 16, 0.1f,       0.0f,      true,  0, "%",      1 },
            /* degF->degC: scale = 0.1 * 5/9, offset = -32 * 5/9 */
            [ECU_SIG_COOLANT_TEMP]    = { 0x5F2, 48, 16, 0.0555556f, -17.7778f, true,  0, "degC",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x5F2, 32, 16, 0.0555556f, -17.7778f, true,  0, "degC",   0 },
            /* AFR->Lambda: scale = 0.1 / 14.7 */
            [ECU_SIG_LAMBDA]          = { 0x5FF,  0,  8, 0.0068027f, 0.0f,      false, 0, "lambda", 2 },
            [ECU_SIG_OIL_TEMP]        = SIG_UNSUPPORTED,  /* generic ADC only */
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,  /* generic ADC only */
            [ECU_SIG_FUEL_PRESSURE]   = { 0x615,  0, 16, 0.1f,       0.0f,      true,  0, "kPa",    1 },
            [ECU_SIG_IGNITION]        = { 0x5F1,  0, 16, 0.1f,       0.0f,      true,  0, "deg",    1 },
            /* m/s->km/h: scale = 0.1 * 3.6 */
            [ECU_SIG_VEHICLE_SPEED]   = { 0x612,  0, 16, 0.36f,      0.0f,      false, 0, "km/h",   0 },
            [ECU_SIG_GEAR]            = { 0x611, 48,  8, 1.0f,       0.0f,      true,  0, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x5F3, 16, 16, 0.1f,       0.0f,      true,  0, "V",      1 },
            /* egocor1 - 1/10 % centered on 100 */
            [ECU_SIG_FUEL_TRIM]       = { 0x5F4, 16, 16, 0.1f,       -100.0f,   true,  0, "%",      1 },
            [ECU_SIG_EGT]             = { 0x606,  0, 16, 0.0555556f, -17.7778f, true,  0, "degC",   0 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Haltech Nexus - base 0x360, Motorola BE
     * Source: Haltech CAN Broadcast Protocol v2.
     * Kelvin->degC via offset -273.15; PSI-abs->kPa-gauge via scale*6.895
     * and offset -101.325.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Haltech",
        .version = "Nexus",
        .display = "Haltech Nexus / Elite",
        .rows = {
            [ECU_SIG_RPM]             = { 0x360,  0, 16, 1.0f,     0.0f,      false, 0, "rpm",    0 },
            /* Manifold pressure: raw*0.1 = kPa absolute -> gauge via -101.325 */
            [ECU_SIG_MAP]             = { 0x360, 16, 16, 0.1f,     0.0f,      false, 0, "kPa",    1 },
            [ECU_SIG_THROTTLE]        = { 0x360, 32, 16, 0.1f,     0.0f,      false, 0, "%",      1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x3E0,  0, 16, 0.1f,     -273.15f,  false, 0, "degC",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x3E0, 16, 16, 0.1f,     -273.15f,  false, 0, "degC",   0 },
            /* Wideband 1 lambda: raw*0.001 directly */
            [ECU_SIG_LAMBDA]          = { 0x368,  0, 16, 0.001f,   0.0f,      false, 0, "lambda", 2 },
            [ECU_SIG_OIL_TEMP]        = { 0x3E0, 48, 16, 0.1f,     -273.15f,  false, 0, "degC",   0 },
            [ECU_SIG_OIL_PRESSURE]    = { 0x361, 16, 16, 0.1f,     -101.325f, false, 0, "kPa",    0 },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x361,  0, 16, 0.1f,     -101.325f, false, 0, "kPa",    0 },
            [ECU_SIG_IGNITION]        = { 0x362, 32, 16, 0.1f,     0.0f,      true,  0, "deg",    1 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x370,  0, 16, 0.1f,     0.0f,      false, 0, "km/h",   0 },
            [ECU_SIG_GEAR]            = { 0x360, 48, 16, 1.0f,     0.0f,      false, 0, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x372,  0, 16, 0.1f,     0.0f,      false, 0, "V",      1 },
            [ECU_SIG_FUEL_TRIM]       = { 0x3E3,  0, 16, 0.1f,     0.0f,      true,  0, "%",      1 },
            [ECU_SIG_EGT]             = { 0x373,  0, 16, 0.1f,     -273.15f,  false, 0, "degC",   0 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * MaxxECU 1.2 - base 0x520, Intel LE
     * Subset of 1.3 (no oil temp/pressure/fuel pressure broadcast).
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "MaxxECU",
        .version = "1.2",
        .display = "MaxxECU (firmware 1.2)",
        .rows = {
            [ECU_SIG_RPM]             = { 0x520,  0, 16, 1.0f,       0.0f,     false, 1, "rpm",    0 },
            [ECU_SIG_MAP]             = { 0x520, 32, 16, 0.1f,       0.0f,     false, 1, "kPa",    1 },
            [ECU_SIG_THROTTLE]        = { 0x520, 16, 16, 0.1f,       0.0f,     false, 1, "%",      1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x530, 48, 16, 0.1f,       0.0f,     true,  1, "degC",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x530, 32, 16, 0.1f,       0.0f,     true,  1, "degC",   0 },
            [ECU_SIG_LAMBDA]          = { 0x520, 48, 16, 0.001f,     0.0f,     false, 1, "lambda", 2 },
            [ECU_SIG_OIL_TEMP]        = SIG_UNSUPPORTED,  /* not in 1.2 */
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = { 0x521, 32, 16, 0.1f,       0.0f,     true,  1, "deg",    1 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x522, 48, 16, 0.1f,       0.0f,     false, 1, "km/h",   0 },
            [ECU_SIG_GEAR]            = { 0x536,  0, 16, 1.0f,       0.0f,     false, 1, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x530,  0, 16, 0.01f,      0.0f,     false, 1, "V",      1 },
            [ECU_SIG_FUEL_TRIM]       = { 0x531,  0, 16, 0.1f,       0.0f,     true,  1, "%",      1 },
            [ECU_SIG_EGT]             = { 0x533, 48, 16, 1.0f,       0.0f,     false, 1, "degC",   0 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Ford BA/BF - factory CAN, Motorola BE throughout.
     * MAP slot = barometric pressure (0x44D b56).
     * INTAKE_AIR_TEMP slot = ambient temperature (0x353 b32).
     * FUEL_TRIM slot = instant fuel value L/hr (0x437 b8).
     * Odometer (0x4C0), km range and instant economy (0x553) have no
     * standard slot and must be added manually in the Signals table.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Ford",
        .version = "BA/BF",
        .display = "Ford Falcon BA / BF",
        .rows = {
            [ECU_SIG_RPM]             = { 0x12D, 39, 16, 0.25f,      0.0f,   false, 0, "rpm",   0 },
            [ECU_SIG_MAP]             = { 0x44D, 56,  8, 0.5f,       0.0f,   false, 0, "kPa",   0 },
            [ECU_SIG_THROTTLE]        = { 0x207, 48,  8, 0.5f,       0.0f,   false, 0, "%",     1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x427,  0,  8, 1.0f,      -40.0f,  false, 0, "degC",  0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x353, 32,  8, 0.333333f, -30.0f,  false, 0, "degC",  0 },
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_TEMP]        = { 0x44D, 48,  8, 1.0f,      -40.0f,  false, 0, "degC",  0 },
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,
            [ECU_SIG_VEHICLE_SPEED]   = { 0x207, 32, 16, 0.0078125f, 0.0f,   false, 0, "km/h",  0 },
            [ECU_SIG_GEAR]            = SIG_UNSUPPORTED,
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x427, 24,  8, 0.1f,       0.0f,   false, 0, "V",     1 },
            [ECU_SIG_FUEL_TRIM]       = { 0x437,  8,  8, 0.51f,      0.0f,   false, 0, "L/hr",  1 },
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Ford FG — factory CAN broadcast. Inherits the BA/BF frame layout
     * (FG keeps the same engine-side broadcasts: 0x12D / 0x207 / 0x427 /
     * 0x44D / 0x437 / 0x353). The previous FG preset used CAN IDs that
     * don't appear anywhere on the actual FG bus (0x109 / 0x204 / 0x156 /
     * 0x171) — those were placeholder values from an early experiment
     * and have been retired.
     *
     * Sources cross-referenced 2026-05-18:
     *   - BA/BF preset (verified in real BA/BF vehicles)
     *   - jakka351/FG-Falcon "BigFalconSheet.xlsx" DBC tab
     *   - jakka351/FG-Falcon resources/fg_controller_area.dbc
     *
     * GEAR is new: FG broadcasts TransmissionMode on 0x210 byte 8 (bit 56,
     * 8-bit). Value is an integer mode code (P=1/R=2/N=3/D=4 typical) —
     * widget can map via a value table.
     *
     * Chassis extras enabled here based on the DBC tab in BigFalconSheet.xlsx
     * (a more complete reverse-engineering than fg_controller_area.dbc).
     * Bit positions sourced from the DBC `BO_/SG_` lines; scales are best
     * guesses since the original DBC encodes them as raw=1.0 (the actual
     * physical scaling isn't documented). The user can adjust scale/offset
     * via the Signals modal after a live capture confirms the encoding:
     *
     *   - BOOST       0x425 PCM_MSG_15  bit 31, 16-bit BE, kPa
     *   - FUEL_LEVEL  0x425 PCM_MSG_15  bit 47, 8-bit BE  (raw -> 0.392% per cnt
     *                                                     assumes 0..255 = 0..100%)
     *   - LATERAL_G   0x4B0 PCM_WHEEL_SPEED bit 56, 8-bit BE (raw scaled 0.01 g)
     *   - PARK_BRAKE  0x360 ReverseParkingSenseSystem bit 16, 8-bit LE (status flag)
     *   - YAW_RATE    0x000 ABS broadcast bit 16, 8-bit LE (placeholder — ID 0x000
     *                       and scaling are the least-confirmed bits in the sheet)
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Ford",
        .version = "FG",
        .display = "Ford Falcon FG",
        .rows = {
            [ECU_SIG_RPM]             = { 0x12D, 39, 16, 0.25f,       0.0f,   false, 0, "rpm",   0 },
            [ECU_SIG_MAP]             = { 0x44D, 56,  8, 0.5f,        0.0f,   false, 0, "kPa",   0 },
            [ECU_SIG_THROTTLE]        = { 0x207, 48,  8, 0.5f,        0.0f,   false, 0, "%",     1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x427,  0,  8, 1.0f,       -40.0f,  false, 0, "degC",  0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x353, 32,  8, 0.333333f,  -30.0f,  false, 0, "degC",  0 },
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_TEMP]        = { 0x44D, 48,  8, 1.0f,       -40.0f,  false, 0, "degC",  0 },
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,
            [ECU_SIG_VEHICLE_SPEED]   = { 0x207, 32, 16, 0.0078125f,  0.0f,   false, 0, "km/h",  0 },
            [ECU_SIG_GEAR]            = { 0x210, 56,  8, 1.0f,        0.0f,   false, 0, "",      0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x427, 24,  8, 0.1f,        0.0f,   false, 0, "V",     1 },
            [ECU_SIG_FUEL_TRIM]       = { 0x437,  8,  8, 0.51f,       0.0f,   false, 0, "L/hr",  1 },
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
            [ECU_SIG_BOOST]           = { 0x425, 31, 16, 1.0f,        0.0f,   false, 0, "kPa",   0 },
            [ECU_SIG_FUEL_LEVEL]      = { 0x425, 47,  8, 0.392157f,   0.0f,   false, 0, "%",     0 },
            [ECU_SIG_PARK_BRAKE]      = { 0x360, 16,  8, 1.0f,        0.0f,   false, 1, "",      0 },
            [ECU_SIG_YAW_RATE]        = { 0x000, 16,  8, 1.0f,        0.0f,   true,  1, "deg/s", 1 },
            [ECU_SIG_LATERAL_G]       = { 0x4B0, 56,  8, 0.01f,       0.0f,   true,  0, "g",     2 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * MaxxECU 1.3 - base 0x520, Intel LE
     * Source: MaxxECU CAN reference manual.
     * Matches existing preconfig_items[] scales.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "MaxxECU",
        .version = "1.3",
        .display = "MaxxECU (firmware 1.3+)",
        .rows = {
            [ECU_SIG_RPM]             = { 0x520,  0, 16, 1.0f,       0.0f,     false, 1, "rpm",    0 },
            [ECU_SIG_MAP]             = { 0x520, 32, 16, 0.1f,       0.0f,     false, 1, "kPa",    1 },
            [ECU_SIG_THROTTLE]        = { 0x520, 16, 16, 0.1f,       0.0f,     false, 1, "%",      1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x530, 48, 16, 0.1f,       0.0f,     true,  1, "degC",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x530, 32, 16, 0.1f,       0.0f,     true,  1, "degC",   0 },
            [ECU_SIG_LAMBDA]          = { 0x520, 48, 16, 0.001f,     0.0f,     false, 1, "lambda", 2 },
            [ECU_SIG_OIL_TEMP]        = { 0x536, 48, 16, 0.1f,       0.0f,     true,  1, "degC",   0 },
            [ECU_SIG_OIL_PRESSURE]    = { 0x536, 32, 16, 0.1f,       0.0f,     false, 1, "kPa",    0 },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x537,  0, 16, 0.1f,       0.0f,     false, 1, "kPa",    0 },
            [ECU_SIG_IGNITION]        = { 0x521, 32, 16, 0.1f,       0.0f,     true,  1, "deg",    1 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x522, 48, 16, 0.1f,       0.0f,     false, 1, "km/h",   0 },
            [ECU_SIG_GEAR]            = { 0x536,  0, 16, 1.0f,       0.0f,     false, 1, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x530,  0, 16, 0.01f,      0.0f,     false, 1, "V",      1 },
            [ECU_SIG_FUEL_TRIM]       = { 0x531,  0, 16, 0.1f,       0.0f,     true,  1, "%",      1 },
            [ECU_SIG_EGT]             = { 0x533, 48, 16, 1.0f,       0.0f,     false, 1, "degC",   0 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Link ECU Generic Dash - base 0x3E8, Intel LE
     * Source: Link G4+/G4X CAN reference.
     * Temperatures transmitted as (degC + 50).
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Link ECU",
        .version = "Generic Dash",
        .display = "Link ECU (G4+ / G4X Generic Dash)",
        .rows = {
            [ECU_SIG_RPM]             = { 0x3E8, 16, 16, 1.0f,  0.0f,    false, 1, "rpm",    0 },
            [ECU_SIG_MAP]             = { 0x3E8, 32, 16, 1.0f,  0.0f,    false, 1, "kPa",    0 },
            [ECU_SIG_THROTTLE]        = { 0x3E9, 32, 16, 0.1f,  0.0f,    false, 1, "%",      1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x3EA, 48, 16, 1.0f,  -50.0f,  false, 1, "degC",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x3EB, 16, 16, 1.0f,  -50.0f,  false, 1, "degC",   0 },
            [ECU_SIG_LAMBDA]          = { 0x3EE, 32, 16, 0.001f, 0.0f,   false, 1, "lambda", 2 },
            [ECU_SIG_OIL_TEMP]        = { 0x3F0, 16, 16, 1.0f,  -50.0f,  false, 1, "degC",   0 },
            [ECU_SIG_OIL_PRESSURE]    = { 0x3F0, 32, 16, 1.0f,  0.0f,    false, 1, "kPa",    0 },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x3EF, 48, 16, 1.0f,  0.0f,    false, 1, "kPa",    0 },
            [ECU_SIG_IGNITION]        = { 0x3EC, 48, 16, 0.1f,  -100.0f, true,  1, "deg",    1 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x3F0, 48, 16, 0.1f,  0.0f,    false, 1, "km/h",   0 },
            [ECU_SIG_GEAR]            = { 0x3EC, 16, 16, 1.0f,  0.0f,    false, 1, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x3EB, 32, 16, 0.01f, 0.0f,    false, 1, "V",      1 },
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,  /* not in Generic Dash */
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,  /* optional extension */
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * RDM-7 Internal - marker preset for the built-in CALCULATED_GEAR
     * flow (gear back-computed from RPM + VEHICLE_SPEED + user ratios;
     * see signal_internal.c). Name matches the "RDM-7" / "Internal"
     * pseudo-ECU already exposed through preset_picker.c's
     * preconfig_items[] so both paths share one identity.
     *
     * No CAN broadcast is implied, so all standard slots are unsupported.
     * Applying this preset is handled specially in
     * ecu_preset_apply_to_layout() — the signals[] array is left
     * untouched so the user keeps whatever RPM/VEHICLE_SPEED bindings
     * they already have from a prior ECU or manual setup.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "RDM-7",
        .version = "Internal",
        .display = "RDM-7 Internal (calculated gear)",
        .rows = {
            [ECU_SIG_RPM]             = SIG_UNSUPPORTED,
            [ECU_SIG_MAP]             = SIG_UNSUPPORTED,
            [ECU_SIG_THROTTLE]        = SIG_UNSUPPORTED,
            [ECU_SIG_COOLANT_TEMP]    = SIG_UNSUPPORTED,
            [ECU_SIG_INTAKE_AIR_TEMP] = SIG_UNSUPPORTED,
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_TEMP]        = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,
            [ECU_SIG_VEHICLE_SPEED]   = SIG_UNSUPPORTED,
            [ECU_SIG_GEAR]            = SIG_UNSUPPORTED,
            [ECU_SIG_BATTERY_VOLTAGE] = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * OBD2 Standard - SAE J1979 Mode 01 polling.
     *
     * No fixed-broadcast decoding: signals come from request/response
     * polling against the vehicle's ECU(s) on 0x7DF / 0x7E8. The 30 PID
     * starter set is defined in obd2_pids.c (default_enabled=true).
     *
     * apply_to_layout() detects this preset and writes the PID list into
     * the layout's `obd2_pids` array instead of rewriting signals[].
     * obd2_start() registers each enabled PID as an external signal at
     * load time.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = OBD2_MAKE,
        .version = OBD2_VERSION,
        .display = "OBD2 Standard (any 2008+ car)",
        .rows = {
            [ECU_SIG_RPM]             = SIG_UNSUPPORTED,
            [ECU_SIG_MAP]             = SIG_UNSUPPORTED,
            [ECU_SIG_THROTTLE]        = SIG_UNSUPPORTED,
            [ECU_SIG_COOLANT_TEMP]    = SIG_UNSUPPORTED,
            [ECU_SIG_INTAKE_AIR_TEMP] = SIG_UNSUPPORTED,
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_TEMP]        = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,
            [ECU_SIG_VEHICLE_SPEED]   = SIG_UNSUPPORTED,
            [ECU_SIG_GEAR]            = SIG_UNSUPPORTED,
            [ECU_SIG_BATTERY_VOLTAGE] = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
        },
    },

    /* Sentinel */
    { .make = NULL, .version = NULL, .display = NULL, .rows = {{0}} },
};

const int ECU_PRESETS_COUNT = (int)(sizeof(ECU_PRESETS)/sizeof(ECU_PRESETS[0])) - 1;

const ecu_preset_t *ecu_preset_find(const char *make, const char *version) {
    if (!make || !version) return NULL;
    for (int i = 0; ECU_PRESETS[i].make; i++) {
        if (strcmp(ECU_PRESETS[i].make, make) == 0 &&
            strcmp(ECU_PRESETS[i].version, version) == 0) {
            return &ECU_PRESETS[i];
        }
    }
    return NULL;
}

/* Build one signal JSON object from a preset row. can_id==0 rows produce
 * an unbound signal (can_id=0, retains name). */
static cJSON *_build_signal_json(const char *name, const ecu_signal_row_t *row) {
    cJSON *s = cJSON_CreateObject();
    if (!s) return NULL;
    cJSON_AddStringToObject(s, "name", name);
    cJSON_AddNumberToObject(s, "can_id", row->can_id);
    cJSON_AddNumberToObject(s, "bit_start", row->bit_start);
    cJSON_AddNumberToObject(s, "bit_length", row->bit_length ? row->bit_length : 16);
    cJSON_AddNumberToObject(s, "scale", row->scale != 0.0f ? row->scale : 1.0f);
    cJSON_AddNumberToObject(s, "offset", row->offset);
    cJSON_AddBoolToObject(s, "is_signed", row->is_signed);
    cJSON_AddStringToObject(s, "unit", row->unit ? row->unit : "");
    cJSON_AddNumberToObject(s, "endian", row->endian);
    return s;
}

bool ecu_preset_is_obd2(const ecu_preset_t *preset) {
    if (!preset || !preset->make || !preset->version) return false;
    return strcmp(preset->make, OBD2_MAKE) == 0 &&
           strcmp(preset->version, OBD2_VERSION) == 0;
}

/* ── Live preset match scoring ──────────────────────────────────────────
 *
 * Score reflects how many of the preset's unique broadcast CAN IDs were
 * seen on the bus within the last LIVE_ID_FRESH_WINDOW_US (≈2.5 s).
 *
 *     score = 100 * |preset_ids ∩ recently_seen_ids| / |preset_ids|
 *
 * Data source: can_id_tracker continuously records every CAN ID with a
 * `last_seen_us` timestamp. This is strictly better than the one-shot
 * `can_bus_test` scan that the picker previously used because:
 *
 *   - The user gets live feedback in the picker — plug in the loom, watch
 *     the blue dot light up next to their preset within a couple of seconds.
 *   - No need to remember to "Scan Car" first; the tracker is always on.
 *   - Disconnect the loom and the score returns to 0 within ~2.5 s as
 *     entries time out. UI's periodic refresh reflects this without help.
 *
 * Returns 0 for: NULL preset, OBD2 placeholder preset (no native
 * broadcast IDs), preset with all-unsupported slots, or no recent
 * frames matching any of the preset's IDs.
 *
 * Implementation: O(N * M) over preset_ids (≤ ECU_SIG__COUNT, small)
 * and tracker entries (≤ 64). Well under a microsecond per call.
 */
int ecu_preset_match_score(const ecu_preset_t *preset) {
    if (!preset) return 0;
    if (ecu_preset_is_obd2(preset)) return 0;  /* OBD2 has no broadcast IDs to match */

    /* Collect unique CAN IDs from the preset. */
    uint32_t preset_ids[ECU_SIG__COUNT];
    int preset_count = 0;
    for (int s = 0; s < ECU_SIG__COUNT; s++) {
        uint32_t id = preset->rows[s].can_id;
        if (id == 0) continue;  /* SIG_UNSUPPORTED */
        bool dup = false;
        for (int j = 0; j < preset_count; j++) {
            if (preset_ids[j] == id) { dup = true; break; }
        }
        if (!dup) preset_ids[preset_count++] = id;
    }
    if (preset_count == 0) return 0;  /* nothing to match against */

    /* Walk the tracker. For each preset ID, count it as a hit if the
     * tracker has an entry for it that was seen within the freshness
     * window. Tracker is single-writer / single-reader on the LVGL task
     * but the call sites for match_score (web handler + LVGL UI) can
     * read it freely — pointers are stable for the lifetime of the
     * table per the tracker header. */
    int64_t now_us = esp_timer_get_time();
    int64_t cutoff = now_us - LIVE_ID_FRESH_WINDOW_US;

    int hits = 0;
    uint16_t tracker_n = can_id_tracker_count();
    for (int i = 0; i < preset_count; i++) {
        for (uint16_t k = 0; k < tracker_n; k++) {
            const can_id_entry_t *e = can_id_tracker_get(k);
            if (!e) continue;
            if (e->can_id != preset_ids[i]) continue;
            if (e->last_seen_us >= cutoff) { hits++; break; }
        }
    }

    /* Round-to-nearest percentage so a 1/3 split surfaces as 33 not 33.33 -> 33. */
    int score = (hits * 100 + preset_count / 2) / preset_count;
    if (score > 100) score = 100;
    return score;
}

esp_err_t ecu_preset_apply_to_layout(const char *layout_name,
                                     const ecu_preset_t *preset) {
    if (!layout_name || !preset) return ESP_ERR_INVALID_ARG;

    /* Load the layout file as raw JSON. */
    char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t len = 0;
    esp_err_t err = layout_manager_read_raw(layout_name, buf, LAYOUT_MAX_FILE_BYTES, &len);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "read_raw failed for '%s': %s", layout_name, esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "parse failed for '%s'", layout_name);
        return ESP_FAIL;
    }

    /* RDM-7 Internal is a marker preset — signals[] is NOT rewritten so
     * existing RPM / VEHICLE_SPEED bindings (needed by CALCULATED_GEAR)
     * survive the apply. Only the ecu/ecu_version identity is updated. */
    bool is_marker_preset = (strcmp(preset->make, "RDM-7") == 0 &&
                             strcmp(preset->version, "Internal") == 0);

    bool is_obd2 = ecu_preset_is_obd2(preset);

    if (is_obd2) {
        /* OBD2 takes over as primary. Drop any prior native preset signals
         * and seed polled_pids[] with the default starter set. signals[] is
         * left empty — obd2_start() registers each enabled PID as an
         * external signal when polling begins.
         *
         * Also drop legacy `obd2_pids` if a previous firmware version wrote
         * one — only `polled_pids` is read going forward (with backwards-
         * compat fallback in layout_manager). */
        cJSON_DeleteItemFromObject(root, "signals");
        cJSON_AddArrayToObject(root, "signals");

        cJSON_DeleteItemFromObject(root, "obd2_pids");
        cJSON_DeleteItemFromObject(root, "polled_pids");
        cJSON *pids_arr = cJSON_AddArrayToObject(root, "polled_pids");
        for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
            if (OBD2_PIDS[i].default_enabled) {
                cJSON_AddItemToArray(pids_arr, cJSON_CreateNumber(OBD2_PIDS[i].pid));
            }
        }
    } else if (!is_marker_preset) {
        /* Replace signals array. */
        cJSON_DeleteItemFromObject(root, "signals");
        cJSON *sigs = cJSON_AddArrayToObject(root, "signals");
        for (int i = 0; i < ECU_SIG__COUNT; i++) {
            cJSON *s = _build_signal_json(ECU_SIGNAL_NAMES[i], &preset->rows[i]);
            if (s) cJSON_AddItemToArray(sigs, s);
        }

        /* Switching FROM OBD2 to a native preset: drop any supplemental
         * polled PIDs whose signal name is now provided by the native preset.
         * The UI picker prevents conflicts going forward, but legacy state
         * from an earlier OBD2 session would still be in the layout.
         * Simplest rule: just clear it. User can re-add supplemental
         * gap-fillers via the OBD2 Signals modal later. */
        cJSON_DeleteItemFromObject(root, "obd2_pids");    /* legacy name */
        cJSON_DeleteItemFromObject(root, "polled_pids");
    }

    /* Update ecu make/version fields. */
    cJSON_DeleteItemFromObject(root, "ecu");
    cJSON_DeleteItemFromObject(root, "ecu_version");
    cJSON_AddStringToObject(root, "ecu", preset->make);
    cJSON_AddStringToObject(root, "ecu_version", preset->version);

    /* Stamp decimals onto widgets whose signal_name matches a preset slot.
     * Applies to panel, bar, and text widgets which all read "decimals"
     * from their config JSON. Other widget types ignore the field harmlessly.
     * OBD2 preset doesn't carry per-slot decimals so we skip this step. */
    if (!is_marker_preset && !is_obd2) {
        cJSON *widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
        if (widgets) {
            cJSON *widget;
            cJSON_ArrayForEach(widget, widgets) {
                cJSON *cfg = cJSON_GetObjectItemCaseSensitive(widget, "config");
                if (!cfg) continue;
                cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
                if (!cJSON_IsString(sig_item)) continue;
                const char *sig_name = sig_item->valuestring;
                for (int i = 0; i < ECU_SIG__COUNT; i++) {
                    if (strcmp(sig_name, ECU_SIGNAL_NAMES[i]) == 0) {
                        cJSON_DeleteItemFromObject(cfg, "decimals");
                        cJSON_AddNumberToObject(cfg, "decimals",
                                                (double)preset->rows[i].decimals);
                        break;
                    }
                }
            }
        }
    }

    /* Write back. */
    err = layout_manager_save_raw(layout_name, root);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Applied preset '%s %s' to layout '%s'",
                 preset->make, preset->version, layout_name);
    }
    return err;
}

/* ── OBD2 PID list helpers ─────────────────────────────────────────────── */

esp_err_t ecu_preset_save_obd2_pids(const char *layout_name,
                                    const uint32_t *pids, uint8_t count) {
    if (!layout_name) return ESP_ERR_INVALID_ARG;

    char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t len = 0;
    esp_err_t err = layout_manager_read_raw(layout_name, buf,
                                            LAYOUT_MAX_FILE_BYTES, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    /* Always emit canonical `polled_pids` with encoded (service<<8|pid)
     * tuples, and clear the legacy `obd2_pids` key so the layout file
     * doesn't carry both. */
    cJSON_DeleteItemFromObject(root, "obd2_pids");
    cJSON_DeleteItemFromObject(root, "polled_pids");
    if (count > 0 && pids) {
        cJSON *arr = cJSON_AddArrayToObject(root, "polled_pids");
        for (uint8_t i = 0; i < count; i++) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(pids[i]));
        }
    }

    err = layout_manager_save_raw(layout_name, root);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %u polled PIDs to layout '%s'", count, layout_name);
    }
    return err;
}

esp_err_t ecu_preset_read_obd2_pids(const char *layout_name,
                                    uint32_t *out, uint8_t max,
                                    uint8_t *out_count) {
    if (!layout_name || !out || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t len = 0;
    esp_err_t err = layout_manager_read_raw(layout_name, buf,
                                            LAYOUT_MAX_FILE_BYTES, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    /* Prefer `polled_pids`; fall back to legacy `obd2_pids` so layouts
     * written by earlier firmware still read correctly. Numbers up to
     * 0xFF mean Mode 01 (back-compat); larger numbers carry the
     * (service<<8|pid) encoded form. */
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "polled_pids");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItemCaseSensitive(root, "obd2_pids");
    }
    if (cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            if (!cJSON_IsNumber(item)) continue;
            if (*out_count >= max) break;
            /* Numbers can be up to 24-bit (Mode 22 16-bit PID + service
             * byte). cJSON's valueint is int — fine for 24-bit values.
             * Use valuedouble for the rare case of larger numbers. */
            double v = item->valuedouble;
            if (v < 0 || v > 0xFFFFFFu) continue;
            out[(*out_count)++] = (uint32_t)v;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}
