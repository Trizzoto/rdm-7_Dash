# ADR-0044: A recording carries its own meaning

Date: 2026-08-20
Status: Accepted
Repos: rdm7-desktop (GPS workspace), rdm-gps-node (wire format — unchanged, described)
Builds on [0008](0008-gps-lap-timing-integration.md): the puck stays dumb, and this is
the other half of the bargain.

## Context

ADR-0008 decided the GPS puck logs **raw CAN field counts** — one 16-bit slot
per channel per sample — and does not carry names, units or scaling. That was
and is right: the node has no business holding a copy of the car's channel
library, the record stays small, and the same log is readable whatever the
library later says.

The consequence was never written down, and it turned out nobody had built it:
**something else has to hold the meaning, and it has to be somewhere the
recording can reach.**

It was not. The library — name, unit, decimals, scale, offset, signedness —
was fetched over HTTP from a connected dash into `gp.dashChans`, a field on an
in-memory object, by exactly one caller: the **Refresh button** on Setup ▸
Channels. Not on opening the view. Not on connecting a dash. Not on
downloading. Unless you had pressed that button, this session, on that card,
Studio held no definitions at all — and it never said so.

What that looks like from the driver's seat, reported 2026-08-20:

> the canbus in the graph is all wrong like throttle position 818 and -74 is
> that right? shouldnt it be between 0 and 100. same with ignition timing

The Mount Barker recording of 16 Aug logged twelve channels. Every step after
that was correct and the result was nonsense:

- `chanIds` = `["rpm","throttle_position", … ,"ignition_timing", …]` — real ids
- nothing resolved them, so scale 1, offset 0, no unit, and the raw id as the
  lane label
- the lane plotted the counts, 0..744
- `gpLaneScale` padded the axis by 10% either side → **−74 .. 818**

The −74 was never a sample. It was breathing room under a column that peaked
at 744. Reading it as a signedness bug would have been the wrong hunt on that
channel — though there *is* one, one layer down, on the channels that really
are signed.

**The signed read.** `trace_log.c` sign-extends an extracted field and keeps
the low sixteen bits (`s_chan_val[i] = (uint16_t)raw`). Studio read every slot
with `getUint16` and never put the sign back. Ignition advance at −6° came
back as 65530; a right-hand yaw rate fed to the drift engine came back as
about +1300 °/s — the identical failure the puck's *own* gyro column had had
and had fixed with `getInt16`, in the one place where getting it wrong quietly
ruins a whole drift analysis rather than looking obviously broken.

**And the unit.** The dash emits `label` and `units_native`; Studio looked for
`name` and `unit`. So every dash channel that *did* resolve still arrived at
the rack unitless.

## Decision

**A recording is self-describing, and the library is written down.**

1. **Snapshot at download.** When a trace comes off the puck, every id that
   the library can describe is resolved and frozen into the recording as
   `chanDefs` — id, name, unit, decimals, scale, offset, signedness — and
   saved with the session. An imported VBO has always carried these; a puck
   download now does too. Ids that are *not* described are deliberately **not**
   frozen, so they still resolve later when the dash is next plugged in.

2. **Cache the library.** The dash's answer is kept in `localStorage`
   (`rdm7_gp_dashchans`, with a date). A live dash always wins; the cache is a
   fallback, never a source of truth, and a dash that fails to answer never
   erases it. Reading a recording must not require the car.

3. **Fetch it without being asked.** `gpChanAutoRead()` reads the dash once per
   session whenever a screen needs the library and a dash is reachable —
   including at the top of a download, which is the last moment the meaning
   can be captured before the columns are frozen.

4. **One resolver, one reader.** `gpChanDef(id, entry)` normalises every source
   into one shape and `gpChanValue(raw, def)` is the only thing that turns a
   slot into a number: two's complement out where the field is signed, `0xFFFF`
   (`TRACE_CHAN_STALE`) as *no reading*, then scale and offset. The rack, the
   drift engine, the CSV and the VBO exporter all go through it. They used to
   each pick the pieces they wanted out of a library entry, which is how the
   unit went missing on one and the sign went missing on all of them.

5. **Say when there is no decode.** A column nothing describes is labelled
   `counts` and tagged **not decoded** on the lane, in the alert colour, beside
   the numbers it is about. This is the part that matters most: an undecoded
   column plots a perfectly smooth, perfectly plausible line, and nothing on
   the screen disagreed with it.

## Consequences

- A download taken at the track is readable at the kitchen table, and readable
  in five years, on a PC that has never seen the car.
- Recordings taken **before** this change carry no `chanDefs`. They resolve
  through the cached library instead, which fills in the first time a dash is
  connected — so his existing sessions repair themselves rather than needing a
  migration.
- A signed −1 is indistinguishable from `TRACE_CHAN_STALE` on the wire. "No
  reading" wins: a stale slot drawn as −1 puts a spike through the middle of a
  lane, and one count of a signed channel is not worth defending. Stated here
  rather than hidden, because it is a property of the wire format, not a bug.
- If the car is rewired and the dash's decode changes, an old recording keeps
  the definitions it was downloaded with. That is the point — it is what it was
  logged as — and the Channels card says how old the cached library is so the
  two can be told apart.
- `tools/check_candecode.js` covers the resolver and the arithmetic, including
  a round trip of every signed field width 2..16 against the node's own
  `(uint16_t)` truncation, and reproduces the reported −74..818 axis from raw
  counts before showing it land inside 0..100 once decoded.
