#include "preset_picker.h"
#include "theme.h"
#include <stdio.h>
#include "lvgl.h"
#include "screens/ui_Screen3.h"
#include "widgets/lv_dropdown.h"
#include <string.h>
#include <stdlib.h>
#include "storage/config_store.h"


/* preconfig_item_t is defined in preset_picker.h */

const preconfig_item_t preconfig_items[] = {

/* ── Ford BA/BF ──────────────────────────────────────────────────────── */
{ "Ford", "BA/BF", "AMBIENT TEMP",    "353", 0, 32,  8, 0.333333, -30,  1, false },
{ "Ford", "BA/BF", "BARO PRESSURE",   "44D", 0, 56,  8, 0.5,        0,  1, false },
{ "Ford", "BA/BF", "BATTERY VOLTAGE", "427", 0, 24,  8, 0.1,        0,  1, false },
{ "Ford", "BA/BF", "COOLANT TEMP",    "427", 0,  0,  8, 1.0,      -40,  0, false },
{ "Ford", "BA/BF", "FUEL PULSE",      "427", 0, 56,  8, 0.0000788519, 0, 4, false },
{ "Ford", "BA/BF", "INSTANT ECONOMY", "553", 0, 24,  8, 0.1,        0,  1, false },
{ "Ford", "BA/BF", "INSTANT FUEL",    "437", 0,  8,  8, 0.51,       0,  2, false },
{ "Ford", "BA/BF", "KM RANGE",        "553", 0,  0, 16, 1.0,        0,  0, false },
{ "Ford", "BA/BF", "OIL TEMP",        "44D", 0, 48,  8, 1.0,      -40,  0, false },
{ "Ford", "BA/BF", "ODOMETER",        "4C0", 0,  0, 24, 0.1,        0,  1, false },
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
{ "Haltech", "Nexus", "BATTERY VOLT",       "372", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "BRAKE PRESSURE",     "36B", 0, 0, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "COOLANT PRESSURE",   "360", 0, 48, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "COOLANT TEMP",       "3E0", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "DIFF OIL TEMP",      "3E1", 0, 16, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 1",       "373", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 2",       "373", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 3",       "373", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 4",       "373", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 5",       "374", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 6",       "374", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 7",       "374", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 8",       "374", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 9",       "375", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 10",      "375", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 11",      "375", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 12",      "375", 0, 48, 16, 0.1, 0, 1, false },
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
{ "Haltech", "Nexus", "GEAR",               "360", 0, 48, 16, 1.0, 0, 0, false },
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
    { "MaxxECU", "1.2", "BARO PRESSURE", "530", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "BATTERY VOLTAGE", "530", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.2", "BOOST SOLENOID DUTY", "536", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "COOLANT TEMP", "530", 1, 48, 16, 0.1, 0, 0, false },
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
    { "MaxxECU", "1.2", "GEAR", "536", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "IGNITION ANGLE", "521", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "IGNITION CUT", "521", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "INTAKE AIR TEMP", "530", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "LAMBDA", "520", 1, 48, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA A", "521", 1, 0, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA B", "521", 1, 16, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA CORR A", "524", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "LAMBDA CORR B", "524", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "LOST SYNC COUNT", "534", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "MAP", "520", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "RPM", "520", 1, 0, 16, 1, 0, 0, true },
    { "MaxxECU", "1.2", "TARGET SLIP", "523", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "THROTTLE %", "520", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TOTAL FUEL TRIM", "531", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TOTAL IGNITION COMP", "531", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TRACTION CTRL POWER LIMIT", "524", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "UNDRIVEN WHEELS AVG SPD", "523", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 1", "535", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 2", "535", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 3", "535", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 4", "535", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "VEHICLE SPEED", "522", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "WHEEL SLIP", "523", 1, 32, 16, 0.1, 0, 0, false },

/* ── MaxxECU 1.3 ─────────────────────────────────────────────────────── */
    { "MaxxECU", "1.3", "AC/IDLE UP ACTIVE", "526", 1, 6, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "ACCELERATION FORWARD", "527", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "ACCELERATION RIGHT", "527", 1, 16, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "ACCELERATION UP", "527", 1, 32, 16, 0.01, 0, 0, false },
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
    { "MaxxECU", "1.3", "COOLANT TEMP", "530", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "CPU TEMP", "534", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "DIFFERENTIAL TEMP", "540", 1, 48, 16, 0.1, 0, 0, false },
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
    { "MaxxECU", "1.3", "GEAR", "536", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "GP LIMITER ACTIVE", "526", 1, 11, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "IGNITION ANGLE", "521", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "IGNITION CUT", "521", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "INTAKE AIR TEMP", "530", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCK CORRECTION", "528", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCK COUNT", "528", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCK DETECTED", "526", 1, 7, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCKLEVEL ALL PEAK", "528", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA", "520", 1, 48, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA A", "521", 1, 0, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA B", "521", 1, 16, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA CORR A", "524", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA CORR B", "524", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA TARGET", "527", 1, 48, 16, 0.001, 0, 0, false },
    { "MaxxECU", "1.3", "LAST KNOCK CYLINDER", "528", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "LAUNCH CONTROL ACTIVE", "526", 1, 3, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "LOST SYNC COUNT", "534", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "MAP", "520", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "RPM", "520", 1, 0, 16, 1, 0, 0, true },
    { "MaxxECU", "1.3", "NITROUS ACTIVE", "526", 1, 14, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "OIL PRESSURE", "536", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "OIL TEMP", "536", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "REV-LIMIT ACTIVE", "526", 1, 1, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "REV-LIMIT RPM", "526", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "SHIFTCUT ACTIVE", "526", 1, 0, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 15, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "SPEED LIMIT ACTIVE", "526", 1, 10, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "TARGET SLIP", "523", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "THROTTLE %", "520", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "THROTTLE BLIP ACTIVE", "526", 1, 5, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "TOTAL FUEL TRIM", "531", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TOTAL IGNITION COMP", "531", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TRACTION CTRL POWER LIMIT", "524", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TRACTION POWER LIMITER ACTIVE", "526", 1, 4, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "TRANSMISSION TEMP", "540", 1, 32, 16, 0.1, 0, 0, false },
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
    { "MaxxECU", "1.3", "VVT EXHAUST CAM 1 POS", "541", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT EXHAUST CAM 2 POS", "541", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT INTAKE CAM 1 POS", "541", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT INTAKE CAM 2 POS", "541", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "WASTEGATE PRESSURE", "537", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "WHEEL SLIP", "523", 1, 32, 16, 0.1, 0, 0, false },

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

/* ── Link ECU — Generic Dash (Base CAN ID = 0x3E8 / 1000) ───────────── */
{ "Link ECU", "Generic Dash", "ENGINE SPEED",          "3E8", 1, 16, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "MAP",                   "3E8", 1, 32, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "MGP",                   "3E8", 1, 48, 16, 1.0,    -100, 0, false },
{ "Link ECU", "Generic Dash", "BARO PRESSURE",         "3E9", 1, 16, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "TPS",                   "3E9", 1, 32, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "INJECTOR DC",           "3E9", 1, 48, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "INJECTOR DC (SEC)",     "3EA", 1, 16, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "INJ PULSE WIDTH",       "3EA", 1, 32, 16, 0.001,  0,    3, false },
{ "Link ECU", "Generic Dash", "COOLANT TEMP",           "3EA", 1, 48, 16, 1.0,    -50,  0, false },
{ "Link ECU", "Generic Dash", "IAT",                   "3EB", 1, 16, 16, 1.0,    -50,  0, false },
{ "Link ECU", "Generic Dash", "ECU VOLTS",             "3EB", 1, 32, 16, 0.01,   0,    2, false },
{ "Link ECU", "Generic Dash", "MAF",                   "3EB", 1, 48, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "GEAR POSITION",         "3EC", 1, 16, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "INJECTOR TIMING",       "3EC", 1, 32, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "IGNITION TIMING",       "3EC", 1, 48, 16, 0.1,    -100, 1, false },
{ "Link ECU", "Generic Dash", "CAM INLET BANK 1",      "3ED", 1, 16, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "CAM INLET BANK 2",      "3ED", 1, 32, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "CAM EXHAUST BANK 1",    "3ED", 1, 48, 16, -0.1,   0,    1, false },
{ "Link ECU", "Generic Dash", "CAM EXHAUST BANK 2",    "3EE", 1, 16, 16, -0.1,   0,    1, false },
{ "Link ECU", "Generic Dash", "LAMBDA 1",              "3EE", 1, 32, 16, 0.001,  0,    3, false },
{ "Link ECU", "Generic Dash", "LAMBDA 2",              "3EE", 1, 48, 16, 0.001,  0,    3, false },
{ "Link ECU", "Generic Dash", "TRIG 1 ERROR COUNT",    "3EF", 1, 16, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "FAULT CODES",           "3EF", 1, 32, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "FUEL PRESSURE",         "3EF", 1, 48, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "OIL TEMP",              "3F0", 1, 16, 16, 1.0,    -50,  0, false },
{ "Link ECU", "Generic Dash", "OIL PRESSURE",          "3F0", 1, 32, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "LF WHEEL SPEED",        "3F0", 1, 48, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "LR WHEEL SPEED",        "3F1", 1, 16, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "RF WHEEL SPEED",        "3F1", 1, 32, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "RR WHEEL SPEED",        "3F1", 1, 48, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 1",         "3F2", 1, 16, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 2",         "3F2", 1, 32, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 3",         "3F2", 1, 48, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 4",         "3F3", 1, 16, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 5",         "3F3", 1, 32, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 6",         "3F3", 1, 48, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 7",         "3F4", 1, 16, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "KNOCK LEVEL 8",         "3F4", 1, 32, 16, 5.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "LIMITS FLAGS",          "3F4", 1, 48, 16, 1.0,    0,    0, false },
{ "Link ECU", "Generic Dash", "APS (MAIN)",            "3F5", 1, 16, 16, 0.1,    0,    1, false },
{ "Link ECU", "Generic Dash", "ETHANOL %",             "3F5", 1, 32, 16, 1.0,    0,    0, false },

/* ── Toyota GT86 Gen 1 ──────────────────────────────────────────────────
 * Decode params published by the GT86/BRZ enthusiast community. Brake
 * pressure shares an 8-bit slot with Brake %; pick whichever is more
 * useful for your dash layout. Brake % clips at 100 in the source data
 * — clamp via widget max-value if you display it as a bar/gauge. */
    { "Toyota", "GT86 Gen 1", "ACCEL PEDAL %",        "140", 1,  0,  8, 0.39215,    0, 1, false },
    { "Toyota", "GT86 Gen 1", "BRAKE %",              "0D1", 1, 16,  8, 1.42857,    0, 0, false },
    { "Toyota", "GT86 Gen 1", "BRAKE PRESSURE",       "0D1", 1, 16,  8, 128.0,      0, 0, false },
    { "Toyota", "GT86 Gen 1", "COOLANT TEMP",         "360", 1, 24,  8, 1.0,      -40, 0, false },
    { "Toyota", "GT86 Gen 1", "ENGINE RPM",           "140", 1, 16, 14, 1.0,        0, 0, false },
    { "Toyota", "GT86 Gen 1", "OIL TEMP",             "360", 1, 16,  8, 1.0,      -40, 0, false },
    { "Toyota", "GT86 Gen 1", "VEHICLE SPEED",        "0D1", 1,  0, 16, 0.015694,   0, 1, true  },
    { "Toyota", "GT86 Gen 1", "STEERING ANGLE",       "0D0", 1,  0, 16, -0.1,       0, 1, true  },
    { "Toyota", "GT86 Gen 1", "LATERAL ACCEL",        "0D0", 1, 48,  8, 0.2,        0, 2, true  },
    { "Toyota", "GT86 Gen 1", "LONGITUDINAL ACCEL",   "0D0", 1, 56,  8, -0.1,       0, 2, true  },
    { "Toyota", "GT86 Gen 1", "THROTTLE %",           "140", 1, 48,  8, 0.39215,    0, 1, false },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD FL",         "0D4", 1,  0, 16, 0.015694,   0, 1, true  },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD FR",         "0D4", 1, 16, 16, 0.015694,   0, 1, true  },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD RL",         "0D4", 1, 32, 16, 0.015694,   0, 1, true  },
    { "Toyota", "GT86 Gen 1", "WHEEL SPD RR",         "0D4", 1, 48, 16, 0.015694,   0, 1, true  },
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
{ "RDM-7", "Internal", "UPTIME S",        "0", 1, 0, 16, 1.0,  0, 0, false },
{ "RDM-7", "Internal", "WIFI RSSI",       "0", 1, 0, 16, 1.0,  0, 0, true  },

{ NULL, NULL, NULL, NULL, 0, 0, 0, 0.0, 0, 0, false }
};

const int preconfig_items_count = sizeof(preconfig_items)/sizeof(preconfig_items[0]);
static const int preconfig_data_count = sizeof(preconfig_items)/sizeof(preconfig_items[0]);

/* =========================================================================
 * Full-screen 3-column preset browser
 *
 * Layout (780 × 456 centred panel):
 *  ┌─ SELECT PRESET ─────────────────────────────────────────── [CLOSE] ─┐ 48px
 *  ├──────────────┬────────────────┬──────────────────────────────────────┤ 356px
 *  │  BRAND       │  PROTOCOL      │  CHANNEL                             │
 *  │  MaxxECU ●  │  v1.2          │  THROTTLE %                          │
 *  │  Haltech     │  v1.3 ●       │  MAP                                 │
 *  │  Ford        │                │  LAMBDA ...  (scrollable)            │
 *  ├──────────────┴────────────────┴──────────────────────────────────────┤ 52px
 *  │  ○ No channel selected                           [✓  APPLY PRESET]  │
 *  └──────────────────────────────────────────────────────────────────────┘
 * ========================================================================= */

/* ── State ──────────────────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *ver_list;       /* inner flex container – version column  */
    lv_obj_t *sig_list;       /* inner flex container – channel column  */
    lv_obj_t *preview_lbl;
    lv_obj_t *apply_btn;
    char      sel_brand[32];
    char      sel_ver[32];
    int       sel_sig;        /* index into preconfig_items[], -1 = none */
    lv_obj_t *hi_brand;
    lv_obj_t *hi_ver;
    lv_obj_t *hi_sig;
    preset_apply_cb_t  apply_cb;
    void              *apply_cb_ctx;
} picker_st_t;

typedef struct { picker_st_t *st; const char *name; } col_txt_ctx_t;
typedef struct { picker_st_t *st; int idx; }           col_sig_ctx_t;

/* Forward declarations of populate helpers (used by click callbacks) */
static void populate_ver_col(picker_st_t *st);
static void populate_sig_col(picker_st_t *st);
static void update_picker_preview(picker_st_t *st, int idx);

/* ── Memory free callbacks ───────────────────────────────────────────────── */
static void picker_st_free_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    picker_st_t *st = (picker_st_t *)lv_event_get_user_data(e);
    if (st) lv_mem_free(st);
}
static void col_txt_free_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    col_txt_ctx_t *c = (col_txt_ctx_t *)lv_event_get_user_data(e);
    if (c) lv_mem_free(c);
}
static void col_sig_free_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    col_sig_ctx_t *c = (col_sig_ctx_t *)lv_event_get_user_data(e);
    if (c) lv_mem_free(c);
}

/* ── Row highlight (accent left-bar + bright text) ───────────────────────── */
static void set_row_hi(lv_obj_t *row, bool on)
{
    if (!row || !lv_obj_is_valid(row)) return;
    lv_obj_set_style_bg_color(row, on ? THEME_COLOR_ACCENT_DIM : THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(row, on ? 3 : 0, 0);
    lv_obj_set_style_border_side(row,  LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, THEME_COLOR_ACCENT, 0);
    lv_obj_t *lbl = lv_obj_get_child(row, 0);
    if (lbl) lv_obj_set_style_text_color(lbl,
        on ? THEME_COLOR_TEXT_PRIMARY : THEME_COLOR_TEXT_MUTED, 0);
}

/* ── Click callbacks ─────────────────────────────────────────────────────── */
static void brand_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    col_txt_ctx_t *ctx = (col_txt_ctx_t *)lv_event_get_user_data(e);
    picker_st_t *st = ctx->st;
    set_row_hi(st->hi_brand, false);
    st->hi_brand = lv_event_get_target(e);
    set_row_hi(st->hi_brand, true);
    strncpy(st->sel_brand, ctx->name, sizeof(st->sel_brand) - 1);
    populate_ver_col(st);
}
static void ver_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    col_txt_ctx_t *ctx = (col_txt_ctx_t *)lv_event_get_user_data(e);
    picker_st_t *st = ctx->st;
    set_row_hi(st->hi_ver, false);
    st->hi_ver = lv_event_get_target(e);
    set_row_hi(st->hi_ver, true);
    strncpy(st->sel_ver, ctx->name, sizeof(st->sel_ver) - 1);
    populate_sig_col(st);
}
/* Core apply logic — shared by channel-click (embedded picker, auto-apply)
 * and the Apply button (standalone overlay). */
