#!/usr/bin/env python3
"""Deterministic Time_Attack benchmark: lock TA's CAN signals against the
live bus, drive everything with the on-device simulator, measure long-window
fps + render time. Run identically across firmware builds to compare."""
import json, time, urllib.request

DASH = "http://192.168.4.61"
WINDOW_S = 40

TA_SIGNALS = ["RPM", "GEAR", "THROTTLE", "VEHICLE_SPEED", "COOLANT_TEMP",
              "LAMBDA", "INTAKE_AIR_TEMP", "BATTERY_VOLTAGE",
              "LAMBDA_CORR_A_lambda_corre_2", "IGNITION"]


def http_get(path):
    with urllib.request.urlopen(DASH + path, timeout=10) as r:
        return r.read()


def http_post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def main():
    http_post("/api/layout/set", {"name": "Time_Attack"})
    time.sleep(3)
    # lock TA signals so live CAN can't write them (sim uses the test path)
    http_post("/api/signal/inject",
              {"values": [{"signal": s, "value": 1} for s in TA_SIGNALS]})
    http_post("/api/signal/simulate", {"enabled": False})
    time.sleep(0.5)
    http_post("/api/signal/simulate", {"enabled": True})
    time.sleep(5)

    fps, rend, px = [], [], []
    for _ in range(WINDOW_S):
        p = json.loads(http_get("/api/perf"))
        fps.append(p["fps"]); rend.append(p["avg_render_ms"])
        px.append(p["avg_px"])
        time.sleep(1)
    print(f"DETERMINISTIC: fps_avg={sum(fps)/len(fps):.1f} "
          f"min={min(fps)} max={max(fps)} "
          f"render_ms_avg={sum(rend)/len(rend):.2f} "
          f"avg_px={sum(px)//len(px)}", flush=True)

    http_post("/api/signal/simulate", {"enabled": False})
    http_post("/api/signal/clear", {"all": True})
    for s in TA_SIGNALS:
        http_post("/api/signal/clear", {"signal": s})
    print("cleaned up", flush=True)


if __name__ == "__main__":
    main()
