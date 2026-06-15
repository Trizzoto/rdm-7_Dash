"""Generate the Toyota Altezza cluster: one baked 800x480 background image
(housing, all gauge faces, ticks, amber EL numerals, redline, icons, LCD frame)
plus the layout.json + manifest.json that overlay live needles and digits.

Run:  python tools/altezza/gen.py
Then in the preview harness call window.reloadScene().
"""
import json
import math
import shutil

from PIL import Image, ImageDraw, ImageFilter

from altezza_lib import (
    SS, REPO, OUT, IMG_OUT, FONT_OUT, FONT_DIR,
    FACE, FACE_EDGE, HOUSING, HOUSING_HI, BEZEL_HI, BEZEL_LO, CHROME_LIP,
    AMBER, AMBER_HOT, AMBER_DIM, RED, RED_HOT, ORANGE_HOT, NEEDLE_RGB, CREAM,
    LCD_AMBER, LCD_GHOST, LCD_BG, WHITE,
    rgb565, save_rdmimg, font, MANROPE, FUGAZ, MONT,
    polar, val_to_deg, text_centered, thick_line, composite_glow,
    radial_alpha, brushed_noise, vgrad_alpha,
)

W, H = 800, 480
CW, CH = W * SS, H * SS

# ---------------------------------------------------------------- gauge specs (final px)
TACH = dict(cx=406, cy=246, r_out=197, r_minor=186, r_major=173, r_label=150,
            vmin=0, vmax=9000, start=135, sweep=270, major=1000, minor=250,
            redline=7500, redline2=8500, divisor=1000)
SPEEDO = dict(cx=119, cy=251, r_out=110, r_minor=101, r_major=90, r_label=72,
              vmin=0, vmax=180, start=135, sweep=270, major=20, minor=10)
FUEL = dict(cx=701, cy=151, r_out=83, r_minor=75, r_major=66, r_label=56,
            vmin=0, vmax=100, start=200, sweep=140, major=50, minor=25)
OIL = dict(cx=406, cy=123, r_out=50, start=215, sweep=110)
VOLT = dict(cx=525, cy=246, r_out=50, start=215, sweep=110)
TEMP = dict(cx=406, cy=352, r_out=48, start=215, sweep=110)


def S(v):
    return int(round(v * SS))


def glass_highlight(base, cx, cy, r_face, alpha=24):
    """Soft diagonal glass sheen clipped to a gauge face (drawn over everything)."""
    cx, cy, r = S(cx), S(cy), S(r_face)
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.ellipse([cx - r * 0.95, cy - r * 1.08, cx + r * 0.55, cy - r * 0.05],
               fill=(255, 255, 255, alpha))
    layer = layer.filter(ImageFilter.GaussianBlur(S(16)))
    mask = Image.new("L", base.size, 0)
    ImageDraw.Draw(mask).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    clipped = Image.composite(layer.split()[3], Image.new("L", base.size, 0), mask)
    base.paste(layer, (0, 0), clipped)


# ---------------------------------------------------------------- pods / housing
def fill_gradient(size, top, bot):
    g = Image.new("RGBA", size)
    px = g.load()
    wd, ht = size
    for y in range(ht):
        t = y / max(1, ht - 1)
        px_row = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(wd):
            px[x, y] = px_row
    return g


