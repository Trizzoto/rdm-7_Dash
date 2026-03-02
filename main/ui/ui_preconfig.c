#include "ui_preconfig.h"
#include <stdio.h>
#include "lvgl.h"
#include "screens/ui_Screen3.h"
#include "widgets/lv_dropdown.h"
#include <string.h>
#include "device_settings.h"

extern value_config_t values_config[13];
extern uint8_t current_value_id;

// Forward declarations
static void delayed_version_event_cb(lv_timer_t * timer);

// Local static pointers to the Pre-configurations objects
static lv_obj_t * ui_Border_2 = NULL;
static lv_obj_t * ui_Preconfig_Text = NULL;
static lv_obj_t * ui_ECU_Text = NULL;
static lv_obj_t * ui_ECU_Input = NULL;
static lv_obj_t * ui_Version_Text = NULL;
static lv_obj_t * ui_Version_Input = NULL;
static lv_obj_t * ui_ID_Text = NULL;
static lv_obj_t * ui_ID_Input = NULL;

// A single "preconfig record"
typedef struct {
    const char* ecu;           // e.g. "MaxxECU"
    const char* version;       // e.g. "1.3"
    const char* label;         // e.g. "Lambda"
    const char* can_id;        // Store as string to preserve exact format
    uint8_t endianess;         // 0 = Big Endian, 1 = Little Endian
    uint8_t bit_start;
    uint8_t bit_length;
    float scale;
    float value_offset;
    uint8_t decimals;
    bool is_signed;           // true = Signed, false = Unsigned
} preconfig_item_t;

