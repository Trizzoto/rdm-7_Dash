#!/usr/bin/env python3
"""Banner preview-sticky verification.

Reproduces the editor flow that broke: enable a banner's "Show" preview,
then move/restyle something (which pushes /api/layout/preview and rebuilds
all widgets, wiping the banner's runtime test_override). Confirms the
firmware contract the JS fix relies on: re-sending /api/banner/test AFTER
a preview rebuild re-shows the banner.

  1. preview a layout with a banner whose condition is OFF (op=GT, hi thresh)
  2. /api/banner/test active -> banner visible      (shot: shown)
  3. /api/layout/preview again (= a move)           (shot: hidden_after_move)
  4. /api/banner/test active again (the re-assert)  (shot: reshown)
"""
import json, sys, time, urllib.request

DASH = "http://192.168.4.61"

def post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()

def shot(name):
    with urllib.request.urlopen(DASH + "/api/screenshot", timeout=15) as r:
        png = r.read()
    with open(name, "wb") as f:
        f.write(png)
    print(f"saved {name}", flush=True)

BANNER = {
    "type": "banner", "id": "banner_0", "x": 0, "y": 0, "w": 800, "h": 90,
    "config": {"text": "PREVIEW ME", "op": 0, "threshold": 99999,
               "signal_name": "COOLANT_TEMP", "bg_color": 0x07E0,
               "bg_opa": 220, "text_color": 0x000000},
    "signal": "COOLANT_TEMP",
}
# A second widget we "move" between preview pushes to force the rebuild.
MARKER = {"type": "text", "id": "marker", "x": 0, "y": -180, "w": 120, "h": 40,
          "config": {"static_text": "X", "value_color": 0xFFFFFF}}

LAY = {"schema_version": 14, "name": "Banner_Test", "screen_w": 800,
       "screen_h": 480, "signals": []}

def layout(marker_y):
    m = dict(MARKER); m = {**MARKER, "y": marker_y}
    return {**LAY, "widgets": [BANNER, m]}

def main(out):
    post("/api/layout/preview", layout(-180))
    time.sleep(2)
    # 2: force-show
    post("/api/banner/test", {"id": "banner_0", "active": True})
    time.sleep(1.2)
    shot(f"{out}/banner_shown.png")
    # 3: a "move" = another full preview push, which rebuilds widgets
    post("/api/layout/preview", layout(-150))
    time.sleep(2)
    shot(f"{out}/banner_hidden_after_move.png")
    # 4: the re-assert the JS fix performs after every preview push
    post("/api/banner/test", {"id": "banner_0", "active": True})
    time.sleep(1.2)
    shot(f"{out}/banner_reshown.png")
    print("done", flush=True)

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