def draw_pod(base, cx, cy, r_face, ring=11, face=FACE):
    """Chrome bezel ring + dark face (with vignette) for a circular pod."""
    cx, cy, r_face, ring = S(cx), S(cy), S(r_face), S(ring)
    r_out = r_face + ring
    # bezel: vertical chrome gradient through an annulus mask
    grad = fill_gradient((2 * r_out, 2 * r_out), BEZEL_HI, BEZEL_LO)
    mask = Image.new("L", (2 * r_out, 2 * r_out), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse([0, 0, 2 * r_out, 2 * r_out], fill=255)
    md.ellipse([ring, ring, 2 * r_out - ring, 2 * r_out - ring], fill=0)
    base.paste(grad, (cx - r_out, cy - r_out), mask)
    d = ImageDraw.Draw(base)
    # face
    d.ellipse([cx - r_face, cy - r_face, cx + r_face, cy + r_face], fill=face + (255,))
    # radial vignette baked onto the face (depth)
    vig = radial_alpha(r_face, inner=0, outer=150, color=(0, 0, 0), power=2.6)
    fmask = Image.new("L", (2 * r_face, 2 * r_face), 0)
    ImageDraw.Draw(fmask).ellipse([0, 0, 2 * r_face, 2 * r_face], fill=255)
    base.paste(vig, (cx - r_face, cy - r_face), Image.composite(vig.split()[3], Image.new("L", vig.size, 0), fmask))
    # chrome lip: bright thin ring at the inner edge of the bezel
    d.ellipse([cx - r_face, cy - r_face, cx + r_face, cy + r_face],
              outline=CHROME_LIP + (200,), width=max(1, S(1)))
    d.ellipse([cx - r_face - S(1), cy - r_face - S(1), cx + r_face + S(1), cy + r_face + S(1)],
              outline=(0, 0, 0, 180), width=max(1, S(1)))
    # glassy highlight arc top-left of the bezel; shadow bottom-right
    d.arc([cx - r_out + S(2), cy - r_out + S(2), cx + r_out - S(2), cy + r_out - S(2)],
          195, 320, fill=(220, 222, 232, 150), width=S(2))
    d.arc([cx - r_out + S(2), cy - r_out + S(2), cx + r_out - S(2), cy + r_out - S(2)],
          20, 140, fill=(0, 0, 0, 150), width=S(2))


# ---------------------------------------------------------------- gauge ticks/numbers
def draw_gauge(sharp_d, red_d, g, *, number_size, minor_w=2.0, major_w=4.0,
               label_color=AMBER, show_numbers=True, redline_color=RED):
    cx, cy = g["cx"], g["cy"]
    vmin, vmax, start, sweep = g["vmin"], g["vmax"], g["start"], g["sweep"]
    major, minor = g["major"], g["minor"]
    r_out, r_minor, r_major, r_label = g["r_out"], g["r_minor"], g["r_major"], g["r_label"]
    redline = g.get("redline", None)      # caution (orange) start
    redline2 = g.get("redline2", None)    # danger (red) start

    def tier(v, base):
        """Return (color, draw-layer) for a value, applying the redline tiers."""
        if redline2 is not None and v >= redline2 - 1e-6:
            return RED, red_d
        if redline is not None and v >= redline - 1e-6:
            return ORANGE_HOT, red_d
        return base, sharp_d

    # ticks
    v = vmin
    while v <= vmax + 1e-6:
        deg = val_to_deg(v, vmin, vmax, start, sweep)
        is_major = (round(v) % major == 0)
        col, d = tier(v, AMBER)
        p0 = polar(S(cx), S(cy), S(r_out), deg)
        if is_major:
            thick_line(d, p0, polar(S(cx), S(cy), S(r_major), deg), col + (255,), major_w * SS)
        else:
            thick_line(d, p0, polar(S(cx), S(cy), S(r_minor), deg), col + (255,), minor_w * SS)
        v += minor

    # numbers at majors
    if show_numbers:
        fnt = font(MANROPE, S(number_size))
        v = vmin
        div = g.get("divisor", 1)
        while v <= vmax + 1e-6:
            deg = val_to_deg(v, vmin, vmax, start, sweep)
            px, py = polar(S(cx), S(cy), S(r_label), deg)
            col, d = tier(v, label_color)
            text_centered(d, (px, py), str(int(round(v / div))), fnt, col + (255,))
            v += major


# ---------------------------------------------------------------- mini gauge (L/H etc.)
def draw_mini(base, sharp_d, g, left_lbl, right_lbl, *, ticks=7):
    cx, cy, r_out, start, sweep = g["cx"], g["cy"], g["r_out"], g["start"], g["sweep"]
    # inset sub-dial: faint recessed circle on the housing/base
    bd = ImageDraw.Draw(base)
    bd.ellipse([S(cx - r_out - 4), S(cy - r_out - 4), S(cx + r_out + 4), S(cy + r_out + 4)],
               fill=(11, 11, 13, 255), outline=(40, 40, 46, 255), width=S(1))
    bd.arc([S(cx - r_out - 4), S(cy - r_out - 4), S(cx + r_out + 4), S(cy + r_out + 4)],
           200, 340, fill=(70, 70, 78, 180), width=S(1))
    mid = (ticks - 1) // 2
    for i in range(ticks):
        t = i / (ticks - 1)
        deg = start + sweep * t
        major = (i == 0 or i == ticks - 1 or i == mid)
        ln = 12 if major else 7
        w = 2.6 if major else 1.6
        p0 = polar(S(cx), S(cy), S(r_out), deg)
        p1 = polar(S(cx), S(cy), S(r_out - ln), deg)
        thick_line(sharp_d, p0, p1, AMBER + (255,), w * SS)
    fnt = font(MANROPE, S(15))
    if left_lbl:
        lp = polar(S(cx), S(cy), S(r_out - 17), start)
        text_centered(sharp_d, lp, left_lbl, fnt, AMBER + (255,))
    if right_lbl:
        rp = polar(S(cx), S(cy), S(r_out - 17), start + sweep)
        text_centered(sharp_d, rp, right_lbl, fnt, AMBER + (255,))


# ---------------------------------------------------------------- icons (simple vector)
def icon_oilcan(d, cx, cy, col):
    cx, cy = S(cx), S(cy)
    c = col + (255,)
    # body
    d.rounded_rectangle([cx - S(10), cy - S(3), cx + S(6), cy + S(6)], radius=S(2), fill=c)
    # spout
    d.line([(cx + S(6), cy - S(1)), (cx + S(15), cy - S(7))], fill=c, width=S(2))
    # handle
    d.arc([cx - S(9), cy - S(9), cx - S(1), cy - S(1)], 180, 350, fill=c, width=S(2))
    # drip
    d.ellipse([cx + S(14), cy - S(2), cx + S(18), cy + S(3)], fill=c)


def icon_battery(d, cx, cy, col):
    cx, cy = S(cx), S(cy)
    c = col + (255,)
    w, h = S(14), S(8)
    d.rounded_rectangle([cx - w, cy - h, cx + w, cy + h], radius=S(2), outline=c, width=S(2))
    d.rectangle([cx - w * 0.55, cy - h - S(3), cx - w * 0.15, cy - h + S(1)], fill=c)
    d.rectangle([cx + w * 0.15, cy - h - S(3), cx + w * 0.55, cy - h + S(1)], fill=c)
    f = font(MANROPE, S(10))
    d.text((cx - w * 0.55, cy + S(1)), "+", font=f, fill=c, anchor="mm")
    d.text((cx + w * 0.55, cy + S(1)), "–", font=f, fill=c, anchor="mm")


def icon_temp(d, cx, cy, col):
    cx, cy = S(cx), S(cy)
    c = col + (255,)
    # thermometer: stem + bulb (tilted), with a water wave under it
    d.line([(cx - S(4), cy - S(9)), (cx - S(4), cy + S(3))], fill=c, width=S(3))
    d.ellipse([cx - S(8), cy + S(2), cx, cy + S(10)], fill=c)
    d.line([(cx + S(2), cy - S(7)), (cx + S(8), cy - S(7))], fill=c, width=S(2))
    d.line([(cx + S(2), cy - S(3)), (cx + S(8), cy - S(3))], fill=c, width=S(2))
    # wave
    d.arc([cx - S(2), cy + S(9), cx + S(6), cy + S(15)], 180, 360, fill=c, width=S(2))
    d.arc([cx + S(6), cy + S(9), cx + S(14), cy + S(15)], 0, 180, fill=c, width=S(2))


def icon_fuel(d, cx, cy, col):
    cx, cy = S(cx), S(cy)
    c = col + (255,)
    # pump body
    d.rounded_rectangle([cx - S(10), cy - S(9), cx + S(2), cy + S(9)], radius=S(2),
                        outline=c, width=S(2))
    d.line([(cx - S(7), cy - S(4)), (cx - S(1), cy - S(4))], fill=c, width=S(2))  # window
    # nozzle/hose
    d.line([(cx + S(2), cy - S(5)), (cx + S(8), cy - S(5))], fill=c, width=S(2))
    d.line([(cx + S(8), cy - S(5)), (cx + S(8), cy + S(6))], fill=c, width=S(2))
    d.line([(cx + S(8), cy + S(6)), (cx + S(11), cy + S(6))], fill=c, width=S(2))


# ---------------------------------------------------------------- LCD panel
def draw_lcd(base):
    d = ImageDraw.Draw(base)
    x0, y0, x1, y1 = S(600), S(300), S(798), S(432)
    # recessed panel
    d.rounded_rectangle([x0, y0, x1, y1], radius=S(8), fill=LCD_BG + (255,),
                        outline=(70, 40, 6, 255), width=S(2))
    sharp = Image.new("RGBA", base.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sharp)
    # ODO row (top)
    sd.text((S(612), S(320)), "ODO", font=font(MANROPE, S(15)), fill=LCD_AMBER + (255,), anchor="lm")
    sd.text((S(788), S(320)), "114239", font=font(FUGAZ, S(28)), fill=LCD_AMBER + (255,), anchor="rm")
    # bottom row labels (the big speed number is a live text widget, centered above)
    sd.text((S(612), S(414)), "SPEED", font=font(MANROPE, S(15)), fill=LCD_AMBER + (255,), anchor="lm")
    sd.text((S(788), S(414)), "km/h", font=font(MANROPE, S(16)), fill=LCD_AMBER + (255,), anchor="rm")
    out = composite_glow(base, sharp, radii=(6, 16), gains=(0.9, 0.5))
    base.paste(out, (0, 0))


# ---------------------------------------------------------------- ALTEZZA wordmark
def draw_altezza(sharp_d):
    fnt = font(MONT, S(19))
    txt = "ALTEZZA"
    base_x, base_y = S(330), S(252)
    # simple letterspacing
    spacing = S(3)
    total = 0
    widths = []
    for ch in txt:
        bb = fnt.getbbox(ch)
        widths.append(bb[2] - bb[0])
        total += (bb[2] - bb[0]) + spacing
    x = base_x - total / 2
    for ch, wch in zip(txt, widths):
        sharp_d.text((x, base_y), ch, font=fnt, fill=AMBER + (255,), anchor="lm")
        x += wch + spacing


# ================================================================ build
def build_background():
    base = Image.new("RGBA", (CW, CH), HOUSING + (255,))
    # vignette: subtle lighter center band, darker corners
    vd = ImageDraw.Draw(base)
    # broad soft top highlight
    hl = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    hld = ImageDraw.Draw(hl)
    hld.ellipse([S(-200), S(-380), S(1000), S(260)], fill=HOUSING_HI + (40,))
    from PIL import ImageFilter
    hl = hl.filter(ImageFilter.GaussianBlur(S(60)))
    base = Image.alpha_composite(base, hl)
    # faint brushed grain over the housing
    base = Image.alpha_composite(base, brushed_noise((CW, CH), amp=6))
    # cowl shadow (hood) top + soft floor bottom
    base.alpha_composite(vgrad_alpha(CW, S(96), 150, 0), (0, 0))
    base.alpha_composite(vgrad_alpha(CW, S(80), 0, 120), (0, CH - S(80)))

    # pods (bezel + face)
    draw_pod(base, **{"cx": SPEEDO["cx"], "cy": SPEEDO["cy"], "r_face": SPEEDO["r_out"] + 6})
    draw_pod(base, **{"cx": TACH["cx"], "cy": TACH["cy"], "r_face": TACH["r_out"] + 8, "ring": 13})
    draw_pod(base, **{"cx": FUEL["cx"], "cy": FUEL["cy"], "r_face": FUEL["r_out"] + 5, "ring": 8})

    # faint inner ring on the main gauges (connects tick roots)
    bd = ImageDraw.Draw(base)
    for g in (TACH, SPEEDO):
        rr = S(g["r_minor"])
        bd.ellipse([S(g["cx"]) - rr, S(g["cy"]) - rr, S(g["cx"]) + rr, S(g["cy"]) + rr],
                   outline=(96, 54, 10, 170), width=max(1, S(1)))

    # sharp amber + red layers for glow
    sharp = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    red = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sharp)
    rd = ImageDraw.Draw(red)

    draw_gauge(sd, rd, TACH, number_size=40, minor_w=2.2, major_w=4.5, label_color=CREAM)
    draw_gauge(sd, rd, SPEEDO, number_size=22, minor_w=1.8, major_w=3.2, label_color=CREAM)
    draw_gauge(sd, rd, FUEL, number_size=0, minor_w=1.8, major_w=3.0, show_numbers=False)

    # tach danger band on the outer rim: orange (caution) -> red (danger), both glow
    def band(v_from, v_to, color):
        a0 = val_to_deg(v_from, TACH["vmin"], TACH["vmax"], TACH["start"], TACH["sweep"]) % 360
        a1 = val_to_deg(v_to, TACH["vmin"], TACH["vmax"], TACH["start"], TACH["sweep"]) % 360
        rb = S(TACH["r_out"] + 2)
        rd.arc([S(TACH["cx"]) - rb, S(TACH["cy"]) - rb, S(TACH["cx"]) + rb, S(TACH["cy"]) + rb],
               a0, a1 if a1 > a0 else a1 + 360, fill=color + (255,), width=S(6))
    band(TACH["redline"], TACH["redline2"], ORANGE_HOT)
    band(TACH["redline2"], TACH["vmax"], RED)

    # tach unit label (bottom gap, below temp sub-dial)
    text_centered(sd, (S(TACH["cx"]), S(TACH["cy"] + 162)), "x1000 r/min",
                  font(MANROPE, S(15)), AMBER + (255,))
    # speedo unit label
    text_centered(sd, (S(SPEEDO["cx"]), S(SPEEDO["cy"] + 54)), "km/h",
                  font(MANROPE, S(13)), AMBER + (255,))
    # fuel E / F
    ep = polar(S(FUEL["cx"]), S(FUEL["cy"]), S(FUEL["r_label"]), FUEL["start"])
    fp = polar(S(FUEL["cx"]), S(FUEL["cy"]), S(FUEL["r_label"]), FUEL["start"] + FUEL["sweep"])
    text_centered(sd, ep, "E", font(MANROPE, S(18)), AMBER + (255,))
    text_centered(sd, fp, "F", font(MANROPE, S(18)), AMBER + (255,))
    icon_fuel(sd, FUEL["cx"], FUEL["cy"] + 10, AMBER)

    # mini gauges
    draw_mini(base, sd, OIL, "L", "H")
    icon_oilcan(sd, OIL["cx"], OIL["cy"] + 24, AMBER)
    draw_mini(base, sd, VOLT, "", "")
    text_centered(sd, (S(VOLT["cx"]), S(VOLT["cy"] - 6)), "18V", font(MANROPE, S(14)), AMBER + (255,))
    icon_battery(sd, VOLT["cx"], VOLT["cy"] + 22, AMBER)
    draw_mini(base, sd, TEMP, "C", "H")
    icon_temp(sd, TEMP["cx"], TEMP["cy"] + 20, AMBER)

    draw_altezza(sd)

    # central hubs (dark caps that sit over needle pivots are drawn by widgets;
    # here just a faint ring so the baked center isn't empty)
    base = composite_glow(base, red, radii=(9, 22), gains=(1.0, 0.7))
    base = composite_glow(base, sharp, radii=(8, 20), gains=(0.85, 0.55))

    # glass sheen over the cover glass of each pod
    glass_highlight(base, TACH["cx"], TACH["cy"], TACH["r_out"] + 6, alpha=22)
    glass_highlight(base, SPEEDO["cx"], SPEEDO["cy"], SPEEDO["r_out"] + 5, alpha=20)
    glass_highlight(base, FUEL["cx"], FUEL["cy"], FUEL["r_out"] + 4, alpha=20)

    draw_lcd(base)
    return base  # big (supersampled) RGBA; caller downscales


