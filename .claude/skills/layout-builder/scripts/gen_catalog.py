#!/usr/bin/env python3
"""Generate the widget catalog (reference/02-widget-catalog.md) from the single
source of truth, schema/widgets.schema.json, cross-checked against the firmware
widget sources. Re-run whenever the schema or a widget's from_json changes so
the skill never drifts:

    python .claude/skills/layout-builder/scripts/gen_catalog.py

Emits one section per widget: its default size, its size limits (what the
EDITOR will resize it within — the firmware does not clamp), and a table of every config field — name, type,
default, range/options, gating, night/rule capability, and the plain-English
"what it does". Colours are RGB565 integers (see 03-colors).

It also diffs the schema against each `main/widgets/widget_<type>.c` and lists
the fields the DEVICE's from_json never reads — i.e. editor-only fields that do
nothing if you put them in device JSON. That list is generated, not hand-kept,
so a new editor-only field cannot silently become a trap (see 05-quirks).
"""
import json, io, os, re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
BUNDLED = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "_bundled"))
SCHEMA = os.path.join(ROOT, "schema", "widgets.schema.json")
if not os.path.exists(SCHEMA):
    SCHEMA = os.path.join(BUNDLED, "schema", "widgets.schema.json")
WIDGETS_DIR = os.path.join(ROOT, "main", "widgets")
OUT = os.path.join(os.path.dirname(__file__), "..", "reference", "02-widget-catalog.md")

# Same regex the validator uses: every cJSON_GetObjectItem*(obj, "name") in the
# widget's C source. That set IS the device's real accepted-field list.
_FIELD_RE = re.compile(r'GetObjectItem(?:CaseSensitive)?\s*\(\s*[A-Za-z_]\w*\s*,\s*"([a-z_]\w*)"')

# Types that are UI affordances in the editor, never a config field.
_NON_FIELD_TYPES = {"label", "info", "button", "upload"}


DEVICE_FIELDS_JSON = os.path.join(BUNDLED, "device_fields.json")
_BUNDLED_FIELDS = None


def device_fields(wtype):
    """Field names widget_<wtype>.c's from_json actually reads. Falls back to the
    _bundled snapshot outside a firmware checkout, so a handed-over copy of the
    skill still regenerates the X flags. None when neither is available."""
    global _BUNDLED_FIELDS
    p = os.path.join(WIDGETS_DIR, "widget_%s.c" % wtype)
    if os.path.exists(p):
        names = set(_FIELD_RE.findall(io.open(p, encoding="utf-8", errors="ignore").read()))
        names.discard("config")
        return names
    if _BUNDLED_FIELDS is None:
        try:
            _BUNDLED_FIELDS = json.load(io.open(DEVICE_FIELDS_JSON, encoding="utf-8"))
        except Exception:
            _BUNDLED_FIELDS = {}
    v = _BUNDLED_FIELDS.get(wtype)
    return set(v) if v is not None else None


def opts_str(f):
    o = f.get("options")
    if not o:
        return ""
    parts = []
    for it in o:
        if isinstance(it, dict):
            parts.append("%s=%s" % (it.get("value"), it.get("label")))
        else:
            parts.append(str(it))
    return "options: " + ", ".join(parts)


def range_str(f):
    if "min" in f or "max" in f:
        s = "%s..%s" % (f.get("min", ""), f.get("max", ""))
        if "step" in f:
            s += " step %s" % f["step"]
        return s
    return ""


def constraint(f):
    bits = []
    for s in (range_str(f), opts_str(f)):
        if s:
            bits.append(s)
    if f.get("enabled_by"):
        bits.append("only when `%s`" % f["enabled_by"])
    return "; ".join(bits)


def flags(f, dev):
    """Compact capability column. N = night-overridable, R = rule-only,
    X = in the schema but NOT read by the device (editor-only)."""
    out = []
    if f.get("night_overridable"):
        key = f.get("night_key")
        out.append("**N**" + ("(`%s`)" % key if key else ""))
    if f.get("rule_only"):
        out.append("**R**")
    elif dev is not None and f["name"] not in dev and f["type"] not in _NON_FIELD_TYPES:
        # R already says "not a config key"; X would add nothing but confusion.
        out.append("**X**")
    return " ".join(out)


