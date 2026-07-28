# ADR 0008 — GPS lap timing: how the puck, the dash and the desktop suite fit together

Status: Accepted (2026-07-20) — dash side implemented. Puck USB RPC + Studio's
RDM GPS **node configurator** built 2026-07-27 (see the addendum); the desktop
**session-analysis** workspace is still not built.

Supersedes nothing. Implements `docs/PLATFORM_PLAN_2026-07.md` §6.2.

## Context

Phase 2 of the platform plan adds an **RDM GPS** — a 25 Hz GNSS puck that hangs
off the CAN bus — and turns the RDM-7 into a lap timer. Three codebases have to
agree for that to work:

| Repo | Role |
|---|---|
| `rdm-gps-node` | Puck firmware: u-blox NEO-M9N → CAN |
| `RDM-7_Dash` (this) | Lap engine, channels, widgets |
| `rdm7-desktop` | RDM Studio: configuration and session analysis |

Before this ADR, the dash had six lap-timing canonical channels defined
(`lap_time_current`, `lap_time_last`, `lap_time_best`, `lap_number`,
`lap_delta_best`, `sector_time_current`) that **nothing ever fed**, and no GPS
channels at all. There was also no CAN ID allocation for RDM-designed bus
devices — only OBD2 IDs were pinned anywhere.

The market research (`docs/research/2026-07-gps-laptimer-market.md`) sets the
bar: 25 Hz, auto track detection, GPS start/finish with no beacon, best/last/
**predictive** delta, sectors, and ±0.02 s. That last number is the constraint
that drives most of what follows.

## Decisions

### 1. The puck is dumb; the lap engine runs on the dash

The RDM GPS broadcasts position, motion, accuracy and time. It computes
nothing. All lap logic lives in `main/lap/` on the dash.

Why: the lap engine then survives a GPS-vendor swap, and any CAN GPS that can be
made to speak the published DBC works with the dash. It also puts the engine
where the track database, the UI and the storage already are. The cost is bus
traffic (~1.4% at 500 kbit/s), which is nothing.

### 2. CAN ID allocation for RDM bus devices: `0x400`–`0x4FF`

Defined in the puck's `docs/PROTOCOL.md` and duplicated as frame offsets in
`main/lap/lap_engine.c`. Blocks of 16 IDs per device:

| Range | Device class |
|---|---|
| `0x400`–`0x43F` | RDM GPS (4 instances) |
| `0x440`–`0x47F` | RDM IO (reserved, Phase 3) |
| `0x480`–`0x4BF` | RDM Keypad native mode (reserved; Blink units use CANopen) |
| `0x4C0`–`0x4FF` | Reserved |

Chosen to clear the ECU broadcast ranges the shipped presets use (Haltech
`0x360`–`0x373`, Link `0x3E8`–`0x3F1`), MoTeC (`0x500`+), MaxxECU (`0x520`+),
CANopen SDO (`0x580`+) and OBD2. It is numerically *below* OBD2, so 25 Hz
telemetry wins arbitration against diagnostic polling but still loses to an
ECU's critical low-ID frames.

The base is configurable because no reservation survives contact with somebody
else's bus.

### 3. GPS channels bind via channel-owned decode, NOT the ECU preset system

`lap_engine_bind_gps_channels()` activates the eight Position & GPS canonical
channels and calls `channel_manager_set_decode()` on each with the bit layout
from the published DBC.

Rejected: adding `ECU_SIG_GPS_*` slots to `ecu_signal_slot_t`. Reasons:

- A GPS is not an ECU. A car has one ECU preset *and* possibly a GPS; the
  preset system assumes one exclusive choice and rewrites the layout's
  `signals[]` wholesale.
- Adding slots means three parallel tables (`ECU_SIGNAL_NAMES`,
  `ECU_SIGNAL_CANONICAL`, `ECU_SIGNAL_ALIASES`) must stay in lockstep.
- ADR-0005 already moved decode ownership to the channel. The preset path is
  the legacy one; new device classes should not extend it.

Consequence: one function call configures a GPS completely. No DBC import step,
no manual bit entry — which is the "zero extra hardware, under ten minutes"
promise the platform plan makes.

### 4. The lap engine reads raw CAN frames, not the GPS channels

**This is the non-obvious one.**

`channel_t.current_value` is a `float`. A float32 carries ~24 bits of mantissa,
so a longitude of 138.6° quantises to about 1.5e-5° — roughly **1.7 metres**,
growing with distance from the prime meridian.

The accuracy target is ±0.02 s. At 200 km/h (55.6 m/s) that is **1.1 metres**.
Reading position from a float channel would put the quantisation error above
the entire accuracy budget — and because it is quantisation rather than noise,
it does not average out.

So `lap_engine_on_can_frame()` is called from `can_process_queued_frames()`
alongside `signal_dispatch_frame()`, decodes the position frame's two `int32`
fields itself, and carries them as `double` all the way into `lap_core`. The
`gps_latitude` / `gps_longitude` channels still exist and are still bound — they
are for display and logging, where a metre does not matter.

