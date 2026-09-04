# ADR-0055: A device on the wrong bit rate is invisible, not broken

Date: 2026-09-02
Status: Accepted — built and shipped 2026-09-02
Repos: RDM-7_Dash (`main/net/web_server_can.c`, `web_server_system.c`),
rdm7-desktop (keypad workspace `kpw*`, `tools/check_keypad.js`)
Bench log: `rdm7-desktop/docs/BLINK_MARINE_PKP2200_CANOPEN_2026-08-28.md`

## Context

Studio could design a keypad configuration and export it as a file of CAN
frames. Sending that file was the customer's problem: buy a USB-CAN dongle,
open SavvyCAN, send the frames at the right speed. The RDM-7 dash was already
wired into the same bus and already served `/api/can/monitor` to the desktop
CAN analyzer, so the missing half was obvious enough to be worth writing down
why it took an afternoon on the bench to see properly.

Setting up one PKP-2200-SI by hand, with a dash and a scripted HTTP client and
both manufacturer manuals open, took most of a day. Almost none of that was
spent on the frames. The frames were right the first time. It was spent on
three failures that all present identically — **nothing on the bus** — and are
all indistinguishable from a dead keypad:

1. **`POST /api/can/config` did not apply the bitrate.** It wrote the value to
   NVS, returned `{"status":"ok"}`, and left the TWAI driver on the old rate.
   `can_change_bitrate()` had existed the whole time; the on-device settings
   screen and the OBD2 scan both called it. The HTTP endpoint did not. So every
   "now try 250k" was another try at 500k, and the keypad's silence was read as
   evidence about the keypad.

2. **Overlapping SDO requests are silently dropped.** Not rejected, not
   aborted — dropped, with the server staying deaf until traffic settles. Every
   write looked ignored, including a deliberate 30-request burst sent to prove
   the writes were being ignored. One write, spaced by seconds, was acked on
   the first attempt and every attempt after. Reads had "mostly worked" only
   because HTTP round-trip timing happened to space them out by luck.

3. **Switching protocol resets the device to the destination protocol's own
   factory defaults, bit rate included.** CANopen defaults to 125k, J1939 to
   250k, the dash to 500k. Mid-session MaxxECU's own keypad panel sent the
   documented CANopen→J1939 switch, and the keypad vanished from every CANopen
   address at every rate that had ever been tried. Hours went into "is it dead,
   is a wire off" before the answer turned out to be a third bit rate nobody
   had looked at.

The reading to resist is that these are three unrelated bugs and the fix is to
fix them. They are one thing wearing three hats: **on a CAN bus, wrong-speed,
unplugged and dead are the same observation.** Any tool that asks a human to
interpret silence will cost that human the same afternoon.

## Decision

**The dash is the CAN interface, and the tool that drives it must earn every
claim it makes from evidence rather than from what it just sent.**

Concretely:

- The dash gets a permanent, guarded write side (`web_server_can.c`): raw
  transmit, tracker reset, promiscuous toggle. Not in `web_server_test.c`,
  whose stated contract is read-mostly and inject-only, and which a raw
  transmitter onto a live vehicle bus plainly violates.
- `POST /api/can/config` applies the rate live, and takes `persist: false` so
  a caller can walk the rates and put the dash back. `GET` reports saved and
  live separately, because during a probe they legitimately differ.
- Studio gets a wizard that hunts across bit rates and both protocols, writes
  one request at a time, and **re-finds the device after anything that could
  have moved it** rather than assuming where it went.

Three consequences of that last point are worth naming, because each one is a
place where the cheap implementation is wrong and passes anyway:

**Freshness is decided by the tracker's `rx_count`, not by age or payload.**
The per-ID tracker keeps the last frame per ID indefinitely. After a rate hop
it is full of entries that are real, well-formed and completely stale — a
genuine SDO reply from a rate we are no longer on. Age does not separate them
either: a device answering fast and an entry that just missed a reset look the
same. A count that went up is the only statement that means "since I sent,
something arrived". The tracker is cleared between probes as well, so the
question is usually "is there an entry" rather than "is this entry stale".

**A protocol switch is followed by a full re-hunt, not a hop to a guessed
rate.** The switch is unacknowledged in both directions — there is no reply
defined — so the only proof it worked is finding the device again.

**Pacing is not tuning.** One request at a time, wait for the reply, then wait
longer, is a correctness requirement, not a performance trade-off. It is why
the wizard takes ten seconds where it looks like it should take one.

## Consequences

The dash can now put arbitrary frames on a customer's vehicle bus over HTTP.
That is a real capability increase and it is guarded accordingly: the RDM
device-bus block and discovery ID are refused (a stray frame there is a valid
message to our own protocol handler, which will act on it), OBD2 request IDs
are refused (they interleave with the dash's own transactions), and the rate is
capped at 24 frames a second so a caller that has lost its wait loop cannot
turn into sustained traffic. Each refusal returns a sentence saying which range
it hit and why, and Studio shows it verbatim.

`max_uri_handlers` went 160 → 192. The three new endpoints took the old cap to
seven slots spare, and ESP-IDF drops handlers past the cap **silently** — the
failure mode is a POST that returns 405 from the wildcard CORS preflight, which
is not obviously about registration at all.

The exported setup file did not go away; someone with a dongle and no dash
still needs it. Both paths now come out of one `kpProvisionSteps()`, which
fixed a real bug in the file: the bit-rate frame was sent **first**, which is
only safe if the keypad defers it to a power-cycle — and this one does not, so
every subsequent frame went out at the wrong speed.

The hard part of testing this is that the interesting behaviour only appears
when the device is somewhere you did not expect. `tools/check_keypad.js` runs
the shipped code, extracted rather than copied, against a simulated dash and
keypad on a virtual clock — deliberately meaner than the real unit, and set up
to reproduce each of the three traps above on purpose. It found three genuine
bugs before this shipped, all in the same family: Apply left the dash on
whatever rate its last probe used, Apply sent its first frame at the dash's
normal rate instead of the keypad's, and so the J1939→CANopen round trip never
completed. All three would have looked fine in any test where the keypad
happened to already be where the code assumed.

**Not verified against the physical keypad.** The dash was off the network for
the whole of this work. The firmware compiles clean and the wizard is tested
against the simulation, but the last mile — flash the dash, press the button,
watch a real keypad move — has not been walked.
