# _bundled — offline snapshot

`validate_layout.py` and `gen_catalog.py` read the RDM-7_Dash repo when
they can. When the skill is used **outside** a firmware checkout they
fall back to this directory, so validation still works on a handed-over
copy.

| file | what |
|---|---|
| `schema/widgets.schema.json` | verbatim copy of the widget schema |
| `device_fields.json` | per widget, the config keys the device's `from_json` reads (extracted from `main/widgets/*.c`) |
| `layout_manager.h` | stub carrying `LAYOUT_SCHEMA_VERSION` only |

**Do not hand-edit any of it.** Regenerate:

```
python scripts/bundle_standalone.py
```

## Provenance

| | |
|---|---|
| firmware commit | `v1.4.9-8-g89533af-dirty` |
| `LAYOUT_SCHEMA_VERSION` | 18 |
| widgets | 18 |
| device fields captured | 506 |

A snapshot older than the firmware it is checked against will validate
a layout as fine when it is not. If the numbers above disagree with the
header of `reference/02-widget-catalog.md`, re-run the bundler.
