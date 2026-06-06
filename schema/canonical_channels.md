# Canonical Channels — RDM-7 Dash v14

**Status**: draft v1.1 for review. Changes from v1.0:
- `gear` promoted to Tier 1; calculated-gear path called out as universally available
- Threshold model upgraded to bi-directional: each channel can warn on the
  LOW side, the HIGH side, or both, with critical tiers above warn on each.
  Coolant temp now shows "cold-blue / normal / hot-warn / hot-critical".

This file is the source-of-truth for the canonical channel registry. The
firmware bakes this list in at compile time. Layouts on the marketplace can
reference any of these by `id` and they will auto-bind to the matching
channel on any RDM-7 dash that has it configured.

## Conventions

### Naming

- **IDs** are lowercase `snake_case`. Reserved namespace — never collides
  with user-defined channels (those are prefixed `custom_`).
- **Native units** are what the channel stores internally and what signals
  feed into it. Always SI / SAE / OBD2-aligned (°C, kPa, km/h, V, A, g).
  The unit conversion to display happens *out* of the channel.
- **Display defaults** are what the channel shows to the user out of the
  box. User can override per-channel; global toggle flips all between
  metric and imperial.

### Threshold model — bi-directional zones

Each channel can have up to **four optional thresholds** that define five
zones from low to high:

```
   LOW_CRITICAL                NORMAL                HIGH_CRITICAL
       ↓                          ↓                        ↓
─────●──────●──────────────────────●──────●─────────────●──────●─────
   v ≤ LC  LC < v ≤ LW         LW < v < HW          HW ≤ v < HC   v ≥ HC
   "alarm" "warn"             "ok"               "warn"        "alarm"
   blue    cyan/blue           channel default     orange       red
```

Any of `LC`, `LW`, `HW`, `HC` may be unset (no threshold at that tier on
that side). Use cases:

- **Coolant temp**: `LW=70 HW=105 HC=115` — cold engine warning, normal
  band 70–105, hot warning, hot critical.
- **Oil pressure**: `LC=50 LW=100` — low-only thresholds. Below 0.5 bar is
  alarm-level engine death; below 1 bar is "something is wrong".
- **Battery voltage**: `LW=12.0 HW=14.8` — discharge warn on the low side,
  regulator-fault warn on the high side.
- **RPM**: `HW=redline HC=revlimit` — high-only.
- **Vehicle speed**: nothing set — no automatic threshold visualization.

### Threshold notation in the tables below

A compact text form for the threshold profile column:

| Notation | Meaning |
|---|---|
| `—` | No thresholds configured |
| `>X` | `high_warn = X` |
| `>>X` | `high_critical = X` |
| `<X` | `low_warn = X` |
| `<<X` | `low_critical = X` |
| `<X >Y` | both low and high warn, no criticals |
| `<<A <B >C >>D` | full four-tier profile |

So coolant reads: `<70 >105 >>115`. Oil pressure: `<<50 <100`.

### Zone colors

Each zone has a default color following this convention. Channels override
individual zones when they want a non-standard palette.

| Zone | Default | Intent |
|---|---|---|
| `LOW_CRITICAL` | `0x0040A0` deep blue | Below survivable range (cold engine, dead battery) |
| `LOW_WARN` | `0x4080FF` blue / cyan | Approaching low limit |
| `NORMAL` | *channel-specific* | Operating range — see `Color` column |
| `HIGH_WARN` | `0xFFA000` amber/orange | Approaching high limit |
| `HIGH_CRITICAL` | `0xFF0000` red | Above survivable / damage range |

Themes can override the whole palette globally.

### Tiers

UI presentation order in the channel picker:

- **1** = always shown first (basics every dash needs).
- **2** = common for tuned / performance cars.
- **3** = advanced / race / specialized.

---

