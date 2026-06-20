#!/usr/bin/env python3
"""Generate the widget catalog (reference/02-widget-catalog.md) from the single
source of truth, schema/widgets.schema.json. Re-run whenever the schema changes
so the skill never drifts from the firmware:

    python .claude/skills/layout-builder/scripts/gen_catalog.py

Emits one section per widget: its default size/position + a table of every
config field (name, type, default, range/options, gated-by, and the plain
"what it does" help text). Colours are RGB565 integers (see 03-colors).
"""
import json, io, os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
SCHEMA = os.path.join(ROOT, "schema", "widgets.schema.json")
OUT = os.path.join(os.path.dirname(__file__), "..", "reference", "02-widget-catalog.md")


def opts_str(f):
    o = f.get("options")
    if not o:
        return ""
    parts = []
    for it in o:
        if isinstance(it, dict):
            parts.append(f"{it.get('value')}={it.get('label')}")
        else:
            parts.append(str(it))
    return "options: " + ", ".join(parts)


def range_str(f):
    if "min" in f or "max" in f:
        s = f"{f.get('min','')}..{f.get('max','')}"
        if "step" in f:
            s += f" step {f['step']}"
        return s
    return ""


def constraint(f):
    bits = []
    r = range_str(f)
    if r:
        bits.append(r)
    o = opts_str(f)
    if o:
        bits.append(o)
    if f.get("enabled_by"):
        bits.append(f"only when `{f['enabled_by']}`")
    return "; ".join(bits)


def md_escape(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def layout_schema_version():
    """The LAYOUT schema_version every layout JSON must carry (≠ the schema-doc
    version). Read from layout_manager.h so it stays current."""
    try:
        h = io.open(os.path.join(ROOT, "main", "layout", "layout_manager.h"),
                    encoding="utf-8").read()
        import re
        m = re.search(r"#define\s+LAYOUT_SCHEMA_VERSION\s+(\d+)", h)
        return m.group(1) if m else "?"
    except Exception:
        return "?"


def main():
    S = json.load(io.open(SCHEMA, encoding="utf-8"))
    widgets = S.get("widgets") or S.get("widget_types")
    out = []
    out.append("# Widget Catalog — every type, every setting\n")
    out.append("> AUTO-GENERATED from `schema/widgets.schema.json` by "
               "`scripts/gen_catalog.py`. Do not hand-edit. Every layout JSON "
               f"must carry `\"schema_version\": {layout_schema_version()}`.\n")
    out.append("Each widget is `{ \"type\", \"id\", \"x\", \"y\", \"w\", \"h\", "
               "\"config\": { ...fields below... } }`. Put a field in `config` "
               "only when it differs from the default (keeps the layout under the "
               "32 KB budget). `*_color` fields are **RGB565 integers** "
               "(see 03-colors). Bind data with `config.signal_name` "
               "(see 04-channels).\n")
    out.append(f"**{len(widgets)} widget types.**\n")

    # quick index
    out.append("| Type | Name | Default size | Singleton |")
    out.append("|---|---|---|---|")
    for w in widgets:
        ds = w.get("default_size", {})
        size = f"{ds.get('w','?')}x{ds.get('h','?')}" if isinstance(ds, dict) else str(ds)
        out.append(f"| `{w['name']}` | {w.get('display_name','')} | {size} | "
                   f"{'yes' if w.get('singleton') else ''} |")
    out.append("")

    for w in widgets:
        out.append(f"\n## `{w['name']}` — {w.get('display_name','')}\n")
        ds = w.get("default_size", {})
        dp = w.get("default_position", {})
        meta = []
        if isinstance(ds, dict):
            meta.append(f"default size {ds.get('w','?')}×{ds.get('h','?')}")
        if isinstance(dp, dict) and dp:
            meta.append(f"default pos ({dp.get('x',0)},{dp.get('y',0)})")
        if w.get("singleton"):
            meta.append("**singleton** (only one allowed)")
        if meta:
            out.append("- " + " · ".join(meta) + "\n")

        # group fields in first-appearance order
        groups, by = [], {}
        for f in w["fields"]:
            g = f.get("group", "Other")
            if g not in by:
                by[g] = []
                groups.append(g)
            by[g].append(f)
        for g in groups:
            out.append(f"**{g}**\n")
            out.append("| field | type | default | constraints | what it does |")
            out.append("|---|---|---|---|---|")
            for f in by[g]:
                out.append("| `{n}` | {t} | {d} | {c} | {h} |".format(
                    n=f["name"], t=f["type"],
                    d=md_escape(f.get("default", "")),
                    c=md_escape(constraint(f)),
                    h=md_escape(f.get("help", ""))))
            out.append("")

    txt = "\n".join(out) + "\n"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(txt)
    nfields = sum(len(w["fields"]) for w in widgets)
    print(f"wrote {os.path.relpath(OUT, ROOT)} — {len(widgets)} widgets, {nfields} fields")


if __name__ == "__main__":
    main()
