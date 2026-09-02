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

- `reference/01-layout-format.md` — the JSON shape, top-level keys, apply endpoints, what `save` refuses. **Read first.**
- `reference/02-widget-catalog.md` — every widget type + every config field (default, range, options, plain-English meaning). Your field dictionary. **Generated — read the flags legend at the top.**
- `reference/03-coordinates-and-colors.md` — center-origin coords, RGB565 colours, the house palette, `grad_stops`.
- `reference/04-channels-and-signals.md` — what `signal_name` can bind to, and why you must read it off the device.
- `reference/05-firmware-quirks.md` — **read before using a `meter` or an `arc`.** Dial angles and meter ticks both differ from the catalog. Plus the procedural-not-images house style.
- `reference/06-rules-and-night.md` — conditional restyling (`config.rules`) and after-dark overrides (`config.night`).
- `reference/07-assets-and-tracks.md` — images, fonts, and `.rdmtrk` circuits for `track_map`.

## Three ways the device JSON differs from the catalog — know these before you start

1. **`meter`/`arc` angles are LVGL angles.** The catalog shows
   `start_angle_user`/`sweep_degrees`; the device reads `start_angle`/`end_angle`
   with `lvgl = user + 270`. Writing the catalog's names does nothing.
2. **`meter` ticks are count-based** (`minor_tick_count` + `major_tick_every`),
   not step-based. `arc` and `pathbar` *are* step-based.
3. **Colours are RGB565 integers, never hex strings.** A `"#RRGGBB"` string is
   silently ignored and the field keeps its default.

The catalog flags every editor-only field with **X** and each widget's section
lists them up front. `validate_layout.py` catches all three. `05-firmware-quirks.md`
has the conversions.

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
> Make **targeted** edits to the current state, not whole-layout overwrites.
> `GET /api/layout/version` is a cheap `{"v":N}` counter — note it when you pull,
> pass it back as `apply_layout.py --expect-version N`, and the save is refused
> rather than silently overwriting a change you didn't see.

1. **Understand the target.**
   - *Image:* identify each element → pick a widget type. Round dial w/ needle → `meter`; coloured arc/ring → `arc`; horizontal RPM strip across the top → `rpm_bar`; a curved/J-shaped tach → `pathbar` (shape J-Hook); level bar → `bar`; big number + label → `panel`; plain number/letter → `text`; warning tiles → `warning`; turn arrows → `indicator`; row of shift LEDs → `shift_light`; circuit outline → `track_map`; static graphic → `image`/`shape_panel`/`line`. Note positions, colours, what each reads.
   - *Description:* map the named gauges the same way.

2. **Find the dash and get context.** Its IP is DHCP and moves — don't reuse a
   remembered one:
   ```
   python scripts/apply_layout.py --scan 192.168.4 --channels --channels-out channels.json
   ```
   - `GET /api/channels` → the bindable `signal_name`s. **Bind to `signal`, not
     the channel `id`** — see 04, they are not the same string.
   - `GET /api/layout/current` → the layout running now — the best source of
     real, current device-format widget blocks to copy and tweak. Note the
     `/api/layout/version` alongside it.

3. **Start a skeleton by hand** (`Write` the JSON file). Just the background
   `shape_panel` + the single hero gauge — nothing else yet. Author in **device
   format** by copying a real widget block from `/api/layout/current` or
   `examples/` and editing it; don't hand-author fields from memory.

4. **The live loop — repeat per widget:**
   - **Preview** (live, not saved): `POST /api/layout/preview` with the whole
     current layout. **Inject** realistic values so gauges read true:
     `POST /api/signal/inject {"values":[{"signal":"RPM","value":4000}, …]}`
     (up to 16 at once). **Check the `unknown` array in the response** — that's
     how a typo'd signal name fails loudly instead of mysteriously.
   - **Screenshot + read it:** `GET /api/screenshot?full=1` → look at it. Give
     the rebuild time to land — `apply_layout.py` polls `/api/screenshot/hash`
     until the framebuffer stops changing rather than sleeping blindly.
   - **Adjust** position/size/colour by hand-`Edit`ing the JSON, re-preview, re-shot
     until that piece is right.
   - **Add the next widget** and loop. Order: background → hero gauge → secondary
     gauges → readouts → decoration (draw order = array order, later paints on top).
   - Edit the JSON with `Write`/`Edit`. Use the helper scripts ONLY for one-off
     math — `scripts/rgb565.py "#RRGGBB"` for a colour — **never to generate the
     file**.

