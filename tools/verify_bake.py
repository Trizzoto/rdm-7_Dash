#!/usr/bin/env python3
"""Verify overlay-bake on Time_Attack.

Perf proof (deterministic sim load): bake_ON-with-shapes should match
shapes-REMOVED (per-frame shape cost eliminated), both faster than
shapes-LIVE-unbaked. Visual proof: baked vs unbaked screenshots at a fixed
needle value should be near-identical (shapes still visible, just in the
cached face). Leaves Time_Attack saved WITH the two shapes baked.
"""
import json, time, copy, io, urllib.request
from PIL import Image, ImageChops

DASH = "http://192.168.4.61"
SIGS = ["RPM", "GEAR", "THROTTLE", "VEHICLE_SPEED", "COOLANT_TEMP", "LAMBDA",
        "INTAKE_AIR_TEMP", "BATTERY_VOLTAGE", "LAMBDA_CORR_A_lambda_corre_2",
        "IGNITION"]
BAKE_IDS = ["shape_panel_1", "shape_panel_2"]


def get(p):
    return urllib.request.urlopen(DASH + p, timeout=10).read()


def post(p, o):
    r = urllib.request.Request(DASH + p, data=json.dumps(o).encode(),
                               headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(r, timeout=20).read()


def variant(base, baked=False, drop_shapes=False):
    lay = copy.deepcopy(base)
    if drop_shapes:
        lay["widgets"] = [w for w in lay["widgets"] if w["id"] not in BAKE_IDS]
    elif baked:
        for w in lay["widgets"]:
            if w["id"] in BAKE_IDS:
                w.setdefault("config", {})["bake_into_gauge"] = True
    return lay


def perf(tag):
    post("/api/signal/inject",
         {"values": [{"signal": s, "value": (4200 if s == "RPM" else 3)}
                     for s in SIGS]})
    post("/api/signal/simulate", {"enabled": False}); time.sleep(0.4)
    post("/api/signal/simulate", {"enabled": True}); time.sleep(5)
    fps, rend = [], []
    for _ in range(20):
        p = json.loads(get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"]); time.sleep(1)
    post("/api/signal/simulate", {"enabled": False})
    r = (sum(fps) / len(fps), sum(rend) / len(rend))
    print(f"{tag:22s} fps={r[0]:5.1f}  render={r[1]:5.1f} ms", flush=True)
    return r


def shot(base, baked, tag):
    post("/api/layout/preview", variant(base, baked=baked)); time.sleep(2)
    post("/api/signal/inject",
         {"values": [{"signal": s, "value": (4200 if s == "RPM" else 3)}
                     for s in SIGS]})
    time.sleep(2)
    d = get("/api/screenshot"); open(f"bake_{tag}.png", "wb").write(d)
    return Image.open(io.BytesIO(d)).convert("RGB")


def main():
    base = json.loads(get("/api/layout/raw?name=Time_Attack").decode("utf-8-sig"))

    print("--- perf (deterministic sim) ---", flush=True)
    post("/api/layout/preview", variant(base)); time.sleep(2)
    r_live = perf("shapes_live_unbaked")
    post("/api/layout/preview", variant(base, drop_shapes=True)); time.sleep(2)
    r_drop = perf("shapes_removed")
    post("/api/layout/preview", variant(base, baked=True)); time.sleep(2)
    r_bake = perf("shapes_BAKED")

    print("\n--- visual parity (fixed needle) ---", flush=True)
    a = shot(base, False, "unbaked")
    b = shot(base, True, "baked")
    diff = ImageChops.difference(a, b)
    tot = sum(1 for px in diff.getdata() if max(px) > 16)
    diff.save("bake_diff.png")
    print(f"unbaked vs baked: diff_px={tot} bbox={diff.getbbox()}", flush=True)

    # cleanup + persist the baked layout as the real Time_Attack
    for s in SIGS:
        post("/api/signal/clear", {"signal": s})
    post("/api/layout/save", variant(base, baked=True))
    post("/api/layout/set", {"name": "Time_Attack"})
    print("\nsaved Time_Attack WITH shapes baked", flush=True)
    print(f"\nSUMMARY: live={r_live[1]:.1f}ms  removed={r_drop[1]:.1f}ms  "
          f"baked={r_bake[1]:.1f}ms  "
          f"(baked≈removed proves per-frame cost gone)", flush=True)


if __name__ == "__main__":
    main()
