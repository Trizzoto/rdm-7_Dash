# Layout JSON format

A layout is one JSON object. This is the **device format** — exactly what
`GET /api/layout/current` returns and `POST /api/layout/save` stores. Author it
directly; there is no separate "editor format" to convert.

```jsonc
{
  "schema_version": 15,        // REQUIRED — must match LAYOUT_SCHEMA_VERSION (see catalog header)
  "name": "my_layout",         // REQUIRED on save — [A-Za-z0-9 _-], becomes the file name
  "screen_w": 800,             // always 800
  "screen_h": 480,             // always 480
  "signals": [],               // OPTIONAL — see "Signals block" below; [] is fine
  "widgets": [ /* … */ ]       // the widgets, drawn in array order (later = on top)
}
```

## Widget object

```jsonc
{
  "type": "meter",     // one of the 16 types in 02-widget-catalog.md
  "id": "coolant",     // unique short string; used for ordering/selection
  "x": -140,           // CENTER-ORIGIN device coords — see 03-coordinates
  "y": -62,
  "w": 300,            // width / height in px
  "h": 300,
  "config": { /* type-specific fields — ONLY non-default ones */ }
}
```

Rules:
- **`config` is defaults-only.** Emit a field only when it differs from the
  catalog default. The whole layout must serialise under **32 KB**
  (`RDM_LAYOUT_MAX_BYTES`) or the save is rejected / silently reverts. Fewer
  fields = smaller, faster, clearer.
- **Bind data with `config.signal_name`** (NOT `signal`) → a channel signal name
  from 04-channels (e.g. `"RPM"`, `"COOLANT_TEMP"`). A widget with no
  `signal_name` just shows its static/default value.
- **Colours are RGB565 integers** (e.g. `"lit_color": 13854`). See 03-colors.
- **Draw order = array order.** Background/decoration first, gauges, then
  text/needles last so they sit on top.
- Each `id` should be unique. Reusing the firmware's own ids for special widgets
  (e.g. `"tach"`, `"speedo"`) is fine and conventional.

## Slot-limited types

A few types are positioned by an internal `slot` instead of free x/y, and are
capped (the firmware drops extras):

| type | cap | note |
|---|---|---|
| `indicator` | 2 | left/right turn signals — `config.slot` 0/1 |
| `warning` | 8 | warning tiles — `config.slot` 0..7 |
| `rpm_bar` | 1 | singleton, spans the top |

Other types are free-positioned by x/y/w/h.

## Signals block (optional)

`signals[]` only carries **per-signal display extras** — CAN decode lives in the
device's channel registry (ADR-0005), NOT here. Include an entry only to attach:

```jsonc
"signals": [
  { "name": "GEAR", "value_map": [ {"v":0,"label":"N"}, {"v":1,"label":"1"} ] },
  { "name": "FUEL_SENDER_V",
    "fuel_cal": { "empty_v": 0.5, "full_v": 3.0, "full_value": 100, "enabled": true } }
]
```

If you don't need value-maps or fuel calibration, use `"signals": []`. Widgets
still bind fine via `signal_name` to channels that already exist on the device.

## Applying a layout

- **Preview (live, NOT saved):** `POST /api/layout/preview` with the whole
  layout object → renders immediately on the dash. Perfect for iterating; a
  reboot/reload restores the real layout. Use this while building.
- **Save (persist):** `POST /api/layout/save` with the layout object (needs
  `name`). Saving the **active** layout's name hot-reloads it on screen.
- **Activate a different one:** `POST /api/layout/set` `{"name":"..."}`.
- `scripts/apply_layout.py` wraps all of this (discover device → validate →
  preview or save → screenshot to verify).