const preconfig_item_t preconfig_items[] = {
    { "MaxxECU", "1.2", "THROTTLE %", "520", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "MAP", "520", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "LAMBDA", "520", 1, 48, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA A", "521", 1, 0, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "LAMBDA B", "521", 1, 16, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.2", "IGNITION ANGLE", "521", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "IGNITION CUT", "521", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "FUEL PULSEWIDTH PRIMARY", "522", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.2", "FUEL DUTY PRIMARY", "522", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "FUEL CUT", "522", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "VEHICLE SPEED", "522", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "UNDRIVEN WHEELS AVG SPD", "523", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "DRIVEN WHEELS AVG SPD", "523", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "WHEEL SLIP", "523", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TARGET SLIP", "523", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TRACTION CTRL POWER LIMIT", "524", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "LAMBDA CORR A", "524", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "LAMBDA CORR B", "524", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "FIRMWARE VERSION", "524", 1, 48, 16, 0.001, 0, 0, false },
    { "MaxxECU", "1.2", "BATTERY VOLTAGE", "530", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.2", "BARO PRESSURE", "530", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "INTAKE AIR TEMP", "530", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "COOLANT TEMP", "530", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TOTAL FUEL TRIM", "531", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "E85 %", "531", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "TOTAL IGNITION COMP", "531", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 1", "531", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 2", "532", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 3", "532", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 4", "532", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 5", "532", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 6", "533", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 7", "533", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT 8", "533", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT HIGHEST", "533", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "EGT DIFFERENCE", "534", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "CPU TEMP", "534", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "ERROR CODE COUNT", "534", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "LOST SYNC COUNT", "534", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 1", "535", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 2", "535", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 3", "535", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "USER ANALOG INPUT 4", "535", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.2", "GEAR", "536", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.2", "BOOST SOLENOID DUTY", "536", 1, 16, 16, 0.1, 0, 0, false },

    { "MaxxECU", "1.3", "OIL PRESSURE", "536", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "OIL TEMP", "536", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL PRESSURE 1", "537", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "WASTEGATE PRESSURE", "537", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "COOLANT PRESSURE", "537", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "BOOST TARGET", "537", 1, 48, 16, 0.1, 0, 0, false },
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
    { "MaxxECU", "1.3", "SHIFTCUT ACTIVE", "526", 1, 0, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "REV-LIMIT ACTIVE", "526", 1, 1, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "ANTI-LAG ACTIVE", "526", 1, 2, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "LAUNCH CONTROL ACTIVE", "526", 1, 3, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "TRACTION POWER LIMITER ACTIVE", "526", 1, 4, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "THROTTLE BLIP ACTIVE", "526", 1, 5, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "AC/IDLE UP ACTIVE", "526", 1, 6, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCK DETECTED", "526", 1, 7, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "BRAKE PEDAL ACTIVE", "526", 1, 8, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "CLUTCH PEDAL ACTIVE", "526", 1, 9, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "SPEED LIMIT ACTIVE", "526", 1, 10, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "GP LIMITER ACTIVE", "526", 1, 11, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "USER CUT ACTIVE", "526", 1, 12, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "ECU IS LOGGING", "526", 1, 13, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "NITROUS ACTIVE", "526", 1, 14, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 15, 1, 0, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "REV-LIMIT RPM", "526", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "SPARE", "526", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "ACCELERATION FORWARD", "527", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "ACCELERATION RIGHT", "527", 1, 16, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "ACCELERATION UP", "527", 1, 32, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA TARGET", "527", 1, 48, 16, 0.001, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCKLEVEL ALL PEAK", "528", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCK CORRECTION", "528", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "KNOCK COUNT", "528", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "LAST KNOCK CYLINDER", "528", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "ACTIVE BOOST TABLE", "540", 1, 0, 8, 1, 0, 0, false },
    { "MaxxECU", "1.3", "ACTIVE TUNE SELECTOR", "540", 1, 8, 8, 1, 0, 0, false },
    { "MaxxECU", "1.3", "VIRTUAL FUEL TANK", "540", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TRANSMISSION TEMP", "540", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "DIFFERENTIAL TEMP", "540", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT INTAKE CAM 1 POS", "541", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT EXHAUST CAM 1 POS", "541", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT INTAKE CAM 2 POS", "541", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "VVT EXHAUST CAM 2 POS", "541", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "THROTTLE %", "520", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "MAP", "520", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA", "520", 1, 48, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA A", "521", 1, 0, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "LAMBDA B", "521", 1, 16, 16, 0.001, 0, 2, false },
    { "MaxxECU", "1.3", "IGNITION ANGLE", "521", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "IGNITION CUT", "521", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL PULSEWIDTH PRIMARY", "522", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL DUTY PRIMARY", "522", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "FUEL CUT", "522", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "VEHICLE SPEED", "522", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "UNDRIVEN WHEELS AVG SPD", "523", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "DRIVEN WHEELS AVG SPD", "523", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "WHEEL SLIP", "523", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TARGET SLIP", "523", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TRACTION CTRL POWER LIMIT", "524", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA CORR A", "524", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "LAMBDA CORR B", "524", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "FIRMWARE VERSION", "524", 1, 48, 16, 0.001, 0, 0, false },
    { "MaxxECU", "1.3", "BATTERY VOLTAGE", "530", 1, 0, 16, 0.01, 0, 0, false },
    { "MaxxECU", "1.3", "BARO PRESSURE", "530", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "INTAKE AIR TEMP", "530", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "COOLANT TEMP", "530", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TOTAL FUEL TRIM", "531", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "E85 %", "531", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "TOTAL IGNITION COMP", "531", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 1", "531", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 2", "532", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 3", "532", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 4", "532", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 5", "532", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 6", "533", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 7", "533", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT 8", "533", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT HIGHEST", "533", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "EGT DIFFERENCE", "534", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "CPU TEMP", "534", 1, 16, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "ERROR CODE COUNT", "534", 1, 32, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "LOST SYNC COUNT", "534", 1, 48, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 1", "535", 1, 0, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 2", "535", 1, 16, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 3", "535", 1, 32, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "USER ANALOG INPUT 4", "535", 1, 48, 16, 0.1, 0, 0, false },
    { "MaxxECU", "1.3", "GEAR", "536", 1, 0, 16, 1, 0, 0, false },
    { "MaxxECU", "1.3", "BOOST SOLENOID DUTY", "536", 1, 16, 16, 0.1, 0, 0, false },

{ "Haltech", "Nexus", "RPM", "360", 0, 0, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "MANIFOLD PRESSURE", "360", 0, 16, 16, 0.0145, 0, 1, false },
{ "Haltech", "Nexus", "THROTTLE POSITION", "360", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "COOLANT PRESSURE", "360", 0, 48, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "FUEL PRESSURE", "361", 0, 0, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "OIL PRESSURE", "361", 0, 16, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "ENGINE DEMAND", "361", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "WASTEGATE PRESS", "361", 0, 48, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "INJECTION STG1", "362", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "INJECTION STG2", "362", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "IGN ANGLE LEAD", "362", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "WHEEL SLIP", "363", 0, 0, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "WHEEL DIFF", "363", 0, 16, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "LAUNCH END RPM", "363", 0, 48, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "INJ STG1 TIME", "364", 0, 0, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "INJ STG2 TIME", "364", 0, 16, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "INJ STG3 TIME", "364", 0, 32, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "INJ STG4 TIME", "364", 0, 48, 16, 0.001, 0, 3, false },
{ "Haltech", "Nexus", "WIDEBAND 1", "368", 0, 0, 16, 0.0147, 0, 3, false },
{ "Haltech", "Nexus", "WIDEBAND 2", "368", 0, 16, 16, 0.0147, 0, 3, false },
{ "Haltech", "Nexus", "WIDEBAND 3", "368", 0, 32, 16, 0.0147, 0, 3, false },
{ "Haltech", "Nexus", "WIDEBAND 4", "368", 0, 48, 16, 0.0147, 0, 3, false },
{ "Haltech", "Nexus", "TRIGGER ERR CNT", "369", 0, 0, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "TRIGGER COUNT", "369", 0, 16, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "TRIGGER SYNC", "369", 0, 48, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "KNOCK LEVEL 1", "36A", 0, 0, 16, 0.01, 0, 2, false },
{ "Haltech", "Nexus", "KNOCK LEVEL 2", "36A", 0, 16, 16, 0.01, 0, 2, false },
{ "Haltech", "Nexus", "BRAKE PRESSURE", "36B", 0, 0, 16, 0.0145, -14.7, 1, false },
{ "Haltech", "Nexus", "NOS PRESSURE", "36B", 0, 16, 16, 0.0319, -14.7, 1, false },
{ "Haltech", "Nexus", "TURBO SPEED", "36B", 0, 32, 16, 10.0, 0, 0, false },
{ "Haltech", "Nexus", "LATERAL G", "36B", 0, 48, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "ENGINE LIMIT", "36E", 0, 0, 16, 1.0, 0, 0, false },
{ "Haltech", "Nexus", "LC IGN RETARD", "36E", 0, 16, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "LC FUEL ENRICH", "36E", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "LONGITUDINAL G", "36E", 0, 48, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "VEHICLE SPEED", "370", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "INTAKE CAM 1", "370", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "INTAKE CAM 2", "370", 0, 48, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "BATTERY VOLT", "372", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 1", "373", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 2", "373", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "AMBIENT AIR TEMP", "376", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "REL HUMIDITY", "376", 0, 16, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "EGT SENSOR 3", "373", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 4", "373", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 5", "374", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 6", "374", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 7", "374", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 8", "374", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 9", "375", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 10", "375", 0, 16, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 11", "375", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "EGT SENSOR 12", "375", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "SPEC HUMIDITY", "376", 0, 32, 16, 100.0, 0, 0, false },
{ "Haltech", "Nexus", "ABS HUMIDITY", "376", 0, 48, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "COOLANT TEMP", "3E0", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "AIR TEMP", "3E0", 0, 16, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "FUEL TEMP", "3E0", 0, 32, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "OIL TEMP", "3E0", 0, 48, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "GEARBOX OIL TEMP", "3E1", 0, 0, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "DIFF OIL TEMP", "3E1", 0, 16, 16, 0.1, -273.15, 1, false },
{ "Haltech", "Nexus", "FUEL COMP", "3E1", 0, 32, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "FUEL LEVEL", "3E2", 0, 0, 16, 0.1, 0, 1, false },
{ "Haltech", "Nexus", "FUEL TRIM ST B1", "3E3", 0, 0, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "FUEL TRIM ST B2", "3E3", 0, 16, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "FUEL TRIM LT B1", "3E3", 0, 32, 16, 0.1, 0, 1, true },
{ "Haltech", "Nexus", "FUEL TRIM LT B2", "3E3", 0, 48, 16, 0.1, 0, 1, true },

// { "Make", "Model", "Name", "ID", Bus/Endian, Start, Len, Scale, Offset, Decimals, Signed }

/* BA/BF Engine & Powertrain (PCM Broadcast) */
{ "Ford", "BA/BF", "ENGINE RPM",      "201", 1, 0,  16, 0.25,     0,   0, false },
{ "Ford", "BA/BF", "THROTTLE %",      "201", 1, 16, 16, 0.00152,  0,   1, false },
{ "Ford", "BA/BF", "ENGINE TORQUE",   "201", 1, 32, 12, 1.0,      0,   0, true  },

/* BA/BF Speed (ABS Module) */
{ "Ford", "BA/BF", "VEHICLE SPEED",   "415", 1, 0,  16, 0.01,     0,   2, false },

/* BA/BF Temperatures (Cluster Feed) */
{ "Ford", "BA/BF", "COOLANT TEMP",    "420", 1, 0,  8,  1.0,    -40,   0, false },

// { "Make", "Model", "Name", "ID", Bus/Endian, Start, Len, Scale, Offset, Decimals, Signed }

/* FG Engine Data (PCM) */
{ "Ford", "FG",    "ENGINE RPM",      "109", 1, 0,  16, 0.25,     0,   0, false },
{ "Ford", "FG",    "VEHICLE SPEED",   "109", 1, 32, 16, 0.01,     0,   2, false }, // PCM Speed Source

/* FG Temperatures (Broadcast) */
{ "Ford", "FG",    "COOLANT TEMP",    "156", 1, 0,  8,  1.0,    -60,   0, false },
{ "Ford", "FG",    "OIL TEMP",        "156", 1, 8,  8,  1.0,    -60,   0, false },

/* FG Pedals & Steering */
{ "Ford", "FG",    "ACCEL PEDAL %",   "204", 1, 0,  16, 0.01,     0,   1, false },
{ "Ford", "FG",    "STEER ANGL",      "082", 1, 0,  16, 0.1,      0,   1, true  }, // Signed (+/-)
{ "Ford", "FG",    "BRAKE SWITCH",    "060", 1, 18, 1,  1.0,      0,   0, false },

/* FG Transmission (ZF 6HP) */
{ "Ford", "FG",    "GEAR (BITMASK)",  "171", 1, 0,  8,  1.0,      0,   0, false },
{ "Ford", "FG",    "SHIFTER POS",     "191", 1, 0,  8,  1.0,      0,   0, false },

/* FG Wheel Speeds (ABS) */
{ "Ford", "FG",    "WHEEL SPD FL",    "217", 1, 0,  16, 0.01,     0,   2, false },
{ "Ford", "FG",    "WHEEL SPD FR",    "217", 1, 16, 16, 0.01,     0,   2, false },
{ "Ford", "FG",    "WHEEL SPD RL",    "217", 1, 32, 16, 0.01,     0,   2, false },
{ "Ford", "FG",    "WHEEL SPD RR",    "217", 1, 48, 16, 0.01,     0,   2, false },

/* FG Fuel */
{ "Ford", "FG",    "FUEL LEVEL %",    "320", 1, 0,  16, 0.01,     0,   1, false },


{ NULL, NULL, NULL, NULL, 0, 0, 0, 0.0, 0, 0, false } // Keep this terminator entry at the end
};

static const int preconfig_data_count = sizeof(preconfig_items)/sizeof(preconfig_items[0]);

static void ecu_dropdown_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        if (!dropdown) return;

        // 1) Get the new ECU selection text
        char selected_txt[64] = {0};
        lv_dropdown_get_selected_str(dropdown, selected_txt, sizeof(selected_txt));
        if (selected_txt[0] == '\0') return;  // Safety check
        
        // 2) Based on "selected_txt", find which Versions exist in "preconfig_items"
        char versions[512] = {0};  // Buffer for versions
        bool first_version_added = false;
        
        // Safety check for NULL terminator in preconfig_items
        for(int i = 0; i < preconfig_data_count && preconfig_items[i].ecu != NULL; i++) {
            if (strcmp(preconfig_items[i].ecu, selected_txt) == 0) {
                // Check if this version is already in our list
                bool version_exists = false;
                if (versions[0] != '\0') {  // If we have any versions already
                    char *version_list = strdup(versions);  // Make a copy for strtok
                    if (version_list) {
                        char *token = strtok(version_list, "\n");
                        while (token) {
                            if (strcmp(token, preconfig_items[i].version) == 0) {
                                version_exists = true;
                                break;
                            }
                            token = strtok(NULL, "\n");
                        }
                        free(version_list);
                    }
                }

                // If version not already in list, add it
                if (!version_exists) {
                    if (first_version_added) {
                        strncat(versions, "\n", sizeof(versions) - strlen(versions) - 1);
                    }
                    strncat(versions, preconfig_items[i].version, sizeof(versions) - strlen(versions) - 1);
                    first_version_added = true;
                }
            }
        }

        // 3) Update the version dropdown with these filtered items
        if (ui_Version_Input && versions[0] != '\0') {
            lv_dropdown_set_options(ui_Version_Input, versions);
            
            // Auto-select based on saved version from device settings
            uint8_t saved_ecu = get_selected_ecu_preconfig();
            uint8_t saved_version = get_selected_ecu_version();
            
            // Only auto-select if the current ECU matches the saved ECU
            if ((saved_ecu == 1 && strcmp(selected_txt, "MaxxECU") == 0) ||
                (saved_ecu == 2 && strcmp(selected_txt, "Haltech") == 0) ||
                (saved_ecu == 3 && strcmp(selected_txt, "Ford") == 0)) {
                
                if (saved_ecu == 1) { // MaxxECU
                    // saved_version: 0="1.2" (first option), 1="1.3" (second option)
                    const char* target_version = (saved_version == 0) ? "1.2" : "1.3";
                    if (strstr(versions, target_version) != NULL) {
                        // Find index of target_version in dropdown
                        char *versions_copy = strdup(versions);
                        if (versions_copy) {
                            int index = 0;
                            char *token = strtok(versions_copy, "\n");
                            while (token) {
                                if (strcmp(token, target_version) == 0) {
                                    lv_dropdown_set_selected(ui_Version_Input, index);
                                    break;
                                }
                                token = strtok(NULL, "\n");
                                index++;
                            }
                            free(versions_copy);
                        }
                    } else {
                        lv_dropdown_set_selected(ui_Version_Input, 0); // Default to first
                    }
                } else if (saved_ecu == 2) { // Haltech
                    // Haltech only has "Nexus", so just select it
                    if (strstr(versions, "Nexus") != NULL) {
                        char *versions_copy = strdup(versions);
                        if (versions_copy) {
                            int index = 0;
                            char *token = strtok(versions_copy, "\n");
                            while (token) {
                                if (strcmp(token, "Nexus") == 0) {
                                    lv_dropdown_set_selected(ui_Version_Input, index);
                                    break;
                                }
                                token = strtok(NULL, "\n");
                                index++;
                            }
                            free(versions_copy);
                        }
                    } else {
                        lv_dropdown_set_selected(ui_Version_Input, 0); // Default to first
                    }
                } else if (saved_ecu == 3) { // Ford
                    // Ford only has "BA/BF/FG", so just select it
                    if (strstr(versions, "BA/BF/FG") != NULL) {
                        char *versions_copy = strdup(versions);
                        if (versions_copy) {
                            int index = 0;
                            char *token = strtok(versions_copy, "\n");
                            while (token) {
                                if (strcmp(token, "BA/BF/FG") == 0) {
                                    lv_dropdown_set_selected(ui_Version_Input, index);
                                    break;
                                }
                                token = strtok(NULL, "\n");
                                index++;
                            }
                            free(versions_copy);
                        }
                    } else {
                        lv_dropdown_set_selected(ui_Version_Input, 0); // Default to first
                    }
                }
                
            } else {
                lv_dropdown_set_selected(ui_Version_Input, 0);
            }
            
            // Use timer to delay the version dropdown event trigger
            // This ensures the dropdown selection has taken effect first
            lv_timer_t * version_event_timer = lv_timer_create(delayed_version_event_cb, 10, NULL);
            lv_timer_set_repeat_count(version_event_timer, 1);
        } else if (ui_Version_Input) {
            lv_dropdown_set_options(ui_Version_Input, "No versions");
            // Clear the ID dropdown if no versions available
            if (ui_ID_Input) {
                lv_dropdown_set_options(ui_ID_Input, "Select ECU and Version first"); 
            }
        }
    }
}

static void version_dropdown_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        if (!dropdown || !ui_ECU_Input) return;

        // 1) Get selected ECU
        char ecu_str[64] = {0};
        lv_dropdown_get_selected_str(ui_ECU_Input, ecu_str, sizeof(ecu_str));
        if (ecu_str[0] == '\0') {
            printf("DEBUG: version_dropdown_event_cb - ECU string is empty\n");
            return;
        }

        // 2) Get selected version
        char version_str[64] = {0};
        lv_dropdown_get_selected_str(dropdown, version_str, sizeof(version_str));
        if (version_str[0] == '\0') {
            printf("DEBUG: version_dropdown_event_cb - Version string is empty\n");
            return;
        }
        
        printf("DEBUG: version_dropdown_event_cb - ECU: '%s', Version: '%s'\n", ecu_str, version_str);

        // 3) Collect all matching labels for that ECU + version
        char* labels[256]; // Array to hold label pointers
        int label_count = 0;
        
        // First pass: collect all matching labels
        for(int i = 0; i < preconfig_data_count && preconfig_items[i].ecu != NULL; i++) {
            if ((strcmp(preconfig_items[i].ecu, ecu_str) == 0) &&
                (strcmp(preconfig_items[i].version, version_str) == 0))
            {
                if (label_count < 256) {
                    labels[label_count] = (char*)preconfig_items[i].label;
                    label_count++;
                }
            }
        }
        
        // Sort the labels alphabetically using bubble sort
        for (int i = 0; i < label_count - 1; i++) {
            for (int j = 0; j < label_count - i - 1; j++) {
                if (strcmp(labels[j], labels[j + 1]) > 0) {
                    // Swap
                    char* temp = labels[j];
                    labels[j] = labels[j + 1];
                    labels[j + 1] = temp;
                }
            }
        }
        
        // Build the final string with sorted labels
        // Increased buffer size to accommodate all MaxxECU 1.3 items (need ~4KB for ~150+ items)
        char id_labels[4096] = {0};
        for (int i = 0; i < label_count; i++) {
            if (i > 0) {
                strncat(id_labels, "\n", sizeof(id_labels) - strlen(id_labels) - 1);
            }
            strncat(id_labels, labels[i], sizeof(id_labels) - strlen(id_labels) - 1);
        }

        // Update the ID dropdown
        if (ui_ID_Input) {
            if (id_labels[0] != '\0') {
                printf("DEBUG: Setting ID dropdown options: '%s'\n", id_labels);
                lv_dropdown_set_options(ui_ID_Input, id_labels);
                lv_dropdown_set_selected(ui_ID_Input, 0);
            } else {
                printf("DEBUG: No ID labels found for ECU '%s' Version '%s'\n", ecu_str, version_str);
                lv_dropdown_set_options(ui_ID_Input, "No IDs available");
            }
        }
    }
}

