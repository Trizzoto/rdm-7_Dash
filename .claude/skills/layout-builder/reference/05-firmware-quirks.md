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

## Performance & assets — prefer widgets, avoid big images

Learned the hard way (16 MB flash, ~8.8 MB LittleFS, ~8 MB PSRAM):

- **NEVER use a full-screen (800×480) background image.** At RGB565+alpha that's
  ~1.15 MB on flash AND it must decode into a *contiguous* PSRAM block to render —
  the board's largest free block is often <0.5 MB once a layout is loaded, so a big
  image silently fails to paint (the widget shows its *name* as fallback text) and
  it balloons load time. For a dark backdrop use a full-screen **`shape_panel`**
  (a drawn rect, zero image cost) + `line`/`text`/`arc` widgets.

- **All-widget dashes are the default.** Text/panel/arc/bar/line/shape_panel/meter
  cost ~nothing in flash, never hit the PSRAM decode wall, and apply with a plain
  `save`+`set` (no reboot). Bind every readout to a real channel (`signal_name`)
  via a `panel` — its `show_unit` pulls the channel's unit + decimals.

- **When you want image quality, keep images gauge-sized (≤~400 px), never
  full-screen**, and use these patterns:
  - **Image-fill gauge** = `arc` with `arc_image` (empty/track) + `arc_image_full`
    (full); the widget reveals the full image via a **horizontal left-to-right
    clip** (`reverse:true` = right-to-left). Great for **linear/bar** gauges
    (fuel, boost, a horizontal rev strip). NOT radial — a *circular* arc won't
    fill around the ring this way.
  - **Static image dial** = `arc`/`meter` with only `arc_image` / `bg_image_name`.
  - **Image needle** = `meter` with `needle_image_name` + `needle_pivot_x/y` (a
    baked tapered/glossy needle; a flat drawn line reads amateur).
  - Each baked image needs ~`w*h*3` bytes of *contiguous* PSRAM to decode; after a
    layout swap PSRAM is fragmented, so big images often only decode on a clean
    boot. Two ~350 px images ≈ 0.7 MB is fine; one 800×480 ≈ 1.15 MB won't.

- **Glow/bloom can only be baked** (widgets draw flat fills). So a circular glowing
  tach is a trade-off: baked glow = static (or linear-fill); widget `arc` = dynamic
  radial fill but flat. Pick per design.

## Golden rule: copy a working widget

The surest way to get a widget's exact, current device fields is to **adapt a
real one** rather than author from memory:

- `examples/ford_cluster.json` — a full, real 35-widget cluster (every widget
  type appears; copy the block you want and tweak).
- `GET /api/layout/current` on a live device — the exact JSON the firmware is
  rendering right now.

Then trim to defaults-only and run `validate_layout.py`.
