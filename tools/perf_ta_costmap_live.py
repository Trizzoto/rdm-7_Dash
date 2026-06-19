#!/usr/bin/env python3
"""Live-CAN knockout cost map of Time_Attack on the SHIPPED build.

Unlike perf_ta_costmap.py (which drives via the simulator/inject path and so
can't see the CAN-dispatch coalescing), this previews each knockout variant
and measures under live bus traffic with long windows to average the replay
loop's phase variance. The render_ms delta vs the full layout is that group's
real steady-state cost as experienced while driving. Restores Time_Attack.
"""
import json, time, copy, statistics, urllib.request

DASH = "http://192.168.4.61"
SETTLE_S = 5
WINDOW_S = 30

KNOCKOUTS = {
    "full": [],
    "no_meter": ["RPM"],
    "no_arcs600": ["ARC COOLANT", "ARC FUEL"],
    "no_gear_text": ["GEAR"],
    "no_speed_text": ["SPEED"],
    "no_shift": ["shift_light_1"],
    "no_panels": ["panel_1", "panel_2", "panel_3", "panel_4"],
    "no_shapes": ["shape_panel_1", "shape_panel_2"],
    "no_throttle_arc": ["THROTTLE"],
    "no_bar": ["bar_1"],
    "no_odo_text": ["text_1", "text_2"],
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
    time.sleep(SETTLE_S)
    fps, rend, maxms = [], [], []
    for _ in range(WINDOW_S):
        p = json.loads(http_get("/api/perf"))
        fps.append(p["fps"])
        rend.append(p["avg_render_ms"])
        time.sleep(1)
    # max single-frame from the boot ring's most recent window
    return (statistics.mean(fps), statistics.mean(rend),
            min(fps), max(fps))


def main():
    base = json.loads(http_get("/api/layout/raw?name=Time_Attack")
                      .decode("utf-8-sig"))
    results = {}
    for tag, removed in KNOCKOUTS.items():
        lay = copy.deepcopy(base)
        lay["widgets"] = [w for w in lay["widgets"] if w["id"] not in removed]
        http_post("/api/layout/preview", lay)
        fps, rend, lo, hi = measure()
        results[tag] = (fps, rend, lo, hi)
        print(f"{tag:16s} fps={fps:5.1f} (min {lo:4.1f} max {hi:4.1f})  "
              f"render={rend:5.1f} ms", flush=True)

    http_post("/api/layout/set", {"name": "Time_Attack"})

    full_rend = results["full"][1]
    full_fps = results["full"][0]
    print("\n--- cost attribution (render ms saved / fps gained by removing) ---",
          flush=True)
    rows = [(t, full_rend - r[1], r[0] - full_fps)
            for t, r in results.items() if t != "full"]
    for tag, dms, dfps in sorted(rows, key=lambda x: -x[1]):
        print(f"{tag:16s} -{dms:5.1f} ms   (+{dfps:4.1f} fps)", flush=True)
    print(f"\nfull layout: {full_fps:.1f} fps, {full_rend:.1f} ms/frame",
          flush=True)
    print("restored Time_Attack", flush=True)


if __name__ == "__main__":
    main()
