/*
 * canonical_channels.c — the firmware-baked canonical channel registry.
 *
 * Data source-of-truth: schema/canonical_channels.md
 *
 * Editing this file directly is OK during early v2 development. Once
 * the markdown-to-C codegen lands (similar to tools/codegen_widget_defs.py),
 * this file becomes generated and manual edits move to the markdown.
 *
 * 92 channels across 12 groups. See header for sentinel semantics.
 */

#include "canonical_channels.h"
#include <string.h>

/* Short macros to keep the table readable at 80 columns. */
#define UL CHANNEL_THRESHOLD_UNSET_LOW
#define UH CHANNEL_THRESHOLD_UNSET_HIGH

const canonical_channel_def_t CANONICAL_CHANNELS[] = {

/* ── Engine — Core ────────────────────────────────────────────────── */
{
	.id="rpm", .label="RPM",
	.group=CHGRP_ENGINE_CORE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="rpm", .units_display_def="rpm", .decimals=0,
	.min_default=0, .max_default=8000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF3030,
	.notes="OBD2 PID 0x0C. Most-used channel. Defaults generic; ECU preset overrides."
},
{
	.id="coolant_temp", .label="Coolant Temp",
	.group=CHGRP_ENGINE_CORE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=0, .max_default=120,
	.low_warn=80, .high_warn=110,
	.color_normal=0xFF8040,
	.notes="OBD2 PID 0x05. Cold-blue → normal → hot-orange → hot-red."
},
{
	.id="oil_temp", .label="Oil Temp",
	.group=CHGRP_ENGINE_CORE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=0, .max_default=130,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFA040, .notes=NULL
},
{
	.id="oil_pressure", .label="Oil Pressure",
	.group=CHGRP_ENGINE_CORE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=1,
	.min_default=0, .max_default=1000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFAA00,
	.notes="<0.5 bar = engine death. Low-only thresholds."
},
{
	.id="intake_air_temp", .label="Intake Air Temp",
	.group=CHGRP_ENGINE_CORE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=-20, .max_default=80,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFA040, .notes="OBD2 PID 0x0F. Heat-soak threshold."
},
{
	.id="throttle_position", .label="Throttle Position",
	.group=CHGRP_ENGINE_CORE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="OBD2 PID 0x11."
},
{
	.id="engine_load", .label="Engine Load",
	.group=CHGRP_ENGINE_CORE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="OBD2 PID 0x04."
},
{
	.id="ignition_timing", .label="Ignition Advance",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="°BTDC", .units_display_def="°BTDC", .decimals=1,
	.min_default=-10, .max_default=50,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="OBD2 PID 0x0E."
},
{
	.id="fuel_pressure", .label="Fuel Pressure",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=1,
	.min_default=0, .max_default=700,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFAA00,
	.notes="Low=pump failure, high=regulator stuck. Defaults ~3 bar EFI."
},
{
	.id="manifold_pressure", .label="MAP",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="kPa", .decimals=0,
	.min_default=0, .max_default=250,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Absolute. OBD2 PID 0x0B."
},
{
	.id="boost_pressure", .label="Boost",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=2,
	.min_default=-100, .max_default=300,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Relative to ambient. High-only — overboost."
},
{
	.id="boost_target", .label="Boost Target",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=2,
	.min_default=-100, .max_default=300,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFC080, .notes="What the ECU is aiming for."
},
{
	.id="mass_air_flow", .label="MAF",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="g/s", .units_display_def="g/s", .decimals=1,
	.min_default=0, .max_default=500,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="OBD2 PID 0x10."
},
{
	.id="afr_bank1", .label="AFR Bank 1",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="AFR", .units_display_def="AFR", .decimals=2,
	.min_default=9, .max_default=18,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80FF80, .notes="Stoich gasoline = 14.7."
},
{
	.id="afr_bank2", .label="AFR Bank 2",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="AFR", .units_display_def="AFR", .decimals=2,
	.min_default=9, .max_default=18,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80FF80, .notes=NULL
},
{
	.id="lambda_bank1", .label="Lambda Bank 1",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="λ", .units_display_def="λ", .decimals=3,
	.min_default=600, .max_default=1300,   /* λ × 1000 stored as int */
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80FF80,
	.notes="Lambda × 1000 internal. λ 1.0 = stoich."
},
{
	.id="lambda_bank2", .label="Lambda Bank 2",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="λ", .units_display_def="λ", .decimals=3,
	.min_default=600, .max_default=1300,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80FF80, .notes=NULL
},
/* Wideband O2 controllers — dedicated AFR channels, kept DISTINCT from
 * lambda_bank* (λ units) and afr_bank* (bank-indexed). Haltech (and
 * similar) broadcast numbered "Wideband 1..N" sensors AFR-scaled; the
 * auto-setup binds them here instead of forcing AFR values into the λ
 * Lambda-Bank channels (wrong units + range). Mirrors afr_bank1's shape. */
{
	.id="wideband_1", .label="Wideband 1",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="AFR", .units_display_def="AFR", .decimals=2,
	.min_default=9, .max_default=18,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80FF80,
	.notes="Wideband O2 sensor 1 (AFR). Stoich petrol = 14.7. Distinct from lambda_bank."
},
{
	.id="wideband_2", .label="Wideband 2",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="AFR", .units_display_def="AFR", .decimals=2,
	.min_default=9, .max_default=18,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80FF80,
	.notes="Wideband O2 sensor 2 (AFR)."
},
{
	.id="target_afr", .label="Target AFR",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="AFR", .units_display_def="AFR", .decimals=2,
	.min_default=9, .max_default=18,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xA0FFA0, .notes="What the ECU is aiming for."
},
{
	.id="target_lambda", .label="Target Lambda",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="λ", .units_display_def="λ", .decimals=3,
	.min_default=600, .max_default=1300,   /* λ × 1000 stored as int */
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xA0FFA0,
	.notes="Lambda × 1000 internal. Closed-loop target the ECU is aiming for."
},
{
	.id="lambda_correction", .label="Lambda Correction",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=1,
	.min_default=-25, .max_default=25,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="Closed-loop fuel correction from O2 feedback."
},
{
	.id="absolute_load", .label="Absolute Load",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=300,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="OBD2 PID 0x43. Normalised cylinder air mass."
},
{
	.id="fuel_rail_pressure", .label="Fuel Rail Pressure",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=1,
	.min_default=0, .max_default=20000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFAA00, .notes="OBD2 PID 0x23. Direct-injection rail."
},
{
	.id="fuel_pressure_diff", .label="Fuel Pressure Diff",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=2,
	.min_default=0, .max_default=700,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFAA00, .notes="Rail-to-manifold delta. Stuck regulator detection."
},
{
	.id="coolant_pressure", .label="Coolant Pressure",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=2,
	.min_default=0, .max_default=400,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF8040, .notes="Cooling system pressure. Head-gasket / cap fault."
},
{
	.id="knock_count", .label="Knock Events",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="count", .units_display_def="count", .decimals=0,
	.min_default=0, .max_default=99,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF0000, .notes="Any knock is bad — warn on first."
},
{
	.id="knock_retard", .label="Knock Retard",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="°", .units_display_def="°", .decimals=1,
	.min_default=0, .max_default=15,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF4040, .notes="Live retard amount being applied."
},
{
	.id="injector_duty", .label="Injector Duty",
	.group=CHGRP_ENGINE_CORE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=1,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Above 85% = approaching injector limit."
},
{
	.id="injector_pulse_width", .label="Inj Pulse Width",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="ms", .units_display_def="ms", .decimals=2,
	.min_default=0, .max_default=25,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFC080, .notes=NULL
},
{
	.id="volumetric_efficiency", .label="VE",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=150,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="Tuning aid."
},
{
	.id="short_term_fuel_trim", .label="STFT",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=1,
	.min_default=-25, .max_default=25,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="OBD2 PID 0x06/0x07."
},
{
	.id="long_term_fuel_trim", .label="LTFT",
	.group=CHGRP_ENGINE_CORE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=1,
	.min_default=-25, .max_default=25,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="OBD2 PID 0x08/0x09."
},

/* ── Engine — Exhaust ─────────────────────────────────────────────── */
{
	.id="exhaust_gas_temp_avg", .label="EGT Average",
	.group=CHGRP_ENGINE_EXHAUST, .tier=2, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=0, .max_default=1000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF6020, .notes="Single-probe or averaged."
},
#define EGT_ENTRY(N) \
{ \
	.id="egt_cyl_" #N, .label="EGT Cyl " #N, \
	.group=CHGRP_ENGINE_EXHAUST, .tier=3, .card=CHCARD_SCALAR, \
	.units_native="°C", .units_display_def="°C", .decimals=0, \
	.min_default=0, .max_default=1000, \
	.low_warn=UL, .high_warn=UH, \
	.color_normal=0xFF6020, .notes=NULL \
}
EGT_ENTRY(1), EGT_ENTRY(2), EGT_ENTRY(3), EGT_ENTRY(4),
EGT_ENTRY(5), EGT_ENTRY(6), EGT_ENTRY(7), EGT_ENTRY(8),
#undef EGT_ENTRY
{
	.id="exhaust_back_pressure", .label="Exhaust Back Pressure",
	.group=CHGRP_ENGINE_EXHAUST, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="kPa", .decimals=0,
	.min_default=0, .max_default=500,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF8040, .notes=NULL
},
{
	.id="wastegate_duty", .label="Wastegate Duty",
	.group=CHGRP_ENGINE_EXHAUST, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=1,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes=NULL
},
{
	.id="egr_command", .label="EGR Command",
	.group=CHGRP_ENGINE_EXHAUST, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="OBD2 PID 0x2C. Commanded EGR valve position."
},

/* ── Drivetrain ───────────────────────────────────────────────────── */
{
	.id="vehicle_speed", .label="Vehicle Speed",
	.group=CHGRP_DRIVETRAIN, .tier=1, .card=CHCARD_SCALAR,
	.units_native="km/h", .units_display_def="km/h", .decimals=0,
	.min_default=0, .max_default=300,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFFFFF, .notes="OBD2 PID 0x0D. Imperial → mph."
},
{
	.id="gear", .label="Gear",
	.group=CHGRP_DRIVETRAIN, .tier=1, .card=CHCARD_ENUM,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=-2, .max_default=8,   /* -2=P, -1=R, 0=N, 1..8=fwd */
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFFFFF,
	.notes="Enum. Calculated-gear path available via gear_config (RPM + speed + ratios)."
},
{
	.id="transmission_temp", .label="Trans Temp",
	.group=CHGRP_DRIVETRAIN, .tier=2, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=0, .max_default=150,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFA040, .notes="Auto/DCT/TC primarily."
},
{
	.id="transmission_pressure", .label="Trans Pressure",
	.group=CHGRP_DRIVETRAIN, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=1,
	.min_default=0, .max_default=2500,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFAA00, .notes="Auto/DCT."
},
{
	.id="clutch_switch", .label="Clutch",
	.group=CHGRP_DRIVETRAIN, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80C0FF, .notes="Clutch pedal engaged."
},
{
	.id="differential_temp", .label="Diff Temp",
	.group=CHGRP_DRIVETRAIN, .tier=3, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=0, .max_default=150,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFA040, .notes=NULL
},
{
	.id="launch_control_active", .label="Launch Control",
	.group=CHGRP_DRIVETRAIN, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes=NULL
},
{
	.id="traction_control_intervention", .label="TCS Intervention",
	.group=CHGRP_DRIVETRAIN, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Live intervention amount."
},
{
	.id="antilag_active", .label="Antilag",
	.group=CHGRP_DRIVETRAIN, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF8040, .notes=NULL
},

/* ── Electrical ───────────────────────────────────────────────────── */
{
	.id="battery_voltage", .label="Battery",
	.group=CHGRP_ELECTRICAL, .tier=1, .card=CHCARD_SCALAR,
	.units_native="V", .units_display_def="V", .decimals=1,
	.min_default=10, .max_default=16,
	.low_warn=UL, .high_warn=UH,
	/* voltages stored as ×10 (12.0V = 120); decimals=1 makes UI present
	 * correctly. Same convention applies to alternator_voltage below. */
	.color_normal=0x80A0FF, .notes="Low=discharge, high=regulator fault. V × 10 internal."
},
{
	.id="alternator_voltage", .label="Alternator",
	.group=CHGRP_ELECTRICAL, .tier=2, .card=CHCARD_SCALAR,
	.units_native="V", .units_display_def="V", .decimals=1,
	.min_default=10, .max_default=16,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80A0FF, .notes="V × 10 internal."
},
{
	.id="alternator_current", .label="Alt Current",
	.group=CHGRP_ELECTRICAL, .tier=3, .card=CHCARD_SCALAR,
	.units_native="A", .units_display_def="A", .decimals=0,
	.min_default=0, .max_default=150,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80A0FF, .notes=NULL
},
{
	.id="ecu_voltage", .label="ECU Voltage",
	.group=CHGRP_ELECTRICAL, .tier=3, .card=CHCARD_SCALAR,
	.units_native="V", .units_display_def="V", .decimals=2,
	.min_default=10, .max_default=16,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80A0FF, .notes="V × 10 internal. Critical-low flags brown-out."
},
{
	.id="system_current", .label="System Current",
	.group=CHGRP_ELECTRICAL, .tier=3, .card=CHCARD_SCALAR,
	.units_native="A", .units_display_def="A", .decimals=0,
	.min_default=0, .max_default=250,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80A0FF, .notes=NULL
},

/* ── Chassis — Dynamics ───────────────────────────────────────────── */
{
	.id="lateral_g", .label="Lateral G",
	.group=CHGRP_CHASSIS_DYNAMICS, .tier=2, .card=CHCARD_SCALAR,
	.units_native="g", .units_display_def="g", .decimals=2,
	.min_default=-250, .max_default=250,   /* g × 100 internal */
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="g × 100 internal. Positive = right."
},
{
	.id="longitudinal_g", .label="Longitudinal G",
	.group=CHGRP_CHASSIS_DYNAMICS, .tier=2, .card=CHCARD_SCALAR,
	.units_native="g", .units_display_def="g", .decimals=2,
	.min_default=-250, .max_default=250,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="g × 100 internal. Positive = accel."
},
{
	.id="vertical_g", .label="Vertical G",
	.group=CHGRP_CHASSIS_DYNAMICS, .tier=3, .card=CHCARD_SCALAR,
	.units_native="g", .units_display_def="g", .decimals=2,
	.min_default=-250, .max_default=250,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="g × 100 internal."
},
{
	.id="yaw_rate", .label="Yaw Rate",
	.group=CHGRP_CHASSIS_DYNAMICS, .tier=3, .card=CHCARD_SCALAR,
	.units_native="°/s", .units_display_def="°/s", .decimals=1,
	.min_default=-180, .max_default=180,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes=NULL
},
{
	.id="steering_angle", .label="Steering Angle",
	.group=CHGRP_CHASSIS_DYNAMICS, .tier=3, .card=CHCARD_SCALAR,
	.units_native="°", .units_display_def="°", .decimals=1,
	.min_default=-540, .max_default=540,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes=NULL
},

/* ── Chassis — Per-Corner (FL/FR/RL/RR) ───────────────────────────── */
#define WHEEL_SPEED_ENTRY(SUFFIX, LABEL) \
{ \
	.id="wheel_speed_" #SUFFIX, .label="Wheel Speed " LABEL, \
	.group=CHGRP_CHASSIS_PER_CORNER, .tier=3, .card=CHCARD_SCALAR, \
	.units_native="km/h", .units_display_def="km/h", .decimals=0, \
	.min_default=0, .max_default=300, \
	.low_warn=UL, .high_warn=UH, \
	.color_normal=0xFFFFFF, .notes=NULL \
}
WHEEL_SPEED_ENTRY(fl, "FL"), WHEEL_SPEED_ENTRY(fr, "FR"),
WHEEL_SPEED_ENTRY(rl, "RL"), WHEEL_SPEED_ENTRY(rr, "RR"),
#undef WHEEL_SPEED_ENTRY

#define TIRE_TEMP_ENTRY(SUFFIX, LABEL) \
{ \
	.id="tire_temp_" #SUFFIX, .label="Tire Temp " LABEL, \
	.group=CHGRP_CHASSIS_PER_CORNER, .tier=3, .card=CHCARD_SCALAR, \
	.units_native="°C", .units_display_def="°C", .decimals=0, \
	.min_default=0, .max_default=150, \
	.low_warn=UL, .high_warn=UH, \
	.color_normal=0xFFA040, .notes="Cold tires / overheated tires." \
}
TIRE_TEMP_ENTRY(fl, "FL"), TIRE_TEMP_ENTRY(fr, "FR"),
TIRE_TEMP_ENTRY(rl, "RL"), TIRE_TEMP_ENTRY(rr, "RR"),
#undef TIRE_TEMP_ENTRY

#define TIRE_PRESS_ENTRY(SUFFIX, LABEL) \
{ \
	.id="tire_pressure_" #SUFFIX, .label="Tire Pressure " LABEL, \
	.group=CHGRP_CHASSIS_PER_CORNER, .tier=3, .card=CHCARD_SCALAR, \
	.units_native="kPa", .units_display_def="psi", .decimals=1, \
	.min_default=100, .max_default=350, \
	.low_warn=UL, .high_warn=UH, \
	.color_normal=0xC0C0C0, .notes="TPMS-style." \
}
TIRE_PRESS_ENTRY(fl, "FL"), TIRE_PRESS_ENTRY(fr, "FR"),
TIRE_PRESS_ENTRY(rl, "RL"), TIRE_PRESS_ENTRY(rr, "RR"),
#undef TIRE_PRESS_ENTRY

#define SUSP_TRAVEL_ENTRY(SUFFIX, LABEL) \
{ \
	.id="suspension_travel_" #SUFFIX, .label="Susp Travel " LABEL, \
	.group=CHGRP_CHASSIS_PER_CORNER, .tier=3, .card=CHCARD_SCALAR, \
	.units_native="mm", .units_display_def="mm", .decimals=0, \
	.min_default=0, .max_default=200, \
	.low_warn=UL, .high_warn=UH, \
	.color_normal=0xC0C0C0, .notes=NULL \
}
SUSP_TRAVEL_ENTRY(fl, "FL"), SUSP_TRAVEL_ENTRY(fr, "FR"),
SUSP_TRAVEL_ENTRY(rl, "RL"), SUSP_TRAVEL_ENTRY(rr, "RR"),
#undef SUSP_TRAVEL_ENTRY

#define RIDE_HEIGHT_ENTRY(SUFFIX, LABEL) \
{ \
	.id="ride_height_" #SUFFIX, .label="Ride Height " LABEL, \
	.group=CHGRP_CHASSIS_PER_CORNER, .tier=3, .card=CHCARD_SCALAR, \
	.units_native="mm", .units_display_def="mm", .decimals=0, \
	.min_default=0, .max_default=300, \
	.low_warn=UL, .high_warn=UH, \
	.color_normal=0xC0C0C0, .notes=NULL \
}
RIDE_HEIGHT_ENTRY(fl, "FL"), RIDE_HEIGHT_ENTRY(fr, "FR"),
RIDE_HEIGHT_ENTRY(rl, "RL"), RIDE_HEIGHT_ENTRY(rr, "RR"),
#undef RIDE_HEIGHT_ENTRY

/* ── Brakes & Driver Inputs ───────────────────────────────────────── */
{
	.id="brake_pressure_front", .label="Brake Pressure Front",
	.group=CHGRP_BRAKES_INPUTS, .tier=2, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=0,
	.min_default=0, .max_default=20000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF4040, .notes=NULL
},
{
	.id="brake_pressure_rear", .label="Brake Pressure Rear",
	.group=CHGRP_BRAKES_INPUTS, .tier=2, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="bar", .decimals=0,
	.min_default=0, .max_default=20000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF4040, .notes=NULL
},
{
	.id="brake_pedal_position", .label="Brake Pedal",
	.group=CHGRP_BRAKES_INPUTS, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF4040, .notes=NULL
},
{
	.id="accel_pedal_position", .label="Accel Pedal",
	.group=CHGRP_BRAKES_INPUTS, .tier=2, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Drive-by-wire. OBD2 PID 0x49."
},
{
	.id="handbrake", .label="Handbrake",
	.group=CHGRP_BRAKES_INPUTS, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF4040, .notes=NULL
},

/* ── Fuel & Distance ──────────────────────────────────────────────── */
{
	.id="fuel_level", .label="Fuel Level",
	.group=CHGRP_FUEL_DISTANCE, .tier=1, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x40C040, .notes="OBD2 PID 0x2F. Low warn at 15%."
},
{
	.id="fuel_remaining_distance", .label="Fuel Range",
	.group=CHGRP_FUEL_DISTANCE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="km", .units_display_def="km", .decimals=0,
	.min_default=0, .max_default=1000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x40C040, .notes="Calculated. Imperial → mi."
},
{
	.id="fuel_consumption_instant", .label="Inst Fuel Use",
	.group=CHGRP_FUEL_DISTANCE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="L/h", .units_display_def="L/h", .decimals=1,
	.min_default=0, .max_default=60,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes=NULL
},
{
	.id="fuel_consumption_avg", .label="Avg Fuel Use",
	.group=CHGRP_FUEL_DISTANCE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="L/100km", .units_display_def="L/100km", .decimals=1,
	.min_default=0, .max_default=40,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes=NULL
},
{
	.id="odometer", .label="Odometer",
	.group=CHGRP_FUEL_DISTANCE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="km", .units_display_def="km", .decimals=0,
	.min_default=0, .max_default=9999999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFFFFF, .notes="Persistent."
},
{
	.id="trip_distance", .label="Trip Distance",
	.group=CHGRP_FUEL_DISTANCE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="km", .units_display_def="km", .decimals=1,
	.min_default=0, .max_default=9999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFFFFF, .notes=NULL
},
{
	.id="ethanol_pct", .label="Ethanol %",
	.group=CHGRP_FUEL_DISTANCE, .tier=2, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x40C040, .notes="OBD2 PID 0x52. Flex-fuel blend (E85 etc.)."
},
{
	.id="fuel_rate", .label="Fuel Rate",
	.group=CHGRP_FUEL_DISTANCE, .tier=3, .card=CHCARD_SCALAR,
	.units_native="L/h", .units_display_def="L/h", .decimals=1,
	.min_default=0, .max_default=60,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="OBD2 PID 0x5E. Engine fuel flow rate."
},

/* ── Environment ──────────────────────────────────────────────────── */
{
	.id="ambient_temp", .label="Ambient Temp",
	.group=CHGRP_ENVIRONMENT, .tier=2, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=-30, .max_default=55,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="OBD2 PID 0x46."
},
{
	.id="cabin_temp", .label="Cabin Temp",
	.group=CHGRP_ENVIRONMENT, .tier=3, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=-10, .max_default=50,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes=NULL
},
{
	.id="barometric_pressure", .label="Barometric Pressure",
	.group=CHGRP_ENVIRONMENT, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kPa", .units_display_def="kPa", .decimals=0,
	.min_default=60, .max_default=110,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="OBD2 PID 0x33."
},

/* ── Lap Timing & Race ────────────────────────────────────────────── */
{
	.id="lap_time_current", .label="Lap Time (Current)",
	.group=CHGRP_LAP_TIMING, .tier=3, .card=CHCARD_SCALAR,
	.units_native="s", .units_display_def="s", .decimals=3,
	.min_default=0, .max_default=9999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Formatted M:SS.sss in UI."
},
{
	.id="lap_time_last", .label="Lap Time (Last)",
	.group=CHGRP_LAP_TIMING, .tier=3, .card=CHCARD_SCALAR,
	.units_native="s", .units_display_def="s", .decimals=3,
	.min_default=0, .max_default=9999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFFFFF, .notes=NULL
},
{
	.id="lap_time_best", .label="Lap Time (Best)",
	.group=CHGRP_LAP_TIMING, .tier=3, .card=CHCARD_SCALAR,
	.units_native="s", .units_display_def="s", .decimals=3,
	.min_default=0, .max_default=9999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x40C040, .notes=NULL
},
{
	.id="lap_number", .label="Lap Number",
	.group=CHGRP_LAP_TIMING, .tier=3, .card=CHCARD_SCALAR,
	.units_native="count", .units_display_def="count", .decimals=0,
	.min_default=0, .max_default=999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFFFFF, .notes=NULL
},
{
	.id="lap_delta_best", .label="Lap Delta vs Best",
	.group=CHGRP_LAP_TIMING, .tier=3, .card=CHCARD_SCALAR,
	.units_native="s", .units_display_def="s", .decimals=2,
	.min_default=-60, .max_default=60,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Negative = ahead of best."
},
{
	.id="sector_time_current", .label="Sector Time",
	.group=CHGRP_LAP_TIMING, .tier=3, .card=CHCARD_SCALAR,
	.units_native="s", .units_display_def="s", .decimals=3,
	.min_default=0, .max_default=999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes=NULL
},

/* ── Body & Lights ────────────────────────────────────────────────── */
{
	.id="headlights", .label="Headlights",
	.group=CHGRP_BODY_LIGHTS, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFFFFF, .notes=NULL
},
{
	.id="high_beam", .label="High Beam",
	.group=CHGRP_BODY_LIGHTS, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x80A0FF, .notes=NULL
},
{
	.id="turn_signal_left", .label="Left Turn",
	.group=CHGRP_BODY_LIGHTS, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x40C040, .notes=NULL
},
{
	.id="turn_signal_right", .label="Right Turn",
	.group=CHGRP_BODY_LIGHTS, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x40C040, .notes=NULL
},
{
	.id="hazards", .label="Hazards",
	.group=CHGRP_BODY_LIGHTS, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes=NULL
},
{
	.id="door_open", .label="Door Open",
	.group=CHGRP_BODY_LIGHTS, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFD000, .notes="Any door."
},

/* ── Diagnostic / OBD2 ────────────────────────────────────────────── */
{
	.id="check_engine", .label="Check Engine",
	.group=CHGRP_DIAGNOSTIC, .tier=2, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFA000, .notes="MIL light. high_warn=1 fires on true."
},
{
	.id="dtc_count", .label="DTC Count",
	.group=CHGRP_DIAGNOSTIC, .tier=3, .card=CHCARD_SCALAR,
	.units_native="count", .units_display_def="count", .decimals=0,
	.min_default=0, .max_default=99,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFA000, .notes=NULL
},
{
	.id="coolant_level_low", .label="Coolant Level Low",
	.group=CHGRP_DIAGNOSTIC, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFF8040, .notes=NULL
},
{
	.id="oil_level_low", .label="Oil Level Low",
	.group=CHGRP_DIAGNOSTIC, .tier=3, .card=CHCARD_BOOLEAN,
	.units_native="", .units_display_def="", .decimals=0,
	.min_default=0, .max_default=1,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xFFAA00, .notes=NULL
},
{
	.id="distance_with_mil_on", .label="Distance w/ MIL",
	.group=CHGRP_DIAGNOSTIC, .tier=3, .card=CHCARD_SCALAR,
	.units_native="km", .units_display_def="km", .decimals=0,
	.min_default=0, .max_default=99999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="OBD2 PID 0x21."
},
{
	.id="runtime", .label="Engine Run Time",
	.group=CHGRP_DIAGNOSTIC, .tier=3, .card=CHCARD_SCALAR,
	.units_native="s", .units_display_def="s", .decimals=0,
	.min_default=0, .max_default=99999,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0xC0C0C0, .notes="OBD2 PID 0x1F. Seconds since engine start."
},

/* ── Dash internals (CHGRP_DASH_SYSTEM) ────────────────────────────
 * These are RDM-7's own runtime stats, fed by signal_internal.c.
 * Auto-activated + pre-bound in channel_manager_init() so users can
 * drop them on a widget without going through the signal picker. */
{
	.id="dash_fps", .label="Dash FPS",
	.group=CHGRP_DASH_SYSTEM, .tier=3, .card=CHCARD_SCALAR,
	.units_native="fps", .units_display_def="fps", .decimals=0,
	.min_default=0, .max_default=60,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x4CC9F0, .notes="LVGL frame rate (signal FPS)."
},
{
	.id="dash_cpu", .label="Dash CPU",
	.group=CHGRP_DASH_SYSTEM, .tier=3, .card=CHCARD_SCALAR,
	.units_native="%", .units_display_def="%", .decimals=0,
	.min_default=0, .max_default=100,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x4CC9F0, .notes="LVGL task CPU percent (signal CPU_PERCENT)."
},
{
	.id="dash_free_heap", .label="Free Heap",
	.group=CHGRP_DASH_SYSTEM, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kB", .units_display_def="kB", .decimals=0,
	.min_default=0, .max_default=400,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x4CC9F0, .notes="Internal SRAM free (signal FREE_HEAP_KB)."
},
{
	.id="dash_free_psram", .label="Free PSRAM",
	.group=CHGRP_DASH_SYSTEM, .tier=3, .card=CHCARD_SCALAR,
	.units_native="kB", .units_display_def="kB", .decimals=0,
	.min_default=0, .max_default=8000,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x4CC9F0, .notes="PSRAM free (signal FREE_PSRAM_KB)."
},
{
	.id="dash_uptime", .label="Uptime",
	.group=CHGRP_DASH_SYSTEM, .tier=3, .card=CHCARD_SCALAR,
	.units_native="s", .units_display_def="s", .decimals=0,
	.min_default=0, .max_default=86400,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x4CC9F0, .notes="Seconds since boot (signal UPTIME_S)."
},
{
	.id="dash_chip_temp", .label="Chip Temp",
	.group=CHGRP_DASH_SYSTEM, .tier=3, .card=CHCARD_SCALAR,
	.units_native="°C", .units_display_def="°C", .decimals=0,
	.min_default=0, .max_default=120,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x4CC9F0, .notes="ESP32-S3 internal temp (signal CHIP_TEMP)."
},
{
	.id="dash_wifi_rssi", .label="WiFi Signal",
	.group=CHGRP_DASH_SYSTEM, .tier=3, .card=CHCARD_SCALAR,
	.units_native="dBm", .units_display_def="dBm", .decimals=0,
	.min_default=-90, .max_default=-30,
	.low_warn=UL, .high_warn=UH,
	.color_normal=0x4CC9F0, .notes="STA RSSI in dBm (signal WIFI_RSSI). More negative = weaker."
},

}; /* end CANONICAL_CHANNELS */

