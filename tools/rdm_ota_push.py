#!/usr/bin/env python3
"""
rdm_ota_push.py — flash a local firmware build to a dash over WiFi.

Uses POST /api/ota/upload (web_server_ota.c), which streams the raw
esp32-firmware.bin straight into the inactive OTA slot and reboots. This is
the path that lets you flash a dash that has no USB cable attached — the
older /api/ota/start could only install a *published GitHub release*, so
testing one board meant cutting a public release.

Usage:
    python tools/rdm_ota_push.py 192.168.4.69
    python tools/rdm_ota_push.py 192.168.4.69 --bin build/esp32-firmware.bin

Finds the dash if you don't know the IP:
    python tools/rdm_ota_push.py --scan 192.168.4

The device serial is read from /api/device/info and echoed back in the
required X-RDM-Device header (see the SECURITY note in web_server_ota.c —
this targets the right board, it is not authentication).

Exit codes: 0 ok, 1 failed, 2 usage/setup error.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(REPO, "build", "esp32-firmware.bin")
ESP_IMAGE_MAGIC = 0xE9


def _get(url: str, timeout: float = 4.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode())


def device_info(ip: str, timeout: float = 4.0):
    return _get(f"http://{ip}/api/device/info", timeout)


def scan(subnet: str):
    """Probe /api/device/info across a /24 — DHCP moves the dash around."""
    print(f"scanning {subnet}.1-254 ...")
    found = []

    def probe(host):
        ip = f"{subnet}.{host}"
        try:
            d = device_info(ip, timeout=1.0)
            if d.get("serial"):
                return ip, d
        except Exception:
            pass
        return None

    with ThreadPoolExecutor(max_workers=64) as ex:
        for res in ex.map(probe, range(1, 255)):
            if res:
                ip, d = res
                found.append(ip)
                print(f"  {ip}  serial={d.get('serial')}  schema={d.get('schema')}")
    if not found:
        print("  no dashes found")
    return found


def push(ip: str, binpath: str) -> int:
    if not os.path.exists(binpath):
        print(f"ERROR: {binpath} not found — run idf.py build first", file=sys.stderr)
        return 2
    blob = open(binpath, "rb").read()
    if not blob or blob[0] != ESP_IMAGE_MAGIC:
        print("ERROR: not an ESP32 app image (bad magic). Use "
              "build/esp32-firmware.bin, not the merged flash image.",
              file=sys.stderr)
        return 2

    try:
        info = device_info(ip)
    except Exception as e:
        print(f"ERROR: no dash at {ip} ({e})", file=sys.stderr)
        return 1

    serial = info.get("serial", "")
    before_schema = info.get("schema")
    print(f"dash    : {ip}  serial={serial}  schema={before_schema}")
    print(f"image   : {binpath}  ({len(blob)/1024/1024:.2f} MB)")

    req = urllib.request.Request(
        f"http://{ip}/api/ota/upload",
        data=blob,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(blob)),
            "X-RDM-Device": serial,
        },
    )
    print("uploading ... (do NOT power-cycle the dash)")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            resp = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        print(f"ERROR: HTTP {e.code} — {body}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"ERROR: upload failed ({e})", file=sys.stderr)
        return 1

    dt = time.time() - t0
    print(f"flashed : {resp.get('bytes')} bytes -> {resp.get('partition')} "
          f"in {dt:.1f}s ({len(blob)/1024/dt:.0f} KB/s)")

    # The dash reboots ~0.7 s after responding. Wait for it to come back and
    # confirm the new image actually booted (a rolled-back slot would report
    # the OLD schema).
    # 4 minutes, not 90 s: on marginal WiFi the dash burns its fast reconnect
    # tier (~60 s) and then waits out a slower tier before rejoining. Observed
    # twice on 2026-08-08 — a 90 s window reported a false failure while the
    # dash was in fact fine and came back shortly after.
    print("rebooting ... waiting for the dash to come back (up to 4 min)")
    deadline = time.time() + 240
    while time.time() < deadline:
        time.sleep(3)
        try:
            info2 = device_info(ip, timeout=3.0)
        except Exception:
            continue
        up = info2.get("system", {}).get("uptime_s", 999)
        if up > 60:
            continue  # still the pre-reboot instance answering
        print(f"back up : schema={info2.get('schema')}  uptime={up}s")
        if before_schema is not None and info2.get("schema") == before_schema:
            print("WARNING: schema unchanged — if you expected a schema bump, "
                  "the bootloader may have rolled back. Check the boot log.")
        print("\nIMPORTANT: let the dash render for ~10 s before any reset. "
              "The rollback guard only marks this image valid once the UI is up.")
        return 0

    print("WARNING: dash did not answer within 90 s. It may still be booting — "
          "re-check with:  curl http://%s/api/device/info" % ip, file=sys.stderr)
    return 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ip", nargs="?", help="dash IP (omit with --scan)")
    ap.add_argument("--bin", default=DEFAULT_BIN, help="firmware .bin to push")
    ap.add_argument("--scan", metavar="SUBNET",
                    help="find dashes on a /24, e.g. 192.168.4")
    args = ap.parse_args()

    if args.scan:
        found = scan(args.scan)
        if len(found) == 1 and not args.ip:
            print(f"\npushing to the only dash found: {found[0]}")
            return push(found[0], args.bin)
        return 0 if found else 1

    if not args.ip:
        ap.print_usage()
        print("give an IP, or use --scan 192.168.4", file=sys.stderr)
        return 2
    return push(args.ip, args.bin)


if __name__ == "__main__":
    sys.exit(main())