def md_escape(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def layout_schema_version():
    """The LAYOUT schema_version every layout JSON must carry (not the
    schema-doc version). Read from layout_manager.h so it stays current."""
    for p in (os.path.join(ROOT, "main", "layout", "layout_manager.h"),
              os.path.join(BUNDLED, "layout_manager.h")):
        try:
            m = re.search(r"#define\s+LAYOUT_SCHEMA_VERSION\s+(\d+)",
                          io.open(p, encoding="utf-8").read())
            if m:
                return m.group(1)
        except Exception:
            continue
    return "?"


def con_str(c):
    """Render a widget's size constraints, resolving the screen_w/h tokens."""
    if not isinstance(c, dict) or not c:
        return ""

    def v(k):
        x = c.get(k)
        return {"screen_w": "800", "screen_h": "480"}.get(x, x)

    w = "w %s..%s" % (v("min_w"), v("max_w")) if "min_w" in c or "max_w" in c else ""
    h = "h %s..%s" % (v("min_h"), v("max_h")) if "min_h" in c or "max_h" in c else ""
    return ", ".join(x for x in (w, h) if x)


def main():
    S = json.load(io.open(SCHEMA, encoding="utf-8"))
    widgets = S.get("widgets") or S.get("widget_types")
    ver = layout_schema_version()
    out = []
    out.append("# Widget Catalog — every type, every setting\n")
    out.append("> AUTO-GENERATED from `schema/widgets.schema.json` (cross-checked "
               "against `main/widgets/*.c`) by `scripts/gen_catalog.py`. Do not "
               "hand-edit. Every layout JSON must carry "
               '`"schema_version": %s`.\n' % ver)
    out.append('Each widget is `{ "type", "id", "x", "y", "w", "h", '
               '"config": { ...fields below... } }`. Put a field in `config` '
               "only when it differs from the default (keeps the layout under the "
               "32 KB budget). `*_color` fields are **RGB565 integers** "
               "(see 03-colors). Bind data with `config.signal_name` "
               "(see 04-channels).\n")
    out.append("**Flags column**\n")
    out.append("- **N** — night-overridable: put it in `config.night` to change it "
               "after dark (see 06-rules-and-night). A `` `key` `` after the N is "
               "the name to use inside `night` when it differs from the field name.")
    out.append("- **R** — *rule-only*: NOT a `config` field. It exists only as a "
               "rule override target (see 06-rules-and-night).")
    out.append("- **X** — **editor-only: the device's `from_json` never reads it.** "
               "Setting it in device JSON does nothing. The widget's section says "
               "so up front; 05-quirks has what to write instead.\n")
    out.append("**%d widget types.**\n" % len(widgets))

    # quick index
    out.append("| Type | Name | Default size | Editor resize limits | Singleton |")
    out.append("|---|---|---|---|---|")
    for w in widgets:
        ds = w.get("default_size", {})
        size = ("%sx%s" % (ds.get("w", "?"), ds.get("h", "?"))
                if isinstance(ds, dict) else str(ds))
        out.append("| `%s` | %s | %s | %s | %s |" % (
            w["name"], w.get("display_name", ""), size,
            con_str(w.get("constraints")) or "—",
            "yes" if w.get("singleton") else ""))
    out.append("")

    editor_only = {}
    for w in widgets:
        dev = device_fields(w["name"])
        # A rule-only field is also absent from from_json, but for a different
        # reason: the device DOES honour it, just as a rule override rather than
        # a config key. Reporting the two together reads as "ignored" and is wrong.
        absent = [f for f in w["fields"]
                  if dev is not None and f["name"] not in dev
                  and f["type"] not in _NON_FIELD_TYPES]
        eo = [f["name"] for f in absent if not f.get("rule_only")]
        ro = [f["name"] for f in absent if f.get("rule_only")]
        if eo:
            editor_only[w["name"]] = eo

        out.append("\n## `%s` — %s\n" % (w["name"], w.get("display_name", "")))
        ds = w.get("default_size", {})
        dp = w.get("default_position", {})
        meta = []
        if isinstance(ds, dict):
            meta.append("default size %s×%s" % (ds.get("w", "?"), ds.get("h", "?")))
        if isinstance(dp, dict) and dp:
            meta.append("default pos (%s,%s)" % (dp.get("x", 0), dp.get("y", 0)))
        cs = con_str(w.get("constraints"))
        if cs:
            meta.append("editor resize limits: %s" % cs)
        if w.get("singleton"):
            meta.append("**singleton** (only one allowed)")
        if meta:
            out.append("- " + " · ".join(meta) + "\n")
        if eo:
            out.append("> ⚠ **Editor-only here (flagged X):** "
                       + ", ".join("`%s`" % n for n in eo)
                       + " — not a `config` key; the device ignores these. "
                         "See 05-quirks.\n")
        if ro:
            out.append("> **Rule-only here (flagged R):** "
                       + ", ".join("`%s`" % n for n in ro)
                       + " — not a `config` key either, but the device DOES "
                         "honour it as a rule override target. "
                         "See 06-rules-and-night.\n")

        groups, by = [], {}
        for f in w["fields"]:
            g = f.get("group", "Other")
            if g not in by:
                by[g] = []
                groups.append(g)
            by[g].append(f)
        for g in groups:
            out.append("**%s**\n" % g)
            out.append("| field | type | default | constraints | flags | what it does |")
            out.append("|---|---|---|---|---|---|")
            for f in by[g]:
                out.append("| `%s` | %s | %s | %s | %s | %s |" % (
                    f["name"], f["type"], md_escape(f.get("default", "")),
                    md_escape(constraint(f)), flags(f, dev),
                    md_escape(f.get("help", ""))))
            out.append("")

    txt = "\n".join(out) + "\n"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(txt)
    nfields = sum(len(w["fields"]) for w in widgets)
    nnight = sum(1 for w in widgets for f in w["fields"] if f.get("night_overridable"))
    print("wrote %s — %d widgets, %d fields, %d night-overridable"
          % (os.path.relpath(OUT, ROOT), len(widgets), nfields, nnight))
    if editor_only:
        print("editor-only (device from_json never reads):")
        for k, v in editor_only.items():
            print("  %s: %s" % (k, ", ".join(v)))
    elif device_fields("meter") is None:
        print("note: no firmware sources and no _bundled snapshot — editor-only "
              "fields NOT cross-checked (run scripts/bundle_standalone.py in the repo)")


if __name__ == "__main__":
    main()