Sub-fix-interval interpolation is the other half of hitting ±0.02 s: at 25 Hz
the fixes are 40 ms apart, so the crossing instant is interpolated between the
two fixes that straddle the line, and crossing timestamps are held as `double`
milliseconds rather than being rounded back to integers.

### 5. `lap_core.c` has no ESP-IDF, so the tests compile it directly

`main/lap/` is split in two:

- `lap_core.c` — geometry, crossing detection, interpolation, sectors,
  predictive delta. No LVGL, no FreeRTOS, no `esp_*`.
- `lap_engine.c` — CAN frames, channels, signals, persistence, logging.

`tests/native/test_lap_core.c` therefore **compiles the firmware source**
rather than mirroring its arithmetic, which is what `test_calculated_gear.c`
and `test_ecu_preset_match.c` are forced to do because the real code drags in
the IDF. A mirrored test can drift out of lockstep with the code it claims to
cover without anything failing. New modules should follow this split.

### 6. New channel group `CHGRP_POSITION`, appended

Group indices are **persisted numerically** in `channels.json`
(`"group": <int>`). Inserting a group in the middle of `channel_group_t` would
silently regroup every channel on every device already in the field. The new
group is therefore appended after `CHGRP_DASH_SYSTEM` as index 13, and the enum
now carries a comment saying so.

### 7. Predictive delta indexes by distance, not by time

The reference lap is stored as "elapsed time at each distance bucket around the
lap" (512 buckets, adapting to lap length after the first completed lap).
Indexing by distance is what makes the delta predictive: at any moment we know
how far round we are, so we can ask how long the reference took to get this far.
Indexing by time would only say where we *were*.

Distance is integrated from **Doppler ground speed**, not from differencing
positions. Position differencing accumulates GPS noise into hundreds of metres
of phantom distance while a car sits in the pits; Doppler speed reads a true
zero.

## Who owns what

| Concern | Owner | Notes |
|---|---|---|
| GNSS → CAN | `rdm-gps-node` | Published DBC; no lap logic |
| CAN → channels | `lap_engine_bind_gps_channels()` | ADR-0005 channel-owned decode |
| Lap arithmetic | `main/lap/lap_core.c` | Host-tested |
| Track storage | `/lfs/lap_track.json` | Atomic write, `schema_version` |
| Lap config UI | firmware `main/web/index.html` | **Not yet built** |
| Session analysis | `rdm7-desktop` only | **Not yet built** — heavy plots |
| Puck device config | `rdm-gps-node` USB RPC + Studio's RDM GPS workspace | Desktop-only of necessity — see the 2026-07-27 addendum |

Per ADR-0007 the lap **configuration** UI belongs in the firmware editor first
so it is phone-configurable, and the desktop inherits it through
`sync_firmware.py`. Session analysis is desktop-exclusive, and so — for a
different and stronger reason — is configuring the puck itself.

## Consequences

Accepted:

- The RDM GPS frame offsets are duplicated between `rdm-gps-node`'s
  `rdm_gps_proto.h` and this repo's `lap_engine.c`. The two repos cannot share
  a header. Mitigated by the protocol being additive-only after first ship.
- The lap engine runs on the LVGL task, in the CAN drain path. It rejects
  non-GPS ids on the first compare, and at 25 Hz the work is trivial arithmetic
  — but it is on the critical rendering path and must stay cheap.
- `signal_source_t` still has no `SIGNAL_SOURCE_GPS`; lap outputs register as
  `SIGNAL_SOURCE_INTERNAL`. The plan calls for a GPS value, but it is
  provenance-display only and adding it means three string-mapping sites
  (`web_server_channels.c`, `web_server_signals.c`, `layout_manager.c`) must
  agree. Deferred deliberately, not forgotten.

Built since acceptance (2026-07-20, same day):

- ~~Lap config UI~~ — the **capture flow**: `POST /api/lap/capture` takes the
  car's live position AND course from the GPS stream, so setting up a track is
  "drive across the line, tap the button". Six endpoints in
  `main/net/web_server_lap.c`, modal in the firmware editor (Setup ▸ Lap
  Timing). Desktop inherits on the next `sync_firmware.py`.
- ~~`M:SS.sss` formatting~~ — `channel_format_display_value()` now renders
  Lap Timing group channels ≥60 s as `M:SS.sss`; deltas stay signed decimals.
- Detection hardening: auto-bind requires the status frame to identify as
  `GpsDeviceType 0x02`, and 29-bit ids numerically inside the block are
  rejected (the coalesce buffer now carries `extd`).

Timing honesty note: fixes are timestamped when the LVGL task drains the CAN
queue (~2 ms cadence), not at bus reception, so crossing timestamps carry a few
milliseconds of scheduling jitter — ~0.1–0.2 m at 200 km/h, inside the ±0.02 s
budget but not free. If the budget ever tightens, timestamp in the RX task.

Still to build:

- Track database with auto-detection. Today one track is stored; the plan wants
  ~100 seeded circuits with auto-selection by proximity.
