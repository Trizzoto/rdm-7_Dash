#!/usr/bin/env python3
"""Visual parity: static_ticks vs dynamic meter rendering at several sizes.

For each size, render the same meter config dynamic and static with the
needle locked at fixed values, screenshot both, and report pixel diffs.
Second value doubles as a needle-trail check (stale pixels from value 1).
"""
import json, time, copy, io, urllib.request
from PIL import Image, ImageChops

DASH = "http://192.168.4.61"

BASE_CFG = {
    "slot": 0, "min": 0, "max": 7000, "start_angle": 90, "end_angle": 0,
    "signal_name": "RPM", "minor_tick_count": 36, "show_needle_ball": False,
    "needle_width": 7, "needle_color": 63488, "needle_rear_length": 29,
    "needle_tip_style": 2, "shadow_dynamic": False, "meter_bg_opa": 0,
    "label_gap": 22, "tick_label_font": "Manrope Bold:25",
    "tick_label_divisor": 1000, "redline_threshold": 6000,
}

SIZES = [450, 240, 140]
VALUES = [3500, 1200]


def http_get(path):
    with urllib.request.urlopen(DASH + path, timeout=10) as r:
        return r.read()


def http_post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def layout(size, static):
    cfg = copy.deepcopy(BASE_CFG)
    if size <= 240:
        cfg["tick_label_font"] = "Manrope Bold:14"
        cfg["label_gap"] = 12
        cfg["needle_rear_length"] = 15
    if static:
        cfg["static_ticks"] = True
    return {"schema_version": 14, "name": "PARITY", "screen_w": 800,
            "screen_h": 480, "signals": [], "widgets": [
                {"type": "meter", "id": "RPM", "x": 0, "y": 0,
                 "w": size, "h": size, "config": cfg, "signal": "RPM"}]}


def grab(tag):
    data = http_get("/api/screenshot")
    fn = f"par_{tag}.png"
    open(fn, "wb").write(data)
    return Image.open(io.BytesIO(data)).convert("RGB")


def main():
    report = []
    for size in SIZES:
        shots = {}
        for static in (False, True):
            http_post("/api/layout/preview", layout(size, static))
            time.sleep(2)
            for v in VALUES:
                http_post("/api/signal/inject", {"signal": "RPM", "value": v})
                time.sleep(1.5)
                mode = "static" if static else "dynamic"
                shots[(static, v)] = grab(f"{size}_{mode}_{v}")
        for v in VALUES:
            a, b = shots[(False, v)], shots[(True, v)]
            diff = ImageChops.difference(a, b)
            bbox = diff.getbbox()
            # count pixels differing by more than a hair (>16/255 in any ch)
            tot = sum(1 for px in diff.getdata()
                      if px[0] > 16 or px[1] > 16 or px[2] > 16)
            line = f"size={size} value={v}: diff_px={tot} bbox={bbox}"
            print(line, flush=True)
            report.append(line)
            diff.save(f"par_diff_{size}_{v}.png")

    http_post("/api/signal/clear", {"signal": "RPM"})
    http_post("/api/layout/set", {"name": "Time_Attack"})
    print("restored Time_Attack", flush=True)


if __name__ == "__main__":
    main()
