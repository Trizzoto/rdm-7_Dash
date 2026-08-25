#include "ecu_presets.h"

#include "can/can_id_tracker.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "layout_manager.h"
#include "obd2.h"
#include "widgets/signal.h"  /* SIGNAL_VALUE_LABEL_MAX */

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

/* Parallel table: which canonical channel each ECU signal slot feeds.
 * This is the bridge between the dash's legacy signal-name vocabulary
 * (RPM, MAP, THROTTLE, …) and the v14 canonical channel registry
 * (rpm, manifold_pressure, throttle_position, …). NULL = no canonical
 * mapping (signal isn't represented in the canonical list yet).
 *
 * Used by channel_manager_record_legacy_widget() to auto-bind widgets
 * whose signal_name doesn't case-match a canonical id directly.
 *
 * Order matches ECU_SIGNAL_NAMES exactly. */
static const char *const ECU_SIGNAL_CANONICAL[ECU_SIG__COUNT] = {
    "rpm",                        /* RPM */
    "manifold_pressure",          /* MAP — absolute pressure */
    "throttle_position",          /* THROTTLE */
    "coolant_temp",               /* COOLANT_TEMP */
    "intake_air_temp",            /* INTAKE_AIR_TEMP */
    "lambda_bank1",               /* LAMBDA — primary O2 sensor */
    "oil_temp",                   /* OIL_TEMP */
    "oil_pressure",               /* OIL_PRESSURE */
    "fuel_pressure",              /* FUEL_PRESSURE */
    "ignition_timing",            /* IGNITION */
    "vehicle_speed",              /* VEHICLE_SPEED */
    "gear",                       /* GEAR */
    "battery_voltage",            /* BATTERY_VOLTAGE */
    "short_term_fuel_trim",       /* FUEL_TRIM — short-term by convention */
    "exhaust_gas_temp_avg",       /* EGT — single-probe avg */
    "boost_pressure",             /* BOOST */
    "fuel_level",                 /* FUEL_LEVEL */
    "handbrake",                  /* PARK_BRAKE */
    "yaw_rate",                   /* YAW_RATE */
    "lateral_g",                  /* LATERAL_G */
};

/* Explicit alias table: derived preset signal names that are NOT an exact
 * ECU slot name but unambiguously denote a canonical channel. This
 * REPLACES the old fuzzy "_<SLOT>" suffix matcher, which produced
 * data-corrupting false positives — e.g. "REV-LIMIT RPM" → "REV_LIMIT_RPM"
 * suffix-matched "_RPM" and got renamed to "RPM", clobbering the real
 * engine-RPM signal's decode (last-write-wins). The suffix heuristic
 * could not distinguish "ENGINE_RPM" (a qualified engine RPM, correct)
 * from "REV_LIMIT_RPM" / "LAUNCH_END_RPM" (setpoints, wrong) from string
 * shape alone — so every alias is now spelled out.
 *
 * Keys are the UPPER_SNAKE form produced by the web editor's / wizard's
 * label→signal-name converter (uppercase, runs of non-alnum → single "_",
 * trailing "_" trimmed). Matched against the underscore-normalized name.
 *
 * EXHAUSTIVE across the current preconfig catalog (verified per-ECU). When
 * adding a new ECU/DBC, add its non-exact signal names here rather than
 * relying on any heuristic. Anything NOT listed (and not an exact slot
 * name) is intentionally left unmapped — better an unbound channel the
 * user can assign than a confidently-wrong auto-binding. */