5. **Validate** before saving:
   ```
   python scripts/validate_layout.py my_layout.json --channels-file channels.json
   ```
   Fix every ERROR; review WARNs (usually typos, editor-only fields, or a
   dropped-by-slot-cap widget).

6. **Sweep-test** the value-driven gauges (see the rule below) — inject the bound
   signal across its whole range and screenshot each; the min/empty state is the
   usual liar. If the layout has `night` overrides or a `night_mode` trigger,
   check both day and night (inject the trigger signal — see 06).

7. **Save when it's right:** `POST /api/layout/save` (needs `name`) then
   `POST /api/layout/set {"name":"…"}` to activate. Confirm with a final
   screenshot.
   ```
   python scripts/apply_layout.py my_layout.json --save --activate \
          --expect-version 42 --shot out.png
   ```

## Rules & gotchas

- **PROCEDURAL, not images — this is the house style.** Build gauges from drawn
  widgets (meter/arc needles + ticks, bars, text, shapes), NOT baked raster
  images. NO full-screen background image, NO meter/arc dial-face image, NO
  image-fill gauge, NO image needle/ticks. Images are only for tiny icons
  (logo, telltale glyphs ≤~40 px). For premium ticks use `show_ticks` + per-tier
  length/width/colour + the drawn **tick outline/glow** (`tick_outline_strength`
  / `_color` / `_fade`); for a premium needle use a `meter` tip style + ball +
  the drawn **needle shadow** (`shadow_enabled` / `_opa` / `_width_extra` /
  `_dynamic`); for a glowing fill use `grad_stops` + `lead_edge_enabled`.
  All free, all dynamic. See `reference/05` "Design philosophy".
- **Device format, not editor format.** `signal_name` (not `signal`); RGB565 int
  colours; center-origin coords; LVGL dial angles. Author exactly what
  `/api/layout/current` returns.
- **Defaults-only config** and the **32 KB** cap — the validator enforces size.
- **Draw order matters** — later widgets paint on top.
- **Slot caps are hard**: `indicator` 0–1, `warning` 0–7. Over-cap widgets are
  **dropped by the firmware**, not clamped. `rpm_bar` is a singleton.
- **Prefer adapting real widgets** (`/api/layout/current`, then `examples/`) over
  authoring fields from scratch.
- **Verify visually** — always pull a screenshot after applying; don't claim it
  looks right without one.
- **Sweep-test value-driven gauges** (arc/bar/meter fill). One screenshot lies —
  a gauge can look fine mid-range but read "full" at 0 or empty at max. Inject
  the bound signal across its range (e.g. `0, ¼, ½, ¾, max`), screenshot each,
  and check it reads correctly at **every** value. The empty (min) state is the
  usual offender: make the unfilled track *very* dim so 0 reads empty, not full.
- **Thresholds live on the channel, not the layout.** If a gauge goes red in the
  wrong place, fix the channel — see 04.
- If a device isn't reachable, still build + validate the JSON and hand it over
  (the user can apply it from the editor's Import, or save it for later). Say
  plainly that it was not verified on hardware.

## Examples

- `examples/ford_cluster.json` — a real 35-widget cluster (`pathbar`, `meter`,
  `arc`, `bar`, `text`, `line`, small `image` icons).
- `examples/feature_demo.json` — smaller; exercises `grad_stops`, `rules`,
  `night` + a `night_mode` trigger, needle shadow, tick outline, and converted
  dial angles.
- `examples/minimal_demo.json` — four widgets, the smallest thing that works.

## Keeping the skill current

The catalog and the offline snapshot are generated from the firmware. After any
schema, `LAYOUT_SCHEMA_VERSION`, or widget `from_json` change:

```
python scripts/gen_catalog.py          # regenerate reference/02-widget-catalog.md
python scripts/bundle_standalone.py    # refresh _bundled/ for offline use
```

`_bundled/` is what lets `validate_layout.py` and `gen_catalog.py` work when
this skill is copied somewhere without an RDM-7_Dash checkout. `_bundled/README.md`
records which firmware commit it was cut from — **a stale snapshot validates
against old firmware without saying so.**
