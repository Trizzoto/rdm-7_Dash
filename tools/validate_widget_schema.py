#!/usr/bin/env python3
"""
validate_widget_schema.py — schema/widgets.schema.json sanity check.

Loads the canonical schema and validates it against the meta-validator
(schema/widgets.schema.meta.json). Exits 0 on success, 1 on validation
failure, 2 on file/IO error.

Usage:
    python tools/validate_widget_schema.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    try:
        import jsonschema  # type: ignore
    except ImportError:
        print(
            "error: 'jsonschema' is not installed.\n"
            "       pip install -r tools/requirements.txt",
            file=sys.stderr,
        )
        return 2

    repo_root = Path(__file__).resolve().parent.parent
    schema_path = repo_root / "schema" / "widgets.schema.json"
    meta_path = repo_root / "schema" / "widgets.schema.meta.json"

    if not schema_path.exists():
        print(f"error: schema not found: {schema_path}", file=sys.stderr)
        return 2
    if not meta_path.exists():
        print(f"error: meta-validator not found: {meta_path}", file=sys.stderr)
        return 2

    try:
        with schema_path.open("r", encoding="utf-8") as f:
            schema = json.load(f)
        with meta_path.open("r", encoding="utf-8") as f:
            meta = json.load(f)
    except json.JSONDecodeError as e:
        print(f"error: invalid JSON: {e}", file=sys.stderr)
        return 2

    try:
        jsonschema.validate(instance=schema, schema=meta)
    except jsonschema.ValidationError as e:
        print(f"validation FAILED:", file=sys.stderr)
        print(f"  path: {'/'.join(str(p) for p in e.absolute_path)}", file=sys.stderr)
        print(f"  message: {e.message}", file=sys.stderr)
        return 1
    except jsonschema.SchemaError as e:
        print(f"meta-schema is itself invalid: {e}", file=sys.stderr)
        return 2

    n_widgets = len(schema.get("widgets", []))
    n_fields = sum(len(w.get("fields", [])) for w in schema.get("widgets", []))
    print(f"validate_widget_schema: OK ({n_widgets} widgets, {n_fields} fields)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
