"""Mercedes-AMG V12 BiTurbo tachometer cluster.

Round black dial w/ chrome bezel + red rim, 0..8 (x1000) scale, red redline band
6.5->8k, "V12 BiTurbo" badge, "R N P" selector + "M", "1/min x1000" label, and a
chrome coolant pod at the bottom. Live overlays: RPM needle (meter), boxed GEAR
(value-map 0->N), coolant temp.
"""
from PIL import Image, ImageDraw, ImageFilter

from cluster_lib import (
    SS, S, F, text, rrect, fill_gradient, sheared_text, save_rdmimg, rgb565,
    polar, val_to_deg, thick_line, cx_to_center, cy_to_center,
    MANROPE, FUGAZ, MONT,
)

NAME = "mercedes"
IMAGE_NAME = "mercedes_bg"
W, H = 800, 480

# ---------------------------------------------------------------- palette (RGB888)
FACE       = (9, 10, 12)
FACE_GLOW  = (34, 37, 44)
CHROME_HI  = (208, 212, 219)
CHROME_MID = (150, 154, 161)
CHROME_LO  = (52, 54, 60)
CHROME_LIP = (176, 180, 188)
RED_RIM    = (158, 18, 22)
NUM        = (226, 230, 236)
NUM_SH     = (0, 0, 0)
WHITE      = (236, 239, 243)
GREY       = (150, 155, 163)
DIM        = (96, 100, 108)
RED        = (214, 32, 30)
RED_HOT    = (255, 70, 56)
NAVY       = (26, 30, 44)
RING       = (46, 49, 57)

# ---------------------------------------------------------------- geometry
CX, CY = 400, 240
R_BEZEL_OUT = 234
R_BEZEL_IN  = 210
R_OUT       = 206
R_MINOR     = 196
R_MAJOR     = 180
R_LABEL     = 155

TACH = dict(cx=CX, cy=CY, r_out=R_OUT, r_minor=R_MINOR, r_major=R_MAJOR,
            r_label=R_LABEL, vmin=0, vmax=8000, start=135, sweep=270,
            major=1000, minor=250, redline=6500)

STATES = {
    "idle": {"RPM": 800, "GEAR": 0, "COOLANT_TEMP": 88},
    "driving": {"RPM": 4000, "GEAR": 4, "COOLANT_TEMP": 90},
    "redline": {"RPM": 6500, "GEAR": 6, "COOLANT_TEMP": 92},
}
DEFAULT_STATE = "driving"


# ---------------------------------------------------------------- background
def make_bg():
    base = Image.new("RGBA", (W * SS, H * SS), (6, 7, 9, 255))
    grad = fill_gradient((W * SS, H * SS), (24, 26, 30), (8, 9, 11))
    return Image.alpha_composite(base, grad)


