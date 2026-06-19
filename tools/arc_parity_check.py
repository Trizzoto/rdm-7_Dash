#!/usr/bin/env python3
"""Arc tick-bake parity: lock all TA signals, screenshot. Run `ref` before
flashing, `post` after — diff should be ~zero."""
import json, sys, time, io, urllib.request
from PIL import Image, ImageChops

DASH = "http://192.168.4.61"
SIGS = ["RPM", "GEAR", "THROTTLE", "VEHICLE_SPEED", "COOLANT_TEMP",
        "LAMBDA", "INTAKE_AIR_TEMP", "BATTERY_VOLTAGE",
        "LAMBDA_CORR_A_lambda_corre_2", "IGNITION"]
VALS = {"RPM": 4200, "VEHICLE_SPEED": 87, "COOLANT_TEMP": 91, "THROTTLE": 42}


def get(p):
    return urllib.request.urlopen(DASH + p, timeout=10).read()


def post(p, o):
    r = urllib.request.Request(DASH + p, data=json.dumps(o).encode(),
                               headers={"Content-Type": "application/json"})
    urllib.request.urlopen(r, timeout=20).read()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "ref"
    post("/api/layout/set", {"name": "Time_Attack"})
    time.sleep(3)
    post("/api/signal/inject",
         {"values": [{"signal": s, "value": VALS.get(s, 3)} for s in SIGS]})
    time.sleep(2)
    d = get("/api/screenshot")
    fn = f"arcpar_{mode}.png"
    open(fn, "wb").write(d)
    print(f"saved {fn}", flush=True)
    if mode == "post":
        a = Image.open("arcpar_ref.png").convert("RGB")
        b = Image.open(io.BytesIO(d)).convert("RGB")
        diff = ImageChops.difference(a, b)
        tot = sum(1 for px in diff.getdata()
                  if px[0] > 16 or px[1] > 16 or px[2] > 16)
        print(f"diff_px={tot} bbox={diff.getbbox()}", flush=True)
        diff.save("arcpar_diff.png")
    for s in SIGS:
        post("/api/signal/clear", {"signal": s})
    print("locks cleared", flush=True)


if __name__ == "__main__":
    main()
