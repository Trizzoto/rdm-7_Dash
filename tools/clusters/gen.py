"""Build CLI for the RDM-7 cluster recreations.

  python tools/clusters/gen.py mercedes              # build one
  python tools/clusters/gen.py mercedes ktm          # build several
  python tools/clusters/gen.py all                   # build every design
  python tools/clusters/gen.py altezza --preview     # + WASM browser-preview bundle
  python tools/clusters/gen.py mercedes --mock        # + standalone composited preview PNG

Each design writes to tools/clusters/dist/<name>/:
  <IMAGE_NAME>.rdmimg   baked background (opaque -> firmware fast path)
  <IMAGE_NAME>.png      eyeball copy
  <name>_layout.json    layout that overlays the live widgets
  meta.json             {image_name, layout_file, states, ...} consumed by deploy.py
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from PIL import Image  # noqa: E402
from cluster_lib import save_rdmimg  # noqa: E402
import designs  # noqa: E402

DIST = HERE / "dist"


def build_one(name, *, preview=False, mock=False):
    mod = designs.get(name)
    out = DIST / name
    out.mkdir(parents=True, exist_ok=True)

    big = mod.build_background()
    final = big.resize((800, 480), Image.LANCZOS).convert("RGBA")
    final.putalpha(255)  # opaque -> firmware opaque-image fast path
    final.convert("RGB").save(out / f"{mod.IMAGE_NAME}.png")
    w, h, n = save_rdmimg(final, out / f"{mod.IMAGE_NAME}.rdmimg")

    layout = mod.build_layout()
    (out / f"{name}_layout.json").write_text(json.dumps(layout, separators=(",", ":")))

    meta = {
        "name": name,
        "image_name": mod.IMAGE_NAME,
        "image_file": f"{mod.IMAGE_NAME}.rdmimg",
        "layout_file": f"{name}_layout.json",
        "states": getattr(mod, "STATES", {}),
        "default_state": getattr(mod, "DEFAULT_STATE", None),
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=1))

    print(f"[{name}] bg {w}x{h} {n} B + layout -> {out}")

    if mock and hasattr(mod, "render_mock"):
        for sname, state in getattr(mod, "STATES", {}).items():
            p = out / f"preview_{sname}.png"
            mod.render_mock(big, state, p)
            print(f"[{name}] mock {sname} -> {p}")
    if preview and hasattr(mod, "emit_preview"):
        mod.emit_preview(big)


def main():
    ap = argparse.ArgumentParser(description="Build RDM-7 cluster recreations.")
    ap.add_argument("targets", nargs="+", help=f"cluster name(s) or 'all'. known: {', '.join(designs.names())}")
    ap.add_argument("--preview", action="store_true", help="also emit the WASM browser-preview bundle (designs that support it)")
    ap.add_argument("--mock", action="store_true", help="also render standalone composited preview PNGs (designs that support it)")
    args = ap.parse_args()

    targets = designs.names() if "all" in args.targets else args.targets
    for name in targets:
        build_one(name, preview=args.preview, mock=args.mock)


if __name__ == "__main__":
    main()
