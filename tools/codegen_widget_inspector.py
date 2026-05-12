#!/usr/bin/env python3
"""
codegen_widget_inspector.py - schema/widgets.schema.json -> widget_fields.gen.c

Emits a compile-time C array describing every widget's user-editable
fields. Used by main/ui/menu/inspector.c to render STYLE / DATA / RULES
tabs without hand-coding per widget type.

Pure stdlib. Python 3.8+.

Usage:
    python tools/codegen_widget_inspector.py           # write out
    python tools/codegen_widget_inspector.py --check   # CI drift check
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_PATH = ROOT / "schema" / "widgets.schema.json"
OUT_PATH    = ROOT / "main" / "widgets" / "widget_fields.gen.c"

# Keep in lockstep with widget_fields.h.
SCHEMA_TYPE_TO_WF = {
    "text":          "WF_TYPE_TEXT",
    "textarea":      "WF_TYPE_TEXTAREA",
    "number":        "WF_TYPE_NUMBER",
    "stepper":       "WF_TYPE_STEPPER",
    "stepper-auto":  "WF_TYPE_STEPPER_AUTO",
    "slider":        "WF_TYPE_SLIDER",
    "checkbox":      "WF_TYPE_CHECKBOX",
    "select":        "WF_TYPE_SELECT",
    "color":         "WF_TYPE_COLOR",
    "font":          "WF_TYPE_FONT",
    "image_picker":  "WF_TYPE_IMAGE_PICKER",
    "can_id":        "WF_TYPE_CAN_ID",
}

SCHEMA_CAT_TO_WF = {
    "data":        "WF_CAT_DATA",
    "appearance":  "WF_CAT_APPEARANCE",
    "alerts":      "WF_CAT_ALERTS",
    "thresholds":  "WF_CAT_THRESHOLDS",
}

# Schema widget names -> widget_type_t enum identifiers.
WIDGET_NAME_TO_ENUM = {
    "panel":       "WIDGET_PANEL",
    "rpm_bar":     "WIDGET_RPM_BAR",
    "bar":         "WIDGET_BAR",
    "indicator":   "WIDGET_INDICATOR",
    "warning":     "WIDGET_WARNING",
    "text":        "WIDGET_TEXT",
    "meter":       "WIDGET_METER",
    "image":       "WIDGET_IMAGE",
    "shape_panel": "WIDGET_SHAPE_PANEL",
    "arc":         "WIDGET_ARC",
    "toggle":      "WIDGET_TOGGLE",
    "button":      "WIDGET_BUTTON",
    "shift_light": "WIDGET_SHIFT_LIGHT",
    "line":        "WIDGET_LINE",
}


def c_str(s: Optional[str]) -> str:
    """Quote a Python string as a C string literal, or 'NULL' for None/empty."""
    if s is None or s == "":
        return "NULL"
    out = '"'
    for ch in s:
        if ch == '\\':
            out += '\\\\'
        elif ch == '"':
            out += '\\"'
        elif ch == '\n':
            out += '\\n'
        elif ch == '\r':
            out += '\\r'
        elif ch == '\t':
            out += '\\t'
        elif ord(ch) < 32 or ord(ch) > 126:
            out += f'\\x{ord(ch):02x}'
        else:
            out += ch
    out += '"'
    return out


def color_hex(s: Optional[str]) -> str:
    """'#RRGGBB' -> '0xRRGGBB'. Returns '0x000000' if unparseable."""
    if not s or not isinstance(s, str) or not s.startswith('#') or len(s) < 7:
        return '0x000000'
    return '0x' + s[1:7].upper()


def as_int(v, fallback: int = 0) -> int:
    try:
        if isinstance(v, bool):
            return 1 if v else 0
        return int(v)
    except (TypeError, ValueError):
        return fallback


def field_defaults(field, fld_type) -> Tuple[str, str, str, str]:
    """Returns (default_int, default_float, default_color, default_str) as C literals."""
    d = field.get('default')
    if fld_type in ('text', 'textarea', 'font', 'image_picker'):
        return '0', '0.0f', '0x000000', c_str(d if isinstance(d, str) else '')
    if fld_type == 'color':
        return '0', '0.0f', color_hex(d if isinstance(d, str) else None), 'NULL'
    if fld_type == 'checkbox':
        return ('1' if bool(d) else '0'), '0.0f', '0x000000', 'NULL'
    if fld_type in ('number', 'stepper', 'stepper-auto', 'slider', 'select', 'can_id'):
        if isinstance(d, (int, float)) and not isinstance(d, bool):
            return str(as_int(d, 0)), f'{float(d):.6f}f', '0x000000', 'NULL'
        return '0', '0.0f', '0x000000', 'NULL'
    return '0', '0.0f', '0x000000', 'NULL'


def emit_options(widget_name: str, field) -> Tuple[Optional[str], Optional[str], int]:
    """Returns (declaration, array_name, count) for select fields with options.
    None/None/0 if no options."""
    opts = field.get('options', [])
    if not opts:
        return None, None, 0
    arr_name = f"{widget_name}_{field['name']}_opts"
    lines = [f"static const widget_field_option_t {arr_name}[] = {{"]
    for o in opts:
        val = as_int(o.get('value', 0))
        lbl = o.get('label', '')
        lines.append(f"    {{ {val}, {c_str(lbl)} }},")
    lines.append("};")
    return '\n'.join(lines), arr_name, len(opts)


def emit_widget(widget) -> str:
    name = widget['name']
    blocks = []

    # Options arrays first - one per select field that has options.
    options_map = {}
    options_decls = []
    for f in widget['fields']:
        if f.get('type') == 'select' and f.get('options'):
            decl, arr_name, count = emit_options(name, f)
            if decl:
                options_decls.append(decl)
                options_map[f['name']] = (arr_name, count)
    if options_decls:
        blocks.append('\n\n'.join(options_decls))

    # Field array.
    field_lines = [f"static const widget_field_t {name}_fields[] = {{"]
    for f in widget['fields']:
        fname = f['name']
        flbl  = f.get('label', fname)
        ftype = f.get('type', 'text')
        fcat  = f.get('category', 'appearance')
        fmin  = as_int(f.get('min'), 0)
        fmax  = as_int(f.get('max'), 0)
        fstep = as_int(f.get('step'), 0)
        dint, dflt, dcol, dstr = field_defaults(f, ftype)

        wf_type = SCHEMA_TYPE_TO_WF.get(ftype, 'WF_TYPE_TEXT')
        wf_cat  = SCHEMA_CAT_TO_WF.get(fcat,  'WF_CAT_APPEARANCE')

        opts_ref = 'NULL'
        opts_cnt = 0
        if fname in options_map:
            arr_name, opts_cnt = options_map[fname]
            opts_ref = arr_name

        enabled_by = c_str(f.get('enabled_by'))
        group      = c_str(f.get('group'))
        inline_key = c_str(f.get('inline'))
        night = 'true' if f.get('night_overridable') else 'false'

        field_lines.append("    {")
        field_lines.append(f"        .name = {c_str(fname)}, .label = {c_str(flbl)},")
        field_lines.append(f"        .type = {wf_type}, .category = {wf_cat},")
        field_lines.append(f"        .min_int = {fmin}, .max_int = {fmax}, .step_int = {fstep},")
        field_lines.append(f"        .default_int = {dint}, .default_float = {dflt}, .default_color = {dcol},")
        field_lines.append(f"        .default_str = {dstr},")
        field_lines.append(f"        .options = {opts_ref}, .option_count = {opts_cnt},")
        field_lines.append(f"        .enabled_by = {enabled_by}, .group = {group}, .inline_key = {inline_key},")
        field_lines.append(f"        .night_overridable = {night},")
        field_lines.append("    },")
    field_lines.append("};")
    blocks.append('\n'.join(field_lines))

    return '\n\n'.join(blocks)


def generate(schema) -> str:
    lines = []
    lines.append("/*")
    lines.append(" * widget_fields.gen.c - AUTO-GENERATED. Do not edit.")
    lines.append(" *")
    lines.append(" * Regenerate via:")
    lines.append(" *     python tools/codegen_widget_inspector.py")
    lines.append(" *")
    lines.append(" * Source of truth: schema/widgets.schema.json")
    lines.append(" */")
    lines.append('#include "widgets/widget_fields.h"')
    lines.append("")

    for w in schema['widgets']:
        if w['name'] not in WIDGET_NAME_TO_ENUM:
            print(f"WARNING: schema widget '{w['name']}' has no enum mapping; skipping",
                  file=sys.stderr)
            continue
        lines.append(emit_widget(w))
        lines.append("")

    lines.append("const widget_fields_def_t WIDGET_FIELDS[] = {")
    for w in schema['widgets']:
        if w['name'] not in WIDGET_NAME_TO_ENUM:
            continue
        wname = w['name']
        wenum = WIDGET_NAME_TO_ENUM[wname]
        lines.append(f"    {{")
        lines.append(f"        .type        = {wenum},")
        lines.append(f"        .type_name   = {c_str(wname)},")
        lines.append(f"        .fields      = {wname}_fields,")
        lines.append(f"        .field_count = sizeof({wname}_fields) / sizeof({wname}_fields[0]),")
        lines.append(f"    }},")
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t WIDGET_FIELDS_COUNT =")
    lines.append("    sizeof(WIDGET_FIELDS) / sizeof(WIDGET_FIELDS[0]);")
    lines.append("")

    return '\n'.join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else None)
    ap.add_argument('--check', action='store_true',
                    help='exit non-zero if generated content drifts from disk')
    args = ap.parse_args(argv)

    schema = json.loads(SCHEMA_PATH.read_text(encoding='utf-8'))
    generated = generate(schema)

    if args.check:
        existing = OUT_PATH.read_text(encoding='utf-8') if OUT_PATH.exists() else ''
        if existing != generated:
            print(f"DRIFT: {OUT_PATH} is out of sync with schema.", file=sys.stderr)
            print("       Run: python tools/codegen_widget_inspector.py", file=sys.stderr)
            return 1
        print(f"OK: {OUT_PATH} matches schema.")
        return 0

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(generated, encoding='utf-8')
    print(f"Wrote {OUT_PATH} ({len(generated)} bytes)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
