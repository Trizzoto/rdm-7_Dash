#!/usr/bin/env python3
"""RGB565 <-> hex helper for layout colours (config *_color fields are RGB565
integers).

    python rgb565.py "#31C2F7"     -> 13854
    python rgb565.py 13854         -> #30C0F0  (rounded back)
    python rgb565.py               -> print the named house palette
"""
import sys

PALETTE = {
    "white": "#FFFFFF", "black": "#000000", "red": "#FF0000", "green": "#00FF00",
    "blue": "#0000FF", "cyan": "#00FFFF", "yellow": "#FFFF00", "orange": "#FF8000",
    "magenta": "#FF00FF", "grey": "#808080",
    # house palette
    "accent_cyan": "#31C2F7", "track_dark": "#08384A", "redline": "#E14129",
    "label_grey": "#8B95A4", "dim_grey": "#6A7583",
}


def hex_to_565(h):
    h = h.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _565_to_hex(v):
    v = int(v) & 0xFFFF
    r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    r, g, b = (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)
    return "#%02X%02X%02X" % (r, g, b)


def main(argv):
    if len(argv) < 2:
        print("Named house palette (name -> hex -> RGB565):")
        for name, hx in PALETTE.items():
            print("  %-12s %s  %d" % (name, hx, hex_to_565(hx)))
        return
    arg = argv[1].strip()
    if arg in PALETTE:
        arg = PALETTE[arg]
    if arg.startswith("#") or (len(arg) == 6 and all(c in "0123456789abcdefABCDEF" for c in arg)):
        print(hex_to_565(arg))
    else:
        print(_565_to_hex(int(arg)))


if __name__ == "__main__":
    main(sys.argv)
