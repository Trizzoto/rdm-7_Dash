# Channels & signals — what to bind to

A widget shows data by setting `config.signal_name` to a **channel's signal
name**. Channels are the device's named data bindings — they own the CAN
decode, units and thresholds (ADR-0005/0006). A layout does **not** define
decode; it just references channels that already exist on the device. That
separation is what makes a layout portable between dashes.

## Always read the live list first

```
GET /api/channels   →   { "channels": [ { "id", "signal", "units", "units_display", … }, … ] }
```

Bind to the **`signal`** value, not the `id`.

> ⚠ **`signal` is NOT simply `id.upper()`.** When a channel is bound to an ECU
> preset, its signal name is derived from that **preset's label** — uppercased
> with non-alphanumerics collapsed to `_`. So the `rpm` channel is `RPM` on one
> ECU and `ENGINE_RPM` on another; a wheel-speed channel can come out
> `WHEEL_SPD_FL`. Guessing gets you a widget stuck on `--`.
>
> Derived/calculated channels (`channel_math`) *do* use `UPPER(id)`, and an
> unbound canonical channel adopts whatever signal it first resolves to. None
> of that is worth reasoning about — **read `/api/channels`.**

`python scripts/apply_layout.py --channels` prints them. Binding to a name that
doesn't exist isn't fatal — the widget just shows `--` until that channel is
configured. `validate_layout.py --channels-file channels.json` warns about it
before you ever apply.

## Channel ids you'll meet

These are canonical **ids** (there are ~113). Use them to find the right row in
`/api/channels`, then bind to that row's `signal`.

**Engine** `rpm` · `coolant_temp` · `oil_temp` · `oil_pressure` ·
`intake_air_temp` · `throttle_position` · `engine_load` · `ignition_timing` ·
`fuel_pressure` · `manifold_pressure` · `boost_pressure` · `boost_target` ·
`mass_air_flow` · `injector_duty` · `knock_count` · `knock_retard` ·
`volumetric_efficiency` · `short_term_fuel_trim` · `long_term_fuel_trim` ·
`exhaust_gas_temp_avg` · `wastegate_duty`

**Mixture** `afr_bank1` / `afr_bank2` · `lambda_bank1` / `lambda_bank2` ·
`wideband_1` / `wideband_2` · `target_afr` · `target_lambda` ·
`lambda_correction`

**Drivetrain** `vehicle_speed` · `gear` · `transmission_temp` ·
`transmission_pressure` · `clutch_switch` · `differential_temp` ·
`launch_control_active` · `traction_control_intervention` · `antilag_active`

**Chassis** `lateral_g` · `longitudinal_g` · `vertical_g` · `yaw_rate` ·
`steering_angle` · `wheel_speed_*` · `tire_temp_*` · `tire_pressure_*` ·
`suspension_travel_*` · `ride_height_*` · `brake_pressure_front` /
`_rear` · `brake_pedal_position` · `accel_pedal_position` · `handbrake`

**Electrical** `battery_voltage` · `alternator_voltage` · `alternator_current` ·
`ecu_voltage` · `system_current`

**Fuel & trip** `fuel_level` · `fuel_remaining_distance` ·
`fuel_consumption_instant` / `_avg` · `fuel_rate` · `ethanol_pct` ·
`odometer` · `trip_distance`

**Lap timing** `lap_time_current` · `lap_time_last` · `lap_time_best` ·
`lap_time_theoretical` · `lap_delta_best` · `lap_number` ·
`sector_time_current` · `sector_number`

**GPS** `gps_latitude` · `gps_longitude` · `gps_speed` · `gps_altitude` ·
`gps_heading` · `gps_satellites` · `gps_fix_type` · `gps_accuracy`
— `gps_latitude`/`gps_longitude` are what `track_map` reads by default
(see 07-assets-and-tracks).

**Telltales** `headlights` · `high_beam` · `turn_signal_left` / `_right` ·
`hazards` · `door_open` · `check_engine` · `dtc_count` · `coolant_level_low` ·
`oil_level_low`

**Ambient** `ambient_temp` · `cabin_temp` · `barometric_pressure`

## Built-in "dash" channels (always present, no ECU needed — great for demos)

These register themselves, so their signal names *are* fixed:

| signal_name | meaning | units |
|---|---|---|
| `FPS` | render frame rate | fps |
| `CPU_PERCENT` | CPU load | % |
| `FREE_HEAP_KB` | free heap | kB |
| `FREE_PSRAM_KB` | free PSRAM | kB |
| `UPTIME_S` | uptime | s |
| `CHIP_TEMP` | SoC temperature | °C |
| `WIFI_RSSI` | Wi-Fi signal | dBm |

Their channel ids are `dash_fps`, `dash_cpu`, `dash_free_heap`,
`dash_free_psram`, `dash_uptime`, `dash_chip_temp`, `dash_wifi_rssi`.

## Thresholds live on the channel, not the layout

`arc` and `meter` have `redline_enabled` / `redline_threshold` — but a bound
channel that carries a **high threshold** switches the warning zone on by itself
and its value wins. Likewise the arc's `arc_low`/`arc_high` alert points and the
bar's colour alerts are **read from the channel's thresholds**, which is why
those fields are flagged editor-only in the catalog. To change where a gauge
goes red, change the *channel*, not the layout.

## GEAR display

`gear` reads as a number; map it to letters with a `value_map` in `signals[]`:

```jsonc
"signals": [
  { "name": "GEAR", "value_map": [
    {"v":0,"label":"N"}, {"v":1,"label":"1"}, {"v":2,"label":"2"},
    {"v":3,"label":"3"}, {"v":4,"label":"4"}, {"v":5,"label":"5"}, {"v":6,"label":"6"} ] }
]
```
Then a `text` or `panel` widget bound to `GEAR` shows the letter. (Use the
actual signal name from `/api/channels` as `"name"`.)

## Units & decimals

Units come from the channel — `units` is the **native/source** unit the ECU
sends, `units_display` is what the dash shows after conversion. A `panel`/`bar`/
`text` pulls the display unit and decimal count itself via its `show_unit`
field. Don't bake a unit into a label unless you mean to override it, and never
convert values in the layout.

## Feeding values while you build

```
POST /api/signal/inject   {"signal":"RPM","value":4000}
POST /api/signal/inject   {"values":[{"signal":"RPM","value":4000},
                                     {"signal":"COOLANT_TEMP","value":92}]}   // up to 16
POST /api/signal/clear    {"signal":"RPM"}   |   {"all":true}
```

The response is `{"injected":[…],"unknown":[…]}` — **check `unknown`.** It
exists precisely so a typo'd signal name fails loudly here instead of
mysteriously downstream. `/api/signal/simulate` drives the bench simulator if
you want motion rather than a held value.
