"""KTM-style 'demon' instrument cluster.

Baked static art + live overlays: an edges-in RPM bar (rpm_bar fill_dir=3), the
big centre GEAR character (value-map 0->N), and the SPEED readout. Scale geometry
matches the live rpm_bar so the baked numbers line up with the fill.
"""
from PIL import Image, ImageDraw, ImageFilter

from cluster_lib import (
    SS, S, F, text, rrect, save_rdmimg, rgb565, MANROPE, FUGAZ, MONT,
)

NAME = "ktm"
IMAGE_NAME = "ktm_bg"
W, H = 800, 480

# ---------------------------------------------------------------- palette (RGB888)
BG_TOP   = (20, 21, 24)
BG_BOT   = (8, 8, 10)
PANEL    = (24, 26, 30)
PANEL_HI = (40, 43, 49)
BORDER   = (54, 58, 65)
WHITE    = (236, 238, 240)
GREY     = (150, 154, 161)
DIM      = (78, 82, 89)
ORANGE   = (255, 106, 0)
ORANGE_BR = (255, 140, 32)
GREEN    = (170, 226, 52)

# ---------------------------------------------------------------- rpm scale geometry
RPM_MAX = 10000
BAR_PIL_L, BAR_PIL_R, BAR_PIL_C = 20, 780, 400
NUM_Y   = 40
TICK_Y0 = 52
TICK_Y1 = 62

STATES = {
    "idle": {"GEAR": 0, "RPM": 4200, "VEHICLE_SPEED": 0},
    "driving": {"GEAR": 3, "RPM": 7400, "VEHICLE_SPEED": 124},
}
DEFAULT_STATE = "driving"


def rpm_x(v, left):
    f = v / RPM_MAX
    return (BAR_PIL_L + f * (BAR_PIL_C - BAR_PIL_L)) if left else (BAR_PIL_R - f * (BAR_PIL_R - BAR_PIL_C))


def make_bg():
    grad = Image.new("RGBA", (1, H * SS))
    for y in range(H * SS):
        t = y / (H * SS)
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        grad.putpixel((0, y), c + (255,))
    base = grad.resize((W * SS, H * SS))
    glow = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([S(150), S(60), S(650), S(440)], fill=(46, 49, 56, 70))
    glow = glow.filter(ImageFilter.GaussianBlur(S(80)))
    return Image.alpha_composite(base, glow)


def draw_header(d):
    rrect(d, (18, 12, 96, 38), 6, outline=GREY, width=2)
    text(d, (57, 25.5), "MENU", F(MANROPE, 14), GREY, anchor="mm")
    text(d, (116, 25), "17:00", F(MANROPE, 18), WHITE, anchor="lm")
    text(d, (188, 25), "21°C", F(MANROPE, 16), GREY, anchor="lm")


def draw_rpm_scale(d):
    fnum = F(MANROPE, 15)
    for left in (True, False):
        for k in range(1, 10):
            v = k * 1000
            x = rpm_x(v, left)
            red = k == 9
            col = ORANGE if red else GREY
            text(d, (x, NUM_Y), str(k), fnum, col, anchor="mm")
            d.line([(S(x), S(TICK_Y0)), (S(x), S(TICK_Y1))],
                   fill=(ORANGE if red else DIM), width=max(1, S(2)))
            xh = rpm_x(v + 500, left)
            d.line([(S(xh), S(TICK_Y0 + 4)), (S(xh), S(TICK_Y1))],
                   fill=DIM, width=max(1, S(1)))
    text(d, (BAR_PIL_C, NUM_Y), "10", fnum, ORANGE, anchor="mm")
    d.line([(S(BAR_PIL_C), S(TICK_Y0)), (S(BAR_PIL_C), S(TICK_Y1))],
           fill=ORANGE, width=max(1, S(2)))
    bx = rpm_x(8400, True)
    rrect(d, (bx, NUM_Y - 14, BAR_PIL_R - (bx - BAR_PIL_L), TICK_Y1 + 2), 5,
          outline=ORANGE, width=2)
    text(d, (765, 80), "x1000", F(MONT, 11), GREY, anchor="rm")