# ---------------------------------------------------------------- layout emit
def cx_to_center(px):  # pixel x -> center-origin x
    return int(round(px - 400))

def cy_to_center(py):
    return int(round(py - 240))


def meter_needle(wid, g, signal, *, w_extra=0, needle_w=5, r_mod=-18, ball=20,
                 redline=None):
    cfg = {
        "slot": 0, "signal_name": signal,
        "min": g["vmin"] if "vmin" in g else 0,
        "max": g["vmax"] if "vmax" in g else 100,
        "start_angle": int(g["start"]) % 360,
        "end_angle": int(g["start"] + g["sweep"]) % 360,
        "show_ticks": False, "show_tick_labels": False,
        "meter_bg_opa": 0, "border_width": 0,
        "show_needle": True, "needle_width": needle_w,
        "needle_color": rgb565(NEEDLE_RGB), "needle_r_mod": r_mod,
        "needle_rear_length": int(g["r_out"] * 0.16),
        "needle_tip_style": 3,
        "show_needle_ball": True, "needle_ball_size": ball,
        "needle_ball_color": rgb565((26, 26, 30)),
        "shadow_enabled": True, "shadow_dynamic": True,
        "shadow_offset_x": 3, "shadow_offset_y": 4, "shadow_opa": 110, "shadow_width_extra": 6,
    }
    if redline is not None:
        cfg.update({"redline_enabled": True, "redline_threshold": redline,
                    "redline_color": rgb565(RED), "redline_show_arc": False,
                    "redline_recolor_ticks": False})
    size = 2 * g["r_out"] + w_extra
    return {"type": "meter", "id": wid,
            "x": cx_to_center(g["cx"]), "y": cy_to_center(g["cy"]),
            "w": size, "h": size, "config": cfg}


