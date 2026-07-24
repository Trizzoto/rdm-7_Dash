# Widget Settings Control-Type Audit — 2026-06-26

Source of truth: `schema/widgets.schema.json` (385 fields across 16 widgets).
Scope of this audit: the **control type** of every numeric setting (185 fields)
and, for bounded controls, their **min / max / step**. Non-numeric types
(`color`, `checkbox`, `select`, `font`, `image_picker`, `text`, `textarea`,
`can_id`) are correct by their nature and were spot-checked — no changes.

## The four numeric controls (what each actually renders)

| type | UI | Can type an exact value? | Best for |
|---|---|---|---|
| `number` | free `<input type=number>` | yes | unbounded / huge-range / precise values |
| `stepper` | `< [slider] [typebox] >` | yes (box auto-expands slider) | **bounded integers** — the workhorse |
| `stepper-auto` | stepper + AUTO/reset (0 = style default) | yes | needle taper etc. (special) |
| `slider` | drag bar + readout, **no typebox** | **no** | continuous-feel where exact value rarely matters |

On-device (`widget_fields.gen.c` → `inspector.c`): `stepper`/`stepper-auto`/`slider`
all render the same stepper/keypad row; `number` renders a numeric keypad. So every
change below is editor-metadata only — **no firmware logic changes**, just the schema
+ the two codegens + a desktop-copy sync note.

## Guiding rules applied

1. A `slider` whose range is so wide that a drag-pixel ≈ hundreds of units is a
   **fake slider** — it can't be used and you can't type into it. → make it `number`.
2. A bounded integer typed as plain `number` wastes its `min/max` (the `number`
   renderer ignores them) and gives no nudge buttons. → make it `stepper`.
3. Opacity / pixel-offset / angle want **exact entry + nudge**, and the codebase
   already overwhelmingly uses `stepper` for them. Stragglers using `slider` →
   `stepper` (consistency + typeability).
4. `smoothing_ms` and `anchor_position` are genuine continuous-feel controls →
   **keep `slider`**.

---

## CHANGE SET A — `stepper` → `number` (range too wide to be a real slider)

These render a slider spanning ±99999 (or 1–100000): undraggable. Free entry is correct.

| widget | field | range | reason |
|---|---|---|---|
| panel | warning_low_threshold | ±99999 | threshold in signal units |
| panel | warning_high_threshold | ±99999 | "" |
| bar | bar_low | ±99999 | "" |
| bar | bar_high | ±99999 | "" |
| arc | arc_low | ±99999 | "" |
| arc | arc_high | ±99999 | "" |
| meter | tick_label_divisor | 1–100000 | divisor, typically 1 or 1000 |
| arc | tick_label_divisor | 1–100000 | "" |
| pathbar | tick_label_divisor | 1–100000 | "" |

(min/max kept on the field — harmless, and `min:1` keeps the divisor ≥1 on the device keypad.)

## CHANGE SET B — `number` → `stepper` (bounded integer deserves the stepper UI)

Each already carries a real `min`/`max` that the `number` renderer silently ignores.

| widget | field | range |
|---|---|---|
| shift_light | led_count | 4–45 |
| meter | tick_image_scale | 10–400 |
| meter | tick_outline_strength | 0–255 |
| meter | tick_outline_fade | 0–20 |
| arc | major_tick_image_scale | 10–400 |
| arc | mid_tick_image_scale | 10–400 |
| arc | minor_tick_image_scale | 10–400 |
| arc | major_tick_image_opa | 0–255 |
| arc | mid_tick_image_opa | 0–255 |
| arc | minor_tick_image_opa | 0–255 |
| arc | major_tick_image_recolor_opa | 0–255 |
| arc | mid_tick_image_recolor_opa | 0–255 |
| arc | minor_tick_image_recolor_opa | 0–255 |
| arc | major_tick_image_offset | -60–60 |
| arc | mid_tick_image_offset | -60–60 |
| arc | minor_tick_image_offset | -60–60 |
| arc | tick_outline_strength | 0–255 |
| arc | tick_outline_fade | 0–20 |

## CHANGE SET C — `slider` → `stepper` (opacity / offset / angle want exact entry + match convention)

| widget | field | range | note |
|---|---|---|---|
| banner | bg_opa | 0–255 | every other `*_opa` is a stepper |
| pathbar | dim_opa | 0–255 | "" |
| arc | arc_image_opa | 0–255 | "" (same widget's tick-image opas are steppers after set B) |
| arc | arc_image_recolor_opa | 0–255 | "" |
| arc | arc_image_full_opa | 0–255 | "" |
| arc | arc_image_full_recolor_opa | 0–255 | "" |
| warning | label_y_offset | -100–100 | panel's label offsets are steppers |
| meter | sweep_degrees | 30–360 step5 | sits beside `start_angle_user` (stepper); exact 270/180 |
| arc | sweep_degrees | 30–360 step5 | "" |

## CHANGE SET D — min/max/step tweak

| widget | field | change | reason |
|---|---|---|---|
| pathbar | smoothing_ms | add `step: 10` | every other `smoothing_ms` slider steps by 10; pathbar stepped by 1 |

(pathbar `smoothing_ms` max stays 500 — deliberately higher for a slow tach bar.)

---

## Deliberately NOT changed (reviewed, correct as-is)

- `smoothing_ms` (rpm_bar/bar/meter/arc/pathbar) and `anchor_position`
  (bar/meter/arc) — textbook continuous-feel sliders; keep `slider`.
- Fractional thresholds `shift_light.threshold_mid` / `threshold_high` (0–1
  step 0.05) and `toggle.signal_on_threshold` — kept `number`; `stepper` uses
  `parseInt` and would destroy the fraction.
- Tick-length maxes (`mid_tick_length` 100 vs `major/minor` 50): a looser max is
  harmless freedom; not reducing it (could clamp existing layouts' slider view).
- `shift_light.border_radius` max 255, `needle_inner_radius`/`corner_radius` 0–400,
  pivots ±400 — large but still draggable; fine.
- All `color` / `checkbox` / `select` / `font` / `image_picker` / `text` /
  `textarea` / `can_id` fields — control type matches data type.

**Total: 37 field edits, all in `schema/widgets.schema.json`.** No `.c`/`.h`
behavior changes. After editing: run both codegens, then sync the desktop copy.

_Status: self-approved → applying._
