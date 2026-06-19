"""Reorder the 'Ticks' group fields in widgets.schema.json into a consistent
order across every tick-bearing widget, so the inspector reads major -> medium
-> minor, section by section (spacing, thickness, length, colour).

Inspector renders fields in array order within each group, so we just sort the
group=='Ticks' fields among themselves and re-emit them contiguously where the
first one sat. Non-Ticks fields are untouched. Run the two codegens afterward.
"""
import json, io, re, sys

PATH = "schema/widgets.schema.json"
S = json.load(io.open(PATH, encoding="utf-8"))
widgets = S.get("widgets") or S.get("widget_types")

def prop_rank(name):
    # section order: master toggle, config (side), spacing, thickness, length, colour
    if name == "show_ticks":            return 0
    if "side" in name:                  return 1
    if "step" in name or "count" in name: return 2   # spacing / position
    if "width" in name:                 return 3      # thickness
    if "len" in name:                   return 4      # length  (covers _len and _length)
    if "color" in name:                 return 5
    return 6

def tier_rank(name):
    if name.startswith("major"): return 0
    if name.startswith("mid"):   return 1
    if name.startswith("minor"): return 2
    return 2   # legacy un-prefixed (pathbar tick_len/width/color) = the minor tier

def norm_tick_label(name):
    """Consistent label for a tier'd tick field, or None to leave it alone."""
    if name.startswith("major"):                                  tier = "Major"
    elif name.startswith("mid"):                                  tier = "Medium"
    elif name.startswith("minor"):                                tier = "Minor"
    elif name in ("tick_len", "tick_width", "tick_color"):        tier = "Minor"  # pathbar legacy minor
    else: return None
    if "step" in name:    prop = "Tick Value Spacing"
    elif "width" in name: prop = "Tick Width"
    elif "len" in name:   prop = "Tick Length"
    elif "color" in name: prop = "Tick Color"
    else: return None
    return tier + " " + prop

relabeled = []
for w in widgets:
    # Only multi-tier gauges (those with a major_tick_* field) — single-tier
    # bars keep their plain "Tick Width" labels.
    if not any(f["name"].startswith("major") and "tick" in f["name"] for f in w["fields"]):
        continue
    for f in w["fields"]:
        if f.get("group") == "Ticks":
            nl = norm_tick_label(f["name"])
            if nl and nl != f.get("label"):
                relabeled.append((w["name"], f["name"], f["label"], nl)); f["label"] = nl

changed = []
for w in widgets:
    fields = w["fields"]
    tick_idx = [i for i, f in enumerate(fields) if f.get("group") == "Ticks"]
    if len(tick_idx) < 2:
        continue
    tick_fields = [fields[i] for i in tick_idx]
    before = [f["name"] for f in tick_fields]
    tick_fields.sort(key=lambda f: (prop_rank(f["name"]), tier_rank(f["name"]), f["name"]))
    after = [f["name"] for f in tick_fields]
    if before == after:
        continue
    # rebuild: drop the old tick fields, splice the sorted block at the first slot
    first = tick_idx[0]
    keep = [f for i, f in enumerate(fields) if i not in set(tick_idx)]
    # count how many non-tick fields preceded `first`
    insert_at = sum(1 for i in range(first) if i not in set(tick_idx))
    w["fields"] = keep[:insert_at] + tick_fields + keep[insert_at:]
    changed.append((w["name"], after))

txt = json.dumps(S, ensure_ascii=False, indent=2)
# Match the original style: keep option objects { "value": X, "label": Y } inline.
txt = re.sub(r'\{\s*\n\s*"value": ([^\n]+?),\s*\n\s*"label": ([^\n]+?)\s*\n\s*\}',
             r'{ "value": \1, "label": \2 }', txt)
with io.open(PATH, "w", encoding="utf-8", newline="\r\n") as f:
    f.write(txt + "\n")
print("reordered Ticks group in %d widget(s):" % len(changed))
for name, order in changed:
    print("  %-10s -> %s" % (name, ", ".join(order)))
print("relabeled %d field(s) for tier consistency:" % len(relabeled))
for wn, fn, old, new in relabeled:
    print("  %-9s %-18s '%s' -> '%s'" % (wn, fn, old, new))
