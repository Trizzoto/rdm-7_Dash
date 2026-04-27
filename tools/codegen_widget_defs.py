#!/usr/bin/env python3
"""
codegen_widget_defs.py — schema/widgets.schema.json -> WIDGET_DEFS JS literal.

Reads schema/widgets.schema.json and emits the WIDGET_DEFS JS object literal
between marker lines in a target HTML file:

    // AUTO-GENERATED BEGIN: WIDGET_DEFS
    const WIDGET_DEFS = { ... };
    // AUTO-GENERATED END: WIDGET_DEFS

Usage:
    python tools/codegen_widget_defs.py <file>           # write/update in place
    python tools/codegen_widget_defs.py --check <file>   # CI: nonzero if drift

Pure stdlib. Python 3.8+.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

BEGIN_MARKER = "// AUTO-GENERATED BEGIN: WIDGET_DEFS"
END_MARKER = "// AUTO-GENERATED END: WIDGET_DEFS"

# Tokens that expand to bare JS identifiers in the editor (resolved at
# runtime via applyScreenDimensions). Schema authors write them as strings.
SIZE_TOKEN_TO_IDENT = {
    "screen_w": "SCREEN_W",
    "screen_h": "SCREEN_H",
    "screen_origin_x": "SCREEN_ORIGIN_X",
    "screen_origin_y": "SCREEN_ORIGIN_Y",
}

# snake_case (schema) -> camelCase (JS) for editor field properties.
FIELD_KEY_RENAMES = {
    "default": "def",
    "category": "cat",
    "enabled_by": "enabledBy",
    "night_overridable": "nightOverridable",
    "night_key": "nightKey",
}

# Indentation: WIDGET_DEFS lives inside a function/IIFE in the HTML, so the
# top-level {} sits at 8 spaces; widget keys at 12 spaces; field entries at 16
# spaces. Matches existing main/web/index.html style.
INDENT_OUTER = " " * 8   # `const WIDGET_DEFS = {`
INDENT_WIDGET = " " * 12  # `"panel": { ... }`
INDENT_FIELDS_ARR = " " * 16  # `fields: [`
INDENT_FIELD = " " * 20  # each field entry
INDENT_FIELD_CONT = " " * 22  # continuation lines inside a multi-line field
INDENT_OPT_ITEM = " " * 26  # options array items


# ---------------------------------------------------------------------------
# Helpers — value -> JS literal
# ---------------------------------------------------------------------------

def _color_to_hex_literal(value: str) -> str:
    """'#RRGGBB' -> '0xRRGGBB'."""
    if not (isinstance(value, str) and len(value) == 7 and value.startswith("#")):
        raise ValueError(f"Color must be '#RRGGBB' string, got: {value!r}")
    return "0x" + value[1:].upper()


def _js_string(s: str) -> str:
    """Render a Python string as a JS double-quoted literal."""
    # JSON-encode then trim the surrounding quotes to get proper JS escaping
    # (handles \, ", control chars, unicode).
    return json.dumps(s, ensure_ascii=False)


def _js_value(v: Any) -> str:
    """Render a JSON-native value as a JS literal."""
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        # Match JSON: integers stay integers; floats keep their natural form.
        if isinstance(v, bool):  # already handled above
            return "true" if v else "false"
        return json.dumps(v)
    if isinstance(v, str):
        return _js_string(v)
    raise TypeError(f"Cannot render JS literal for type {type(v).__name__}: {v!r}")


def _size_value_to_js(v: Any) -> str:
    """Render a constraint or default-size value: int -> int, token -> ident."""
    if isinstance(v, str):
        if v not in SIZE_TOKEN_TO_IDENT:
            raise ValueError(f"Unknown screen-size token: {v!r}")
        return SIZE_TOKEN_TO_IDENT[v]
    return _js_value(v)


def _field_default_to_js(field: Dict[str, Any]) -> str:
    """Render a field's `default` honouring `type === 'color'`."""
    val = field.get("default")
    if field.get("type") == "color":
        return _color_to_hex_literal(val)
    return _js_value(val)


# ---------------------------------------------------------------------------
# Helpers — comment formatting
# ---------------------------------------------------------------------------