def draw_left_cluster(d):
    cx, cy, r = 150, 150, 15
    d.arc([S(cx - r), S(cy - r), S(cx + r), S(cy + r)], 30, 320, fill=GREY, width=max(1, S(2)))
    d.ellipse([S(cx + r - 5), S(cy - 4), S(cx + r + 1), S(cy + 2)], fill=ORANGE)
    text(d, (118, 200), "1", F(FUGAZ, 78), WHITE, anchor="mm")
    px, py = 92, 252
    rrect(d, (px, py - 9, px + 13, py + 9), 2, outline=GREY, width=2)
    d.line([(S(px + 13), S(py - 4)), (S(px + 17), S(py - 4)), (S(px + 17), S(py + 6))],
           fill=GREY, width=max(1, S(2)))
    text(d, (px + 26, py), "300 km", F(MANROPE, 18), WHITE, anchor="lm")


def draw_tiles(d):
    rrect(d, (92, 286, 232, 346), 8, fill=PANEL, outline=BORDER, width=2)
    wx, wy = 118, 322
    d.ellipse([S(wx - 9), S(wy + 2), S(wx + 1), S(wy + 12)], outline=WHITE, width=max(1, S(2)))
    d.ellipse([S(wx + 12), S(wy - 4), S(wx + 22), S(wy + 6)], outline=WHITE, width=max(1, S(2)))
    d.line([(S(wx - 4), S(wy + 7)), (S(wx + 8), S(wy - 6)), (S(wx + 17), S(wy + 1))],
           fill=WHITE, width=max(1, S(2)))
    text(d, (150, 305), "AW", F(MONT, 12), GREY, anchor="lm")
    text(d, (150, 326), "V.HIGH", F(MANROPE, 17), WHITE, anchor="lm")

    rrect(d, (242, 286, 352, 346), 8, fill=PANEL, outline=BORDER, width=2)
    for i in range(3):
        cx = 262 + i * 11
        d.ellipse([S(cx - 3), S(303), S(cx + 3), S(309)], fill=ORANGE if i < 2 else DIM)
    text(d, (297, 320), "LAUNCH", F(MONT, 11), GREY, anchor="mm")
    text(d, (297, 334), "3", F(MANROPE, 18), WHITE, anchor="mm")


def draw_ride_and_speed(d):
    text(d, (700, 300), "RIDE MODE", F(MONT, 13), GREY, anchor="mm")
    text(d, (700, 324), "TRACK", F(MANROPE, 22), ORANGE, anchor="mm")
    rrect(d, (612, 372, 788, 440), 8, fill=(14, 15, 18), outline=BORDER, width=2)
    d.line([(S(612), S(434)), (S(788), S(434))], fill=ORANGE, width=max(1, S(2)))
    text(d, (700, 452), "km/h", F(MONT, 14), GREY, anchor="mm")


def build_background():
    base = make_bg()
    d = ImageDraw.Draw(base)
    draw_header(d)
    draw_rpm_scale(d)
    draw_left_cluster(d)
    draw_tiles(d)
    draw_ride_and_speed(d)
    return base


def build_layout():
    return {
        "schema_version": 14,
        "name": "KTM",
        "screen_w": 800,
        "screen_h": 480,
        "signals": [
            {"name": "GEAR", "value_map": [{"v": 0, "label": "N"}]},
        ],
        "widgets": [
            {"type": "image", "id": "bg", "x": 0, "y": 0, "w": 800, "h": 480,
             "config": {"image_name": IMAGE_NAME, "auto_size": True}},
            {"type": "rpm_bar", "id": "rpm_bar_0", "x": 0, "y": -162, "w": 760, "h": 22,
             "config": {"signal_name": "RPM", "rpm_max": RPM_MAX, "redline": 9000,
                        "fill_dir": 3, "show_ticks": False, "smoothing_ms": 90,
                        "bar_color": rgb565(ORANGE), "bar_bg_color": rgb565((30, 32, 36))}},
            {"type": "text", "id": "gear", "x": 0, "y": -6, "w": 200, "h": 180,
             "config": {"signal_name": "GEAR", "decimals": 0,
                        "font": "Fugaz One:150", "text_color": rgb565(GREEN)}},
            {"type": "text", "id": "speed", "x": 300, "y": 152, "w": 170, "h": 70,
             "config": {"signal_name": "VEHICLE_SPEED", "decimals": 0,
                        "font": "Fugaz One:60", "text_color": rgb565(WHITE)}},
        ],
    }