static void _apply_selection(picker_st_t *st)
{
    if (!st->apply_cb || st->sel_sig < 0) return;
    const preconfig_item_t *it = &preconfig_items[st->sel_sig];
    st->apply_cb(it, st->apply_cb_ctx);
    if (st->preview_lbl) {
        char buf[96];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Applied: %s", it->label);
        lv_label_set_text(st->preview_lbl, buf);
        lv_obj_set_style_text_color(st->preview_lbl, THEME_COLOR_GREEN, 0);
    }
}

static void sig_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    col_sig_ctx_t *ctx = (col_sig_ctx_t *)lv_event_get_user_data(e);
    picker_st_t *st = ctx->st;
    set_row_hi(st->hi_sig, false);
    st->hi_sig = lv_event_get_target(e);
    set_row_hi(st->hi_sig, true);
    st->sel_sig = ctx->idx;
    update_picker_preview(st, ctx->idx);
    /* Auto-apply on channel selection — no separate Apply step needed.
     * The modal's SAVE button is the sole commit-to-disk action; pending
     * edits (including this preset binding) go out together on SAVE. */
    _apply_selection(st);
}

/* ── Preview footer update ───────────────────────────────────────────────── */
static void update_picker_preview(picker_st_t *st, int idx)
{
    if (!st->preview_lbl) return;
    /* Reset colour in case it was set to green by a previous Apply */
    lv_obj_set_style_text_color(st->preview_lbl, THEME_COLOR_TEXT_MUTED, 0);
    if (idx < 0) {
        lv_label_set_text(st->preview_lbl, "Select a brand, protocol, then channel");
        if (st->apply_btn) lv_obj_add_state(st->apply_btn, LV_STATE_DISABLED);
    } else {
        const preconfig_item_t *it = &preconfig_items[idx];
        char buf[128];
        snprintf(buf, sizeof(buf), "%s | CAN 0x%s | %s | Bit %d  Len %d | x%.4g",
            it->label, it->can_id,
            it->endianess ? "LE" : "BE",
            it->bit_start, it->bit_length,
            (double)it->scale);
        lv_label_set_text(st->preview_lbl, buf);
        if (st->apply_btn) lv_obj_clear_state(st->apply_btn, LV_STATE_DISABLED);
    }
}

