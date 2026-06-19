#!/usr/bin/env python3
"""Bar widget configuration test bench.

Phases (pick with argv[1]):
  poison   - preview a center-fill bar bound to boost_pressure, then report
             whether the channel's low/high warns got silently adopted from
             the widget defaults (the "blue below zero" bug).
  clean    - clear boost_pressure low/high warns.
  verify   - drive boost negative via MAP/BARO injection and screenshot:
             no warn set -> must stay in-range colour; low_warn=-4 -> blue
             only below -4. Saves PNGs to argv[2].
  styles   - apply a 7-bar styling matrix layout, screenshot, then check
             every config field survives the firmware from_json -> to_json
             round trip via /api/layout/current.
"""
import json, sys, time, urllib.request

DASH = "http://192.168.4.61"

def get(path):
    with urllib.request.urlopen(DASH + path, timeout=15) as r:
        return json.loads(r.read())

def get_bin(path):
    with urllib.request.urlopen(DASH + path, timeout=15) as r:
        return r.read()

def post(path, obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(DASH + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()

def boost_channel():
    for c in get("/api/channels")["channels"]:
        if c["id"] == "boost_pressure":
            return c
    raise SystemExit("boost_pressure channel missing")

def inject(vals):
    post("/api/signal/inject",
         {"values": [{"signal": s, "value": v} for s, v in vals.items()]})
    time.sleep(1.2)

def shot(name):
    png = get_bin("/api/screenshot")
    with open(name, "wb") as f:
        f.write(png)
    print(f"saved {name}", flush=True)

# Boost center bar: channel-bound so range comes from the channel (-100..300).
BOOST_BAR = {
    "type": "bar", "id": "bar_boost", "x": 0, "y": -180, "w": 540, "h": 54,
    "config": {"label": "BOOST", "center_fill": True, "show_bar_value": True,
               "channel": "boost_pressure", "signal_name": "MATH_BOOST_PRESSURE",
               "bar_in_range_color": 0x07E0, "bar_radius": 8,
               "indicator_radius": 6},
    "signal": "MATH_BOOST_PRESSURE",
}

# Styling matrix - all bound to COOLANT_TEMP (locked at 90), range 0..130.
def style_bar(idx, x, y, w, h, cfg):
    base = {"label": f"S{idx}", "bar_min": 0, "bar_max": 130,
            "signal_name": "COOLANT_TEMP", "show_bar_value": True}
    base.update(cfg)
    return {"type": "bar", "id": f"bar_s{idx}", "x": x, "y": y, "w": w,
            "h": h, "config": base, "signal": "COOLANT_TEMP"}

STYLE_BARS = [
    style_bar(1, -200, -90, 360, 46,
              {"bar_radius": 0, "indicator_radius": 0, "bar_border_width": 3,
               "bar_border_color": 0xFFE0, "bar_bg_color": 0x0010,
               "bar_in_range_color": 0xFC00}),          # square, yellow border
    style_bar(2, 200, -90, 360, 46,
              {"bar_radius": 23, "indicator_radius": 18,
               "bar_bg_color": 0x4208, "bar_border_width": 0,
               "bar_in_range_color": 0x07FF}),          # pill, cyan fill
    style_bar(3, -200, -10, 360, 46,
              {"bar_radius": 23, "indicator_radius": 0,
               "bar_border_width": 1, "bar_border_color": 0xFFFF}),  # mismatched radii
    style_bar(4, 200, -10, 360, 46,
              {"show_ticks": True, "tick_count": 7, "tick_length": 8,
               "tick_width": 2, "tick_color": 0xFFFF, "tick_side": 2,
               "bar_radius": 4}),                       # ticks both sides
    style_bar(5, -200, 70, 360, 46,
              {"invert_bar_value": True, "decimals": 1,
               "bar_bg_opa": 80, "bar_in_range_color": 0xF81F}),  # invert, translucent
    style_bar(6, 330, 110, 26, 200,
              {"center_fill": True, "bar_min": -10, "bar_max": 10,
               "bar_radius": 0, "indicator_radius": 0,
               "bar_in_range_color": 0x07E0}),          # vertical center
]

LAYOUT_BASE = {"schema_version": 14, "name": "Bar_Test",
               "screen_w": 800, "screen_h": 480, "signals": []}

def phase_poison():
    c = boost_channel()
    print("before:", {k: c.get(k) for k in ("low_warn", "high_warn")}, flush=True)
    lay = dict(LAYOUT_BASE); lay["widgets"] = [BOOST_BAR]
    post("/api/layout/preview", lay)
    time.sleep(2)
    c = boost_channel()
    print("after preview:", {k: c.get(k) for k in ("low_warn", "high_warn")},
          flush=True)
    lw, hw = c.get("low_warn"), c.get("high_warn")
    print("POISONED" if (lw is not None or hw is not None) else "CLEAN", flush=True)

def phase_clean():
    post("/api/channels/update",
         {"id": "boost_pressure", "fields": {"low_warn": None, "high_warn": None}})
    c = boost_channel()
    print("cleaned:", {k: c.get(k) for k in ("low_warn", "high_warn")}, flush=True)

def phase_verify(outdir):
    lay = dict(LAYOUT_BASE); lay["widgets"] = [BOOST_BAR]
    post("/api/layout/preview", lay)
    time.sleep(2)
    c = boost_channel()
    print("warns after preview:", {k: c.get(k) for k in ("low_warn", "high_warn")},
          flush=True)
    # negative boost, no warn set -> must stay in-range green
    inject({"MAP": 40, "BARO_PRESSURE": 100})       # boost = -60 kPa
    time.sleep(1)
    shot(f"{outdir}/boost_neg_nowarn.png")
    # arm a real low warn at -4 kPa
    post("/api/channels/update",
         {"id": "boost_pressure", "fields": {"low_warn": -4}})
    time.sleep(1)
    inject({"MAP": 98, "BARO_PRESSURE": 100})       # boost = -2 -> above -4, stay green
    time.sleep(1)
    shot(f"{outdir}/boost_-2_warn-4.png")
    inject({"MAP": 40, "BARO_PRESSURE": 100})       # boost = -60 -> below -4, blue
    time.sleep(1)
    shot(f"{outdir}/boost_-60_warn-4.png")
    post("/api/channels/update",
         {"id": "boost_pressure", "fields": {"low_warn": None}})
    post("/api/signal/clear", {"all": True})
    print("verify done (warn cleared)", flush=True)

def phase_styles(outdir):
    lay = dict(LAYOUT_BASE)
    lay["widgets"] = [BOOST_BAR] + STYLE_BARS
    post("/api/layout/preview", lay)
    time.sleep(2)
    inject({"COOLANT_TEMP": 90, "MAP": 160, "BARO_PRESSURE": 100,
            "LAMBDA_CORR_A_lambda_corre_2": 0})
    time.sleep(1)
    shot(f"{outdir}/bar_styles.png")
    # round-trip: live firmware serialization vs what we sent
    cur = get("/api/layout/current")
    sent = {w["id"]: w["config"] for w in lay["widgets"]}
    live = {w["id"]: w.get("config", {}) for w in cur.get("widgets", [])
            if w.get("type") == "bar"}
    bad = 0
    for wid, cfg in sent.items():
        lcfg = live.get(wid)
        if lcfg is None:
            print(f"MISSING widget {wid}", flush=True); bad += 1; continue
        for k, v in cfg.items():
            if k in ("channel", "signal_name", "slot"):
                continue
            lv_ = lcfg.get(k)
            # defaults-only serializer omits factory defaults; absence is only
            # OK if we happened to send the default - report for manual eyeball
            if lv_ is None:
                print(f"  {wid}.{k}: sent {v!r}, omitted by to_json", flush=True)
            elif (isinstance(v, float) or isinstance(lv_, float)) and \
                 isinstance(lv_, (int, float)) and abs(float(lv_) - float(v)) < 1e-3:
                pass
            elif lv_ != v:
                print(f"  {wid}.{k}: sent {v!r}, came back {lv_!r}  <-- MISMATCH",
                      flush=True)
                bad += 1
    print(f"round-trip complete, {bad} hard mismatches", flush=True)
    post("/api/signal/clear", {"all": True})

if __name__ == "__main__":
    ph = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    if ph == "poison":   phase_poison()
    elif ph == "clean":  phase_clean()
    elif ph == "verify": phase_verify(out)
    elif ph == "styles": phase_styles(out)
    else: raise SystemExit(f"unknown phase {ph}")
