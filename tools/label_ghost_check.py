#!/usr/bin/env python3
"""Ghost-pixel check for the lv_label tight-invalidation patch: flip text
widgets between wide and narrow strings and screenshot after each change.
Any stale glyph fragments (old text not fully erased) show up in the PNGs."""
import json, sys, time, urllib.request

DASH = "http://192.168.4.61"

def http_get(path):
    with urllib.request.urlopen(DASH + path, timeout=15) as r:
        return r.read()

def http_post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()

def shot(name):
    png = http_get("/api/screenshot")
    with open(name, "wb") as f:
        f.write(png)
    print(f"saved {name} ({len(png)} bytes)", flush=True)

def inject(vals):
    http_post("/api/signal/inject",
              {"values": [{"signal": s, "value": v} for s, v in vals.items()]})
    time.sleep(1.2)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    http_post("/api/layout/set", {"name": "Time_Attack"})
    time.sleep(3)
    # lock everything the layout shows so live CAN can't repaint over ghosts
    base = {"RPM": 4000, "THROTTLE": 50, "COOLANT_TEMP": 90, "LAMBDA": 1.0,
            "INTAKE_AIR_TEMP": 30, "BATTERY_VOLTAGE": 13.8, "IGNITION": 15,
            "LAMBDA_CORR_A_lambda_corre_2": 0}
    # wide values first
    inject({**base, "VEHICLE_SPEED": 188, "GEAR": 3})
    shot(f"{out}/ghost_wide.png")
    # narrow values - old wide glyphs must be fully erased
    inject({**base, "VEHICLE_SPEED": 8, "GEAR": 1})
    shot(f"{out}/ghost_narrow.png")
    # rapid flicker then settle, catches partial-union bugs
    for v in (188, 7, 199, 5, 188, 9):
        inject({"VEHICLE_SPEED": v})
    inject({"VEHICLE_SPEED": 42, "GEAR": 2})
    shot(f"{out}/ghost_settled.png")

    http_post("/api/signal/clear", {"all": True})
    print("done", flush=True)

if __name__ == "__main__":
    main()