## Engine — Core

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `rpm` | RPM | 1 | rpm | rpm | 0 | 0–8000 | `>6500 >>7200` | `0xFF3030` | OBD2 PID 0x0C. Most-used channel. Defaults are generic; real values per ECU preset. |
| `coolant_temp` | Coolant Temp | 1 | °C | °C | 0 | 0–120 | `<70 >105 >>115` | `0xFF8040` | Cold-blue → normal → hot-orange → hot-red. OBD2 PID 0x05. |
| `oil_temp` | Oil Temp | 1 | °C | °C | 0 | 0–130 | `<60 >120 >>130` | `0xFFA040` | Cold below 60 = don't rev. Hot above 120 = engine wear. |
| `oil_pressure` | Oil Pressure | 1 | kPa | bar | 1 | 0–10 | `<<50 <100` | `0xFFAA00` | Low-only. < 0.5 bar = engine death. |
| `intake_air_temp` | Intake Air Temp | 1 | °C | °C | 0 | -20–80 | `>55 >>70` | `0xFFA040` | High-only. Heat-soak threshold. OBD2 PID 0x0F. |
| `throttle_position` | Throttle Position | 1 | % | % | 0 | 0–100 | `—` | `0xFFD000` | OBD2 PID 0x11. |
| `engine_load` | Engine Load | 1 | % | % | 0 | 0–100 | `—` | `0xFFD000` | OBD2 PID 0x04. |
| `ignition_timing` | Ignition Advance | 2 | °BTDC | °BTDC | 1 | -10–50 | `—` | `0xFFD000` | OBD2 PID 0x0E. |
| `fuel_pressure` | Fuel Pressure | 2 | kPa | bar | 1 | 0–700 | `<<200 <250 >450` | `0xFFAA00` | Low = pump failure; high = regulator stuck. Defaults for ~3 bar EFI; ECU preset overrides. |
| `manifold_pressure` | MAP | 2 | kPa | kPa | 0 | 0–250 | `—` | `0xFFD000` | Absolute. OBD2 PID 0x0B. |
| `boost_pressure` | Boost | 2 | kPa | bar | 2 | -1–3 | `>200 >>250` | `0xFFD000` | Relative to ambient. High-only — overboost warn / critical. |
| `boost_target` | Boost Target | 2 | kPa | bar | 2 | -1–3 | `—` | `0xFFC080` | What the ECU is aiming for. |
| `mass_air_flow` | MAF | 2 | g/s | g/s | 1 | 0–500 | `—` | `0xFFD000` | OBD2 PID 0x10. |
| `afr_bank1` | AFR Bank 1 | 2 | AFR | AFR | 2 | 9–18 | `<11 >16` | `0x80FF80` | Lean and rich warnings — stoich = 14.7 (gasoline). |
| `afr_bank2` | AFR Bank 2 | 2 | AFR | AFR | 2 | 9–18 | `<11 >16` | `0x80FF80` | |
| `lambda_bank1` | Lambda Bank 1 | 2 | λ | λ | 3 | 0.6–1.3 | `<0.75 >1.10` | `0x80FF80` | Lambda 1.0 = stoich. |
| `lambda_bank2` | Lambda Bank 2 | 2 | λ | λ | 3 | 0.6–1.3 | `<0.75 >1.10` | `0x80FF80` | |
| `target_afr` | Target AFR | 2 | AFR | AFR | 2 | 9–18 | `—` | `0xA0FFA0` | What the ECU is aiming for. |
| `knock_count` | Knock Events | 3 | count | count | 0 | 0–99 | `>1 >>5` | `0xFF0000` | Any knock is bad — warn on first, critical at 5. |
| `knock_retard` | Knock Retard | 3 | ° | ° | 1 | 0–15 | `>2 >>6` | `0xFF4040` | Live retard amount being applied. |
| `injector_duty` | Injector Duty | 2 | % | % | 1 | 0–100 | `>85 >>95` | `0xFFD000` | Above 85% = approaching injector limit. |
| `injector_pulse_width` | Inj Pulse Width | 3 | ms | ms | 2 | 0–25 | `—` | `0xFFC080` | |
| `volumetric_efficiency` | VE | 3 | % | % | 0 | 0–150 | `—` | `0xC0C0C0` | Tuning aid. |
| `short_term_fuel_trim` | STFT | 3 | % | % | 1 | -25–25 | `<-15 >15` | `0xC0C0C0` | OBD2 PID 0x06/0x07. |
| `long_term_fuel_trim` | LTFT | 3 | % | % | 1 | -25–25 | `<-15 >15` | `0xC0C0C0` | OBD2 PID 0x08/0x09. |

