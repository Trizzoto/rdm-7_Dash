# Camera control and video/data time alignment

Status: plan, 2026-07-28. Research verified against vendor documentation; nothing
implemented yet.

Goal: when a lap session starts, the RDM GPS puck starts the driver's action
camera; when it ends, it stops it. The video and the GPS trace must then be
alignable well enough to overlay telemetry and to jump to a specific corner on a
specific lap.

---

## 1. The constraint that shapes everything

GoPro's own FAQ, on issuing the same command to multiple cameras:

> "This is not deterministic due to camera processing and network propagation
> variance. Best case synchronization accuracy can vary from 20 – 500 ms. … In
> general, this is not a good solution for synchronization if you require more
> than 1 second accuracy. It is better to do this in post processing."

A community measurement (OpenGoPro issue #212, open since July 2022) puts the
mean at ~132 ms with ±20–40 ms residual after calibrating it out — **and notes
the latency differs per preset**.

So: **the shutter command is not a synchronisation primitive.** Any design whose
accuracy rests on "we know when we sent the command" is capped at roughly
±100 ms. That is fine for "jump to that corner". It is not fine for overlaying a
telemetry bar onto video.

Setting the camera clock does not rescue it either — the BLE `Set Date Time`
command has **no sub-second field**, so clock-based alignment is capped at ±1 s.

Precision has to come from metadata correlation after the session. Section 4.

---

## 2. GoPro — the supported path

Official, documented, and stable: **Open GoPro**. Docs at
`https://gopro.github.io/OpenGoPro/docs` (note: the older `/OpenGoPro/ble/...`
URLs that most search results still cite now 404).

Two machine-readable files are more useful than the rendered docs, several of
whose tables are client-side rendered and appear empty to a fetcher:

- `https://gopro.github.io/OpenGoPro/references.json` — every command ID, TLV
  type, and per-camera support matrix.
- `https://gopro.github.io/OpenGoPro/http/openapi.json` — the HTTP API.

### 2.1 GATT

Advertised service **`0xFEA6`** — filter on this to find cameras.

Characteristics use base `b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b`:

| Short | UUID suffix | Access | Purpose |
|---|---|---|---|
| Command | `GP-0072` | Write | commands |
| Command Response | `GP-0073` | Notify | command responses |
| Settings | `GP-0074` | Write | settings **and keep-alive** |
| Settings Response | `GP-0075` | Notify | settings responses |
| Query | `GP-0076` | Write | queries |
| Query Response | `GP-0077` | Notify | responses + async status pushes |

Two traps worth naming, because both present as "the camera won't respond":

- **Keep-alive goes to `GP-0074`, not `GP-0072`.** Every other command-shaped
  thing goes to Command, so this is easy to get wrong.
- **The camera does not cache CCCD subscriptions.** They must be re-written on
  *every* connection, including bonded reconnects.

### 2.2 Commands

TLV bodies with a packet header prepended. GoPro's own SDK always uses the
13-bit extended header, and so should we — it avoids implementing two header
formats on the transmit side.

| Operation | Char | Bytes |
|---|---|---|
| Shutter ON | `GP-0072` | `20 03 01 01 01` |
| Shutter OFF | `GP-0072` | `20 03 01 01 00` |
| Keep Alive | `GP-0074` | `20 03 5B 01 42` |
| Get Hardware Info | `GP-0072` | `20 01 3C` |
| Hilight Moment | `GP-0072` | `20 01 18` |
| Set Local Date Time | `GP-0072` | `20 0C 0F 0A <7 bytes> <int16 utc_off_min> <dst>` |
| Get Status (encoding, busy) | `GP-0076` | `20 03 13 0A 08` |
| Register for Encoding pushes | `GP-0076` | `20 02 53 0A` |
| Enable WiFi AP | `GP-0072` | `20 03 17 01 01` |

All multi-byte fields are big-endian. Status **10 (Encoding)** is the "am I
recording" flag; status **8 (Busy)** must be clear before sending anything other
than a query.

Register for pushes rather than polling — notifications arrive with ID `0x93` on
`GP-0077`.

### 2.3 Fragmentation

BLE payload is assumed to be 20 bytes; GoPro does not rely on MTU negotiation, so
**do not try to negotiate around writing the accumulator**. Messages longer than
20 bytes split into a start packet plus continuation packets whose header has
bit 7 set and a 4-bit counter that wraps after `0xF`.

Our commands are all short. Our *responses* are not — `Get Hardware Info` returns
model, firmware, serial, SSID and MAC and will always fragment. Since readiness
detection depends on parsing that reply, an implementation that ignores
continuation packets fails at the very first step.

### 2.4 Keep-alive

Every **3.0 s**, for the whole session, unconditionally, from the moment the
connection is up. Two independent timers must both expire for the camera to
sleep — Auto Power Down and the keep-alive timer — and only keep-alive messages
reset the second one. Omitting this is the classic "worked on the bench for two
minutes, died on track".

### 2.5 Pairing

Set up once. First connection needs the camera put into pairing mode through its
own UI (Connections → Connect Device → Quick App); after that the camera stores
the bond and subsequent connections need no user action. It advertises whenever
powered, **and for 8 hours after being put to sleep** — and connecting to a
sleeping camera wakes and boots it.

For a track day that is the right behaviour: the camera gets switched on in the
paddock, and the puck can start it thereafter without anyone touching it.

Note also: up to 4 simultaneous BLE connections, but only one WiFi client.

### 2.6 Readiness

BLE is not usable immediately after connect. Poll `Get Hardware Info` until it
returns success before sending anything else.

---

## 3. DJI — viable, with caveats

**`https://github.com/dji-sdk/Osmo-GPS-Controller-Demo`** — under DJI's official
GitHub org, created 2025-02-05, last pushed 2025-11-17, MIT licensed, not
archived.

It is startlingly close to our product: **ESP32-C6** host, a Quectel LC76G GNSS
at up to 10 Hz, implementing DJI's "R SDK protocol" to do start/stop recording,
**GPS injection into the camera**, mode switching and status subscription.
Targets Osmo Action 4 / 5 Pro / 6 and Osmo 360.

Because it is ESP32-based and ESP-IDF, the porting cost to our puck is unusually
low.

**But** it is a demo, not a supported SDK. MIT licensed and community-supported;
DJI owes us no compatibility guarantee and can change the protocol in a firmware
update. Confidence it works today: high. Confidence it is a stable long-term
platform: medium.

**Unverified and material:** the demo's README does not document GATT UUIDs or
the pairing handshake — those need reading out of the source (roughly half a day)
— and, more importantly, **whether DJI writes a timestamped metadata track and in
what format is unknown**. That single unknown decides whether DJI can meet the
alignment requirement at all, because everything in §4 depends on it.

**Sequence: GoPro first**, because its protocol is *specified* rather than
*exemplified* and we can reason about alignment precision from published metadata
documentation. DJI second, once GoPro ships.

---

## 4. Time alignment

### 4.1 What's in a GoPro file

Every telemetry-carrying GoPro MP4 has a `GoPro MET` track carrying GPMF (KLV,
big-endian, FourCC keys):

| Key | Content | Rate |
|---|---|---|
| `ACCL` / `GYRO` | accel / gyro | 200 / 400 Hz |
| **`GPS9`** | **lat, lon, alt, 2D+3D speed, days-since-2000, secs-since-midnight (ms), DOP, fix** | **10 Hz** |
| `GPS5` | older GPS stream, deprecated | 18 Hz |
| `CORI`/`IORI`/`GRAV` | orientation quaternions, gravity | frame rate |
| `TSMP` | samples since record start | per payload |

`GPS9` is the one that matters: absolute UTC to millisecond precision, with a
quality indicator, indexed against the video timeline.

Sample rates are **nominal, not exact** — GoPro state that timing is determined
after capture. A "200 Hz" stream measures at ~201.9 Hz. Recover the true rate by
differencing `TSMP` across the file against the track duration.

Not available in real time. GoPro: *"the GPS track on the camera as well as other
metadata is not available until the file is written and saved."* Retrieval is
post-session over WiFi: `GET /gopro/media/gpmf?path=<file>`.

### 4.2 ⚠️ HERO12 has no GPS

From the GPMF specification: HERO12 removed both `GPS5` and `GPS9` — *"No GPS
receiver in HERO12"*. HERO13 restores it. HERO10 Bones also lacks the GPS board.

So metadata correlation works on **HERO9, 10, 11, 13 and MAX 2**, and is
**unavailable on HERO12** — a 2023 camera still very much in circulation.

This must be detected at connect time from `Get Hardware Info` and surfaced to
the user as a deliberate product behaviour. It must not be an emergent surprise
on someone's track day.

### 4.3 The strategy

Two layers: a universal real-time baseline, refined afterwards where the camera
can support it.

**Layer 1 — every camera, always, in real time.**

Latch three GNSS-disciplined timestamps and store them with the session, along
with the active preset ID:

- `T_cmd` — immediately *before* writing shutter-on
- `T_ack` — when the success notification lands on `GP-0073`
- `T_enc` — when status 10 (Encoding) flips true; the tightest real-time bound

Storing all three costs nothing and lets us build a per-preset latency model from
real sessions rather than guessing at one. Accuracy: ~±100 ms — enough to jump to
the right corner, not enough to overlay.

**Layer 2 — post-session, where the camera has GPS.**

- **2a, absolute UTC.** Parse `GPS9`, map its UTC against the video timeline.
  Both devices are now timestamped by the same constellation. Residual is
  sample-to-frame indexing, ~10–50 ms.
- **2b, speed cross-correlation.** Both devices measured *the same car*.
  Cross-correlate the camera's `GPS9` 2D speed against the puck's speed trace and
  take the lag at peak correlation. This **trusts no clock on either device** —
  not the camera's, not ours, not the command latency. On a circuit the speed
  trace is dense with braking zones and apexes, so the peak is sharp.

Run both and use their agreement as a confidence metric: agreement within ~50 ms
means high confidence; divergence flags the session for review.

**2b is the one to rely on.** It is the only method whose accuracy does not
depend on any clock we do not control.

### 4.4 Rejected, and why

| Option | Accuracy | Why not |
|---|---|---|
| Camera clock + file timestamps | ±1 s | `Set Date Time` has no sub-second field; media-list `cre`/`mod` are Unix seconds |
| Shutter-ack timestamping alone | ±100 ms | GoPro's latency is officially non-deterministic (§1) |
| GoPro Labs `*SYNC=1` | ~33–66 ms | Excellent, but requires the customer to flash alternative firmware. Offer as an optional precision mode, never the default |
| Visual/audio sync marker | 1 frame / ~1 ms | Accurate, but needs extra hardware and manual post-processing |

GoPro Labs is worth exposing as an opt-in: `*SYNC=1` disciplines the camera clock
to GPS at millisecond precision, and `*TCAL=<±ms>` trims residual offset. On
HERO13/MAX 2 those can even be pushed programmatically over WiFi via
`GET /gopro/qrcode?labs=1&code=<cmd>` rather than shown as a QR code to the lens.

---

## 5. ESP32-S3 constraints

**Radio.** ESP32-S3 is Bluetooth 5 (LE) only — no Classic. Costs us nothing here;
both GoPro and DJI are BLE. Central role supported.

**Stack.** NimBLE, not Bluedroid. Bluedroid exists for dual-mode Classic+LE which
the S3 cannot do anyway; NimBLE is materially smaller for a central-only role.
`ble_gattc_*` is the client-side API.

**Coexistence — the architectural decision.** From the ESP-IDF 5.3.1 coexistence
matrix for ESP32-S3:

| BLE state | WiFi STA connected | SoftAP beacon | **SoftAP connected** |
|---|---|---|---|
| Connected | **Y** stable | **Y** stable | **C1 unstable** |

BLE-central plus WiFi STA is officially stable. BLE-central plus a *client
associated to our SoftAP* is officially unstable — and our puck runs SoftAP for
provisioning.

**Therefore: provisioning and camera control are mutually exclusive phases.**
Provision over SoftAP with BLE idle; tear the AP down before a session; run
BLE + STA during it. Provisioning is a one-time setup action and a session is a
separate mode, so this costs nothing in usability and keeps us out of the only
unstable cell in the table.

Mitigations to apply regardless:

- Pin the WiFi stack and the BLE controller/host to different cores (the S3 has
  two).
- `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` must be on.
- Leave WiFi connectionless power-save at defaults.
- Do not enable LE Coded PHY — it degrades WiFi and we do not need range to a
  camera on the same car.
- Use a relaxed connection interval (50–200 ms). We send one keep-alive per 3 s
  and a handful of commands per session; this is an extremely low duty-cycle
  link, and asking for 7.5 ms out of habit just steals radio time from WiFi.

---

## 6. Session sequence

```
one-time setup
  user puts camera in pairing mode (camera UI)
  puck scans, filters advertisements for 0xFEA6
  connect, LE Secure Connections pairing, bond stored both sides

every session
  connect to bonded peer            (wakes the camera if asleep <8 h)
  discover services
  write CCCD on GP-0073/0075/0077   ** must be redone every connect **
  poll  GP-0072 <- 20 01 3C         until status 0  (BLE ready)
  start 3.0 s keep-alive on GP-0074 <- 20 03 5B 01 42
  register GP-0076 <- 20 02 53 0A   (Encoding pushes)
  set local date/time               (1 s resolution — for file naming only)
  wait for status 8 (Busy) == 0

  T_cmd = GNSS now
  GP-0072 <- 20 03 01 01 01         SHUTTER ON
  T_ack = GNSS now                  (GP-0073: 01 00)
  T_enc = GNSS now                  (GP-0077: Encoding true)

    ... 20–500 ms of camera latency lives here ...
    ... keep-alive continues every 3.0 s ...
    ... optional 20 01 18 Hilight at known GNSS instants ...

  GP-0072 <- 20 03 01 01 00         SHUTTER OFF

post-session, over WiFi
  GP-0072 <- 20 03 17 01 01         enable AP
  read GP-0002 / GP-0003            SSID / password
  GET /gopro/media/list             filenames + cre (Unix s)
  GET /gopro/media/gpmf?path=<f>    GPMF container
  parse GPS9  -> absolute UTC, 10 Hz, ms precision
  cross-correlate GPS9 speed vs puck speed -> refined offset
```

---

## 7. Build order

1. **NimBLE central skeleton** — scan, filter `0xFEA6`, connect, bond, discover,
   subscribe. Prove it against a real camera before writing any protocol logic.
2. **Framing layer** — the header/continuation accumulator, tested on host
   against captured byte sequences. This is pure logic with no ESP-IDF
   dependency, so it belongs beside `lap_core.c` in the host test harness.
3. **Command set** — readiness poll, keep-alive timer, shutter, status
   registration. Keep-alive first; nothing else stays up without it.
4. **Session hooks** — tie shutter on/off to lap session start/stop, latch the
   three timestamps into the trace file.
5. **Studio UI** — pair, show connection and recording state, model detection
   with the HERO12 warning.
6. **Post-session correlation** — GPMF fetch and parse, then 2a and 2b, in
   Studio rather than on the puck.
7. **DJI** — only after the above is shipped and stable.

Steps 1–4 are on the puck; 5–6 are Studio; the framing layer in 2 is host-testable
and should be written that way.

---

## 8. Open items

- DJI GATT UUIDs and pairing handshake — read out of the DJI demo source.
- **Whether DJI writes a timestamped metadata track, and its precision.** This
  decides whether DJI can meet the alignment requirement at all.
- ESP32-S3 maximum simultaneous BLE connections — not re-verified.
- Our own per-preset shutter latency measurements. GoPro's 20–500 ms is
  authoritative; the ~132 ms mean is one community measurement and should not be
  trusted until we have measured our own.
- Whether to expose GoPro Labs precision mode, and how to document the firmware
  flash without creating a support burden.
