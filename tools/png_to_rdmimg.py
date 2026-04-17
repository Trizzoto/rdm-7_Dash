#!/usr/bin/env python3
"""Convert a PNG (or any PIL-supported image) to RDMIMG binary format.

RDMIMG format:
  Header (12 bytes):
    offset 0..3  : magic "RDMI"
    offset 4..5  : width  (uint16 LE)
    offset 6..7  : height (uint16 LE)
    offset 8     : color format (5 = LV_IMG_CF_TRUE_COLOR_ALPHA)
    offset 9..11 : reserved
  Pixel data: width * height * 3 bytes
    per pixel: RGB565 (2 bytes LE) + alpha (1 byte)

Usage:
  python tools/png_to_rdmimg.py <input.png> <output.rdmimg> [--size WxH]
"""
import argparse
import struct
import sys
from pathlib import Path

from PIL import Image


def convert(src_path: Path, dst_path: Path, size: tuple[int, int] | None) -> None:
    im = Image.open(src_path).convert("RGBA")
    if size is not None:
        im = im.resize(size, Image.LANCZOS)

    w, h = im.size
    if w > 65535 or h > 65535:
        sys.exit(f"error: dimensions {w}x{h} exceed uint16 range")

    pixels = im.load()
    out = bytearray(12 + w * h * 3)
    # Header
    out[0:4] = b"RDMI"
    struct.pack_into("<HHB3x", out, 4, w, h, 5)  # width, height, cf=5, reserved

    # Pixel data: RGB565 LE + alpha
    off = 12
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F
            rgb565 = (r5 << 11) | (g6 << 5) | b5
            out[off] = rgb565 & 0xFF
            out[off + 1] = (rgb565 >> 8) & 0xFF
            out[off + 2] = a
            off += 3

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_bytes(out)
    print(f"wrote {dst_path} ({w}x{h}, {len(out)} bytes)")


def parse_size(s: str) -> tuple[int, int]:
    try:
        w, h = s.lower().split("x")
        return int(w), int(h)
    except Exception:
        raise argparse.ArgumentTypeError(f"expected WxH, got {s!r}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("src", type=Path, help="source image (PNG/JPG/...)")
    p.add_argument("dst", type=Path, help="output .rdmimg path")
    p.add_argument("--size", type=parse_size, default=None,
                   help="target size WxH (e.g. 240x124). Default: keep source size.")
    args = p.parse_args()
    convert(args.src, args.dst, args.size)


if __name__ == "__main__":
    main()