const size_t CANONICAL_CHANNEL_COUNT =
	sizeof(CANONICAL_CHANNELS) / sizeof(CANONICAL_CHANNELS[0]);

#undef UL
#undef UH

/* ── Group metadata table ────────────────────────────────────────── */
static const struct {
	channel_group_t g;
	const char *name;
	const char *short_name;
} GROUP_META[] = {
	/* NOTE: keep group names ASCII-only — the on-device Montserrat font
	 * is built with `-r 0x20-0x7F,0xB0,0x2022` and any other glyph (em
	 * dash, en dash, etc.) renders as a blank box. The web UI uses its
	 * own font so it can show whatever it likes. */
	{CHGRP_ENGINE_CORE,        "Engine - Core",       "engine"},
	{CHGRP_ENGINE_EXHAUST,     "Engine - Exhaust",    "exhaust"},
	{CHGRP_DRIVETRAIN,         "Drivetrain",          "drivetrain"},
	{CHGRP_ELECTRICAL,         "Electrical",          "electrical"},
	{CHGRP_CHASSIS_DYNAMICS,   "Chassis - Dynamics",  "chassis"},
	{CHGRP_CHASSIS_PER_CORNER, "Chassis - Per-Corner","corners"},
	{CHGRP_BRAKES_INPUTS,      "Brakes & Inputs",     "brakes"},
	{CHGRP_FUEL_DISTANCE,      "Fuel & Distance",     "fuel"},
	{CHGRP_ENVIRONMENT,        "Environment",         "environment"},
	{CHGRP_LAP_TIMING,         "Lap Timing & Race",   "race"},
	{CHGRP_BODY_LIGHTS,        "Body & Lights",       "body"},
	{CHGRP_DIAGNOSTIC,         "Diagnostics",         "diag"},
	{CHGRP_DASH_SYSTEM,        "Dash Internals",      "dash"},
};