## Engine — Exhaust

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `exhaust_gas_temp_avg` | EGT Average | 2 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | Single-probe or averaged. |
| `egt_cyl_1` | EGT Cyl 1 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `egt_cyl_2` | EGT Cyl 2 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `egt_cyl_3` | EGT Cyl 3 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `egt_cyl_4` | EGT Cyl 4 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `egt_cyl_5` | EGT Cyl 5 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `egt_cyl_6` | EGT Cyl 6 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `egt_cyl_7` | EGT Cyl 7 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `egt_cyl_8` | EGT Cyl 8 | 3 | °C | °C | 0 | 0–1000 | `>850 >>950` | `0xFF6020` | |
| `exhaust_back_pressure` | Exhaust Back Pressure | 3 | kPa | kPa | 0 | 0–500 | `>200` | `0xFF8040` | |
| `wastegate_duty` | Wastegate Duty | 3 | % | % | 1 | 0–100 | `—` | `0xFFD000` | |

## Drivetrain

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `vehicle_speed` | Vehicle Speed | 1 | km/h | km/h | 0 | 0–300 | `—` | `0xFFFFFF` | OBD2 PID 0x0D. Imperial flip → mph. |
| `gear` | Gear | 1 | enum | enum | — | — | `—` | `0xFFFFFF` | **Enum channel** (P/R/N/D/1–8). Has a built-in calculated path — when no direct CAN gear signal is configured, the dash computes it from RPM + vehicle_speed + user-supplied gear ratios + final drive + wheel circumference. Always selectable. See `gear_config` API. |
| `transmission_temp` | Trans Temp | 2 | °C | °C | 0 | 0–150 | `<60 >120 >>140` | `0xFFA040` | Auto/DCT/torque-converter primarily. |
| `transmission_pressure` | Trans Pressure | 3 | kPa | bar | 1 | 0–25 | `<<300 <500` | `0xFFAA00` | Auto/DCT. |
| `clutch_switch` | Clutch | 3 | bool | bool | — | 0/1 | `—` | `0x80C0FF` | Clutch pedal engaged. |
| `differential_temp` | Diff Temp | 3 | °C | °C | 0 | 0–150 | `>110 >>130` | `0xFFA040` | |
| `launch_control_active` | Launch Control | 3 | bool | bool | — | 0/1 | `—` | `0xFFD000` | |
| `traction_control_intervention` | TCS Intervention | 3 | % | % | 0 | 0–100 | `>30` | `0xFFD000` | Live intervention amount. |
| `antilag_active` | Antilag | 3 | bool | bool | — | 0/1 | `—` | `0xFF8040` | |

## Electrical

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `battery_voltage` | Battery | 1 | V | V | 1 | 10–16 | `<<11.5 <12.0 >14.8` | `0x80A0FF` | Low = discharge; high = regulator fault. |
| `alternator_voltage` | Alternator | 2 | V | V | 1 | 10–16 | `<<11.5 <12.0 >14.8` | `0x80A0FF` | |
| `alternator_current` | Alt Current | 3 | A | A | 0 | 0–150 | `—` | `0x80A0FF` | |
| `ecu_voltage` | ECU Voltage | 3 | V | V | 2 | 10–16 | `<<11.0 <11.5` | `0x80A0FF` | Critical-low to flag brown-out. |
| `system_current` | System Current | 3 | A | A | 0 | 0–250 | `—` | `0x80A0FF` | |

## Chassis — Dynamics

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `lateral_g` | Lateral G | 2 | g | g | 2 | -2.5–2.5 | `—` | `0xC0C0C0` | Positive = right. |
| `longitudinal_g` | Longitudinal G | 2 | g | g | 2 | -2.5–2.5 | `—` | `0xC0C0C0` | Positive = accel. |
| `vertical_g` | Vertical G | 3 | g | g | 2 | -2.5–2.5 | `—` | `0xC0C0C0` | |
| `yaw_rate` | Yaw Rate | 3 | °/s | °/s | 1 | -180–180 | `—` | `0xC0C0C0` | |
| `steering_angle` | Steering Angle | 3 | ° | ° | 1 | -540–540 | `—` | `0xC0C0C0` | |

