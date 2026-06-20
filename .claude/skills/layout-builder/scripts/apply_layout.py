#!/usr/bin/env python3
"""Apply a layout to the dash. Validates first, then PREVIEWS (live, not saved)
by default — perfect for the build/verify loop — or SAVES (persists). Pulls a
screenshot so you can see the result.

  # list bindable channels on the device
  python apply_layout.py --channels

  # build/verify loop: live-preview + grab a screenshot (NOT persisted)
  python apply_layout.py my_layout.json --shot out.png

  # persist it and make it the active layout on screen
  python apply_layout.py my_layout.json --save --activate --shot out.png

Device host: --host, else $RDM7_HOST, else 192.168.4.61 (USB/LAN) — the dash's
AP is 192.168.4.1. Uses only the stdlib.
"""
import json, io, os, sys, time, argparse, urllib.request, urllib.error, subprocess

HERE = os.path.dirname(__file__)


def host():
    return os.environ.get("RDM7_HOST", "192.168.4.61")


def _req(h, path, method="GET", body=None, timeout=20):
    url = f"http://{h}{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def alive(h):
    try:
        s, _ = _req(h, "/api/selftest", timeout=5)
        return s == 200
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layout", nargs="?", help="layout JSON file")
    ap.add_argument("--host", default=host())
    ap.add_argument("--channels", action="store_true", help="print bindable channels and exit")
    ap.add_argument("--save", action="store_true", help="persist via /api/layout/save (default is preview-only)")
    ap.add_argument("--activate", action="store_true", help="after --save, make it the active layout")
    ap.add_argument("--shot", metavar="PNG", help="save a full screenshot here after applying")
    ap.add_argument("--no-validate", action="store_true")
    args = ap.parse_args()
    h = args.host

    if not alive(h):
        print(f"device not reachable at {h} (set --host or $RDM7_HOST; AP is 192.168.4.1)")
        sys.exit(2)

    if args.channels:
        _, body = _req(h, "/api/channels")
        cj = json.loads(body)
        ch = cj.get("channels", cj if isinstance(cj, list) else [])
        print(f"{len(ch)} channels:")
        for c in ch:
            print(f"  {c.get('signal',''):<20} {c.get('units_display') or c.get('units') or ''}")
        return

    if not args.layout:
        print("need a layout file (or --channels)")
        sys.exit(2)

    if not args.no_validate:
        v = subprocess.run([sys.executable, os.path.join(HERE, "validate_layout.py"), args.layout])
        if v.returncode != 0:
            print("validation failed — not applying")
            sys.exit(1)

    L = json.load(io.open(args.layout, encoding="utf-8"))

    if args.save:
        s, body = _req(h, "/api/layout/save", "POST", L)
        print("save:", s, body[:120].decode(errors="replace"))
        if args.activate and L.get("name"):
            s2, b2 = _req(h, "/api/layout/set", "POST", {"name": L["name"]})
            print("activate:", s2, b2[:120].decode(errors="replace"))
    else:
        s, body = _req(h, "/api/layout/preview", "POST", L)
        print("preview (live, not saved):", s, body[:120].decode(errors="replace"))

    if args.shot:
        time.sleep(2.5)   # the rebuild is deferred (lv_async_call) — let it render before the shot
                          # (heavy image layouts take ~2.3s; bump if a shot looks half-built)
        try:
            _, png = _req(h, "/api/screenshot?full=1", timeout=30)
            with open(args.shot, "wb") as f:
                f.write(png)
            print(f"screenshot -> {args.shot} ({len(png)} B)")
        except Exception as e:
            print("screenshot failed:", e)


if __name__ == "__main__":
    main()
