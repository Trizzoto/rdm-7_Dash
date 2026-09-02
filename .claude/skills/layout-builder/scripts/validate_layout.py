#!/usr/bin/env python3
"""Validate a layout JSON against the firmware schema BEFORE applying it.
Catches the mistakes that otherwise fail silently on the device: wrong/typo'd
widget types or config fields, hex-string colours, editor-only fields the device
ignores, out-of-range coords, over-cap slots, malformed rules/gradients, size
clamping, duplicate ids, a missing schema_version, and an over-32 KB payload.

    python validate_layout.py my_layout.json

Exit 0 = OK (warnings allowed), exit 1 = errors (don't apply). Pass
--channels-file channels.json (from /api/channels) to also check signal_name
bindings against the live device.
"""
import json, io, os, sys, re, argparse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
# Standalone handoff (no RDM-7_Dash repo checkout): fall back to a bundled
# snapshot shipped alongside this script — see _bundled/README.md.
BUNDLED = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "_bundled"))
SCHEMA = os.path.join(ROOT, "schema", "widgets.schema.json")
if not os.path.exists(SCHEMA):
    SCHEMA = os.path.join(BUNDLED, "schema", "widgets.schema.json")
WIDGETS_DIR = os.path.join(ROOT, "main", "widgets")
DEVICE_FIELDS_JSON = os.path.join(BUNDLED, "device_fields.json")
LAYOUT_MANAGER_H = os.path.join(ROOT, "main", "layout", "layout_manager.h")
if not os.path.exists(LAYOUT_MANAGER_H):
    LAYOUT_MANAGER_H = os.path.join(BUNDLED, "layout_manager.h")
MAX_BYTES = 32768

# Binding / positioning fields handled by the common loader (valid on any
# widget, not in the per-widget schema).
UNIVERSAL = {"signal_name", "signal", "channel", "slot", "rules", "night"}

# Slot caps the firmware enforces by dropping the widget outright.
SLOT_CAPS = {"indicator": 2, "warning": 8}

RULE_OPS = {">", "<", ">=", "<=", "==", "!=", "range"}
RULE_VALUE_TYPES = {"number", "color", "bool", "string"}
MAX_WIDGET_RULES = 16
MAX_RULE_OVERRIDES = 16
RULE_FIELD_NAME_LEN = 32
GRADIENT_MAX_STOPS = 8

# Widgets whose firmware has no support for these features at all.
NO_RULES_TYPES = {"pathbar", "anim"}
NO_NIGHT_TYPES = {"pathbar", "track_map"}

# The editor schema is only the EDITOR's field set; the device from_json accepts
# a superset. So the real allowlist = schema fields UNION the string literals the
# widget's from_json actually reads (the firmware contract). See 05-quirks.
_FIELD_RE = re.compile(r'GetObjectItem(?:CaseSensitive)?\s*\(\s*[A-Za-z_]\w*\s*,\s*"([a-z_]\w*)"')

_UI_ONLY_TYPES = {"label", "info", "button", "upload"}

_BUNDLED_FIELDS = None


def device_fields_from_c(wtype):
    """Field names the device's from_json actually reads. Returns None when
    neither the firmware sources nor a bundled snapshot are available, so
    callers can skip the checks that depend on knowing them."""
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


def load_schema():
    S = json.load(io.open(SCHEMA, encoding="utf-8"))
    widgets = S.get("widgets") or S.get("widget_types")
    fields, color_fields, constraints, singletons = {}, {}, {}, set()
    for w in widgets:
        fmap = {f["name"]: f for f in w["fields"]}
        fields[w["name"]] = fmap
        color_fields[w["name"]] = {n for n, f in fmap.items() if f["type"] == "color"}
        constraints[w["name"]] = w.get("constraints") or {}
        if w.get("singleton"):
            singletons.add(w["name"])
    ver = "?"
    try:
        m = re.search(r"#define\s+LAYOUT_SCHEMA_VERSION\s+(\d+)",
                      io.open(LAYOUT_MANAGER_H, encoding="utf-8").read())
        ver = int(m.group(1)) if m else "?"
    except Exception:
        pass
    return fields, color_fields, constraints, singletons, ver


def _clamp_limit(c, key):
    v = c.get(key)
    return {"screen_w": 800, "screen_h": 480}.get(v, v)


