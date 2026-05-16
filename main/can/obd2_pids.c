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
};

const int OBD2_PIDS_COUNT = (int)(sizeof(OBD2_PIDS) / sizeof(OBD2_PIDS[0]));

const obd2_pid_def_t *obd2_pid_find(uint8_t pid)
{
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        if (OBD2_PIDS[i].pid == pid) return &OBD2_PIDS[i];
    }
    return NULL;
}
