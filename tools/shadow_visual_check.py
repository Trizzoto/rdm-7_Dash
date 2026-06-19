#!/usr/bin/env python3
"""Capture fixed-needle screenshots of shadow/ball/redline meter scenes.

Run with arg `ref` before flashing (saves sv_ref_*.png) and `post` after
(saves sv_post_*.png + prints pixel diff vs ref). Restores Time_Attack.
"""
import json, sys, time, copy, io, urllib.request
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

SHADOW_CIRCLE = {
    "type": "shape_panel", "id": "ovl", "x": 0, "y": 0, "w": 250, "h": 250,
    "config": {"shape_type": "circle", "bg_color": 0, "shadow_width": 30,
               "shadow_color": 10565, "shadow_opa": 150, "shadow_ofs_x": -2,
               "shadow_ofs_y": 5},
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


def scene(tag):
    cfg = copy.deepcopy(BASE_CFG)
    widgets = []
    if tag == "shadow":
        widgets.append(SHADOW_CIRCLE)
    elif tag == "ball_redline":
        cfg["show_needle_ball"] = True
        cfg["needle_ball_size"] = 22
        cfg["redline_enabled"] = True
    m = {"type": "meter", "id": "RPM", "x": 0, "y": 0, "w": 450, "h": 450,
         "config": cfg, "signal": "RPM"}
    return {"schema_version": 14, "name": "SV", "screen_w": 800,
            "screen_h": 480, "signals": [], "widgets": [m] + widgets}


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "ref"
    for tag in ("shadow", "ball_redline"):
        http_post("/api/layout/preview", scene(tag))
        time.sleep(2)
        http_post("/api/signal/inject", {"signal": "RPM", "value": 3500})
        time.sleep(1.5)
        # second tap so any shadow-cache hit path is what's on screen
        http_post("/api/signal/inject", {"signal": "RPM", "value": 3600})
        time.sleep(1.5)
        data = http_get("/api/screenshot")
        fn = f"sv_{mode}_{tag}.png"
        open(fn, "wb").write(data)
        print(f"saved {fn}", flush=True)
        if mode == "post":
            try:
                a = Image.open(f"sv_ref_{tag}.png").convert("RGB")
                b = Image.open(io.BytesIO(data)).convert("RGB")
                diff = ImageChops.difference(a, b)
                tot = sum(1 for px in diff.getdata()
                          if px[0] > 16 or px[1] > 16 or px[2] > 16)
                print(f"  {tag}: diff_px vs ref = {tot} bbox={diff.getbbox()}",
                      flush=True)
                diff.save(f"sv_diff_{tag}.png")
            except FileNotFoundError:
                print(f"  {tag}: no ref image", flush=True)

    http_post("/api/signal/clear", {"signal": "RPM"})
    http_post("/api/layout/set", {"name": "Time_Attack"})
    print("restored Time_Attack", flush=True)


if __name__ == "__main__":
    main()