def draw_needle(base, g, value, vmin=None, vmax=None, *, color=NEEDLE_RGB,
                width_base=8, length_frac=0.9, rear_frac=0.14, hub=15,
                hub_color=(30, 30, 34)):
    """Render a glowing tapered needle onto the (supersampled) base image."""
    import math as _m
    vmin = g.get("vmin", 0) if vmin is None else vmin
    vmax = g.get("vmax", 100) if vmax is None else vmax
    cx, cy = S(g["cx"]), S(g["cy"])
    deg = val_to_deg(value, vmin, vmax, g["start"], g["sweep"])
    a = _m.radians(deg)
    pa = a + _m.pi / 2
    rlen = S(g["r_out"] * length_frac)
    rrear = S(g["r_out"] * rear_frac)
    wb = S(width_base) / 2.0
    tip = (cx + rlen * _m.cos(a), cy + rlen * _m.sin(a))
    rear = (cx - rrear * _m.cos(a), cy - rrear * _m.sin(a))
    bl = (cx + wb * _m.cos(pa), cy + wb * _m.sin(pa))
    br = (cx - wb * _m.cos(pa), cy - wb * _m.sin(pa))
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).polygon([tip, bl, rear, br], fill=color + (255,))
    out = composite_glow(base.copy(), layer, radii=(4, 11), gains=(0.6, 0.35))
    base.paste(out, (0, 0))
    d = ImageDraw.Draw(base)
    d.ellipse([cx - S(hub), cy - S(hub), cx + S(hub), cy + S(hub)],
              fill=hub_color + (255,), outline=(96, 96, 104, 255), width=max(1, S(1)))
    d.ellipse([cx - S(hub * 0.35), cy - S(hub * 0.35), cx + S(hub * 0.35), cy + S(hub * 0.35)],
              fill=(150, 90, 40, 255))


