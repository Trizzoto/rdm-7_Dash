#!/usr/bin/env python3
"""
rdm_unpack.py — unpack an RDM .rdm bundle into its layout JSON + assets.

.rdm binary format (see main/web/index.html exportRdm):
  Header (16 bytes): "RDM1" | u16 LE version(1) | u16 LE entry_count | 8x reserved
  Per entry: u8 type(0=layout,1=image,2=font) | u8 name_len | name | u32 LE data_len | data

Usage:
    python tools/rdm_unpack.py <file.rdm> [file2.rdm ...] --out <dir> [--report]

Writes:  <dir>/layouts/<name>.json, <dir>/fonts/<family>.ttf, <dir>/images/<name>.rdmimg
Always prints an inventory (name, sizes, fonts, widget count).
Pure stdlib.
"""
import argparse
import json
import os
import struct
import sys

TYPE_LAYOUT, TYPE_IMAGE, TYPE_FONT = 0, 1, 2
# Fonts already seeded by boot_assets.c — never need re-bundling.
BUILTIN_FONTS = {"Montserrat", "Fugaz One", "Manrope Bold"}


def unpack(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RDM1":
        raise ValueError(f"{path}: not a .rdm bundle (bad magic)")
    version, count = struct.unpack_from("<HH", data, 4)
    if version != 1:
        raise ValueError(f"{path}: unsupported .rdm version {version}")
    off = 16
    entries = []
    for _ in range(count):
        etype = data[off]; off += 1
        nlen = data[off]; off += 1
        name = data[off:off + nlen].decode("utf-8"); off += nlen
        (dlen,) = struct.unpack_from("<I", data, off); off += 4
        blob = data[off:off + dlen]; off += dlen
        entries.append((etype, name, blob))
    return version, entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", default=None, help="output dir (omit for report-only)")
    args = ap.parse_args()

    all_fonts = {}   # family -> size
    for path in args.files:
        _, entries = unpack(path)
        layout_name = None
        fonts, images = [], []
        widget_count = None
        for etype, name, blob in entries:
            if etype == TYPE_LAYOUT:
                layout_name = name
                try:
                    widget_count = len(json.loads(blob.decode("utf-8")).get("widgets", []))
                except Exception:
                    widget_count = "?"
                if args.out:
                    d = os.path.join(args.out, "layouts")
                    os.makedirs(d, exist_ok=True)
                    with open(os.path.join(d, f"{name}.json"), "wb") as o:
                        o.write(blob)
            elif etype == TYPE_FONT:
                fonts.append((name, len(blob)))
                all_fonts[name] = len(blob)
                if args.out and name not in BUILTIN_FONTS:
                    d = os.path.join(args.out, "fonts")
                    os.makedirs(d, exist_ok=True)
                    with open(os.path.join(d, f"{name}.ttf"), "wb") as o:
                        o.write(blob)
            elif etype == TYPE_IMAGE:
                images.append((name, len(blob)))
                if args.out:
                    d = os.path.join(args.out, "images")
                    os.makedirs(d, exist_ok=True)
                    with open(os.path.join(d, f"{name}.rdmimg"), "wb") as o:
                        o.write(blob)

        print(f"\n=== {os.path.basename(path)} ===")
        print(f"  layout : {layout_name}  (widgets: {widget_count})")
        if fonts:
            for fn, sz in fonts:
                tag = " [BUILT-IN, skip]" if fn in BUILTIN_FONTS else " [bundle]"
                print(f"  font   : {fn:20s} {sz/1024:7.1f} KB{tag}")
        else:
            print("  font   : (none)")
        if images:
            for im, sz in images:
                print(f"  image  : {im:20s} {sz/1024:7.1f} KB")

    print("\n--- unique fonts across all bundles ---")
    total_new = 0
    for fam, sz in sorted(all_fonts.items()):
        if fam in BUILTIN_FONTS:
            print(f"  {fam:20s} {sz/1024:7.1f} KB  [built-in]")
        else:
            print(f"  {fam:20s} {sz/1024:7.1f} KB  [NEW -> embed]")
            total_new += sz
    print(f"\n  new font flash cost: {total_new/1024:.1f} KB")


if __name__ == "__main__":
    main()
