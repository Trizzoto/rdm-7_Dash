/*
 * preset_picker_data.c — the preconfig catalogue, and nothing else.
 *
 * Split out of preset_picker.c (ADR-0033) so this table is compilable
 * on the host: tools/native/gen_channel_catalog.c links it (with the
 * mock lvgl.h satisfying preset_picker.h) and emits the catalogue JSON
 * that ships inside main/web/index.html for offline setup. One table,
 * three consumers — device picker, web source-options, baked page copy —
 * and none of them can drift because they all read THIS array.
 */

#include "preset_picker.h"

/* Silence -Wmissing-field-initializers across the table — positional
 * initializers omit the trailing `obd2_pid` field, which C zero-inits
 * (= 0 = "not OBD2"), which is exactly what we want for every native
 * preset entry. Adding `, 0` to ~400 rows would be churn for nothing. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

const preconfig_item_t preconfig_items[] = {

/* ── Ford BA/BF ──────────────────────────────────────────────────────── */
{ "Ford", "BA/BF", "AMBIENT TEMP",    "353", 0, 32,  8, 0.333333, -30,  1, false },
{ "Ford", "BA/BF", "BARO PRESSURE",   "44D", 0, 56,  8, 0.5,        0,  1, false },
{ "Ford", "BA/BF", "BATTERY VOLTAGE", "427", 0, 24,  8, 0.1,        0,  1, false },
{ "Ford", "BA/BF", "COOLANT TEMP",    "427", 0,  0,  8, 1.0,      -40,  0, false },
{ "Ford", "BA/BF", "DAMPED FUEL LVL", "437", 0,  0,  8, 0.51,       0,  1, false },
{ "Ford", "BA/BF", "FUEL PULSE",      "427", 0, 56,  8, 0.0000788519, 0, 4, false },
{ "Ford", "BA/BF", "INSTANT ECONOMY", "553", 0, 24,  8, 0.1,        0,  1, false },
{ "Ford", "BA/BF", "INSTANT FUEL",    "437", 0,  8,  8, 0.51,       0,  2, false },
{ "Ford", "BA/BF", "KM RANGE",        "553", 0,  0, 16, 1.0,        0,  0, false },
{ "Ford", "BA/BF", "OIL PRESS SECTOR","427", 0,  8,  4, 1.0,        0,  0, false },
{ "Ford", "BA/BF", "OIL PRESS WARN",  "427", 0, 43,  1, 1.0,        0,  0, false },
{ "Ford", "BA/BF", "OIL TEMP",        "44D", 0, 48,  8, 1.0,      -40,  0, false },
{ "Ford", "BA/BF", "ODOMETER",        "427", 0, 32, 32, 0.000201167, 0, 0, false },
{ "Ford", "BA/BF", "ENGINE RPM",      "207", 0,  0, 16, 0.25,       0,  0, false },
{ "Ford", "BA/BF", "THROTTLE %",      "207", 0, 48,  8, 0.5,        0,  1, false },
{ "Ford", "BA/BF", "VEHICLE SPEED",   "207", 0, 32, 16, 0.0078125,  0,  2, false },

/* ── Ford FG ─────────────────────────────────────────────────────────── */
{ "Ford", "FG",    "ACCEL PEDAL %",   "204", 1, 0,  16, 0.01,     0,   1, false },
{ "Ford", "FG",    "BRAKE SWITCH",    "060", 1, 18, 1,  1.0,      0,   0, false },
{ "Ford", "FG",    "COOLANT TEMP",    "156", 1, 0,  8,  1.0,    -60,   0, false },
{ "Ford", "FG",    "ENGINE RPM",      "109", 1, 0,  16, 0.25,     0,   0, false },
{ "Ford", "FG",    "FUEL LEVEL %",    "320", 1, 0,  16, 0.01,     0,   1, false },
{ "Ford", "FG",    "GEAR (BITMASK)",  "171", 1, 0,  8,  1.0,      0,   0, false },
{ "Ford", "FG",    "OIL TEMP",        "156", 1, 8,  8,  1.0,    -60,   0, false },
{ "Ford", "FG",    "SHIFTER POS",     "191", 1, 0,  8,  1.0,      0,   0, false },
{ "Ford", "FG",    "STEER ANGL",      "082", 1, 0,  16, 0.1,      0,   1, true  },
{ "Ford", "FG",    "VEHICLE SPEED",   "109", 1, 32, 16, 0.01,     0,   2, false },
{ "Ford", "FG",    "WHEEL SPD FL",    "217", 1, 0,  16, 0.01,     0,   2, false },
{ "Ford", "FG",    "WHEEL SPD FR",    "217", 1, 16, 16, 0.01,     0,   2, false },
{ "Ford", "FG",    "WHEEL SPD RL",    "217", 1, 32, 16, 0.01,     0,   2, false },
{ "Ford", "FG",    "WHEEL SPD RR",    "217", 1, 48, 16, 0.01,     0,   2, false },