## Chassis — Per-Corner (FL/FR/RL/RR)

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `wheel_speed_fl` | Wheel Speed FL | 3 | km/h | km/h | 0 | 0–300 | `—` | `0xFFFFFF` | |
| `wheel_speed_fr` | Wheel Speed FR | 3 | km/h | km/h | 0 | 0–300 | `—` | `0xFFFFFF` | |
| `wheel_speed_rl` | Wheel Speed RL | 3 | km/h | km/h | 0 | 0–300 | `—` | `0xFFFFFF` | |
| `wheel_speed_rr` | Wheel Speed RR | 3 | km/h | km/h | 0 | 0–300 | `—` | `0xFFFFFF` | |
| `tire_temp_fl` | Tire Temp FL | 3 | °C | °C | 0 | 0–150 | `<60 >110` | `0xFFA040` | Cold tires; overheated tires. |
| `tire_temp_fr` | Tire Temp FR | 3 | °C | °C | 0 | 0–150 | `<60 >110` | `0xFFA040` | |
| `tire_temp_rl` | Tire Temp RL | 3 | °C | °C | 0 | 0–150 | `<60 >110` | `0xFFA040` | |
| `tire_temp_rr` | Tire Temp RR | 3 | °C | °C | 0 | 0–150 | `<60 >110` | `0xFFA040` | |
| `tire_pressure_fl` | Tire Pressure FL | 3 | kPa | psi | 1 | 100–350 | `<<150 <180 >280` | `0xC0C0C0` | TPMS-style. |
| `tire_pressure_fr` | Tire Pressure FR | 3 | kPa | psi | 1 | 100–350 | `<<150 <180 >280` | `0xC0C0C0` | |
| `tire_pressure_rl` | Tire Pressure RL | 3 | kPa | psi | 1 | 100–350 | `<<150 <180 >280` | `0xC0C0C0` | |
| `tire_pressure_rr` | Tire Pressure RR | 3 | kPa | psi | 1 | 100–350 | `<<150 <180 >280` | `0xC0C0C0` | |
| `suspension_travel_fl` | Susp Travel FL | 3 | mm | mm | 0 | 0–200 | `—` | `0xC0C0C0` | |
| `suspension_travel_fr` | Susp Travel FR | 3 | mm | mm | 0 | 0–200 | `—` | `0xC0C0C0` | |
| `suspension_travel_rl` | Susp Travel RL | 3 | mm | mm | 0 | 0–200 | `—` | `0xC0C0C0` | |
| `suspension_travel_rr` | Susp Travel RR | 3 | mm | mm | 0 | 0–200 | `—` | `0xC0C0C0` | |
| `ride_height_fl` | Ride Height FL | 3 | mm | mm | 0 | 0–300 | `—` | `0xC0C0C0` | |
| `ride_height_fr` | Ride Height FR | 3 | mm | mm | 0 | 0–300 | `—` | `0xC0C0C0` | |
| `ride_height_rl` | Ride Height RL | 3 | mm | mm | 0 | 0–300 | `—` | `0xC0C0C0` | |
| `ride_height_rr` | Ride Height RR | 3 | mm | mm | 0 | 0–300 | `—` | `0xC0C0C0` | |

## Brakes & Driver Inputs

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `brake_pressure_front` | Brake Pressure Front | 2 | kPa | bar | 0 | 0–200 | `—` | `0xFF4040` | |
| `brake_pressure_rear` | Brake Pressure Rear | 2 | kPa | bar | 0 | 0–200 | `—` | `0xFF4040` | |
| `brake_pedal_position` | Brake Pedal | 3 | % | % | 0 | 0–100 | `—` | `0xFF4040` | |
| `accel_pedal_position` | Accel Pedal | 2 | % | % | 0 | 0–100 | `—` | `0xFFD000` | Drive-by-wire. OBD2 PID 0x49. |
| `handbrake` | Handbrake | 3 | bool | bool | — | 0/1 | `—` | `0xFF4040` | |

## Fuel & Distance

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `fuel_level` | Fuel Level | 1 | % | % | 0 | 0–100 | `<<5 <15` | `0x40C040` | OBD2 PID 0x2F. Warn at 15, critical at 5. |
| `fuel_remaining_distance` | Fuel Range | 2 | km | km | 0 | 0–1000 | `<<20 <50` | `0x40C040` | Calculated. Imperial → mi. |
| `fuel_consumption_instant` | Inst Fuel Use | 3 | L/h | L/h | 1 | 0–60 | `—` | `0xC0C0C0` | |
| `fuel_consumption_avg` | Avg Fuel Use | 3 | L/100km | L/100km | 1 | 0–40 | `—` | `0xC0C0C0` | |
| `odometer` | Odometer | 2 | km | km | 0 | 0–9999999 | `—` | `0xFFFFFF` | Persistent. |
| `trip_distance` | Trip Distance | 2 | km | km | 1 | 0–9999 | `—` | `0xFFFFFF` | |

