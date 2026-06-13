#!/usr/bin/env python3
"""Recreate the MoTeC C127 'RACE' factory display as closely as possible.

Reference: orange-fill thick ring tacho (numbers 0-6, red redline near 6),
dark centre disc with a RACE badge / huge GEAR / RPMx1000, a left column
(ENGINE/GBOX/DIFF OIL TMP) and right column (WATER TMP / OIL PRESS /
FUEL PRESS) with orange labels + big white values, and a bottom lap-timing
strip (REFERENCE LAP / GAIN-LOSS / RUNNING LAP + gain/loss bar).

usage: build_c127_race.py preview|save|demo [outdir]
  demo = preview + inject representative values + screenshot (matches the ref)
"""
import json, sys, time, urllib.request

DASH = "http://192.168.4.61"

def rgb(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

ORANGE = rgb(255, 132, 0)      # MoTeC label / fill accent
WHITE  = rgb(255, 255, 255)
SILVER = rgb(216, 216, 216)    # unfilled tacho track
GREY   = rgb(150, 150, 150)
DIMTXT = rgb(120, 120, 120)
RED    = rgb(255, 40, 40)
GREEN  = rgb(45, 210, 70)
DARK   = rgb(14, 15, 17)       # centre disc
RING   = rgb(70, 74, 82)       # disc border ring
TRACK  = rgb(40, 43, 48)       # gain/loss bar track

GEARF  = "Manrope Bold:116"
SIDEV  = "Manrope Bold:44"
SIDEL  = "Montserrat:15"
LAPV   = "Manrope Bold:38"
LAPL   = "Montserrat:15"
RACEF  = "Manrope Bold:20"
SUBF   = "Montserrat:13"
TICKF  = "Manrope Bold:24"

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

def readout(wid, x, y, signal, channel, label, align, decimals=0):
    """Side column readout: small orange label over a big white value.
    align: 0=left (left column), 2=right (right column)."""
    return {"type": "panel", "id": wid, "x": x, "y": y, "w": 230, "h": 78,
            "config": {"slot": 8, "label": label, "signal_name": signal,
                       "channel": channel, "value_font": SIDEV, "label_font": SIDEL,
                       "value_color": WHITE, "label_color": ORANGE,
                       "decimals": decimals, "bg_opa": 0, "border_width": 0,
                       "text_align": align,
                       "label_y_offset": -26, "value_y_offset": 12},
            "signal": signal}

def stxt(wid, x, y, w, h, txt, font, color):
    return {"type": "text", "id": wid, "x": x, "y": y, "w": w, "h": h,
            "config": {"static_text": txt, "font": font, "text_color": color}}

def build():
    widgets = [
        # ── Thick ring tacho: orange fill over a silver track, red redline.
        # Ticks/labels live in the dark gap between the ring inner edge and the
        # centre disc (the overlay is z-behind the fill, so anything inside the
        # ring band is occluded). Thin-ish ring + long ticks that poke past the
        # inner edge + label_gap that seats numbers in the gap. ──
        {"type": "arc", "id": "tacho", "x": 0, "y": -36, "w": 352, "h": 352,
         "config": {"signal_name": "RPM", "channel": "rpm",
                    "signal_min": 0, "signal_max": 7000,
                    "start_angle": 135, "end_angle": 45,
                    "arc_width": 42, "bg_arc_width": 42,
                    "arc_color": ORANGE, "bg_arc_color": SILVER,
                    "rounded_ends": False,
                    # Ticks + numbers drawn ON TOP of the ring (MoTeC look):
                    # marks span the ring, numbers seat just inside it.
                    "ticks_on_top": True,
                    "show_ticks": True, "minor_tick_step": 250,
                    "major_tick_step": 1000, "minor_tick_length": 14,
                    "minor_tick_width": 2, "major_tick_length": 22,
                    "major_tick_width": 3, "minor_tick_color": rgb(60,62,66),
                    "major_tick_color": rgb(40,42,46),
                    "show_tick_labels": True, "tick_label_divisor": 1000,
                    "tick_label_font": TICKF, "tick_label_color": WHITE,
                    "label_gap": 34,
                    "redline_enabled": True, "redline_threshold": 6400,
                    "redline_color": RED, "redline_arc_width": 42},
         "signal": "RPM"},
        # ── Centre disc (circle) ──
        {"type": "shape_panel", "id": "disc", "x": 0, "y": -36, "w": 186,
         "h": 186, "config": {"shape_type": "circle", "bg_color": DARK,
                             "bg_opa": 255, "border_color": RING,
                             "border_width": 3}},
        # RACE badge pill.
        {"type": "shape_panel", "id": "racepill", "x": 0, "y": -84, "w": 78,
         "h": 26, "config": {"shape_type": "rectangle", "bg_color": rgb(34,36,40),
                            "bg_opa": 255, "border_width": 0,
                            "border_radius": 13}},
        stxt("race", 0, -85, 78, 26, "RACE", RACEF, WHITE),
        {"type": "text", "id": "gear", "x": 0, "y": -32, "w": 170, "h": 150,
         "config": {"signal_name": "GEAR", "channel": "gear", "font": GEARF,
                    "text_color": WHITE, "decimals": 0}, "signal": "GEAR"},
        stxt("rpmlbl", 0, 34, 130, 24, "RPMx1000", SUBF, GREY),
        # ── Left column (left-aligned) ──
        readout("l1", -282, -150, "OIL_TEMP", "oil_temp", "ENGINE OIL TMP", 0),
        readout("l2", -282, -55, "TRANSMISSION_TEMP", "transmission_temp", "GBOX OIL TMP", 0),
        readout("l3", -282, 40, "DIFFERENTIAL_TEMP", "differential_temp", "DIFF OIL TMP", 0),
        # ── Right column (right-aligned) ──
        readout("r1", 282, -150, "COOLANT_TEMP", "coolant_temp", "WATER TMP", 2),
        readout("r2", 282, -55, "OIL_PRESSURE", "oil_pressure", "OIL PRESS", 2),
        readout("r3", 282, 40, "FUEL_PRESSURE", "fuel_pressure", "FUEL PRESS", 2),
        # ── Bottom lap-timing strip ──
        {"type": "line", "id": "lapdiv", "x": 0, "y": 100, "w": 780, "h": 4,
         "config": {"line_color": RING, "line_width": 2, "orientation": 0}},
        stxt("ref_l", -250, 122, 240, 22, "REFERENCE LAP", LAPL, ORANGE),
        stxt("ref_v", -250, 150, 240, 44, "2:24.50", LAPV, WHITE),
        stxt("gl_l", 0, 122, 200, 22, "GAIN/LOSS", LAPL, ORANGE),
        stxt("gl_v", 0, 150, 200, 44, "0.21", LAPV, WHITE),
        stxt("run_l", 250, 122, 240, 22, "RUNNING LAP", LAPL, ORANGE),
        stxt("run_v", 250, 150, 240, 44, "2:24.3", LAPV, WHITE),
        # gain/loss bar
        {"type": "shape_panel", "id": "glbar", "x": 0, "y": 212, "w": 600,
         "h": 12, "config": {"shape_type": 0, "bg_color": TRACK, "bg_opa": 255,
                            "border_width": 0, "border_radius": 6}},
        {"type": "shape_panel", "id": "glmark", "x": -70, "y": 212, "w": 26,
         "h": 12, "config": {"shape_type": 0, "bg_color": GREEN, "bg_opa": 255,
                            "border_width": 0, "border_radius": 6}},
        stxt("loss_l", -330, 212, 70, 20, "LOSS", SUBF, GREY),
        stxt("gain_l", 330, 212, 70, 20, "GAIN", SUBF, GREY),
    ]
    return {"schema_version": 14, "name": "MoTeC_C127_Race", "screen_w": 800,
            "screen_h": 480, "signals": [], "widgets": widgets}

DEMO = {"RPM": 3200, "GEAR": 5, "OIL_TEMP": 110, "TRANSMISSION_TEMP": 92,
        "DIFFERENTIAL_TEMP": 113, "COOLANT_TEMP": 95, "OIL_PRESSURE": 482}

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "preview"
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    post("/api/channels/update", {"id": "rpm", "fields": {"high_warn": 6400}})
    # Engine oil temp ships in degF; show degC to match the gearbox/diff temps
    # (and the MoTeC reference, which reads 110 degC).
    post("/api/channels/update", {"id": "oil_temp", "fields": {"units_display": "°C"}})
    time.sleep(0.3)
    lay = build()
    if mode == "save":
        post("/api/layout/save", lay)
        post("/api/layout/set", {"name": "MoTeC_C127_Race"})
        print("saved + active: MoTeC_C127_Race", flush=True)
        time.sleep(2.5)
    else:
        post("/api/layout/preview", lay)
        time.sleep(2.5)
        if mode == "demo":
            post("/api/signal/inject",
                 {"values": [{"signal": s, "value": v} for s, v in DEMO.items()]})
            time.sleep(1.5)
    shot(f"{out}/c127_race.png")
    print("done", flush=True)

if __name__ == "__main__":
    main()
