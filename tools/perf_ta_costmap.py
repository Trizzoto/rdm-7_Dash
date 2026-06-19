#!/usr/bin/env python3
"""Knockout cost-map of the Time_Attack layout under deterministic sim load.

Previews TA with one widget group removed at a time; the render_ms delta vs
the full layout is that group's steady-state cost. Restores Time_Attack."""
import json, time, copy, urllib.request

DASH = "http://192.168.4.61"
WINDOW_S = 25

TA_SIGNALS = ["RPM", "GEAR", "THROTTLE", "VEHICLE_SPEED", "COOLANT_TEMP",
              "LAMBDA", "INTAKE_AIR_TEMP", "BATTERY_VOLTAGE",
              "LAMBDA_CORR_A_lambda_corre_2", "IGNITION"]

KNOCKOUTS = {
    "full": [],
    "no_meter": ["RPM"],                       # ids to remove
    "no_arcs600": ["ARC COOLANT", "ARC FUEL"],
    "no_gear_text": ["GEAR"],
    "no_speed_text": ["SPEED"],
    "no_shift": ["shift_light_1"],
    "no_panels": ["panel_1", "panel_2", "panel_3", "panel_4"],
    "no_shapes": ["shape_panel_1", "shape_panel_2"],
    "no_throttle_arc": ["THROTTLE"],
    "no_bar": ["bar_1"],
}


def http_get(path):
    with urllib.request.urlopen(DASH + path, timeout=10) as r:
        return r.read()


def http_post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def measure():
    time.sleep(4)
    fps, rend = [], []
    for _ in range(WINDOW_S):
        p = json.loads(http_get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"])
        time.sleep(1)
    return sum(fps) / len(fps), sum(rend) / len(rend)


def main():
    base = json.loads(http_get("/api/layout/raw?name=Time_Attack")
                      .decode("utf-8-sig"))
    results = {}
    for tag, removed_ids in KNOCKOUTS.items():
        lay = copy.deepcopy(base)
        lay["widgets"] = [w for w in lay["widgets"]
                          if w["id"] not in removed_ids]
        http_post("/api/layout/preview", lay)
        time.sleep(2)
        http_post("/api/signal/inject",
                  {"values": [{"signal": s, "value": 1} for s in TA_SIGNALS]})
        http_post("/api/signal/simulate", {"enabled": False})
        time.sleep(0.5)
        http_post("/api/signal/simulate", {"enabled": True})
        fps, rend = measure()
        results[tag] = (fps, rend)
        print(f"{tag}: fps={fps:.1f} render_ms={rend:.1f}", flush=True)

    http_post("/api/signal/simulate", {"enabled": False})
    for s in TA_SIGNALS:
        http_post("/api/signal/clear", {"signal": s})
    http_post("/api/layout/set", {"name": "Time_Attack"})

    full_rend = results["full"][1]
    print("\n--- cost vs full ---", flush=True)
    for tag, (fps, rend) in sorted(results.items(), key=lambda x: x[1][1]):
        if tag == "full":
            continue
        print(f"{tag}: saves {full_rend - rend:+.1f} ms/frame", flush=True)
    print("restored Time_Attack", flush=True)


if __name__ == "__main__":
    main()
