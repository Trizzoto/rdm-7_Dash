#!/usr/bin/env python3
"""Validate a layout JSON against the firmware schema BEFORE applying it.
Catches the mistakes that otherwise fail silently on the device: wrong/typo'd
widget types or config fields, out-of-range colours/coords, duplicate ids, a
missing schema_version, and an over-32 KB payload.

    python validate_layout.py my_layout.json

Exit 0 = OK (warnings allowed), exit 1 = errors (don't apply). Pass
--channels-file channels.json (from /api/channels) to also check signal_name
bindings against the live device.
"""
import json, io, os, sys, re, argparse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
# Standalone handoff (no RDM-7_Dash repo checkout): fall back to a bundled
# snapshot shipped alongside this script — see _bundled/README.txt.
BUNDLED = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "_bundled"))
SCHEMA = os.path.join(ROOT, "schema", "widgets.schema.json")
if not os.path.exists(SCHEMA):
    SCHEMA = os.path.join(BUNDLED, "schema", "widgets.schema.json")
WIDGETS_DIR = os.path.join(ROOT, "main", "widgets")
if not os.path.isdir(WIDGETS_DIR):
    WIDGETS_DIR = os.path.join(BUNDLED, "widgets")
LAYOUT_MANAGER_H = os.path.join(ROOT, "main", "layout", "layout_manager.h")
if not os.path.exists(LAYOUT_MANAGER_H):
    LAYOUT_MANAGER_H = os.path.join(BUNDLED, "layout_manager.h")
MAX_BYTES = 32768

# Binding / positioning fields handled by the common loader (valid on any widget,
# not in the per-widget schema).
UNIVERSAL = {"signal_name", "signal", "channel", "slot"}

# The editor schema is only the EDITOR's field set; the device from_json accepts
# a superset. So the real allowlist = schema fields UNION the string literals the
# widget's from_json actually reads (the firmware contract). See 05-quirks.
_FIELD_RE = re.compile(r'GetObjectItem(?:CaseSensitive)?\s*\(\s*[A-Za-z_]\w*\s*,\s*"([a-z_]\w*)"')


def device_fields_from_c(wtype):
    try:
        txt = io.open(os.path.join(WIDGETS_DIR, f"widget_{wtype}.c"),
                      encoding="utf-8", errors="ignore").read()
    except Exception:
        return set()
    names = set(_FIELD_RE.findall(txt))
    names.discard("config")   # the container, not a field
    return names


def load_schema():
    S = json.load(io.open(SCHEMA, encoding="utf-8"))
    widgets = S.get("widgets") or S.get("widget_types")
    fields = {}        # type -> {field_name: field_def}
    color_fields = {}  # type -> set(field_name)
    for w in widgets:
        fmap = {f["name"]: f for f in w["fields"]}
        fields[w["name"]] = fmap
        color_fields[w["name"]] = {n for n, f in fmap.items() if f["type"] == "color"}
    ver = "?"
    try:
        import re
        h = io.open(LAYOUT_MANAGER_H, encoding="utf-8").read()
        m = re.search(r"#define\s+LAYOUT_SCHEMA_VERSION\s+(\d+)", h)
        ver = int(m.group(1)) if m else "?"
    except Exception:
        pass
    return fields, color_fields, ver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layout")
    ap.add_argument("--channels-file", help="JSON from GET /api/channels, to check signal_name bindings")
    args = ap.parse_args()

    fields, color_fields, ver = load_schema()
    errors, warns = [], []

    raw = io.open(args.layout, encoding="utf-8").read()
    try:
        L = json.loads(raw)
    except Exception as e:
        print("ERROR: not valid JSON:", e)
        sys.exit(1)

    size = len(json.dumps(L, separators=(",", ":")).encode("utf-8"))
    if size > MAX_BYTES:
        errors.append(f"layout is {size} B — over the {MAX_BYTES} B budget; trim defaults-equal fields")

    if L.get("schema_version") != ver:
        warns.append(f'schema_version is {L.get("schema_version")!r}; device expects {ver}')
    for k in ("screen_w", "screen_h"):
        if L.get(k) not in (None, 800 if k == "screen_w" else 480):
            warns.append(f"{k}={L.get(k)} (expected {'800' if k=='screen_w' else '480'})")
    if not L.get("name"):
        warns.append("no top-level 'name' — required by /api/layout/save")

    known_signals = None
    if args.channels_file:
        try:
            cj = json.load(io.open(args.channels_file, encoding="utf-8"))
            ch = cj.get("channels", cj if isinstance(cj, list) else [])
            known_signals = {c.get("signal") for c in ch if c.get("signal")}
        except Exception as e:
            warns.append(f"could not read channels file: {e}")

    ids = {}
    ws = L.get("widgets", [])
    if not isinstance(ws, list) or not ws:
        errors.append("'widgets' is missing or empty (boots to a blank screen)")
    for i, w in enumerate(ws or []):
        tag = f"widgets[{i}] id={w.get('id','?')}"
        t = w.get("type")
        if t not in fields:
            errors.append(f"{tag}: unknown type {t!r} (see 02-widget-catalog)")
            continue
        for key in ("id", "x", "y", "w", "h"):
            if key not in w:
                warns.append(f"{tag}: missing '{key}'")
        if w.get("id") in ids:
            warns.append(f"{tag}: duplicate id (also widgets[{ids[w['id']]}])")
        ids[w.get("id")] = i
        for c, lim in (("x", 400), ("y", 240)):
            v = w.get(c)
            if isinstance(v, (int, float)) and abs(v) > lim:
                warns.append(f"{tag}: {c}={v} off-screen (|{c}|<= {lim})")
        cfg = w.get("config", {}) or {}
        if t == "meter" and ("minor_tick_step" in cfg or "major_tick_step" in cfg):
            warns.append(f"{tag}: meter minor/major ticks are COUNT-based — use "
                         "minor_tick_count + major_tick_every, not *_tick_step (see 05-quirks)")
        allowed = set(fields[t]) | UNIVERSAL | device_fields_from_c(t)
        for fn, fv in cfg.items():
            if fn not in allowed:
                warns.append(f"{tag}: unknown config field '{fn}' (typo? wrong widget?)")
                continue
            if fn in color_fields[t] and isinstance(fv, (int, float)):
                if not (0 <= fv <= 65535):
                    errors.append(f"{tag}: {fn}={fv} not a valid RGB565 (0..65535)")
        sn = cfg.get("signal_name")
        if sn and known_signals is not None and sn not in known_signals:
            warns.append(f"{tag}: signal_name {sn!r} not a live channel (will read '--')")

    print(f"size: {size} B / {MAX_BYTES} B   widgets: {len(ws or [])}")
    for w_ in warns:
        print("  WARN:", w_)
    for e_ in errors:
        print("  ERROR:", e_)
    if errors:
        print("FAILED")
        sys.exit(1)
    print("OK")


if __name__ == "__main__":
    main()