- Race Page bundled layout, predictive-delta bar widget, lap list panel.
- Session logging to SD with a lap/sector index, and the desktop analysis
  workspace that reads it.

## Addendum — 2026-07-27: the puck's USB RPC, and how Studio tells devices apart

The puck gained a **USB JSON-RPC** interface (`rdm-gps-node/main/net/serial_rpc.c`,
specified in that repo's `docs/USB_RPC.md`), and RDM Studio gained an **RDM GPS**
workspace that speaks it. Both verified against the rev A board on 2026-07-27
with a live 15–16 satellite fix.

**Decision 1 is unchanged.** This is a configuration and diagnostics channel,
not a data path. The puck still computes nothing, the lap engine still lives on
the dash, and CAN broadcast is identical whether or not a USB host is attached.
Nothing in the lap pipeline reads USB.

Why it exists: without it the puck can only be configured over CAN (`0x40E`
command frames), which requires something already on the bus. That is a poor
first-run experience for a device with a USB socket, and it makes WiFi
provisioning circular — you would need the network to configure the network.

### The wire format is deliberately the dash's

`[STX][len 4B LE][payload][CRC16-CCITT 2B LE][ETX]`, `payload[0] = 0x00` for
JSON — byte-identical to `main/net/uart_protocol.h`. Studio therefore talks to
both devices through one `UsbTransport` and one set of Rust serial commands,
with no second protocol implementation to keep in step. Console `ESP_LOG` output
shares the same CDC interface (the S3's USB-Serial/JTAG gives exactly one), so
every client must discard non-frame bytes; Studio's Rust parser already did.

### `device_type` is the discriminator — and its absence means "dash"

`device.info` on the puck returns `"device_type":"gps"`. Dash firmware predates
the field and answers with `schema` instead, so **absent is treated as "dash"**.
That default is the whole reason no device already in the field changes
behaviour.

This closed a real bug rather than merely adding a feature. Studio's connection
test opened with `listLayouts()`, so a perfectly good link to a puck was
reported to the user as a **connection failure**. The flow now probes
`device.info` first and only asks a `"dash"` for layouts, images or fonts. The
same guard sits in the `/api/*` router (dash-only endpoints are answered locally
rather than sent to a node that has no such RPC) and in the connection UI, which
hides backup/restore, OTA and the live screen mirror when the connected device
is not a dash. The Rust probe behind USB auto-detect had the same assumption
baked in — it required a `schema` field — and so had never been able to find a
puck at all.

### The node configurator is desktop-first of necessity

ADR-0007 and the table above put configuration UI in the firmware editor first,
so it is phone-configurable, with the desktop inheriting it via
`sync_firmware.py`. **The RDM GPS workspace is a permanent exception**: the puck
has no display, no web server and no HTTP API. USB is its only configuration
path and a browser cannot open a serial port, so there is no firmware editor for
this UI to live in. This narrows nothing about ADR-0007 for the dash's own
editor.

It is also distinct from the existing **GPS Lap Timer** workspace, which is
unchanged. That one does tracks, gates and delta analysis; this one configures
and proves out the device — fix state, satellites, PDOP, accuracy, link and CAN
counters with derived rates, live IMU, WiFi provisioning and identify.

### Honesty constraints this interface forces on the UI

These are not stylistic. Each is a case where the obvious reading of a reply is
wrong, and each cost real debugging time to establish:

- **`identify` returns `led_healthy: true` on a board whose LED cannot light.**
  The field reports the RMT channel, which stays healthy while transmitting an
  illegal waveform (`REV_A_ERRATA` E6) — and on rev A the pixel is fitted 180°
  out with its data input wired to nothing (E7). Studio warns *before* the
  click and never reports a flash the hardware cannot produce.
- **Opening the CDC port resets the S3.** A stale `link:false`, or no fix
  immediately after connecting, is expected rather than a fault; a cold start
  takes 10–30 s. The workspace says so instead of showing an unexplained blank.
- **`wifi.config.get` never returns the password**, only `has_password`. An
  absent `password` on set leaves the stored one alone, so Studio omits the key
  entirely when the field is blank — an empty box *keeps* the saved key, it does
  not clear it. Sending `""` would risk overwriting a customer's key with
  nothing. `reboot_required` is reported as received, not assumed.
- The node **stores** WiFi credentials but does not yet bring a station up.
  Provisioning deliberately landed ahead of the radio, and the UI says so.
- **Absent is not zero.** Position, altitude, speed, PDOP and accuracy are
  omitted from `gps.status` entirely until there is a fix. In JavaScript
  `null / 1000` is `0`, so the naive formatter renders a confident `0.00 m` for
  a reading that was never sent — and then grades it red. Every scaled readout
  goes through one helper that preserves "absent" for exactly this reason.

Position stays in raw 1e-7-degree integers end to end — wire, `gps.status`, and
Studio's own state — with only the renderer dividing, at the full seven decimal
places the wire carries. Converting on the way in would cost ~1.7 m, which is
the whole reason decision 3 keeps the integer.