/* ── Column builder helpers ──────────────────────────────────────────────── */

/* Scrollable flex-column container that fills remaining height of its parent */
static lv_obj_t *make_col_list(lv_obj_t *col)
{
    lv_obj_t *list = lv_obj_create(col);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_style_bg_color(list, THEME_COLOR_SCROLLBAR,
                               LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    return list;
}

/* One complete column (header strip + scrollable list), returns the list obj */
static lv_obj_t *make_col(lv_obj_t *body, const char *hdr_text, bool right_border,
                           lv_coord_t col_w, lv_coord_t col_h)
{
    lv_obj_t *col = lv_obj_create(body);
    lv_obj_set_size(col, col_w, col_h);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    if (right_border) {
        lv_obj_set_style_border_side(col, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(col, THEME_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(col, 1, 0);
    }

    /* Column header label strip */
    lv_obj_t *chdr = lv_obj_create(col);
    lv_obj_set_size(chdr, lv_pct(100), 28);
    lv_obj_clear_flag(chdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(chdr, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(chdr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chdr, 0, 0);
    lv_obj_set_style_border_width(chdr, 0, 0);
    lv_obj_set_style_border_side(chdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(chdr, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(chdr, 1, 0);
    lv_obj_set_style_pad_left(chdr, 12, 0);
    lv_obj_set_style_pad_right(chdr, 6, 0);
    lv_obj_set_style_pad_top(chdr, 0, 0);
    lv_obj_set_style_pad_bottom(chdr, 0, 0);

    lv_obj_t *clbl = lv_label_create(chdr);
    lv_label_set_text(clbl, hdr_text);
    lv_obj_set_style_text_color(clbl, THEME_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(clbl, THEME_FONT_SMALL, 0);
    lv_obj_align(clbl, LV_ALIGN_LEFT_MID, 0, 0);

    return make_col_list(col);
}

/* A single clickable row inside a column list */
static lv_obj_t *make_col_row(lv_obj_t *list, const char *text)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 6, 0);
    lv_obj_set_style_pad_top(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_set_style_bg_color(row, THEME_COLOR_INPUT_BG, LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, lv_pct(90));
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_BODY, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

/* ── Populate helpers ────────────────────────────────────────────────────── */
/* ── Auto-select the brand + version saved in NVS (config_store_load_ecu).
 * Called right after the brand list is populated. If the saved ECU maps
 * to a row in the list, highlight it and cascade to populate the version
 * column; if a matching version is present, highlight that too. No-op if
 * the NVS key is empty (Custom / None / first run). */
static void _preselect_ecu_from_nvs(picker_st_t *st, lv_obj_t *brand_list)
{
    if (!st || !brand_list) return;
    char ecu_make[32] = {0}, ecu_ver[32] = {0};
    if (config_store_load_ecu(ecu_make, sizeof(ecu_make),
                              ecu_ver, sizeof(ecu_ver)) != ESP_OK)
        return;
    if (ecu_make[0] == '\0') return;

    uint32_t nb = lv_obj_get_child_cnt(brand_list);
    for (uint32_t i = 0; i < nb; i++) {
        lv_obj_t *row = lv_obj_get_child(brand_list, i);
        if (!row) continue;
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (!lbl) continue;
        const char *txt = lv_label_get_text(lbl);
        if (!txt || strcmp(txt, ecu_make) != 0) continue;

        strncpy(st->sel_brand, ecu_make, sizeof(st->sel_brand) - 1);
        set_row_hi(row, true);
        st->hi_brand = row;
        populate_ver_col(st);
        if (ecu_ver[0] == '\0') return;

        uint32_t nv = lv_obj_get_child_cnt(st->ver_list);
        for (uint32_t j = 0; j < nv; j++) {
            lv_obj_t *vrow = lv_obj_get_child(st->ver_list, j);
            if (!vrow) continue;
            lv_obj_t *vlbl = lv_obj_get_child(vrow, 0);
            if (!vlbl) continue;
            const char *vtxt = lv_label_get_text(vlbl);
            if (!vtxt || strcmp(vtxt, ecu_ver) != 0) continue;
            strncpy(st->sel_ver, ecu_ver, sizeof(st->sel_ver) - 1);
            set_row_hi(vrow, true);
            st->hi_ver = vrow;
            populate_sig_col(st);
            return;
        }
        return;
    }
}

static void populate_ver_col(picker_st_t *st)
{
    lv_obj_clean(st->ver_list);
    lv_obj_clean(st->sig_list);
    st->hi_ver = st->hi_sig = NULL;
    st->sel_sig = -1;
    st->sel_ver[0] = '\0';
    update_picker_preview(st, -1);

    const char *vers[16]; int nv = 0;
    for (int i = 0; i < preconfig_data_count - 1 && preconfig_items[i].ecu; i++) {
        if (strcmp(preconfig_items[i].ecu, st->sel_brand) != 0) continue;
        bool dup = false;
        for (int j = 0; j < nv; j++)
            if (strcmp(vers[j], preconfig_items[i].version) == 0) { dup = true; break; }
        if (!dup && nv < 16) vers[nv++] = preconfig_items[i].version;
    }
    /* Sort protocols alphabetically */
    for (int i = 0; i < nv - 1; i++)
        for (int j = i + 1; j < nv; j++)
            if (strcmp(vers[i], vers[j]) > 0) {
                const char *tmp = vers[i]; vers[i] = vers[j]; vers[j] = tmp;
            }
    for (int i = 0; i < nv; i++) {
        lv_obj_t *row = make_col_row(st->ver_list, vers[i]);
        col_txt_ctx_t *ctx = lv_mem_alloc(sizeof(col_txt_ctx_t));
        ctx->st = st; ctx->name = vers[i];
        lv_obj_add_event_cb(row, ver_click_cb,   LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, col_txt_free_cb, LV_EVENT_DELETE, ctx);
    }
}

static void populate_sig_col(picker_st_t *st)
{
    lv_obj_clean(st->sig_list);
    st->hi_sig = NULL;
    st->sel_sig = -1;
    update_picker_preview(st, -1);

    /* Collect matching indices */
    int idxs[256]; int nc = 0;
    for (int i = 0; i < preconfig_data_count - 1 && preconfig_items[i].ecu; i++) {
        if (strcmp(preconfig_items[i].ecu,     st->sel_brand) != 0) continue;
        if (strcmp(preconfig_items[i].version, st->sel_ver)   != 0) continue;
        if (nc < 256) idxs[nc++] = i;
    }
    /* Sort channels alphabetically by label */
    for (int i = 0; i < nc - 1; i++)
        for (int j = i + 1; j < nc; j++)
            if (strcmp(preconfig_items[idxs[i]].label,
                       preconfig_items[idxs[j]].label) > 0) {
                int tmp = idxs[i]; idxs[i] = idxs[j]; idxs[j] = tmp;
            }
    /* Create rows in sorted order */
    for (int k = 0; k < nc; k++) {
        lv_obj_t *row = make_col_row(st->sig_list, preconfig_items[idxs[k]].label);
        col_sig_ctx_t *ctx = lv_mem_alloc(sizeof(col_sig_ctx_t));
        ctx->st = st; ctx->idx = idxs[k];
        lv_obj_add_event_cb(row, sig_click_cb,   LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, col_sig_free_cb, LV_EVENT_DELETE, ctx);
    }
}

/* ── Embedded picker (for config modal PRESETS tab) ─────────────────────── */

static void _populate_brands(lv_obj_t *brand_list, picker_st_t *st)
{
    const char *brands[16]; int nb = 0;
    for (int i = 0; i < preconfig_data_count - 1 && preconfig_items[i].ecu; i++) {
        bool dup = false;
        for (int j = 0; j < nb; j++)
            if (strcmp(brands[j], preconfig_items[i].ecu) == 0) { dup = true; break; }
        if (!dup && nb < 16) brands[nb++] = preconfig_items[i].ecu;
    }
    for (int i = 0; i < nb - 1; i++)
        for (int j = i + 1; j < nb; j++)
            if (strcmp(brands[i], brands[j]) > 0) {
                const char *tmp = brands[i]; brands[i] = brands[j]; brands[j] = tmp;
            }
    for (int i = 0; i < nb; i++) {
        lv_obj_t *row = make_col_row(brand_list, brands[i]);
        col_txt_ctx_t *ctx = lv_mem_alloc(sizeof(col_txt_ctx_t));
        ctx->st = st; ctx->name = brands[i];
        lv_obj_add_event_cb(row, brand_click_cb,  LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, col_txt_free_cb, LV_EVENT_DELETE, ctx);
    }

    _preselect_ecu_from_nvs(st, brand_list);
}

void build_preset_picker_embedded(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                   preset_apply_cb_t cb, void *ctx)
{
    picker_st_t *st = lv_mem_alloc(sizeof(picker_st_t));
    memset(st, 0, sizeof(*st));
    st->sel_sig      = -1;
    st->overlay      = NULL;
    st->apply_cb     = cb;
    st->apply_cb_ctx = ctx;

    lv_obj_add_event_cb(parent, picker_st_free_cb, LV_EVENT_DELETE, st);

    /* Configure parent as vertical flex, zero padding */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    lv_coord_t footer_h = 42;
    lv_coord_t body_h   = h - footer_h;
    lv_coord_t col_w    = w / 3;

    /* ── 3-column body (flex-row) ────────────────────────────────── */
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_set_size(body, w, body_h);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_column(body, 0, 0);

    lv_obj_t *brand_list = make_col(body, "BRAND",    true,  col_w, body_h);
    st->ver_list          = make_col(body, "PROTOCOL", true,  col_w, body_h);
    st->sig_list          = make_col(body, "CHANNEL",  false, col_w, body_h);

    /* ── Footer (preview + Apply) ────────────────────────────────── */
    lv_obj_t *footer = lv_obj_create(parent);
    lv_obj_set_size(footer, w, footer_h);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(footer, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(footer, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_pad_left(footer, 10, 0);
    lv_obj_set_style_pad_right(footer, 10, 0);
    lv_obj_set_style_pad_top(footer, 0, 0);
    lv_obj_set_style_pad_bottom(footer, 0, 0);

    lv_obj_t *prev_lbl = lv_label_create(footer);
    lv_label_set_text(prev_lbl, "Select a brand, protocol, then channel");
    lv_obj_set_style_text_color(prev_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(prev_lbl, THEME_FONT_SMALL, 0);
    lv_label_set_long_mode(prev_lbl, LV_LABEL_LONG_DOT);
    /* Apply button was removed (channel click auto-applies) — preview
     * label takes the full footer width and stays horizontally centered. */
    lv_obj_set_width(prev_lbl, w - 20);
    lv_obj_align(prev_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    st->preview_lbl = prev_lbl;
    st->apply_btn   = NULL;

    /* ── Populate brands ─────────────────────────────────────────── */
    _populate_brands(brand_list, st);
}

