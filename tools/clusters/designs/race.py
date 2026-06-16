"""GT / F1-style race strip with a CONTOUR-FILL RPM ribbon.

The headline: the RPM bar is an angled ribbon whose fill follows the contour
(not a flat L->R rectangle). We do it with the firmware's existing image-fill
bar mode (widget_bar.c): the *unlit* ribbon is baked into the background; the
*lit* ribbon is a separate overlay image (`bar_image_full`) that the bar reveals
left->right by clipping. Because the shape lives in the art, the revealed edge
traces the contour automatically.

Two images per build:
  race_bg    - opaque 800x480 background (low-poly + unlit ribbon + all static UI)
  race_fill  - the lit ribbon overlay (alpha; bright teal hatch + red blocks)

Live overlays: SPEED (text), GEAR (text in the tab). RPM drives the ribbon bar.
"""
import numpy as np
from PIL import Image, ImageDraw, ImageChops

from cluster_lib import (
    SS, S, F, text, rrect, save_rdmimg, rgb565, polar, val_to_deg, thick_line,
    cx_to_center, cy_to_center, MANROPE, FUGAZ, MONT,
)

NAME = "race"
IMAGE_NAME = "race_bg"
FILL_NAME = "race_fill"
W, H = 800, 480

# ---------------------------------------------------------------- ribbon geometry
# RH hugs the band so the (full-height) leading-edge tip line doesn't overshoot.
RX0, RY0 = 40, 52          # ribbon image top-left on screen
RW, RH = 720, 84           # ribbon image size (1:1 with the bar widget -> zoom 256)
RPM_MAX = 10000

# Contour control points: (t = rpm/RPM_MAX, image-y). Lower y = higher on screen.
TEAL_TOP = [(0.00, 62), (0.035, 58), (0.09, 32), (0.155, 12), (0.22, 8),
            (0.45, 6), (0.63, 4), (0.70, 6), (0.735, 22), (0.758, 58)]
TEAL_BOT = [(0.00, 76), (0.10, 79), (0.50, 79), (0.735, 77), (0.758, 66)]
RED_TOP  = [(0.795, 8), (0.86, 24), (0.93, 40), (1.00, 56)]
RED_BOT  = [(0.795, 76), (0.86, 78), (0.93, 79), (1.00, 81)]

# ---------------------------------------------------------------- palette (RGB888)
BG_BASE   = (17, 20, 26)
WHITE     = (238, 241, 245)
GREY      = (150, 156, 165)
DIM       = (92, 98, 108)
TEAL_BASE_LIT = (16, 132, 116);  TEAL_HATCH_LIT = (74, 236, 202)
TEAL_BASE_DIM = (14, 50, 47);    TEAL_HATCH_DIM = (28, 92, 84)
RED_BASE_LIT  = (176, 30, 28);   RED_HATCH_LIT  = (244, 74, 60)
RED_BASE_DIM  = (58, 22, 24);    RED_HATCH_DIM  = (100, 36, 36)
OUTLINE   = (96, 150, 142)
CYAN      = (130, 255, 236)
YELLOW    = (244, 200, 38);  YELLOW_HATCH = (255, 226, 96)
MAGENTA   = (212, 96, 236)
GREEN     = (124, 230, 124)

STATES = {
    "idle":    {"RPM": 0,    "VEHICLE_SPEED": 0,   "GEAR": 0},
    "driving": {"RPM": 6500, "VEHICLE_SPEED": 142, "GEAR": 4},
    "launch":  {"RPM": 5200, "VEHICLE_SPEED": 0,   "GEAR": 1},
    "redline": {"RPM": 9200, "VEHICLE_SPEED": 188, "GEAR": 6},
}
DEFAULT_STATE = "driving"


# ---------------------------------------------------------------- contour helpers
def _interp(points, t):
    if t <= points[0][0]:
        return points[0][1]
    if t >= points[-1][0]:
        return points[-1][1]
    for (t0, y0), (t1, y1) in zip(points, points[1:]):
        if t0 <= t <= t1:
            f = (t - t0) / (t1 - t0) if t1 > t0 else 0
            return y0 + (y1 - y0) * f
    return points[-1][1]