/* ── Haltech Nexus ───────────────────────────────────────────────────── */
{ "Haltech", "Nexus", "ABS HUMIDITY",       "376", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "AIR TEMP",           "3E0", 0, 16, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "AMBIENT AIR TEMP",   "376", 0, 0, 16, 0.1, -273.15, 1, false },
/* Baro shares 0x372 with battery voltage: bytes 6-7, 0.1 kPa, 10 Hz
 * (Haltech CAN Broadcast Protocol v2.35.0). Binds canonical
 * barometric_pressure via the BARO_PRESSURE alias in ecu_presets.c. */
{ "Haltech", "Nexus", "BARO PRESSURE",      "372", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "BATTERY VOLT",       "372", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "BRAKE PRESSURE",     "36B", 0, 0, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "COOLANT PRESSURE",   "360", 0, 48, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "COOLANT TEMP",       "3E0", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "DIFF OIL TEMP",      "3E1", 0, 16, 16, 0.1, -273.15, 1, false },
/* EGT frames 0x373-0x375 broadcast 0.1 Kelvin — same convention as the
 * 0x3E0 temps, so they need the same -273.15 offset (was 0: read +273°C
 * high). Matches the bulk preset's ECU_SIG_EGT row. */
{ "Haltech", "Nexus", "EGT SENSOR 1",       "373", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 2",       "373", 0, 16, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 3",       "373", 0, 32, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 4",       "373", 0, 48, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 5",       "374", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 6",       "374", 0, 16, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 7",       "374", 0, 32, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 8",       "374", 0, 48, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 9",       "375", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 10",      "375", 0, 16, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 11",      "375", 0, 32, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 12",      "375", 0, 48, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "ENGINE DEMAND",      "361", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "ENGINE LIMIT",       "36E", 0, 0, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "FUEL COMP",          "3E1", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "FUEL LEVEL",         "3E2", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "FUEL PRESSURE",      "361", 0, 0, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "FUEL TEMP",          "3E0", 0, 32, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "FUEL TRIM LT B1",    "3E3", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "FUEL TRIM LT B2",    "3E3", 0, 48, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "FUEL TRIM ST B1",    "3E3", 0, 0, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "FUEL TRIM ST B2",    "3E3", 0, 16, 16, 0.1, 0, 1, true },
/* GEAR was previously bound to 0x360 byte 6-7 — but that's Coolant Pressure
 * per the Haltech v2.35.0 spec. The real gear channel is 0x470 byte 7,
 * an 8-bit signed enum (-1=Reverse, 0=Neutral, 1..6=gear). */
{ "Haltech", "Nexus", "GEAR",               "470", 0, 56,  8, 1.0, 0, 0, true  },
{ "Haltech", "Nexus", "GEARBOX OIL TEMP",   "3E1", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "IGN ANGLE LEAD",     "362", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "INJ STG1 TIME",      "364", 0, 0, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "INJ STG2 TIME",      "364", 0, 16, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "INJ STG3 TIME",      "364", 0, 32, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "INJ STG4 TIME",      "364", 0, 48, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "INJECTION STG1",     "362", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "INJECTION STG2",     "362", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "INTAKE CAM 1",       "370", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "INTAKE CAM 2",       "370", 0, 48, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "KNOCK LEVEL 1",      "36A", 0, 0, 16, 0.01, 0, 2, false },
{ "Haltech", "Nexus", "KNOCK LEVEL 2",      "36A", 0, 16, 16, 0.01, 0, 2, false },
{ "Haltech", "Nexus", "LATERAL G",          "36B", 0, 48, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "LAUNCH END RPM",     "363", 0, 48, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "LC FUEL ENRICH",     "36E", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "LC IGN RETARD",      "36E", 0, 16, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "LONGITUDINAL G",     "36E", 0, 48, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "MANIFOLD PRESSURE",  "360", 0, 16, 16, 0.0145, 0, 1, false },
{ "Haltech", "Nexus", "NOS PRESSURE",       "36B", 0, 16, 16, 0.0319, -14.7, 1, false },
{ "Haltech", "Nexus", "OIL PRESSURE",       "361", 0, 16, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "OIL TEMP",           "3E0", 0, 48, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "REL HUMIDITY",       "376", 0, 16, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "RPM",                "360", 0, 0, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "SPEC HUMIDITY",      "376", 0, 32, 16, 100.0, 0, 0, false },
{ "Haltech", "Nexus", "THROTTLE POSITION",  "360", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "TRIGGER COUNT",      "369", 0, 16, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "TRIGGER ERR CNT",    "369", 0, 0, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "TRIGGER SYNC",       "369", 0, 48, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "TURBO SPEED",        "36B", 0, 32, 16, 10.0, 0, 0, false },
{ "Haltech", "Nexus", "VEHICLE SPEED",      "370", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "WASTEGATE PRESS",    "361", 0, 48, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "WHEEL DIFF",         "363", 0, 16, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "WHEEL SLIP",         "363", 0, 0, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "WIDEBAND 1",         "368", 0, 0, 16, 0.0147, 0, 3, false },
{ "Haltech", "Nexus", "WIDEBAND 2",         "368", 0, 16, 16, 0.0147, 0, 3, false },
{ "Haltech", "Nexus", "WIDEBAND 3",         "368", 0, 32, 16, 0.0147, 0, 3, false },
{ "Haltech", "Nexus", "WIDEBAND 4",         "368", 0, 48, 16, 0.0147, 0, 3, false },

/* ── MaxxECU 1.2 ─────────────────────────────────────────────────────── */
/* SIGN: the MaxxECU DBCs mark nearly everything @1+ (unsigned), but the ECU
 * puts bidirectional quantities on the wire as two's-complement — temps,
 * ignition/knock/lambda corrections, fuel trim, gear (reverse = -1), accel
 * axes, cam positions, slip. Those rows are is_signed=true here; signed is
 * bit-identical to unsigned across each field's legitimate positive range,
 * so nothing regresses where values happen to stay positive. MUST stay in
 * lockstep with ECU_PRESETS in ecu_presets.c — see the SIGN comments there
 * and tools/check_preset_signedness.py. Field-verified 2026-08. */
    { "MaxxECU", "1.2", "BARO PRESSURE", "530", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "BATTERY VOLTAGE", "530", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.2", "BOOST SOLENOID DUTY", "536", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "COOLANT TEMP", "530", 1, 48, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "CPU TEMP", "534", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "DRIVEN WHEELS AVG SPD", "523", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "E85 %", "531", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 1", "531", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 2", "532", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 3", "532", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 4", "532", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 5", "532", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 6", "533", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 7", "533", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 8", "533", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT DIFFERENCE", "534", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT HIGHEST", "533", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "ERROR CODE COUNT", "534", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "FIRMWARE VERSION", "524", 1, 48, 16, 0.001, 0, 0, false },
    { "MaxxECU", "1.2", "FUEL CUT", "522", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "FUEL DUTY PRIMARY", "522", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "FUEL PULSEWIDTH PRIMARY", "522", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.2", "GEAR", "536", 1, 0, 16, 1, 0, 0, true },
    { "MaxxECU", "1.2", "IGNITION ANGLE", "521", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "IGNITION CUT", "521", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "INTAKE AIR TEMP", "530", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "LAMBDA", "520", 1, 48, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA A", "521", 1, 0, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA B", "521", 1, 16, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA CORR A", "524", 1, 16, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "LAMBDA CORR B", "524", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "LOST SYNC COUNT", "534", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "MAP", "520", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "RPM", "520", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "TARGET SLIP", "523", 1, 48, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "THROTTLE %", "520", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TOTAL FUEL TRIM", "531", 1, 0, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "TOTAL IGNITION COMP", "531", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.2", "TRACTION CTRL POWER LIMIT", "524", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "UNDRIVEN WHEELS AVG SPD", "523", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 1", "535", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 2", "535", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 3", "535", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 4", "535", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "VEHICLE SPEED", "522", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "WHEEL SLIP", "523", 1, 32, 16, 0.1, 0, 0, true },

/* ── MaxxECU 1.3 ─────────────────────────────────────────────────────── */
    { "MaxxECU", "1.3", "AC/IDLE UP ACTIVE", "526", 1, 6, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "ACCELERATION FORWARD", "527", 1, 0, 16, 0.01, 0, 0, true },
    { "MaxxECU", "1.3", "ACCELERATION RIGHT", "527", 1, 16, 16, 0.01, 0, 0, true },
    { "MaxxECU", "1.3", "ACCELERATION UP", "527", 1, 32, 16, 0.01, 0, 0, true },
    { "MaxxECU", "1.3", "ACTIVE BOOST TABLE", "540", 1, 0, 8, 1, 0, 0, false },
    { "MaxxECU", "1.3", "ACTIVE TUNE SELECTOR", "540", 1, 8, 8, 1, 0, 0, false },
    { "MaxxECU", "1.3", "ANTI-LAG ACTIVE", "526", 1, 2, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "BARO PRESSURE", "530", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "BATTERY VOLTAGE", "530", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "BOOST SOLENOID DUTY", "536", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "BOOST TARGET", "537", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "BRAKE PEDAL ACTIVE", "526", 1, 8, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "CLUTCH PEDAL ACTIVE", "526", 1, 9, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "COOLANT PRESSURE", "537", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "COOLANT TEMP", "530", 1, 48, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "CPU TEMP", "534", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "DIFFERENTIAL TEMP", "540", 1, 48, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "DRIVEN WHEELS AVG SPD", "523", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "E85 %", "531", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "ECU IS LOGGING", "526", 1, 13, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 1", "531", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 2", "532", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 3", "532", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 4", "532", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 5", "532", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 6", "533", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 7", "533", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 8", "533", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT DIFFERENCE", "534", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT HIGHEST", "533", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "ERROR CODE COUNT", "534", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "FIRMWARE VERSION", "524", 1, 48, 16, 0.001, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL CUT", "522", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL DUTY PRIMARY", "522", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL PRESSURE 1", "537", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL PULSEWIDTH PRIMARY", "522", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "GEAR", "536", 1, 0, 16, 1, 0, 0, true },
    { "MaxxECU", "1.3", "GP LIMITER ACTIVE", "526", 1, 11, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "IGNITION ANGLE", "521", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "IGNITION CUT", "521", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "INTAKE AIR TEMP", "530", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "KNOCK CORRECTION", "528", 1, 16, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "KNOCK COUNT", "528", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCK DETECTED", "526", 1, 7, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCKLEVEL ALL PEAK", "528", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA", "520", 1, 48, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA A", "521", 1, 0, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA B", "521", 1, 16, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA CORR A", "524", 1, 16, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "LAMBDA CORR B", "524", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "LAMBDA TARGET", "527", 1, 48, 16, 0.001, 0, 0, false },
    { "MaxxECU", "1.3", "LAST KNOCK CYLINDER", "528", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "LAUNCH CONTROL ACTIVE", "526", 1, 3, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "LOST SYNC COUNT", "534", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "MAP", "520", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "RPM", "520", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "NITROUS ACTIVE", "526", 1, 14, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "OIL PRESSURE", "536", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "OIL TEMP", "536", 1, 48, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "REV-LIMIT ACTIVE", "526", 1, 1, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "REV-LIMIT RPM", "526", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "SHIFTCUT ACTIVE", "526", 1, 0, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 15, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "SPEED LIMIT ACTIVE", "526", 1, 10, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "TARGET SLIP", "523", 1, 48, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "THROTTLE %", "520", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "THROTTLE BLIP ACTIVE", "526", 1, 5, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "TOTAL FUEL TRIM", "531", 1, 0, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "TOTAL IGNITION COMP", "531", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "TRACTION CTRL POWER LIMIT", "524", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TRACTION POWER LIMITER ACTIVE", "526", 1, 4, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "TRANSMISSION TEMP", "540", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "UNDRIVEN WHEELS AVG SPD", "523", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 1", "535", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 2", "535", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 3", "535", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 4", "535", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 1", "538", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 2", "538", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 3", "538", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 4", "538", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 5", "539", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 6", "539", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 7", "539", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 8", "539", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 9", "525", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 10", "525", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 11", "525", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CHANNEL 12", "525", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER CUT ACTIVE", "526", 1, 12, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "VEHICLE SPEED", "522", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VIRTUAL FUEL TANK", "540", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT EXHAUST CAM 1 POS", "541", 1, 16, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "VVT EXHAUST CAM 2 POS", "541", 1, 48, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "VVT INTAKE CAM 1 POS", "541", 1, 0, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "VVT INTAKE CAM 2 POS", "541", 1, 32, 16, 0.1, 0, 0, true },
    { "MaxxECU", "1.3", "WASTEGATE PRESSURE", "537", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "WHEEL SLIP", "523", 1, 32, 16, 0.1, 0, 0, true },

/* ── ECU Master Black / Classic (Base CAN ID = 0x600, Intel LE) ───────── */
{ "ECU Master", "Black/Classic", "RPM",                "600", 1,  0, 16, 1.0,        0,        0, false },
{ "ECU Master", "Black/Classic", "THROTTLE %",         "600", 1, 16,  8, 0.5,        0,        1, false },
{ "ECU Master", "Black/Classic", "INTAKE AIR TEMP",    "600", 1, 24,  8, 1.0,        0,        0, true  },
{ "ECU Master", "Black/Classic", "MAP (kPa)",          "600", 1, 32, 16, 1.0,        0,        0, false },
{ "ECU Master", "Black/Classic", "VEHICLE SPEED",      "602", 1,  0, 16, 1.0,        0,        0, false },
{ "ECU Master", "Black/Classic", "OIL TEMP",           "602", 1, 24,  8, 1.0,        0,        0, false },
{ "ECU Master", "Black/Classic", "OIL PRESSURE (kPa)", "602", 1, 32,  8, 6.25,       0,        1, false },
{ "ECU Master", "Black/Classic", "FUEL PRESSURE (kPa)","602", 1, 40,  8, 6.25,       0,        1, false },
{ "ECU Master", "Black/Classic", "COOLANT TEMP",       "602", 1, 48, 16, 1.0,        0,        0, true  },
{ "ECU Master", "Black/Classic", "IGNITION ANGLE",     "603", 1,  0,  8, 0.5,        0,        1, true  },
{ "ECU Master", "Black/Classic", "LAMBDA",             "603", 1, 16,  8, 0.0078125,  0,        3, false },
{ "ECU Master", "Black/Classic", "LAMBDA CORRECTION",  "603", 1, 24,  8, 0.5,        -100,     1, false },
{ "ECU Master", "Black/Classic", "EGT 1",              "603", 1, 32, 16, 1.0,        0,        0, false },
{ "ECU Master", "Black/Classic", "EGT 2",              "603", 1, 48, 16, 1.0,        0,        0, false },
{ "ECU Master", "Black/Classic", "GEAR",               "604", 1,  0,  8, 1.0,        0,        0, true  },
{ "ECU Master", "Black/Classic", "BATTERY VOLT",       "604", 1, 16, 16, 0.027,      0,        2, false },
{ "ECU Master", "Black/Classic", "BOOST TARGET",       "607", 1,  0, 16, 1.0,        0,        0, false },

/* ── MegaSquirt MS3-Pro (Base CAN ID = 0x5F0, Motorola BE, metric) ────── */
{ "MegaSquirt", "MS3-Pro", "RPM",             "5F0", 0, 48, 16, 1.0,       0,         0, false },
{ "MegaSquirt", "MS3-Pro", "IGNITION ANGLE",  "5F1", 0,  0, 16, 0.1,       0,         1, true  },
{ "MegaSquirt", "MS3-Pro", "MAP (kPa)",       "5F2", 0, 16, 16, 0.1,       0,         1, true  },
{ "MegaSquirt", "MS3-Pro", "INTAKE AIR TEMP", "5F2", 0, 32, 16, 0.0555556, -17.7778,  1, true  },
{ "MegaSquirt", "MS3-Pro", "COOLANT TEMP",    "5F2", 0, 48, 16, 0.0555556, -17.7778,  1, true  },
{ "MegaSquirt", "MS3-Pro", "THROTTLE %",      "5F3", 0,  0, 16, 0.1,       0,         1, true  },
{ "MegaSquirt", "MS3-Pro", "BATTERY VOLT",    "5F3", 0, 16, 16, 0.1,       0,         1, true  },
{ "MegaSquirt", "MS3-Pro", "FUEL TRIM B1",    "5F4", 0, 16, 16, 0.1,       -100,      1, true  },
{ "MegaSquirt", "MS3-Pro", "EGT 1",           "606", 0,  0, 16, 0.0555556, -17.7778,  1, true  },
{ "MegaSquirt", "MS3-Pro", "GEAR",            "611", 0, 48,  8, 1.0,       0,         0, true  },
{ "MegaSquirt", "MS3-Pro", "VEHICLE SPEED",   "612", 0,  0, 16, 0.36,      0,         1, false },
{ "MegaSquirt", "MS3-Pro", "FUEL PRESSURE",   "615", 0,  0, 16, 0.1,       0,         1, true  },
{ "MegaSquirt", "MS3-Pro", "LAMBDA (AFR1)",   "5FF", 0,  0,  8, 0.0068027, 0,         3, false },

/* ── Link ECU — Generic Dash (CAN ID = 0x3E8 / 1000, MULTIPLEXED) ─────
 *
 * The whole stream lives on ONE id. Byte 0 carries a frame index and the
 * three 16-bit words at bytes 2-3 / 4-5 / 6-7 mean something different for
 * each index — so every row here shares can_id 0x3E8 and is separated by
 * mux_value, not by id.
 *
 * These rows previously encoded the frame index as an id offset (0x3E8 +
 * frame), which put 13 of the 14 frames on ids the ECU never transmits.
 * The consequences were that auto-detect could never see more than one of
 * the preset's ids (so a Link was undetectable), and applying the preset by
 * hand left 11 dead channels while RPM/MAP/MGP decoded from every frame
 * indiscriminately. The tell that it was never right: no row reads bits
 * 0-15, because those bytes were never data in the first place. */
{ "Link ECU", "Generic Dash", "ENGINE SPEED",          "3E8", 1, 16, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 0 },
{ "Link ECU", "Generic Dash", "MAP",                   "3E8", 1, 32, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 0 },
{ "Link ECU", "Generic Dash", "MGP",                   "3E8", 1, 48, 16, 1.0,    -100, 0, false, 0, 0, 0, 8, 0 },
{ "Link ECU", "Generic Dash", "BARO PRESSURE",         "3E8", 1, 16, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 1 },
{ "Link ECU", "Generic Dash", "TPS",                   "3E8", 1, 32, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 1 },
{ "Link ECU", "Generic Dash", "INJECTOR DC",           "3E8", 1, 48, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 1 },
{ "Link ECU", "Generic Dash", "INJECTOR DC (SEC)",     "3E8", 1, 16, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 2 },
{ "Link ECU", "Generic Dash", "INJ PULSE WIDTH",       "3E8", 1, 32, 16, 0.001,  0,    3, false, 0, 0, 0, 8, 2 },
{ "Link ECU", "Generic Dash", "COOLANT TEMP",           "3E8", 1, 48, 16, 1.0,    -50,  0, false, 0, 0, 0, 8, 2 },
{ "Link ECU", "Generic Dash", "IAT",                   "3E8", 1, 16, 16, 1.0,    -50,  0, false, 0, 0, 0, 8, 3 },
{ "Link ECU", "Generic Dash", "ECU VOLTS",             "3E8", 1, 32, 16, 0.01,   0,    2, false, 0, 0, 0, 8, 3 },
{ "Link ECU", "Generic Dash", "MAF",                   "3E8", 1, 48, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 3 },
{ "Link ECU", "Generic Dash", "GEAR POSITION",         "3E8", 1, 16, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 4 },
{ "Link ECU", "Generic Dash", "INJECTOR TIMING",       "3E8", 1, 32, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 4 },
{ "Link ECU", "Generic Dash", "IGNITION TIMING",       "3E8", 1, 48, 16, 0.1,    -100, 1, false, 0, 0, 0, 8, 4 },
{ "Link ECU", "Generic Dash", "CAM INLET BANK 1",      "3E8", 1, 16, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 5 },
{ "Link ECU", "Generic Dash", "CAM INLET BANK 2",      "3E8", 1, 32, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 5 },
{ "Link ECU", "Generic Dash", "CAM EXHAUST BANK 1",    "3E8", 1, 48, 16, -0.1,   0,    1, false, 0, 0, 0, 8, 5 },
{ "Link ECU", "Generic Dash", "CAM EXHAUST BANK 2",    "3E8", 1, 16, 16, -0.1,   0,    1, false, 0, 0, 0, 8, 6 },
{ "Link ECU", "Generic Dash", "LAMBDA 1",              "3E8", 1, 32, 16, 0.001,  0,    3, false, 0, 0, 0, 8, 6 },
{ "Link ECU", "Generic Dash", "LAMBDA 2",              "3E8", 1, 48, 16, 0.001,  0,    3, false, 0, 0, 0, 8, 6 },
{ "Link ECU", "Generic Dash", "TRIG 1 ERROR COUNT",    "3E8", 1, 16, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 7 },
{ "Link ECU", "Generic Dash", "FAULT CODES",           "3E8", 1, 32, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 7 },
{ "Link ECU", "Generic Dash", "FUEL PRESSURE",         "3E8", 1, 48, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 7 },
{ "Link ECU", "Generic Dash", "OIL TEMP",              "3E8", 1, 16, 16, 1.0,    -50,  0, false, 0, 0, 0, 8, 8 },
{ "Link ECU", "Generic Dash", "OIL PRESSURE",          "3E8", 1, 32, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 8 },
{ "Link ECU", "Generic Dash", "LF WHEEL SPEED",        "3E8", 1, 48, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 8 },
{ "Link ECU", "Generic Dash", "LR WHEEL SPEED",        "3E8", 1, 16, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 9 },
{ "Link ECU", "Generic Dash", "RF WHEEL SPEED",        "3E8", 1, 32, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 9 },
{ "Link ECU", "Generic Dash", "RR WHEEL SPEED",        "3E8", 1, 48, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 9 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 1",         "3E8", 1, 16, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 10 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 2",         "3E8", 1, 32, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 10 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 3",         "3E8", 1, 48, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 10 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 4",         "3E8", 1, 16, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 11 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 5",         "3E8", 1, 32, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 11 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 6",         "3E8", 1, 48, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 11 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 7",         "3E8", 1, 16, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 12 },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 8",         "3E8", 1, 32, 16, 5.0,    0,    0, false, 0, 0, 0, 8, 12 },
{ "Link ECU", "Generic Dash", "LIMITS FLAGS",          "3E8", 1, 48, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 12 },
{ "Link ECU", "Generic Dash", "APS (MAIN)",            "3E8", 1, 16, 16, 0.1,    0,    1, false, 0, 0, 0, 8, 13 },
{ "Link ECU", "Generic Dash", "ETHANOL %",             "3E8", 1, 32, 16, 1.0,    0,    0, false, 0, 0, 0, 8, 13 },

/* ── Toyota GT86 Gen 1 ──────────────────────────────────────────────────
 * Decode params published by the GT86/BRZ enthusiast community. Brake
 * pressure shares an 8-bit slot with Brake %; pick whichever is more
 * useful for your dash layout. Brake % clips at 100 in the source data
 * — clamp via widget max-value if you display it as a bar/gauge.
 *
 * Speed scale is 0.05625 = 3.6/64. The raw 16-bit field is 1/64 m/s, so
 * the km/h scale MUST carry the 3.6 factor. These rows previously used
 * 0.015694 (~1/64), which emitted m/s while the channel, the odometer and
 * CALCULATED_GEAR all treat the value as km/h — speed read 3.6x low, so an
 * indicated "30" was really 108 km/h. Don't drop the 3.6 again. */
    { "Toyota", "GT86 Gen 1", "ACCEL PEDAL %",        "140", 1,  0,  8, 0.39215,    0, 1, false },
    { "Toyota", "GT86 Gen 1", "BRAKE %",              "0D1", 1, 16,  8, 1.42857,    0, 0, false },
    { "Toyota", "GT86 Gen 1", "BRAKE PRESSURE",       "0D1", 1, 16,  8, 128.0,      0, 0, false },
    { "Toyota", "GT86 Gen 1", "COOLANT TEMP",         "360", 1, 24,  8, 1.0,      -40, 0, false },
    { "Toyota", "GT86 Gen 1", "ENGINE RPM",           "140", 1, 16, 14, 1.0,        0, 0, false },
    { "Toyota", "GT86 Gen 1", "OIL TEMP",             "360", 1, 16,  8, 1.0,      -40, 0, false },
    { "Toyota", "GT86 Gen 1", "VEHICLE SPEED",        "0D1", 1,  0, 16, 0.05625,    0, 1, true  },
    { "Toyota", "GT86 Gen 1", "STEERING ANGLE",       "0D0", 1,  0, 16, -0.1,       0, 1, true  },
    { "Toyota", "GT86 Gen 1", "LATERAL ACCEL",        "0D0", 1, 48,  8, 0.2,        0, 2, true  },
    { "Toyota", "GT86 Gen 1", "LONGITUDINAL ACCEL",   "0D0", 1, 56,  8, -0.1,       0, 2, true  },
    { "Toyota", "GT86 Gen 1", "THROTTLE %",           "140", 1, 48,  8, 0.39215,    0, 1, false },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD FL",         "0D4", 1,  0, 16, 0.05625,    0, 1, true  },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD FR",         "0D4", 1, 16, 16, 0.05625,    0, 1, true  },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD RL",         "0D4", 1, 32, 16, 0.05625,    0, 1, true  },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD RR",         "0D4", 1, 48, 16, 0.05625,    0, 1, true  },
    { "Toyota", "GT86 Gen 1", "YAW RATE",             "0D0", 1, 16, 16, -0.286478,  0, 2, true  },
    { "Toyota", "GT86 Gen 1", "HAND BRAKE",           "152", 1, 51,  1, 1.0,        0, 0, false },
    { "Toyota", "GT86 Gen 1", "ANY DOOR OPEN",        "375", 1, 26,  1, 1.0,        0, 0, false },
    { "Toyota", "GT86 Gen 1", "LIGHTS ON",            "375", 1, 27,  1, 1.0,        0, 0, false },
    { "Toyota", "GT86 Gen 1", "DRIVER DOOR OPEN",     "375", 1,  8,  1, 1.0,        0, 0, false },
    { "Toyota", "GT86 Gen 1", "PASSENGER DOOR OPEN",  "375", 1,  9,  1, 1.0,        0, 0, false },
    { "Toyota", "GT86 Gen 1", "BOOT OPEN",            "375", 1, 13,  1, 1.0,        0, 0, false },

/* ── RDM-7 GPIO ──────────────────────────────────────────────────────── */
{ "RDM-7", "GPIO",     "FUEL SENDER V",   "0", 1, 0, 16, 1.0,  0, 2, false },
{ "RDM-7", "GPIO",     "INDICATOR LEFT",  "0", 1, 0, 8,  1.0,  0, 0, false },
{ "RDM-7", "GPIO",     "INDICATOR RIGHT", "0", 1, 0, 8,  1.0,  0, 0, false },

/* ── RDM-7 Internal ──────────────────────────────────────────────────── */
{ "RDM-7", "Internal", "CALCULATED GEAR", "0", 1, 0, 16, 1.0,  0, 0, false },
{ "RDM-7", "Internal", "CHIP TEMP",       "0", 1, 0, 16, 1.0,  0, 1, false },
{ "RDM-7", "Internal", "CPU PERCENT",     "0", 1, 0, 16, 1.0,  0, 0, false },
{ "RDM-7", "Internal", "FPS",             "0", 1, 0, 16, 1.0,  0, 0, false },
{ "RDM-7", "Internal", "FREE HEAP KB",    "0", 1, 0, 16, 1.0,  0, 0, false },
{ "RDM-7", "Internal", "FREE PSRAM KB",   "0", 1, 0, 16, 1.0,  0, 0, false },
/* ODOMETER is published by signal_internal.c: accumulates km from the
 * configured vehicle-speed signal each tick, persists to NVS on a
 * hybrid trigger (≥ 1 km OR ≥ 5 min unsaved). Editable via the
 * Vehicle Settings card (on-device) or POST /api/odometer (web). */
{ "RDM-7", "Internal", "ODOMETER",        "0", 1, 0, 16, 1.0,  0, 1, false },
{ "RDM-7", "Internal", "UPTIME S",        "0", 1, 0, 16, 1.0,  0, 0, false },
{ "RDM-7", "Internal", "WIFI RSSI",       "0", 1, 0, 16, 1.0,  0, 0, true  },

{ NULL, NULL, NULL, NULL, 0, 0, 0, 0.0, 0, 0, false }
};

#pragma GCC diagnostic pop

const int preconfig_items_count = sizeof(preconfig_items)/sizeof(preconfig_items[0]);
