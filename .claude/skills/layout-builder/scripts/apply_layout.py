#!/usr/bin/env python3
"""Apply a layout to the dash. Validates first, then PREVIEWS (live, not saved)
by default — perfect for the build/verify loop — or SAVES (persists). Waits for
the screen to stop changing, then pulls a screenshot so you can see the result.

  # find the dash (DHCP moves it — don't assume last week's IP)
  python apply_layout.py --scan 192.168.4

  # list bindable channels on the device
  python apply_layout.py --channels

  # build/verify loop: live-preview + grab a screenshot (NOT persisted)
  python apply_layout.py my_layout.json --shot out.png

  # persist it and make it the active layout on screen
  python apply_layout.py my_layout.json --save --activate --shot out.png

  # feed the gauges first so the shot isn't all zeros
  python apply_layout.py my_layout.json --inject RPM=4200 --inject COOLANT_TEMP=92 --shot out.png

Device host: --host, else $RDM7_HOST, else --scan. Uses only the stdlib.
"""
import json, io, os, sys, time, argparse, subprocess
import urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(__file__)


def _req(h, path, method="GET", body=None, timeout=20):
    url = "http://%s%s" % (h, path)
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        url, data=data, method=method,
        headers={"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def alive(h, timeout=2):
    try:
        s, _ = _req(h, "/api/selftest", timeout=timeout)
        return s == 200
    except Exception:
        return False


def scan(prefix):
    """Sweep prefix.1-254 for a dash. The bench unit is on DHCP and moves."""
    hosts = ["%s.%d" % (prefix, i) for i in range(1, 255)]
    with ThreadPoolExecutor(max_workers=64) as ex:
        for h, ok in zip(hosts, ex.map(lambda x: alive(x, 1), hosts)):
            if ok:
                return h
    return None


def resolve_host(args):
    if args.host:
        return args.host
    env = os.environ.get("RDM7_HOST")
    if env:
        return env
    if args.scan:
        print("scanning %s.0/24 …" % args.scan)
        h = scan(args.scan)
        if h:
            print("found dash at %s" % h)
        return h
    return None


def screen_hash(h):
    """FNV hash of the shadow framebuffer. `torn` means the flush tap wrote
    mid-hash — retry rather than trust it."""
    try:
        _, b = _req(h, "/api/screenshot/hash", timeout=10)
        j = json.loads(b)
        return None if j.get("torn") else j.get("hash")
    except Exception:
        return None


def wait_settled(h, timeout=8.0, stable_for=2):
    """Wait until the framebuffer hash stops changing. Beats a fixed sleep: a
    light layout is ready in ~200 ms, a heavy one takes seconds, and a blind
    sleep either wastes time or screenshots a half-built screen."""
    deadline = time.time() + timeout
    last, runs = None, 0
    while time.time() < deadline:
        hsh = screen_hash(h)
        if hsh is not None and hsh == last:
            runs += 1
            if runs >= stable_for:
                return True
        else:
            runs = 0
            last = hsh
        time.sleep(0.25)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layout", nargs="?", help="layout JSON file")
    ap.add_argument("--host", help="dash IP (else $RDM7_HOST, else --scan)")
    ap.add_argument("--scan", metavar="PREFIX",
                    help="sweep PREFIX.1-254 for the dash, e.g. 192.168.4")
    ap.add_argument("--channels", action="store_true",
                    help="print bindable channels and exit")
    ap.add_argument("--channels-out", metavar="JSON",
                    help="also save /api/channels here (feeds validate_layout.py)")
    ap.add_argument("--save", action="store_true",
                    help="persist via /api/layout/save (default is preview-only)")
    ap.add_argument("--activate", action="store_true",
                    help="after --save, make it the active layout")
    ap.add_argument("--inject", action="append", default=[], metavar="SIG=VAL",
                    help="inject a signal value before screenshotting (repeatable)")
    ap.add_argument("--shot", metavar="PNG", help="save a screenshot here after applying")
    ap.add_argument("--expect-version", type=int, metavar="N",
                    help="refuse to save if /api/layout/version is not N "
                         "(guards against clobbering edits made since you last pulled)")
    ap.add_argument("--no-validate", action="store_true")
    args = ap.parse_args()

    h = resolve_host(args)
    if not h:
        print("no dash: pass --host, set $RDM7_HOST, or use --scan 192.168.4")
        sys.exit(2)
    if not alive(h):
        print("device not reachable at %s" % h)
        sys.exit(2)

    if args.channels or args.channels_out:
        _, body = _req(h, "/api/channels")
        cj = json.loads(body)
        ch = cj.get("channels", cj if isinstance(cj, list) else [])
        if args.channels_out:
            io.open(args.channels_out, "w", encoding="utf-8").write(
                body.decode("utf-8", "replace"))
            print("channels -> %s" % args.channels_out)
        if args.channels:
            print("%d channels:" % len(ch))
            for c in ch:
                print("  %-24s %-10s %s" % (
                    c.get("signal", ""), c.get("id", ""),
                    c.get("units_display") or c.get("units") or ""))
            return

    if not args.layout:
        print("need a layout file (or --channels)")
        sys.exit(2)

    if not args.no_validate:
        v = subprocess.run([sys.executable,
                            os.path.join(HERE, "validate_layout.py"), args.layout])
        if v.returncode != 0:
            print("validation failed — not applying")
            sys.exit(1)

    L = json.load(io.open(args.layout, encoding="utf-8"))

    if args.save:
        # Clobber guard: the user may have edited the layout in the browser
        # between your turns. Saving a stale local copy deletes their work.
        if args.expect_version is not None:
            try:
                _, vb = _req(h, "/api/layout/version", timeout=5)
                cur = json.loads(vb).get("v")
            except Exception as e:
                print("could not read /api/layout/version (%s) — refusing to save" % e)
                sys.exit(1)
            if cur != args.expect_version:
                print("REFUSING TO SAVE: layout version is %s, you expected %s.\n"
                      "The layout changed on the dash since you pulled it. Re-pull\n"
                      "/api/layout/current and re-apply your edit on top of that."
                      % (cur, args.expect_version))
                sys.exit(1)
        try:
            s, body = _req(h, "/api/layout/save", "POST", L)
            print("save:", s, body[:200].decode(errors="replace"))
        except urllib.error.HTTPError as e:
            detail = e.read()[:300].decode(errors="replace")
            print("save FAILED: HTTP %s %s" % (e.code, detail))
            if e.code == 409:
                print("  -> 409 is the empty-layout guard. Your widgets[] is empty "
                      "over a layout that has widgets. Re-pull /api/layout/current "
                      "and find out why before adding allow_empty.")
            sys.exit(1)
        if args.activate and L.get("name"):
            s2, b2 = _req(h, "/api/layout/set", "POST", {"name": L["name"]})
            print("activate:", s2, b2[:120].decode(errors="replace"))
    else:
        s, body = _req(h, "/api/layout/preview", "POST", L)
        print("preview (live, not saved):", s, body[:120].decode(errors="replace"))

    if args.inject:
        vals = []
        for spec in args.inject:
            if "=" not in spec:
                print("bad --inject %r (want SIG=VAL)" % spec)
                sys.exit(2)
            k, v = spec.split("=", 1)
            vals.append({"signal": k.strip(), "value": float(v)})
        # 16 per request is the firmware's batch cap.
        for i in range(0, len(vals), 16):
            _, ib = _req(h, "/api/signal/inject", "POST", {"values": vals[i:i + 16]})
            rj = json.loads(ib)
            if rj.get("unknown"):
                print("WARNING: not live channels, injected nothing: %s"
                      % ", ".join(rj["unknown"]))
            if rj.get("injected"):
                print("injected: %s" % ", ".join(rj["injected"]))

    if args.shot:
        if not wait_settled(h):
            print("note: screen still changing after the settle timeout — the "
                  "shot may catch a partial render")
        try:
            _, png = _req(h, "/api/screenshot?full=1", timeout=30)
            with open(args.shot, "wb") as f:
                f.write(png)
            print("screenshot -> %s (%d B)" % (args.shot, len(png)))
        except Exception as e:
            print("screenshot failed:", e)


if __name__ == "__main__":
    main()
