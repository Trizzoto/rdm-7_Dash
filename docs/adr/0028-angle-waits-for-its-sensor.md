# ADR-0028 — Angle waits for its sensor

Accepted 2026-08-10. Studio, GPS workspace, Drift view.
Plan: `../../../rdm7-desktop/docs/DRIFT_MODE_PLAN_2026-08.md`.
Harness: `rdm7-desktop/tools/check_drift.js`.

## Context

Formula Drift adopted Race Data Labs' Wally for 2026: roughly 80% of a PRO
qualifying score — line and angle — is now generated from telemetry, and only
style is left to the three judges. D1GP has scored speed, angle and angle
stability objectively for years with DOSS. The appetite for a machine that
reads a drift run is proven, and so is what happens when the machine's formula
is unpublished and contested.

The RDM GPS puck is a single-antenna receiver. It measures where the car
**travelled**. Drift angle is where the car **pointed** minus where it
travelled — and a car circling at 40° of slip lays down the identical GPS
trace as one gripping round the same arc. The second quantity is not merely
noisy in the recording; it is absent from it.

The puck does carry a 6-axis IMU whose gyro Z is calibrated vehicle yaw rate,
phase-locked to the fixes and broadcast on CAN. The flash recorder does not
store it (`trace_sample_t` is 12 bytes), and the node cannot sniff its own
broadcasts (`TWAI_MODE_NORMAL`), so today that sensor reaches Studio only
live — never in a recording.

## Decision

**Drifting is a seventh view, and angle appears only when a sensor backed it.**

1. **The unit is the run, not the lap.** A run is a burst of driving bracketed
   by gaps — which arrives free, because the node records nothing under
   8 km/h — or the stretch between an entry line and an end line when a course
   is drawn. Runs are `{from,to}` spans shaped exactly like laps, so playback,
   the scrubber, ghosts, framing and the dock work unchanged.

2. **No angle from position alone. Ever.** With no yaw or slip channel in the
   recording the Angle lane, tiles and map mode are honest empty frames
   carrying a plain statement of what has to happen first (the ADR-0012
   pattern). Line, speed, cornering load and switches are all measured path
   geometry and work on the plain 12-byte trace.

3. **One angle engine, any yaw source.** A yaw-rate channel (°/s) is
   integrated; a slip-angle channel (°) is passed through and attributed to
   the instrument that measured it. The source is chosen by UNIT, never by
   name — a channel called "Angle" logged in radians would be read as degrees
   and every number after it wrong by a factor of 57, with nothing on screen
   looking odd. The user can override the pick; "none" is one of the options.
   The same engine therefore serves an imported drift-box log today, a car's
   CAN yaw channel when trace v2 is bench-proven, and the puck's own gyro when
   the recorder stores it — with no analysis code changed at any tier.

4. **Grip driving is the calibration laboratory.** A gyro's sensitivity is
   good to about a percent, and a percent of 50 °/s held for ten seconds is
   five degrees — more error than every other source combined. Whenever the
   car grips, the body must be turning at the course rate, and a session holds
   minutes of that. Two passes fit scale and bias against those samples; the
   second pass judges "gripping" on the corrected difference so the mask is
   not contaminated by the drifts themselves. A fit more than a quarter off is
   REFUSED rather than applied — past that the channel is not what we think it
   is — and the recording is flagged as taken at face value.

   **The fit regresses the noisy channel on the clean one, not the reverse.**
   Least squares assumes the regressor is exact, and the regressor here is the
   noisiest thing in the room — course over ground differentiated over a
   quarter of a second — while the gyro is a rate sensor already averaged over
   the same window. Noise in the regressor costs bias; noise in the response
   costs only precision. Fitting rb on rp returned 0.964 for a gyro that was
   truly 1.008: a 4.5% error, five times the effect the fit exists to remove,
   which put a 38° corner on screen as 41°. Fit rp on rb and invert. (A lagged
   instrument also works in principle and was tried; it needs the driving to
   still correlate with itself past the smoothing window, which fast circuit
   corners do not, and it quietly fell back to the biased estimate exactly
   where it was needed.)

   **And a steady angle is not a gripping car.** A car HOLDING 40° has a
   beta-dot of zero, so it passes any "the rates agree" mask carrying the
   largest turn rates in the session — and then supplies nearly all the fit's
   leverage, biased. The second pass therefore carries a leaky-integrator
   estimate of the angle itself and excludes anything sideways. On a
   drift-only recording that leaves nothing to fit, and the honest answer is
   to take the channel at face value and say the fit was weak — which is what
   happens.

