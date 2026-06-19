#!/usr/bin/env python3
"""Isolated meter+overlay benchmark on the dash.

Minimal layout (one 450x450 RPM meter + FPS text), RPM test-locked and driven
by the on-device signal simulator (deterministic triangle sweep), overlay and
static_ticks varied per run. Restores Time_Attack and clears locks at the end.
"""
import json, sys, time, copy, urllib.request

DASH = "http://192.168.4.61"
SETTLE_S = 5
SAMPLES = 15
SAMPLE_GAP_S = 1.0

METER = {
    "type": "meter", "id": "RPM", "x": 0, "y": 0, "w": 450, "h": 450,
    "config": {
        "slot": 0, "min": 0, "max": 7000, "start_angle": 90, "end_angle": 0,
        "signal_name": "RPM", "minor_tick_count": 36, "show_needle_ball": False,
        "needle_width": 7, "needle_color": 63488, "needle_rear_length": 29,
        "needle_tip_style": 2, "needle_ball_size": 22, "shadow_dynamic": False,
        "meter_bg_opa": 0, "label_gap": 22, "tick_label_font": "Manrope Bold:25",
        "tick_label_divisor": 1000, "redline_threshold": 6000,
    },
    "signal": "RPM",
}

FPS_TEXT = {
    "type": "text", "id": "FPS", "x": 350, "y": 220, "w": 80, "h": 30,
    "config": {"slot": 0, "decimals": 0, "signal_name": "FPS"}, "signal": "FPS",
}

CIRCLE = {
    "type": "shape_panel", "id": "ovl", "x": 0, "y": 0, "w": 250, "h": 250,
    "config": {"shape_type": "circle", "bg_color": 0},
}

CIRCLE_SHADOW = {
    "type": "shape_panel", "id": "ovl", "x": 0, "y": 0, "w": 250, "h": 250,
    "config": {"shape_type": "circle", "bg_color": 0, "shadow_width": 30,
               "shadow_color": 10565, "shadow_opa": 150, "shadow_ofs_x": -2,
               "shadow_ofs_y": 5},
}

RECT_ALPHA = {
    "type": "shape_panel", "id": "ovl", "x": 0, "y": 0, "w": 250, "h": 250,
    "config": {"bg_color": 0, "bg_opa": 140, "border_radius": 20},
}

OVERLAYS = {
    "bare": None,
    "circle": CIRCLE,
    "circle_shadow": CIRCLE_SHADOW,
    "rect_alpha": RECT_ALPHA,
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


def build_layout(overlay, static_ticks):
    meter = copy.deepcopy(METER)
    if static_ticks:
        meter["config"]["static_ticks"] = True
    widgets = [meter]
    if overlay:
        widgets.append(copy.deepcopy(overlay))
    widgets.append(FPS_TEXT)
    return {"schema_version": 14, "name": "PERF_TEST", "screen_w": 800,
            "screen_h": 480, "signals": [], "widgets": widgets}


def measure(tag):
    time.sleep(SETTLE_S)
    fps, rend, px, flush, maxpct = [], [], [], [], []
    for _ in range(SAMPLES):
        p = json.loads(http_get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"])
        px.append(p["avg_px"]); flush.append(p["flush_us_per_frame"])
        maxpct.append(p["max_pct_screen"])
        time.sleep(SAMPLE_GAP_S)
    out = {"tag": tag,
           "fps_avg": round(sum(fps) / len(fps), 1),
           "fps_min": min(fps), "fps_max": max(fps),
           "render_ms_avg": round(sum(rend) / len(rend), 1),
           "render_ms_min": min(rend), "render_ms_max": max(rend),
           "avg_px": round(sum(px) / len(px)),
           "flush_us_avg": round(sum(flush) / len(flush))}
    print(json.dumps(out), flush=True)
    return out


def main():
    results = []
    for st in (False, True):
        for name, ovl in OVERLAYS.items():
            tag = f"{name}_{'static' if st else 'dynamic'}"
            print(f"--- {tag}", flush=True)
            http_post("/api/layout/preview", build_layout(ovl, st))
            time.sleep(2)
            # lock RPM against live CAN (layout reload released prior lock)
            http_post("/api/signal/inject", {"signal": "RPM", "value": 3500})
            # restart sim so it rescans widget bounds for this layout
            http_post("/api/signal/simulate", {"enabled": False})
            time.sleep(0.5)
            http_post("/api/signal/simulate", {"enabled": True})
            r = measure(tag)
            data = http_get("/api/screenshot")
            fn = f"shot_iso_{tag}.png"
            open(fn, "wb").write(data)
            results.append(r)

    http_post("/api/signal/simulate", {"enabled": False})
    http_post("/api/signal/clear", {"signal": "RPM"})
    http_post("/api/layout/set", {"name": "Time_Attack"})
    print("restored Time_Attack, sim off, lock cleared", flush=True)
    print(json.dumps(results, indent=1), flush=True)


if __name__ == "__main__":
    main()