static void delayed_version_event_cb(lv_timer_t * timer)
{
    if (ui_Version_Input) {
        lv_event_send(ui_Version_Input, LV_EVENT_VALUE_CHANGED, NULL);
    }
    lv_timer_del(timer);
}

static void id_dropdown_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    // 1) Gather ECU, version, label
    char ecu_str[64];
    lv_dropdown_get_selected_str(ui_ECU_Input, ecu_str, sizeof(ecu_str));

    char version_str[64];
    lv_dropdown_get_selected_str(ui_Version_Input, version_str, sizeof(version_str));

    lv_obj_t * dropdown = lv_event_get_target(e);
    char label_str[64];
    lv_dropdown_get_selected_str(dropdown, label_str, sizeof(label_str));

    // 2) Find the matching record
    const preconfig_item_t * found = NULL;
    for(int i = 0; i < preconfig_data_count; i++) {
        if ((strcmp(preconfig_items[i].ecu, ecu_str) == 0) &&
            (strcmp(preconfig_items[i].version, version_str) == 0) &&
            (strcmp(preconfig_items[i].label, label_str) == 0))
        {
            found = &preconfig_items[i];
            break;
        }
    }

    if (!found) return;

    // 3) Update the values_config
    uint8_t idx = (current_value_id <= 13) ? (current_value_id - 1) : 0;
    values_config[idx].can_id = (uint32_t)strtol(found->can_id, NULL, 16);
    values_config[idx].endianess    = found->endianess;
    values_config[idx].bit_start    = found->bit_start;
    values_config[idx].bit_length   = found->bit_length;
    values_config[idx].scale        = found->scale;
    values_config[idx].value_offset = found->value_offset;
    values_config[idx].decimals     = found->decimals;
    values_config[idx].is_signed    = found->is_signed;
    values_config[idx].enabled      = true;

    // Also set the label_texts array
    strncpy(label_texts[idx], found->label, sizeof(label_texts[idx]) - 1);
    label_texts[idx][sizeof(label_texts[idx]) - 1] = '\0';

    // Print debug
    printf("Auto-populated panel #%d => label=%s, CAN=%s, bit_start=%d, bit_len=%d, scale=%.3f\n",
           current_value_id, found->label, found->can_id, found->bit_start,
           found->bit_length, found->scale);

    // 4) Update the actual UI text controls so the user sees the changes
    // Label
    if (g_label_input[idx]) {
        lv_textarea_set_text(g_label_input[idx], found->label);
    }

    // CAN ID - directly copy the string from preconfig
    if (g_can_id_input[idx]) {
        lv_textarea_set_text(g_can_id_input[idx], found->can_id);
    }

    // Endian dropdown (0 => Big, 1 => Little)
    if (g_endian_dropdown[idx]) {
        if (found->endianess == 0) {
            lv_dropdown_set_selected(g_endian_dropdown[idx], 0); // Big Endian
        } else {
            lv_dropdown_set_selected(g_endian_dropdown[idx], 1); // Little Endian
        }
    }

    // Bit start
    if (g_bit_start_dropdown[idx]) {
        lv_dropdown_set_selected(g_bit_start_dropdown[idx], found->bit_start);
    }

    // Bit length
    if (g_bit_length_dropdown[idx]) {
        lv_dropdown_set_selected(g_bit_length_dropdown[idx], (found->bit_length - 1));
    }

    // Scale
    if (g_scale_input[idx]) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.6g", found->scale);
        lv_textarea_set_text(g_scale_input[idx], buf);
    }

    // Offset
    if (g_offset_input[idx]) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.6g", found->value_offset);
        lv_textarea_set_text(g_offset_input[idx], buf);
    }

    // Decimals
    if (g_decimals_dropdown[idx]) {
        lv_dropdown_set_selected(g_decimals_dropdown[idx], found->decimals);
    }

    // Add type dropdown update
    if (g_type_dropdown[idx]) {
        lv_dropdown_set_selected(g_type_dropdown[idx], found->is_signed ? 1 : 0); // 0 = Unsigned, 1 = Signed
    }
}

