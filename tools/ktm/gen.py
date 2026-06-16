"""Generate a KTM-style 'demon' instrument cluster for the RDM-7 dash.

Technique (proven on the Altezza build): bake ALL the static art into one
800x480 background image; overlay only the live elements:
  - the RPM bar  (rpm_bar widget, fill_dir = 3 "Edges In" — the KTM mirror sweep)
  - the big centre GEAR character (text widget bound to GEAR, value-map 0 -> "N")
  - the SPEED readout (text widget bound to VEHICLE_SPEED)

Run:  python tools/ktm/gen.py
Outputs into tools/ktm/dist/:  ktm_bg.png (eyeball), ktm_bg.rdmimg, ktm_layout.json
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "altezza"))
from altezza_lib import SS, FONT_DIR, save_rdmimg, rgb565  # noqa: E402
from PIL import ImageFont  # noqa: E402

W, H = 800, 480
OUT = Path(__file__).resolve().parent / "dist"
OUT.mkdir(parents=True, exist_ok=True)

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
ORANGE_BR= (255, 140, 32)
GREEN    = (170, 226, 52)   # neutral / N tint (the live N widget uses this too)

# ---------------------------------------------------------------- fonts
def F(name, size):
    return ImageFont.truetype(str(FONT_DIR / name), size * SS)

MANROPE = "manrope_bold.ttf"
FUGAZ   = "fugaz_one.ttf"
MONT    = "montserrat.ttf"


def S(v):
    return int(round(v * SS))


def text(d, xy, s, fnt, fill, anchor="mm", spacing=0):
    d.text((S(xy[0]), S(xy[1])), s, font=fnt, fill=fill, anchor=anchor)


def rrect(d, box, radius, fill=None, outline=None, width=1):
    d.rounded_rectangle([S(box[0]), S(box[1]), S(box[2]), S(box[3])],
                        radius=S(radius), fill=fill, outline=outline,
                        width=max(1, S(width)))


# ---------------------------------------------------------------- background
def make_bg():
    base = Image.new("RGBA", (W * SS, H * SS), BG_BOT + (255,))
    # vertical gradient top->bottom
    grad = Image.new("RGBA", (1, H * SS))
    for y in range(H * SS):
        t = y / (H * SS)
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        grad.putpixel((0, y), c + (255,))
    base = grad.resize((W * SS, H * SS))
    # subtle centre glow
    glow = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([S(150), S(60), S(650), S(440)], fill=(46, 49, 56, 70))
    glow = glow.filter(ImageFilter.GaussianBlur(S(80)))
    base = Image.alpha_composite(base, glow)
    return base


# ---------------------------------------------------------------- header (top-left)
def draw_header(d):
    rrect(d, (18, 12, 96, 38), 6, outline=GREY, width=2)
    text(d, (57, 25.5), "MENU", F(MANROPE, 14), GREY, anchor="mm")
    text(d, (116, 25), "17:00", F(MANROPE, 18), WHITE, anchor="lm")
    text(d, (188, 25), "21°C", F(MANROPE, 16), GREY, anchor="lm")


# ---------------------------------------------------------------- rpm scale band
# Geometry MUST match the live rpm_bar widget so numbers line up with the fill.
# Bar spans device x [-380,380] => PIL x [20,780]; edges-in: each half fills
# from the outer edge toward centre (PIL x 400).  rpm value v (0..RPM_MAX)
#   left half  : x = 20  + (v/MAX)*380
#   right half : x = 780 - (v/MAX)*380
RPM_MAX = 10000
BAR_PIL_L, BAR_PIL_R, BAR_PIL_C = 20, 780, 400
NUM_Y   = 40       # rpm thousands labels baseline
TICK_Y0 = 52       # tick top
TICK_Y1 = 62       # tick bottom (just above the live bar)


def rpm_x(v, left):
    f = v / RPM_MAX
    return (BAR_PIL_L + f * (BAR_PIL_C - BAR_PIL_L)) if left else (BAR_PIL_R - f * (BAR_PIL_R - BAR_PIL_C))


def draw_rpm_scale(d):
    fnum = F(MANROPE, 15)
    for left in (True, False):
        for k in range(1, 10):            # 1..9 per arm; 10 is shared at centre
            v = k * 1000
            x = rpm_x(v, left)
            red = k == 9
            col = ORANGE if red else GREY
            text(d, (x, NUM_Y), str(k), fnum, col, anchor="mm")
            d.line([(S(x), S(TICK_Y0)), (S(x), S(TICK_Y1))],
                   fill=(ORANGE if red else DIM), width=max(1, S(2)))
            xh = rpm_x(v + 500, left)     # minor tick at the half-thousand
            d.line([(S(xh), S(TICK_Y0 + 4)), (S(xh), S(TICK_Y1))],
                   fill=DIM, width=max(1, S(1)))
    # shared "10" redline marker exactly at the centre (where both arms meet)
    text(d, (BAR_PIL_C, NUM_Y), "10", fnum, ORANGE, anchor="mm")
    d.line([(S(BAR_PIL_C), S(TICK_Y0)), (S(BAR_PIL_C), S(TICK_Y1))],
           fill=ORANGE, width=max(1, S(2)))
    # single redline bracket spanning the 9-10-9 zone, symmetric about centre
    bx = rpm_x(8400, True)
    rrect(d, (bx, NUM_Y - 14, BAR_PIL_R - (bx - BAR_PIL_L), TICK_Y1 + 2), 5,
          outline=ORANGE, width=2)
    text(d, (765, 80), "x1000", F(MONT, 11), GREY, anchor="rm")


# ---------------------------------------------------------------- left cluster
def draw_left_cluster(d):
    # traction-control swirl icon
    cx, cy, r = 150, 150, 15
    d.arc([S(cx - r), S(cy - r), S(cx + r), S(cy + r)], 30, 320,
          fill=GREY, width=max(1, S(2)))
    d.ellipse([S(cx + r - 5), S(cy - 4), S(cx + r + 1), S(cy + 2)], fill=ORANGE)
    # ghost gear "1"  (current gear, shown big to match the reference)
    text(d, (118, 200), "1", F(FUGAZ, 78), WHITE, anchor="mm")
    # fuel range "300 km" with a tiny pump glyph
    px, py = 92, 252
    rrect(d, (px, py - 9, px + 13, py + 9), 2, outline=GREY, width=2)
    d.line([(S(px + 13), S(py - 4)), (S(px + 17), S(py - 4)), (S(px + 17), S(py + 6))],
           fill=GREY, width=max(1, S(2)))
    text(d, (px + 26, py), "300 km", F(MANROPE, 18), WHITE, anchor="lm")


# ---------------------------------------------------------------- info tiles
def draw_tiles(d):
    # AW (anti-wheelie) tile
    rrect(d, (92, 286, 232, 346), 8, fill=PANEL, outline=BORDER, width=2)
    # wheelie motorcycle glyph (two wheels + a body line, front lifted)
    wx, wy = 118, 322
    d.ellipse([S(wx - 9), S(wy + 2), S(wx + 1), S(wy + 12)], outline=WHITE, width=max(1, S(2)))
    d.ellipse([S(wx + 12), S(wy - 4), S(wx + 22), S(wy + 6)], outline=WHITE, width=max(1, S(2)))
    d.line([(S(wx - 4), S(wy + 7)), (S(wx + 8), S(wy - 6)), (S(wx + 17), S(wy + 1))],
           fill=WHITE, width=max(1, S(2)))
    text(d, (150, 305), "AW", F(MONT, 12), GREY, anchor="lm")
    text(d, (150, 326), "V.HIGH", F(MANROPE, 17), WHITE, anchor="lm")

    # LAUNCH tile
    rrect(d, (242, 286, 352, 346), 8, fill=PANEL, outline=BORDER, width=2)
    for i in range(3):
        cx = 262 + i * 11
        d.ellipse([S(cx - 3), S(303), S(cx + 3), S(309)],
                  fill=ORANGE if i < 2 else DIM)
    text(d, (262, 300), "", F(MONT, 11), GREY, anchor="lm")
    text(d, (297, 320), "LAUNCH", F(MONT, 11), GREY, anchor="mm")
    text(d, (297, 334), "3", F(MANROPE, 18), WHITE, anchor="mm")


# ---------------------------------------------------------------- ride mode + speed
def draw_ride_and_speed(d):
    text(d, (700, 300), "RIDE MODE", F(MONT, 13), GREY, anchor="mm")
    text(d, (700, 324), "TRACK", F(MANROPE, 22), ORANGE, anchor="mm")

    # speed box (digits overlaid live) + km/h
    rrect(d, (612, 372, 788, 440), 8, fill=(14, 15, 18), outline=BORDER, width=2)
    d.line([(S(612), S(434)), (S(788), S(434))], fill=ORANGE, width=max(1, S(2)))
    text(d, (700, 452), "km/h", F(MONT, 14), GREY, anchor="mm")


def build():
    base = make_bg()
    d = ImageDraw.Draw(base)
    draw_header(d)
    draw_rpm_scale(d)
    draw_left_cluster(d)
    draw_tiles(d)
    draw_ride_and_speed(d)
    img = base.resize((W, H), Image.LANCZOS).convert("RGBA")
    # force fully opaque so the firmware opaque-image fast path engages
    img.putalpha(255)
    img.convert("RGB").save(OUT / "ktm_bg.png")
    w, h, n = save_rdmimg(img, OUT / "ktm_bg.rdmimg")
    print(f"bg: {w}x{h}  {n} bytes  -> {OUT/'ktm_bg.rdmimg'}")
    write_layout()


# ---------------------------------------------------------------- layout json
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
             "config": {"image_name": "ktm_bg", "auto_size": True}},
            # RPM bar — Edges In (outside->inside) mirror sweep. Ticks off
            # (baked); redline-zone/Panel9 auto-hidden in non-default fill modes.
            {"type": "rpm_bar", "id": "rpm_bar_0", "x": 0, "y": -162, "w": 760, "h": 22,
             "config": {"signal_name": "RPM", "rpm_max": RPM_MAX, "redline": 9000,
                        "fill_dir": 3, "show_ticks": False, "smoothing_ms": 90,
                        "bar_color": rgb565(ORANGE), "bar_bg_color": rgb565((30, 32, 36))}},
            # Centre gear (N / 1..6)
            {"type": "text", "id": "gear", "x": 0, "y": -6, "w": 200, "h": 180,
             "config": {"signal_name": "GEAR", "decimals": 0,
                        "font": "Fugaz One:150", "text_color": rgb565(GREEN)}},
            # Speed
            {"type": "text", "id": "speed", "x": 300, "y": 152, "w": 170, "h": 70,
             "config": {"signal_name": "VEHICLE_SPEED", "decimals": 0,
                        "font": "Fugaz One:60", "text_color": rgb565(WHITE)}},
        ],
    }


def write_layout():
    p = OUT / "ktm_layout.json"
    p.write_text(json.dumps(build_layout(), separators=(",", ":")))
    print(f"layout -> {p}  ({p.stat().st_size} bytes)")


if __name__ == "__main__":
    build()
