#!/usr/bin/env python3
"""A/B: steady-state FPS after plain layout reload vs after boot-anim fade.

Hypothesis: the fade leaves a local opa style on every widget root and that
(or other anim residue) costs steady-state frames. Long windows to average
over CAN replay phases.
"""
import json, time, urllib.request

DASH = "http://192.168.4.61"
WINDOW_S = 30


def http_get(path):
    with urllib.request.urlopen(DASH + path, timeout=10) as r:
        return r.read()


def http_post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def measure(tag):
    time.sleep(4)
    fps, rend = [], []
    for _ in range(WINDOW_S):
        p = json.loads(http_get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"])
        time.sleep(1)
    print(f"{tag}: fps_avg={sum(fps)/len(fps):.1f} min={min(fps)} "
          f"max={max(fps)} render_ms_avg={sum(rend)/len(rend):.1f}", flush=True)


def main():
    # A: fresh widgets, no animation ever ran on them
    http_post("/api/layout/set", {"name": "Time_Attack"})
    measure("A_plain_reload")

    # B: same widgets after a fade preview (leaves local opa styles)
    http_post("/api/splash/bootanim",
              {"enabled": True, "style": "fade", "preview": True})
    time.sleep(4)   # let the fade finish
    measure("B_after_fade")

    # C: control — curtain preview touches no widget styles
    http_post("/api/layout/set", {"name": "Time_Attack"})
    http_post("/api/splash/bootanim",
              {"enabled": True, "style": "curtain", "preview": True})
    time.sleep(3)
    measure("C_after_curtain")

    # restore default style
    http_post("/api/splash/bootanim", {"enabled": True, "style": "fade"})
    print("style restored to fade", flush=True)


if __name__ == "__main__":
    main()
