---
name: layout-builder
description: Build and apply an RDM-7 dashboard layout from a reference image or a text description — author the layout JSON, validate it, preview it live on the dash, screenshot to check, and save it. Use whenever the user wants to create, redesign, copy, or apply a gauge cluster / dashboard layout for the dash.
---

# RDM-7 Layout Builder

Turn a reference image or a description into a working RDM-7 dashboard layout,
apply it to the dash, and verify it looks right. A layout is one JSON object in
the **device format** (see `reference/01-layout-format.md`) — author it directly
and POST it to the dash.

## When to use

The user gives you a dashboard photo / mock-up ("make my dash look like this"),
or a description ("a tach across the top, coolant + oil gauges, a big speed
readout"), or asks to tweak/restyle/copy the current layout. Goal: produce and
apply the layout.

## References (read what you need — don't dump them all)

- `reference/01-layout-format.md` — the JSON shape, coordinates-in-brief, apply endpoints. **Read first.**
- `reference/02-widget-catalog.md` — every widget type + every config field (default, range, options, plain-English meaning). Your field dictionary.
- `reference/03-coordinates-and-colors.md` — center-origin coords + RGB565 colors + the house palette.
- `reference/04-channels-and-signals.md` — what `signal_name` can bind to.
- `reference/05-firmware-quirks.md` — **read before using a `meter`** (its ticks are count-based, not step). And the golden rule: copy real widgets.

## Workflow

1. **Understand the target.**
   - *Image:* identify each element → pick a widget type. Round dial w/ needle → `meter`; coloured arc/ring → `arc`; horizontal RPM strip across the top → `rpm_bar`; a curved/J-shaped tach → `pathbar` (shape J-Hook); level bar → `bar`; big number + label → `panel`; plain number/letter → `text`; warning tiles → `warning`; turn arrows → `indicator`; row of shift LEDs → `shift_light`; static graphic → `image`/`shape_panel`/`line`. Note positions, colours, what each reads.
   - *Description:* map the named gauges the same way.

2. **Get device context** (if a dash is reachable — default host `192.168.4.61`, AP `192.168.4.1`):
   - `python scripts/apply_layout.py --channels` → the bindable `signal_name`s.
   - `GET /api/layout/current` → the layout running now (great to adapt / copy widget blocks from, and to match existing conventions).

3. **Build the layout JSON.** Start from `examples/minimal_demo.json` or by copying widget blocks out of `examples/ford_cluster.json` / the device's current layout — that's faster and more correct than authoring from memory (the device accepts a superset of the catalog; see 05). Then:
   - Position with center-origin `x,y` (widget centre; `0,0` = middle). Draw order = array order (background first, text/needles last).
   - Bind data with `config.signal_name`. Colours are RGB565 ints (`scripts/rgb565.py "#RRGGBB"`).
   - **`config` is defaults-only** — include a field only when it differs from the catalog default. Keep the whole file under 32 KB.
   - For a `meter`, set ticks with `minor_tick_count` + `major_tick_every` (see 05), not the step fields.

4. **Validate** before applying:
   ```
   python scripts/validate_layout.py my_layout.json
   ```
   Fix every ERROR; review WARNs (unknown fields are usually typos or wrong-widget fields). Optionally pass `--channels-file` (a saved `/api/channels`) to check bindings.

5. **Preview live + screenshot, then iterate** (preview does NOT persist — ideal for the build loop):
   ```
   python scripts/apply_layout.py my_layout.json --shot /tmp/look.png
   ```
   Read the screenshot. Adjust positions/sizes/colours and re-preview until it matches. Inject test values if needed (`POST /api/signal/inject {"signal":"RPM","value":4000}`) so gauges show realistic readings in the shot.

6. **Save when it's right** (persist + show it):
   ```
   python scripts/apply_layout.py my_layout.json --save --activate --shot /tmp/final.png
   ```
   Confirm the final screenshot. Saving the active layout's name hot-reloads it.

## Rules & gotchas

- **Device format, not editor format.** `signal_name` (not `signal`); RGB565 int colours; center-origin coords. Author exactly what `/api/layout/current` returns.
- **Defaults-only config** and the **32 KB** cap — the validator enforces size.
- **Meter ticks are count-based** (`reference/05`). Arc/pathbar are step-based.
- **Draw order matters** — later widgets paint on top.
- **Prefer adapting real widgets** (examples / `/api/layout/current`) over authoring fields from scratch.
- **Verify visually** — always pull a screenshot after applying; don't claim it looks right without one.
- **Sweep-test value-driven gauges** (arc/bar/meter fill, fill-images). One screenshot lies — a gauge can look fine mid-range but read "full" at 0 or empty at max. Inject the bound signal across its range (e.g. `0, ¼, ½, ¾, max`), screenshot each, and check it reads correctly at **every** value. The empty (min) state is the usual offender: make the unfilled track *very* dim so 0 reads empty, not full. (`tools/_audi/sweep.sh SIGNAL "v1 v2 …"` stitches a contact sheet.)
- If a device isn't reachable, still build + validate the JSON and hand it over (the user can apply it from the editor's Import, or save it for later).

## Keeping the catalog current

If the widget schema changes, regenerate the catalog:
```
python scripts/gen_catalog.py
```
