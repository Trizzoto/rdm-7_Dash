"""Generate a 'fast' Altezza variant: solid dark dials + native amber ticks
(no background image), keeping the user's current meter geometry. This is the
high-fps path (~45-50fps) — the dial faces are cheap so needles sweep near-free.
Run: python tools/altezza/gen_native.py  -> altezza_native_polished.json
"""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "altezza_noshadow.json"
OUT = HERE / "altezza_native_polished.json"


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

AMBER = rgb565(255, 150, 20)
CREAM = rgb565(255, 232, 196)
RED = rgb565(255, 60, 24)
FACE = rgb565(9, 9, 11)
NEEDLE = rgb565(255, 94, 42)

# per-signal native scale config: (minor_step, major_step, divisor, redline_or_None)
SCALE = {
    "RPM":           (500, 1000, 1000, 7500),
    "VEHICLE_SPEED": (10,  20,   1,    None),
    "FUEL_LEVEL":    (25,  50,   1,    None),
    "OIL_PRESS":     (1,   5,    1,    None),
    "BATT_VOLT":     (1,   5,    1,    None),
    "COOLANT_TEMP":  (10,  40,   1,    None),
}


def native_cfg(c):
    sig = c.get("signal_name", "")
    vmin = c.get("min", 0); vmax = c.get("max", 100)
    minor, major, div, redline = SCALE.get(sig, (max(1, (vmax - vmin) / 10), max(1, (vmax - vmin) / 5), 1, None))
    rng = max(1e-6, vmax - vmin)
    minor_count = int(round(rng / minor)) + 1
    minor_count = max(2, min(100, minor_count))
    major_every = max(1, min(50, int(round(major / minor))))
    big = c.get("w", 100) >= 200
    c.update({
        "meter_bg_opa": 255, "meter_bg_color": FACE, "border_width": 0,
        "show_ticks": True, "show_tick_labels": True,
        "minor_tick_count": minor_count, "major_tick_every": major_every,
        "minor_tick_width": 2 if big else 1, "minor_tick_length": 9 if big else 5,
        "major_tick_width": 4 if big else 2, "major_tick_length": 16 if big else 9,
        "minor_tick_color": AMBER, "major_tick_color": AMBER,
        "tick_label_color": CREAM, "tick_label_divisor": div,
        "tick_label_font": "manrope_35_bold" if big else "fugaz_17",
        "label_gap": 14 if big else 7, "scale_padding": 8 if big else 4,
        "needle_color": NEEDLE,
        "static_ticks": True,   # snapshot the tick layer; needle redraw stays cheap over solid face
    })
    if redline is not None:
        c.update({"redline_enabled": True, "redline_threshold": redline,
                  "redline_color": RED, "redline_recolor_ticks": True, "redline_show_arc": True,
                  "redline_arc_width": 5})
    return c


def main():
    layout = json.loads(SRC.read_text(encoding="utf-8-sig"))
    widgets = []
    for w in layout["widgets"]:
        if w["type"] == "image" and w.get("id") == "bg":
            continue  # drop the full-screen background
        if w["type"] == "meter":
            w["config"] = native_cfg(w["config"])
        widgets.append(w)
    layout["widgets"] = widgets
    OUT.write_text(json.dumps(layout))
    print("wrote", OUT, "-", len(widgets), "widgets")


if __name__ == "__main__":
    main()
