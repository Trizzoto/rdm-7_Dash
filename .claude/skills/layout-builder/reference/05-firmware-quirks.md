# Firmware quirks — where the device JSON differs from the catalog

The catalog (02) is generated from the editor's schema. A few fields are
**converted by the editor before saving**, so the *device JSON* (what you author
here) wants different fields than the catalog shows. Verified deviations:

## METER ticks are COUNT-based, not step-based ⚠️

For a `meter`, the device JSON uses:

- `minor_tick_count` — **total** minor ticks across the whole min..max range.
- `major_tick_every` — every Nth minor tick is promoted to a major tick.

The catalog's `minor_tick_step` / `major_tick_step` for meter are **editor-only**
(the editor's `_meterSyncDerived` converts them to count/every on save) — setting
them in the device JSON does nothing for minor/major ticks.

Convert "ticks every S units, majors every M units" over range `min..max`:

```
minor_tick_count = round((max - min) / S) + 1
major_tick_every = round(M / S)
```

Example — a 0–8000 tach, minor every 250, major every 1000:
`minor_tick_count = 8000/250 + 1 = 33`, `major_tick_every = 1000/250 = 4`.

The **medium** tier is the exception: `mid_tick_step` IS in display units (set
`0` to disable). `tick_label_divisor` is used directly (e.g. `1000` shows a 7000
tick as "7").

`arc` and `pathbar` are **step-based** (use `minor_tick_step` / `major_tick_step`
as the catalog shows) — no conversion needed. Only `meter` deviates.

## Other notes

- Config key is `signal_name`, not `signal` (the catalog's data binding).
- A meter is kept square by the firmware; set `w == h`.
- `arc` has an internal `auto_ticks` flag the editor manages — don't set it; just
  provide `minor_tick_step`/`major_tick_step`.

## Design philosophy: PROCEDURAL, not images — big images aren't for pro layouts

Professional layouts are built from **drawn widgets** — needles, ticks, arcs,
bars, text, shapes — **not baked raster images**. Images don't scale: they eat
flash + LittleFS, hit a *contiguous*-PSRAM decode wall (largest free block is
often <0.5 MB once a layout loads → the image silently fails to paint and the
widget shows its *name* as fallback), can't recolour/restyle live, and look
dated. Build the gauges; reach for an image only for tiny icons.

**Hard rules (default to ALL of these):**
- **NO full-screen (800×480) background image** (~1.15 MB). Use a full-screen
  **`shape_panel`** (a drawn rect, ~zero cost) + `line`/`text`/`arc`/`meter`.
- **NO meter/arc "dial face" background image, NO image-fill gauge, NO image
  needle, NO image ticks** in a normal layout. These were stepping stones — the
  drawn equivalents below now look as good and stay dynamic (recolour, night
  mode, live values).
- **Images ONLY for small icons** (≤~40 px): a logo, telltale glyphs (turn
  signals, high-beam, oil, battery, temp). Decoration, never a gauge surface.
- **All-widget dashes apply with a plain `save`+`set`** (no reboot, no PSRAM
  wall). Bind every readout to a real channel via a `panel` (`show_unit` pulls
  the channel's unit + decimals).

**Build premium gauges procedurally:**
- **Round gauge** = `meter` (needle) or `arc` (sweep fill). Both draw ticks +
  numbers themselves.
- **NEEDLE** = `meter` drawn needle with a **tip style** (Dagger / Spade /
  Diamond — see the meter's needle fields in `02-widget-catalog.md`) + a centre
  ball. Tapered tip + ball reads far better than a flat line, and it recolours
  live. (Never a baked image needle.)
- **TICKS** = `show_ticks` + per-tier length / width / colour
  (`{major,mid,minor}_tick_length` / `_width` / `_color`). For the raised /
  embossed **"pop"** that used to need a baked image, use the drawn **tick
  outline/glow** (arc + meter): `tick_outline_strength` (0-255) +
  `tick_outline_color` (usually black) + `tick_outline_fade` (0 = hard edge,
  higher = soft glow). It bakes into the static-tick snapshot → **free at
  runtime**.
- **FILL / glow** = `arc` value fill with `arc_color` → `grad_end_color`
  gradient + `lead_edge_enabled` glowing tip (or a `bar`). A drawn gradient fill
  with a bright lead-edge reads premium without a baked tube.
- A true soft *bloom* is the only thing widgets can't draw — and a pro layout
  doesn't need one. The lead-edge + tick outline give all the "pop" required.

## Golden rule: copy a working widget

The surest way to get a widget's exact, current device fields is to **adapt a
real one** rather than author from memory:

- `examples/ford_cluster.json` — a full, real 35-widget cluster (every widget
  type appears; copy the block you want and tweak).
- `GET /api/layout/current` on a live device — the exact JSON the firmware is
  rendering right now.

Then trim to defaults-only and run `validate_layout.py`.