def _poly(top, bot, n=120):
    """Closed band polygon in supersampled ribbon-image px."""
    t0, t1 = top[0][0], top[-1][0]
    pts = []
    for i in range(n + 1):
        t = t0 + (t1 - t0) * i / n
        pts.append((S(t * RW), S(_interp(top, t))))
    b0, b1 = bot[0][0], bot[-1][0]
    for i in range(n + 1):
        t = b1 - (b1 - b0) * i / n
        pts.append((S(t * RW), S(_interp(bot, t))))
    return pts


def _fill_hatch(img, poly, base, hatch, base_a, spacing=7, lw=2):
    """Fill `poly` with a flat base + diagonal hatch, clipped to the polygon."""
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).polygon(poly, fill=255)
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.polygon(poly, fill=base + (base_a,))
    step = S(spacing)
    for x in range(-img.size[1], img.size[0], step):
        ld.line([(x, img.size[1]), (x + img.size[1], 0)], fill=hatch + (255,), width=S(lw))
    layer.putalpha(ImageChops.multiply(layer.split()[3], mask))
    img.alpha_composite(layer)


def ribbon_image(lit):
    """Supersampled RGBA ribbon (RW*SS x RH*SS). lit=bright fill, else dim+outline."""
    img = Image.new("RGBA", (RW * SS, RH * SS), (0, 0, 0, 0))
    teal = _poly(TEAL_TOP, TEAL_BOT)
    red = _poly(RED_TOP, RED_BOT)
    if lit:
        _fill_hatch(img, teal, TEAL_BASE_LIT, TEAL_HATCH_LIT, 210, spacing=6, lw=2)
        _fill_hatch(img, red, RED_BASE_LIT, RED_HATCH_LIT, 220, spacing=6, lw=2)
    else:
        _fill_hatch(img, teal, TEAL_BASE_DIM, TEAL_HATCH_DIM, 150, spacing=6, lw=1)
        _fill_hatch(img, red, RED_BASE_DIM, RED_HATCH_DIM, 150, spacing=6, lw=1)
        d = ImageDraw.Draw(img)
        d.line(teal + [teal[0]], fill=OUTLINE + (200,), width=S(1))
        d.line(red + [red[0]], fill=OUTLINE + (170,), width=S(1))
    return img


# ---------------------------------------------------------------- background
def _lowpoly(base):
    rng = np.random.default_rng(7)
    cols, rows = 10, 6
    pts = {}
    for j in range(rows + 1):
        for i in range(cols + 1):
            x = i / cols * W + rng.uniform(-28, 28)
            y = j / rows * H + rng.uniform(-22, 22)
            pts[(i, j)] = (S(x), S(y))
    d = ImageDraw.Draw(base)
    for j in range(rows):
        for i in range(cols):
            a, b, c, e = pts[(i, j)], pts[(i + 1, j)], pts[(i, j + 1)], pts[(i + 1, j + 1)]
            for tri in ((a, b, c), (b, e, c)):
                sh = int(rng.uniform(-7, 9))
                cy = sum(p[1] for p in tri) / 3 / (H * SS)   # darker toward bottom
                col = tuple(max(0, min(255, int(BG_BASE[k] + sh - cy * 6))) for k in range(3))
                d.polygon(list(tri), fill=col + (255,))


def _draw_numbers(d):
    fn = F(MANROPE, 26)
    for k in range(11):
        x = RX0 + k * (RW / 10)
        red = k >= 8
        text(d, (x, 32), str(k), fn, (RED_HATCH_LIT if red else WHITE), anchor="mm")
        # minor tick between each number, sitting just above the band
        d.line([(S(x), S(46)), (S(x), S(50))], fill=DIM + (255,), width=max(1, S(1)))


