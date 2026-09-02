# Coordinates, colors & gradients

## Coordinates — center origin

`(0,0)` is the **screen centre** on every build. A widget's `x,y` is the
position of its **centre**, `+x` = right, `+y` = down. But the screen itself is
**not always 800 × 480** — size and shape (rect vs round) are a firmware
build-time choice (`main/system/screen_config.h`, selected via Kconfig):

| profile | `SCREEN_W`×`SCREEN_H` | shape | centre range |
|---|---|---|---|
| 7" (default) | 800×480 | rect | `x ∈ [-400,400]`, `y ∈ [-240,240]` |
| 4" | 720×720 | round | `x ∈ [-360,360]`, `y ∈ [-360,360]` |
| 4"/2.8" | 480×480 | rect or round | `x ∈ [-240,240]`, `y ∈ [-240,240]` |

**Find out which one you're targeting** — `GET /api/device/info` returns
`display:{width,height,shape}` for the connected dash; use `screen_w`/`screen_h`
already present in `GET /api/layout/current` when editing an existing layout.
Never hardcode 800×480.

```
       y = -screen_h/2  (top)
                │
 x = -w/2 ──────┼────── x = +w/2
  (left)        │        (right)
       y = +screen_h/2  (bottom)
```

- Visible centre range is `±screen_w/2` and `±screen_h/2` (see table above).
  Keep a widget's centre on-screen or it can't be selected in the editor.
- Pixel ↔ device: `pixel = device + (screen_w/2, screen_h/2)`. So a widget in
  the top-left quadrant has negative x and y.
- A widget spanning the full width (e.g. `rpm_bar`) is `x:0, w:screen_w` —
  `800` on the 7" panel, but `720` or `480` on a round build. A full top
  banner: `x:0, y:-screen_h/2+40, w:screen_w`.
- Quick anchors (7" panel numbers — scale to your `screen_w`/`screen_h`):
  top-centre `(0,-200)`, bottom-centre `(0,200)`, dead-centre `(0,0)`,
  right-half centre `(200,0)`.
- `w`/`h` have per-type size limits in the catalog's index table. They are the
  **editor's** resize limits, not a firmware clamp: the device draws what you
  give it, but the editor snaps the widget if the user drags it.
- **`shape: "round"` clips the corners** — the visible area is the circle
  inscribed in the `screen_w`×`screen_h` square. On a round build, keep
  content that matters (labels, gauge faces) inside that inscribed circle;
  the square's corners exist in coordinate space but are never lit.

## Angles — dials use a different convention in device JSON

`meter` and `arc` store `start_angle` / `end_angle` in **LVGL** convention
(0° = 3 o'clock). The catalog shows the *editor's* `start_angle_user` /
`sweep_degrees` (0° = 12 o'clock), which the device never reads. This is the
single most common way to get a dial pointing the wrong way — the conversion is
in `05-firmware-quirks.md`. Read it before placing a dial.

## Colors — RGB565 integers

Every `*_color` field is a **16-bit RGB565 integer**, written as a **decimal
number** in JSON. Pack:

```
rgb565 = ((R>>3) << 11) | ((G>>2) << 5) | (B>>3)      # R,G,B are 0..255
```

> **A `"#RRGGBB"` string is not a colour here.** The firmware tests
> `cJSON_IsNumber()` and skips anything else, so a hex string leaves the field
> at its default with no error anywhere. `validate_layout.py` flags this.

Use `scripts/rgb565.py` to convert: `python rgb565.py "#31C2F7"` → `13854`,
or `python rgb565.py 13854` → `#31C2F7`. It also prints the named palette.

### Common values (decimal RGB565)

| name | hex | RGB565 |
|---|---|---|
| white | #FFFFFF | 65535 |
| black | #000000 | 0 |
| red | #FF0000 | 63488 |
| green | #00FF00 | 2016 |
| blue | #0000FF | 31 |
| cyan | #00FFFF | 2047 |
| yellow | #FFFF00 | 65504 |
| orange | #FF8000 | 64512 |
| magenta | #FF00FF | 63519 |
| mid grey | #808080 | 33808 |

### The dash's house palette (from the Ford/Mustang work — cohesive set)

| role | hex | RGB565 |
|---|---|---|
| accent cyan (fills, highlights) | #31C2F7 | 13854 |
| dark track / unlit | #08384A* | 2277 |
| redline / alert red | #E14129 | 57861 |
| label grey (titles, units) | #8B95A4 | 36020 |
| dim grey (minor ticks, inactive) | #6A7583 | 27568 |
| white (big numbers, major ticks) | #FFFFFF | 65535 |

\* approximate hex — RGB565 rounds; the integer is what matters.

**Tip:** RGB565 loses low bits, so `rgb565(rgb888(x))` is lossy — round-trip a
colour once and reuse the integer. `scripts/rgb565.py` round-trips for you.

## Gradients — `grad_stops`

`bar`, `rpm_bar` and `arc` blend their fill through up to **8** colour stops:

```jsonc
"grad_stops": [
  { "pos": 0,   "color": 2016  },   // green at empty
  { "pos": 50,  "color": 65504 },   // yellow halfway
  { "pos": 100, "color": 63488 }    // red at full
]
```

- `pos` is **0–100 %** along the fill; `color` is RGB565, same as everywhere else.
- Needs **≥ 2** stops to do anything; fewer and the widget paints solid.
- Keep them **sorted by `pos`** (the loader sorts defensively, but the editor
  assumes sorted).
- Low/high **alert colours and the redline still override the gradient** — the
  gradient is the in-range look, not the warning look.
- Bars bake the gradient into an RGB565 image that grows with the fill; the arc
  lerps its indicator colour against the current value.

**Legacy form:** older layouts used `grad_enabled` + `grad_end_color` /
`bar_grad_end_color`. Those are *migrated on load* into a
`[{0, base}, {100, end}]` pair and never written back. Author `grad_stops` in
anything new.