const char *channel_group_name(channel_group_t g) {
	for (size_t i = 0; i < sizeof(GROUP_META)/sizeof(GROUP_META[0]); ++i)
		if (GROUP_META[i].g == g) return GROUP_META[i].name;
	return "Unknown";
}

const char *channel_group_short_name(channel_group_t g) {
	for (size_t i = 0; i < sizeof(GROUP_META)/sizeof(GROUP_META[0]); ++i)
		if (GROUP_META[i].g == g) return GROUP_META[i].short_name;
	return "unknown";
}

const char *channel_cardinality_name(channel_cardinality_t c) {
	switch (c) {
	case CHCARD_SCALAR:  return "scalar";
	case CHCARD_ENUM:    return "enum";
	case CHCARD_BOOLEAN: return "boolean";
	default:             return "unknown";
	}
}

/* ── Lookup helpers ──────────────────────────────────────────────── */

const canonical_channel_def_t *canonical_channel_find(const char *id) {
	if (!id || id[0] == '\0') return NULL;
	for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; ++i) {
		if (strcmp(CANONICAL_CHANNELS[i].id, id) == 0)
			return &CANONICAL_CHANNELS[i];
	}
	return NULL;
}

bool canonical_channel_exists(const char *id) {
	return canonical_channel_find(id) != NULL;
}