def _gear_mode_box(d):
    rrect(d, (118, 188, 252, 250), 10, outline=GREY, width=2)
    # 3 status dots
    for i, on in enumerate((True, True, False)):
        cyp = 202 + i * 14
        col = WHITE if on else DIM
        d.ellipse([S(134), S(cyp - 4), S(142), S(cyp + 4)], fill=col + (255,))
    text(d, (200, 214), "R", F(FUGAZ, 40), WHITE, anchor="mm")
    text(d, (200, 240), "RACE", F(MONT, 13), GREY, anchor="mm")


def _speed_label(d):
    text(d, (412, 262), "MPH", F(MONT, 20), GREY, anchor="mm")


def _gear_gauge(d):
    # vertical 7..1 column
    fn = F(MANROPE, 13)
    for i, n in enumerate(range(7, 0, -1)):
        y = 188 + i * 13
        text(d, (612, y), str(n), fn, DIM, anchor="mm")
    # left-pointing plus marker
    d.line([(S(626), S(218)), (S(636), S(212)), (S(636), S(224))], fill=GREY + (255,), width=max(1, S(2)))
    # white tab (gear number overlays; "M" baked)
    pts = [(S(644), S(196)), (S(726), S(196)), (S(726), S(238)), (S(644), S(238)), (S(636), S(217))]
    d.polygon(pts, fill=WHITE + (255,))
    text(d, (712, 226), "M", F(MONT, 16), (40, 44, 52), anchor="mm")