def draw_bezel(base):
    cx, cy = S(CX), S(CY)
    r_out = S(R_BEZEL_OUT)
    r_in = S(R_BEZEL_IN)
    ring = r_out - r_in
    d = ImageDraw.Draw(base)

    sh = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - r_out - S(10), cy - r_out + S(6),
                                cx + r_out + S(10), cy + r_out + S(20)], fill=(0, 0, 0, 150))
    sh = sh.filter(ImageFilter.GaussianBlur(S(16)))
    base.alpha_composite(sh)

    d.ellipse([cx - r_out - S(5), cy - r_out - S(5), cx + r_out + S(5), cy + r_out + S(5)],
              fill=RED_RIM + (255,))

    grad = fill_gradient((2 * r_out, 2 * r_out), CHROME_HI, CHROME_LO)
    mask = Image.new("L", (2 * r_out, 2 * r_out), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse([0, 0, 2 * r_out, 2 * r_out], fill=255)
    md.ellipse([ring, ring, 2 * r_out - ring, 2 * r_out - ring], fill=0)
    base.paste(grad, (cx - r_out, cy - r_out), mask)

    d.arc([cx - r_out + S(2), cy - r_out + S(2), cx + r_out - S(2), cy + r_out - S(2)],
          188, 320, fill=(255, 255, 255, 150), width=S(2))
    d.arc([cx - r_out + S(2), cy - r_out + S(2), cx + r_out - S(2), cy + r_out - S(2)],
          18, 132, fill=(0, 0, 0, 130), width=S(2))

    d.ellipse([cx - r_in, cy - r_in, cx + r_in, cy + r_in], fill=FACE + (255,))
    d.ellipse([cx - r_in, cy - r_in, cx + r_in, cy + r_in],
              outline=CHROME_LIP + (210,), width=S(2))
    d.ellipse([cx - r_in - S(2), cy - r_in - S(2), cx + r_in + S(2), cy + r_in + S(2)],
              outline=(0, 0, 0, 160), width=S(1))

    sheen = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(sheen).ellipse([cx - S(150), cy - S(140), cx + S(150), cy + S(140)],
                                  fill=FACE_GLOW + (255,))
    sheen = sheen.filter(ImageFilter.GaussianBlur(S(64)))
    base.alpha_composite(sheen)
    for rr in (88, 110, 132):
        d.ellipse([cx - S(rr), cy - S(rr), cx + S(rr), cy + S(rr)],
                  outline=RING + (150,), width=S(1))


def draw_redband(d):
    g = TACH
    a0 = val_to_deg(g["redline"], g["vmin"], g["vmax"], g["start"], g["sweep"]) % 360
    a1 = val_to_deg(g["vmax"], g["vmin"], g["vmax"], g["start"], g["sweep"]) % 360
    if a1 <= a0:
        a1 += 360
    rb = S(R_OUT - 2)
    cx, cy = S(CX), S(CY)
    d.arc([cx - rb, cy - rb, cx + rb, cy + rb], a0, a1, fill=RED + (255,), width=S(12))
    d.arc([cx - rb, cy - rb, cx + rb, cy + rb], a0, a1, fill=RED_HOT + (200,), width=S(4))


def draw_dial(d):
    g = TACH
    v = g["vmin"]
    while v <= g["vmax"] + 1e-6:
        deg = val_to_deg(v, g["vmin"], g["vmax"], g["start"], g["sweep"])
        is_major = round(v) % g["major"] == 0
        col = WHITE if is_major else GREY
        r1 = g["r_major"] if is_major else g["r_minor"]
        p0 = polar(S(g["cx"]), S(g["cy"]), S(g["r_out"]), deg)
        p1 = polar(S(g["cx"]), S(g["cy"]), S(r1), deg)
        thick_line(d, p0, p1, col + (255,), S(4) if is_major else S(2))
        v += g["minor"]

    fn = F(MANROPE, 42)
    v = g["vmin"]
    while v <= g["vmax"] + 1e-6:
        deg = val_to_deg(v, g["vmin"], g["vmax"], g["start"], g["sweep"])
        px, py = polar(S(g["cx"]), S(g["cy"]), S(g["r_label"]), deg)
        n = str(int(round(v / 1000)))
        d.text((px + S(1.5), py + S(2)), n, font=fn, fill=NUM_SH + (200,), anchor="mm")
        d.text((px, py), n, font=fn, fill=NUM + (255,), anchor="mm")
        v += g["major"]


def draw_badge(base):
    d = ImageDraw.Draw(base)
    sheared_text(base, (332, 222), "V12", F(FUGAZ, 46), NUM, shear=-0.22, anchor="lm")
    fnt = F(MONT, 22)
    x = S(452)
    for ch in "BITURBO":
        d.text((x, S(222)), ch, font=fnt, fill=WHITE + (255,), anchor="lm")
        x += fnt.getbbox(ch)[2] - fnt.getbbox(ch)[0] + S(3)


def draw_selector(base):
    d = ImageDraw.Draw(base)
    text(d, (400, 296), "R", F(MANROPE, 22), GREY, anchor="mm")
    tx, ty = S(372), S(330)
    d.polygon([(tx, ty - S(7)), (tx - S(6), ty + S(4)), (tx + S(6), ty + S(4))], fill=WHITE + (255,))
    text(d, (398, 330), "N", F(MANROPE, 24), WHITE, anchor="mm")
    text(d, (430, 330), "P", F(MANROPE, 22), GREY, anchor="mm")
    rrect(d, (366, 348, 408, 386), 4, outline=GREY, width=2)
    text(d, (436, 367), "M", F(MANROPE, 24), RED_HOT, anchor="mm")
    text(d, (548, 326), "1/min", F(MANROPE, 16), GREY, anchor="mm")
    text(d, (548, 348), "x1000", F(MANROPE, 16), GREY, anchor="mm")


def icon_temp(d, cx, cy, col):
    cx, cy = S(cx), S(cy)
    c = col + (255,)
    d.line([(cx, cy - S(11)), (cx, cy + S(3))], fill=c, width=S(3))
    d.ellipse([cx - S(5), cy + S(2), cx + S(5), cy + S(12)], outline=c, width=S(2))
    d.ellipse([cx - S(2), cy + S(5), cx + S(2), cy + S(9)], fill=c)
    for dy in (-8, -4, 0):
        d.line([(cx + S(3), cy + S(dy)), (cx + S(7), cy + S(dy))], fill=c, width=S(1))
    d.arc([cx - S(11), cy + S(11), cx - S(3), cy + S(17)], 180, 360, fill=c, width=S(2))
    d.arc([cx - S(3), cy + S(11), cx + S(5), cy + S(17)], 0, 180, fill=c, width=S(2))
    d.arc([cx + S(5), cy + S(11), cx + S(13), cy + S(17)], 180, 360, fill=c, width=S(2))


def draw_coolant(base):
    d = ImageDraw.Draw(base)
    x0, y0, x1, y1 = 305, 427, 495, 474
    grad = fill_gradient(((x1 - x0) * SS, (y1 - y0) * SS), (190, 194, 201), (60, 62, 68))
    mask = Image.new("L", grad.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, grad.size[0] - 1, grad.size[1] - 1],
                                           radius=S((y1 - y0) / 2), fill=255)
    base.paste(grad, (S(x0), S(y0)), mask)
    rrect(d, (x0 + 4, y0 + 4, x1 - 4, y1 - 4), (y1 - y0) / 2 - 4, fill=(16, 17, 20, 255))
    rrect(d, (x0 + 4, y0 + 4, x1 - 4, y1 - 4), (y1 - y0) / 2 - 4, outline=(70, 72, 78, 255), width=1)
    icon_temp(d, 360, 451, WHITE)
    text(d, (442, 446), "°C", F(MANROPE, 22), WHITE, anchor="lm")


