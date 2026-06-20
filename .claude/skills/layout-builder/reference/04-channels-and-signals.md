# Channels & signals — what to bind to

A widget shows data by setting `config.signal_name` to a **channel signal name**.
Channels are the device's named data bindings (they own the CAN decode, units,
and thresholds — see ADR-0005). A layout does **not** define decode; it just
references channels that already exist on the device.

## Get the live list (do this first when a device is reachable)

```
GET /api/channels   →   { "channels": [ { "id", "signal", "units"/"units_display", ... }, … ] }
```

Bind to the `signal` value (UPPER_SNAKE), e.g. `"signal_name": "COOLANT_TEMP"`.
`scripts/apply_layout.py --channels` prints them. Binding to a name that doesn't
exist isn't fatal — the widget just shows `--` until that channel is configured.

## Common canonical channels (typical engine build)

| signal_name | meaning | units |
|---|---|---|
| `RPM` | engine speed | rpm |
| `VEHICLE_SPEED` | road speed | km/h |
| `COOLANT_TEMP` | coolant temperature | °C |
| `OIL_TEMP` | oil temperature | °C |
| `OIL_PRESSURE` | oil pressure | kPa/bar/psi |
| `MAP` | manifold pressure / boost | kPa |
| `THROTTLE` | throttle position | % |
| `ENGINE_LOAD` | calculated load | % |
| `INTAKE_AIR_TEMP` | intake air temp | °C |
| `BATTERY_VOLTAGE` | battery / system voltage | V |
| `FUEL_LEVEL` | fuel level | % |
| `LAMBDA` / `AFR` | mixture | λ / AFR |
| `IGNITION` | ignition timing | °BTDC |
| `GEAR` | engaged gear | enum (use a value_map) |

## Built-in "dash" channels (always present, no ECU needed — great for demos)

| signal_name | meaning | units |
|---|---|---|
| `FPS` | render frame rate | fps |
| `CPU_PERCENT` | CPU load | % |
| `FREE_HEAP_KB` | free heap | kB |
| `FREE_PSRAM_KB` | free PSRAM | kB |
| `UPTIME_S` | uptime | s |
| `CHIP_TEMP` | SoC temperature | °C |
| `WIFI_RSSI` | Wi-Fi signal | dBm |

## GEAR display

`GEAR` reads as a number; map it to letters with a `value_map` in `signals[]`:

```jsonc
"signals": [
  { "name": "GEAR", "value_map": [
    {"v":0,"label":"N"}, {"v":1,"label":"1"}, {"v":2,"label":"2"},
    {"v":3,"label":"3"}, {"v":4,"label":"4"}, {"v":5,"label":"5"}, {"v":6,"label":"6"} ] }
]
```
Then a `text` or `panel` widget bound to `GEAR` shows the letter.

## Units & decimals

Units come from the channel (a `panel`/`bar`/`text` can show the unit suffix via
its own field — see the catalog). Don't bake a unit into a label unless you mean
to override it.