typedef struct { const char *derived; const char *canonical; } ecu_alias_t;
static const ecu_alias_t ECU_SIGNAL_ALIASES[] = {
    /* RPM — only these qualified forms; rev-limit/launch-end RPM are
     * deliberately excluded (they are setpoints, not live engine RPM). */
    { "ENGINE_RPM",        "rpm" },                 /* Ford BA/BF, Ford FG, Toyota GT86 */
    { "ENGINE_SPEED",      "rpm" },                 /* Link ECU — its only RPM label */

    /* Ignition advance/timing — slot is bare "IGNITION", these are the
     * common descriptive forms. */
    { "IGNITION_ANGLE",    "ignition_timing" },     /* ECU Master, MegaSquirt, MaxxECU */
    { "IGNITION_TIMING",   "ignition_timing" },     /* Link ECU */

    /* Manifold pressure — slot is "MAP". */
    { "MANIFOLD_PRESSURE", "manifold_pressure" },   /* Haltech */
    { "MAP_KPA",           "manifold_pressure" },   /* ECU Master, MegaSquirt "MAP (kPa)" */

    /* Throttle — slot is bare "THROTTLE". */
    { "THROTTLE_POSITION", "throttle_position" },   /* Haltech */

    /* Battery — slot is "BATTERY_VOLTAGE"; "VOLT" is the common short form. */
    { "BATTERY_VOLT",      "battery_voltage" },      /* Haltech, ECU Master, MegaSquirt */

    /* Gear — slot is bare "GEAR". */
    { "GEAR_POSITION",     "gear" },                 /* Link ECU */
    { "GEAR_BITMASK",      "gear" },                 /* Ford FG */

    /* Pressures carried with a unit suffix block the bare-slot exact match. */
    { "OIL_PRESSURE_KPA",  "oil_pressure" },         /* ECU Master */
    { "FUEL_PRESSURE_KPA", "fuel_pressure" },        /* ECU Master */

    /* MaxxECU's "Total Fuel Trim" is the fuel-trim quantity its preset
     * binds to the FUEL_TRIM slot. Explicit so we don't have to whitelist
     * the ambiguous "TOTAL" prefix globally. */
    { "TOTAL_FUEL_TRIM",   "short_term_fuel_trim" }, /* MaxxECU 1.2 / 1.3 */

    /* Barometric — no ECU slot exists; binds the canonical channel
     * directly (no rename, since there's no legacy slot name). */
    { "BARO_PRESSURE",     "barometric_pressure" },  /* Ford, MaxxECU, Link, Haltech */

    /* ── Wizard 100%-coverage aliases ──────────────────────────────────────
     * Preset signals whose derived name doesn't exact-match a canonical id
     * (case-insensitive) and isn't one of the 20 ECU slots. Each is an
     * explicit, hand-verified map onto an existing canonical channel so the
     * setup wizard can bind 100% of a preset's signals to canonical channels
     * (curated units/ranges/grouping) instead of auto-creating custom ones.
     * EXACT match only — no fuzzy matching (see header comment). Signals NOT
     * listed here AND not exact-id matches fall through to a custom channel. */

    /* Temperatures / air */
    { "AMBIENT_AIR_TEMP",  "ambient_temp" },          /* Haltech */
    { "AIR_TEMP",          "intake_air_temp" },        /* Haltech 3E0 air temp */
    { "IAT",               "intake_air_temp" },        /* Link */
    { "DIFF_OIL_TEMP",     "differential_temp" },      /* Haltech */
    { "GEARBOX_OIL_TEMP",  "transmission_temp" },      /* Haltech */

    /* Throttle / pedal */
    { "TPS",               "throttle_position" },      /* Link */
    { "APS_MAIN",          "accel_pedal_position" },   /* Link */
    { "ACCEL_PEDAL",       "accel_pedal_position" },   /* Ford FG */

    /* Pressure / boost / air flow */
    { "MGP",               "boost_pressure" },         /* Link manifold gauge pressure */
    { "BRAKE_PRESSURE",    "brake_pressure_front" },   /* Haltech */
    { "MAF",               "mass_air_flow" },          /* Link */

    /* Electrical */
    { "ECU_VOLTS",         "ecu_voltage" },            /* Link */

    /* Fuel system */
    { "DAMPED_FUEL_LVL",   "fuel_level" },             /* Ford BA/BF */
    { "INSTANT_ECONOMY",   "fuel_consumption_instant" }, /* Ford BA/BF */
    { "KM_RANGE",          "fuel_remaining_distance" },  /* Ford BA/BF */
    { "ETHANOL",           "ethanol_pct" },            /* Link "ETHANOL %" */
    { "FUEL_COMP",         "ethanol_pct" },            /* Haltech fuel composition */
    { "INJ_PULSE_WIDTH",   "injector_pulse_width" },   /* Link */
    { "INJECTOR_DC",       "injector_duty" },          /* Link */
    { "FUEL_TRIM_ST_B1",   "short_term_fuel_trim" },   /* Haltech */
    { "FUEL_TRIM_LT_B1",   "long_term_fuel_trim" },    /* Haltech */
    { "FUEL_TRIM_B1",      "short_term_fuel_trim" },   /* MegaSquirt */

    /* Lambda — λ-scaled sensors map to the λ Lambda-Bank channels. */
    { "LAMBDA_1",          "lambda_bank1" },           /* Link (λ-scaled) */
    { "LAMBDA_2",          "lambda_bank2" },           /* Link (λ-scaled) */
    { "LAMBDA_AFR1",       "lambda_bank1" },           /* MegaSquirt "LAMBDA (AFR1)" — λ-scaled */

    /* Wideband — Haltech broadcasts AFR-scaled "Wideband N" sensors. These
     * are a DIFFERENT quantity (AFR, not λ) so they get dedicated AFR
     * Wideband channels, NOT the λ Lambda-Bank channels — putting an AFR
     * value (~14.7) into a λ channel (range 0.6–1.3) was wrong units +
     * range. Primary + secondary only; sensors 3/4 fall to custom. */
    { "WIDEBAND_1",        "wideband_1" },             /* Haltech (AFR-scaled) */
    { "WIDEBAND_2",        "wideband_2" },             /* Haltech (AFR-scaled) */

    /* Ignition */
    { "IGN_ANGLE_LEAD",    "ignition_timing" },        /* Haltech */

    /* EGT — Haltech "EGT SENSOR N" / ECU Master "EGT N" → egt_cyl_1..8
     * (probes 9-12 have no canonical and fall to custom). */
    { "EGT_SENSOR_1",      "egt_cyl_1" },              /* Haltech */
    { "EGT_SENSOR_2",      "egt_cyl_2" },
    { "EGT_SENSOR_3",      "egt_cyl_3" },
    { "EGT_SENSOR_4",      "egt_cyl_4" },
    { "EGT_SENSOR_5",      "egt_cyl_5" },
    { "EGT_SENSOR_6",      "egt_cyl_6" },
    { "EGT_SENSOR_7",      "egt_cyl_7" },
    { "EGT_SENSOR_8",      "egt_cyl_8" },
    { "EGT_1",             "egt_cyl_1" },              /* ECU Master, MegaSquirt */
    { "EGT_2",             "egt_cyl_2" },

    /* Wheel speeds — both naming conventions in the catalog. */
    { "WHEEL_SPD_FL",      "wheel_speed_fl" },         /* Ford FG */
    { "WHEEL_SPD_FR",      "wheel_speed_fr" },
    { "WHEEL_SPD_RL",      "wheel_speed_rl" },
    { "WHEEL_SPD_RR",      "wheel_speed_rr" },
    { "LF_WHEEL_SPEED",    "wheel_speed_fl" },         /* Link */
    { "RF_WHEEL_SPEED",    "wheel_speed_fr" },
    { "LR_WHEEL_SPEED",    "wheel_speed_rl" },
    { "RR_WHEEL_SPEED",    "wheel_speed_rr" },
};
static const size_t ECU_SIGNAL_ALIASES_COUNT =
    sizeof(ECU_SIGNAL_ALIASES) / sizeof(ECU_SIGNAL_ALIASES[0]);

const char *ecu_signal_name_to_canonical(const char *signal_name) {
    if (!signal_name || signal_name[0] == '\0') return NULL;

    /* 1. Direct case-sensitive match — fast path for standard preset names. */
    for (size_t i = 0; i < ECU_SIG__COUNT; ++i) {
        if (strcmp(ECU_SIGNAL_NAMES[i], signal_name) == 0)
            return ECU_SIGNAL_CANONICAL[i];
    }

    /* 2. Normalized match — strip leading and trailing underscores. Catches
     *    user-typed labels normalized to signal names by the web editor's
     *    label-to-signal converter, which leaves trailing underscores when
     *    the label has trailing whitespace/punctuation (e.g. "Fuel Level "
     *    → "FUEL_LEVEL_" and "Fuel Level - " → "FUEL_LEVEL__"). */
    char norm[64];
    size_t ni = 0, si = 0;
    while (signal_name[si] == '_') si++;
    while (signal_name[si] && ni + 1 < sizeof(norm)) norm[ni++] = signal_name[si++];
    while (ni > 0 && norm[ni - 1] == '_') ni--;
    norm[ni] = '\0';
    if (ni == 0) return NULL;

    for (size_t i = 0; i < ECU_SIG__COUNT; ++i) {
        if (strcmp(ECU_SIGNAL_NAMES[i], norm) == 0)
            return ECU_SIGNAL_CANONICAL[i];
    }

    /* 3. Explicit alias lookup (replaces the old suffix heuristic).
     *    EXACT match only — no fuzzy prefixes/suffixes, so collisions and
     *    false positives are impossible. */
    for (size_t i = 0; i < ECU_SIGNAL_ALIASES_COUNT; ++i) {
        if (strcmp(ECU_SIGNAL_ALIASES[i].derived, norm) == 0)
            return ECU_SIGNAL_ALIASES[i].canonical;
    }

    return NULL;
}

const char *ecu_signal_slot_name(ecu_signal_slot_t slot) {
    if (slot >= ECU_SIG__COUNT) return "";
    return ECU_SIGNAL_NAMES[slot];
}

