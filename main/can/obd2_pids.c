/**
 * obd2_pids.c — SAE J1979 standard PID decode table.
 *
 * Every decode happens to be linear (scale + offset), so the table is data,
 * not function pointers. Bytes=1 PIDs use A directly; bytes=2 use A*256+B.
 *
 * Source: ISO 15031-5 / SAE J1979 (the canonical OBD2 spec).
 *
 * default_enabled=true rows form the 30-PID "starter set" used when the
 * user picks the OBD2 Standard preset. suggested_filler=true rows are the
 * gap-fillers proposed when running OBD2 alongside a native ECU preset
 * (signals the standalone engine ECU typically doesn't broadcast).
 */
#include "obd2.h"

#include <stddef.h>

#define FAST OBD2_TIER_FAST
#define SLOW OBD2_TIER_SLOW

/* Silence -Wmissing-field-initializers across the table — positional
 * initializers omit `service`, `category`, `sub_fields`, `sub_field_count`,
 * and `request_id` which C zero-inits. service=0 is translated to 0x01
 * (Mode 01) by obd2_def_service(); the rest correctly default to absent. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

const obd2_pid_def_t OBD2_PIDS[] = {
    /* PID,  signal_name,             human_name,                unit,    bytes, scale,        offset,  tier, default, filler */
    { 0x04, "ENGINE_LOAD",            "Calculated Engine Load",  "%",     1, 0.392157f,   0.0f,    FAST, true,  false },
    { 0x05, "COOLANT_TEMP",           "Coolant Temperature",     "degC",  1, 1.0f,        -40.0f,  SLOW, true,  false },
    { 0x06, "SHORT_FUEL_TRIM_1",      "Short Fuel Trim B1",      "%",     1, 0.78125f,    -100.0f, SLOW, true,  false },
    { 0x07, "LONG_FUEL_TRIM_1",       "Long Fuel Trim B1",       "%",     1, 0.78125f,    -100.0f, SLOW, true,  false },
    { 0x0A, "FUEL_PRESSURE",          "Fuel Pressure (kPa abs)", "kPa",   1, 3.0f,        0.0f,    SLOW, true,  false },
    { 0x0B, "MAP",                    "Intake Manifold Pressure","kPa",   1, 1.0f,        0.0f,    FAST, true,  false },
    { 0x0C, "RPM",                    "Engine RPM",              "rpm",   2, 0.25f,       0.0f,    FAST, true,  false },
    { 0x0D, "VEHICLE_SPEED",          "Vehicle Speed",           "km/h",  1, 1.0f,        0.0f,    FAST, true,  false },
    { 0x0E, "TIMING_ADVANCE",         "Timing Advance",          "deg",   1, 0.5f,        -64.0f,  FAST, true,  false },
    { 0x0F, "INTAKE_AIR_TEMP",        "Intake Air Temperature",  "degC",  1, 1.0f,        -40.0f,  SLOW, true,  false },
    { 0x10, "MAF",                    "Mass Airflow Rate",       "g/s",   2, 0.01f,       0.0f,    FAST, true,  false },
    /* PID 0x11 is the *raw* throttle position sensor — on most cars it
     * reads ~12-15% at idle because the throttle plate is never fully
     * closed and the sensor isn't calibrated to "0 at idle". Users see
     * a "wrong" throttle %. We expose it as THROTTLE_ABS for diagnostics
     * but default to PID 0x45 (relative) for the friendly "THROTTLE"
     * signal that widgets actually want. */
    { 0x11, "THROTTLE_ABS",           "Throttle Sensor (raw)",   "%",     1, 0.392157f,   0.0f,    FAST, false, false },
    { 0x14, "O2_B1S1",                "O2 Sensor B1S1 Voltage",  "V",     1, 0.005f,      0.0f,    SLOW, true,  false },
    { 0x1F, "RUN_TIME",               "Run Time Since Engine On","s",     2, 1.0f,        0.0f,    SLOW, true,  false },
    { 0x21, "DTC_DISTANCE",           "Distance with MIL on",    "km",    2, 1.0f,        0.0f,    SLOW, true,  false },
    { 0x23, "FUEL_RAIL_PRESSURE",     "Fuel Rail Pressure",      "kPa",   2, 10.0f,       0.0f,    SLOW, true,  false },
    { 0x2C, "EGR_CMD",                "Commanded EGR",           "%",     1, 0.392157f,   0.0f,    SLOW, true,  false },
    { 0x2D, "EGR_ERROR",              "EGR Error",               "%",     1, 0.78125f,    -100.0f, SLOW, false, false },
    { 0x2F, "FUEL_LEVEL",             "Fuel Level",              "%",     1, 0.392157f,   0.0f,    SLOW, true,  true  },
    { 0x31, "DISTANCE_SINCE_CLEAR",   "Distance Since Codes Cleared", "km", 2, 1.0f,     0.0f,    SLOW, true,  true  },
    { 0x33, "BAROMETRIC_PRESSURE",    "Barometric Pressure",     "kPa",   1, 1.0f,        0.0f,    SLOW, true,  false },
    { 0x42, "BATTERY_VOLTAGE",        "Control Module Voltage",  "V",     2, 0.001f,      0.0f,    SLOW, true,  false },
    { 0x43, "ABSOLUTE_LOAD",          "Absolute Engine Load",    "%",     2, 0.392157f,   0.0f,    FAST, true,  false },
    /* Lambda (commanded equivalence ratio) raw is unitless 0..2 in steps of 2/65535.
     * Direct lambda output, no further conversion needed. */
    { 0x44, "LAMBDA",                 "Commanded Equivalence Ratio", "lambda", 2, 0.0000305f, 0.0f, FAST, true,  false },
    /* PID 0x45 is the *calibrated* throttle position — 0% at idle, 100%
     * at full pedal. This is what every OBD2 dash actually wants to show.
     * Owns the canonical "THROTTLE" signal name so existing widgets bind
     * to it without further config. */
    { 0x45, "THROTTLE",               "Throttle",                "%",     1, 0.392157f,   0.0f,    FAST, true,  false },
    { 0x46, "AMBIENT_TEMP",           "Ambient Air Temperature", "degC",  1, 1.0f,        -40.0f,  SLOW, true,  true  },
    { 0x4C, "THROTTLE_CMD",           "Commanded Throttle Actuator","%",  1, 0.392157f,   0.0f,    FAST, true,  false },
    { 0x52, "ETHANOL_PCT",            "Ethanol Fuel %",          "%",     1, 0.392157f,   0.0f,    SLOW, true,  false },
    { 0x5A, "PEDAL_POSITION",         "Accelerator Pedal Position","%",   1, 0.392157f,   0.0f,    FAST, true,  false },
    { 0x5C, "OIL_TEMP",               "Engine Oil Temperature",  "degC",  1, 1.0f,        -40.0f,  SLOW, true,  true  },
    { 0x5E, "FUEL_RATE",              "Engine Fuel Rate",        "L/h",   2, 0.05f,       0.0f,    SLOW, true,  true  },

    /* ── Additional PIDs (not in the starter set; surfaced by discovery) ── */
    { 0x03, "FUEL_SYSTEM_STATUS",     "Fuel System Status",      "",      1, 1.0f,        0.0f,    SLOW, false, false },
    { 0x09, "LONG_FUEL_TRIM_2",       "Long Fuel Trim B2",       "%",     1, 0.78125f,    -100.0f, SLOW, false, false },
    { 0x08, "SHORT_FUEL_TRIM_2",      "Short Fuel Trim B2",      "%",     1, 0.78125f,    -100.0f, SLOW, false, false },
    { 0x22, "FUEL_RAIL_REL_PRESSURE", "Fuel Rail Pressure (rel)","kPa",   2, 0.079f,      0.0f,    SLOW, false, false },
    { 0x32, "EVAP_VAPOR_PRESSURE",    "Evap Vapor Pressure",     "Pa",    2, 0.25f,       -8192.0f,SLOW, false, false },
    { 0x34, "O2S1_LAMBDA",            "O2 Sensor 1 Lambda",      "lambda",2, 0.0000305f,  0.0f,    SLOW, false, false },
    { 0x47, "ABS_THROTTLE_B",         "Absolute Throttle Pos B", "%",     1, 0.392157f,   0.0f,    SLOW, false, false },
    { 0x48, "ABS_THROTTLE_C",         "Absolute Throttle Pos C", "%",     1, 0.392157f,   0.0f,    SLOW, false, false },
    { 0x49, "ACCEL_PEDAL_D",          "Accelerator Pedal D",     "%",     1, 0.392157f,   0.0f,    SLOW, false, false },
    { 0x4A, "ACCEL_PEDAL_E",          "Accelerator Pedal E",     "%",     1, 0.392157f,   0.0f,    SLOW, false, false },
    { 0x4B, "ACCEL_PEDAL_F",          "Accelerator Pedal F",     "%",     1, 0.392157f,   0.0f,    SLOW, false, false },
    { 0x5B, "HYBRID_BATTERY_PCT",     "Hybrid Battery Pack %",   "%",     1, 0.392157f,   0.0f,    SLOW, false, false },
    { 0x5D, "FUEL_INJECTION_TIMING",  "Fuel Injection Timing",   "deg",   2, 0.0078125f,  -210.0f, SLOW, false, false },
    { 0x61, "ENGINE_TORQUE_DEMAND",   "Engine Torque Demand",    "%",     1, 1.0f,        -125.0f, SLOW, false, false },
    { 0x62, "ENGINE_TORQUE_ACT",      "Actual Engine Torque",    "%",     1, 1.0f,        -125.0f, SLOW, false, false },
    { 0x63, "ENGINE_REF_TORQUE",      "Engine Reference Torque", "Nm",    2, 1.0f,        0.0f,    SLOW, false, false },

    /* ── Diagnostic counters (J1979 standard, low-rate) ───────────────
     *
     * These are SLOW-tier engine-life / MIL-status counters that don't
     * change second-to-second but make great "engine health" widgets.
     * All single-value, A or A*256+B per spec — no support byte to skip. */
    { 0x30, "WARMUPS_SINCE_CLEAR",    "Warmups Since Codes Cleared","count", 1, 1.0f,    0.0f,    SLOW, false, false },
    { 0x4D, "MIL_ON_TIME",            "Time with MIL On",        "min",   2, 1.0f,        0.0f,    SLOW, false, false },
    { 0x4E, "RUN_TIME_SINCE_CLEAR",   "Run Time Since DTCs Cleared","min", 2, 1.0f,      0.0f,    SLOW, false, false },

    /* ── Diesel-specific (SAE J1979-DA) ────────────────────────────────
     *
     * Confirmed on a 2024 Toyota HiAce diesel (Mode 01 PIDs 0x77, 0x78,
     * 0x7A reported supported via discovery scan, raw bytes captured via
     * the picker's "Dump Unknowns" button).
     *
     * Each of these PIDs carries a leading "support" byte indicating
     * which downstream sensor slots are populated, then the data slots.
     * We use the packed (sub_fields) decode path with byte_offset=1+ to
     * skip the support byte. Unsupported sensors usually return zero,
     * which decodes as the floor (e.g. -40 °C for an EGT slot). */

    /* PID 0x77 — Charge Air Cooler Temperature (intercooler outlet).
     * Byte 1 = T1A (charge air cooler 1, sensor A), simple temp - 40 °C. */
    {
        .pid = 0x77,
        .signal_name = NULL,
        .human_name = "Charge Air Cooler Temp",
        .unit = "",
        .bytes = 0, .scale = 0, .offset = 0,
        .tier = OBD2_TIER_SLOW,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x01,
        .category = NULL,
        .sub_fields = (const obd2_subfield_t[]){
            { "CHARGE_AIR_TEMP", "degC", 1, 1, false, 1.0f, -40.0f },
        },
        .sub_field_count = 1,
        .request_id = 0,
    },

    /* PID 0x78 — Exhaust Gas Temperature, Bank 1 (up to 4 sensors,
     * each 2 bytes, formula (A*256+B)/10 - 40 °C). On modern diesels
     * EGT1 is usually pre-turbo, EGT2 post-turbo, EGT3 pre-DPF, EGT4
     * post-DPF — exact mapping varies by manufacturer. */
    {
        .pid = 0x78,
        .signal_name = NULL,
        .human_name = "EGT Bank 1 (4 sensors)",
        .unit = "",
        .bytes = 0, .scale = 0, .offset = 0,
        .tier = OBD2_TIER_FAST,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x01,
        .category = NULL,
        .sub_fields = (const obd2_subfield_t[]){
            { "EGT_1", "degC", 1, 2, false, 0.1f, -40.0f },
            { "EGT_2", "degC", 3, 2, false, 0.1f, -40.0f },
            { "EGT_3", "degC", 5, 2, false, 0.1f, -40.0f },
            { "EGT_4", "degC", 7, 2, false, 0.1f, -40.0f },
        },
        .sub_field_count = 4,
        .request_id = 0,
    },

    /* PID 0x7A — DPF differential pressure (across the DPF).
     * Bytes 1,2 signed 16-bit, scale 0.01 kPa. Clean filter reads ~0,
     * climbs to ~5-20 kPa as soot loads up, triggers regen at threshold. */
    {
        .pid = 0x7A,
        .signal_name = NULL,
        .human_name = "DPF Differential Pressure",
        .unit = "",
        .bytes = 0, .scale = 0, .offset = 0,
        .tier = OBD2_TIER_SLOW,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x01,
        .category = NULL,
        .sub_fields = (const obd2_subfield_t[]){
            { "DPF_DELTA_P", "kPa", 1, 2, true, 0.01f, 0.0f },
        },
        .sub_field_count = 1,
        .request_id = 0,
    },

    /* ── Tier 3: Diesel-specific, EXPERIMENTAL decodes ─────────────────
     *
     * These PIDs have multiple competing formulas in different revisions
     * of SAE J1979 / J1979-DA. The decodes below are the MOST CITED
     * interpretations and produce plausible values against a 2024
     * Toyota HiAce 1GD-FTV dump. Each PID exposes only the first
     * commanded / actual slot — the spec defines additional cmd_B and
     * act_B fields for twin-turbo / dual-rail systems, but for a
     * single-turbo single-rail diesel those extra slots return zero
     * and just create widget noise. Users with twin-turbo / dual-rail
     * setups can decode the remaining fields via Custom PIDs in the
     * web editor (Service 01, same PID byte, different byte_offset). */

    /* PID 0x70 — Boost Pressure Control.
     * Bytes 1,2 = pressure value, scale 0.03125 kPa per count.
     * Spec interpretation varies between "commanded" and "actual" by
     * model — most cars use slot A for actual boost (absolute manifold
     * pressure including atmospheric). A reading of ~100 kPa at idle
     * confirms absolute (= atm); ~0 kPa at idle would mean gauge.
     * Range with 0.03125 scale: 0..~2 MPa, plenty for diesel boost. */
    {
        .pid = 0x70,
        .signal_name = NULL,
        .human_name = "Boost Pressure (experimental)",
        .unit = "",
        .bytes = 0, .scale = 0, .offset = 0,
        .tier = OBD2_TIER_FAST,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x01,
        .category = NULL,
        .sub_fields = (const obd2_subfield_t[]){
            { "BOOST_PRESSURE_ABS", "kPa", 1, 2, false, 0.03125f, 0.0f },
        },
        .sub_field_count = 1,
        .request_id = 0,
    },

    /* PID 0x6D — Fuel Pressure Control System (Diesel common rail).
     * Bytes 1,2 = commanded rail pressure, scale 10 kPa per count.
     * Typical diesel hot-idle setpoint is 25-40 MPa; cranking is ~30 MPa;
     * heavy load can hit 200+ MPa on modern injection systems.
     * Distinct from PID 0x23 (FUEL_RAIL_PRESSURE) which is a simpler
     * single-value PID on petrol/older diesel. */
    {
        .pid = 0x6D,
        .signal_name = NULL,
        .human_name = "Commanded Fuel Rail Pressure (experimental)",
        .unit = "",
        .bytes = 0, .scale = 0, .offset = 0,
        .tier = OBD2_TIER_FAST,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x01,
        .category = NULL,
        .sub_fields = (const obd2_subfield_t[]){
            { "RAIL_PRESSURE_CMD", "kPa", 1, 2, false, 10.0f, 0.0f },
        },
        .sub_field_count = 1,
        .request_id = 0,
    },

    /* PID 0x71 — Variable Geometry Turbo (VGT) actuator position.
     * Byte 1 single-byte, scale 100/255 ≈ 0.392157 (% per count).
     * 0% = vanes fully open (low boost / coast), 100% = fully closed
     * (max boost). Typical idle: 20-30%; full-load acceleration: 70-95%.
     * Note: some manufacturers invert the convention (0% = closed); if
     * the value goes DOWN under acceleration, your ECU uses inverted. */
    {
        .pid = 0x71,
        .signal_name = NULL,
        .human_name = "VGT Position (experimental)",
        .unit = "",
        .bytes = 0, .scale = 0, .offset = 0,
        .tier = OBD2_TIER_FAST,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x01,
        .category = NULL,
        .sub_fields = (const obd2_subfield_t[]){
            { "VGT_POSITION", "%", 1, 1, false, 0.392157f, 0.0f },
        },
        .sub_field_count = 1,
        .request_id = 0,
    },

    /* ── Toyota Mode 21 (PID 0x80 = engine real-time block) ───────────
     *
     * This is the most commonly-implemented Toyota Mode 21 PID across
     * ETC-i powertrains (modern I4/V6 petrol). The response is a single
     * ISO-TP multi-frame message (~20-28 bytes) packing every key live
     * value into one round-trip — substantially faster than polling
     * each value individually via Mode 01.
     *
     * The byte layout below is a best-effort decode derived from
     * cross-referenced forum / Techstream sources for modern Toyota
     * petrol engines (2TR-FE / 2AR-FE / 2GR-FE class). It is
     * EXPERIMENTAL — the layout varies by model and ECU version, so
     * the user should verify by enabling on their vehicle and
     * sanity-checking values (rev the engine, watch RPM track).
     *
     * Signals use the canonical Mode 01 names (RPM, COOLANT_TEMP, etc.)
     * rather than a TY_-prefixed namespace. If the user has both this
     * PID AND Mode 01 PID 0x0C (RPM) enabled at once, both write to the
     * same "RPM" registry slot — last-write-wins. In practice users
     * enable Toyota's bundle OR Mode 01 individuals, not both, because
     * the preset picker exposes them under separate brands (Toyota /
     * Mode 21 vs OBD2 / Standard).
     *
     * Addresses the engine ECU directly on 0x7E0 (response 0x7E8) to
     * avoid the transmission/hybrid ECUs returning NRCs on broadcast. */
    {
        .pid = 0x80,
        .signal_name = NULL,               /* packed — see sub_fields */
        .human_name = "Toyota Engine Block (experimental)",
        .unit = "",
        .bytes = 0, .scale = 0, .offset = 0,
        .tier = OBD2_TIER_FAST,
        .default_enabled = false,          /* opt-in: avoid surprises on non-Toyota cars */
        .suggested_filler = false,
        .service = 0x21,
        .category = "Toyota",
        .sub_fields = (const obd2_subfield_t[]){
            { "RPM",              "rpm",   2,  2, false, 0.25f,       0.0f   },
            { "THROTTLE",         "%",     4,  1, false, 0.392157f,   0.0f   },
            { "VEHICLE_SPEED",    "km/h",  5,  1, false, 1.0f,        0.0f   },
            { "COOLANT_TEMP",     "degC",  6,  1, false, 1.0f,        -40.0f },
            { "INTAKE_AIR_TEMP",  "degC",  7,  1, false, 1.0f,        -40.0f },
            { "MAF",              "g/s",   8,  2, false, 0.01f,       0.0f   },
            { "BATTERY_VOLTAGE",  "V",    12,  1, false, 0.1f,        0.0f   },
        },
        .sub_field_count = 7,
        .request_id = 0x7E0u,
    },

    /* ── Toyota Mode 21 PID 0x21 — transmission ATF temperature ───────
     *
     * Single-value response carrying ATF (auto trans fluid) temperature
     * on many Toyota AT models — Hilux, HiAce, Tundra, 4Runner, Tacoma,
     * many petrol Camrys, etc. Format: byte 0 = temp - 40 °C. The TCM
     * answers on 0x7E9, so addressed request to 0x7E1.
     *
     * Coexists with Mode 01 PID 0x21 (DTC_DISTANCE) safely thanks to
     * service-aware lookup — they're distinct entries.
     *
     * EXPERIMENTAL — layout varies; some models put gear position in
     * byte 1 and TCM mode in byte 2. We only decode byte 0 for now. */
    {
        .pid = 0x21,
        .signal_name = "ATF_TEMP",
        .human_name = "Toyota ATF Temperature (experimental)",
        .unit = "degC",
        .bytes = 1,
        .scale = 1.0f,
        .offset = -40.0f,
        .tier = OBD2_TIER_SLOW,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x21,
        .category = "Toyota",
        .sub_fields = NULL,
        .sub_field_count = 0,
        .request_id = 0x7E1u,    /* TCM */
    },

    /* ── Toyota Mode 21 PID 0xA8 — knock retard angle ─────────────────
     *
     * Knock-correction ignition retard. Widely consistent across Toyota
     * engine families (1GD-FTV diesel, 2TR-FE / 2AR-FE / 2GR-FE petrol)
     * because it's a single self-contained value the ECU exposes via
     * a standard slot. Value is byte 0 in 0.5° increments — typically
     * 0° at idle/cruise, increasing under knock conditions.
     *
     * EXPERIMENTAL — confirm on your vehicle by enabling, then watch
     * the value: should stay near 0 at steady throttle, may climb to
     * 2-5° under aggressive acceleration on hot day / cheap fuel.
     * Bytes 1-7 (if present) often carry per-cylinder retard or knock
     * counts; not decoded here. */
    {
        .pid = 0xA8,
        .signal_name = "KNOCK_RETARD",
        .human_name = "Toyota Knock Retard (experimental)",
        .unit = "deg",
        .bytes = 1,
        .scale = 0.5f,
        .offset = 0.0f,
        .tier = OBD2_TIER_FAST,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x21,
        .category = "Toyota",
        .sub_fields = NULL,
        .sub_field_count = 0,
        .request_id = 0x7E0u,    /* engine ECU */
    },

    /* ── Toyota Mode 21 PID 0xC1 — accelerator pedal sensor 1 raw ────
     *
     * Drive-by-wire pedal position from sensor 1 (most Toyota DBW
     * engines have a redundant sensor 2 on a different PID). Single
     * byte, 0.5% per count → 0% pedal off, ~100% pedal floored.
     *
     * EXPERIMENTAL — on some hybrids and diesels the layout differs
     * (raw ADC counts, signed offset, etc.). The "Test" button in the
     * Custom PIDs editor will show whether the decoded value tracks
     * your pedal position. */
    {
        .pid = 0xC1,
        .signal_name = "PEDAL_RAW",
        .human_name = "Toyota Accel Pedal Raw (experimental)",
        .unit = "%",
        .bytes = 1,
        .scale = 0.5f,
        .offset = 0.0f,
        .tier = OBD2_TIER_FAST,
        .default_enabled = false,
        .suggested_filler = false,
        .service = 0x21,
        .category = "Toyota",
        .sub_fields = NULL,
        .sub_field_count = 0,
        .request_id = 0x7E0u,
    },
};