def _format_doc_comment(text: str, indent: str) -> List[str]:
    """Render a `_doc` field as a /* ... */ JS block comment.

    Single-line stays single-line; multiline gets the leading-asterisk style.
    """
    lines = text.split("\n")
    if len(lines) == 1:
        return [f"{indent}/* {lines[0]} */"]
    out = [f"{indent}/* {lines[0]}"]
    for line in lines[1:-1]:
        out.append(f"{indent} * {line}" if line else f"{indent} *")
    out.append(f"{indent} * {lines[-1]} */")
    return out


# ---------------------------------------------------------------------------
# Helpers — options array
# ---------------------------------------------------------------------------

def _options_to_js(options: List[Dict[str, Any]]) -> List[str]:
    """Render an `options` array as a multi-line JS literal block.

    Returns lines starting with '[' on the first line and ']' on the last.
    Caller wraps with appropriate `options: ` prefix and field-context indents.
    """
    items = []
    for opt in options:
        v = _js_value(opt["value"])
        l = _js_string(opt["label"])
        items.append(f"{{ v: {v}, l: {l} }}")
    return items


# ---------------------------------------------------------------------------
# Helpers — field rendering
# ---------------------------------------------------------------------------

# Canonical key order for emitted field literals. Keys not in this list go
# at the end in the order they appeared in the schema.
FIELD_KEY_ORDER = [
    "name", "label", "type",
    "def", "cat", "group",
    "min", "max", "step",
    "options",
    "enabledBy", "inline",
    "nightOverridable", "nightKey",
]


def _ordered_field_keys(d: Dict[str, Any]) -> List[str]:
    seen = set()
    out: List[str] = []
    for k in FIELD_KEY_ORDER:
        if k in d:
            out.append(k)
            seen.add(k)
    for k in d.keys():
        if k not in seen:
            out.append(k)
    return out


def _render_field(field: Dict[str, Any]) -> List[str]:
    """Render one field entry as a list of source lines (no trailing comma)."""
    # Build a flat dict of camelCase keys -> rendered JS strings, plus track
    # any deferred multi-line items (options, _raw_extra).
    rendered: Dict[str, str] = {}

    rendered["name"] = _js_string(field["name"])
    rendered["label"] = _js_string(field["label"])
    rendered["type"] = _js_string(field["type"])
    rendered["def"] = _field_default_to_js(field)
    if "category" in field:
        rendered["cat"] = _js_string(field["category"])
    if "group" in field:
        rendered["group"] = _js_string(field["group"])
    if "min" in field:
        rendered["min"] = _js_value(field["min"])
    if "max" in field:
        rendered["max"] = _js_value(field["max"])
    if "step" in field:
        rendered["step"] = _js_value(field["step"])
    if "enabled_by" in field:
        rendered["enabledBy"] = _js_string(field["enabled_by"])
    if "inline" in field:
        rendered["inline"] = _js_string(field["inline"])
    if field.get("night_overridable") is True:
        rendered["nightOverridable"] = "true"
    if "night_key" in field:
        rendered["nightKey"] = _js_string(field["night_key"])

    # Options: either inline simple expression (raw or short) or multi-line.
    options_lines: Optional[List[str]] = None
    if "_raw_options" in field:
        rendered["options"] = field["_raw_options"]
    elif "options" in field:
        opt_items = _options_to_js(field["options"])
        # Always render options multi-line for readability, regardless of count.
        options_lines = opt_items

    # _raw_extra: arbitrary extra JS keys (e.g. autoFn).
    raw_extra: Dict[str, str] = field.get("_raw_extra", {}) or {}

    keys = _ordered_field_keys(rendered)

    # If the field has multi-line options or any raw_extra value, render as
    # a multi-line block. Otherwise one line.
    has_multiline = options_lines is not None or bool(raw_extra)

    if not has_multiline:
        parts = [f"{k}: {rendered[k]}" for k in keys]
        return [INDENT_FIELD + "{ " + ", ".join(parts) + " }"]

    # Multi-line build.
    out: List[str] = []
    head_parts: List[str] = []
    options_idx = keys.index("options") if "options" in keys else -1

    # Strategy: emit on one line everything UP TO (but not including) options
    # if options is multi-line. After options, continue on a new continuation
    # line. raw_extra keys also start on their own line.
    # If no multi-line options but raw_extra exists: emit head on one line,
    # then raw_extra on continuation lines.

    if options_lines is not None and options_idx >= 0:
        head_keys = keys[:options_idx]
        tail_keys = keys[options_idx + 1:]
    else:
        head_keys = keys
        tail_keys = []

    head_parts = [f"{k}: {rendered[k]}" for k in head_keys]

    if options_lines is not None:
        # First line: { name: ..., label: ..., ..., type: ...,
        first_line = INDENT_FIELD + "{ " + ", ".join(head_parts) + (
            "," if (head_parts) else ""
        )
        out.append(first_line)
        # options block:
        out.append(f"{INDENT_FIELD_CONT}options: [")
        for i, item in enumerate(options_lines):
            comma = "," if i < len(options_lines) - 1 else ""
            out.append(f"{INDENT_OPT_ITEM}{item}{comma}")
        # Closing of options: with possible trailing keys:
        if tail_keys or raw_extra:
            out.append(f"{INDENT_FIELD_CONT}],")
            tail_parts = [f"{k}: {rendered[k]}" for k in tail_keys]
            for i, key in enumerate(raw_extra):
                # raw_extra rendered separately below
                pass
            if tail_parts and not raw_extra:
                out.append(INDENT_FIELD_CONT + ", ".join(tail_parts) + " }")
                return out
            if tail_parts:
                out.append(INDENT_FIELD_CONT + ", ".join(tail_parts) + ",")
            # raw_extra
            re_keys = list(raw_extra.keys())
            for i, k in enumerate(re_keys):
                last = (i == len(re_keys) - 1)
                out.extend(_render_raw_extra_value(k, raw_extra[k], last))
            return out
        else:
            out.append(f"{INDENT_FIELD_CONT}] }}")
            return out

    # No multi-line options. raw_extra exists -> head on first line, raw_extra
    # follows on continuation.
    first_line = INDENT_FIELD + "{ " + ", ".join(head_parts) + ","
    out.append(first_line)
    re_keys = list(raw_extra.keys())
    for i, k in enumerate(re_keys):
        last = (i == len(re_keys) - 1)
        out.extend(_render_raw_extra_value(k, raw_extra[k], last))
    return out


