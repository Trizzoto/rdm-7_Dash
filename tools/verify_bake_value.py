#!/usr/bin/env python3
"""Demonstrate overlay-bake value on the case that actually benefits: a
translucent + shadowed circle the needle sweeps through (the user's original
'circle on meter kills fps' case). Baked should approach bare-meter fps."""
import json, time, copy, urllib.request

DASH = "http://192.168.4.61"

METER = {"type": "meter", "id": "RPM", "x": 0, "y": 0, "w": 450, "h": 450,
         "config": {"slot": 0, "min": 0, "max": 7000, "start_angle": 135,
                    "end_angle": 45, "signal_name": "RPM",
                    "minor_tick_count": 36, "needle_width": 7,
                    "needle_color": 63488}}
# Translucent + shadowed circle over the sweep — the expensive overlay.
CIRCLE = {"type": "shape_panel", "id": "ovl", "x": 0, "y": 0, "w": 230,
          "h": 230, "config": {"shape_type": "circle", "bg_color": 1057,
                               "bg_opa": 150, "shadow_width": 25,
                               "shadow_color": 65535, "shadow_opa": 160}}
FPS = {"type": "text", "id": "FPS", "x": 360, "y": 210, "w": 70, "h": 30,
       "config": {"slot": 0, "decimals": 0, "signal_name": "FPS"}}


def get(p):
    return urllib.request.urlopen(DASH + p, timeout=10).read()


def post(p, o):
    r = urllib.request.Request(DASH + p, data=json.dumps(o).encode(),
                               headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(r, timeout=20).read()


def lay(overlay, bake):
    ws = [copy.deepcopy(METER)]
    if overlay:
        c = copy.deepcopy(CIRCLE)
        if bake:
            c["config"]["bake_into_gauge"] = True
        ws.append(c)
    ws.append(copy.deepcopy(FPS))
    return {"schema_version": 14, "name": "PERF", "screen_w": 800,
            "screen_h": 480, "signals": [], "widgets": ws}


def measure(tag):
    post("/api/signal/inject", {"signal": "RPM", "value": 3500})
    post("/api/signal/simulate", {"enabled": False}); time.sleep(0.4)
    post("/api/signal/simulate", {"enabled": True}); time.sleep(5)
    fps, rend = [], []
    for _ in range(18):
        p = json.loads(get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"]); time.sleep(1)
    post("/api/signal/simulate", {"enabled": False})
    print(f"{tag:22s} fps={sum(fps)/len(fps):5.1f}  "
          f"render={sum(rend)/len(rend):5.1f} ms", flush=True)


def main():
    post("/api/layout/preview", lay(False, False)); time.sleep(2)
    measure("bare_meter")
    post("/api/layout/preview", lay(True, False)); time.sleep(2)
    measure("shadowed_circle_LIVE")
    post("/api/layout/preview", lay(True, True)); time.sleep(2)
    measure("shadowed_circle_BAKED")
    h = json.loads(get("/api/widgets").decode())
    ovl = [w for w in h["widgets"] if w["id"] == "ovl"]
    print("overlay hidden(baked) =", ovl[0].get("hidden") if ovl else "n/a",
          flush=True)
    post("/api/signal/clear", {"signal": "RPM"})
    post("/api/layout/set", {"name": "Time_Attack"})
    print("restored Time_Attack", flush=True)


if __name__ == "__main__":
    main()