5. **Every channel is resolved through one shared table.** What a channel id
   MEANS — name, unit, decode — comes from the channel library first and the
   recording's own definitions second. The Drift view originally read only the
   second, so every puck-recorded channel arrived unnamed, unitless and
   undecoded: a GT86's yaw rate reached the angle engine as raw CAN counts
   while the graph rack one panel away drew the same channel correctly scaled.
   Two resolvers cannot be kept in step by hoping, so there is now one
   (`gpChanDefsById`).

6. **An anchor is a car going STRAIGHT, not a car whose angle is steady.**
   This is the one that matters. A car holding 40° through a long corner has a
   perfectly constant angle, so the rate difference is zero there too;
   anchoring on that zeroes the angle in the middle of the drift being
   measured. The harness caught it reading a 38° corner as 29° and splitting
   one drift into three. The honest anchor requires the path not turning, the
   body not turning, and no lateral load — a car cannot hold a big slip angle
   along a straight path on tarmac.

7. **The error bar is measured, not quoted.** Every leg between anchors ends
   back in grip, where the angle must return to zero. Whatever it returns to
   instead IS that leg's accumulated error, directly observed: redistribute it
   across the leg the way a surveyor closes a traverse, and put half of it on
   screen. Legs past 8° of misclosure are greyed out rather than hidden — "we
   tried and it did not close" is information. Open ends (before the first
   anchor, after the last) grow their error bar with distance from the anchor
   rather than carrying a flat worst case.

8. **A hole in the recording is a hard break.** A gap means the car stopped,
   so no angle carries across it — and because the course rate is a central
   difference, a seam poisons several samples EITHER SIDE with a turn that
   never happened. The rate is blanked across that window and anchored just
   outside it. Without this, a run beside a 95 s gap claimed ±16° when its own
   driving was worth ±1.

9. **Scored shapes, and a denominator that shrinks.** A course carries clips
   (a point the car should reach) and zones (a line it should ride, with a
   band) — two shapes, both provable from a single moving point, no polygons.
   Clips score on closest approach, zones on the fraction of their length
   covered. **With no angle source the total a run is scored OUT OF falls**;
   it is never renormalised, so a card with no angle on it cannot look like a
   full one. Every card is stamped with a scoring version, so tuning the
   formula never silently restates a score a run already earned. Style is
   never machine-scored.

10. **The editor refuses shapes finer than the receiver.** Nothing under 2 m.
   Without corrections the M9N is metre-class, and an editor that accepted a
   30 cm clip would promise a precision it has not got, with every score after
   it inheriting the promise.

