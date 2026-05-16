/**
 * obd2_dtc_db.c — Generic SAE J2012 DTC descriptions.
 *
 * Curated set of the most-encountered generic Powertrain DTCs. Manufacturer-
 * specific ranges (P1xxx, P2xxx in OEM ranges, P3xxx, C/B/Uxxxx) intentionally
 * excluded — they vary by vehicle and would balloon flash for marginal value.
 *
 * Source: SAE J2012-DA (latest revisions). Descriptions paraphrased for
 * brevity (max ~50 chars) so they fit one row in the Code Reader modal
 * without truncation on a 800px display.
 *
 * Roughly grouped:
 *   P00xx — Fuel & air metering, auxiliary emission controls
 *   P01xx — Fuel & air metering (sensors)
 *   P02xx — Fuel & air metering (injectors)
 *   P03xx — Ignition / misfire
 *   P04xx — Auxiliary emission controls (EGR, EVAP, secondary air)
 *   P05xx — Vehicle speed, idle control, ancillary inputs
 *   P06xx — Computer & auxiliary outputs
 *   P07xx — Transmission (generic)
 */
#include "obd2_dtc_db.h"
#include <string.h>

typedef struct {
    const char *code;     /* "P0420" */
    const char *desc;     /* short paraphrased description */
} dtc_entry_t;

