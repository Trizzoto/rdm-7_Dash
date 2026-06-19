#!/usr/bin/env python3
"""Build Time_Attack_Fast: Ferrari-style optimization of Time_Attack.
Drops the two 600px arcs (incl. the buggy ARC FUEL=RPM), replaces the
overlapping big SPEED text with cheap bottom bars (coolant + speed), keeps
everything else. Saves as a NEW layout (original Time_Attack untouched)."""
import json, sys, urllib.request

DASH = "http://192.168.4.61"


def get(p):
    return urllib.request.urlopen(DASH + p, timeout=10).read()


def post(p, o):
    r = urllib.request.Request(DASH + p, data=json.dumps(o).encode(),
                               headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(r, timeout=20).read()


def bar(bid, x, y, w, h, label, sig, vmin, vmax, anchor=None, show_val=False):
    cfg = {"slot": 0, "label": label, "bar_min": vmin, "bar_max": vmax,
           "bar_low_color": 25407, "bar_high_color": 63488,
           "bar_in_range_color": 42316, "show_bar_value": show_val,
           "signal_name": sig, "bar_bg_color": 12579,
           "bar_border_color": 12579, "label_color": 65535,
           "value_color": 65535}
    if anchor is not None:
        cfg.update({"anchor_enabled": True, "anchor_value": anchor,
                    "anchor_position": 50})
    return {"type": "bar", "id": bid, "x": x, "y": y, "w": w, "h": h,
            "config": cfg, "signal": sig}


def main():
    pos = json.loads(sys.argv[1]) if len(sys.argv) > 1 else None
    base = json.loads(get("/api/layout/raw?name=Time_Attack").decode("utf-8-sig"))
    base["name"] = "Time_Attack_Fast"
    drop = {"ARC COOLANT", "ARC FUEL", "SPEED"}
    base["widgets"] = [w for w in base["widgets"] if w["id"] not in drop]

    # default placements (overridable via argv json {coolant:[x,y,w,h], speed:[...]})
    cp = (pos or {}).get("coolant", [-258, 208, 180, 26])
    sp = (pos or {}).get("speed", [258, 208, 180, 26])
    base["widgets"].append(
        bar("COOLANT_BAR", cp[0], cp[1], cp[2], cp[3], "COOLANT",
            "COOLANT_TEMP", 0, 120, anchor=90))
    base["widgets"].append(
        bar("SPEED_BAR", sp[0], sp[1], sp[2], sp[3], "SPEED",
            "VEHICLE_SPEED", 0, 260, show_val=True))

    post("/api/layout/save", base)
    post("/api/layout/set", {"name": "Time_Attack_Fast"})
    open("ta_fast.png", "wb").write(get("/api/screenshot"))
    print("saved + set Time_Attack_Fast; screenshot ta_fast.png", flush=True)
    print("widgets:", len(base["widgets"]),
          "(removed 2x600 arcs + SPEED text, added 2 bars)", flush=True)


if __name__ == "__main__":
    main()
