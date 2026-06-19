#!/usr/bin/env python3
"""A/B test: overlay shapes on meter x static_ticks, measured via /api/perf.

Uses /api/layout/preview (live apply, no persist). Restores the saved active
layout at the end via /api/layout/set.
"""
import json, sys, time, copy, urllib.request

DASH = "http://192.168.4.61"
SETTLE_S = 4
SAMPLES = 8
SAMPLE_GAP_S = 1.0


def http_get(path):
    with urllib.request.urlopen(DASH + path, timeout=10) as r:
        return r.read()


def http_post(path, body):
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def measure(tag):
    time.sleep(SETTLE_S)
    fps, rend, px, flush = [], [], [], []
    for _ in range(SAMPLES):
        p = json.loads(http_get("/api/perf"))
        fps.append(p["fps"])
        rend.append(p["avg_render_ms"])
        px.append(p["avg_px"])
        flush.append(p["flush_us_per_frame"])
        time.sleep(SAMPLE_GAP_S)
    out = {
        "tag": tag,
        "fps_avg": round(sum(fps) / len(fps), 1),
        "fps_min": min(fps), "fps_max": max(fps),
        "render_ms_avg": round(sum(rend) / len(rend), 1),
        "avg_px": round(sum(px) / len(px)),
        "flush_us_avg": round(sum(flush) / len(flush)),
    }
    print(json.dumps(out), flush=True)
    return out


def screenshot(tag):
    data = http_get("/api/screenshot")
    fn = f"shot_{tag}.png"
    with open(fn, "wb") as f:
        f.write(data)
    print(f"saved {fn} ({len(data)} bytes)", flush=True)


def variant(base, shapes, static_ticks):
    lay = copy.deepcopy(base)
    if not shapes:
        lay["widgets"] = [w for w in lay["widgets"]
                          if w["type"] != "shape_panel"]
    for w in lay["widgets"]:
        if w["type"] == "meter":
            cfg = w.setdefault("config", {})
            if static_ticks:
                cfg["static_ticks"] = True
            else:
                cfg.pop("static_ticks", None)
    return lay


def main():
    base = json.loads(open("ta_layout.json", "rb").read().decode("utf-8-sig"))
    results = []
    matrix = [
        ("A_shapes_static", True, True),     # as shipped
        ("B_noshapes_static", False, True),
        ("C_shapes_dynamic", True, False),
        ("D_noshapes_dynamic", False, False),
    ]
    for tag, shapes, st in matrix:
        lay = variant(base, shapes, st)
        body = json.dumps(lay).encode()
        print(f"--- {tag}: preview apply ({len(body)} B)", flush=True)
        http_post("/api/layout/preview", body)
        r = measure(tag)
        screenshot(tag)
        results.append(r)

    # restore the real active layout
    http_post("/api/layout/set", json.dumps({"name": "Time_Attack"}).encode())
    print("restored Time_Attack", flush=True)
    print(json.dumps(results, indent=1), flush=True)


if __name__ == "__main__":
    main()
