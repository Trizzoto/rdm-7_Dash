#!/usr/bin/env python3
"""
provision_dash.py — load the standard layouts (and the fonts they use) onto a
customer dash over USB, as the last step before it ships.

Every RDM-7 goes out with the included looks: Modern, Molec, RDM Analog,
RDM Race and Retro Digital, next to the firmware's own Default. They are
ordinary layouts on LittleFS, so the customer can delete any they don't want.
They are not baked into the app image: the app partition has no room, and a
LittleFS image flash would wipe a used dash.

Sources: main/embed/layouts/*.json and main/embed/fonts/ (the non-built-in
faces). Default stays the active layout.

Usage:
    python tools/provision_dash.py COM47            # fonts + layouts, then verify
    python tools/provision_dash.py COM47 --check    # only report what is on the dash

Needs pyserial. The UART switch must be on UART1 (USB).
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rdm_serial_rpc import frame, read_frame, TAG_JSON, BAUD  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
LAYOUT_DIR = ROOT / "main" / "embed" / "layouts"
FONT_DIR = ROOT / "main" / "embed" / "fonts"

# Font family name on the dash (font_manager keys by file stem) -> source file.
# Montserrat, Fugaz One and Manrope Bold (subset) are built in and not sent.
FONTS = {
    "Barlow": "barlow.ttf",
    "DSEG14I": "dseg14i.ttf",
    "DSEG7I": "dseg7i.ttf",
    "Manrope-Bold": "manrope_bold_full.ttf",
}

# Dash layout name -> source file, in the order they appear on the dash.
LAYOUTS = {
    "modern": "modern.json",
    "molec": "molec.json",
    "rdm_analog": "rdm_analog.json",
    "rdm_race": "rdm_race.json",
    "retro_digital": "retro_digital.json",
}


class Dash:
    def __init__(self, port: str):
        import serial  # type: ignore
        # Never let pyserial toggle DTR/RTS on open: on this board they drive
        # EN/GPIO0 and would reset the dash (see rdm_serial_rpc.py).
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = BAUD
        self.ser.timeout = 0.4
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.reset_input_buffer()
        self._id = 100

    def call(self, method: str, params=None, timeout=20.0):
        self._id += 1
        rid = self._id
        req = json.dumps({"id": rid, "method": method, "params": params or {}}).encode()
        self.ser.write(frame(bytes([TAG_JSON]) + req))
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            p = read_frame(self.ser, timeout=max(1.0, deadline - time.time()))
            if p is None:
                break
            if p[0] != TAG_JSON:
                continue
            try:
                msg = json.loads(p[1:].decode("utf-8", "replace"))
            except ValueError:
                continue
            if msg.get("id") == rid:
                if msg.get("error"):
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result")
        raise TimeoutError(f"{method}: no reply")

    def upload_font(self, name: str, blob: bytes):
        try:
            self.call("upload.abort")
        except (RuntimeError, TimeoutError):
            pass
        start = self.call("upload.start", {"type": "font", "name": name, "size": len(blob)})
        session, cs = int(start["session"]), int(start["chunk_size"])
        time.sleep(0.3)
        n = (len(blob) + cs - 1) // cs
        idx, tries = 0, 0
        while idx < n:
            chunk = blob[idx * cs:(idx + 1) * cs]
            self.ser.write(frame(bytes([0x01]) + struct.pack("<QH", session, idx) + chunk))
            self.ser.flush()
            nxt = None
            t0 = time.time()
            while time.time() - t0 < 3:
                p = read_frame(self.ser, timeout=3)
                if p is None:
                    break
                if p[0] != TAG_JSON:
                    continue
                try:
                    r = (json.loads(p[1:]).get("result") or {})
                except ValueError:
                    continue
                if isinstance(r, dict) and "chunk" in r:
                    nxt = idx + 1 if r.get("ok") else int(r.get("expected", idx))
                    break
            if nxt is None:
                tries += 1
                if tries > 8:
                    self.call("upload.abort")
                    raise RuntimeError(f"font {name}: chunk {idx} never acknowledged")
                time.sleep(0.5)
                continue
            idx, tries = nxt, 0
        self.call("upload.finish", timeout=30)


def report(dash: Dash) -> bool:
    fonts = {f["name"] for f in dash.call("font.list")}
    layouts = dash.call("layout.list")
    have = set(layouts.get("layouts", []))
    missing_f = [f for f in FONTS if f not in fonts]
    missing_l = [l for l in LAYOUTS if l not in have]
    print(f"  active layout: {layouts.get('active')}")
    print(f"  layouts: {', '.join(sorted(have))}")
    print(f"  fonts:   {', '.join(sorted(fonts))}")
    if missing_f or missing_l:
        print(f"  MISSING fonts {missing_f} layouts {missing_l}")
        return False
    print("  all standard layouts and fonts present")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("port")
    ap.add_argument("--check", action="store_true", help="only report what is on the dash")
    a = ap.parse_args()

    dash = Dash(a.port)
    info = dash.call("device.info")
    print(f"Dash {info.get('serial')} firmware {info.get('version')}")

    if a.check:
        return 0 if report(dash) else 1

    for name, file in FONTS.items():
        blob = (FONT_DIR / file).read_bytes()
        print(f"  font   {name:<14} {len(blob) / 1024:6.1f} KB", end="", flush=True)
        # The dash is still flushing the previous font to flash for a moment
        # after upload.finish; a chunk sent then can go unacknowledged. Start
        # the whole font again rather than fail the dash.
        for attempt in range(3):
            try:
                dash.upload_font(name, blob)
                break
            except RuntimeError:
                if attempt == 2:
                    raise
                print(" (retry)", end="", flush=True)
                time.sleep(3)
                dash.ser.reset_input_buffer()
        time.sleep(2)
        print("  ok")

    for name, file in LAYOUTS.items():
        data = json.loads((LAYOUT_DIR / file).read_text(encoding="utf-8"))
        print(f"  layout {name:<14}", end="", flush=True)
        # layout.save makes it active and rebuilds the screen; a request that
        # lands mid-rebuild can go unanswered, so give each one room and retry.
        for attempt in range(3):
            try:
                dash.call("layout.save", {"name": name, "data": data}, timeout=20)
                break
            except TimeoutError:
                if attempt == 2:
                    raise
                print(" (retry)", end="", flush=True)
                time.sleep(6)
                dash.ser.reset_input_buffer()
        time.sleep(8)
        print("  ok")

    dash.call("layout.set", {"name": "default"})
    time.sleep(4)
    print("Verifying")
    return 0 if report(dash) else 1


if __name__ == "__main__":
    sys.exit(main())
