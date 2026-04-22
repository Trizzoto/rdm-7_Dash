#include "ecu_presets.h"

#include "cJSON.h"
#include "esp_log.h"
#include "layout_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ecu_presets";

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

/* [0]=can_id, then bit_start, bit_length, scale, offset, is_signed, endian, unit */
#define SIG_UNSUPPORTED { 0, 0, 0, 0.0f, 0.0f, false, 0, "" }

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
            [ECU_SIG_RPM]             = { 0x600,  0, 16, 1.0f,       0.0f,   false, 1, "rpm"   },
            [ECU_SIG_MAP]             = { 0x600, 32, 16, 1.0f,       0.0f,   false, 1, "kPa"   },
            [ECU_SIG_THROTTLE]        = { 0x600, 16,  8, 0.5f,       0.0f,   false, 1, "%"     },
            [ECU_SIG_COOLANT_TEMP]    = { 0x602, 48, 16, 1.0f,       0.0f,   true,  1, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x600, 24,  8, 1.0f,       0.0f,   true,  1, "degC"  },
            [ECU_SIG_LAMBDA]          = { 0x603, 16,  8, 0.0078125f, 0.0f,   false, 1, "lambda"},
            [ECU_SIG_OIL_TEMP]        = { 0x602, 24,  8, 1.0f,       0.0f,   false, 1, "degC"  },
            [ECU_SIG_OIL_PRESSURE]    = { 0x602, 32,  8, 6.25f,      0.0f,   false, 1, "kPa"   },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x602, 40,  8, 6.25f,      0.0f,   false, 1, "kPa"   },
            [ECU_SIG_IGNITION]        = { 0x603,  0,  8, 0.5f,       0.0f,   true,  1, "deg"   },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x602,  0, 16, 1.0f,       0.0f,   false, 1, "km/h"  },
            [ECU_SIG_GEAR]            = { 0x604,  0,  8, 1.0f,       0.0f,   true,  1, ""      },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x604, 16, 16, 0.027f,     0.0f,   false, 1, "V"     },
            [ECU_SIG_FUEL_TRIM]       = { 0x603, 24,  8, 0.5f,       -100.0f,false, 1, "%"     },
            [ECU_SIG_EGT]             = { 0x603, 32, 16, 1.0f,       0.0f,   false, 1, "degC"  },
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
            [ECU_SIG_RPM]             = { 0x5F0, 48, 16, 1.0f,       0.0f,      false, 0, "rpm"   },
            [ECU_SIG_MAP]             = { 0x5F2, 16, 16, 0.1f,       0.0f,      true,  0, "kPa"   },
            [ECU_SIG_THROTTLE]        = { 0x5F3,  0, 16, 0.1f,       0.0f,      true,  0, "%"     },
            /* degF->degC: scale = 0.1 * 5/9, offset = -32 * 5/9 */
            [ECU_SIG_COOLANT_TEMP]    = { 0x5F2, 48, 16, 0.0555556f, -17.7778f, true,  0, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x5F2, 32, 16, 0.0555556f, -17.7778f, true,  0, "degC"  },
            /* AFR->Lambda: scale = 0.1 / 14.7 */
            [ECU_SIG_LAMBDA]          = { 0x5FF,  0,  8, 0.0068027f, 0.0f,      false, 0, "lambda"},
            [ECU_SIG_OIL_TEMP]        = SIG_UNSUPPORTED,  /* generic ADC only */
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,  /* generic ADC only */
            [ECU_SIG_FUEL_PRESSURE]   = { 0x615,  0, 16, 0.1f,       0.0f,      true,  0, "kPa"   },
            [ECU_SIG_IGNITION]        = { 0x5F1,  0, 16, 0.1f,       0.0f,      true,  0, "deg"   },
            /* m/s->km/h: scale = 0.1 * 3.6 */
            [ECU_SIG_VEHICLE_SPEED]   = { 0x612,  0, 16, 0.36f,      0.0f,      false, 0, "km/h"  },
            [ECU_SIG_GEAR]            = { 0x611, 48,  8, 1.0f,       0.0f,      true,  0, ""      },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x5F3, 16, 16, 0.1f,       0.0f,      true,  0, "V"     },
            /* egocor1 - 1/10 % centered on 100 */
            [ECU_SIG_FUEL_TRIM]       = { 0x5F4, 16, 16, 0.1f,       -100.0f,   true,  0, "%"     },
            [ECU_SIG_EGT]             = { 0x606,  0, 16, 0.0555556f, -17.7778f, true,  0, "degC"  },
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
            [ECU_SIG_RPM]             = { 0x360,  0, 16, 1.0f,     0.0f,     false, 0, "rpm"   },
            /* Manifold pressure: raw*0.1 = kPa absolute -> gauge via -101.325 */
            [ECU_SIG_MAP]             = { 0x360, 16, 16, 0.1f,     0.0f,     false, 0, "kPa"   },
            [ECU_SIG_THROTTLE]        = { 0x360, 32, 16, 0.1f,     0.0f,     false, 0, "%"     },
            [ECU_SIG_COOLANT_TEMP]    = { 0x3E0,  0, 16, 0.1f,     -273.15f, false, 0, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x3E0, 16, 16, 0.1f,     -273.15f, false, 0, "degC"  },
            /* Wideband 1 lambda: raw*0.001 directly */
            [ECU_SIG_LAMBDA]          = { 0x368,  0, 16, 0.001f,   0.0f,     false, 0, "lambda"},
            [ECU_SIG_OIL_TEMP]        = { 0x3E0, 48, 16, 0.1f,     -273.15f, false, 0, "degC"  },
            [ECU_SIG_OIL_PRESSURE]    = { 0x361, 16, 16, 0.1f,     -101.325f,false, 0, "kPa"   },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x361,  0, 16, 0.1f,     -101.325f,false, 0, "kPa"   },
            [ECU_SIG_IGNITION]        = { 0x362, 32, 16, 0.1f,     0.0f,     true,  0, "deg"   },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x370,  0, 16, 0.1f,     0.0f,     false, 0, "km/h"  },
            [ECU_SIG_GEAR]            = { 0x360, 48, 16, 1.0f,     0.0f,     false, 0, ""      },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x372,  0, 16, 0.1f,     0.0f,     false, 0, "V"     },
            [ECU_SIG_FUEL_TRIM]       = { 0x3E3,  0, 16, 0.1f,     0.0f,     true,  0, "%"     },
            [ECU_SIG_EGT]             = { 0x373,  0, 16, 0.1f,     -273.15f, false, 0, "degC"  },
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
            [ECU_SIG_RPM]             = { 0x520,  0, 16, 1.0f,       0.0f,     false, 1, "rpm"   },
            [ECU_SIG_MAP]             = { 0x520, 32, 16, 0.1f,       0.0f,     false, 1, "kPa"   },
            [ECU_SIG_THROTTLE]        = { 0x520, 16, 16, 0.1f,       0.0f,     false, 1, "%"     },
            [ECU_SIG_COOLANT_TEMP]    = { 0x530, 48, 16, 0.1f,       0.0f,     true,  1, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x530, 32, 16, 0.1f,       0.0f,     true,  1, "degC"  },
            [ECU_SIG_LAMBDA]          = { 0x520, 48, 16, 0.001f,     0.0f,     false, 1, "lambda"},
            [ECU_SIG_OIL_TEMP]        = SIG_UNSUPPORTED,  /* not in 1.2 */
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = { 0x521, 32, 16, 0.1f,       0.0f,     true,  1, "deg"   },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x522, 48, 16, 0.1f,       0.0f,     false, 1, "km/h"  },
            [ECU_SIG_GEAR]            = { 0x536,  0, 16, 1.0f,       0.0f,     false, 1, ""      },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x530,  0, 16, 0.01f,      0.0f,     false, 1, "V"     },
            [ECU_SIG_FUEL_TRIM]       = { 0x531,  0, 16, 0.1f,       0.0f,     true,  1, "%"     },
            [ECU_SIG_EGT]             = { 0x533, 48, 16, 1.0f,       0.0f,     false, 1, "degC"  },
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Ford BA/BF - factory CAN, sparse broadcast
     * Source: existing preconfig_items[] entries.
     * Most core sensors not on CAN (factory PCM doesn't broadcast them).
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Ford",
        .version = "BA/BF",
        .display = "Ford Falcon BA / BF",
        .rows = {
            [ECU_SIG_RPM]             = { 0x201,  0, 16, 0.25f,    0.0f,   false, 1, "rpm"   },
            [ECU_SIG_MAP]             = SIG_UNSUPPORTED,
            [ECU_SIG_THROTTLE]        = { 0x201, 16, 16, 0.00152f, 0.0f,   false, 1, "%"     },
            [ECU_SIG_COOLANT_TEMP]    = { 0x420,  0,  8, 1.0f,     -40.0f, false, 1, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = SIG_UNSUPPORTED,
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_TEMP]        = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,
            [ECU_SIG_VEHICLE_SPEED]   = { 0x415,  0, 16, 0.01f,    0.0f,   false, 1, "km/h"  },
            [ECU_SIG_GEAR]            = SIG_UNSUPPORTED,
            [ECU_SIG_BATTERY_VOLTAGE] = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
        },
    },

    /* ══════════════════════════════════════════════════════════════════
     * Ford FG - factory CAN, richer broadcast than BA/BF.
     * Source: existing preconfig_items[] entries.
     * ══════════════════════════════════════════════════════════════════ */
    {
        .make = "Ford",
        .version = "FG",
        .display = "Ford Falcon FG",
        .rows = {
            [ECU_SIG_RPM]             = { 0x109,  0, 16, 0.25f,    0.0f,   false, 1, "rpm"   },
            [ECU_SIG_MAP]             = SIG_UNSUPPORTED,
            [ECU_SIG_THROTTLE]        = { 0x204,  0, 16, 0.01f,    0.0f,   false, 1, "%"     },
            [ECU_SIG_COOLANT_TEMP]    = { 0x156,  0,  8, 1.0f,     -60.0f, false, 1, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = SIG_UNSUPPORTED,
            [ECU_SIG_LAMBDA]          = SIG_UNSUPPORTED,
            [ECU_SIG_OIL_TEMP]        = { 0x156,  8,  8, 1.0f,     -60.0f, false, 1, "degC"  },
            [ECU_SIG_OIL_PRESSURE]    = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_PRESSURE]   = SIG_UNSUPPORTED,
            [ECU_SIG_IGNITION]        = SIG_UNSUPPORTED,
            [ECU_SIG_VEHICLE_SPEED]   = { 0x109, 32, 16, 0.01f,    0.0f,   false, 1, "km/h"  },
            [ECU_SIG_GEAR]            = { 0x171,  0,  8, 1.0f,     0.0f,   false, 1, ""      },
            [ECU_SIG_BATTERY_VOLTAGE] = SIG_UNSUPPORTED,
            [ECU_SIG_FUEL_TRIM]       = SIG_UNSUPPORTED,
            [ECU_SIG_EGT]             = SIG_UNSUPPORTED,
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
            [ECU_SIG_RPM]             = { 0x520,  0, 16, 1.0f,       0.0f,     false, 1, "rpm"   },
            [ECU_SIG_MAP]             = { 0x520, 32, 16, 0.1f,       0.0f,     false, 1, "kPa"   },
            [ECU_SIG_THROTTLE]        = { 0x520, 16, 16, 0.1f,       0.0f,     false, 1, "%"     },
            [ECU_SIG_COOLANT_TEMP]    = { 0x530, 48, 16, 0.1f,       0.0f,     true,  1, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x530, 32, 16, 0.1f,       0.0f,     true,  1, "degC"  },
            [ECU_SIG_LAMBDA]          = { 0x520, 48, 16, 0.001f,     0.0f,     false, 1, "lambda"},
            [ECU_SIG_OIL_TEMP]        = { 0x536, 48, 16, 0.1f,       0.0f,     true,  1, "degC"  },
            [ECU_SIG_OIL_PRESSURE]    = { 0x536, 32, 16, 0.1f,       0.0f,     false, 1, "kPa"   },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x537,  0, 16, 0.1f,       0.0f,     false, 1, "kPa"   },
            [ECU_SIG_IGNITION]        = { 0x521, 32, 16, 0.1f,       0.0f,     true,  1, "deg"   },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x522, 48, 16, 0.1f,       0.0f,     false, 1, "km/h"  },
            [ECU_SIG_GEAR]            = { 0x536,  0, 16, 1.0f,       0.0f,     false, 1, ""      },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x530,  0, 16, 0.01f,      0.0f,     false, 1, "V"     },
            [ECU_SIG_FUEL_TRIM]       = { 0x531,  0, 16, 0.1f,       0.0f,     true,  1, "%"     },
            [ECU_SIG_EGT]             = { 0x533, 48, 16, 1.0f,       0.0f,     false, 1, "degC"  },
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
            [ECU_SIG_RPM]             = { 0x3E8, 16, 16, 1.0f,  0.0f,     false, 1, "rpm"   },
            [ECU_SIG_MAP]             = { 0x3E8, 32, 16, 1.0f,  0.0f,     false, 1, "kPa"   },
            [ECU_SIG_THROTTLE]        = { 0x3E9, 32, 16, 0.1f,  0.0f,     false, 1, "%"     },
            [ECU_SIG_COOLANT_TEMP]    = { 0x3EA, 48, 16, 1.0f,  -50.0f,   false, 1, "degC"  },
            [ECU_SIG_INTAKE_AIR_TEMP] = { 0x3EB, 16, 16, 1.0f,  -50.0f,   false, 1, "degC"  },
            [ECU_SIG_LAMBDA]          = { 0x3EE, 32, 16, 0.001f,0.0f,     false, 1, "lambda"},
            [ECU_SIG_OIL_TEMP]        = { 0x3F0, 16, 16, 1.0f,  -50.0f,   false, 1, "degC"  },
            [ECU_SIG_OIL_PRESSURE]    = { 0x3F0, 32, 16, 1.0f,  0.0f,     false, 1, "kPa"   },
            [ECU_SIG_FUEL_PRESSURE]   = { 0x3EF, 48, 16, 1.0f,  0.0f,     false, 1, "kPa"   },
            [ECU_SIG_IGNITION]        = { 0x3EC, 48, 16, 0.1f,  -100.0f,  true,  1, "deg"   },
            [ECU_SIG_VEHICLE_SPEED]   = { 0x3F0, 48, 16, 0.1f,  0.0f,     false, 1, "km/h"  },
            [ECU_SIG_GEAR]            = { 0x3EC, 16, 16, 1.0f,  0.0f,     false, 1, ""      },
            [ECU_SIG_BATTERY_VOLTAGE] = { 0x3EB, 32, 16, 0.01f, 0.0f,     false, 1, "V"     },
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

    if (!is_marker_preset) {
        /* Replace signals array. */
        cJSON_DeleteItemFromObject(root, "signals");
        cJSON *sigs = cJSON_AddArrayToObject(root, "signals");
        for (int i = 0; i < ECU_SIG__COUNT; i++) {
            cJSON *s = _build_signal_json(ECU_SIGNAL_NAMES[i], &preset->rows[i]);
            if (s) cJSON_AddItemToArray(sigs, s);
        }
    }

    /* Update ecu make/version fields. */
    cJSON_DeleteItemFromObject(root, "ecu");
    cJSON_DeleteItemFromObject(root, "ecu_version");
    cJSON_AddStringToObject(root, "ecu", preset->make);
    cJSON_AddStringToObject(root, "ecu_version", preset->version);

    /* Write back. */
    err = layout_manager_save_raw(layout_name, root);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Applied preset '%s %s' to layout '%s'",
                 preset->make, preset->version, layout_name);
    }
    return err;
}
