#!/usr/bin/env python3
"""Build a MoTeC C127-style ARC (round tacho) factory layout and push it.

Big 270-degree circular RPM tacho centred on screen with tick numbers 0-7,
a huge GEAR in the middle, SPEED below it, a shift-light strip across the top,
and four corner numeric readouts. Adapted to the live MaxxECU channels.

usage: build_c127_arc.py preview|save [outdir]
"""
import json, sys, time, urllib.request

DASH = "http://192.168.4.61"

def rgb(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

WHITE  = rgb(255, 255, 255)
GREY   = rgb(150, 150, 150)
DIM    = rgb(90, 95, 105)
DARK   = rgb(30, 33, 38)
RED    = rgb(255, 30, 30)
GREEN  = rgb(40, 220, 80)
AMBER  = rgb(255, 180, 20)
CYAN   = rgb(45, 200, 255)
TILEBG = rgb(20, 22, 26)
TILEBD = rgb(55, 60, 68)

GEARF = "Manrope Bold:138"
SPDF  = "Manrope Bold:60"
TILEF = "Manrope Bold:30"
TICKF = "Manrope Bold:22"

def post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()

def shot(name):
    with urllib.request.urlopen(DASH + "/api/screenshot", timeout=15) as r:
        png = r.read()
    with open(name, "wb") as f:
        f.write(png)
    print(f"saved {name}", flush=True)

def tile(wid, x, y, signal, channel, label, decimals=0, color=WHITE):
    return {"type": "panel", "id": wid, "x": x, "y": y, "w": 150, "h": 88,
            "config": {"slot": 8, "label": label, "signal_name": signal,
                       "channel": channel, "value_font": TILEF,
                       "value_color": color, "label_color": GREY,
                       "decimals": decimals, "bg_color": TILEBG, "bg_opa": 255,
                       "border_width": 1, "border_color": TILEBD,
                       "value_y_offset": 18},
            "signal": signal}

def build():
    widgets = [
        # Shift-light LED strip pinned to the very top edge.
        {"type": "shift_light", "id": "shift_0", "x": 0, "y": -230, "w": 796,
         "h": 15, "config": {"signal_name": "RPM", "led_count": 18,
                             "range_max": 7000, "flash_threshold": 6400,
                             "flash_speed": 70, "color_off": rgb(28, 30, 34),
                             "fill_mode": 1}, "signal": "RPM"},
        # Big 270-degree circular tacho, centred.
        {"type": "arc", "id": "tacho", "x": 0, "y": 16, "w": 438, "h": 438,
         "config": {"signal_name": "RPM", "channel": "rpm",
                    "signal_min": 0, "signal_max": 7000,
                    "start_angle": 135, "end_angle": 45,
                    "arc_width": 20, "bg_arc_width": 20,
                    "arc_color": WHITE, "bg_arc_color": DARK,
                    "rounded_ends": False,
                    "show_ticks": True, "minor_tick_step": 250,
                    "major_tick_step": 1000, "minor_tick_length": 9,
                    "minor_tick_width": 2, "major_tick_length": 18,
                    "major_tick_width": 3, "minor_tick_color": DIM,
                    "major_tick_color": GREY,
                    "show_tick_labels": True, "tick_label_divisor": 1000,
                    "tick_label_font": TICKF, "tick_label_color": WHITE,
                    "label_gap": 6,
                    "redline_enabled": True, "redline_threshold": 6400,
                    "redline_color": RED, "redline_arc_width": 20,
                    "redline_recolor_fill": True,
                    "show_value_line": True, "value_line_width": 4,
                    "value_line_color": CYAN, "value_line_r_mod": 0},
         "signal": "RPM"},
        # Hero GEAR dead-centre of the arc.
        {"type": "text", "id": "gear", "x": 0, "y": -6, "w": 200, "h": 180,
         "config": {"signal_name": "GEAR", "channel": "gear", "font": GEARF,
                    "text_color": WHITE, "decimals": 0}, "signal": "GEAR"},
        # SPEED below the gear, inside the arc.
        {"type": "panel", "id": "speed", "x": 0, "y": 96, "w": 240, "h": 86,
         "config": {"slot": 8, "label": "SPEED  km/h", "signal_name":
                    "VEHICLE_SPEED", "channel": "vehicle_speed",
                    "value_font": SPDF, "value_color": WHITE,
                    "label_color": GREY, "decimals": 0, "bg_opa": 0,
                    "border_width": 0, "value_y_offset": 24},
         "signal": "VEHICLE_SPEED"},
        # Four corner readouts.
        tile("c_water", -312, -150, "COOLANT_TEMP", "coolant_temp", "WATER  C"),
        tile("c_oilp",   312, -150, "OIL_PRESSURE", "oil_pressure", "OIL PRESS"),
        tile("c_boost", -312,  158, "MATH_BOOST_PRESSURE", "boost_pressure",
             "BOOST  bar", decimals=2, color=CYAN),
        tile("c_batt",   312,  158, "BATTERY_VOLTAGE", "battery_voltage",
             "BATTERY  V", decimals=1),
    ]
    return {"schema_version": 14, "name": "MoTeC_C127_Arc", "screen_w": 800,
            "screen_h": 480, "signals": [], "widgets": widgets}

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "preview"
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    # Arc redline is CHANNEL-owned (ADR-0005): the widget reads it from the
    # bound channel's high_warn, not the layout config. Set the rpm channel's
    # high_warn so the redline zone + fill-recolor render. (This is what the
    # web editor does automatically when you set a redline on a channel arc.)
    post("/api/channels/update", {"id": "rpm", "fields": {"high_warn": 6400}})
    time.sleep(0.3)
    lay = build()
    if mode == "save":
        post("/api/layout/save", lay)
        post("/api/layout/set", {"name": "MoTeC_C127_Arc"})
        print("saved + active: MoTeC_C127_Arc", flush=True)
    else:
        post("/api/layout/preview", lay)
    time.sleep(2.5)
    shot(f"{out}/c127_arc.png")
    print("done", flush=True)

if __name__ == "__main__":
    main()
