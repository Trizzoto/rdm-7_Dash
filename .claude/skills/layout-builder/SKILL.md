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

## Workflow — build it LIVE on the dash, one piece at a time

Do **not** generate the whole layout in a script and load it in one shot. Grow it
incrementally on the real screen, hand-editing the JSON and screenshotting after
each step, so you catch overlap / position / colour problems per-widget instead
of debugging a 10-widget blob. The dash is your canvas; `/api/screenshot` is your
eyes.

> **⚠️ The live layout on the dash is the source of truth — never clobber the
> user's edits.** The user may open the editor and modify the layout *between*
> your turns. If you re-apply a stale local JSON copy, you **delete their work**.
> So: **before every edit/resume, re-pull `GET /api/layout/current`** and make your
> change on top of *that* (don't trust a file you wrote earlier in the session).
> Make **targeted** edits to the current state, not whole-layout overwrites. When
> in doubt about whether they've touched it, pull current and diff before applying.

1. **Understand the target.**
   - *Image:* identify each element → pick a widget type. Round dial w/ needle → `meter`; coloured arc/ring → `arc`; horizontal RPM strip across the top → `rpm_bar`; a curved/J-shaped tach → `pathbar` (shape J-Hook); level bar → `bar`; big number + label → `panel`; plain number/letter → `text`; warning tiles → `warning`; turn arrows → `indicator`; row of shift LEDs → `shift_light`; static graphic → `image`/`shape_panel`/`line`. Note positions, colours, what each reads.
   - *Description:* map the named gauges the same way.

2. **Get device context** (default host `192.168.4.61`, AP `192.168.4.1`):
   - `GET /api/channels` → the bindable `signal_name`s (units, decimals, warns).
   - `GET /api/layout/current` → the layout running now — the best source of real,
     current device-format widget blocks to copy and tweak.

3. **Start a skeleton by hand** (`Write` the JSON file). Just the background
   `shape_panel` + the single hero gauge — nothing else yet. Author in **device
   format** by copying a real widget block from `/api/layout/current` or
   `examples/` and editing it; don't hand-author fields from memory.

4. **The live loop — repeat per widget:**
   - **Preview** (live, not saved): `POST /api/layout/preview` with the whole
     current layout. **Inject** realistic values so gauges read true:
     `POST /api/signal/inject {"signal":"RPM","value":4000}`.
   - **Screenshot + read it:** `GET /api/screenshot` → look at it.
   - **Adjust** position/size/colour by hand-`Edit`ing the JSON, re-preview, re-shot
     until that piece is right.
   - **Add the next widget** and loop. Order: background → hero gauge → secondary
     gauges → readouts → decoration (draw order = array order, later paints on top).
   - Edit the JSON with `Write`/`Edit`. Use the helper scripts ONLY for one-off
     math — `scripts/rgb565.py "#RRGGBB"` for a colour — **never to generate the
     file**. Position with center-origin `x,y`; `config` is **defaults-only**; for a
     `meter`, ticks are `minor_tick_count` + `major_tick_every` (see 05).

5. **Validate** before saving:
   ```
   python scripts/validate_layout.py my_layout.json --channels-file channels.json
   ```
   Fix every ERROR; review WARNs (usually typos or wrong-widget fields).

6. **Sweep-test** the value-driven gauges (see the rule below) — inject the bound
   signal across its whole range and screenshot each; the min/empty state is the
   usual liar.

7. **Save when it's right:** `POST /api/layout/save` (needs `name`) then
   `POST /api/layout/set {"name":"…"}` to activate. Confirm with a final
   screenshot. (`scripts/apply_layout.py my_layout.json --save --activate --shot
   out.png` wraps validate→save→activate→shot if you want one call for the final
   persist — but the *building* above stays hand-edited + incremental.)

## Rules & gotchas

- **PROCEDURAL, not images — this is the house style.** Build gauges from drawn
  widgets (meter/arc needles + ticks, bars, text, shapes), NOT baked raster
  images. NO full-screen background image, NO meter/arc dial-face image, NO
  image-fill gauge, NO image needle/ticks. Images are only for tiny icons
  (logo, telltale glyphs ≤~40 px). For premium ticks use `show_ticks` + per-tier
  length/width/colour + the drawn **tick outline/glow** (`tick_outline_strength`
  / `_color` / `_fade`); for a premium needle use a `meter` tip style + ball; for
  a glowing fill use `arc` gradient (`arc_color`→`grad_end_color`) + a
  `lead_edge`. All free, all dynamic. See `reference/05` "Design philosophy".
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