## Environment

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `ambient_temp` | Ambient Temp | 2 | °C | °C | 0 | -30–55 | `—` | `0xC0C0C0` | OBD2 PID 0x46. |
| `cabin_temp` | Cabin Temp | 3 | °C | °C | 0 | -10–50 | `—` | `0xC0C0C0` | |
| `barometric_pressure` | Barometric Pressure | 3 | kPa | kPa | 0 | 60–110 | `—` | `0xC0C0C0` | OBD2 PID 0x33. |

## Lap Timing & Race

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `lap_time_current` | Lap Time (Current) | 3 | s | s | 3 | 0–9999 | `—` | `0xFFD000` | Formatted as M:SS.sss in UI. |
| `lap_time_last` | Lap Time (Last) | 3 | s | s | 3 | 0–9999 | `—` | `0xFFFFFF` | |
| `lap_time_best` | Lap Time (Best) | 3 | s | s | 3 | 0–9999 | `—` | `0x40C040` | |
| `lap_number` | Lap Number | 3 | count | count | 0 | 0–999 | `—` | `0xFFFFFF` | |
| `lap_delta_best` | Lap Delta vs Best | 3 | s | s | 2 | -60–60 | `—` | `0xFFD000` | Negative = ahead. |
| `sector_time_current` | Sector Time | 3 | s | s | 3 | 0–999 | `—` | `0xFFD000` | |

## Body & Lights

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `headlights` | Headlights | 3 | bool | bool | — | 0/1 | `—` | `0xFFFFFF` | |
| `high_beam` | High Beam | 3 | bool | bool | — | 0/1 | `—` | `0x80A0FF` | |
| `turn_signal_left` | Left Turn | 3 | bool | bool | — | 0/1 | `—` | `0x40C040` | |
| `turn_signal_right` | Right Turn | 3 | bool | bool | — | 0/1 | `—` | `0x40C040` | |
| `hazards` | Hazards | 3 | bool | bool | — | 0/1 | `—` | `0xFFD000` | |
| `door_open` | Door Open | 3 | bool | bool | — | 0/1 | `—` | `0xFFD000` | Any door. |

## Diagnostic / OBD2

| ID | Label | Tier | Native | Display | Dec | Range | Thresholds | Color | Notes |
|---|---|---|---|---|---|---|---|---|---|
| `check_engine` | Check Engine | 2 | bool | bool | — | 0/1 | `>0` | `0xFFA000` | MIL light — high "warn" fires immediately when on. |
| `dtc_count` | DTC Count | 3 | count | count | 0 | 0–99 | `>0 >>5` | `0xFFA000` | Stored trouble codes. |
| `coolant_level_low` | Coolant Level Low | 3 | bool | bool | — | 0/1 | `>0` | `0xFF8040` | Warning sensor. |
| `oil_level_low` | Oil Level Low | 3 | bool | bool | — | 0/1 | `>0` | `0xFFAA00` | Warning sensor. |
| `distance_with_mil_on` | Distance w/ MIL | 3 | km | km | 0 | 0–99999 | `—` | `0xC0C0C0` | OBD2 PID 0x21. |

---

## Custom Channels (User-Defined)

When a user has a data point that isn't in this list (e.g. methanol injection
duty, water injection flow, specific accessory monitoring), they can define
their own channel. Convention:

- **ID prefix**: `custom_` followed by snake_case (e.g. `custom_meth_dc`).
- The custom channel works identically to canonical ones on the user's dash.
- Layouts referencing custom channels **will NOT auto-bind on other dashes**
  on download. The web UI flags this with a "custom — won't auto-bind on
  shared layouts" warning when defining one.
- If a custom channel later becomes widely used and should be canonical, it
  gets added to this list in a future firmware release. The user's old
  layouts continue to work; the canonical version becomes the recommended
  ID.

---

