"""KTM Duke-style L-shaped RPM bar demo (the `pathbar` widget) — dark/neon.

"Just the bar": a dark background with the 0..11 scale baked along the path, plus
the live `pathbar` widget whose smooth continuous fill follows
the path -- vertical up the left, around a radius, then horizontal across the top.

The SAME centerline is used to (a) emit the widget's `path` points and (b) place
the baked numbers, so the fill and the scale stay aligned.
"""
import math

import numpy as np
from PIL import Image, ImageDraw

from cluster_lib import S, F, rgb565, fill_gradient, cx_to_center, cy_to_center, MANROPE, FUGAZ, MONT

NAME = "lbar"
IMAGE_NAME = "lbar_bg"
W, H = 800, 480
RPM_MAX = 11000
REDLINE = 10000

# Dark theme
BG_TOP = (18, 20, 25)
BG_BOT = (9, 10, 13)
TEAL = (48, 228, 200)        # neon fill
DIM_TRACK = (46, 50, 58)     # empty track
ORANGE = (255, 92, 50)       # redline
NUM = (206, 211, 218)
NUM_RED = (255, 120, 74)
WHITE = (238, 241, 245)
GREY = (120, 126, 135)

STATES = {
    "idle": {"RPM": 1100, "VEHICLE_SPEED": 0, "GEAR": 0},
    "drive": {"RPM": 6200, "VEHICLE_SPEED": 96, "GEAR": 3},
    "redline": {"RPM": 10600, "VEHICLE_SPEED": 152, "GEAR": 5},
}
DEFAULT_STATE = "drive"


# ---------------------------------------------------------------- the path
def centerline():
    """Vertical (bottom->top) -> rounded corner -> horizontal. Screen px."""
    pts = []
    for t in np.linspace(0, 1, 42):
        pts.append((158.0, 392 + (168 - 392) * t))
    cx, cy, r = 228.0, 168.0, 70.0
    for a in np.linspace(180, 270, 30):
        ar = math.radians(a)
        pts.append((cx + r * math.cos(ar), cy + r * math.sin(ar)))
    for t in np.linspace(0, 1, 92):
        pts.append((228 + (760 - 228) * t, 98.0))
    out = [pts[0]]
    for p in pts[1:]:
        if abs(p[0] - out[-1][0]) > 0.4 or abs(p[1] - out[-1][1]) > 0.4:
            out.append(p)
    return out


