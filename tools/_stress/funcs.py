#!/usr/bin/env python3
"""RDM-7 Dash functional + per-widget fuzz harness. Stdlib only.

Goal: find crashes / 500s / hangs across subsystems (channels, sim, wifi-read,
misc GETs) and across EVERY widget type's settings. Non-destructive: uses
/api/layout/preview (never saves), operates channel CRUD only on a throwaway
test channel, restores sim + the active layout at the end, and never touches
wifi-config / reboot / ota / log-start / deletes of real assets.

Crash detector: selftest panic_count (NVS-persisted) + device uptime (resets on
reboot). If a widget type trips a crash, the type is re-run field-by-field with
per-field snapshots to bisect the exact culprit field+value.
"""
import json, io, time, sys, urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor

HOST = "http://192.168.4.61"
ISSUES = []          # [{area, detail, ...}]
WIDGET_ROWS = []     # per widget type summary

def req(method, path, body=None, timeout=15):
    data = None; headers = {}
    if body is not None:
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    r = urllib.request.Request(HOST + path, data=data, headers=headers, method=method)
    t0 = time.time()
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, (time.time()-t0)*1000.0, resp.read(), None
    except urllib.error.HTTPError as e:
        return e.code, (time.time()-t0)*1000.0, (e.read() if hasattr(e,'read') else b""), f"HTTP{e.code}"
    except Exception as e:
        return 0, (time.time()-t0)*1000.0, b"", type(e).__name__

def jget(path, timeout=10):
    st,_,body,err = req("GET", path, timeout=timeout)
    try: return st, json.loads(body)
    except Exception: return st, None

def snap():
    st,_,body,_ = req("GET","/api/selftest",timeout=15)
    if st!=200:
        return None
    try:
        s=json.loads(body)
        return {"heap":s["heap"]["free"], "psram_lg":s["heap"]["psram_largest_free"],
                "panic":s["crash"]["panic_count"], "lock_ms":s["lvgl"]["lock_ms"]}
    except Exception:
        return None

def uptime():
    st,d = jget("/api/device/info",timeout=8)
    if st==200 and d: return (d.get("system") or {}).get("uptime_s")
    return None

def wait_alive(why, max_s=150):
    """Block until the device answers selftest again (after a suspected crash)."""
    print(f"    waiting for device to recover ({why})...", flush=True)
    t0=time.time()
    while time.time()-t0 < max_s:
        if snap() is not None:
            time.sleep(3)
            return True
        time.sleep(4)
    return False

def issue(area, detail, **kw):
    rec={"area":area,"detail":detail}; rec.update(kw); ISSUES.append(rec)
    print(f"    !! ISSUE [{area}] {detail} {kw if kw else ''}", flush=True)

# ── schema -------------------------------------------------------------------
SCHEMA = json.load(io.open("schema/widgets.schema.json", encoding="utf-8"))
WIDGETS = SCHEMA.get("widgets") or SCHEMA.get("widget_types")
WDEF = {w["name"]: w for w in WIDGETS}

def default_config(w):
    cfg={}
    for f in w.get("fields",[]):
        if "default" in f and f["type"] not in ("image_picker",):
            cfg[f["name"]] = f["default"]
    return cfg

def field_values(f):
    """Yield (label, value) test cases for a field."""
    t=f["type"]; out=[]
    if t in ("number","stepper","stepper-auto","slider"):
        lo=f.get("min"); hi=f.get("max"); dv=f.get("default",0)
        mid = (lo+hi)//2 if (isinstance(lo,(int,float)) and isinstance(hi,(int,float))) else (int(dv)+1 if isinstance(dv,(int,float)) else 10)
        out.append(("mid", mid))
        if isinstance(lo,(int,float)): out.append(("min", lo)); out.append(("under", lo-1))
        if isinstance(hi,(int,float)): out.append(("max", hi)); out.append(("over", hi+1))
        if not out: out.append(("ten",10))
    elif t=="color":
        out=[("rgb",0xFF8800),("black",0),("white",0xFFFFFF)]
    elif t=="checkbox":
        out=[("on",True),("off",False)]
    elif t=="select":
        opts=[o.get("value") for o in f.get("options",[]) if "value" in o]
        if opts: out.append(("opt0",opts[0])); out.append(("optN",opts[-1]))
        out.append(("oob",9999))
    elif t in ("text","textarea"):
        out=[("str","TEST"),("empty",""),("long","X"*200)]
    elif t=="font":
        out=[("font","Montserrat:24")]
    elif t=="can_id":
        out=[("id",0x200),("zero",0),("big",0x7FF)]
    elif t=="image_picker":
        out=[]  # skip live image set (editor excludes; needs real asset)
    else:
        out=[("dv",f.get("default",0))]
    return out

def build_layout(wtype):
    w=WDEF[wtype]
    sz=w.get("default_size") or {"w":120,"h":80}
    cfg=default_config(w)
    if wtype in ("indicator","warning","panel","bar"): cfg.setdefault("slot",0)
    wid=f"t_{wtype}"
    return {"schema_version":15,"name":"_functest","screen_w":800,"screen_h":480,
            "signals":[],
            "widgets":[{"type":wtype,"id":wid,"x":0,"y":0,
                        "w":sz.get("w",120),"h":sz.get("h",80),"config":cfg}]}, wid

