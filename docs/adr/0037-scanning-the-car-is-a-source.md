# ADR-0037: Scanning the car is a source, and diagnostics is a different job

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/data/`, `main/net/web_server_obd2.c`,
`main/ui/screens/first_run_wizard.c`, `main/ui/settings/device_settings.c`,
`main/web/index.html`), rdm7-desktop (resynced base, 0.6.1)
Follows 0030–0036. Prompted by the owner: *"we have to take a look at the
OBD2 setup. both in web UI, desktop and on device it's a bit non intuitive
ATM. you have to go into device settings, scan, etc."*

## What was already there, and where it was trapped

The dash has had real OBD2 discovery since 1.1.26 — `obd2_discovery_start()`
walks the Mode 01 supported-PID bitmask chain and auto-searches both common
bitrates and both addressing modes. The first-run wizard already did the
intuitive thing with it: scan in step 2, then fill unbound channels from the
answers in step 3.

Everywhere else exposed the parts instead of the job:

- **Web / desktop Setup** had an "OBD2" card whose four-step prose ended in
  "now go to the Channels editor", plus a raw PID picker.
- **Device Settings** had an "OBD2 Signals" card that opened the same PID
  picker. To get a reading on the glass you had to know what a PID was.
- **The Channels page's "+ Add channels" menu** — the one place that answers
  "where does a channel come from" — listed a pre-defined ECU, a DBC file and
  a custom channel, but not the car itself.

So the capability existed and the one surface that should have carried it
didn't.

## Decision

**Scanning is a source.** `+ Add channels → From OBD2 — scan this car…` runs
the existing discovery and offers the answers as channels, on the web page,
in RDM Studio and on the dash's own editor. It sits beside "From a
pre-defined ECU" and "From a DBC file" because that is the same kind of
thing: a place channels come from.

**Diagnostics is what's left.** The Setup card is now *OBD2 diagnostics* —
protocol status, VIN, ECU name, trouble codes — and says in one line where
readings come from instead of reciting a recipe. The dash's card is now
*OBD2 PIDs*: "Pick exact PIDs by hand. For readings, use Channels." The
by-hand tools stay; they stopped pretending to be the main road.

**One resolver, in the firmware.** A scan answers with PID bytes; turning
those into channels is `CANONICAL_OBD2_MAP` read backwards, cross-referenced
against the live channel set. That translation lives in
`channel_obd2_matches()` (`main/data/channel_source_apply.c`) and nowhere
else:

- `POST /api/obd2/scan` now returns `channels:[{id,label,units,signal_name,
  service,pid,active,bound}]` alongside the raw `pids`.
- `POST /api/obd2/adopt {channels:[ids]}` binds them, so the client sends
  channel ids and never holds a copy of the PID table.
- The dash's own modal calls `channel_obd2_matches()` directly.

This is the same move as `first_run_wizard_apply_ecu` in ADR-0033: export the
firmware's resolution rather than reimplement it in JavaScript. Three
surfaces, one answer.

**Binding is one path too.** `channel_apply_obd2()` activates any channel
that isn't live yet, adds each PID to the polled set, and does **one** layout
read + save + `obd2_start()` for the whole batch. `/api/channels/bind-source`
was rewritten to call it with a single row, deleting its copy of the
add-a-PID-to-the-layout logic.

**Everything is pre-ticked, nothing is automatic.** The list arrives with
every unbound match ticked — the honest common answer is "yes, all of it" —
but nothing is written until Add. Channels the car offers that are already
sourced show as "already set up" and can't be re-picked. The one exception is
the first-run wizard, where the OBD2 step has already asked and the answer
was yes: there the scan applies without a second tap and the modal reports
what it did.

## The probe this replaced

The wizard's gap-fill used to work the other way round: take the channels
that are active-but-unbound, enable the PIDs they *would* need, poll for
3.2 s, and keep whichever answered. That guessed instead of asking, and it
could only ever bind channels that were already in the set — a fresh dash
with nothing set up got nothing. Discovery is the car's own list and covers
both cases, so `_obd2_compute_gaps` / `_obd2_begin_probe` / `_obd2_probe_tick`
and the save-and-unwind PID snapshot are gone. The chip they hid behind is
gone too: it only appeared when there were gaps, so the one control that
could answer "what can this car give me" was invisible to anyone whose
channels happened to all be bound. It is now always offered, reading
**Scan for OBD2**.

## The leak this exposed: PIDs went in and never came out

Binding a channel adds a PID to the polled set. Nothing ever removed one. At
one channel at a time that was a slow leak; a scan that adopts a dozen at
once makes `OBD2_MAX_ENABLED` (48) a real ceiling, and the failure mode is a
mystifying "OBD2 PID limit reached" much later.

`channel_obd2_prune()` gives them back, called after unbind and after
removal, on both the web and device paths. It only drops PIDs that map to a
canonical channel and that no active channel is bound to — a PID enabled by
hand through the PID picker has no channel and nothing here can tell it isn't
still wanted, so it is left alone.

It ends with `obd2_start(keep, kept)` even when `kept` is zero, which looks
odd and isn't: `obd2_stop()` deliberately leaves `s_poll[]` populated so UI
can query it after a stop, so stopping alone would leave `/api/obd2/pids`
reporting PIDs nothing polls. `obd2_start()` zeroes the count first. That
discrepancy is exactly what the bench test caught — the layout had been saved
empty while the status endpoint still listed the old PIDs.

## Two things the scan is honest about

- **A name collision prefers the broadcast.** `obd2_start()` skips a PID
  whose signal name is already registered against a real CAN broadcast — the
  native preset owns it, and polling too would let the response overwrite the
  broadcast value. So adopting `manifold_pressure` on a car whose ECU already
  broadcasts `MAP` binds the channel to the broadcast and doesn't poll. That
  is the right outcome, and the offer list never invites it: such channels
  come back `bound: true` and render as "already set up".
- **The empty answer names its causes.** No response gets ignition /
  wiring / pre-2008 K-line / "your ECU may already be on the bus under its
  own preset", not a spinner that stops.

## Verification

Firmware on the bench dash (`RDM-DCB4-D926`, 192.168.4.69), 148 URI handlers
registered, 0 failures:

- `POST /api/obd2/scan` with no car: `{ok:true, completed:false, count:0,
  pids:[], channels:[]}` in 2.4 s — an honest empty answer, not a hang.
- `POST /api/obd2/adopt {["engine_load","fuel_level"]}` → `{ok:true,bound:2}`;
  both channels bound, both PIDs added to the polled set and persisted.
- Unbinding `engine_load` dropped `0x04` and **kept** `0x2F`; unbinding the
  last one emptied the list. Bogus id → 404 with a sentence.
- The dash's own editor, driven over remote touch: the header chip reads
  **Scan for OBD2** and is present with every channel bound; tapping it opens
  "Add channels from OBD2" with the spinner and "Asking the car what it can
  report…", then resolves to "The car didn't answer" with Rescan / Done.
- Dash restored from its pre-test backup: 32 channels, zero differences, no
  stray PIDs.

Web page against the mock (the dev server now serves the page's own baked
catalogue, so browse-the-catalogue is testable in browser dev): scan → "Your
car answered. 9 readings can be added", 2 already-set-up rows disabled, Add
9 → 9 channels bound, list refreshed, 0 console errors. Against the live
dash: 32 rows, 0 blank Source cells, 15 "Built in", 6 "Not set up". RDM Studio
build carries the modal, the menu entry and the reframed card.

## Related: the Source column got quieter

Same pass, same page. Five coloured provenance pills became two: a CAN pill
next to a frame id said nothing, and an RDM-7 pill labelled something the
user never chose and cannot change. Now only OBD2 and calculated channels
wear a tag; CAN prints its frame id, built-ins say "Built in", unbound says
"Not set up". ADR-0035's rule stands — never blank — and the frame id now
falls back to the signal registry for channels whose decode still lives in
the layout rather than on the channel.

The three view chips (All / Configured / Available) went with them. The list
is simply "the channels this car has"; search still reaches the catalogue and
spills matches under their own heading, `+ Add channels` brings them in, and
the two things left — tick-several-to-remove and browse-the-standard-list —
moved into a `⋯` overflow. Browsing shows a sticky bar naming where you are
and offering the way back, because a hundred rows of catalogue with no
explanation reads as a bug.
