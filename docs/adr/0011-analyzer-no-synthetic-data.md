# ADR 0011 — The CAN analyzer never fabricates traffic

**Status**: Accepted (2026-07-25)
**Context**: RDM Studio's CAN Bus Analyzer shipped with a built-in demo
generator: with no dash connected it synthesized a coherent "car" (RPM,
throttle, coolant, road speed) on four proprietary CAN IDs plus a matching
slow OBD2 surface, so both the bit probe and the OBD↔CAN Match workbench had
something to chew on at a desk. That has been removed. The analyzer now shows
the real bus or nothing, and names the reason when it's nothing.

## The problem we were solving

The demo traffic was written to make the workspace demonstrable on the bench.
In practice it made the workspace *untrustworthy in the field*, because
nothing in the UI distinguished the two sources strongly enough:

- The only tell was a topbar chip reading "no dash link — demo traffic". Every
  other affordance — the frame table, the Hz column, bus load, the bit-activity
  heat map, the live trace, the Match ranking — behaved identically. A user who
  glanced past the chip was reverse-engineering a signal that does not exist on
  their car.
- Match would happily "recover" a decode from demo data and present it with an
  **Excellent match** verdict and a *"enter this in the Channels tab"*
  instruction. The recovered decode was for the demo generator's own arithmetic.
- Worse, the demo path was the *fallback* for every failure. Dash asleep, wrong
  subnet, USB rather than WiFi, device simulator left on — all of them landed in
  the same place: a screen full of plausible, moving, entirely fictional frames.
  The one moment a diagnostic tool must be loudest is exactly when it cannot see
  the thing it is diagnosing.

A CAN analyzer's whole value proposition is "this is what is on your wire". A
mode where that sentence is false, distinguished only by a small grey chip, is
not worth what it costs.

## Options considered

1. **Keep the demo, mark it harder** — red border, watermark, disable Match's
   adopt/copy actions. Rejected: still leaves the failure fallback pointing at
   fiction, and the "make it obvious" bar keeps rising forever.
2. **Keep the demo behind an explicit Developer-Options toggle**, default off.
   Rejected as unnecessary — the firmware already has both simulators
   (`/api/signal/simulate`, `/api/obd2/sim`), on the device, where a bench demo
   actually exercises the real pipeline end to end. A second, desktop-side
   simulator duplicates that with strictly less fidelity.
3. **Delete it and make every empty state explain itself.** Chosen.

## Decision

**The analyzer renders device data only.** No client-side generator exists, and
none should be reintroduced. Where frames cannot be read, the workspace names
the specific cause and offers the one action that fixes it.

The link state machine (`ca.link.state` in `tauri-overlay.html`):

| State | Meaning | Offered action |
|---|---|---|
| `offline` | Local/offline mode | Connect over WiFi |
| `usb` | Serial link — the per-ID tracker dump is HTTP-only (there is no `can.monitor` serial RPC) | Connect over WiFi |
| `nolink` | Dash configured, health check failing | Reconnect |
| `error` | `/api/can/monitor` erroring (2 consecutive failures before it says so) | Reconnect |
| `sim` | Device signal simulator running — `can_manager.c` skips the entire rx drain, tracker included, so no frame can arrive | Turn simulator off |
| `silent` | Linked, tracker empty — wiring / ignition / bitrate | — (coaching text) |
| `live` | Frames flowing | — |

Riding alongside any state: **virtual ECU on** (`/api/obd2/sim` → `sim:true`)
warns that the OBD2 reference is synthesized by the dash, so a Match run against
it correlates the dash with its own synthesizer. Also surfaced: the 64-entry
`CAN_ID_TRACKER_MAX_IDS` ceiling, once reached.

Two supporting changes fall out of the same principle — *show what is, not what
is convenient*:

- **Bus load uses the dash's real bitrate**, read once from `/api/can/config`
  (index → 125k/250k/500k/1M, per `_bitrate_to_timing`), instead of a UI guess.
  A manual pick still wins and sticks.
- **The OBD2 reference list comes from `/api/signals/values`** (`source:"obd2"`),
  not from the layout's channels. Channels only supply the label and units when
  one happens to bind that signal. A polled PID with no channel mapped is a
  perfectly good reference and used to be invisible. Never-answered and stale
  PIDs are listed and marked (grey / amber dot) rather than dropped, so the rail
  stops flickering — but a stale reading is still skipped when *recording*, since
  it is a frozen last-value and would feed the correlator a flat run that never
  happened on the car.

## Consequences

- **Good**: what the analyzer shows is what is on the wire. The Match
  workbench's "adopt this decode" instruction can be trusted.
- **Good**: every failure mode is now named and actionable rather than masked.
  The device-simulator case in particular was previously invisible — the
  simulator silently starves the tracker, so the analyzer looked merely quiet.
- **Bad**: no offline demo. Screenshots, sales demos and UI work on the analyzer
  now need a dash on the bench. That is the intended trade: the device
  simulators (`/api/signal/simulate` for signals, `/api/obd2/sim` for a virtual
  ECU answering real PID requests) cover the bench case with real plumbing.
- **Neutral**: the analyzer now polls two extra endpoints while open —
  `/api/signal/simulate` at 2 s (lock-free on the device) and `/api/obd2/sim` at
  6 s (takes the LVGL lock, hence the slower cadence).

## References

- Code: `../rdm7-desktop/src/tauri-overlay.html` (block `suite-home-keypad` —
  `ca.link`, `_caLinkMode`, `_caRenderLink`, `_caPollDeviceState`, `_caPoll`,
  `caMatchRefList`)
- Firmware: `main/net/web_server_test.c` (`/api/can/monitor`),
  `main/net/web_server_signals.c` (`/api/signals/values`, `/api/signal/simulate`),
  `main/net/web_server_obd2.c` (`/api/obd2/sim`, `/api/obd2/pids`),
  `main/can/can_manager.c` (`signal_sim_is_active()` gating the rx drain),
  `main/can/can_id_tracker.h` (`CAN_ID_TRACKER_MAX_IDS`)
- Related ADRs: 0007 (the overlay is where desktop-only behaviour belongs)
