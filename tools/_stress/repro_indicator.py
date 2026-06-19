#!/usr/bin/env python3
"""Focused reproducer for the indicator resize heap-corruption bug.
Waits for the device, instantiates an indicator, then hammers resize transforms
across pathological sizes. Pre-fix this panics within a handful of transforms;
post-fix it should complete with panic_count flat and no reboot."""
import json, time, urllib.request, urllib.error
HOST="http://192.168.4.61"
def req(m,p,b=None,t=15):
    d=None;h={}
    if b is not None: d=json.dumps(b).encode();h["Content-Type"]="application/json"
    r=urllib.request.Request(HOST+p,data=d,headers=h,method=m)
    try:
        with urllib.request.urlopen(r,timeout=t) as x: return x.status,x.read()
    except urllib.error.HTTPError as e: return e.code,b""
    except Exception as ex: return 0,str(ex).encode()
def selftest():
    s,b=req("GET","/api/selftest",t=10)
    if s!=200: return None
    j=json.loads(b); return j["crash"]["panic_count"], j["heap"]["free"]
def up():
    s,b=req("GET","/api/device/info",t=8)
    if s!=200: return None
    return (json.loads(b).get("system") or {}).get("uptime_s")

# wait for device after flash
print("waiting for device...",flush=True)
for _ in range(40):
    if selftest() is not None: break
    time.sleep(4)
st0=selftest(); u0=up()
print(f"device up: panic={st0[0] if st0 else '?'} heap={st0[1] if st0 else '?'} uptime={u0}",flush=True)

# instantiate one indicator (slot 0)
layout={"schema_version":15,"name":"_repro","screen_w":800,"screen_h":480,"signals":[],
        "widgets":[{"type":"indicator","id":"t_indicator","x":0,"y":0,"w":50,"h":50,"config":{"slot":0}}]}
s,_=req("POST","/api/layout/preview",layout,t=20); time.sleep(0.6)
print(f"preview indicator: {s}",flush=True)

sizes=[(1,1),(800,480),(2,400),(400,2),(5,5),(1,480),(800,1),(60,60),(3,3),(120,40),(1,1),(700,700)]
N=200; reboots=0; lastup=u0
for i in range(N):
    w,h=sizes[i%len(sizes)]
    s,_=req("POST","/api/widget/transform",{"id":"t_indicator","x":0,"y":0,"w":w,"h":h})
    if s==0:
        print(f"  [{i}] transform {w}x{h} -> connection died; waiting...",flush=True)
        for _ in range(40):
            if selftest() is not None: break
            time.sleep(4)
        reboots+=1
    if i%40==39:
        st=selftest(); u=up()
        print(f"  [{i+1}/{N}] panic={st[0] if st else '?'} heap={st[1] if st else '?'} uptime={u}",flush=True)

st1=selftest(); u1=up()
print(f"\nRESULT: panic {st0[0] if st0 else '?'} -> {st1[0] if st1 else '?'}   "
      f"uptime {u0} -> {u1}   connection-deaths={reboots}",flush=True)
crashed = (st0 and st1 and st1[0]>st0[0]) or (u0 is not None and u1 is not None and u1<u0) or reboots>0
print("VERDICT:", "STILL CRASHING" if crashed else "FIX HOLDS (no panic, no reboot)",flush=True)
# restore
req("POST","/api/layout/set",{"name":"ford_cluster"},t=25)
