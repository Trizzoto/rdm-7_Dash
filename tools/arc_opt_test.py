#!/usr/bin/env python3
"""Quantify arc optimizations on Time_Attack without removing the arcs.
Isolates: (1) the ARC FUEL=RPM fast-signal binding, (2) the 600px radius."""
import json, copy, time, urllib.request

DASH = "http://192.168.4.61"


def get(p):
    return urllib.request.urlopen(DASH + p, timeout=10).read()


def post(p, o):
    r = urllib.request.Request(DASH + p, data=json.dumps(o).encode(),
                               headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(r, timeout=20).read()


def measure(tag, secs=25):
    time.sleep(5)
    fps, rend = [], []
    for _ in range(secs):
        p = json.loads(get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"]); time.sleep(1)
    print(f"{tag:34s} fps={sum(fps)/len(fps):5.1f}  render={sum(rend)/len(rend):5.1f} ms",
          flush=True)


def main():
    base = json.loads(get("/api/layout/raw?name=Time_Attack").decode("utf-8-sig"))

    post("/api/layout/preview", base); measure("original (ARC FUEL=RPM, 600px)")

    # (1) rebind ARC FUEL off the fast RPM signal onto a slow one
    v1 = copy.deepcopy(base)
    for w in v1["widgets"]:
        if w["id"] == "ARC FUEL":
            w["config"]["signal_name"] = "COOLANT_TEMP"
            w["config"].pop("channel", None)
            w["signal"] = "COOLANT_TEMP"
            w.pop("channel", None)
    post("/api/layout/preview", v1); measure("ARC FUEL rebound RPM->coolant")

    # (2) shrink both 600px arcs to 360px (radius 300->180)
    v2 = copy.deepcopy(base)
    for w in v2["widgets"]:
        if w["id"] in ("ARC COOLANT", "ARC FUEL"):
            w["w"] = w["h"] = 360
    post("/api/layout/preview", v2); measure("both 600px arcs -> 360px")

    # (3) both fixes together
    v3 = copy.deepcopy(v1)
    for w in v3["widgets"]:
        if w["id"] in ("ARC COOLANT", "ARC FUEL"):
            w["w"] = w["h"] = 360
    post("/api/layout/preview", v3); measure("rebound + shrunk (both)")

    post("/api/layout/set", {"name": "Time_Attack_Fast"})
    print("restored active = Time_Attack_Fast", flush=True)


if __name__ == "__main__":
    main()
