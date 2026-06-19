"""Deploy + verify a built cluster on a live RDM-7 dash.

  python tools/clusters/deploy.py mercedes
  python tools/clusters/deploy.py mercedes --host 192.168.4.61 --state driving
  python tools/clusters/deploy.py ktm --state '{"RPM":7400,"GEAR":3}'
  python tools/clusters/deploy.py altezza --state idle --shot idle.jpg

Pipeline (per the proven loop): upload baked image -> save+activate layout ->
inject a test state -> screenshot. Reads tools/clusters/dist/<name>/meta.json
written by gen.py; build first with `python tools/clusters/gen.py <name>`.

Uses urllib (stdlib) so JSON bodies aren't mangled by shell quoting.
"""
import argparse
import json
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIST = HERE / "dist"
DEFAULT_HOST = "192.168.4.61"
SETTLE_S = 1.4  # async hot-reload + needle smoothing settle time


def _req(url, data=None, ctype=None, timeout=30, method=None):
    headers = {"Content-Type": ctype} if ctype else {}
    r = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        try:
            body = e.read()
        except Exception:
            body = b"(error body unreadable)"
        return e.code, body
    except urllib.error.URLError as e:
        return None, str(e.reason).encode()
    except OSError as e:
        # ConnectionResetError etc. -- device dropped us (often PSRAM OOM mid-upload)
        return None, str(e).encode()


# A minimal no-image layout. Saving it frees ALL baked-image PSRAM so a large
# image can be uploaded into the freed space (the dash buffers the whole file).
BLANK_LAYOUT = {
    "schema_version": 14, "name": "_blank", "screen_w": 800, "screen_h": 480,
    "signals": [],
    "widgets": [{"type": "text", "id": "t", "x": 0, "y": 0, "w": 120, "h": 40,
                 "config": {"static_text": " ", "font": "Manrope Bold:20"}}],
}


def post_bytes(host, path, data, ctype, timeout=40):
    return _req(f"http://{host}{path}", data=data, ctype=ctype, timeout=timeout, method="POST")


def get_file(host, path, out_path, timeout=25):
    status, body = _req(f"http://{host}{path}", timeout=timeout)
    if status == 200:
        out_path.write_bytes(body)
    return status, len(body) if body else 0


def resolve_state(meta, arg):
    if arg in (None, "", "none", "live"):
        if arg in ("none", "live"):
            return None
        arg = meta.get("default_state")
    if arg is None:
        return None
    if isinstance(arg, str) and arg.strip().startswith("{"):
        return json.loads(arg)
    states = meta.get("states", {})
    if arg in states:
        return states[arg]
    raise SystemExit(f"unknown state '{arg}'. known: {', '.join(states) or '(none)'} or a JSON object")


def main():
    ap = argparse.ArgumentParser(description="Deploy + verify a cluster on a live dash.")
    ap.add_argument("name", help="cluster name (must be built into dist/<name>/)")
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"dash IP (default {DEFAULT_HOST})")
    ap.add_argument("--state", default=None, help="inject state: a STATES key, a JSON object, or 'none' to skip")
    ap.add_argument("--shot", default=None, help="screenshot output path (default dist/<name>/<name>_<state>.jpg)")
    ap.add_argument("--no-shot", action="store_true", help="skip the screenshot")
    ap.add_argument("--skip-upload", action="store_true", help="don't re-upload the baked image (already on device)")
    ap.add_argument("--skip-save", action="store_true", help="don't save/activate the layout")
    ap.add_argument("--free-first", action="store_true", help="push a blank no-image layout before uploading to free PSRAM (large images on low-RAM devices)")
    args = ap.parse_args()

    out = DIST / args.name
    meta_p = out / "meta.json"
    if not meta_p.exists():
        raise SystemExit(f"no build at {out} -- run: python tools/clusters/gen.py {args.name}")
    meta = json.loads(meta_p.read_text())
    host = args.host

    # 0) optionally free PSRAM by activating a blank no-image layout first
    if not args.skip_upload and args.free_first:
        status, body = post_bytes(host, "/api/layout/save",
                                  json.dumps(BLANK_LAYOUT).encode(), "application/json")
        print(f"free: {status} {body.decode(errors='replace')[:80]}")
        time.sleep(1.3)

    # 1) upload every baked image (retry on OOM: switching layout away frees PSRAM)
    if not args.skip_upload:
        imgs = meta.get("images") or [{"name": meta["image_name"], "file": meta["image_file"]}]
        for spec in imgs:
            data = (out / spec["file"]).read_bytes()
            for attempt in range(3):
                status, body = post_bytes(host, f"/api/image/upload?name={spec['name']}",
                                          data, "application/octet-stream")
                print(f"upload {spec['name']}[{attempt}]: {status} {body.decode(errors='replace')[:100]}")
                if status == 200:
                    break
                time.sleep(1.5)
            else:
                raise SystemExit(f"upload '{spec['name']}' failed (out of PSRAM? try a smaller active layout first)")

    # 2) save + activate layout
    if not args.skip_save:
        layout = (out / meta["layout_file"]).read_bytes()
        status, body = post_bytes(host, "/api/layout/save", layout, "application/json")
        print(f"save: {status} {body.decode(errors='replace')[:120]}")
        if status != 200:
            raise SystemExit("layout save failed")
        time.sleep(SETTLE_S)

    # 3) inject a test state
    state = resolve_state(meta, args.state)
    if state:
        values = [{"signal": k, "value": v} for k, v in state.items()]
        body_json = json.dumps({"values": values}).encode()
        status, body = post_bytes(host, "/api/signal/inject", body_json, "application/json", timeout=20)
        print(f"inject: {status} {body.decode(errors='replace')[:160]}")
        time.sleep(SETTLE_S)

    # 4) screenshot
    if not args.no_shot:
        state_tag = args.state if isinstance(args.state, str) and args.state and not args.state.startswith("{") else (meta.get("default_state") or "shot")
        shot = Path(args.shot) if args.shot else (out / f"{args.name}_{state_tag}.jpg")
        if not shot.is_absolute():
            shot = out / shot
        status, nbytes = get_file(host, "/api/screenshot", shot)
        print(f"shot: {status} {nbytes} B -> {shot}")


if __name__ == "__main__":
    main()
