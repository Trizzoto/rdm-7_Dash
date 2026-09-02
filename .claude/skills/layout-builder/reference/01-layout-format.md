# Layout JSON format

A layout is one JSON object. This is the **device format** — exactly what
`GET /api/layout/current` returns and `POST /api/layout/save` stores. Author it
directly; there is no separate "editor format" to convert.

```jsonc
{
  "schema_version": 18,        // REQUIRED — the catalog header (02) states the current one
  "name": "my_layout",         // REQUIRED on save — becomes the file name
  "screen_w": 800,             // match the TARGET dash — see below, don't assume 800x480
  "screen_h": 480,
  "signals": [],               // OPTIONAL — display extras only; [] is fine
  "widgets": [ /* … */ ]       // the widgets, drawn in array order (later = on top)
}
```

Never restate the schema version from memory — `02-widget-catalog.md`'s header
line is generated from `LAYOUT_SCHEMA_VERSION` in the firmware, and
`validate_layout.py` checks yours against it.

**`screen_w`/`screen_h` are not always 800×480.** Screen size *and shape*
(rect vs round) are picked at firmware build time via Kconfig
(`main/system/screen_config.h`) — 800×480 rect is the default 7" panel, but
480×480 and 720×720 round/square builds exist too. Query the real target with
`GET /api/device/info` → `display:{width,height,shape}` before authoring
coordinates; don't hand-guess. See 03-coordinates for the per-size centre
ranges and what "round" means for placement.

## Widget object

```jsonc
{
  "type": "meter",     // one of the types in 02-widget-catalog.md
  "id": "coolant",     // unique short string; used for ordering/selection
  "x": -140,           // CENTER-ORIGIN device coords — see 03-coordinates
  "y": -62,
  "w": 300,            // width / height in px — see the size-limit note below
  "h": 300,
  "config": { /* type-specific fields — ONLY non-default ones */ }
}
```

Rules:
- **`config` is defaults-only.** Emit a field only when it differs from the
  catalog default. The whole layout must serialise under **32 KB**
  (`LAYOUT_MAX_FILE_BYTES` in `web_server_layout.c`) or the save is rejected
  with `413 {"error":"layout_too_large"}`. Fewer fields = smaller, faster, clearer.
- **`w`/`h` have advisory size limits.** Every type declares
  `min_w/max_w/min_h/max_h` (in the catalog's index table). These are the
  **editor's** resize limits, not a firmware clamp — the device renders whatever
  you give it, and real layouts do go outside them for small text. But the
  editor snaps a widget to them the moment the user drags it, so treat a big
  excursion as a design smell rather than a plan.
- **Bind data with `config.signal_name`** (NOT `signal`) → a channel signal name
  from 04-channels (e.g. `"RPM"`, `"COOLANT_TEMP"`). A widget with no
  `signal_name` just shows its static/default value.
- **Colours are RGB565 integers** (e.g. `"lit_color": 13854`) — a *number*, never
  a `"#RRGGBB"` string. A string is parsed as "not a number" and silently
  ignored, leaving the default. See 03-colors.
- **Draw order = array order.** Background/decoration first, gauges, then
  text/needles last so they sit on top.
- Each `id` should be unique. Reusing the firmware's own ids for special widgets
  (e.g. `"tach"`, `"speedo"`) is fine and conventional.
- `config.rules` (conditional restyling) and `config.night` (after-dark
  overrides) are per-widget too — see `06-rules-and-night.md`.

## Optional top-level keys

Beyond the five above, `/api/layout/current` may return — and `save` preserves —
these. **Carry them through verbatim** when you edit an existing layout; don't
hand-author them from scratch.

| key | what it is |
|---|---|
| `night_mode` | `{ "signal_name": "…", "active_when": 1 }` — the CAN trigger that flips the dash to night. See 06. |
| `ecu`, `ecu_version` | which ECU preset this layout was built against (also mirrored into NVS on load). |
| `polled_pids` | OBD2 PIDs to poll, encoded `service<<8 \| pid`. |
| `custom_pids` | user-defined OBD2 PID definitions. |
| `allow_empty` | *request-only* flag for `save` — see the empty-layout guard below. |

## Slot-assigned types

A few types carry a `config.slot` that decides *which* lamp/tile they are, and
the firmware hard-drops out-of-range slots:

| type | slots | note |
|---|---|---|
| `indicator` | 0–1 | left/right turn signals; `slot >= 2` is dropped |
| `warning` | 0–7 | warning tiles; `slot >= 8` is dropped |
| `panel`, `bar` | any | slot only drives *auto*-positioning; unlimited |
| `rpm_bar` | — | **singleton** — one per layout |

`x`/`y` are still applied to every widget afterwards, slot-assigned ones
included: the slot picks the object, `x`/`y` place it. Don't assume a slotted
widget ignores its coordinates.

## Signals block (optional)

`signals[]` only carries **per-signal display extras** — CAN decode lives in the
device's channel registry (ADR-0005/0006), NOT here. That separation is what
makes a layout portable between dashes. Include an entry only to attach:

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

| call | does |
|---|---|
| `POST /api/layout/preview` | render the posted layout **live, not saved**. A reboot/reload restores the real one. Use this while building. |
| `POST /api/layout/save` | persist it (needs `name`). Saving the **active** layout's name hot-reloads it on screen. Add `?apply=0` to write the file without touching the screen. |
| `POST /api/layout/set` `{"name":"…"}` | activate a different saved layout. |
| `GET /api/layout/current` | the layout rendering **right now**, in device format. |
| `GET /api/layout/version` | `{"v":N}` — cheap change counter. Poll this instead of refetching the layout. |
| `GET /api/layout/list` | saved layout names (`?details=1` for more). |
| `GET /api/layout/raw?name=…` | a saved layout's file, without activating it. |
| `POST /api/layout/rename` `{"old_name","new_name"}` | rename. |
| `POST /api/layout/delete` | delete. |
| `POST /api/layout/reset_default` | regenerate the factory `default` layout (and re-apply the ECU preset). Recovery hatch when a layout is wrecked. |

`scripts/apply_layout.py` wraps the common ones (validate → preview or save →
screenshot to verify).

### What `save` will refuse

- **Invalid JSON** → `400`, nothing is written (deliberate boot-loop guard).
- **Missing/invalid `name` or `widgets`** → `400`.
- **A `name` that isn't filename-safe** (path traversal) → `400`.
- **Over 32 KB** → `413 {"error":"layout_too_large","max":32768,"actual":N}`.
- **An empty `widgets: []` over a layout that currently has widgets** → `409`.
  This guard exists because a client that had never populated its widget list
  used to be able to wipe a 26-widget dash silently. If you *mean* it, send
  `"allow_empty": true`. **If you get a 409, that is the guard doing its job —
  re-pull `/api/layout/current` and find out why your copy went empty. Do not
  reflexively add `allow_empty`.**

A missing/zero `schema_version` is filled in with the current one rather than
rejected — but set it correctly anyway so the validator can check your work.
