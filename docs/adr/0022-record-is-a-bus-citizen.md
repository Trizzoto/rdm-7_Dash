# ADR-0022: Record is a bus citizen, and the wedge cannot end a day

Date: 2026-07-31
Status: Accepted
Repos: rdm-gps-node (`d270059`), rdm7-desktop (Studio button); RDM-7_Dash owes the dash-side button

## Context

Two things met on the same night.

The user asked for a record start/stop button "in the top" of Studio, "red
and grey", and said the same control "will be controllable from the dash also
to start and stop recording (helps save space)". Recording control therefore
has to be a *protocol* feature, not a Studio feature — Studio's button is one
client of it.

And the GNSS wedge recurred: the receiver stream died again hours after
ADR-0021's revival, with the same signature — `rx_overflow` 0, `rx_recover`
0. Zero recoveries is the tell: the in-task silence watchdog lives inside
`gnss_task`, so whatever wedges the task also wedges its own rescue. A puck
in a car with no laptop would silently log nothing for the rest of the day.
The manual remedy (Setup → Restart the node) requires the laptop that the
whole point of the puck is to not need.

## Decision

**`RECORD` (0x08) joins the 0x40E command frame.** B1 = 1 starts the trace
logger, 0 stops; other values are rejected (a corrupted byte must not stop a
session). Immediate, unstaged, **unpersisted** — the boot default stays ON,
so the worst forgetting can do is record too much, never too little.
Recording state is broadcast as bit 6 of every status frame's `GpsFlags`, so
a dash button displays the state it commands instead of guessing.

**Studio's topbar chip became that button.** Red pulsing ● REC = recording,
press to stop; grey still ○ REC = stopped, press to start. No confirm
dialog — it flips a flag, destroys nothing, and its own colour is the
receipt. The chip only re-renders on state change, because a CSS pulse
restarted every poll tick never gets past its first frames.

**A sentinel that does not live in the task it watches.** A separate 2 KB
task restarts the chip after 90 s without a well-formed receiver message —
six times the in-task threshold, so the existing recovery gets every chance
first. The ring resumes across the reboot (the write head is recovered from
sector headers), the track is in NVS, and logging returns with the boot
default. Capped at three trips per power-cycle in RTC memory: a genuinely
dead receiver gets three honest attempts, then the node stays up as a
CAN/IMU device rather than flapping off the bus every ninety seconds. The
count clears when messages resume, is reported as `sentinel_reboots` in
`gps.status`, and renders as the **Self-restarts** row on Studio's receiver
card — worth amber, never red, because nonzero means the failure was
survived.

## What RDM-7_Dash owes

The dash side of the record button: a control bound to `RDM_GPS_COMMAND`
(`base+0xE`, cmd `0x08`) for the press and to `GpsFlags.RECORDING` for the
lamp. Auto-bind already identifies the puck's block from its status frame,
so the binding story is the same as the Position & GPS channels. Until that
ships, `POST /api/can/inject` can exercise the command end-to-end once dash
and puck share a bus.

## Verification

- Firmware builds under ESP-IDF 5.3.1; flashed to the bench puck 01:58 and
  smoke-tested: boot, 15-sat reacquire, `sentinel_reboots: 0` flowing
  through `gps.status` into the new Studio row.
- Studio's button toggled against the new firmware over USB: grey ○ stopped,
  red ● recording, round-tripped through `trace.record` + `trace.info` each
  press.
- NOT yet verified: the CAN path itself (no shared bus on the bench — the
  dash meets the puck's bus in the car), and a live sentinel trip (needs the
  wedge to recur, which the bench does within about an hour; the next
  recurrence should appear as Self-restarts = 1 instead of a dead evening).

## References

- ADR-0020, ADR-0021 (the readiness card and the restart layers this extends)
- `rdm-gps-node/docs/PROTOCOL.md` §0x40E / `GpsFlags` (the wire contract)
