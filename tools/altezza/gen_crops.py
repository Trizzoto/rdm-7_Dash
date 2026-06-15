"""Test the cache-locality hypothesis: give each meter a SMALL per-gauge crop of
the HKS art as its bg_image (meter-sized, cache-friendly) instead of one big
full-screen image. Preserves the art exactly (crop = what's on screen under each
meter) but may render far faster. Produces crop .rdmimg files + altezza_crops.json.
Run: python tools/altezza/gen_crops.py
"""
import json
from pathlib import Path
from PIL import Image
from altezza_lib import save_rdmimg

HERE = Path(__file__).resolve().parent
HKS = HERE / "hks.png"
LAY = HERE / "altezza_noshadow.json"
CROPDIR = HERE / "crops"
CROPDIR.mkdir(exist_ok=True)

W, H = 800, 480

def main():
    src = Image.open(HKS).convert("RGBA")          # 792x445
    # reproduce the on-screen background: auto_size fit into 800x480
    sw, sh = src.size
    z = min(W / sw, H / sh)
    dw, dh = int(round(sw * z)), int(round(sh * z))
    bg = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    bg.paste(src.resize((dw, dh), Image.LANCZOS), ((W - dw) // 2, (H - dh) // 2))

    layout = json.loads(LAY.read_text(encoding="utf-8-sig"))
    uploads = []
    widgets = []
    for w in layout["widgets"]:
        if w["type"] == "image" and w.get("id") == "bg":
            continue
        if w["type"] == "meter":
            pcx = 400 + w["x"]; pcy = 240 + w["y"]
            ww = w["w"]; hh = w["h"]
            box = (pcx - ww // 2, pcy - hh // 2, pcx - ww // 2 + ww, pcy - hh // 2 + hh)
            crop = bg.crop(box)
            name = "face_" + w["id"]
            path = CROPDIR / (name + ".rdmimg")
            save_rdmimg(crop, path)
            uploads.append((name, str(path)))
            c = w["config"]
            c.update({"meter_bg_opa": 255, "bg_image_name": name,
                      "show_ticks": False, "show_tick_labels": False, "static_ticks": False})
        widgets.append(w)
    layout["widgets"] = widgets
    (HERE / "altezza_crops.json").write_text(json.dumps(layout))
    # emit upload manifest for the PS uploader
    (HERE / "crops_manifest.json").write_text(json.dumps(uploads))
    print("crops:", [u[0] for u in uploads])
    print("wrote altezza_crops.json")

if __name__ == "__main__":
    main()
