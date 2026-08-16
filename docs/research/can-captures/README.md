# Raw CAN captures

Reference traces pulled off a dash with the raw-frame logger
(`can_raw_logger.c` — every frame, no decoding). Kept so Link/ECU decode work
can be re-checked offline without a car on the bench.

Format is the logger's CSV: `Time Stamp,ID,Extended,Bus,LEN,D1..D8`.
Timestamps are microseconds since boot. Bytes are `0x`-prefixed.

Replay one onto a dash with `POST /api/replay/start` (`signal_replay.c`), or
just parse it directly.

## 2026-08-16_RDM-DCB4-D926_canraw.csv

- Device `RDM-DCB4-D926`, firmware 1.4.2 (`ca2f331` + local worktree changes).
- 12,729 frames over 40.0 s (~319 frames/s), 824 KB.
- Captured to LittleFS (no SD mounted), downloaded via
  `GET /api/log/download?name=canraw_48.csv`.

Two IDs on the bus:

| ID | Frames | Rate | Notes |
|---|---|---|---|
| `0x3E8` | 11,135 | 278.6 Hz | Link Generic Dash, multiplexed on byte 0 |
| `0x44C` | 1,594 | 39.9 Hz | second stream, not yet identified |

**This is the trace that demonstrates the Link mux layout.** `0x3E8` carries
14 distinct sub-frames selected by byte 0 (`0x00`–`0x0D`), each arriving at
~20 Hz (793–797 occurrences apiece over the 40 s window). It is *one* CAN id
muxed fourteen ways — not fourteen consecutive ids, which is what the channel
catalogue wrongly assumed before `c60064d`. Any regression in Link detection
or decode should be reproducible against this file.
