"""Make inspector field ORDER consistent within pattern-groups across all
widgets (no renames, no behaviour change) so the editor is easier to follow:

  Ticks   : show -> spacing -> width -> length -> colour, each major>medium>minor
  Numbers : show -> font -> colour -> gap -> divisor -> extras
  Fill*   : state-section on/active (colour,opa) then off/inactive (colour,opa)
            (only the state-paired widgets: indicator, warning, toggle)
  Alerts  : low section then high section (enable, threshold, colour, apply...)

Inspector renders fields in array order within each group, so we sort the
group's fields among themselves and re-emit them where they sat. Preserves the
schema's CRLF + inline { "value":.., "label":.. } formatting for a clean diff.
"""
import json, io, re

PATH = "schema/widgets.schema.json"
S = json.load(io.open(PATH, encoding="utf-8"))
widgets = S.get("widgets") or S.get("widget_types")

def tier3(n):   # ticks: major / medium / minor (legacy un-prefixed == minor)
    if n.startswith("major"): return 0
    if n.startswith("mid"):   return 1
    if n.startswith("minor"): return 2
    return 2

def ticks_key(n):
    if n == "show_ticks": return (0, 0)
    if "side" in n:       return (1, 0)
    if "step" in n or "count" in n: return (2, tier3(n))
    if "width" in n:      return (3, tier3(n))
    if "len" in n:        return (4, tier3(n))
    if "color" in n:      return (5, tier3(n))
    return (6, 0)

def numbers_key(n):
    if n.startswith("show"):  return 0
    if "font" in n:           return 1
    if "color" in n:          return 2
    if "gap" in n:            return 3
    if "divisor" in n:        return 4
    return 5   # ticks_on_top etc.

def fill_state_key(n):
    state = 1 if ("off" in n or "inactive" in n) else 0     # on/active first
    prop  = 1 if "opa" in n else 0                           # colour before opacity
    return (state, prop)

def alerts_key(n):
    # master enable (no low/high in the name) sorts first
    tier = 0 if "low" in n else (1 if "high" in n else -1)
    if "enabled" in n:        prop = 0
    elif "color" in n:        prop = 2
    elif "apply_label" in n:  prop = 3
    elif "apply_value" in n:  prop = 4
    elif "apply_panel" in n:  prop = 5
    else:                     prop = 1   # the threshold value (bar_low / warning_low_threshold / arc_high ...)
    return (tier, prop)

# group -> (sort key fn, optional widget-name allowlist)
GROUPS = {
    "Ticks":   (ticks_key,   None),
    "Numbers": (numbers_key, None),
    "Alerts":  (alerts_key,  None),
    "Fill":    (fill_state_key, {"indicator", "warning", "toggle"}),  # state-paired only
}

# Unify the Ticks section: tick SPACING (steps) is cat:data so it renders in the
# Data accordion, away from width/length/color (cat:appearance, Appearance
# accordion). Move the steps to appearance so all tick settings sit together as
# one ordered sequence (spacing -> width -> length -> color) like the user asked.
recat = []
for w in widgets:
    for f in w["fields"]:
        if f.get("group") == "Ticks" and f["name"].endswith("tick_step") and f.get("category") == "data":
            f["category"] = "appearance"; recat.append((w["name"], f["name"]))

changed = []
for w in widgets:
    for gname, (keyfn, allow) in GROUPS.items():
        if allow and w["name"] not in allow:
            continue
        idx = [i for i, f in enumerate(w["fields"]) if f.get("group") == gname]
        if len(idx) < 2:
            continue
        block = [w["fields"][i] for i in idx]
        before = [f["name"] for f in block]
        block.sort(key=lambda f: (keyfn(f["name"]), f["name"]))
        after = [f["name"] for f in block]
        if before == after:
            continue
        first = idx[0]
        idxset = set(idx)
        keep = [f for i, f in enumerate(w["fields"]) if i not in idxset]
        insert_at = sum(1 for i in range(first) if i not in idxset)
        w["fields"] = keep[:insert_at] + block + keep[insert_at:]
        changed.append((w["name"], gname, after))

# Tick LABELS ("Numbers") belong right after the tick MARKS ("Ticks"), not after
# Frame. Relocate the Numbers block to immediately follow the Ticks block.
moved_grp = []
for w in widgets:
    gp = set(f.get("group") for f in w["fields"])
    if "Ticks" in gp and "Numbers" in gp:
        nums = [f for f in w["fields"] if f.get("group") == "Numbers"]
        rest = [f for f in w["fields"] if f.get("group") != "Numbers"]
        last_tick = max(i for i, f in enumerate(rest) if f.get("group") == "Ticks")
        if rest[last_tick + 1: last_tick + 1 + len(nums)] != nums:
            w["fields"] = rest[:last_tick + 1] + nums + rest[last_tick + 1:]
            moved_grp.append(w["name"])

txt = json.dumps(S, ensure_ascii=False, indent=2)
txt = re.sub(r'\{\s*\n\s*"value": ([^\n]+?),\s*\n\s*"label": ([^\n]+?)\s*\n\s*\}',
             r'{ "value": \1, "label": \2 }', txt)
with io.open(PATH, "w", encoding="utf-8", newline="\r\n") as f:
    f.write(txt + "\n")

print("moved %d tick-step field(s) to appearance (unify Ticks): %s" % (len(recat), [f for _,f in recat[:3]]))
print("moved Numbers after Ticks in: %s" % moved_grp)
print("reordered %d group(s):" % len(changed))
for wn, g, order in changed:
    print("  %-10s [%s] -> %s" % (wn, g, ", ".join(order)))
