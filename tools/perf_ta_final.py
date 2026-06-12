#!/usr/bin/env python3
"""Final check: Time_Attack layout FPS, as saved and with a shadowed circle."""
import json, time, copy, urllib.request

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


def measure(tag, secs=20):
    time.sleep(5)
    fps, rend = [], []
    for _ in range(secs):
        p = json.loads(http_get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"])
        time.sleep(1)
    print(f"{tag}: fps_avg={sum(fps)/len(fps):.1f} fps_min={min(fps)} "
          f"fps_max={max(fps)} render_ms_avg={sum(rend)/len(rend):.1f}",
          flush=True)


def main():
    base = json.loads(http_get("/api/layout/raw?name=Time_Attack")
                      .decode("utf-8-sig"))

    http_post("/api/layout/preview", base)
    measure("TA_as_saved")

    shadowed = copy.deepcopy(base)
    for w in shadowed["widgets"]:
        if w["id"] == "shape_panel_1":
            w["config"]["shadow_width"] = 30
    http_post("/api/layout/preview", shadowed)
    measure("TA_shadowed_circle")

    data = http_get("/api/screenshot")
    open("shot_ta_final.png", "wb").write(data)
    http_post("/api/layout/set", {"name": "Time_Attack"})
    print("restored Time_Attack", flush=True)


if __name__ == "__main__":
    main()