#pragma GCC diagnostic pop

const int OBD2_PIDS_COUNT = (int)(sizeof(OBD2_PIDS) / sizeof(OBD2_PIDS[0]));

const obd2_pid_def_t *obd2_pid_find_svc(uint8_t service, uint16_t pid)
{
    if (service == 0) service = 0x01;
    /* Built-in first. */
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        if (OBD2_PIDS[i].pid != pid) continue;
        uint8_t s = OBD2_PIDS[i].service ? OBD2_PIDS[i].service : 0x01;
        if (s == service) return &OBD2_PIDS[i];
    }
    /* Custom PID registry. */
    const obd2_pid_def_t *c = obd2_custom_find_svc(service, pid);
    if (c) return c;
    /* Fall back to first match by PID byte alone (covers code paths
     * that don't know which service the PID came from). */
    return obd2_pid_find(pid);
}

const obd2_pid_def_t *obd2_pid_find(uint16_t pid)
{
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        if (OBD2_PIDS[i].pid == pid) return &OBD2_PIDS[i];
    }
    /* Custom PIDs — first match by byte. */
    for (uint8_t i = 0; i < obd2_custom_count(); i++) {
        const obd2_pid_def_t *d = obd2_custom_at(i);
        if (d && d->pid == pid) return d;
    }
    return NULL;
}

/* Unified iteration helpers — built-in first, then custom. */
uint8_t obd2_pid_total_count(void)
{
    int total = OBD2_PIDS_COUNT + obd2_custom_count();
    return (total > 0xFF) ? 0xFF : (uint8_t)total;
}

const obd2_pid_def_t *obd2_pid_at(uint8_t index)
{
    if (index < OBD2_PIDS_COUNT) return &OBD2_PIDS[index];
    return obd2_custom_at((uint8_t)(index - OBD2_PIDS_COUNT));
}