def build_background():
    base = make_bg()
    draw_bezel(base)
    d = ImageDraw.Draw(base)
    draw_redband(d)
    draw_dial(d)
    draw_badge(base)
    draw_selector(base)
    draw_coolant(base)
    return base


# ---------------------------------------------------------------- layout
def build_layout():
    g = TACH
    size = 2 * g["r_out"]
    return {
        "schema_version": 14,
        "name": "Mercedes",
        "screen_w": 800,
        "screen_h": 480,
        "signals": [
            {"name": "RPM", "can_id": 256, "bit_start": 0, "bit_length": 16,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "rpm"},
            {"name": "GEAR", "can_id": 257, "bit_start": 0, "bit_length": 8,
             "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "",
             "value_map": [{"v": 0, "label": "N"}]},
            {"name": "COOLANT_TEMP", "can_id": 257, "bit_start": 8, "bit_length": 16,
             "scale": 0.1, "offset": 0, "is_signed": False, "endian": 1, "unit": "degC"},
        ],
        "widgets": [
            {"type": "image", "id": "bg", "x": 0, "y": 0, "w": 800, "h": 480,
             "config": {"image_name": IMAGE_NAME, "auto_size": True}},
            {"type": "meter", "id": "tach",
             "x": cx_to_center(g["cx"]), "y": cy_to_center(g["cy"]),
             "w": size, "h": size,
             "config": {
                 "slot": 0, "signal_name": "RPM",
                 "min": g["vmin"], "max": g["vmax"],
                 "start_angle": g["start"] % 360, "end_angle": (g["start"] + g["sweep"]) % 360,
                 "show_ticks": False, "show_tick_labels": False,
                 "meter_bg_opa": 0, "border_width": 0,
                 "show_needle": True, "needle_width": 5,
                 "needle_color": rgb565(NAVY), "needle_r_mod": -28,
                 "needle_rear_length": 30, "needle_tip_style": 3,
                 "show_needle_ball": True, "needle_ball_size": 26,
                 "needle_ball_color": rgb565((20, 22, 32)),
                 "smoothing_ms": 120,
                 "shadow_enabled": True, "shadow_dynamic": True,
                 "shadow_offset_x": 3, "shadow_offset_y": 4,
                 "shadow_opa": 90, "shadow_width_extra": 5}},
            {"type": "text", "id": "gear",
             "x": cx_to_center(387), "y": cy_to_center(366), "w": 44, "h": 40,
             "config": {"slot": 0, "signal_name": "GEAR", "decimals": 0,
                        "static_text": "", "font": "Fugaz One:30",
                        "text_color": rgb565(NUM)}},
            {"type": "text", "id": "coolant",
             "x": cx_to_center(406), "y": cy_to_center(450), "w": 84, "h": 46,
             "config": {"slot": 0, "signal_name": "COOLANT_TEMP", "decimals": 0,
                        "static_text": "", "font": "Manrope Bold:34",
                        "text_color": rgb565(WHITE)}},
        ],
    }
