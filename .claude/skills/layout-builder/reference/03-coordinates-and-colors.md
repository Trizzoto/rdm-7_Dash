# Coordinates & colors

## Coordinates — center origin

The screen is **800 × 480**. `(0,0)` is the **screen centre**. A widget's `x,y`
is the position of its **centre**, `+x` = right, `+y` = down.

```
            y = -240  (top)
                │
 x = -400 ──────┼────── x = +400
  (left)        │        (right)
            y = +240  (bottom)
```

- Visible centre range: `x ∈ [-400, 400]`, `y ∈ [-240, 240]`. Keep a widget's
  centre on-screen or it can't be selected in the editor.
- Pixel ↔ device: `pixel = device + (400, 240)`. So a widget centred at the
  top-left-ish quadrant has negative x and y.
- A widget spanning the full width (e.g. `rpm_bar`) is `x:0, w:800`. A full
  top banner: `x:0, y:-200, w:800`.
- Quick anchors: top-centre `(0,-200)`, bottom-centre `(0,200)`, dead-centre
  `(0,0)`, right-half centre `(200,0)`.

## Colors — RGB565 integers

Every `*_color` field is a **16-bit RGB565 integer** (decimal in JSON). Pack:

```
rgb565 = ((R>>3) << 11) | ((G>>2) << 5) | (B>>3)      # R,G,B are 0..255
```

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
