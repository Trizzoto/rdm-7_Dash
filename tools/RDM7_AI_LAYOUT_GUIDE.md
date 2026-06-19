# RDM-7 Dash — AI Layout Authoring Guide

You are generating a **dashboard layout** for the RDM-7 Dash (an automotive
instrument cluster: an 800×480 screen driven by ESP32 firmware). Your output is
a single **layout JSON** object. The user pastes it into the RDM-7 web studio
(**Setup → Load JSON**, or the **☰ menu → File → Load JSON…**), where it renders
instantly with no hardware needed, and can then be pushed to a real dash.

This guide + the attached `widgets.schema.json` (authoritative field list) +
`ford_cluster.json` (a full worked example) are everything you need.

---

## 1. Output contract

Output **one JSON object**, nothing else (no prose, no markdown fences around it
unless asked). Shape:

```json
{
  "name": "my_layout",
  "widgets": [
    { "type": "arc",  "x": -180, "y": 0, "w": 320, "h": 320, "config": { ... } },
    { "type": "text", "x": 200,  "y": 0, "w": 220, "h": 90,  "config": { ... } }
  ]
}
```

- `name` — short identifier (lowercase, no spaces). Avoid `"default"`.
- `widgets` — an array. **Draw order = array order** (later widgets paint on top).
- Each widget: `type`, position `x`/`y`, size `w`/`h`, and a `config` object of
  type-specific fields.
- Do **not** include a top-level `signals` array — binding is by name (see §6).

---

## 2. Coordinate system (READ THIS — it's the #1 mistake)

**The origin (0,0) is the CENTRE of the screen.** Not the top-left.

- `x`, `y` are the **centre of the widget**, in pixels, relative to screen centre.
- Screen is 800 wide × 480 tall, so usable ranges are roughly:
  - `x`: **−400 (far left) … +400 (far right)**
  - `y`: **−240 (top) … +240 (bottom)**
- A widget at `x: 0, y: 0` sits dead centre. `x: -180` is left of centre.
  `y: -150` is up high.
- `w`/`h` are the widget's width/height in pixels.

Keep widgets inside the screen: `x − w/2 ≥ −400`, `x + w/2 ≤ 400`, similarly for y.

---

## 3. Colours — ALWAYS use hex strings

Write every colour as a **`"#RRGGBB"` hex string** (e.g. `"#33C0F0"`, `"#FF3B30"`,
`"#0000FF"`). Short form `"#RGB"` also works.

> Why this matters: the device stores colours as 16-bit RGB565 integers, so a bare
> integer colour ≤ 65535 is interpreted as RGB565 and a pure blue/green would come
> out wrong. Hex strings are unambiguous and always read as true RGB888. Just use
> hex strings everywhere and you never have to think about it.

The screen is RGB565 (32 levels red, 64 green, 32 blue), so very subtle gradients
will show slight banding on real hardware — fine for design, just don't rely on
ultra-smooth fades.

---

## 4. Sizes, fonts, units

- **Fonts**: `"Family:size"`, e.g. `"Montserrat:48"`. Reliable built-in families:
  **`Montserrat`**, **`Fugaz One`**, **`Manrope Bold`**. Pick a size in px (8–500).
- **Numbers/decimals**: most value widgets have a `decimals` field (0 = whole
  numbers). The displayed unit comes from the bound channel.
- **Defaults-only is best**: only set fields you actually want to change from the
  firmware default. Omitted fields fall back to sensible defaults. This keeps the
  layout small (there is a **~32 KB** size budget for the whole layout — don't
  emit hundreds of redundant fields).

---

## 5. Widget catalogue

16 widget types. Full field lists are in `widgets.schema.json` — consult it for
anything beyond the common fields below.

| type | purpose |
|---|---|
| `arc` | Curved gauge with a filled arc (tach, speedo, temp). The workhorse. |
| `meter` | Round gauge with a **needle** + tick scale (classic analog look). |
| `bar` | Horizontal/vertical fill bar (fuel, boost, temp strips). |
| `text` | A value or static text readout (digital speed, gear, labels). |
| `pathbar` | A fill gauge that follows a **custom path** (e.g. an L/J-shaped tach). |
| `panel` | A labelled value "card" with optional warning thresholds. |
| `line` | A straight divider line. |
| `shape_panel` | A background rectangle / shape (panels, pods, backdrops). |
| `image` | An uploaded image/icon (`.rdmimg`). **Won't render unless the asset exists on the device** — avoid for AI-only layouts, or tell the user to upload it. |
| `indicator` | Slotted status dot (max 2). |
| `warning` | Slotted telltale/warning light (max 8). |
| `banner` | Full-width message banner. |
| `rpm_bar` | Segmented shift-light style RPM bar. |
| `shift_light` | Shift light. |
| `toggle` / `button` | Interactive controls. |

