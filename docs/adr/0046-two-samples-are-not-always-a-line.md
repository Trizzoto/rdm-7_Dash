# ADR-0046: Two samples are not always a line

Date: 2026-08-22
Status: Accepted
Repos: rdm7-desktop (GPS workspace)
Rests on [0008](0008-gps-lap-timing-integration.md) — the puck logs, Studio interprets.
Evidence: `rdm7-desktop/docs/IN_THE_CAR_2026-08-22.md` (the drive to Mallala).

## Context

Every reader of a recording — the map, the gate scan, the smoother, the drift
engine — assumes that two consecutive samples are two readings of one
continuous line. Nothing had ever checked that assumption, and on the 22 August
drive to Mallala it is false six times in 168 105 samples.

Two different things make it false, and they need telling apart.

**The receiver lost its fix mid-sector.** `trace_log_sample()` skips a sample
with no fix but leaves the page open, and `trace_log_read()` dates every sample
in a page as `base_itow_ms + slot * 40`. So thirteen seconds of missing road
came back stamped as one 40 ms step covering 444 m, at 106 km/h, with the
cadence perfect and nothing anywhere disagreeing. Lap times are interpolated
off those stamps: **a lap containing one of these reads about thirteen seconds
faster than it was driven.**

**The flash writer stalled.** Four holes of 1.7–6.1 s where the clock is
honest — the elapsed time really did pass — and only the samples are missing.
The time can be trusted; the road cannot.

Both drew as a straight bar across the map, and the first also produced a lap
time that was simply wrong, silently, in the direction a driver most wants to
believe.

## Decision

**A pair of samples that no driving connects is marked as such, once, at
ingest, and every reader honours the mark.**

1. **The test is the recording's own speed channel.** Speed comes from Doppler
   and is the one number here that does not depend on position, so a step
   further than the reported speed can carry in the stamped time is not a step.
   A floor of 12 m absorbs standstill jitter; a parked car never breaks its own
   trace.

2. **Two marks, because there are two faults.** `brk` — no line joins these
   two. `brkTime` — and the clock across it is wrong too. Only the second can
   invent a lap time, and only the second flags a run.

3. **Marked at ingest, on the samples themselves.** `gpMarkBreaks` runs inside
   `gpComputeG`, which every path producing samples already calls — download,
   open, import, live. A reader cannot be handed a sample set whose breaks have
   not been found.

4. **Nothing is deleted, moved, or interpolated.** The samples are all still
   there and all still shown. What changes is only what is claimed *between*
   them, which was never measured.

5. **A run containing a falsified clock is flagged `jump`,** greyed, kept on the
   board, unable to be the reference and unable to be a personal best. The
   existing `gap` flag stays for holes the clock knows about — those cost the
   run the part it did not measure, which is a smaller and different claim.

6. **A break is a wall in the smoothing axis, not just a flag on a sample.**
   `gpSmoothAxis` integrates speed, so 444 m of ground costs 40 ms of axis: the
   fitting window spanned the jump and fitted a quadratic through samples half a
   kilometre apart, putting *smoothed* points where the car had never been, in a
   staircase of 200 m steps that were not themselves breaks and so were drawn.
   Found in the running app after the sample-level marking was already correct.

7. **A recording the test does not describe keeps its straight lines.** Past 2%
   of samples breaking, the marks are dropped wholesale: that is an import with
   no usable speed channel, not a recording with holes, and cutting it into
   confetti would be worse than leaving it.

## Consequences

- The map draws no line where there is no line. Measured on the fixture: the
  longest stroke fell from 219 m to 2.2 m, identical to the same circuit with
  no hole in it.
- A gate cannot be crossed by a jump, so a teleport across the start/finish
  line no longer times a lap. This is the position-side half of the rule
  `lap_core.c` already applies to time (`LAP_MAX_FIX_GAP_MS`).
- A track's permanent shape is never taken off a flagged lap, and an outline
  spanning a break is refused rather than saved with a bar across the infield.
- Six breaks in 168 105 real samples, 95 ms to find them all. Quiet enough that
  a mark means something.
- This is a *reader-side* correction of a *recorder-side* fault. The node fix —
  close the page when a sample is skipped while moving, so the next fix opens a
  sector carrying its own `base_itow_ms` — is still the right fix and is still
  worth making. Studio must keep this mark either way: recordings already
  downloaded carry the fault forever, and the honest reading of them is that
  the road and the time between those two samples are unknown.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` — `gpMarkBreaks`, `gpRunBreakM`,
  `gpSmoothAxis`, `gpGateHits`, `gpGradeRuns`, `GpTraceLayer._strand`
- Tests: `rdm7-desktop/tools/check_breaks.js` — 46 checks, including the real
  22 Aug ring sample by sample
- Related ADRs: 0008, 0044, 0045
