# ADR-0023: Recording is a choice — power-on default, and a lamp that holds still

Date: 2026-07-31
Status: Accepted
Repos: rdm-gps-node (`record_on_boot` + RTC armed-state), rdm7-desktop (Setup toggle, copy, de-flashed REC)

## Context

ADR-0020 made "the node records from power-on, nothing to press" the whole
recording story, and for a track car it is the right one. Then the owner said
the car is also a daily: the ring holds ~6 hours at the plain record and then
overwrites its oldest, so a puck that logs every commute eats its own
weekend. For that car the right story is the opposite — arm recording
yourself for the drive that matters ("helps save space").

Two behaviours, one flag, no way to choose. And the REC lamp pulsed, which
the owner read as unprofessional — an alert, not an instrument.

## Decision

**`record_on_boot`, persisted in NVS, default on.** On is ADR-0020 unchanged.
Off makes the REC button (Studio topbar, or the dash over
`RDM_GPS_CMD_RECORD`) the thing that starts a recording. Exposed as
Setup → Recording → "Start at power-on", written through `node.config.set`,
and explained in owner's terms: track car / daily, and what the ring does
when it fills.

**The live armed-state survives soft resets in RTC memory; power-on takes
the setting.** This is the piece that makes the two features safe together:
ADR-0022's sentinel restarts the chip mid-session, and before this change a
restart forced the armed-state back to the boot default — which would
silently STOP a daily's manually-armed session (setting off), or silently
START logging on a daily parked at the airport (setting on… wrong the other
way). Now: sentinel/`REBOOT`/serial reset keep the driver's last choice;
pulling the plug is the reset that consults the setting. Changing the
setting deliberately does not touch the live state — enforced on the node —
so configuring at a desk cannot stop a recording in progress.

**The lamp holds still.** Solid red ● REC recording, grey ○ REC stopped.
The pulse died: real recorders show a steady lamp, and a UI element that
blinks forever is claiming an urgency it does not have.

Copy went setting-aware everywhere it used to promise "a power cycle turns
it back on" — that sentence is only true with the setting on, and a daily's
readiness card must not hand out a remedy that stopped being one.

## Consequences

- ADR-0022's "boot default stays ON" is superseded in detail: power-on
  follows `record_on_boot`; soft resets follow the driver.
- Old firmware ignores `record_on_boot` in `node.config.set` (rejects with
  "nothing to change"); Studio shows the error rather than pretending.
- The wire protocol is unchanged — `RECORD` (0x08) and `GpsFlags.RECORDING`
  work identically in both modes, so the dash button needs no awareness of
  the setting.
- The device was left with the setting **ON** for the imminent car test
  (matching every doc written before today); flipping it off is one toggle
  when the car goes back to commuting.

## Verification (real puck, flashed 2026-07-31 ~05:50)

- Setting round-trips through Setup: Off → "Saved — from the next power-up,
  recording waits for the REC button", toggle state follows the node's
  reply, live recording untouched.
- Retention, both directions, through real chip reboots (Setup → Restart
  the node): recording OFF → reboot → still off (the old firmware would
  have relit it); recording ON → reboot → still on (a sentinel trip cannot
  drop a session).
- REC button on the new firmware: grey ○ stopped / solid red ● recording,
  no animation.
- Power-on path (RTC magic invalid → setting consulted) is by-construction;
  it needs a physical replug to demonstrate and was not bench-verified.

## References

- ADR-0020 (nothing pressed — now the track-car mode), ADR-0022 (the
  sentinel and the CAN command this makes mode-safe)
- `rdm-gps-node/docs/PROTOCOL.md` §0x40E `RECORD`
