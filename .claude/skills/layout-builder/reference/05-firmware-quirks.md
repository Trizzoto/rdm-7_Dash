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

## Golden rule: copy a working widget

The surest way to get a widget's exact, current device fields is to **adapt a
real one** rather than author from memory:

- `examples/ford_cluster.json` — a full, real 35-widget cluster (every widget
  type appears; copy the block you want and tweak).
- `GET /api/layout/current` on a live device — the exact JSON the firmware is
  rendering right now.

Then trim to defaults-only and run `validate_layout.py`.
