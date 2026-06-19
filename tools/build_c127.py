#!/usr/bin/env python3
"""Build a MoTeC C127-style factory layout for the RDM-7 dash and push it.

C127 factory aesthetic: black background, full-width horizontal tacho with
shift lights along the very top, a huge central GEAR, large SPEED (left) and
WATER (right) flanking it, and a bottom strip of small labelled numeric tiles.
Adapted to the live channels on this car (MaxxECU).

usage: build_c127.py preview|save [outdir]
"""
import json, sys, time, urllib.request

DASH = "http://192.168.4.61"

# ── RGB565 colour helpers ────────────────────────────────────────────────
def rgb(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

WHITE  = rgb(255, 255, 255)
GREY   = rgb(150, 150, 150)
DIM    = rgb(95, 95, 95)
RED    = rgb(255, 30, 30)
GREEN  = rgb(40, 220, 80)
AMBER  = rgb(255, 180, 20)
CYAN   = rgb(40, 200, 255)
TILEBG = rgb(20, 22, 26)
TILEBD = rgb(55, 60, 68)

BIG   = "Manrope Bold:150"     # gear hero
MED   = "Manrope Bold:78"      # speed / water
TILE  = "Manrope Bold:30"      # tile values
LBL   = "Montserrat:13"        # tile labels

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

def text(wid, x, y, w, h, signal, channel, font, color=WHITE, decimals=0):
    return {"type": "text", "id": wid, "x": x, "y": y, "w": w, "h": h,
            "config": {"signal_name": signal, "channel": channel, "font": font,
                       "text_color": color, "decimals": decimals},
            "signal": signal}

def tile(wid, x, signal, channel, label, decimals=0, color=WHITE):
    """Bottom-strip numeric tile: small grey label over a value. Channel-bound
    so units convert (e.g. boost kPa->bar) and zone thresholds colour it."""
    return {"type": "panel", "id": wid, "x": x, "y": 192, "w": 126, "h": 86,
            "config": {"slot": 8, "label": label, "signal_name": signal,
                       "channel": channel,
                       "value_font": TILE, "value_color": color,
                       "label_color": GREY, "decimals": decimals,
                       "bg_color": TILEBG, "bg_opa": 255,
                       "border_width": 1, "border_color": TILEBD,
                       "value_y_offset": 18},
            "signal": signal}

def hero(wid, x, signal, channel, label, font, decimals=0, color=WHITE):
    """Large flanking readout: label on top, big value below."""
    return {"type": "panel", "id": wid, "x": x, "y": 5, "w": 250, "h": 180,
            "config": {"slot": 8, "label": label, "signal_name": signal,
                       "channel": channel,
                       "value_font": font, "value_color": color,
                       "label_color": GREY, "decimals": decimals,
                       "bg_opa": 0, "border_width": 0,
                       "value_y_offset": 30},
            "signal": signal}

def build():
    widgets = [
        # Shift-light LED strip pinned to the very top edge.
        {"type": "shift_light", "id": "shift_0", "x": 0, "y": -230, "w": 796,
         "h": 16, "config": {"signal_name": "RPM", "led_count": 18,
                             "range_max": 7000, "flash_threshold": 6400,
                             "flash_speed": 70, "color_off": rgb(28, 30, 34),
                             "fill_mode": 1}, "signal": "RPM"},
        # Horizontal tacho bar just below the shift lights, green->amber->red.
        {"type": "rpm_bar", "id": "rpm_0", "x": 0, "y": -190, "w": 784, "h": 52,
         "config": {"signal_name": "RPM", "channel": "rpm",
                    "rpm_max": 7000, "redline": 6400,
                    "bar_color": GREEN, "bar_bg_color": rgb(18, 20, 24),
                    "grad_stops": [{"pos": 0, "color": GREEN},
                                   {"pos": 70, "color": AMBER},
                                   {"pos": 100, "color": RED}],
                    "show_ticks": True, "tick_side": 1, "tick_length": 9,
                    "tick_width": 2, "tick_color": DIM,
                    "show_rpm_value": True, "rpm_value_font": "Manrope Bold:26",
                    "rpm_value_color": WHITE}, "signal": "RPM"},
        # Hero row: SPEED · GEAR · WATER
        hero("speed", -268, "VEHICLE_SPEED", "vehicle_speed", "SPEED  km/h", MED),
        text("gear", 0, 18, 220, 200, "GEAR", "gear", BIG, WHITE),
        hero("water", 268, "COOLANT_TEMP", "coolant_temp", "WATER  C", MED),
        # Faint divider sectioning the bottom tile strip (MoTeC styling).
        {"type": "line", "id": "div_0", "x": 0, "y": 144, "w": 770, "h": 4,
         "config": {"line_color": TILEBD, "line_width": 2, "line_opa": 255,
                    "orientation": 0}},
        # Bottom strip of tiles.
        tile("t_oilp", -325, "OIL_PRESSURE", "oil_pressure", "OIL PRESS  kPa"),
        tile("t_oilt", -195, "OIL_TEMP", "oil_temp", "OIL TEMP  F"),
        tile("t_batt",  -65, "BATTERY_VOLTAGE", "battery_voltage", "BATTERY  V",
             decimals=1),
        tile("t_boost",  65, "MATH_BOOST_PRESSURE", "boost_pressure",
             "BOOST  bar", decimals=2, color=CYAN),
        tile("t_lam",   195, "LAMBDA", "lambda_bank1", "LAMBDA", decimals=2),
        tile("t_iat",   325, "INTAKE_AIR_TEMP", "intake_air_temp", "INTAKE  C"),
    ]
    return {"schema_version": 14, "name": "MoTeC_C127", "screen_w": 800,
            "screen_h": 480, "signals": [], "widgets": widgets}

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "preview"
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    lay = build()
    if mode == "save":
        post("/api/layout/save", lay)
        print("saved layout MoTeC_C127 to device", flush=True)
        post("/api/layout/set", {"name": "MoTeC_C127"})
    else:
        post("/api/layout/preview", lay)
    time.sleep(2.5)
    shot(f"{out}/c127.png")
    print("done", flush=True)

if __name__ == "__main__":
    main()