def _render_raw_extra_value(key: str, raw_js: str, is_last: bool) -> List[str]:
    """Render a `_raw_extra[key]: raw_js` block, terminated by ', ' or ' }'."""
    raw_lines = raw_js.split("\n")
    if len(raw_lines) == 1:
        suffix = " }" if is_last else ","
        return [f"{INDENT_FIELD_CONT}{key}: {raw_lines[0]}{suffix}"]
    out = [f"{INDENT_FIELD_CONT}{key}: {raw_lines[0]}"]
    for line in raw_lines[1:-1]:
        out.append(line)  # raw lines preserve their own indentation
    suffix = " }" if is_last else ","
    out.append(f"{raw_lines[-1]}{suffix}")
    return out


# ---------------------------------------------------------------------------
# Widget rendering
# ---------------------------------------------------------------------------

def _render_widget(widget: Dict[str, Any], is_last: bool) -> List[str]:
    """Render one widget entry. Returns source lines."""
    out: List[str] = []

    if "_doc" in widget:
        out.extend(_format_doc_comment(widget["_doc"], INDENT_WIDGET))

    out.append(f'{INDENT_WIDGET}{_js_string(widget["name"])}: {{')

    # Header line: displayName, defW, defH, [defY,] [singleton: true,]
    header_parts = [
        f'displayName: {_js_string(widget["display_name"])}',
        f'defW: {_js_value(widget["default_size"]["w"])}',
        f'defH: {_js_value(widget["default_size"]["h"])}',
    ]
    pos = widget.get("default_position")
    if pos:
        if "x" in pos:
            header_parts.append(f'defX: {_js_value(pos["x"])}')
        if "y" in pos:
            header_parts.append(f'defY: {_js_value(pos["y"])}')
    if widget.get("singleton") is True:
        header_parts.append("singleton: true")
    out.append(f"{INDENT_FIELDS_ARR}{', '.join(header_parts)},")

    # fields:
    fields = widget.get("fields", [])
    out.append(f"{INDENT_FIELDS_ARR}fields: [")
    for i, f in enumerate(fields):
        is_last_field = (i == len(fields) - 1)
        # Pre-field doc comment
        if "_doc" in f:
            out.extend(_format_doc_comment(f["_doc"], INDENT_FIELD))
        field_lines = _render_field(f)
        # Append comma to last line of field if not last
        if not is_last_field:
            field_lines[-1] = field_lines[-1] + ","
        out.extend(field_lines)
    out.append(f"{INDENT_FIELDS_ARR}]")

    out.append(f"{INDENT_WIDGET}}}{'' if is_last else ','}")
    return out