## Color Palette Intent (NORMAL Zone)

The default colors above follow a rough convention. Themes can override
this entire palette via design tokens — these are the FALLBACK when no
theme is active.

| Group | Intent | Hex |
|---|---|---|
| Engine speed / tach | Energy / danger | `0xFF3030` |
| Coolant / cylinder temps | Heat — warn high | `0xFF8040` |
| Oil pressure / fuel pressure | Critical fluid — warn low | `0xFFAA00` |
| Oil temp / IAT / generic temps | Warm gauge | `0xFFA040` |
| Boost / throttle / VE / load | Performance / power | `0xFFD000` |
| AFR / Lambda | Tuning / mixture | `0x80FF80` |
| Battery / alternator / voltage | Electrical | `0x80A0FF` |
| Fuel level / range | Reserve | `0x40C040` |
| Brake | Critical | `0xFF4040` |
| Knock / fault | Alarm | `0xFF0000` |
| EGT | Exhaust heat | `0xFF6020` |
| Speed / odometer / neutral | Neutral | `0xFFFFFF` |
| G-force / suspension / dyn | Telemetry | `0xC0C0C0` |
| Warning / caution | Caution | `0xFFA000` |

---

## Total Count

**90 canonical channels** across 12 groups. Sized so the firmware-embedded
registry is ~11 KB ROM (slightly larger than v1.0 thanks to the threshold
profile fields), negligible against the 16 MB flash.

## Channel Struct (data model)

```c
typedef struct {
    char     id[32];
    char     label[32];
    char     signal_name[32];
    int16_t  signal_index;     // runtime cache
    char     units_native[8];  // °C, kPa, V, ...
    char     units_display[8]; // °F, bar, psi, ... — user-overrideable
    uint8_t  decimals;
    int32_t  min, max;         // display range
    int32_t  sanity_min, sanity_max; // values outside = treat as bad data

    /* Threshold profile — sentinel values when unset: low_* = INT32_MIN,
     * high_* = INT32_MAX. Comparisons short-circuit cleanly. */
    int32_t  low_critical;
    int32_t  low_warn;
    int32_t  high_warn;
    int32_t  high_critical;

    /* Zone colors — 0xFFFFFFFF means "use the default convention color
     * for this zone". Saves having to specify all 5 per channel; only
     * the override-from-convention ones need a value. */
    uint32_t color_normal;
    uint32_t color_low_warn;
    uint32_t color_low_critical;
    uint32_t color_high_warn;
    uint32_t color_high_critical;

    uint8_t  tier;          // 1, 2, or 3
    uint8_t  cardinality;   // 0 = scalar, 1 = enum, 2 = boolean
    uint8_t  group;         // ENGINE_CORE, DRIVETRAIN, etc.

    /* Calculated-channel hook — when non-null, channel value comes from
     * this function instead of a direct signal subscription. Used by
     * gear, fuel_remaining_distance, fuel_consumption_avg, etc. */
    void (*calculate_fn)(struct channel *self);
} channel_t;
```

## Open Questions for Review

1. **8 EGT channels or 12?** Currently `egt_cyl_1` through `_8`. I-6 and V-12 builds want 12. Cheap to add (~400 bytes).
2. **Pressure display unit defaults**: I picked bar for oil/fuel/boost, kPa for MAP/exhaust back pressure, psi for tires. The mix follows racing convention but is inconsistent. Want it homogenized?
3. **Lap timing channels at Tier 3** — assumes the dash will support lap timing eventually. Drop if out of scope for now.
4. **Should I tag OBD2-universal channels** with a flag so the "Generic OBD2" preset auto-populates just those without the user picking?
5. **Missing channels** — race-heavy users sometimes want: coolant flow rate, oil cooler delta-T, fuel pressure differential, individual TPMS battery voltages, individual cylinder knock. Left out as edge-case (use `custom_`). Anything else?
6. **Bool channel thresholds**: I wrote `>0` for things like `check_engine` and `oil_level_low` so they trigger HIGH_WARN immediately when the sensor goes true. Confirm that's the right semantic — a bool warning should fire on the "on" state, not need a numeric threshold setup per-channel.

Mark up this file with edits, add/remove rows, leave notes inline. I'll
fold your feedback into v1.2 and then generate the C header + initial
ECU preset mappings from it.
