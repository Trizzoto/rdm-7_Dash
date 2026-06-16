"""DEPRECATED back-compat shim.

The cluster toolkit now lives in `tools/clusters/cluster_lib.py`, and the cluster
designs (Altezza/KTM/Mercedes) in `tools/clusters/designs/`. Build with:

    python tools/clusters/gen.py altezza            # bake -> tools/clusters/dist/
    python tools/clusters/gen.py altezza --preview  # + WASM browser-preview bundle
    python tools/clusters/deploy.py altezza          # upload + activate + screenshot

This module is kept only so the Altezza perf-experiment scripts in this folder
(gen_crops.py / gen_native.py / gen_surround.py) keep importing `save_rdmimg`
etc. New code should import from `cluster_lib` directly.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "clusters"))

from cluster_lib import *           # noqa: F401,F403,E402
from cluster_lib import (           # noqa: E402  (explicit re-export)
    SS, REPO, OUT, IMG_OUT, FONT_OUT, FONT_DIR,
    FACE, FACE_EDGE, HOUSING, HOUSING_HI, BEZEL_HI, BEZEL_LO, CHROME_LIP,
    AMBER, AMBER_HOT, AMBER_DIM, ORANGE_HOT, RED, RED_HOT, NEEDLE_RGB, CREAM,
    LCD_AMBER, LCD_GHOST, LCD_BG, WHITE,
    rgb565, save_rdmimg, font, MANROPE, FUGAZ, MONT,
    polar, val_to_deg, text_centered, thick_line, composite_glow,
    radial_alpha, brushed_noise, vgrad_alpha, bloom,
)