def _slant_gauge(d, x0, top_lbl, bot_lbl, fill_frac, color):
    """Small slanted bracket gauge (H/C or F/E) with a partial hatch fill."""
    x1 = x0 + 24
    yt, yb = 300, 360
    d.line([(S(x0 + 6), S(yt)), (S(x1), S(yt))], fill=GREY + (255,), width=max(1, S(2)))
    d.line([(S(x0), S(yb)), (S(x1 - 6), S(yb))], fill=GREY + (255,), width=max(1, S(2)))
    d.line([(S(x0 + 3), S(yt)), (S(x0 - 3), S(yb))], fill=DIM + (255,), width=max(1, S(1)))
    text(d, (x0 + 12, yt - 12), top_lbl, F(MONT, 12), GREY, anchor="mm")
    text(d, (x0 + 12, yb + 12), bot_lbl, F(MONT, 12), GREY, anchor="mm")
    # partial hatch fill from bottom
    fh = int((yb - yt) * fill_frac)
    for yy in range(yb, yb - fh, -S(4) // SS or 1):
        pass  # (filled by hatch below)
    mask_top = yb - fh
    layer = Image.new("RGBA", (S(W), S(H)), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    poly = [(S(x0 + 1), S(mask_top)), (S(x1 - 1), S(mask_top)), (S(x1 - 3), S(yb - 1)), (S(x0 - 1), S(yb - 1))]
    m = Image.new("L", layer.size, 0)
    ImageDraw.Draw(m).polygon(poly, fill=255)
    ld.polygon(poly, fill=color + (120,))
    for xx in range(-layer.size[1], layer.size[0], S(5)):
        ld.line([(xx, layer.size[1]), (xx + layer.size[1], 0)], fill=color + (255,), width=S(1))
    layer.putalpha(ImageChops.multiply(layer.split()[3], m))
    return layer  # composited by caller


def _bottom_band(base):
    d = ImageDraw.Draw(base)
    # lap time + sector readouts
    text(d, (150, 286), "LAP TIME: 03:18.983", F(MONT, 16), WHITE, anchor="lm")
    text(d, (560, 286), "SECTOR 1: 01:27:13", F(MONT, 16), MAGENTA, anchor="lm")
    text(d, (650, 306), "-0:78", F(MONT, 14), GREEN, anchor="lm")
    # slant gauges flank the launch banner
    for layer in (_slant_gauge(d, 74, "H", "C", 0.55, (90, 200, 220)),
                  _slant_gauge(d, 706, "F", "E", 0.7, (90, 200, 220))):
        base.alpha_composite(layer)
    # LAUNCH banner: yellow hatched
    bx0, bx1, by0, by1 = 110, 690, 326, 366
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    m = Image.new("L", base.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([S(bx0), S(by0), S(bx1), S(by1)], radius=S(6), fill=255)
    ld.rounded_rectangle([S(bx0), S(by0), S(bx1), S(by1)], radius=S(6), fill=YELLOW + (255,))
    for xx in range(-layer.size[1], layer.size[0], S(7)):
        ld.line([(xx, layer.size[1]), (xx + layer.size[1], 0)], fill=YELLOW_HATCH + (255,), width=S(2))
    layer.putalpha(ImageChops.multiply(layer.split()[3], m))
    base.alpha_composite(layer)
    text(d, ((bx0 + bx1) / 2, (by0 + by1) / 2), "LAUNCH", F(MANROPE, 30), (28, 26, 14), anchor="mm")


def build_background():
    base = Image.new("RGBA", (W * SS, H * SS), BG_BASE + (255,))
    _lowpoly(base)
    # unlit ribbon
    base.alpha_composite(ribbon_image(False), (S(RX0), S(RY0)))
    d = ImageDraw.Draw(base)
    _draw_numbers(d)
    _gear_mode_box(d)
    _speed_label(d)
    _gear_gauge(d)
    _bottom_band(base)
    return base


def build_extra_images():
    """The lit ribbon overlay (bar_image_full), authored 1:1 with the bar."""
    lit = ribbon_image(True).resize((RW, RH), Image.LANCZOS)
    return [(FILL_NAME, lit)]


# ---------------------------------------------------------------- layout
def build_layout():
    return {
        "schema_version": 14,
        "name": "Race",
        "screen_w": 800,
        "screen_h": 480,
        "signals": [
            {"name": "RPM", "can_id": 256, "bit_start": 0, "bit_length": 16,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "rpm"},
            {"name": "VEHICLE_SPEED", "can_id": 256, "bit_start": 16, "bit_length": 16,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "mph"},
            {"name": "GEAR", "can_id": 257, "bit_start": 0, "bit_length": 8,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "",
             "value_map": [{"v": 0, "label": "N"}]},
        ],
        "widgets": [
            {"type": "image", "id": "bg", "x": 0, "y": 0, "w": 800, "h": 480,
             "config": {"image_name": IMAGE_NAME, "auto_size": True}},
            # CONTOUR-FILL RPM ribbon: image-fill bar reveals the lit ribbon L->R,
            # following the baked contour. Transparent track -> bg unlit ribbon shows.
            {"type": "bar", "id": "rpm_ribbon",
             "x": cx_to_center(RX0 + RW // 2), "y": cy_to_center(RY0 + RH // 2),
             "w": RW, "h": RH,
             "config": {"signal_name": "RPM", "bar_min": 0, "bar_max": RPM_MAX,
                        "bar_image_full": FILL_NAME, "bar_bg_opa": 0,
                        "show_bar_label": False, "show_bar_value": False,
                        "fill_edge_width": 4, "fill_edge_color": rgb565(CYAN),
                        "smoothing_ms": 70}},
            # live speed (big, centre)
            {"type": "text", "id": "speed",
             "x": cx_to_center(412), "y": cy_to_center(218), "w": 220, "h": 96,
             "config": {"slot": 0, "signal_name": "VEHICLE_SPEED", "decimals": 0,
                        "static_text": "", "font": "Fugaz One:84",
                        "text_color": rgb565(WHITE)}},
            # live gear (dark, inside the white tab)
            {"type": "text", "id": "gear",
             "x": cx_to_center(684), "y": cy_to_center(217), "w": 48, "h": 44,
             "config": {"slot": 0, "signal_name": "GEAR", "decimals": 0,
                        "static_text": "", "font": "Fugaz One:34",
                        "text_color": rgb565((30, 34, 42))}},
        ],
    }
