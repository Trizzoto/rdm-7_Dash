#!/usr/bin/env python3
"""RDM-7 Dash live stress harness. Stdlib only.

Hammers the device HTTP API across many paths, measures latency, tracks heap
(leak detection) and uptime/panic_count (crash detection) between phases, and
probes robustness with malformed payloads. Non-destructive: backs nothing up
itself (caller already backed up the active layout) and restores the active
layout at the end.
"""
import json, time, sys, statistics, threading, urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor

HOST = "http://192.168.4.61"
LOG = []          # structured event log
HEAP_TL = []      # (label, heap_free, psram_largest, uptime, panic, lock_ms)

def req(method, path, body=None, timeout=15):
    url = HOST + path
    data = None
    headers = {}
    if body is not None:
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    r = urllib.request.Request(url, data=data, headers=headers, method=method)
    t0 = time.time()
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            payload = resp.read()
            return resp.status, (time.time()-t0)*1000.0, len(payload), payload, None
    except urllib.error.HTTPError as e:
        return e.code, (time.time()-t0)*1000.0, 0, b"", f"HTTP{e.code}"
    except Exception as e:
        return 0, (time.time()-t0)*1000.0, 0, b"", type(e).__name__

def snapshot(label):
    """Pull heap/uptime/panic. Returns dict or None if unreachable."""
    st, _, _, body, err = req("GET", "/api/selftest", timeout=20)
    di, _, _, dbody, _   = req("GET", "/api/device/info", timeout=20)
    if st != 200:
        HEAP_TL.append((label, None, None, None, None, None))
        return None
    try:
        s = json.loads(body)
        d = json.loads(dbody) if di == 200 else {}
        snap = {
            "heap_free": s["heap"]["free"],
            "psram_largest": s["heap"]["psram_largest_free"],
            "min_free": s["heap"]["min_free"],
            "uptime": (d.get("system") or {}).get("uptime_s"),
            "panic": s["crash"]["panic_count"],
            "lock_ms": s["lvgl"]["lock_ms"],
            "free_signals": s["signals"]["fresh"],
        }
        HEAP_TL.append((label, snap["heap_free"], snap["psram_largest"], snap["uptime"], snap["panic"], snap["lock_ms"]))
        return snap
    except Exception as e:
        HEAP_TL.append((label, None, None, None, None, None))
        return None

def burst(name, make_call, n, concurrency=1):
    """make_call() -> (status, ms, bytes, err). Returns stats dict."""
    lat, errs, codes = [], [], {}
    print(f"\n--- {name}  (n={n}, conc={concurrency}) ---", flush=True)
    before = snapshot(f"{name}:before")
    t0 = time.time()
    def one(_):
        st, ms, by, _b, err = make_call()
        return st, ms, err
    if concurrency == 1:
        results = [one(i) for i in range(n)]
    else:
        with ThreadPoolExecutor(max_workers=concurrency) as ex:
            results = list(ex.map(one, range(n)))
    wall = time.time()-t0
    for st, ms, err in results:
        lat.append(ms)
        codes[st] = codes.get(st, 0)+1
        if err: errs.append(err)
    after = snapshot(f"{name}:after")
    ok = codes.get(200, 0)
    lat_s = sorted(lat)
    def pct(p):
        if not lat_s: return 0
        return lat_s[min(len(lat_s)-1, int(len(lat_s)*p))]
    stats = {
        "name": name, "n": n, "conc": concurrency,
        "ok": ok, "codes": codes, "errs": _tally(errs),
        "wall_s": round(wall,2), "rps": round(n/wall,1) if wall>0 else 0,
        "lat_min": round(min(lat),1) if lat else 0,
        "lat_med": round(statistics.median(lat),1) if lat else 0,
        "lat_p95": round(pct(0.95),1),
        "lat_max": round(max(lat),1) if lat else 0,
    }
    # crash / leak deltas
    crash = leak = None
    if before and after:
        if after["panic"] > before["panic"]:
            crash = f"PANIC +{after['panic']-before['panic']} (count {before['panic']}->{after['panic']})"
        if before["uptime"] is not None and after["uptime"] is not None and after["uptime"] < before["uptime"]:
            crash = (crash or "") + f" REBOOT (uptime {before['uptime']}->{after['uptime']})"
        leak = after["heap_free"] - before["heap_free"]
        stats["heap_delta"] = leak
        stats["heap_after"] = after["heap_free"]
        stats["psram_largest_after"] = after["psram_largest"]
        stats["lock_ms_after"] = after["lock_ms"]
    elif not after:
        crash = "DEVICE UNREACHABLE after burst (probable reboot/hang)"
    stats["crash"] = crash
    LOG.append(stats)
    print(f"  ok={ok}/{n} codes={codes} errs={stats['errs']}", flush=True)
    print(f"  lat ms: min={stats['lat_min']} med={stats['lat_med']} p95={stats['lat_p95']} max={stats['lat_max']}  rps={stats['rps']}", flush=True)
    if leak is not None:
        print(f"  heap_free delta={leak:+d}  (now {after['heap_free']}, psram_largest {after['psram_largest']}, lock_ms {after['lock_ms']})", flush=True)
    if crash:
        print(f"  !!! {crash}", flush=True)
    return stats