static const dtc_entry_t DTC_DB[] = {
    /* ── P00xx — System tests / fuel & air metering ── */
    { "P0010", "Camshaft Position Actuator Circuit (Bank 1)" },
    { "P0011", "Cam Position Timing Over-Advanced (Bank 1)" },
    { "P0014", "Exhaust Cam Position Over-Advanced (Bank 1)" },
    { "P0016", "Crankshaft/Camshaft Position Correlation B1" },
    { "P0017", "Crank/Cam Position Correlation B1 Sensor B" },
    { "P0021", "Cam Position Timing Over-Advanced (Bank 2)" },
    { "P0030", "HO2S Heater Control Circuit B1 S1" },
    { "P0031", "HO2S Heater Low Voltage B1 S1" },
    { "P0036", "HO2S Heater Control Circuit B1 S2" },
    { "P0053", "HO2S Heater Resistance B1 S1" },
    { "P0068", "MAP/MAF - Throttle Position Correlation" },

    /* ── P01xx — Fuel & air metering sensors ── */
    { "P0100", "Mass Air Flow Sensor Circuit" },
    { "P0101", "MAF Sensor Range/Performance" },
    { "P0102", "MAF Sensor Low Input" },
    { "P0103", "MAF Sensor High Input" },
    { "P0106", "MAP Sensor Range/Performance" },
    { "P0107", "MAP Sensor Low Input" },
    { "P0108", "MAP Sensor High Input" },
    { "P0111", "Intake Air Temp Sensor Range/Performance" },
    { "P0112", "Intake Air Temp Sensor Low" },
    { "P0113", "Intake Air Temp Sensor High" },
    { "P0116", "Coolant Temp Sensor Range/Performance" },
    { "P0117", "Coolant Temp Sensor Low Input" },
    { "P0118", "Coolant Temp Sensor High Input" },
    { "P0121", "Throttle Position Sensor Range/Performance" },
    { "P0122", "Throttle Position Sensor Low Input" },
    { "P0123", "Throttle Position Sensor High Input" },
    { "P0125", "Insufficient Coolant Temp for Closed Loop" },
    { "P0128", "Coolant Below Thermostat Regulating Temp" },
    { "P0130", "O2 Sensor Circuit Malfunction B1 S1" },
    { "P0131", "O2 Sensor Low Voltage B1 S1" },
    { "P0132", "O2 Sensor High Voltage B1 S1" },
    { "P0133", "O2 Sensor Slow Response B1 S1" },
    { "P0134", "O2 Sensor No Activity B1 S1" },
    { "P0135", "O2 Sensor Heater Circuit B1 S1" },
    { "P0137", "O2 Sensor Low Voltage B1 S2" },
    { "P0138", "O2 Sensor High Voltage B1 S2" },
    { "P0141", "O2 Sensor Heater Circuit B1 S2" },
    { "P0150", "O2 Sensor Circuit Malfunction B2 S1" },
    { "P0151", "O2 Sensor Low Voltage B2 S1" },
    { "P0152", "O2 Sensor High Voltage B2 S1" },
    { "P0155", "O2 Sensor Heater Circuit B2 S1" },
    { "P0161", "O2 Sensor Heater Circuit B2 S2" },
    { "P0171", "System Too Lean (Bank 1)" },
    { "P0172", "System Too Rich (Bank 1)" },
    { "P0174", "System Too Lean (Bank 2)" },
    { "P0175", "System Too Rich (Bank 2)" },
    { "P0181", "Fuel Temp Sensor A Range/Performance" },
    { "P0182", "Fuel Temp Sensor A Low Input" },
    { "P0183", "Fuel Temp Sensor A High Input" },
    { "P0190", "Fuel Rail Pressure Sensor Circuit" },
    { "P0191", "Fuel Rail Pressure Sensor Range/Performance" },
    { "P0192", "Fuel Rail Pressure Sensor Low Input" },
    { "P0193", "Fuel Rail Pressure Sensor High Input" },

    /* ── P02xx — Injectors, EGR, EVAP ── */
    { "P0200", "Injector Circuit Malfunction" },
    { "P0201", "Injector Circuit - Cylinder 1" },
    { "P0202", "Injector Circuit - Cylinder 2" },
    { "P0203", "Injector Circuit - Cylinder 3" },
    { "P0204", "Injector Circuit - Cylinder 4" },
    { "P0205", "Injector Circuit - Cylinder 5" },
    { "P0206", "Injector Circuit - Cylinder 6" },
    { "P0217", "Engine Over Temperature" },
    { "P0218", "Transmission Over Temperature" },
    { "P0219", "Engine Overspeed" },
    { "P0234", "Turbo/Supercharger Overboost" },
    { "P0235", "Turbo/Supercharger Boost Sensor A" },
    { "P0236", "Turbo Boost Sensor A Range/Performance" },
    { "P0237", "Turbo Boost Sensor A Low" },
    { "P0238", "Turbo Boost Sensor A High" },
    { "P0299", "Turbo/Supercharger Underboost" },

    /* ── P03xx — Misfire ── */
    { "P0300", "Random/Multiple Cylinder Misfire" },
    { "P0301", "Cylinder 1 Misfire Detected" },
    { "P0302", "Cylinder 2 Misfire Detected" },
    { "P0303", "Cylinder 3 Misfire Detected" },
    { "P0304", "Cylinder 4 Misfire Detected" },
    { "P0305", "Cylinder 5 Misfire Detected" },
    { "P0306", "Cylinder 6 Misfire Detected" },
    { "P0325", "Knock Sensor 1 Circuit (Bank 1)" },
    { "P0335", "Crankshaft Position Sensor A Circuit" },
    { "P0336", "CKP Sensor A Range/Performance" },
    { "P0340", "Camshaft Position Sensor A Circuit B1" },
    { "P0341", "Cam Position Sensor A Range/Perf B1" },
    { "P0351", "Ignition Coil A Primary/Secondary Circuit" },
    { "P0352", "Ignition Coil B Primary/Secondary Circuit" },

    /* ── P04xx — Aux emission controls (EGR / EVAP / DPF / Cat) ── */
    { "P0400", "EGR Flow Malfunction" },
    { "P0401", "EGR Insufficient Flow Detected" },
    { "P0402", "EGR Excessive Flow Detected" },
    { "P0403", "EGR Control Circuit" },
    { "P0420", "Catalyst Efficiency Below Threshold (B1)" },
    { "P0421", "Warm-Up Catalyst Efficiency Below Thresh B1" },
    { "P0430", "Catalyst Efficiency Below Threshold (B2)" },
    { "P0440", "EVAP System Malfunction" },
    { "P0441", "EVAP Incorrect Purge Flow" },
    { "P0442", "EVAP System Small Leak Detected" },
    { "P0446", "EVAP Vent Control Circuit" },
    { "P0455", "EVAP System Large Leak Detected" },
    { "P0456", "EVAP System Very Small Leak Detected" },
    { "P0480", "Fan 1 Control Circuit" },
    { "P0491", "Secondary Air Insufficient Flow B1" },

    /* ── P05xx — VSS, idle, ancillary ── */
    { "P0500", "Vehicle Speed Sensor Malfunction" },
    { "P0501", "Vehicle Speed Sensor Range/Performance" },
    { "P0505", "Idle Air Control System Malfunction" },
    { "P0506", "Idle Speed Low" },
    { "P0507", "Idle Speed High" },
    { "P0521", "Engine Oil Pressure Sensor Range/Perf" },
    { "P0524", "Engine Oil Pressure Too Low" },
    { "P0562", "System Voltage Low" },
    { "P0563", "System Voltage High" },
    { "P0571", "Brake Switch A Circuit" },

    /* ── P06xx — Internal / outputs ── */
    { "P0601", "Internal Control Module Memory Checksum" },
    { "P0606", "ECM/PCM Processor Fault" },
    { "P0610", "Vehicle Options Error" },
    { "P0628", "Fuel Pump A Control Circuit Low" },
    { "P0629", "Fuel Pump A Control Circuit High" },

    /* ── P07xx — Transmission generic ── */
    { "P0700", "Transmission Control System Malfunction" },
    { "P0701", "Trans Control System Range/Performance" },
    { "P0705", "Trans Range Sensor Circuit" },
    { "P0715", "Input/Turbine Speed Sensor Circuit" },
    { "P0720", "Output Speed Sensor Circuit" },
    { "P0730", "Incorrect Gear Ratio" },
    { "P0740", "Torque Converter Clutch Circuit" },
    { "P0741", "TCC Performance / Stuck Off" },
    { "P0750", "Shift Solenoid A" },
    { "P0755", "Shift Solenoid B" },

    /* ── P20xx — Diesel-specific (DEF/SCR/DPF/NOx) ── */
    { "P2002", "DPF Efficiency Below Threshold (B1)" },
    { "P2032", "EGT Sensor Low Bank 1 Sensor 2" },
    { "P2033", "EGT Sensor High Bank 1 Sensor 2" },
    { "P2080", "EGT Sensor Range/Performance B1 S1" },
    { "P2196", "O2 Sensor Stuck Rich B1 S1" },
    { "P2197", "O2 Sensor Stuck Lean B1 S1" },
    { "P2226", "Barometric Pressure Circuit" },
    { "P2228", "Barometric Pressure Low" },
    { "P2229", "Barometric Pressure High" },
    { "P242F", "DPF Restriction - Ash Accumulation" },
    { "P2453", "DPF Diff Pressure Sensor Range/Performance" },
    { "P2454", "DPF Diff Pressure Sensor Low" },
    { "P2455", "DPF Diff Pressure Sensor High" },

    /* Sentinel — keep last */
    { NULL, NULL }
};

const char *obd2_dtc_lookup(const char *code) {
    if (!code || !code[0]) return NULL;
    for (const dtc_entry_t *e = DTC_DB; e->code != NULL; e++) {
        if (strcmp(e->code, code) == 0) return e->desc;
    }
    return NULL;
}
