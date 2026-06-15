"""Add the surround corner pieces (ALTEZZA / TRC OFF / x1000 r/min / icons) that
fall in the gaps between the square meter footprints. Each is a static image
widget placed ONLY in a gap (no overlap with any meter bbox), so it's drawn once
and never redrawn under a needle = free. Builds on altezza_crops.json.
Run: python tools/altezza/gen_surround.py
"""
import json
from pathlib import Path
from PIL import Image
from altezza_lib import save_rdmimg

HERE = Path(__file__).resolve().parent
CROPDIR = HERE / "crops"
W, H = 800, 480

# gap rectangles (screen px) that contain art but don't overlap tach[218-612,24-418],
# speedo[65-285,127-347] or fuel[599-765,94-260]. id, x0,y0,x1,y1
GAPS = [
    ("sur_bl", 0,   348, 217, 480),   # bottom-left: TRC OFF, x1000 r/min
    ("sur_br", 613, 261, 800, 480),   # bottom-right: ALTEZZA
    ("sur_tl", 0,   0,   217, 126),   # top-left
    ("sur_tr", 613, 0,   800, 93),    # top-right: check-engine icon
]


def main():
    src = Image.open(HERE / "hks.png").convert("RGBA")
    sw, sh = src.size
    z = min(W / sw, H / sh)
    dw, dh = int(round(sw * z)), int(round(sh * z))
    bg = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    bg.paste(src.resize((dw, dh), Image.LANCZOS), ((W - dw) // 2, (H - dh) // 2))

    layout = json.loads((HERE / "altezza_crops.json").read_text(encoding="utf-8-sig"))
    uploads = json.loads((HERE / "crops_manifest.json").read_text())  # extend existing
    new_widgets = []
    for gid, x0, y0, x1, y1 in GAPS:
        crop = bg.crop((x0, y0, x1, y1))
        path = CROPDIR / (gid + ".rdmimg")
        save_rdmimg(crop, path)
        uploads.append([gid, str(path)])
        cx = (x0 + x1) // 2 - 400
        cy = (y0 + y1) // 2 - 240
        new_widgets.append({"type": "image", "id": gid, "x": cx, "y": cy,
                            "w": x1 - x0, "h": y1 - y0,
                            "config": {"image_name": gid, "auto_size": True}})
    # surround pieces first (behind), then the existing meters/text on top
    layout["widgets"] = new_widgets + layout["widgets"]
    (HERE / "altezza_crops_full.json").write_text(json.dumps(layout))
    (HERE / "crops_manifest.json").write_text(json.dumps(uploads))
    print("added surround:", [g[0] for g in GAPS])

if __name__ == "__main__":
    main()