def _arclen(pts):
    d = [0.0]
    for a, b in zip(pts, pts[1:]):
        d.append(d[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    return d


def at_fraction(pts, cum, f):
    target = f * cum[-1]
    i = int(np.searchsorted(cum, target))
    i = max(1, min(i, len(pts) - 1))
    p0, p1 = pts[i - 1], pts[i]
    seg = cum[i] - cum[i - 1]
    u = 0.0 if seg <= 0 else (target - cum[i - 1]) / seg
    px = p0[0] + (p1[0] - p0[0]) * u
    py = p0[1] + (p1[1] - p0[1]) * u
    tx, ty = p1[0] - p0[0], p1[1] - p0[1]
    n = math.hypot(tx, ty) or 1.0
    return (px, py), (tx / n, ty / n)


# ---------------------------------------------------------------- background
def build_background():
    base = Image.new("RGBA", (W * 3, H * 3), BG_BOT + (255,))
    base = Image.alpha_composite(base, fill_gradient((W * 3, H * 3), BG_TOP, BG_BOT))
    d = ImageDraw.Draw(base)
    pts = centerline()
    cum = _arclen(pts)
    fn = F(MANROPE, 22)
    for k in range(0, 12):
        f = k / 11.0
        (px, py), (tx, ty) = at_fraction(pts, cum, f)
        nx, ny = -ty, tx
        lx, ly = px - nx * 26, py - ny * 26
        d.text((S(lx), S(ly)), str(k), font=fn, fill=(NUM_RED if k >= 10 else NUM), anchor="mm")
    d.text((S(600), S(160)), "STREET", font=F(MONT, 18), fill=WHITE, anchor="lm")
    d.text((S(600), S(182)), "RPM x1000", font=F(MONT, 14), fill=GREY, anchor="lm")
    d.rounded_rectangle([S(470), S(150), S(556), S(236)], radius=S(8), outline=TEAL, width=max(1, S(3)))
    d.text((S(600), S(420)), "KM/H", font=F(MONT, 15), fill=GREY, anchor="lm")
    return base


# ---------------------------------------------------------------- layout
def _bbox(pts, margin):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    return (min(xs) - margin, min(ys) - margin, max(xs) + margin, max(ys) + margin)


# True = let the FIRMWARE generate the L from shape+corner_radius fit to the box
# (the editor-authoring path); False = ship the explicit point array (tooling).
PARAMETRIC = True


def build_layout():
    band = 26
    if PARAMETRIC:
        # Box derived so the firmware L (inset = band/2+2) lands on the SAME
        # geometry as the baked numbers: vertical x=158, horizontal y=98, r=70.
        inset = band / 2 + 2
        bx = 158 - inset           # left leg x  - inset
        by = 98 - inset            # top leg y   - inset
        bw = 760 - 158 + 2 * inset
        bh = 392 - 98 + 2 * inset
        cx = bx + bw / 2.0
        cy = by + bh / 2.0
        pathbar_cfg = {"signal_name": "RPM", "min": 0, "max": RPM_MAX,
                       "redline": REDLINE, "band_width": band,
                       "rounded": True, "shape": 1, "orientation": 0, "corner_radius": 70,
                       "dim_color": rgb565(DIM_TRACK), "lit_color": rgb565(TEAL),
                       "redline_color": rgb565(ORANGE), "dim_opa": 120, "smoothing_ms": 90}
    else:
        pts = centerline()
        x0, y0, x1, y1 = _bbox(pts, band / 2 + 8)
        bw, bh = x1 - x0, y1 - y0
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        flat = []
        for (px, py) in pts:
            flat.append(round(px, 1)); flat.append(round(py, 1))
        pathbar_cfg = {"signal_name": "RPM", "min": 0, "max": RPM_MAX,
                       "redline": REDLINE, "band_width": band,
                       "rounded": True, "dim_color": rgb565(DIM_TRACK),
                       "lit_color": rgb565(TEAL), "redline_color": rgb565(ORANGE),
                       "dim_opa": 120, "smoothing_ms": 90, "path": flat}
    return {
        "schema_version": 14,
        "name": "LBar",
        "screen_w": 800,
        "screen_h": 480,
        "signals": [
            {"name": "RPM", "can_id": 256, "bit_start": 0, "bit_length": 16,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "rpm"},
            {"name": "VEHICLE_SPEED", "can_id": 256, "bit_start": 16, "bit_length": 16,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "km/h"},
            {"name": "GEAR", "can_id": 257, "bit_start": 0, "bit_length": 8,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "",
             "value_map": [{"v": 0, "label": "N"}]},
        ],
        "widgets": [
            {"type": "image", "id": "bg", "x": 0, "y": 0, "w": 800, "h": 480,
             "config": {"image_name": IMAGE_NAME, "auto_size": True}},
            # THE BAR: smooth continuous fill following the L-path.
            {"type": "pathbar", "id": "rpm_path",
             "x": cx_to_center(cx), "y": cy_to_center(cy),
             "w": int(round(bw)), "h": int(round(bh)),
             "config": pathbar_cfg},
            {"type": "text", "id": "gear",
             "x": cx_to_center(513), "y": cy_to_center(193), "w": 80, "h": 80,
             "config": {"slot": 0, "signal_name": "GEAR", "decimals": 0,
                        "static_text": "", "font": "Fugaz One:60",
                        "text_color": rgb565(WHITE)}},
            {"type": "text", "id": "speed",
             "x": cx_to_center(690), "y": cy_to_center(395), "w": 180, "h": 60,
             "config": {"slot": 0, "signal_name": "VEHICLE_SPEED", "decimals": 0,
                        "static_text": "", "font": "Manrope Bold:44",
                        "text_color": rgb565(WHITE)}},
        ],
    }