def check_rules(cfg, tag, t, warns, errors):
    rules = cfg.get("rules")
    if rules is None:
        return
    if not isinstance(rules, list):
        errors.append("%s: 'rules' must be an array" % tag)
        return
    if t in NO_RULES_TYPES:
        warns.append("%s: %s has no rule support in firmware — 'rules' is ignored" % (tag, t))
    if len(rules) > MAX_WIDGET_RULES:
        warns.append("%s: %d rules, only the first %d are kept"
                     % (tag, len(rules), MAX_WIDGET_RULES))
    for ri, r in enumerate(rules):
        rtag = "%s rules[%d]" % (tag, ri)
        if not isinstance(r, dict):
            errors.append("%s: not an object" % rtag)
            continue
        if not r.get("signal_name"):
            warns.append("%s: no signal_name — the rule is inert and dropped on save" % rtag)
        op = r.get("op")
        if op not in RULE_OPS:
            warns.append("%s: op %r unknown — the firmware falls back to '=='" % (rtag, op))
        if op == "range":
            if "range_min" not in r or "range_max" not in r:
                errors.append("%s: op 'range' needs range_min and range_max" % rtag)
        elif "threshold" not in r:
            warns.append("%s: op %r has no threshold (defaults to 0)" % (rtag, op))
        ovs = r.get("overrides")
        if ovs is None:
            warns.append("%s: no overrides — the rule does nothing" % rtag)
            continue
        if not isinstance(ovs, list):
            errors.append("%s: 'overrides' must be an array" % rtag)
            continue
        if len(ovs) > MAX_RULE_OVERRIDES:
            warns.append("%s: %d overrides, only the first %d are kept"
                         % (rtag, len(ovs), MAX_RULE_OVERRIDES))
        for oi, ov in enumerate(ovs):
            otag = "%s overrides[%d]" % (rtag, oi)
            if not isinstance(ov, dict):
                errors.append("%s: not an object" % otag)
                continue
            fn = ov.get("field")
            if not fn:
                errors.append("%s: missing 'field'" % otag)
            elif len(fn) >= RULE_FIELD_NAME_LEN:
                errors.append("%s: field %r is >= %d chars and will be truncated"
                              % (otag, fn, RULE_FIELD_NAME_LEN))
            vt = ov.get("type")
            if vt not in RULE_VALUE_TYPES:
                warns.append("%s: type %r unknown — treated as 'number'" % (otag, vt))
            val = ov.get("value")
            if vt == "color" and isinstance(val, str):
                errors.append("%s: colour override is the string %r — must be an "
                              "RGB565 integer" % (otag, val))
            elif vt == "color" and isinstance(val, (int, float)) and not (0 <= val <= 65535):
                errors.append("%s: colour %s is not a valid RGB565 (0..65535)" % (otag, val))
            elif vt == "bool" and not isinstance(val, bool):
                warns.append("%s: type 'bool' but value is %r" % (otag, val))


def check_grad_stops(cfg, tag, warns, errors):
    gs = cfg.get("grad_stops")
    if gs is None:
        return
    if not isinstance(gs, list):
        errors.append("%s: 'grad_stops' must be an array" % tag)
        return
    if 0 < len(gs) < 2:
        warns.append("%s: grad_stops has %d stop — needs >= 2 or the widget "
                     "paints solid" % (tag, len(gs)))
    if len(gs) > GRADIENT_MAX_STOPS:
        warns.append("%s: %d gradient stops, only the first %d are kept"
                     % (tag, len(gs), GRADIENT_MAX_STOPS))
    last = None
    for si, s in enumerate(gs):
        stag = "%s grad_stops[%d]" % (tag, si)
        if not isinstance(s, dict):
            errors.append("%s: not an object" % stag)
            continue
        pos, col = s.get("pos"), s.get("color")
        if not isinstance(pos, (int, float)) or not (0 <= pos <= 100):
            errors.append("%s: pos=%r must be a number 0..100" % (stag, pos))
        elif last is not None and pos < last:
            warns.append("%s: pos %s is out of order (stops should ascend)" % (stag, pos))
        else:
            last = pos
        if isinstance(col, str):
            errors.append("%s: color is the string %r — must be an RGB565 integer"
                          % (stag, col))
        elif not isinstance(col, (int, float)) or not (0 <= col <= 65535):
            errors.append("%s: color=%r is not a valid RGB565 (0..65535)" % (stag, col))


