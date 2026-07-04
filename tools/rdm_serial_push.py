#!/usr/bin/env python3
"""
rdm_serial_push.py — push layout JSON files to the dash over the UART protocol.

Frame: STX(0x02) | len(4B LE) | payload | CRC16-CCITT(2B LE) | ETX(0x03)
payload[0] = 0x00 (JSON).  JSON-RPC: {"id","method","params"}.
Protocol is UART1 @ 921600 (shares pins 43/44 with the UART0 console via the
on-board hardware switch — must be in the UART1/PC position).

Usage:
    python tools/rdm_serial_push.py --port COM32 [--push] [--activate NAME] FILE.json ...
Without --push it only handshakes + inventories (read-only).
"""
import argparse
import glob
import json
import struct
import sys
import time

import serial  # pyserial

STX, ETX = 0x02, 0x03
TAG_JSON = 0x00


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        crc &= 0xFFFF
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def build_frame(payload: bytes) -> bytes:
    return bytes([STX]) + struct.pack("<I", len(payload)) + payload + \
        struct.pack("<H", crc16(payload)) + bytes([ETX])


def parse_frames(buf: bytes):
    """Consume complete frames from buf; return (json_objs, remaining)."""
    out = []
    while True:
        i = buf.find(bytes([STX]))
        if i < 0:
            return out, b""
        buf = buf[i:]
        if len(buf) < 5:
            return out, buf
        ln = struct.unpack_from("<I", buf, 1)[0]
        if ln == 0 or ln > 64 * 1024:
            buf = buf[1:]            # bogus STX (log text) — resync
            continue
        total = 1 + 4 + ln + 2 + 1
        if len(buf) < total:
            return out, buf
        payload = buf[5:5 + ln]
        crc_rx = struct.unpack_from("<H", buf, 5 + ln)[0]
        etx = buf[5 + ln + 2]
        rest = buf[total:]
        if etx == ETX and crc16(payload) == crc_rx and payload[:1] == bytes([TAG_JSON]):
            try:
                out.append(json.loads(payload[1:].decode("utf-8")))
            except Exception:
                pass
        buf = rest


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, 921600, timeout=0.1)
        self.buf = b""
        self.nid = 0

    def request(self, method, params=None, timeout=5.0):
        self.nid += 1
        rid = self.nid
        req = {"id": rid, "method": method, "params": params or {}}
        self.ser.write(build_frame(bytes([TAG_JSON]) + json.dumps(req).encode("utf-8")))
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
                objs, self.buf = parse_frames(self.buf)
                for o in objs:
                    if o.get("id") == rid:
                        return o
            else:
                time.sleep(0.01)
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--push", action="store_true")
    ap.add_argument("--activate", default=None)
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()

    files = []
    for f in args.files:
        files.extend(glob.glob(f))

    link = Link(args.port)
    time.sleep(0.3)

    print("-> handshake (device.info) ...")
    info = link.request("device.info")
    if info is None:
        print("  [x] no framed response. The UART switch is likely on the console "
              "(UART0) side, or the port is wrong. Flip it to the PC/UART1 position.")
        sys.exit(2)
    print(f"  [ok] link up: {json.dumps(info.get('result', info))[:160]}")

    fl = link.request("font.list")
    fonts = fl.get("result") if fl else None
    print(f"\nfonts on device: {fonts}")

    ll = link.request("layout.list")
    print(f"layouts on device: {ll.get('result') if ll else None}")

    if not args.push:
        print("\n(read-only; pass --push to write the layouts)")
        return

    print("\n-> pushing layouts ...")
    for path in files:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        name = data.get("name") or path
        r = link.request("layout.save", {"name": name, "data": data}, timeout=10.0)
        ok = r and r.get("error") in (None, "null") and "error" not in (r or {}) or (r and not r.get("error"))
        status = "[ok]" if (r and not r.get("error")) else f"[x] {r.get('error') if r else 'timeout'}"
        print(f"  {status}  {name}  ({len(json.dumps(data))} bytes)")

    if args.activate:
        r = link.request("layout.set", {"name": args.activate})
        print(f"\n-> set active = {args.activate}: {'[ok]' if (r and not r.get('error')) else r}")

    ll2 = link.request("layout.list")
    print(f"\nlayouts now on device: {ll2.get('result') if ll2 else None}")


if __name__ == "__main__":
    main()
