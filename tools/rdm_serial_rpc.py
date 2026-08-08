#!/usr/bin/env python3
"""
rdm_serial_rpc.py — talk to a dash over USB when WiFi is unavailable.

Speaks the framed JSON-RPC in main/net/uart_protocol.h, so the dash stays
controllable with no network at all (out of range, wrong SSID, AP down).

  Frame : [STX 0x02][len 4B LE][payload][CRC16 2B LE][ETX 0x03]
  Payload: [0x00 = JSON][utf-8 json]     (0x01 = binary chunk, upload only)
  CRC   : CRC16-CCITT/FALSE (init 0xFFFF, poly 0x1021) over the payload
  Baud  : 921600 on the CH343 port (NOT the 115200 console — UART0 carries
          this protocol once uart_protocol_init() takes it over, which is
          why the port looks like garbage at console baud)

Usage:
    python tools/rdm_serial_rpc.py COM40 device.info
    python tools/rdm_serial_rpc.py COM40 layout.list
    python tools/rdm_serial_rpc.py COM40 wifi.config.get
    python tools/rdm_serial_rpc.py COM40 system.reboot

SCOPE: JSON request/response methods only. Methods whose payload is streamed
as separate BINARY chunk frames (`screenshot`, `download.chunk`,
`log.download.chunk`) return their metadata here — `screenshot` replies
{size, format, width, height} — but the bytes themselves need the 0x01
chunk-frame reader, which this client does not implement. Use
GET /api/screenshot over WiFi for images; this tool exists for the case
where WiFi is down and you need to inspect or steer the dash.

Needs pyserial (`pip install pyserial`).
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
import time

STX, ETX = 0x02, 0x03
TAG_JSON = 0x00
BAUD = 921600


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(payload: bytes) -> bytes:
    return (bytes([STX]) + len(payload).to_bytes(4, "little") + payload
            + crc16(payload).to_bytes(2, "little") + bytes([ETX]))


def read_frame(ser, timeout=15.0):
    """Read one STX..ETX frame, skipping any mid-stream noise."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] != STX:
            continue
        hdr = ser.read(4)
        if len(hdr) < 4:
            continue
        n = int.from_bytes(hdr, "little")
        if n <= 0 or n > 1024 * 1024:
            continue
        payload = b""
        while len(payload) < n and time.time() < deadline:
            chunk = ser.read(n - len(payload))
            if not chunk:
                break
            payload += chunk
        if len(payload) < n:
            continue
        tail = ser.read(3)
        if len(tail) < 3:
            continue
        got = int.from_bytes(tail[:2], "little")
        if got != crc16(payload):
            print(f"  (crc mismatch, resyncing)", file=sys.stderr)
            continue
        return payload
    return None


def rpc(port: str, method: str, params=None, timeout=20.0):
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial not installed — pip install pyserial", file=sys.stderr)
        sys.exit(2)

    req = json.dumps({"id": 1, "method": method, "params": params or {}}).encode()
    # CRITICAL: do NOT let pyserial assert DTR/RTS on open. On the esptool
    # auto-reset wiring RTS drives EN and DTR drives GPIO0, so a plain
    # serial.Serial(port) REBOOTS the dash the instant you connect — which
    # both kills the RPC you were about to send and drops the dash off WiFi
    # while it reboots. Build the port unopened, clear both lines, then open.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.4
    ser.dtr = False
    ser.rts = False
    ser.open()
    with ser:
        ser.reset_input_buffer()
        ser.write(frame(bytes([TAG_JSON]) + req))
        ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            payload = read_frame(ser, timeout=max(1.0, deadline - time.time()))
            if payload is None:
                break
            if not payload or payload[0] != TAG_JSON:
                continue                      # binary/heartbeat frame — skip
            try:
                msg = json.loads(payload[1:].decode("utf-8", "replace"))
            except Exception:
                continue
            if msg.get("id") == 1:            # ignore unsolicited pushes
                return msg
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("method")
    ap.add_argument("--params", default="{}", help="JSON params object")
    ap.add_argument("--out", help="write a base64 result (e.g. screenshot) here")
    ap.add_argument("--timeout", type=float, default=20.0)
    a = ap.parse_args()

    msg = rpc(a.port, a.method, json.loads(a.params), a.timeout)
    if msg is None:
        print(f"no response from {a.port} — is the dash on that port, and is "
              f"RDM7_DEBUG_KEEP_CONSOLE off? (that flag skips uart_protocol_init "
              f"and the RPC never starts)", file=sys.stderr)
        return 1
    if msg.get("error"):
        print(f"ERROR: {msg['error']}", file=sys.stderr)
        return 1

    res = msg.get("result")
    if a.out and isinstance(res, str):
        raw = base64.b64decode(res)
        open(a.out, "wb").write(raw)
        print(f"wrote {len(raw)} bytes -> {a.out}")
        return 0
    if a.out and isinstance(res, dict):
        for k in ("data", "jpeg", "image", "b64"):
            if isinstance(res.get(k), str):
                raw = base64.b64decode(res[k])
                open(a.out, "wb").write(raw)
                print(f"wrote {len(raw)} bytes -> {a.out}")
                return 0
    print(json.dumps(res, indent=1)[:4000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