### Common fields for the gauges you'll use most

**`arc`** (default size 200×200; make tachs/speedos bigger, e.g. 320×320):
`signal_min`, `signal_max`, `start_angle_user` (0 = 12 o'clock, clockwise; e.g.
225 for a 7-o'clock start), `sweep_degrees` (e.g. 270), `arc_width`, `arc_color`,
`bg_arc_color`, `bg_arc_width`, `rounded_ends` (bool), `show_ticks`,
`minor_tick_step`, `major_tick_step`, `tick_label_divisor` (e.g. 1000 → a 0–8000
range labels as 0–8), `redline_arc_width`, `redline_color`, `arc_high` (redline
starts here), `reverse`.

**`meter`** (needle gauge, default 300×300): `min`, `max`, `start_angle_user`,
`sweep_degrees`, `show_needle`, `needle_color`, `needle_width`, `redline_enabled`,
`redline_threshold`, `redline_color`, `show_ticks`, `major_tick_step`,
`minor_tick_step`, `tick_label_divisor`, `meter_bg_color`, `meter_bg_opa`.

**`text`** (default 100×30): `static_text` (fixed string; omit to show the bound
value), `decimals`, `font`, `text_color`, `rotation`.

**`bar`** (default 300×30): `bar_min`, `bar_max`, `fill_dir`, `bar_in_range_color`,
`bar_bg_color`, `show_bar_value`, `decimals`, `value_font`, `label`, `label_font`,
`bar_alerts_enabled`, `bar_low`/`bar_high` (+ their colours), `bar_radius`.

**`pathbar`** (custom-path fill gauge): `min`, `max`, `band_width`, `lit_color`,
`dim_color`, `redline`, `redline_color`, `rounded` (bool), plus a tick/number
scale: `show_ticks`, `minor_tick_step`, `major_tick_step`, `show_labels`,
`tick_label_divisor`, `tick_color`, `major_tick_color`, `label_color`,
`label_font`. The path itself comes from `shape`/`orientation` (built-in
L-bend) or a custom `path` array of absolute screen-pixel points
`[x0,y0,x1,y1,…]` (centre-origin coords). See `ford_cluster.json` for a real
J-shaped tach path.

**`shape_panel`** (backdrop): `shape_type`, `bg_color`, `bg_opa`, `border_color`,
`border_width`, `border_radius`. Place these **first** in the array so gauges
draw on top.

**`panel`** (value card): `label`, `custom_text`, `decimals`, `bg_color`,
`bg_opa`, `label_color`, `value_color`, `value_font`, `border_radius`,
`warning_high_enabled` + `warning_high_threshold` + `warning_high_color`.

---

## 6. Binding widgets to live data

A widget shows live data by binding to a **signal name** via `config.signal_name`:

```json
{ "type": "arc", "x": -180, "y": 0, "w": 320, "h": 320,
  "config": { "signal_name": "rpm", "signal_max": 8000, "arc_color": "#33C0F0" } }
```

**Canonical channel ids** (use these as `signal_name` — they're the portable,
device-independent names):

`rpm`, `vehicle_speed`, `coolant_temp`, `oil_temp`, `oil_pressure`,
`fuel_level`, `battery_voltage`, `manifold_pressure` (MAP), `boost_pressure`,
`intake_air_temp`, `throttle_position`, `gear`, `lambda_bank1`, `afr_bank1`,
`ignition_timing`, `engine_load`, `fuel_pressure`, `mass_air_flow`, `runtime`,
`odometer`, `ambient_temp`, `ethanol_pct`, `transmission_temp`,
`steering_angle`, `lateral_g`, `exhaust_gas_temp_avg` … (the firmware has ~100;
these are the common ones). The studio's channels editor lists what a given car
has — those names are authoritative for that device.

**Offline-preview values**: when the user previews in the studio with no device,
these names get realistic demo values automatically (and animate with **Sim**):
`rpm` (≈4500), `speed`/`vehicle_speed` (≈55), `coolant`/`coolant_temp` (≈90),
`oil_temp` (≈104), `oil_pres`/`oil_pressure` (≈58), `fuel`/`fuel_level` (≈62),
`volt`/`battery_voltage` (≈13.8), `boost`/`map`/`manifold_pressure` (≈12),
`gear` (4). Any other name shows a placeholder (~42). So for a good-looking
preview, prefer names containing those keywords.

> On a **real device**, `signal_name` must match a signal the car actually
> provides (set up in the studio's Channels editor / ECU preset). Binding to the
> canonical ids above is the portable choice. If a name isn't present on that
> device, the widget just reads `--`.

---

## 7. Constraints / gotchas

- **Whole layout ≤ ~32 KB.** Emit only non-default fields. Don't bloat.
- **`indicator` max 2, `warning` max 8** instances.
- **`image` widgets render blank** unless the named `.rdmimg` asset is already on
  the device. For AI-generated layouts, prefer drawing things with
  `arc`/`meter`/`bar`/`text`/`shape_panel` rather than images.
- Keep widgets on-screen (§2). Background `shape_panel`s go **first** in the array.
- Angles: `start_angle_user` 0 = top (12 o'clock), increasing clockwise.

---

## 8. Worked examples

### A. Minimal — one RPM tach

```json
{
  "name": "simple_tach",
  "widgets": [
    { "type": "arc", "x": 0, "y": 0, "w": 360, "h": 360,
      "config": {
        "signal_name": "rpm",
        "signal_min": 0, "signal_max": 8000,
        "start_angle_user": 225, "sweep_degrees": 270,
        "arc_width": 26, "arc_color": "#33C0F0",
        "bg_arc_color": "#1A1F26", "bg_arc_width": 26,
        "rounded_ends": false,
        "show_ticks": true, "minor_tick_step": 500, "major_tick_step": 1000,
        "tick_label_divisor": 1000, "major_tick_color": "#FFFFFF",
        "arc_high": 6500, "redline_arc_width": 26, "redline_color": "#FF3B30"
      } },
    { "type": "text", "x": 0, "y": 70, "w": 200, "h": 80,
      "config": { "signal_name": "rpm", "decimals": 0, "font": "Montserrat:44", "text_color": "#FFFFFF" } }
  ]
}
```

### B. Small cluster — tach + digital speed + coolant bar

```json
{
  "name": "mini_cluster",
  "widgets": [
    { "type": "shape_panel", "x": 0, "y": 0, "w": 800, "h": 480,
      "config": { "shape_type": "rect", "bg_color": "#06090E", "bg_opa": 255 } },

    { "type": "arc", "x": -210, "y": -10, "w": 320, "h": 320,
      "config": { "signal_name": "rpm", "signal_min": 0, "signal_max": 8000,
        "start_angle_user": 225, "sweep_degrees": 270, "arc_width": 24,
        "arc_color": "#33C0F0", "bg_arc_color": "#1A1F26", "bg_arc_width": 24,
        "rounded_ends": false, "show_ticks": true, "major_tick_step": 1000,
        "tick_label_divisor": 1000, "arc_high": 6500,
        "redline_arc_width": 24, "redline_color": "#FF3B30" } },

    { "type": "text", "x": 210, "y": -30, "w": 260, "h": 130,
      "config": { "signal_name": "vehicle_speed", "decimals": 0,
        "font": "Fugaz One:120", "text_color": "#FFFFFF" } },
    { "type": "text", "x": 210, "y": 70, "w": 200, "h": 40,
      "config": { "static_text": "MPH", "font": "Montserrat:24", "text_color": "#7A8290" } },

    { "type": "bar", "x": 0, "y": 200, "w": 520, "h": 26,
      "config": { "signal_name": "coolant_temp", "bar_min": 40, "bar_max": 120,
        "bar_in_range_color": "#33C0F0", "bar_bg_color": "#1A1F26",
        "bar_radius": 6, "label": "COOLANT", "label_font": "Montserrat:16",
        "label_color": "#7A8290", "show_bar_value": true, "value_font": "Montserrat:16",
        "bar_alerts_enabled": true, "bar_high": 110, "bar_high_color": "#FF3B30" } }
  ]
}
```

---

## 9. Before you output — checklist

1. Single JSON object, `name` + `widgets[]`.
2. All positions centre-origin and on-screen (§2).
3. **All colours are `"#RRGGBB"` strings** (§3).
4. Fonts are `"Family:size"` with a real family (§4).
5. Data widgets have a `signal_name` from §6.
6. Background `shape_panel`s first; only non-default fields set; layout stays small.
7. No `image` widgets unless the user confirmed the asset exists.

Then tell the user: **paste it into the studio → Setup → Load JSON → Load into
Editor** (or ☰ → File → Load JSON…). It previews offline; hit **Save** to push to
a connected dash.