/* tolower without locale dependence — ASCII only, which is what
 * canonical IDs use. */
static inline char _tolower_ascii(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

const canonical_channel_def_t *canonical_channel_find_ci(const char *name) {
	if (!name || name[0] == '\0') return NULL;
	for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; ++i) {
		const char *id = CANONICAL_CHANNELS[i].id;
		size_t j = 0;
		while (id[j] != '\0' && name[j] != '\0' &&
		       _tolower_ascii(id[j]) == _tolower_ascii(name[j])) {
			j++;
		}
		if (id[j] == '\0' && name[j] == '\0')
			return &CANONICAL_CHANNELS[i];
	}
	return NULL;
}

bool channel_id_is_custom(const char *id) {
	if (!id) return false;
	return strncmp(id, "custom_", 7) == 0;
}

size_t canonical_channel_count_in_group(channel_group_t g) {
	size_t n = 0;
	for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; ++i)
		if (CANONICAL_CHANNELS[i].group == g) n++;
	return n;
}

/* ── Default zone color convention ───────────────────────────────── */

uint32_t channel_default_zone_color(channel_zone_t zone) {
	switch (zone) {
	case CHZONE_LOW_WARN:      return 0x4080FF;  /* blue / cyan */
	case CHZONE_NORMAL:        return 0xC0C0C0;  /* fallback if channel didn't set */
	case CHZONE_HIGH_WARN:     return 0xFFA000;  /* amber / orange */
	default:                   return 0xFFFFFF;
	}
}