def check_night(cfg, tag, t, fields_t, warns):
    night = cfg.get("night")
    if night is None:
        return
    if not isinstance(night, dict):
        warns.append("%s: 'night' must be an object" % tag)
        return
    if t in NO_NIGHT_TYPES:
        warns.append("%s: %s has no night-mode support in firmware — 'night' is "
                     "ignored" % (tag, t))
        return
    # Accept either the field name or its night_key alias.
    ok = set()
    for n, f in fields_t.items():
        if f.get("night_overridable"):
            ok.add(f.get("night_key") or n)
    for k, v in night.items():
        if ok and k not in ok:
            warns.append("%s: night.%s is not night-overridable on %s (catalog "
                         "flags the ones that are with N)" % (tag, k, t))
        if isinstance(v, str) and k.endswith("_color"):
            warns.append("%s: night.%s is the string %r — colours are RGB565 "
                         "integers" % (tag, k, v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layout")
    ap.add_argument("--channels-file",
                    help="JSON from GET /api/channels, to check signal_name bindings")
    args = ap.parse_args()

    fields, color_fields, constraints, singletons, ver = load_schema()
    errors, warns = [], []

    raw = io.open(args.layout, encoding="utf-8").read()
    try:
        L = json.loads(raw)
    except Exception as e:
        print("ERROR: not valid JSON:", e)
        sys.exit(1)

    size = len(json.dumps(L, separators=(",", ":")).encode("utf-8"))
    if size > MAX_BYTES:
        errors.append("layout is %d B — over the %d B budget; trim defaults-equal "
                      "fields" % (size, MAX_BYTES))

    if L.get("schema_version") != ver:
        warns.append("schema_version is %r; device expects %s"
                     % (L.get("schema_version"), ver))
    for k in ("screen_w", "screen_h"):
        exp = 800 if k == "screen_w" else 480
        if L.get(k) not in (None, exp):
            warns.append("%s=%s (expected %d)" % (k, L.get(k), exp))
    if not L.get("name"):
        warns.append("no top-level 'name' — required by /api/layout/save")

    nm = L.get("night_mode")
    if nm is not None:
        if not isinstance(nm, dict) or not nm.get("signal_name"):
            warns.append("'night_mode' needs {signal_name, active_when} to do anything")

    known_signals = None
    if args.channels_file:
        try:
            cj = json.load(io.open(args.channels_file, encoding="utf-8"))
            ch = cj.get("channels", cj if isinstance(cj, list) else [])
            known_signals = {c.get("signal") for c in ch if c.get("signal")}
        except Exception as e:
            warns.append("could not read channels file: %s" % e)

    ids, seen_types, slots_used, offsize = {}, {}, {}, []
    ws = L.get("widgets", [])
    if not isinstance(ws, list) or not ws:
        errors.append("'widgets' is missing or empty (boots to a blank screen)")
    for i, w in enumerate(ws or []):
        tag = "widgets[%d] id=%s" % (i, w.get("id", "?"))
        t = w.get("type")
        if t not in fields:
            errors.append("%s: unknown type %r (see 02-widget-catalog)" % (tag, t))
            continue
        seen_types[t] = seen_types.get(t, 0) + 1
        for key in ("id", "x", "y", "w", "h"):
            if key not in w:
                warns.append("%s: missing '%s'" % (tag, key))
        if w.get("id") in ids:
            warns.append("%s: duplicate id (also widgets[%d])" % (tag, ids[w["id"]]))
        ids[w.get("id")] = i

        for c, lim in (("x", 400), ("y", 240)):
            v = w.get(c)
            if isinstance(v, (int, float)) and abs(v) > lim:
                warns.append("%s: %s=%s off-screen (|%s| <= %d)" % (tag, c, v, c, lim))

        # Editor resize limits. The FIRMWARE does not clamp (widget_constraints[]
        # is defined but never read), so this is not a rendering problem — real
        # layouts sit under the minimum for small text. It only matters because
        # the editor snaps the widget the first time someone drags it. Collected
        # and reported as one summary line so it can't drown the real findings.
        con = constraints.get(t, {})
        for dim, lo_k, hi_k in (("w", "min_w", "max_w"), ("h", "min_h", "max_h")):
            v = w.get(dim)
            if not isinstance(v, (int, float)):
                continue
            lo, hi = _clamp_limit(con, lo_k), _clamp_limit(con, hi_k)
            if isinstance(lo, (int, float)) and v < lo:
                offsize.append("%s %s=%s<%s" % (w.get("id", i), dim, v, lo))
            if isinstance(hi, (int, float)) and v > hi:
                offsize.append("%s %s=%s>%s" % (w.get("id", i), dim, v, hi))
        if t == "meter" and w.get("w") != w.get("h"):
            warns.append("%s: a meter is kept square by the firmware — set w == h" % tag)

        cfg = w.get("config", {}) or {}
        if not isinstance(cfg, dict):
            errors.append("%s: 'config' must be an object" % tag)
            continue

        # Slot caps: over-cap slot widgets are dropped outright by the firmware.
        cap = SLOT_CAPS.get(t)
        if cap is not None:
            s = cfg.get("slot", 0)
            if isinstance(s, (int, float)) and s >= cap:
                errors.append("%s: slot=%s but %s only has slots 0..%d — the "
                              "firmware DROPS this widget" % (tag, s, t, cap - 1))
            key = (t, s)
            if key in slots_used:
                warns.append("%s: slot %s already used by widgets[%d]"
                             % (tag, s, slots_used[key]))
            slots_used[key] = i

        if t == "meter" and ("minor_tick_step" in cfg or "major_tick_step" in cfg):
            warns.append("%s: meter minor/major ticks are COUNT-based — use "
                         "minor_tick_count + major_tick_every, not *_tick_step "
                         "(see 05-quirks)" % tag)
        if t in ("meter", "arc") and ("start_angle_user" in cfg or "sweep_degrees" in cfg):
            warns.append("%s: start_angle_user/sweep_degrees are EDITOR-ONLY — the "
                         "device reads start_angle/end_angle in LVGL degrees "
                         "(lvgl = user + 270). See 05-quirks." % tag)

        dev = device_fields_from_c(t)
        allowed = set(fields[t]) | UNIVERSAL | (dev or set())
        for fn, fv in cfg.items():
            if fn not in allowed:
                warns.append("%s: unknown config field '%s' (typo? wrong widget?)"
                             % (tag, fn))
                continue
            # In the schema but not read by the device = editor-only, silently
            # ignored. Only checkable when we know the device's field set.
            fdef = fields[t].get(fn)
            if (dev is not None and fn not in dev and fn not in UNIVERSAL
                    and fdef is not None and fdef["type"] not in _UI_ONLY_TYPES):
                if fn not in ("start_angle_user", "sweep_degrees",
                              "minor_tick_step", "major_tick_step"):
                    warns.append("%s: '%s' is EDITOR-ONLY — the device's from_json "
                                 "never reads it (see 05-quirks)" % (tag, fn))
            if fn in color_fields[t]:
                if isinstance(fv, str):
                    errors.append("%s: %s is the string %r — colours are RGB565 "
                                  "INTEGERS; a string is silently ignored" % (tag, fn, fv))
                elif isinstance(fv, (int, float)) and not (0 <= fv <= 65535):
                    errors.append("%s: %s=%s not a valid RGB565 (0..65535)" % (tag, fn, fv))

        check_rules(cfg, tag, t, warns, errors)
        check_grad_stops(cfg, tag, warns, errors)
        check_night(cfg, tag, t, fields[t], warns)

        sn = cfg.get("signal_name")
        if sn and known_signals is not None and sn not in known_signals:
            warns.append("%s: signal_name %r not a live channel (will read '--')"
                         % (tag, sn))

    if offsize:
        warns.append("%d widget(s) outside the EDITOR's resize limits — they "
                     "render fine (the firmware does not clamp), but the editor "
                     "snaps them if dragged: %s"
                     % (len(offsize), ", ".join(offsize)))

    for t in singletons:
        if seen_types.get(t, 0) > 1:
            errors.append("%d '%s' widgets — it is a singleton, only one is kept"
                          % (seen_types[t], t))

    print("size: %d B / %d B   widgets: %d" % (size, MAX_BYTES, len(ws or [])))
    if device_fields_from_c("meter") is None:
        print("  note: no firmware sources or bundled snapshot — editor-only and "
              "device-field checks were SKIPPED")
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