def preview(layout):
    st,ms,body,err = req("POST","/api/layout/preview", layout, timeout=20)
    time.sleep(0.5)   # async rebuild settle
    return st,err

def set_field(wid, field, value):
    return req("POST","/api/widget/set", {"id":wid,"field":field,"value":value})

# ── widget sweep -------------------------------------------------------------
def sweep_widget(wtype, deep=False):
    w=WDEF[wtype]
    layout,wid = build_layout(wtype)
    pst,perr = preview(layout)
    if pst!=200:
        issue("widget:"+wtype, f"preview failed code={pst} {perr}")
        return
    before=snap(); up0=uptime()
    codes={}; n500=0; n503=0; crashed_field=None
    for f in w.get("fields",[]):
        for label,val in field_values(f):
            st,ms,body,err = set_field(wid, f["name"], val)
            codes[st]=codes.get(st,0)+1
            if st==500: n500+=1; issue("widget:"+wtype, f"HTTP500 on field {f['name']}={val} ({label})")
            if st==0:   # connection died -> probable crash
                if not wait_alive(f"after {wtype}.{f['name']}={val}"):
                    issue("widget:"+wtype, f"DEVICE DOWN, no recovery after {f['name']}={val}")
                    return
                up1=uptime()
                if up1 is not None and up0 is not None and up1<up0:
                    issue("widget:"+wtype, f"CRASH/REBOOT on field {f['name']}={val} ({label})", crash=True)
                    crashed_field=(f["name"],val)
                up0=up1
            if deep:
                s=snap()
                if before and s and s["panic"]>before["panic"]:
                    issue("widget:"+wtype, f"PANIC on field {f['name']}={val} ({label})", crash=True)
                    before=s
    # geometry stress
    for (gw,gh) in [(1,1),(800,480),(2,400),(400,2)]:
        st,ms,body,err = req("POST","/api/widget/transform", {"id":wid,"x":0,"y":0,"w":gw,"h":gh})
        codes[st]=codes.get(st,0)+1
        if st==0 and not wait_alive(f"after {wtype} transform {gw}x{gh}"):
            issue("widget:"+wtype, f"DEVICE DOWN after transform {gw}x{gh}"); return
    after=snap(); up1=uptime()
    reboot = (up0 is not None and up1 is not None and up1<up0)
    panic = (before and after and after["panic"]>before["panic"])
    leak = (after["heap"]-before["heap"]) if (before and after) else None
    row={"type":wtype,"fields":len(w.get("fields",[])),"codes":codes,
         "n500":n500,"reboot":reboot,"panic":bool(panic),"heap_delta":leak}
    WIDGET_ROWS.append(row)
    flag = "  <-- CRASH" if (reboot or panic or n500) else ""
    print(f"  {wtype:14s} codes={codes} 500s={n500} reboot={reboot} panic={bool(panic)} heapd={leak}{flag}", flush=True)
    if (reboot or panic) and not deep:
        print(f"    re-running {wtype} field-by-field to bisect...", flush=True)
        sweep_widget(wtype, deep=True)

def widget_phase():
    print("\n==================  WIDGET SETTINGS SWEEP  ==================", flush=True)
    for wtype in WDEF:
        sweep_widget(wtype)

# ── channels -----------------------------------------------------------------
def channels_phase():
    print("\n==================  CHANNELS  ==================", flush=True)
    st,lst = jget("/api/channels"); print(f"  list code={st} count={(lst or {}).get('count')}", flush=True)
    if st!=200: issue("channels","list failed code=%d"%st)
    st,canon = jget("/api/channels/canonical"); print(f"  canonical code={st} count={(canon or {}).get('count')}", flush=True)
    # pick a canonical channel NOT currently active as the throwaway
    test_id=None
    for c in (canon or {}).get("channels",[]):
        if not c.get("active"): test_id=c["id"]; break
    if not test_id:
        print("  (no inactive canonical channel to test CRUD on; skipping mutation)", flush=True); return
    print(f"  CRUD test channel: {test_id}", flush=True)
    seq=[
        ("activate", "POST","/api/channels/activate",{"id":test_id}),
        ("update-decode","POST","/api/channels/update",{"id":test_id,"fields":{"can_id":512,"bit_start":8,"bit_length":16,"scale":0.1,"offset":0}}),
        ("update-thresh","POST","/api/channels/update",{"id":test_id,"fields":{"low_warn":10,"high_warn":90}}),
        ("update-units","POST","/api/channels/update",{"id":test_id,"fields":{"units_display":"psi"}}),
        ("math-set","POST","/api/channels/update",{"id":test_id,"math":{"a":"rpm","op":"+","b":100}}),
        ("math-clear","POST","/api/channels/update",{"id":test_id,"math":None}),
        ("bad-update","POST","/api/channels/update",{"id":test_id,"fields":{"bit_length":9999,"scale":1e30}}),
        ("delete","POST","/api/channels/delete",{"id":test_id}),
    ]
    for name,m,p,b in seq:
        st,ms,body,err = req(m,p,b)
        ok = st in (200,400)
        print(f"  {name:14s} code={st} {err or ''}", flush=True)
        if st==500: issue("channels", f"{name} -> 500", body=body[:120].decode('latin1'))
        if st==0:
            if not wait_alive(f"channels {name}"): return
            issue("channels", f"{name} -> device down (crash)", crash=True)
    # source-options is GET (?id=); export is GET — read-only round-trips
    st,_so = jget("/api/channels/source-options?id="+test_id); print(f"  source-options code={st}", flush=True)
    if st>=500: issue("channels","source-options -> %d"%st)
    st,exp = jget("/api/channels/export"); print(f"  export code={st}", flush=True)