const char *channel_zone_name(channel_zone_t zone) {
	switch (zone) {
	case CHZONE_LOW_WARN:  return "low_warn";
	case CHZONE_NORMAL:    return "normal";
	case CHZONE_HIGH_WARN: return "high_warn";
	default:               return "unknown";
	}
}

channel_zone_t canonical_channel_value_zone(
	const canonical_channel_def_t *def, float value)
{
	if (!def) return CHZONE_NORMAL;
	if (def->low_warn != CHANNEL_THRESHOLD_UNSET_LOW &&
	    value <= def->low_warn) return CHZONE_LOW_WARN;
	if (def->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH &&
	    value >= def->high_warn) return CHZONE_HIGH_WARN;
	return CHZONE_NORMAL;
}

/* ── Canonical channel → OBD2 PID map ────────────────────────────────
 *
 * Channels not listed here have no standard J1979 PID equivalent — the
 * source picker silently omits the OBD2 group for those. */
const canonical_obd2_map_t CANONICAL_OBD2_MAP[] = {
	{ "rpm",                  "RPM",                  0x01, 0x0C },
	{ "coolant_temp",         "COOLANT_TEMP",         0x01, 0x05 },
	{ "vehicle_speed",        "VEHICLE_SPEED",        0x01, 0x0D },
	{ "throttle_position",    "THROTTLE",             0x01, 0x45 },
	{ "intake_air_temp",      "INTAKE_AIR_TEMP",      0x01, 0x0F },
	{ "engine_load",          "ENGINE_LOAD",          0x01, 0x04 },
	{ "battery_voltage",      "BATTERY_VOLTAGE",      0x01, 0x42 },
	{ "fuel_level",           "FUEL_LEVEL",           0x01, 0x2F },
	{ "ignition_timing",      "TIMING_ADVANCE",       0x01, 0x0E },
	{ "manifold_pressure",    "MAP",                  0x01, 0x0B },
	{ "mass_air_flow",        "MAF",                  0x01, 0x10 },
	{ "fuel_pressure",        "FUEL_PRESSURE",        0x01, 0x0A },
	{ "oil_temp",             "OIL_TEMP",             0x01, 0x5C },
	{ "short_term_fuel_trim", "SHORT_FUEL_TRIM_1",    0x01, 0x06 },
	{ "long_term_fuel_trim",  "LONG_FUEL_TRIM_1",     0x01, 0x07 },
	{ "ambient_temp",         "AMBIENT_TEMP",         0x01, 0x46 },
	{ "lambda_bank1",         "LAMBDA",               0x01, 0x44 },
	{ "barometric_pressure",  "BAROMETRIC_PRESSURE",  0x01, 0x33 },
	{ "runtime",              "RUN_TIME",             0x01, 0x1F },
	{ "accel_pedal_position", "PEDAL_POSITION",       0x01, 0x5A },
	{ "ethanol_pct",          "ETHANOL_PCT",          0x01, 0x52 },
	{ "egr_command",          "EGR_CMD",              0x01, 0x2C },
	{ "absolute_load",        "ABSOLUTE_LOAD",        0x01, 0x43 },
	{ "fuel_rate",            "FUEL_RATE",            0x01, 0x5E },
	{ "fuel_rail_pressure",   "FUEL_RAIL_PRESSURE",   0x01, 0x23 },
	{ "distance_with_mil_on", "DTC_DISTANCE",         0x01, 0x21 },
};
const size_t CANONICAL_OBD2_MAP_COUNT =
	sizeof(CANONICAL_OBD2_MAP) / sizeof(CANONICAL_OBD2_MAP[0]);

const canonical_obd2_map_t *canonical_channel_obd2_for(const char *channel_id) {
	if (!channel_id || !channel_id[0]) return NULL;
	for (size_t i = 0; i < CANONICAL_OBD2_MAP_COUNT; ++i) {
		if (strcmp(CANONICAL_OBD2_MAP[i].channel_id, channel_id) == 0)
			return &CANONICAL_OBD2_MAP[i];
	}
	return NULL;
}
