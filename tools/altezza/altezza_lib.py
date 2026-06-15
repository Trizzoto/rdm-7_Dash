"""Shared helpers for the Toyota Altezza cluster recreation.

- RGB565 conversion (firmware layout colors are raw RGB565 decimals)
- .rdmimg writer (numpy-vectorised; matches tools/png_to_rdmimg.py byte format)
- gauge geometry (LVGL angle convention: 0deg = 3 o'clock, CW because screen Y is down)
- EL-glow drawing helpers (supersampled + gaussian bloom)
"""
import math
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# ---------------------------------------------------------------- paths
REPO = Path(__file__).resolve().parents[2]          # ...\workspace\RDM-7_Dash
FONT_DIR = REPO / "main" / "embed" / "fonts"
OUT = REPO.parent / "rdm7-wasm-editor" / "altezza"  # served by the preview harness
IMG_OUT = OUT / "img"
FONT_OUT = OUT / "fonts"

SS = 3  # supersample factor for crisp anti-aliasing

# ---------------------------------------------------------------- colors (RGB888)
FACE       = (7, 7, 8)        # near-black optitron face
FACE_EDGE  = (16, 16, 19)
HOUSING    = (12, 12, 14)
HOUSING_HI = (34, 34, 39)
BEZEL_HI   = (124, 126, 136)  # chrome highlight
BEZEL_LO   = (6, 6, 8)
CHROME_LIP = (168, 170, 182)
AMBER      = (255, 146, 16)   # main numerals / ticks
AMBER_HOT  = (255, 196, 96)   # bright core
AMBER_DIM  = (120, 62, 8)     # unlit / faint
ORANGE_HOT = (255, 110, 18)   # caution tier (just below redline)
RED        = (255, 52, 20)    # redline
RED_HOT    = (255, 120, 70)
NEEDLE_RGB = (255, 94, 42)    # red-orange needle
CREAM      = (255, 230, 192)  # brightly-lit numerals
LCD_AMBER  = (255, 138, 0)
LCD_GHOST  = (44, 22, 2)      # unlit LCD segments
LCD_BG     = (14, 9, 2)
WHITE      = (240, 240, 240)


def rgb565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def save_rdmimg(im: Image.Image, path: Path):
    """Write an RGBA PIL image to .rdmimg (RGB565 LE + alpha)."""
    im = im.convert("RGBA")
    w, h = im.size
    arr = np.asarray(im, dtype=np.uint16)          # h,w,4
    r = (arr[:, :, 0] >> 3) & 0x1F
    g = (arr[:, :, 1] >> 2) & 0x3F
    b = (arr[:, :, 2] >> 3) & 0x1F
    a = arr[:, :, 3].astype(np.uint8)
    p565 = (r << 11) | (g << 5) | b                # h,w uint16
    lo = (p565 & 0xFF).astype(np.uint8)
    hi = (p565 >> 8).astype(np.uint8)
    pix = np.dstack([lo, hi, a]).reshape(-1)        # interleaved
    out = bytearray(12 + w * h * 3)
    out[0:4] = b"RDMI"
    struct.pack_into("<HHB3x", out, 4, w, h, 5)
    out[12:] = pix.tobytes()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))
    return w, h, len(out)


# ---------------------------------------------------------------- fonts
_font_cache = {}

def font(name: str, size: int):
    key = (name, size)
    if key not in _font_cache:
        _font_cache[key] = ImageFont.truetype(str(FONT_DIR / name), size)
    return _font_cache[key]

MANROPE = "manrope_bold.ttf"
FUGAZ = "fugaz_one.ttf"
MONT = "montserrat.ttf"


# ---------------------------------------------------------------- geometry
def polar(cx, cy, r, deg):
    """LVGL angle convention: 0=+x (3 o'clock), increasing clockwise (screen Y down)."""
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def val_to_deg(v, vmin, vmax, start, sweep):
    t = 0.0 if vmax == vmin else (v - vmin) / (vmax - vmin)
    t = max(0.0, min(1.0, t))
    return start + sweep * t


# ---------------------------------------------------------------- drawing helpers
def text_centered(draw, xy, text, fnt, fill, anchor="mm"):
    draw.text(xy, text, font=fnt, fill=fill, anchor=anchor)


def thick_line(draw, p0, p1, color, width):
    draw.line([p0, p1], fill=color, width=int(round(width)))
    # round the cap ends a touch
    r = width / 2.0
    for p in (p0, p1):
        draw.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=color)


def radial_alpha(r, inner=0, outer=130, color=(0, 0, 0), power=2.2):
    """RGBA disc: `color` with alpha ramping inner->outer along radius (for vignette)."""
    yy, xx = np.mgrid[0:2 * r, 0:2 * r]
    d = np.sqrt((xx - r) ** 2 + (yy - r) ** 2) / r
    d = np.clip(d, 0, 1) ** power
    a = (inner + (outer - inner) * d).astype(np.uint8)
    img = np.zeros((2 * r, 2 * r, 4), np.uint8)
    img[:, :, 0], img[:, :, 1], img[:, :, 2] = color
    img[:, :, 3] = a
    return Image.fromarray(img, "RGBA")


def vgrad_alpha(w, h, a0, a1, color=(0, 0, 0)):
    """Vertical alpha gradient (a0 at top -> a1 at bottom), constant color. RGBA."""
    col = np.zeros((h, w, 4), np.uint8)
    col[:, :, 0], col[:, :, 1], col[:, :, 2] = color
    a = np.linspace(a0, a1, h).astype(np.uint8)[:, None]
    col[:, :, 3] = np.broadcast_to(a, (h, w))
    return Image.fromarray(col, "RGBA")


def brushed_noise(size, amp=5, seed=7):
    """Subtle metallic/plastic grain as a faint additive overlay (RGBA)."""
    rng = np.random.default_rng(seed)
    w, h = size
    n = rng.normal(0, amp, (h, w))
    # slight horizontal streak (brushed)
    n = (n + np.roll(n, 1, axis=1) + np.roll(n, 2, axis=1)) / 3.0
    v = np.clip(128 + n, 0, 255).astype(np.uint8)
    img = np.zeros((h, w, 4), np.uint8)
    img[:, :, 0] = img[:, :, 1] = img[:, :, 2] = v
    img[:, :, 3] = 14  # very faint
    return Image.fromarray(img, "RGBA")


def bloom(layer: Image.Image, radius: float) -> Image.Image:
    """Return a blurred, warmed copy of the bright `layer` for EL glow."""
    return layer.filter(ImageFilter.GaussianBlur(radius))


def composite_glow(base: Image.Image, sharp: Image.Image, radii=(10, 26), gains=(1.0, 0.7)):
    """Composite multi-radius bloom of `sharp` under `sharp` onto `base`. All RGBA."""
    for radius, gain in zip(radii, gains):
        b = bloom(sharp, radius * SS)
        if gain != 1.0:
            a = b.split()[3].point(lambda v: int(v * gain))
            b.putalpha(a)
        base = Image.alpha_composite(base, b)
    base = Image.alpha_composite(base, sharp)
    return base
