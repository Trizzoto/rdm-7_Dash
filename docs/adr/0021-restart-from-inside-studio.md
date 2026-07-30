# ADR-0021: Everything can be restarted from inside Studio, and recording is red

Date: 2026-07-30
Status: Accepted
Repos: rdm7-desktop (`src-tauri/src/lib.rs`, `src/tauri-overlay.html`, `src/transport.js`)

## Context

Three restart-shaped gaps surfaced in one evening on the bench:

1. **Studio itself had no recovery path.** When the app misbehaves, the only
   remedies were OS-level: kill the process or reboot the PC. The user asked
   for "a restart in File or something in case the app is playing up".
2. **The node had none either.** The GNSS receiver went quiet mid-session with
   both health counters at zero — the silence watchdog lives inside the very
   task that wedged (`gnss_task`), so it can never fire for its own death, and
   there is no reboot RPC (a wedged firmware could not answer one anyway).
   The only remedy was pulling the plug.
3. **Recording state was a text row.** Whether the puck was logging — the one
   fact a driver must not misread — was stated in 11 px grey type.

## Decision

**Two recovery items at the bottom of File, handled natively.** *Reload
Interface* runs `win.eval("window.location.reload()")` and *Restart RDM
Studio* runs `app.restart()`, both in `on_menu_event` in Rust — deliberately
NOT routed through the `menu-action` event like every other item, because
their whole reason to exist is the day the frontend is wedged and an emitted
event would land in a dead listener.

**A "Restart the node" button in Setup → Device, implemented as a hardware
reset over the USB control lines** (`serial_pulse_reset`). The puck is a
native USB-Serial-JTAG device (VID 303A); its reset latch answers only to the
full esptool handshake — idle → DTR-arm → the inverted RTS double-write →
release (which lands in the bootloader), then the classic RTS pulse out of
the bootloader into the app. Simpler patterns were tried first and the ubx
counter proved the chip never reset: a lone RTS pulse and a plain both-lines
toggle both do nothing from a running app. The command takes the port OUT of
the shared state for the whole ceremony (concurrent RPCs fail fast instead of
queuing on the mutex), then closes, waits for re-enumeration, reopens, and
restarts the log drain. WiFi-attached nodes show "USB only" instead of a
button — there is no EN line to pulse.

**● REC, the camera convention.** A topbar chip — red, pulsing, in every view
— when the node reports logging; a still amber "❚❚ logging paused" when it
does not; hidden when no node/no report. The readiness card's Logging row
uses the same red pulse (tone `rec`), so red means exactly one thing
everywhere. `trace.info` is refreshed every 5 s on the lap poll's clock
(never during a download), because a reboot resumes logging by itself and a
chip fed only by on-demand fetches would lie about it.

## Consequences

- `app.restart()` under `cargo tauri dev` detaches the app from the watcher —
  the watcher sees its child exit and shuts down, and hot-rebuild stops. On
  an installed build there is no watcher and the behaviour is clean. Dev-only
  caveat, noted in the project memory.
- A standalone `cargo build` debug exe **embeds** the frontend at compile
  time; only `tauri dev` serves `src/dist` live from disk. Frontend changes
  therefore need a rebuild when running standalone — *Reload Interface*
  reloads state, not assets, in that configuration.
- The node-restart confirm dialog states the true costs: settings, track and
  the recording ring survive (verified in `trace_log.c` — boot recovers the
  write head from sector headers); the live session resets; logging comes
  back ON because `s_recording` boots true.
- The 12 s probe after the pulse gives the status poll a grace window
  (`gp._misses = -20`) so the workspace does not declare the link dead
  mid-boot.
- Firmware hardening (an out-of-task watchdog on `gnss_task`) remains the
  real fix for the wedge itself — deliberately NOT flashed tonight, the night
  before a car test, on top of the build proven at 17:0x. The Studio-side
  reset makes the failure recoverable in the meantime.

## Verification (all on the real puck, serial 1CDBD4880D18, fw 0.1.0)

- The wedged receiver — quiet for over an hour, `rx_overflow`/`rx_recover`
  both 0, watchdog provably never fired — came back to a **15-sat 3D fix**
  after one reset (first via esptool to prove the recipe, then twice via the
  Setup button). `Messages received` reset 235116 → ~800 → climbing at
  30 msgs/s each time: genuine chip reboots, not reconnects.
- "Node restarted — back in 1.1 s", timed from the button press.
- *Restart RDM Studio* replaced the process (new PID, fresh home screen);
  *Reload Interface* reloaded the page.
- ● REC verified live in the topbar on hardware, and the pause → amber →
  resume → red cycle verified against a fake transport in the browser
  harness, alongside a full synthetic end-to-end: 6,241 samples in the real
  wire format at Winton's library gate → download → 3 laps (1:16.560 /
  1:16.600 / 1:16.560) → saved session → correct `gpNoLapsWhy` diagnosis when
  the course was deliberately placed 256 m off the gate.

## References

- ADR-0015 (the node is part of readiness), ADR-0020 (recording needs nothing
  pressed — the readiness card this builds on)
- `docs/IN_THE_CAR_2026-07-30.md` §"If the GPS goes quiet" (updated remedy)
