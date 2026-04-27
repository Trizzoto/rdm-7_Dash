# Widget Schema — Single Source of Truth

This directory will hold the canonical description of every widget type the RDM-7 Dash supports — its display name, slot caps, pixel constraints, and editor-inspector field metadata. Four repos consume this metadata today, all with hand-maintained copies that drift:

| Repo | File | Lines (today) |
|---|---|---|
| **firmware** (canonical) | `main/web/index.html` `WIDGET_DEFS` block | 337 |
| desktop | `../rdm7-desktop/src/index.html` | 301 |
| wasm-editor | `../rdm7-wasm-editor/web/index.html` + `editor.html` | 256 / 258 |
| marketplace | (validates layouts; doesn't duplicate metadata) | — |

After the work tracked here lands, all four consumers derive their metadata (or validation rules) from one file: `schema/widgets.schema.json`. Drift is detected by CI on the firmware repo (the schema lives here) and surfaces as PR-blockers when the codegen output differs from what's checked in.

## File layout (target)

```
schema/
├── README.md                    (you are here)
├── widgets.schema.json          (the canonical data — see §Schema below)
├── widgets.schema.meta.json     (JSON Schema validator for widgets.schema.json)
└── examples/
    └── panel.json               (one example widget extract — handy when reviewing diffs)
tools/
├── codegen_widget_defs.py       (schema → WIDGET_DEFS JS literal)
├── validate_widget_schema.py    (validates schema against meta-validator)
└── check_widget_codegen.py      (runs codegen, fails if output differs from committed file)
```

## Schema

`widgets.schema.json`:

```json
{
  "schema_version": 1,
  "comment": "Source-of-truth for widget metadata across firmware, desktop, web editor, marketplace.",
  "widgets": [
    {
      "name": "panel",
      "display_name": "Panel",
      "singleton": false,
      "default_size": { "w": 155, "h": 92 },
      "default_position": null,
      "constraints": {
        "min_w": 80,
        "min_h": 40,
        "max_w": 250,
        "max_h": 130
      },
      "fields": [
        {
          "name": "label",
          "label": "Header Label",
          "type": "text",
          "default": "",
          "category": "data"
        },
        {
          "name": "decimals",
          "label": "Decimals",
          "type": "number",
          "default": 0,
          "category": "data"
        },
        {
          "name": "warning_high_color",
          "label": "Color",
          "type": "color",
          "default": "#FF0000",
          "category": "alerts",
          "group": "high",
          "enabled_by": "warning_high_enabled",
          "night_overridable": true
        }
      ]
    }
  ]
}
```

### Conventions

- **Numbers** for sizes / positions / decimal defaults.
- **Strings** for colour defaults (`"#RRGGBB"` form, **always** — codegen converts to `0xRRGGBB` JS hex literal for editors and to `lv_color_make()` C source for any C-side codegen).
- **Booleans** for booleans.
- **`null`** for "not present" optional fields (e.g., `default_position` for non-singleton widgets).
- **`screen_w`**, **`screen_h`**, **`screen_origin_x`** as quoted string tokens in `constraints` are special — they expand at codegen time:
  - JS codegen → `SCREEN_W`, `SCREEN_H`, `SCREEN_ORIGIN_X` (resolved at editor runtime from `applyScreenDimensions`).
  - C codegen → preprocessor macros from `system/screen_config.h`.

### Field types

Match what the editor's `WIDGET_DEFS` already supports, named with snake_case:

| Schema `type` | JS editor input | Note |
|---|---|---|
| `text` | text input | |
| `number` | number input | |
| `stepper` | number with min/max/step | requires `min`, `max`; `step` optional |
| `slider` | slider | requires `min`, `max` |
| `color` | colour picker | default is `"#RRGGBB"` |
| `checkbox` | checkbox | |
| `select` | dropdown | requires `options: [{value, label}, …]` |
| `font` | font picker | |
| `image_picker` | image picker | |

Optional field properties:

- `category`: `"data"` / `"appearance"` / `"alerts"` — affects grouping in the inspector.
- `group`: free-form string for sub-grouping inside a category (e.g., `"high"` / `"low"` for alert pairs).
- `enabled_by`: name of another field; this field is greyed out unless that other field is true.
- `inline`: free-form group key for visually inlining checkboxes (e.g., `"warning_high_apply"`).
- `night_overridable`: bool — exposes a per-instance night-mode override slot.
- `min`, `max`, `step`: numeric — required for `stepper` / `slider`, optional otherwise.
- `options`: required for `select` — array of `{value, label}` pairs.

### Doc comments

The current firmware `WIDGET_DEFS` has occasional `/* ... */` JS comments explaining non-obvious fields (e.g., the anchor-curve scale on `bar`). JSON doesn't allow comments. The codegen script emits these from a `_doc` field on the widget or field:

```json
{
  "name": "anchor_value",
  "label": "Anchor Value",
  "type": "number",
  "default": 50,
  "category": "data",
  "enabled_by": "anchor_enabled",
  "_doc": "Anchor-based non-linear scale. Pin THIS data value to THIS position on the bar."
}
```

The codegen output places `_doc` content as a `/* ... */` comment immediately preceding the field's JS object literal. `_doc` keys are stripped from generated JSON (e.g., for marketplace validation).

## Codegen

`tools/codegen_widget_defs.py`:

```bash
python tools/codegen_widget_defs.py main/web/index.html  # update firmware copy
python tools/codegen_widget_defs.py --check main/web/index.html  # CI mode: fail on drift
```

Each consumer's HTML file has a delimited block:

```js
// AUTO-GENERATED FROM schema/widgets.schema.json — DO NOT EDIT BY HAND.
// Run: python tools/codegen_widget_defs.py <this-file>
// AUTO-GENERATED BEGIN: WIDGET_DEFS
const WIDGET_DEFS = {
    /* …generated content… */
};
// AUTO-GENERATED END: WIDGET_DEFS
```

The codegen script replaces the contents between BEGIN and END markers; everything outside is untouched.

## Sync workflow

1. Developer edits `schema/widgets.schema.json`.
2. Developer runs `python tools/codegen_widget_defs.py main/web/index.html` (and `data/web/index.html`, the mirror).
3. Pre-commit hook / CI runs `validate_widget_schema.py` (schema is well-formed) and `check_widget_codegen.py main/web/index.html` (no drift between schema and the generated block).
4. Cross-repo sync: each consumer (desktop, wasm-editor) imports `schema/widgets.schema.json` via a vendored copy plus a CI step that runs the same codegen script and fails on drift. The schema file is the load-bearing artefact; consumers vendor it.

The marketplace doesn't need codegen — it validates layouts against a JSON Schema produced from `widgets.schema.json` (`tools/build_marketplace_validator.py` is a future addition; out of scope for the first pass).

## What is NOT in scope (yet)

- The C-side `widget_constraints[]` table in `main/widgets/widget_types.c`. That uses preprocessor macros that adapt to runtime `SCREEN_W` — codegen could produce it but the value-add is small (the table is short, rarely changes). **Future**: add a C-side codegen target that emits this table, with macro tokens preserved.
- The widget's `type_data` C struct definitions in `main/widgets/widget_*.h`. These don't follow the field-list pattern cleanly (they include LVGL pointers and runtime state mixed with serialised data). Out of scope for this round.
- Per-widget JSON serialisation (`to_json` / `from_json`). Continues to be hand-maintained. The schema describes the editor inspector, not the firmware persistence format.
- Layout-level fields (`signals`, `night_mode`, etc.). Same — schema is widget-scoped.

## Status

- Design committed (this README).
- Implementation work tracked in [docs/adr/0005-widget-schema-source-of-truth.md](../docs/adr/0005-widget-schema-source-of-truth.md) once Wave 1 lands.