def _tally(xs):
    out = {}
    for x in xs: out[x] = out.get(x,0)+1
    return out

def main():
    print("=== RDM-7 Dash stress test ===", flush=True)
    base = snapshot("baseline")
    if not base:
        print("Device unreachable at start. Abort.", flush=True); sys.exit(1)
    print(f"baseline: heap_free={base['heap_free']} psram_largest={base['psram_largest']} "
          f"min_free={base['min_free']} uptime={base['uptime']}s panic={base['panic']} lock_ms={base['lock_ms']}", flush=True)

    layout = json.load(open("tools/_stress/backup_ford_cluster.json"))
    # find a widget id to poke
    wid = None
    for w in layout.get("widgets", []):
        if w.get("id"):
            wid = w["id"]; break
    print(f"target widget id for set/transform: {wid}", flush=True)

    # A. Screenshot storm (heaviest read path: FB capture + JPEG encode)
    burst("screenshot_serial", lambda: req("GET","/api/screenshot"), 50, 1)
    burst("screenshot_conc8",  lambda: req("GET","/api/screenshot"), 48, 8)

    # B. Layout preview storm (full screen rebuild from posted JSON, non-persisted)
    burst("layout_preview", lambda: req("POST","/api/layout/preview", layout, timeout=25), 30, 1)

    # C. Signal inject storm (high-rate value updates)
    sigs = ["RPM","COOLANT_TEMP","MAP","SPEED","OIL_PRESSURE","BATTERY_V","GEAR"]
    cnt = {"i":0}
    def inj():
        cnt["i"] += 1
        vals=[{"signal":s,"value":(cnt["i"]*37+i*113)%8000} for i,s in enumerate(sigs)]
        return req("POST","/api/signal/inject", {"values":vals})
    burst("signal_inject", inj, 400, 1)
    burst("signal_inject_conc8", inj, 200, 8)

    # D. Widget fast-path churn
    # pick a widget with a numeric config field for the live-apply set path
    setw, setfield, setbase = None, None, 0
    for w in layout.get("widgets", []):
        for k, v in (w.get("config") or {}).items():
            if isinstance(v, (int, float)) and k not in ("slot",) and not isinstance(v, bool):
                setw, setfield, setbase = w["id"], k, int(v); break
        if setw: break
    print(f"target widget/set: id={setw} field={setfield} base={setbase}", flush=True)
    if wid and setw:
        tg={"i":0}
        def wset():
            tg["i"]+=1
            return req("POST","/api/widget/set", {"id":setw,"field":setfield,"value":setbase+(tg["i"]%8)})
        burst("widget_set", wset, 200, 1)
        tx={"i":0}
        def wtx():
            tx["i"]+=1
            o=(tx["i"]%120)-60
            return req("POST","/api/widget/transform", {"id":wid,"x":o,"y":o})
        burst("widget_transform", wtx, 200, 1)

    # E. Channels list GET churn (large JSON serialize)
    burst("channels_get", lambda: req("GET","/api/channels"), 120, 6)

    # F. Touch storm (UI input path; random coords across 800x480)
    tc={"i":0}
    def touch():
        tc["i"]+=1
        x=(tc["i"]*53)%800-400; y=(tc["i"]*97)%480-240
        return req("POST","/api/touch", {"x":x,"y":y})
    burst("touch_storm", touch, 150, 1)

    # G. Concurrent mixed load (lock contention / races across subsystems)
    mix=[
        lambda: req("GET","/api/screenshot"),
        lambda: req("GET","/api/signals/values"),
        lambda: req("GET","/api/channels"),
        lambda: req("POST","/api/signal/inject", {"signal":"RPM","value":5000}),
        lambda: req("GET","/api/selftest"),
        lambda: req("GET","/api/layout/current"),
    ]
    mc={"i":0}
    def mixcall():
        mc["i"]+=1
        return mix[mc["i"]%len(mix)]()
    burst("mixed_conc12", mixcall, 360, 12)

    # H. Robustness: malformed / oversized / missing-field payloads
    print("\n--- robustness probes ---", flush=True)
    probes = [
        ("preview_garbage", lambda: req("POST","/api/layout/preview", "}{not json")),
        ("preview_empty",   lambda: req("POST","/api/layout/preview", "")),
        ("preview_huge",    lambda: req("POST","/api/layout/preview", '{"widgets":['+ ('{"type":"panel","id":"x","x":0,"y":0,"w":10,"h":10,"config":{}},'*2000)+'{"type":"panel","id":"z","x":0,"y":0,"w":10,"h":10,"config":{}}]}', timeout=30)),
        ("inject_nofield",  lambda: req("POST","/api/signal/inject", "{}")),
        ("inject_badtype",  lambda: req("POST","/api/signal/inject", {"signal":12345,"value":"abc"})),
        ("widget_set_noid", lambda: req("POST","/api/widget/set", {"field":"x","value":1})),
        ("widget_set_badid",lambda: req("POST","/api/widget/set", {"id":"nope_999","field":"x","value":1})),
        ("touch_oob",       lambda: req("POST","/api/touch", {"x":999999,"y":-999999})),
        ("notfound",        lambda: req("GET","/api/does/not/exist")),
        ("wrongmethod",     lambda: req("POST","/api/selftest")),
        ("huge_signal_name",lambda: req("POST","/api/signal/inject", {"signal":"A"*5000,"value":1})),
    ]
    snapshot("robustness:before")
    for nm, fn in probes:
        st, ms, by, _b, err = fn()
        print(f"  {nm:18s} -> code={st} {round(ms,0)}ms {err or ''}", flush=True)
        LOG.append({"name":"probe:"+nm,"code":st,"ms":round(ms,1),"err":err})
    rb = snapshot("robustness:after")
    if rb: print(f"  after robustness: heap_free={rb['heap_free']} panic={rb['panic']} uptime={rb['uptime']}", flush=True)

    # I. Layout switch storm (full teardown+rebuild w/ image/font load from LFS)
    others = [n for n in ["Altezza","Race","MoTeC_C127","LBar","default"] ]
    sc={"i":0}
    def lswitch():
        nm=others[sc["i"]%len(others)]; sc["i"]+=1
        return req("POST","/api/layout/set", {"name":nm}, timeout=25)
    burst("layout_switch", lswitch, 15, 1)

    # restore active layout
    print("\nrestoring ford_cluster as active...", flush=True)
    req("POST","/api/layout/set", {"name":"ford_cluster"}, timeout=25)
    time.sleep(2)

    final = snapshot("final")
    print("\n=== HEAP TIMELINE ===", flush=True)
    for label,h,p,u,pa,lk in HEAP_TL:
        if h is None: print(f"  {label:28s} UNREACHABLE", flush=True)
        else: print(f"  {label:28s} heap={h:>9d} psram_lg={p:>9d} uptime={u} panic={pa} lock_ms={lk}", flush=True)
    if base and final:
        print(f"\nNET heap delta over whole run: {final['heap_free']-base['heap_free']:+d} "
              f"(start {base['heap_free']} -> end {final['heap_free']})", flush=True)
        print(f"panic_count: {base['panic']} -> {final['panic']}  "
              f"({'NO NEW PANICS' if final['panic']==base['panic'] else 'NEW PANICS!'})", flush=True)
        print(f"uptime: {base['uptime']} -> {final['uptime']}  "
              f"({'no reboot' if final['uptime']>=base['uptime'] else 'REBOOTED'})", flush=True)

    json.dump({"log":LOG,"heap_timeline":HEAP_TL,"baseline":base,"final":final},
              open("tools/_stress/results.json","w"), indent=2)
    print("\nresults -> tools/_stress/results.json", flush=True)

if __name__ == "__main__":
    main()
