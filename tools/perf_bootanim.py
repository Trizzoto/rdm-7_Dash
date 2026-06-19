#!/usr/bin/env python3
"""Measure FPS/render time during the boot reveal animation (live preview)."""
import json, time, urllib.request

DASH = "http://192.168.4.61"


def http_get(path):
    with urllib.request.urlopen(DASH + path, timeout=10) as r:
        return r.read()


def http_post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def main():
    # idle baseline
    p0 = json.loads(http_get("/api/perf"))
    print(f"idle: fps={p0['fps']} render_ms={p0['avg_render_ms']}", flush=True)

    http_post("/api/splash/bootanim", {"enabled": True, "preview": True})
    t0 = time.time()
    worst_ms, worst_fps = 0, 999
    while time.time() - t0 < 5:
        p = json.loads(http_get("/api/perf"))
        t = time.time() - t0
        print(f"t={t:4.1f}s fps={p['fps']:5.1f} render_ms={p['avg_render_ms']:3d} "
              f"avg_px={p['avg_px']:6d} max%={p['max_pct_screen']}", flush=True)
        worst_ms = max(worst_ms, p["avg_render_ms"])
        worst_fps = min(worst_fps, p["fps"])
        time.sleep(0.3)
    print(f"WORST: render_ms={worst_ms} fps={worst_fps}", flush=True)


if __name__ == "__main__":
    main()