# ── sim ----------------------------------------------------------------------
def sim_phase():
    print("\n==================  SIM / SIGNALS  ==================", flush=True)
    st,cur = jget("/api/signal/simulate"); orig=(cur or {}).get("enabled")
    print(f"  simulate status code={st} enabled={orig}", flush=True)
    for b in [{"enabled":False},{"enabled":True}]:
        st,ms,body,err=req("POST","/api/signal/simulate",b)
        print(f"  simulate {b} -> {st} {err or ''}", flush=True)
        if st>=500: issue("sim", f"simulate {b} -> {st}")
    # inject valid + junk
    for b in [{"signal":"RPM","value":6000},{"values":[{"signal":"RPM","value":1},{"signal":"NOPE","value":2}]},
              {"signal":"RPM"},{"value":5},{}]:
        st,ms,body,err=req("POST","/api/signal/inject",b)
        if st>=500: issue("sim", f"inject {b} -> {st}")
    st,ms,body,err=req("POST","/api/signal/clear",{"all":True})
    print(f"  clear all -> {st}", flush=True)
    # restore sim to original
    if orig is not None:
        req("POST","/api/signal/simulate",{"enabled":bool(orig)})
    print(f"  restored sim enabled={orig}", flush=True)

# ── misc GET sweep + reversible sets -----------------------------------------
SAFE_GETS = ["/api/device/info","/api/system/health","/api/storage/info","/api/wifi/config",
    "/api/brightness","/api/dimmer/config","/api/gear/config","/api/can/config",
    "/api/font/list","/api/image/list","/api/fuel/status","/api/ecu/current","/api/ecu/list",
    "/api/presets","/api/presets/custom","/api/obd2/pids","/api/obd2/protocols",
    "/api/log/status","/api/log/list","/api/log/config","/api/sd/status","/api/replay/status",
    "/api/ota/status","/api/odometer","/api/screenshot/hash","/api/layout/version",
    "/api/canraw/status","/api/signals/values"]

def misc_phase():
    print("\n==================  MISC FUNCTION GETs  ==================", flush=True)
    for p in SAFE_GETS:
        st,ms,body,err=req("GET",p,timeout=12)
        tag = "" if st==200 else f"  <-- {st}"
        print(f"  {p:34s} {st}{tag}", flush=True)
        if st>=500 or st==0: issue("misc", f"GET {p} -> {st} {err or ''}")
    # reversible setters: brightness (read, set, restore)
    st,b = jget("/api/brightness")
    if st==200 and b is not None:
        orig=b.get("brightness", b.get("value"))
        for v in [10,255,128]:
            r,_,_,_=req("POST","/api/brightness",{"brightness":v})
            if r>=500: issue("misc",f"brightness {v} -> {r}")
        if orig is not None: req("POST","/api/brightness",{"brightness":orig})
        print(f"  brightness set/restore ok (orig={orig})", flush=True)
    # ota check (GET-side already; POST check is a network query, harmless)
    r,_,body,_=req("POST","/api/ota/check",{},timeout=20)
    print(f"  ota/check -> {r}", flush=True)
    if r>=500: issue("misc",f"ota/check -> {r}")

def main():
    print("=== RDM-7 Dash functional/widget fuzz ===", flush=True)
    b=snap()
    if not b: print("device down at start"); sys.exit(1)
    print(f"baseline heap={b['heap']} panic={b['panic']} uptime={uptime()}", flush=True)

    misc_phase()
    sim_phase()
    channels_phase()
    widget_phase()

    # restore active layout
    print("\nrestoring ford_cluster...", flush=True)
    req("POST","/api/layout/set",{"name":"ford_cluster"},timeout=25); time.sleep(2)
    f=snap()
    print("\n==================  SUMMARY  ==================", flush=True)
    print(f"panic {b['panic']} -> {f['panic'] if f else '?'}   "
          f"heap {b['heap']} -> {f['heap'] if f else '?'}", flush=True)
    print(f"ISSUES FOUND: {len(ISSUES)}", flush=True)
    for i in ISSUES: print("  -", i, flush=True)
    json.dump({"issues":ISSUES,"widget_rows":WIDGET_ROWS,"baseline":b,"final":f},
              io.open("tools/_stress/funcs_results.json","w",encoding="utf-8"), indent=2)
    print("\nresults -> tools/_stress/funcs_results.json", flush=True)

if __name__=="__main__":
    main()