void show_preconfig_menu(lv_obj_t * parent)
{
    if (ui_Border_2 != NULL) {
        // Menu already created, make it visible
        lv_obj_clear_flag(ui_Border_2, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    
    // 2) Create a border or panel object (similar to your code)
    ui_Border_2 = lv_obj_create(parent);
    lv_obj_set_width(ui_Border_2, 373);
    lv_obj_set_height(ui_Border_2, 102);
    lv_obj_set_x(ui_Border_2, 201);
    lv_obj_set_y(ui_Border_2, -180);
    lv_obj_set_align(ui_Border_2, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Border_2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_Border_2, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Border_2, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Border_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Border_2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 3) Create "Pre-configurations" label
    ui_Preconfig_Text = lv_label_create(parent);
    lv_obj_set_width(ui_Preconfig_Text, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Preconfig_Text, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Preconfig_Text, 90);
    lv_obj_set_y(ui_Preconfig_Text, -216);
    lv_obj_set_align(ui_Preconfig_Text, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Preconfig_Text, "Pre-configurations");
    lv_obj_set_style_text_color(ui_Preconfig_Text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Preconfig_Text, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 4) ECU label
    ui_ECU_Text = lv_label_create(parent);
    lv_obj_set_width(ui_ECU_Text, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ECU_Text, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_ECU_Text, 39);
    lv_obj_set_y(ui_ECU_Text, -185);
    lv_obj_set_align(ui_ECU_Text, LV_ALIGN_CENTER);
    lv_label_set_text(ui_ECU_Text, "ECU:");
    lv_obj_set_style_text_color(ui_ECU_Text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_ECU_Text, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

        // 6) Version label (create labels and version dropdown BEFORE ECU auto-selection)
    ui_Version_Text = lv_label_create(parent);
    lv_obj_set_width(ui_Version_Text, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Version_Text, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Version_Text, 51);
    lv_obj_set_y(ui_Version_Text, -148);
    lv_obj_set_align(ui_Version_Text, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Version_Text, "Version:");
    lv_obj_set_style_text_color(ui_Version_Text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Version_Text, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 7) Version dropdown (create and style BEFORE ECU auto-selection)
    ui_Version_Input = lv_dropdown_create(parent);
    lv_dropdown_set_options(ui_Version_Input, "Select Version");
    lv_obj_add_event_cb(ui_Version_Input, version_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_width(ui_Version_Input, 109);
    lv_obj_set_height(ui_Version_Input, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Version_Input, 142);
    lv_obj_set_y(ui_Version_Input, -148);
    lv_obj_set_align(ui_Version_Input, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Version_Input, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_radius(ui_Version_Input, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_Version_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Version_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Version_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Version_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 5) ECU dropdown
    ui_ECU_Input = lv_dropdown_create(parent);
    lv_dropdown_set_options(ui_ECU_Input, "MaxxECU\nHaltech\nFord");
    lv_obj_add_event_cb(ui_ECU_Input, ecu_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_width(ui_ECU_Input, 109);
    lv_obj_set_height(ui_ECU_Input, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_ECU_Input, 142);
    lv_obj_set_y(ui_ECU_Input, -185);
    lv_obj_set_align(ui_ECU_Input, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ECU_Input, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_radius(ui_ECU_Input, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_ECU_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_ECU_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_ECU_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_ECU_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    
        // Auto-select based on saved ECU preconfig from device settings (AFTER all dropdowns are created)
    uint8_t saved_ecu = get_selected_ecu_preconfig();
    if (saved_ecu == 1) { // MaxxECU
        lv_dropdown_set_selected(ui_ECU_Input, 0); // MaxxECU is first in dropdown
    } else if (saved_ecu == 2) { // Haltech
        lv_dropdown_set_selected(ui_ECU_Input, 1); // Haltech is second in dropdown
    } else if (saved_ecu == 3) { // Ford
        lv_dropdown_set_selected(ui_ECU_Input, 2); // Ford is third in dropdown
    }
    
    // Trigger ECU dropdown event to update version dropdown (for any ECU)
    // This will automatically handle version selection through the ECU callback
    if (saved_ecu > 0) {
        lv_event_send(ui_ECU_Input, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // 8) ID label
    ui_ID_Text = lv_label_create(parent);
    lv_obj_set_width(ui_ID_Text, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ID_Text, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_ID_Text, 227);
    lv_obj_set_y(ui_ID_Text, -185);
    lv_obj_set_align(ui_ID_Text, LV_ALIGN_CENTER);
    lv_label_set_text(ui_ID_Text, "ID:");
    lv_obj_set_style_text_color(ui_ID_Text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_ID_Text, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 9) ID dropdown
    ui_ID_Input = lv_dropdown_create(parent);
    lv_dropdown_set_options(ui_ID_Input, "Select ID"); // placeholder
    lv_dropdown_set_dir(ui_ID_Input, LV_DIR_LEFT);
    lv_obj_add_event_cb(ui_ID_Input, id_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_width(ui_ID_Input, 122);
    lv_obj_set_height(ui_ID_Input, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_ID_Input, 313);
    lv_obj_set_y(ui_ID_Input, -185);
    lv_obj_set_align(ui_ID_Input, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ID_Input, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_radius(ui_ID_Input, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_ID_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_ID_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_ID_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_ID_Input, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void destroy_preconfig_menu(void)
{
    // If the objects exist and are still valid, delete them
    // Otherwise just reset pointers since they may have been deleted by parent
    if (ui_Border_2 && lv_obj_is_valid(ui_Border_2)) { 
        lv_obj_del(ui_Border_2); 
    }
    ui_Border_2 = NULL;
    
    if (ui_Preconfig_Text && lv_obj_is_valid(ui_Preconfig_Text)) { 
        lv_obj_del(ui_Preconfig_Text); 
    }
    ui_Preconfig_Text = NULL;
    
    if (ui_ECU_Text && lv_obj_is_valid(ui_ECU_Text)) { 
        lv_obj_del(ui_ECU_Text); 
    }
    ui_ECU_Text = NULL;
    
    if (ui_ECU_Input && lv_obj_is_valid(ui_ECU_Input)) { 
        lv_obj_del(ui_ECU_Input); 
    }
    ui_ECU_Input = NULL;
    
    if (ui_Version_Text && lv_obj_is_valid(ui_Version_Text)) { 
        lv_obj_del(ui_Version_Text); 
    }
    ui_Version_Text = NULL;
    
    if (ui_Version_Input && lv_obj_is_valid(ui_Version_Input)) { 
        lv_obj_del(ui_Version_Input); 
    }
    ui_Version_Input = NULL;
    
    if (ui_ID_Text && lv_obj_is_valid(ui_ID_Text)) { 
        lv_obj_del(ui_ID_Text); 
    }
    ui_ID_Text = NULL;
    
    if (ui_ID_Input && lv_obj_is_valid(ui_ID_Input)) { 
        lv_obj_del(ui_ID_Input); 
    }
    ui_ID_Input = NULL;
}