# ---------------------------------------------------------------------------
# Top-level codegen
# ---------------------------------------------------------------------------

def render_widget_defs_block(schema: Dict[str, Any]) -> str:
    """Produce the JS literal block (between BEGIN/END markers)."""
    lines: List[str] = []
    lines.append(BEGIN_MARKER)
    lines.append(f"{INDENT_OUTER}const WIDGET_DEFS = {{")
    widgets = schema.get("widgets", [])
    for i, w in enumerate(widgets):
        lines.extend(_render_widget(w, is_last=(i == len(widgets) - 1)))
    lines.append(f"{INDENT_OUTER}}};")
    lines.append(END_MARKER)
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# In-place file editing
# ---------------------------------------------------------------------------

def _detect_eol(text: str) -> str:
    """Detect the dominant line ending in `text`."""
    if "\r\n" in text:
        return "\r\n"
    return "\n"


def _splice_block(file_text: str, new_block: str) -> str:
    """Replace the AUTO-GENERATED block in `file_text` with `new_block`.

    `new_block` includes both BEGIN and END markers. The replacement preserves
    the indentation of the BEGIN marker line (so the markers stay aligned in
    the source file).
    """
    eol = _detect_eol(file_text)
    lines = file_text.split(eol)

    begin_idx = None
    end_idx = None
    for i, line in enumerate(lines):
        if line.lstrip().startswith(BEGIN_MARKER):
            begin_idx = i
            break
    if begin_idx is None:
        raise RuntimeError(
            f"BEGIN marker not found: {BEGIN_MARKER!r}.\n"
            f"Add it (and the END marker) around the WIDGET_DEFS block."
        )

    for j in range(begin_idx + 1, len(lines)):
        if lines[j].lstrip().startswith(END_MARKER):
            end_idx = j
            break
    if end_idx is None:
        raise RuntimeError(
            f"END marker not found after BEGIN: {END_MARKER!r}."
        )

    # Indentation is taken from the BEGIN line so generated block aligns.
    indent = lines[begin_idx][: len(lines[begin_idx]) - len(lines[begin_idx].lstrip())]

    new_lines = new_block.split("\n")
    indented_new = [(indent + nl) if nl else "" for nl in new_lines]

    out = lines[:begin_idx] + indented_new + lines[end_idx + 1:]
    return eol.join(out)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def _load_schema(schema_path: Path) -> Dict[str, Any]:
    with schema_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate WIDGET_DEFS JS literal from schema/widgets.schema.json."
    )
    parser.add_argument(
        "files", nargs="+",
        help="Target HTML files (must contain AUTO-GENERATED BEGIN/END markers)."
    )
    parser.add_argument(
        "--check", action="store_true",
        help="CI mode: nonzero exit if file content would change."
    )
    parser.add_argument(
        "--schema", default=None,
        help="Path to widgets.schema.json (default: schema/widgets.schema.json relative to repo root)."
    )
    args = parser.parse_args(argv)

    repo_root = Path(__file__).resolve().parent.parent
    schema_path = Path(args.schema) if args.schema else repo_root / "schema" / "widgets.schema.json"
    if not schema_path.exists():
        print(f"error: schema not found: {schema_path}", file=sys.stderr)
        return 2

    schema = _load_schema(schema_path)
    new_block = render_widget_defs_block(schema)

    any_drift = False
    any_error = False

    for file_str in args.files:
        target = Path(file_str)
        if not target.exists():
            print(f"error: file not found: {target}", file=sys.stderr)
            any_error = True
            continue
        try:
            text = target.read_text(encoding="utf-8")
        except UnicodeDecodeError as e:
            print(f"error: cannot read {target} as UTF-8: {e}", file=sys.stderr)
            any_error = True
            continue
        try:
            new_text = _splice_block(text, new_block)
        except RuntimeError as e:
            print(f"error: {target}: {e}", file=sys.stderr)
            any_error = True
            continue

        if new_text == text:
            if not args.check:
                print(f"unchanged: {target}")
            continue

        if args.check:
            print(f"drift: {target}", file=sys.stderr)
            any_drift = True
        else:
            target.write_text(new_text, encoding="utf-8")
            print(f"updated: {target}")

    if any_error:
        return 2
    if args.check and any_drift:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