def render_mock(base_big, state, out_path):
    """Composite needles + live speed onto a copy of the baked art (standalone PNG)."""
    b = base_big.copy()
    draw_needle(b, TACH, state["RPM"], width_base=9, hub=16)
    draw_needle(b, SPEEDO, state["VEHICLE_SPEED"], width_base=7, hub=13)
    draw_needle(b, FUEL, state["FUEL_LEVEL"], width_base=5, hub=9, length_frac=0.82)
    draw_needle(b, OIL, state["OIL_PRESS"], 0, 10, width_base=4, hub=7, length_frac=0.78)
    draw_needle(b, VOLT, state["BATT_VOLT"], 8, 18, width_base=4, hub=7, length_frac=0.78)
    draw_needle(b, TEMP, state["COOLANT_TEMP"], 40, 120, width_base=4, hub=7, length_frac=0.78)
    spd = str(int(round(state["VEHICLE_SPEED"])))
    ImageDraw.Draw(b).text((S(699), S(366)), spd, font=font(FUGAZ, S(60)),
                           fill=LCD_AMBER + (255,), anchor="mm")
    b.resize((W, H), Image.LANCZOS).convert("RGB").save(out_path)


def mini_meter(wid, g, signal, vmin, vmax):
    gg = dict(g); gg["vmin"] = vmin; gg["vmax"] = vmax
    return meter_needle(wid, gg, signal, needle_w=3, r_mod=-8, ball=10)