11. **Switching runs holds your place** (ADR-0027's law, generalised):
    `gpSameSpot` now works on spans, so clicking another run keeps the map and
    the position on the corner you were reading.

## Consequences

- A drifter with only the puck gets runs, line overlay, entry/lowest/exit
  speeds, switches with what each cost, ghost replay and clip/zone scoring on
  day one — and an Angle column that says why it is empty.
- A drifter who owns a drift box gets the full card by dragging in the file
  they already have; VBO channels already flow to lanes with no import change.
- The naming repair ships with any yaw source: the existing "Yaw rate" lane is
  the rate the PATH turns at, so it becomes "Turning (path)" the moment a body
  rate exists. Letting one keep the bare word would let it impersonate the
  other, and the difference between them is the entire signal.
- Course authoring lives in the Drift view, not Tracks (the plan said Tracks):
  you draw a clip and immediately see it scored against the run you just did.
  Tracks stays about gates and layouts.
- `courses:[]` rides on the track record beside `outline`, normalised on load
  like everything else there, and is never sent to the node.
- This is a **practice instrument**, and says so. Wally judges with claimed
  centimetre positioning; this is metre-class. The moment it claims to be a
  judge, ADR-0011 is broken in spirit even when every number is real.

## Addendum, 2026-08-10 — the puck now logs its own gyro

The Context above said the recorder throws the gyro away. It no longer does.
`trace_sample_t` grew from 12 to 14 bytes for `int16_t gyro_z_2cdps`, sector
magic RDMV → RDMW, one SPI burst shared by the record and the CAN frame so
both carry the same instant. Nothing is released, so there was no installed
base to migrate and no reason to prefer the cheaper channel-slot hack over
the clean field.

**Stored at 0.02 °/s per LSB, deliberately unlike the CAN frame.** An int16 of
centi-deg/s stops at ±327.67 °/s; a hard drift transition measured 339 on the
Mallala fixture, and clipping there corrupts the angle at the exact instant
the switch happens. Two bytes either way; one of them covers the sensor's full
±500 dps.

**Every reader asks the node for the record width.** `trace.read` declares
`fix_bytes`; Studio falls back to `record_bytes` minus the channel tail, then
to 12 for an older node, and refuses a page whose length disagrees rather than
decoding it into plausible nonsense.

The angle engine did not change. The gyro joins the candidate sources as a
virtual entry ranked LAST, so a recording that already had a car-bus yaw
channel keeps choosing exactly what it chose before.

## Addendum, 2026-08-10 — one idea taken from BMW

BMW's M Drift Analyser is the only factory system that scores a drift at all.
Its five-star rating is, in BMW's own published words, based on "duration,
length and average angle of the drift **and some magic dust**" — and the star
ceiling is capped by the traction-control level, so a scruffy assist-off drift
outscores a tidy assisted one. Two published data points cannot be reconciled
without the weights: 3.5 stars for 4.1 s / 111 yd / 16.4°, and 4 stars for
56 yd / 28.1°. Nobody has ever validated its angle against a VBOX. That is the
same failure as Wally, and it is not copied.

The one thing BMW **does** define precisely is worth having: *"the drift
performance is the Schwimmwinkelbetragsintegral over all drifts"* — the
time-integral of |sideslip angle|, in degree-seconds. It is now shown per run
and it replaces the angle term in the scorecard, which previously had three
invented constants in it. The integral has none: holding more angle and
holding it longer both raise it, and nobody had to decide the exchange rate
between them.

## What was deliberately not built

- **A composite drift score.** UDSM's and DOSS's formulas are unpublished, the
  community fight over Wally shows how a contested single number lands, and no
  sensor backs a weighting. The facts table is the product.
- **Tandem proximity.** Two cars on a common clock is free (GPS time of week),
  but measured car-to-car distance needs RTK-class hardware.
- **Angle estimated from position alone.** Named here so nobody adds it later
  thinking it was an oversight. It cannot be done, and the empty frame is the
  correct output.

## Verification

`node tools/check_drift.js` — 67 checks against a drive whose true slip angle
is known by construction, with a gyro corrupted the way a real one is (scale
error, zero offset, noise, and a zero that wanders with temperature) **and a
receiver whose heading carries noise**. That last one matters: an exact
heading hid the regression-dilution bias entirely, and the fixture passed
while the shipped estimator was 4.5% out.

Recovered RMS error 0.15°, peak within 0.4° of truth, scale fitted to 1.0042
against a true 1.008, and no sample outside its own claimed error bar. A
gripping car comes back with a peak phantom angle of 0.27° against a 10°
threshold; a parked car claims no angle anywhere; a course with no straight in
it reports zero anchors, is flagged rough, and is dropped from the scorecard
rather than scored.

`node tools/check_mallala.js` — the same engine against a **real circuit**.
`tools/make_drift_fixture.js` builds a five-run drift practice day on
Mallala's own OSM survey (the right-left-right complex onto the main
straight), driven out on the grip and drifted through the complex, and the
check runs the file through the app's own VBO importer before asking the
Drift view what it sees. 21 checks: every run found, every widest angle within
0.5° of the truth, two switches a run as the corners demand, the best run
picked correctly, and the run that dropped its angle reading lower than the
one that did not.

That fixture was worth more than the analytic one. It found the
steady-angle calibration bias, it found that the lagged instrument failed on
fast corners, and it found that a recording of nothing but drifting cannot
calibrate at all — none of which the analytic drive could show, because it had
no real circuit in it.

An adversarial review of the finished code (four independent lenses, each
finding verified by a separate agent trying to refute it) produced 30 claims,
of which 14 survived and were fixed. Three were worth the exercise on their
own: the shared-resolver bug above, the least-squares dilution above, and
`gpLaneRowsAll` rebuilding its lane list from the raw declarations — which
discarded the live Angle lane precisely when a recording had the channels that
would fill it, making the lane unreachable. Regression checks for all three are
in the harness.