ecu_signal_slot_t ecu_slot_from_canonical_id(const char *canonical_id) {
    if (!canonical_id || !canonical_id[0]) return ECU_SIG__COUNT;
    for (size_t i = 0; i < ECU_SIG__COUNT; ++i) {
        const char *c = ECU_SIGNAL_CANONICAL[i];
        if (c && strcmp(c, canonical_id) == 0) return (ecu_signal_slot_t)i;
    }
    return ECU_SIG__COUNT;
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
#define SIG_UNSUPPORTED { 0, 0, 0, 0.0f, 0.0f, false, 0, "", 0, NULL }

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
     * Haltech Nexus / Elite — base 0x360, Motorola BE (everything).
     * Source: Haltech CAN Broadcast Protocol v2.35.0.
     * Kelvin->degC via offset -273.15; kPa-abs->kPa-gauge via -101.325.
     *
     * GEAR was previously bound to 0x360 byte 6-7 — which is actually
     * Coolant Pressure per the v2.35.0 spec. The real gear channel lives
     * at 0x470 byte 7 as a single signed enum byte:
     *   -1=Reverse, 0=Neutral, 1=1st, 2=2nd, 3=3rd, 4=4th, 5=5th, 6=6th
     * (0x470 byte 6 is the gear-selector position, also signed, with a
     * wider enum including Park/Drive/Sport/Manual/Low/Overdrive — that
     * one lives on the layout's Signals modal if a user wants it.)
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
            /* GEAR: corrected from 0x360 byte 6-7 (Coolant Pressure) to
             * 0x470 byte 7 (signed enum). See block comment above. */
            [ECU_SIG_GEAR]            = { 0x470, 56,  8, 1.0f,     0.0f,      true,  0, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x372,  0, 16, 0.1f,     0.0f,      false, 0, "V",      1 },
            /* Fuel Trim Short Term Bank 1: 0x3E3 byte 0-1, signed % */
            [ECU_SIG_FUEL_TRIM]       = { 0x3E3,  0, 16, 0.1f,     0.0f,      true,  0, "%",      1 },
            [ECU_SIG_EGT]             = { 0x373,  0, 16, 0.1f,     -273.15f,  false, 0, "degC",   0 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * MaxxECU 1.2 - base 0x520, Intel LE
     * Subset of 1.3 (no oil temp/pressure/fuel pressure broadcast).
     *
     * Bit/scale/offset/id verified against the official
     * MaxxECU_Default_CAN_protocol_v1.2.dbc — every mapped field matches.
     * The v1.2 DBC marks ALL fields `@1+` (Intel unsigned).
     *
     * SIGN — same deliberate deviation as the 1.3 preset (see below):
     * COOLANT_TEMP, INTAKE_AIR_TEMP, IGNITION, GEAR and FUEL_TRIM are
     * forced is_signed=true so sub-zero temps, ignition retard, reverse
     * gear and negative trims (which the ECU sends as two's-complement)
     * decode correctly instead of reading ~6500/65535. Signed is
     * identical to unsigned for every field's legitimate positive range
     * (raw never sets bit 15 below 3276.7), so this is safe even where
     * the value happens to always be positive. Confirmed in-vehicle
     * (gear/fuel-trim misreads field-reported 2026-08).
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
            [ECU_SIG_GEAR]            = { 0x536,  0, 16, 1.0f,       0.0f,     true,  1, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x530,  0, 16, 0.01f,      0.0f,     false, 1, "V",      1 },
            /* FuelTrimTotal: two's-complement ±% on the wire (0 = no
             * correction); signed decode is identity for positives. */
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
     * Ford FG — factory hscan (500 kbps engine bus) only.
     *
     * Cross-referenced 2026-05-24 against BigFalconSheet.xlsx
     * (FgControllerArea tab) and live canraw captures from an XR6T.
     * The workbook's DBC tab is partly mis-parsed (overlapping
     * length=1 rows); FgControllerArea is byte-accurate and is the
     * authoritative source used below.
     *
     * Verification status of each row, against the canraw + decoded log:
     *
     *   VERIFIED (live values match expected physical range)
     *     RPM, THROTTLE (idle..~63%), COOLANT_TEMP, OIL_TEMP,
     *     BATTERY_VOLTAGE (alt charging visible at 13.6V)
     *
     *   POSITION VERIFIED, MAGNITUDE NEEDS A DRIVING CAPTURE
     *     VEHICLE_SPEED — bytes 4-5 of 0x207 read 0xFFFF sentinel
     *       (= 511.99 km/h) whenever stationary. Scale 0.0078125
     *       km/h/cnt (= raw 12800 -> 100 km/h) per spreadsheet.
     *     GEAR — 0x230 byte 0 reads 0 in Park. Byte 2 (ActualGear-
     *       Position) reads 0x68 = 104 in P; both bytes use an
     *       encoded value table, not a literal gear number. Need a
     *       full P->R->N->D->1..5 cycle to build the mapping.
     *
     *   UNVERIFIED — keeps current values pending more data
     *     BOOST — 0x425 bytes 3-4 are stuck at 0xFFFF across every
     *       captured frame, even on a turbo (engine made 510 Nm per
     *       0x097 ActualEngineTorque in our log). Either the PCM
     *       only updates this when actually positive-boost, or the
     *       real boost signal lives elsewhere (0x425 byte 7 is the
     *       only fast-varying engine byte and is a candidate — but
     *       it could equally be InstantFuelConsumption). Needs a
     *       WOT pull capture to disambiguate.
     *     FUEL_LEVEL — current 0x425 bit 47 / scale 0.392 reads 75%
     *       at 3/4 tank, but mathematically the result is driven by
     *       byte 6 (fuel-consumption signal that happens to sit
     *       around 0x80 at idle), not byte 5 (which is constant 0x03
     *       = quarter-tank index). Real encoding is likely byte 5
     *       with scale 25.0 %/cnt (4 quarters). Left as-is for now;
     *       re-verify across a tank burn.
     *     PARK_BRAKE — 0x437 byte 2 (HandbrakeStatus) is the right
     *       per-spec position, but 0x437 didn't appear in our two
     *       captures. Low broadcast rate or condition-gated. Verify
     *       with a longer capture toggling the handbrake.
     *
     *   DROPPED (no factory broadcast on hscan, or prior row pointed
     *   at the wrong thing)
     *     MAP            -> SIG_UNSUPPORTED (FG factory doesn't
     *                       broadcast manifold pressure; 0x44D byte 7
     *                       is barometric, not MAP)
     *     INTAKE_AIR_TEMP-> SIG_UNSUPPORTED (was reading HVAC cabin
     *                       temp on mscan bus we can't even hear)
     *     FUEL_TRIM      -> SIG_UNSUPPORTED (no STFT/LTFT broadcast)
     *     LATERAL_G      -> SIG_UNSUPPORTED (0x4B0 byte 7 is wheel
     *                       speed, not lat-G)
     *     YAW_RATE       -> SIG_UNSUPPORTED (prior 0x000 was a
     *                       placeholder, ID doesn't exist)
     *
     * Bit-position fixes vs the previous revision:
     *   RPM   bit 39 -> 32  (off by 7 bits, was decoding ~15 000 rpm
     *                        at idle by reading bytes 4-5 through a
     *                        1-bit shift instead of as a clean 16-bit
     *                        BE field)
     *   GEAR  0x210 bit 56 -> 0x230 bit 0  (0x210 has no gear field;
     *                        real gear data lives on 0x230)
     *   PARK_BRAKE 0x360 bit 16 -> 0x437 bit 16  (0x360 is mscan;
     *                        0x437 byte 2 is the right hscan source)
     *
     * Frames in this preset and the bus they live on (for reference
     * when adding a second-channel transceiver later):
     *   hscan: 0x12D, 0x207, 0x230, 0x425, 0x427, 0x437, 0x44D
     *   mscan: 0x353 (HVAC), 0x360 (park sense), 0x4B0 (wheel speeds)
     *
     * Bonus signals visible on hscan but not yet wired to a standard
     * ECU_SIG_* slot — users can expose via the Signals modal:
     *   0x097 bytes 0-1 IndicatedEngineTorque (Nm, 16-bit BE signed)
     *   0x097 bytes 2-3 FrictionLoss (Nm)
     *   0x097 bytes 4-5 ActualEngineTorque (Nm)
     *   0x097 bytes 6-7 DriverDemandedTorque (Nm)
     *   0x120 bytes 0-1 EngineTorqueMinusReduction (Nm)
     *   0x12D byte 1    AcceleratorPedalPosition (%, x0.5)
     *   0x4C0 bytes 0-2 Odometer (24-bit LE, raw ~ km x 100)
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Ford",
        .version = "FG",
        .display = "Ford Falcon FG",
        .rows = {
            [ECU_SIG_RPM]             = { 0x12D, 32, 16, 0.25f,       0.0f,   false, 0, "rpm",   0 },
            [ECU_SIG_MAP]             = SIG_UNSUPPORTED,
            [ECU_SIG_THROTTLE]        = { 0x207, 48,  8, 0.5f,        0.0f,   false, 0, "%",     1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x427,  0,  8, 1.0f,       -40.0f,  false, 0, "degC",  0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = SIG_UNSUPPORTED,
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_TEMP]        = { 0x44D, 48,  8, 1.0f,       -40.0f,  false, 0, "degC",  0 },
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,
            [ECU_SIG_VEHICLE_SPEED]   = { 0x207, 32, 16, 0.0078125f,  0.0f,   false, 0, "km/h",  0 },
            [ECU_SIG_GEAR]            = { 0x230,  0,  8, 1.0f,        0.0f,   false, 0, "",      0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x427, 24,  8, 0.1f,        0.0f,   false, 0, "V",     1 },
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
            [ECU_SIG_BOOST]           = { 0x425, 31, 16, 1.0f,        0.0f,   false, 0, "kPa",   0 },
            [ECU_SIG_FUEL_LEVEL]      = { 0x425, 47,  8, 0.392157f,   0.0f,   false, 0, "%",     0 },
            [ECU_SIG_PARK_BRAKE]      = { 0x437, 16,  8, 1.0f,        0.0f,   false, 0, "",      0 },
            [ECU_SIG_YAW_RATE]        = SIG_UNSUPPORTED,
            [ECU_SIG_LATERAL_G]       = SIG_UNSUPPORTED,
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * MaxxECU 1.3 - base 0x520, Intel LE
     * Source: MaxxECU_Default_CAN_protocol_v1.3.dbc (official download).
     *
     * Bit/scale/offset/id all verified against the official
     * MaxxECU_Default_CAN_protocol_v1.3.dbc — every mapped field matches.
     *
     * SIGN — deliberate deviation from the DBC for three fields:
     * The DBC (AEM-template-derived, note its auto-generated [0|6553.5]
     * ranges) marks every field `@1+` (Intel unsigned) EXCEPT
     * Engine_Oil_Temperature (0x536 b48, `@1-`). But CoolantTemp,
     * IntakeAirTemp and Ignition_Timing all read negative in-vehicle —
     * temps below 0°C on a cold start, ignition retarding past TDC — which
     * the ECU can only put on the wire as two's-complement. The DBC's
     * `@1+` then misreads those as ~6500. Their raw never legitimately
     * sets bit 15 in range (120°C = 1200, 50° adv = 500), so reading them
     * SIGNED is identical for all positive values and correct for the
     * negatives. We therefore force is_signed=true on COOLANT_TEMP,
     * INTAKE_AIR_TEMP, IGNITION and OIL_TEMP (the DBC's own `@1-` field).
     * Confirmed in-vehicle on a MaxxECU.
     *
     * GEAR and FUEL_TRIM are ALSO signed (field-reported 2026-08):
     * reverse gear arrives as two's-complement -1 (unsigned decode showed
     * 65535) and FuelTrimTotal goes negative when trimming fuel out
     * (unsigned decode showed ~6500). Both stay bit-identical to unsigned
     * across their positive range, so configs that never see a negative
     * are unaffected. The same rule is applied to every bidirectional
     * quantity in the preconfig catalog (preset_picker.c): knock/ignition
     * compensation, lambda corrections, accelerometer axes, cam positions,
     * slip and the gearbox/diff temps — keep the two tables in lockstep
     * (tools/check_preset_signedness.py enforces this).
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
            /* Engine_Oil_Temperature: the one signed field in this block. */
            [ECU_SIG_OIL_TEMP]        = { 0x536, 48, 16, 0.1f,       0.0f,     true,  1, "degC",   0 },
            [ECU_SIG_OIL_PRESSURE]    = { 0x536, 32, 16, 0.1f,       0.0f,     false, 1, "kPa",    0 },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x537,  0, 16, 0.1f,       0.0f,     false, 1, "kPa",    0 },
            [ECU_SIG_IGNITION]        = { 0x521, 32, 16, 0.1f,       0.0f,     true,  1, "deg",    1 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x522, 48, 16, 0.1f,       0.0f,     false, 1, "km/h",   0 },
            [ECU_SIG_GEAR]            = { 0x536,  0, 16, 1.0f,       0.0f,     true,  1, "",       0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x530,  0, 16, 0.01f,      0.0f,     false, 1, "V",      1 },
            /* FuelTrimTotal: two's-complement ±% (0 = no correction). */
            [ECU_SIG_FUEL_TRIM]       = { 0x531,  0, 16, 0.1f,       0.0f,     true,  1, "%",      1 },
            [ECU_SIG_EGT]             = { 0x533, 48, 16, 1.0f,       0.0f,     false, 1, "degC",   0 },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Link ECU Generic Dash - CAN id 0x3E8, Intel LE, MULTIPLEXED
     * Source: Link G4+/G4X CAN reference.
     *
     * The entire stream is ONE can id. Byte 0 is a frame index and the three
     * 16-bit words at bytes 2-3 / 4-5 / 6-7 carry different quantities for
     * each index, so these rows are separated by mux_value (the trailing
     * { 0, 8, N }) rather than by id. The frame index used to be folded into
     * the id as 0x3E8 + frame, which addressed 13 ids the ECU never sends —
     * see the note above the matching rows in preset_picker_data.c.
     *
     * Temperatures transmitted as (degC + 50).
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Link ECU",
        .version = "Generic Dash",
        .display = "Link ECU (G4+ / G4X Generic Dash)",
        /* The generic dash stream id is whatever the tuner types into
         * PCLink; 0x3E8 (1000) is only the convention. Declaring it here
         * lets the picker offer a Base CAN ID, which is what a car running
         * two dashes off one bus needs — a second generic stream on a spare
         * id for the RDM, leaving 0x3E8 to the other dash. */
        .base_id = 0x3E8,
        .rows = {
            [ECU_SIG_RPM]             = { 0x3E8, 16, 16, 1.0f,  0.0f,    false, 1, "rpm",    0, NULL, 0, 8, 0 },
            [ECU_SIG_MAP]             = { 0x3E8, 32, 16, 1.0f,  0.0f,    false, 1, "kPa",    0, NULL, 0, 8, 0 },
            [ECU_SIG_THROTTLE]        = { 0x3E8, 32, 16, 0.1f,  0.0f,    false, 1, "%",      1, NULL, 0, 8, 1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x3E8, 48, 16, 1.0f,  -50.0f,  false, 1, "degC",   0, NULL, 0, 8, 2 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x3E8, 16, 16, 1.0f,  -50.0f,  false, 1, "degC",   0, NULL, 0, 8, 3 },
            [ECU_SIG_LAMBDA]          = { 0x3E8, 32, 16, 0.001f, 0.0f,   false, 1, "lambda", 2, NULL, 0, 8, 6 },
            [ECU_SIG_OIL_TEMP]        = { 0x3E8, 16, 16, 1.0f,  -50.0f,  false, 1, "degC",   0, NULL, 0, 8, 8 },
            [ECU_SIG_OIL_PRESSURE]    = { 0x3E8, 32, 16, 1.0f,  0.0f,    false, 1, "kPa",    0, NULL, 0, 8, 8 },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x3E8, 48, 16, 1.0f,  0.0f,    false, 1, "kPa",    0, NULL, 0, 8, 7 },
            /* Timing is offset-encoded unsigned on the wire: raw =
             * (deg + 100) * 10, so the -100 offset does the sign work. */
            [ECU_SIG_IGNITION]        = { 0x3E8, 48, 16, 0.1f,  -100.0f, false, 1, "deg",    1, NULL, 0, 8, 4 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x3E8, 48, 16, 0.1f,  0.0f,    false, 1, "km/h",   0, NULL, 0, 8, 8 },
            [ECU_SIG_GEAR]            = { 0x3E8, 16, 16, 1.0f,  0.0f,    false, 1, "",       0, NULL, 0, 8, 4 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x3E8, 32, 16, 0.01f, 0.0f,    false, 1, "V",      1, NULL, 0, 8, 3 },
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,  /* not in Generic Dash */
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,  /* optional extension */
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Link ECU — AiM stream. CAN ids 0x5F0-0x5FF, Intel LE, NOT multiplexed.
     * Source: "AiM Infotech — AiM CAN protocol, Release 1.01" (the template
     * an ECU transmits so an AiM MXS/MXL/MXG can read it). Select it in
     * PCLink as the AiM/CAN dash stream; AiM's own end is configured as
     * manufacturer "AIM", model "CAN_500kbits" or "CAN_1Mbit".
     *
     * NOT a mux, unlike the Generic Dash above — no frame index, no bit
     * shuffling. Sixteen plain consecutive ids, each 8 bytes carrying four
     * 16-bit little-endian words at fixed byte offsets, so a row is
     * addressed by (id, bit_start) alone and mux_bit_length stays 0.
     *
     * The protocol is full-scale 16-bit: every channel spends the whole
     * 0-65535 range on its stated span, which is why the divisors look
     * unusual (TPS is raw/650 for 0-100.8 %, not raw/10). AiM states them
     * as MULT/DIV plus a "x10"/"x100"/"x1000" tag on the unit; the scales
     * below fold both together, e.g. TPS = 1/65 then /10 = raw/650.
     * Temperatures are (raw/19 - 450) tenths of °C, so raw/190 - 45.
     * Offsets are applied AFTER scaling here, which is why -45 and not -450.
     *
     * Pressures are converted to the dash's metric convention on the way in:
     * AiM sends MAP/baro in mBar and oil/fuel/boost in bar, the dash wants
     * kPa everywhere (1 mBar = 0.1 kPa, 1 bar = 100 kPa).
     *
     * FUEL LEVEL is deliberately absent: AiM sends litres (ECU_FUEL_LEV,
     * 0x5F5 bytes 6-7) and this slot is a percentage everywhere else, so
     * binding it would put litres on a % gauge. Same reasoning for the
     * IMU channels on 0x5FA — AiM's "gyro" is degrees, not deg/s.
     *
     * DECODE NOT YET CONFIRMED AGAINST A CAR. The Generic Dash rows above
     * are backed by docs/research/can-captures — these are from the AiM
     * spec only. Capture a car running the AiM stream and check before
     * treating them as proven.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Link ECU",
        .version = "AiM Stream",
        .display = "Link ECU (G4+ / G4X AiM Stream)",
        /* The AiM stream's base is whatever the tuner sets in PCLink; 0x5F0 is
         * the AiM default. A car running two dashes off one bus moves one of
         * the two streams, and it can just as easily be this one — a Link
         * feeding an AiM dash on the generic stream at 0x3E8 while the RDM
         * reads the AiM stream somewhere else. Sixteen consecutive ids, even
         * though the bound rows stop at base+0x0B. */
        .base_id = 0x5F0,
        .stream_span = 0x0F,
        .rows = {
            [ECU_SIG_RPM]             = { 0x5F0,  0, 16, 1.0f,        0.0f,   false, 1, "rpm",    0 },
            [ECU_SIG_THROTTLE]        = { 0x5F0, 16, 16, 0.00153846f, 0.0f,   false, 1, "%",      1 },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x5F0, 48, 16, 0.01f,       0.0f,   false, 1, "km/h",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x5F2,  0, 16, 0.00526316f, -45.0f, false, 1, "degC",   0 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x5F2, 16, 16, 0.00526316f, -45.0f, false, 1, "degC",   0 },
            [ECU_SIG_OIL_TEMP]        = { 0x5F2, 48, 16, 0.00526316f, -45.0f, false, 1, "degC",   0 },
            [ECU_SIG_MAP]             = { 0x5F3,  0, 16, 0.01f,       0.0f,   false, 1, "kPa",    0 },
            [ECU_SIG_OIL_PRESSURE]    = { 0x5F3, 32, 16, 0.1f,        0.0f,   false, 1, "kPa",    0 },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x5F3, 48, 16, 5.0f,        0.0f,   false, 1, "kPa",    0 },
            [ECU_SIG_BOOST]           = { 0x5F4,  0, 16, 0.01f,       0.0f,   false, 1, "kPa",    0 },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x5F4, 16, 16, 0.0003125f,  0.0f,   false, 1, "V",      1 },
            /* Gear and ignition are the only signed rows in the template. */
            [ECU_SIG_GEAR]            = { 0x5F4, 48, 16, 1.0f,        0.0f,   true,  1, "",       0 },
            [ECU_SIG_LAMBDA]          = { 0x5F6,  0, 16, 0.0005f,     0.0f,   false, 1, "lambda", 2 },
            [ECU_SIG_IGNITION]        = { 0x5FB, 32, 16, 0.00333333f, 0.0f,   true,  1, "deg",    1 },
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,  /* not in the AiM template */
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,  /* not in the AiM template */
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Toyota 86 / Subaru BRZ / Scion FR-S (2012-2020) — HS-CAN @ 500 kbps.
     *
     * VERIFIED ON HARDWARE 2026-07-25 against two Australian-market cars
     * (bus captures + live value cross-check). These rows are now kept in
     * lockstep with the hardware-proven GT86 table in preset_picker.c —
     * if you change one, change both.
     *
     * The previous rows were community-sourced and substantially wrong;
     * applying this preset actively broke a working dash, which is why the
     * two bench cars had to run hand-built custom configs. Corrected:
     *   RPM        len 16 -> 14. The top 2 bits of the byte pair are NOT
     *              rpm; at 1023 rpm a 16-bit read returns 17416.
     *   THROTTLE   0x143 -> 0x140 bit 48. 0x143 is NOT BROADCAST on these
     *              cars at all (absent from a 37-ID bus capture) — the
     *              slot was simply dead.
     *   SPEED      0x141 -> 0x0D1, scale 0.01 -> 0.05625. 0x141 bytes 0-1
     *              are a CONSTANT 0x8426 (reads 338 km/h parked). Scale is
     *              3.6/64 because the raw field is 1/64 m/s — the old
     *              1/64 scale emitted m/s labelled km/h, reading 3.6x low
     *              ("speed stops at 30" = 108 km/h). See preset_picker.c.
     *   GEAR       0x161 -> unsupported. Also NOT BROADCAST. Use the
     *              CALCULATED_GEAR internal signal (gear derived from
     *              rpm x speed + ratios) and give it a value_map so 0
     *              renders as "N".
     *   COOLANT /  were marked OBD2-only, but BOTH broadcast natively on
     *   OIL_TEMP   0x360 (bits 24 and 16). Native beats polling.
     *   YAW/LAT G  added — both live on 0x0D0.
     *
     * Also raises the auto-detect match score: the old ID set was
     * {0x140,0x141,0x143,0x161} of which two never appear (50% ceiling);
     * the new set {0x140,0x0D1,0x360,0x0D0} is all-broadcast.
     *
     * Genuinely OBD2-only on this platform (Mode 01 gap-fill): MAP 0x0B,
     * IAT 0x0F, lambda 0x44, short fuel trim 0x06, ignition timing 0x0E,
     * battery 0x42, fuel level 0x2F. Note the registry names OBD2 gives
     * those last two decode paths are SHORT_FUEL_TRIM_1 and
     * TIMING_ADVANCE, NOT FUEL_TRIM / IGNITION.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Toyota / Subaru",
        .version = "86 / BRZ",
        .display = "Toyota 86 / Subaru BRZ (2012-2020)",
        .rows = {
            [ECU_SIG_RPM]             = { 0x140, 16, 14, 1.0f,  0.0f,    false, 1, "rpm",    0 },
            [ECU_SIG_MAP]             = SIG_UNSUPPORTED,  /* OBD2 PID 0x0B */
            [ECU_SIG_THROTTLE]        = { 0x140, 48,  8, 0.39215f, 0.0f, false, 1, "%",      1 },
            [ECU_SIG_COOLANT_TEMP]    = { 0x360, 24,  8, 1.0f, -40.0f,   false, 1, "degC",   0 },
            [ECU_SIG_INTAKE_AIR_TEMP] = SIG_UNSUPPORTED,  /* OBD2 PID 0x0F */
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,  /* OBD2 PID 0x44 */
            [ECU_SIG_OIL_TEMP]        = { 0x360, 16,  8, 1.0f, -40.0f,   false, 1, "degC",   0 },
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,  /* OBD2 PID 0x0E -> TIMING_ADVANCE */
            /* SIGN: signed, matching the "Toyota"/"GT86 Gen 1" preconfig rows
             * (preset_picker.c) and their four 0x0D4 wheel-speed siblings —
             * same 1/64 m/s field format. Speed never goes negative and never
             * reaches the signed ceiling (32767/64 = 1843 km/h), so signed and
             * unsigned decode identically for every real value; signed is the
             * safer read of the 0xFFFF "not available" sentinel OEM buses send
             * (-0.06 km/h instead of 3686 km/h). The two tables MUST agree —
             * they're aliased in tools/check_preset_signedness.py. */
            [ECU_SIG_VEHICLE_SPEED]   = { 0x0D1, 0, 16, 0.05625f, 0.0f,  true,  1, "km/h",   0 },
            [ECU_SIG_GEAR]            = SIG_UNSUPPORTED,  /* not broadcast — use CALCULATED_GEAR */
            [ECU_SIG_BATTERY_VOLTAGE] = SIG_UNSUPPORTED,  /* OBD2 PID 0x42 */
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,  /* OBD2 PID 0x06 -> SHORT_FUEL_TRIM_1 */
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
            [ECU_SIG_YAW_RATE]        = { 0x0D0, 16, 16, -0.286478f, 0.0f, true, 1, "deg/s", 1 },
            [ECU_SIG_LATERAL_G]       = { 0x0D0, 48,  8, 0.2f,  0.0f,    true,  1, "g",      2 },
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

/* Parse a CSV-style value→label table ("0=N,1=1,2=2,…") into a cJSON
 * array of {v: int, label: str} objects. Returns NULL when csv is NULL
 * or empty so the caller can skip emitting an empty array. Caller owns
 * the returned cJSON; pass it via cJSON_AddItemToObject. */
static cJSON *_value_map_csv_to_json(const char *csv) {
    if (!csv || !csv[0]) return NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    const char *p = csv;
    while (*p) {
        /* skip leading whitespace/commas between entries */
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        /* parse numeric value (optionally negative, optional decimal)
         * up to '='. strtof handles ints, floats, and edge cases like
         * "+0.85" / "-1" / "1." uniformly. */
        const char *num_start = p;
        char *end = NULL;
        float v = strtof(num_start, &end);
        if (end == num_start || !end || *end != '=') {
            /* malformed entry — bail entirely rather than mis-parse */
            cJSON_Delete(arr);
            return NULL;
        }
        p = end + 1;  /* skip '=' */

        /* label runs to next ',' or end-of-string */
        const char *label_start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - label_start);
        if (len == 0) continue;  /* empty label, skip */
        if (len >= SIGNAL_VALUE_LABEL_MAX) len = SIGNAL_VALUE_LABEL_MAX - 1;
        char lbl[SIGNAL_VALUE_LABEL_MAX];
        memcpy(lbl, label_start, len);
        lbl[len] = '\0';

        cJSON *entry = cJSON_CreateObject();
        if (!entry) continue;
        cJSON_AddNumberToObject(entry, "v", (double)v);
        cJSON_AddStringToObject(entry, "label", lbl);
        cJSON_AddItemToArray(arr, entry);
    }

    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
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
    cJSON *vm = _value_map_csv_to_json(row->value_map_csv);
    if (vm) cJSON_AddItemToObject(s, "value_map", vm);
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

    /* Collect the unique FRAMES the preset expects — (can_id, mux_value), not
     * can_id alone. A multiplexed ECU (Link Generic Dash) runs its whole
     * stream through one id and separates the payloads with a frame index in
     * the message, so counting ids would score it 1-out-of-1 the instant any
     * frame appeared, and never distinguish "the ECU is there" from "one
     * unrelated device happens to use that id". */
    struct { uint32_t id; uint8_t mux_len; uint16_t mux_val; }
        preset_frames[ECU_SIG__COUNT];
    int preset_count = 0;
    for (int s = 0; s < ECU_SIG__COUNT; s++) {
        const ecu_signal_row_t *r = &preset->rows[s];
        if (r->can_id == 0) continue;  /* SIG_UNSUPPORTED */
        bool dup = false;
        for (int j = 0; j < preset_count; j++) {
            if (preset_frames[j].id == r->can_id &&
                preset_frames[j].mux_len == r->mux_bit_length &&
                preset_frames[j].mux_val == r->mux_value) { dup = true; break; }
        }
        if (!dup) {
            preset_frames[preset_count].id      = r->can_id;
            preset_frames[preset_count].mux_len = r->mux_bit_length;
            preset_frames[preset_count].mux_val = r->mux_value;
            preset_count++;
        }
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
            if (e->can_id != preset_frames[i].id || e->extended) continue;
            /* Multiplexed frame: require its specific index to have been
             * seen. mux_seen is cumulative rather than windowed, so this
             * leans on the id's own freshness check below for liveness. */
            if (preset_frames[i].mux_len) {
                if (preset_frames[i].mux_val > 15) continue;
                if (!(e->mux_seen & (uint16_t)(1u << preset_frames[i].mux_val)))
                    continue;
            }
            if (e->last_seen_us >= cutoff) { hits++; break; }
        }
    }

    /* Round-to-nearest percentage so a 1/3 split surfaces as 33 not 33.33 -> 33. */
    int score = (hits * 100 + preset_count / 2) / preset_count;
    if (score > 100) score = 100;
    return score;
}

uint32_t ecu_preset_id_span(const ecu_preset_t *preset) {
    if (!preset || preset->base_id == 0) return 0;
    /* A declared stream width wins over what the rows happen to reach — see
     * ecu_preset_t.stream_span. */
    if (preset->stream_span) return preset->stream_span;
    uint32_t span = 0;
    for (int i = 0; i < ECU_SIG__COUNT; i++) {
        uint32_t id = preset->rows[i].can_id;
        if (id == 0 || id < preset->base_id) continue;  /* unsupported slot */
        uint32_t off = id - preset->base_id;
        if (off > span) span = off;
    }
    return span;
}

esp_err_t ecu_preset_apply_to_layout(const char *layout_name,
                                     const ecu_preset_t *preset) {
    return ecu_preset_apply_to_layout_based(layout_name, preset, 0);
}

esp_err_t ecu_preset_apply_to_layout_based(const char *layout_name,
                                           const ecu_preset_t *preset,
                                           uint32_t base_id) {
    if (!layout_name || !preset) return ESP_ERR_INVALID_ARG;

    /* Re-base the stream, if this preset has a configurable base and the
     * caller asked for a different one. Only can_id moves, so the shift is
     * carried as a delta and applied per row where the signals are emitted
     * — copying the whole preset would put ~660 bytes on a 5 KB httpd
     * stack, and the const preset table must not be written to in any case
     * (several callers share it, including the auto-detect scorer). */
    int32_t id_delta = 0;
    if (preset->base_id != 0 && base_id != 0 && base_id != preset->base_id) {
        uint32_t span = ecu_preset_id_span(preset);
        if (base_id > ECU_BASE_ID_MAX || base_id + span > ECU_BASE_ID_MAX) {
            ESP_LOGE(TAG, "base id 0x%03lX (+ span %lu) runs past 0x7FF",
                     (unsigned long)base_id, (unsigned long)span);
            return ESP_ERR_INVALID_ARG;
        }
        id_delta = (int32_t)base_id - (int32_t)preset->base_id;
        ESP_LOGI(TAG, "%s %s re-based to 0x%03lX (delta %+ld)",
                 preset->make, preset->version,
                 (unsigned long)base_id, (long)id_delta);
    }

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
            /* Row copy so the shifted id never reaches the const table.
             * Unsupported slots (can_id 0) stay 0 — shifting them would
             * invent a binding the ECU does not broadcast. */
            ecu_signal_row_t row = preset->rows[i];
            if (id_delta && row.can_id)
                row.can_id = (uint32_t)((int32_t)row.can_id + id_delta);
            cJSON *s = _build_signal_json(ECU_SIGNAL_NAMES[i], &row);
            if (s) cJSON_AddItemToArray(sigs, s);
        }

        /* Preserve supplemental OBD2 gap-fillers (e.g. fuel level, ambient
         * temp) across a native-preset apply — they fill data the CAN preset
         * doesn't broadcast, and wiping them silently broke fuel-over-OBD2
         * every time the user re-applied their car's preset. The runtime
         * conflict guard in obd2_start() already skips any preserved PID whose
         * signal the new preset now owns (can_id != 0), so keeping the list is
         * safe. Only clear polled_pids when the PREVIOUS preset was OBD2-primary
         * — its full 30-PID starter set is stale under a native preset. The old
         * ecu/ecu_version fields are still in `root` here (overwritten below). */
        cJSON *prev_ecu = cJSON_GetObjectItemCaseSensitive(root, "ecu");
        cJSON *prev_ver = cJSON_GetObjectItemCaseSensitive(root, "ecu_version");
        bool prev_was_obd2 =
            cJSON_IsString(prev_ecu) && cJSON_IsString(prev_ver) &&
            strcmp(prev_ecu->valuestring, OBD2_MAKE) == 0 &&
            strcmp(prev_ver->valuestring, OBD2_VERSION) == 0;
        cJSON_DeleteItemFromObject(root, "obd2_pids");    /* legacy name — always cleared */
        if (prev_was_obd2) {
            cJSON_DeleteItemFromObject(root, "polled_pids");
        }
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

/* ── Per-slot preset application ──────────────────────────────────────
 *
 * The bulk ecu_preset_apply_to_layout rewrites every supported slot's
 * signal row to match the chosen preset. The per-slot variant below is
 * what the source picker uses when the user picks "Haltech RPM" for
 * just the rpm channel — it rewrites ONE entry in signals[] without
 * disturbing the others. The layout's overall `ecu`/`ecu_version`
 * fields are intentionally left alone (they describe the bulk preset).
 */
/* Shared helper: write a single signal row into the named layout's
 * signals[] array. Both the per-slot ECU bulk apply path AND the web
 * channels source-picker preconfig path funnel through here so the
 * cJSON allocation + disk-save story stays in ONE place.
 *
 * unit == NULL omits the unit field. decimals < 0 omits the decimals
 * field. This matters because the preconfig flow doesn't carry a unit
 * but the ECU preset flow does. */
/* Upsert one signal row into an already-parsed signals[] cJSON array.
 * Shared by the single-signal writer and the batched ecu_layout_writer so
 * the find-or-create + stale-wipe + field-set logic lives in ONE place. */
static void _upsert_signal_entry(cJSON *signals, const char *signal_name,
                                 uint32_t can_id, uint8_t bit_start,
                                 uint8_t bit_length, float scale, float offset,
                                 bool is_signed, uint8_t endian,
                                 const char *unit, int decimals) {
    if (!signals || !signal_name || !signal_name[0]) return;

    cJSON *entry = NULL;
    cJSON *sig;
    cJSON_ArrayForEach(sig, signals) {
        cJSON *jname = cJSON_GetObjectItemCaseSensitive(sig, "name");
        if (cJSON_IsString(jname) && strcmp(jname->valuestring, signal_name) == 0) {
            entry = sig; break;
        }
    }
    if (!entry) {
        entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", signal_name);
        cJSON_AddItemToArray(signals, entry);
    } else {
        /* Wipe stale decode params before re-adding. value_map is
         * also wiped since it's keyed to specific raw values; a new
         * decode means previous mappings may not match. */
        cJSON_DeleteItemFromObject(entry, "can_id");
        cJSON_DeleteItemFromObject(entry, "bit_start");
        cJSON_DeleteItemFromObject(entry, "bit_length");
        cJSON_DeleteItemFromObject(entry, "scale");
        cJSON_DeleteItemFromObject(entry, "offset");
        cJSON_DeleteItemFromObject(entry, "is_signed");
        cJSON_DeleteItemFromObject(entry, "endian");
        cJSON_DeleteItemFromObject(entry, "unit");
        cJSON_DeleteItemFromObject(entry, "decimals");
        cJSON_DeleteItemFromObject(entry, "value_map");
    }

    cJSON_AddNumberToObject(entry, "can_id",     (double)can_id);
    cJSON_AddNumberToObject(entry, "bit_start",  bit_start);
    cJSON_AddNumberToObject(entry, "bit_length", bit_length);
    cJSON_AddNumberToObject(entry, "scale",      scale);
    cJSON_AddNumberToObject(entry, "offset",     offset);
    cJSON_AddBoolToObject  (entry, "is_signed",  is_signed);
    cJSON_AddNumberToObject(entry, "endian",     endian);
    if (unit)              cJSON_AddStringToObject(entry, "unit",     unit);
    if (decimals >= 0)     cJSON_AddNumberToObject(entry, "decimals", decimals);
}

esp_err_t ecu_preset_write_signal_to_layout(const char *layout_name,
                                            const char *signal_name,
                                            uint32_t can_id,
                                            uint8_t bit_start,
                                            uint8_t bit_length,
                                            float scale, float offset,
                                            bool is_signed, uint8_t endian,
                                            const char *unit,
                                            int decimals) {
    if (!layout_name || !signal_name || !signal_name[0])
        return ESP_ERR_INVALID_ARG;

    char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = 0;
    esp_err_t err = layout_manager_read_raw(layout_name, buf,
                                            LAYOUT_MAX_FILE_BYTES, &len);
    if (err != ESP_OK) { free(buf); return err; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    /* Find or create the signals[] array, then upsert the entry. */
    cJSON *signals = cJSON_GetObjectItemCaseSensitive(root, "signals");
    if (!cJSON_IsArray(signals)) {
        signals = cJSON_AddArrayToObject(root, "signals");
    }
    _upsert_signal_entry(signals, signal_name, can_id, bit_start, bit_length,
                         scale, offset, is_signed, endian, unit, decimals);

    err = layout_manager_save_raw(layout_name, root);
    cJSON_Delete(root);
    return err;
}

/* ── Batched layout signal writer (see header) ────────────────────────── */
struct ecu_layout_writer {
    char   layout_name[64];
    cJSON *root;
    cJSON *signals;   /* borrowed pointer into root */
};

ecu_layout_writer_t *ecu_layout_writer_open(const char *layout_name) {
    if (!layout_name || !layout_name[0]) return NULL;

    char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
    if (!buf) return NULL;
    size_t len = 0;
    esp_err_t err = layout_manager_read_raw(layout_name, buf,
                                            LAYOUT_MAX_FILE_BYTES, &len);
    if (err != ESP_OK) { free(buf); return NULL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return NULL;

    ecu_layout_writer_t *w = calloc(1, sizeof(*w));
    if (!w) { cJSON_Delete(root); return NULL; }

    strncpy(w->layout_name, layout_name, sizeof(w->layout_name) - 1);
    w->root = root;
    w->signals = cJSON_GetObjectItemCaseSensitive(root, "signals");
    if (!cJSON_IsArray(w->signals))
        w->signals = cJSON_AddArrayToObject(root, "signals");
    return w;
}

void ecu_layout_writer_upsert(ecu_layout_writer_t *w,
                              const char *signal_name,
                              uint32_t can_id,
                              uint8_t bit_start, uint8_t bit_length,
                              float scale, float offset,
                              bool is_signed, uint8_t endian,
                              const char *unit, int decimals) {
    if (!w) return;
    _upsert_signal_entry(w->signals, signal_name, can_id, bit_start,
                         bit_length, scale, offset, is_signed, endian,
                         unit, decimals);
}

esp_err_t ecu_layout_writer_commit(ecu_layout_writer_t *w) {
    if (!w) return ESP_ERR_INVALID_ARG;
    esp_err_t err = layout_manager_save_raw(w->layout_name, w->root);
    cJSON_Delete(w->root);
    free(w);
    return err;
}

void ecu_layout_writer_abort(ecu_layout_writer_t *w) {
    if (!w) return;
    cJSON_Delete(w->root);
    free(w);
}

esp_err_t ecu_preset_apply_slot_to_layout(const char *layout_name,
                                          const ecu_preset_t *preset,
                                          ecu_signal_slot_t slot) {
    if (!layout_name || !preset || slot >= ECU_SIG__COUNT)
        return ESP_ERR_INVALID_ARG;

    const ecu_signal_row_t *row = &preset->rows[slot];
    if (row->can_id == 0) return ESP_ERR_INVALID_ARG;

    const char *signal_name = ECU_SIGNAL_NAMES[slot];
    if (!signal_name || !signal_name[0]) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ecu_preset_write_signal_to_layout(
        layout_name, signal_name,
        row->can_id, row->bit_start, row->bit_length,
        row->scale, row->offset,
        row->is_signed, row->endian,
        row->unit ? row->unit : "",
        row->decimals);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Slot %s ← %s %s (CAN 0x%lx) in layout '%s'",
                 signal_name, preset->make, preset->version,
                 (unsigned long)row->can_id, layout_name);
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