def build_layout():
    signals = [
        {"name": "RPM", "can_id": 256, "bit_start": 0, "bit_length": 16, "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "rpm"},
        {"name": "VEHICLE_SPEED", "can_id": 256, "bit_start": 16, "bit_length": 16, "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "km/h"},
        {"name": "FUEL_LEVEL", "can_id": 257, "bit_start": 0, "bit_length": 8, "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "%"},
        {"name": "BATT_VOLT", "can_id": 257, "bit_start": 8, "bit_length": 16, "scale": 0.1, "offset": 0, "is_signed": False, "endian": 1, "unit": "V"},
        {"name": "OIL_PRESS", "can_id": 257, "bit_start": 24, "bit_length": 8, "scale": 1, "offset": 0, "is_signed": False, "endian": 1, "unit": "bar"},
        {"name": "COOLANT_TEMP", "can_id": 257, "bit_start": 32, "bit_length": 16, "scale": 0.1, "offset": 0, "is_signed": False, "endian": 1, "unit": "degC"},
    ]
    widgets = []
    # index 0: full background
    widgets.append({"type": "image", "id": "bg", "x": 0, "y": 0, "w": 800, "h": 480,
                    "config": {"image_name": "cluster_bg", "auto_size": True}})
    # needles
    widgets.append(meter_needle("tach", TACH, "RPM", needle_w=6, r_mod=-14, ball=26, redline=TACH["redline"]))
    widgets.append(meter_needle("speedo", SPEEDO, "VEHICLE_SPEED", needle_w=5, r_mod=-12, ball=20))
    widgets.append(meter_needle("fuel", FUEL, "FUEL_LEVEL", needle_w=3, r_mod=-8, ball=10))
    widgets.append(mini_meter("oil", OIL, "OIL_PRESS", 0, 10))
    widgets.append(mini_meter("volt", VOLT, "BATT_VOLT", 8, 18))
    widgets.append(mini_meter("temp", TEMP, "COOLANT_TEMP", 40, 120))
    # live digital speed on the LCD (centered, big)
    widgets.append({"type": "text", "id": "lcd_speed",
                    "x": cx_to_center(699), "y": cy_to_center(368),
                    "w": 184, "h": 78,
                    "config": {"slot": 0, "signal_name": "VEHICLE_SPEED", "decimals": 0,
                               "static_text": "", "font": "Fugaz One:60",
                               "text_color": rgb565(LCD_AMBER)}})

    layout = {"schema_version": 14, "name": "Altezza", "screen_w": 800, "screen_h": 480,
              "ecu": "", "ecu_version": "", "signals": signals, "widgets": widgets}
    return layout


def write_manifest():
    return {
        "fonts": [
            {"name": "Fugaz One", "file": "fonts/fugaz_one.ttf"},
        ],
        "images": [{"name": "cluster_bg", "file": "img/cluster_bg.rdmimg"}],
        "layout": "layout.json",
        "signals": {"RPM": 850, "VEHICLE_SPEED": 0, "FUEL_LEVEL": 86,
                    "BATT_VOLT": 14.2, "OIL_PRESS": 4.5, "COOLANT_TEMP": 88},
    }


HERE = REPO / "tools" / "altezza"

IDLE = {"RPM": 850, "VEHICLE_SPEED": 0, "FUEL_LEVEL": 86, "BATT_VOLT": 14.2,
        "OIL_PRESS": 4.5, "COOLANT_TEMP": 88}
DRIVING = {"RPM": 4800, "VEHICLE_SPEED": 128, "FUEL_LEVEL": 64, "BATT_VOLT": 13.9,
           "OIL_PRESS": 6, "COOLANT_TEMP": 95}


def main():
    IMG_OUT.mkdir(parents=True, exist_ok=True)
    FONT_OUT.mkdir(parents=True, exist_ok=True)
    big = build_background()
    final = big.resize((W, H), Image.LANCZOS)
    w, h, n = save_rdmimg(final, IMG_OUT / "cluster_bg.rdmimg")
    final.convert("RGB").save(IMG_OUT / "cluster_bg.png")
    print(f"cluster_bg: {w}x{h}, {n} bytes")
    # standalone hero previews (needles + live speed composited)
    render_mock(big, IDLE, HERE / "preview_idle.png")
    render_mock(big, DRIVING, HERE / "preview_driving.png")
    # runtime font for the live LCD digits (family "Fugaz One" already on device)
    shutil.copyfile(FONT_DIR / FUGAZ, FONT_OUT / "fugaz_one.ttf")
    # layout + manifest
    (OUT / "layout.json").write_text(json.dumps(build_layout(), indent=1))
    (OUT / "manifest.json").write_text(json.dumps(write_manifest(), indent=1))
    print("wrote layout.json + manifest.json + fonts + previews ->", OUT)


if __name__ == "__main__":
    main()
