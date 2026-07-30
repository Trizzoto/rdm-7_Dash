# ADR-0012: Corner-phase time attribution, and the optimal-lap boundary

Status: accepted, 2026-07-28. Implemented in RDM Studio's Session view.

## Context

Competitive research across the lap-timing market (see
`docs/research/2026-07-gps-laptimer-market.md` and the 2026-07 competitor
sweep) produced one finding that stood above the rest:

> No mainstream real-world motorsport tool decomposes a corner into phases and
> attributes time to each. Circuit Tools gives you a delta-T channel and sector
> highlighting; MoTeC gives you a variance channel; AiM gives you Time Compare.
> All three then leave the driver to read the traces.

Sim-racing tools *do* decompose corners — Coach Dave Academy's Delta reports
`Braking −0.36s / Entry +0.12s / Apex +0.16s / Exit −0.02s` with causes in
metres and km/h — and their users report it is what actually made them faster.

Separately, the loudest thing owners say about every product in this category
is not about accuracy:

> "easy to use...you'll use it...if difficult...you won't"

Devices get abandoned unopened. The binding constraint is comprehension, not
data quality.

## Decision

### 1. Attribute time per corner phase, not just per lap or per sector

Every corner is split into **braking / entry / apex / exit**, and each phase
carries a signed time against a reference lap, with the cause expressed in
metres, km/h and seconds of coasting.

Phases are **sequential and non-overlapping** and tile the corner exactly.
Overlapping them would let the same tenth be attributed twice, and the corner
total would stop adding up to the lap total — the kind of quiet arithmetic
error that destroys trust in a number nobody can independently check.

### 2. Align laps by GPS position, never by integrated distance

Racelogic publish the reason, and it is decisive: two drivers at the
Nordschleife who had both covered exactly 19,785 m of rolling distance were
**186 m apart on track**. Every per-corner number would inherit that error.

MoTeC patches the same problem with a "Distance Stretching" setting defaulting
to 5%, and it demonstrably misleads: a user reported a 0.571 s lap difference
displaying as 0.529 s of variance, with the fix buried under
Tools → Corrected Speed and Distance.

We are GPS-native, so position alignment costs us nothing. Corners are detected
on the reference lap and their phase boundaries mapped onto the analysed lap by
nearest position, with a windowed search so a circuit that crosses itself
cannot match the wrong section.

### 3. The delta must reconcile to the lap-time difference by construction

Not by a settable fudge factor. Each sample's delta is
`(elapsed in this lap) − (elapsed in the reference at the same position)`, so
the value at the flag equals the lap-time difference to within one sample
period (40 ms at 25 Hz), and that residual is quantisation, not method error.

Verified: 2.440 s reported against a 2.400 s actual difference; identical laps
produce a flat 0.000 s.

### 4. Prescribe an experiment, not a number

Driver61's own coaching article on braking too early contains **no telemetry
at all** — no traces, no channels, no numbers except a risk warning. The method
is behavioural: pick a reference point, move it forward over four or five laps,
verify turn-in stays smooth, repeat.

Telling someone they braked 12 m early is a fact. Telling them what to change
and what to watch for is coaching. Every opportunity therefore ends in an
action with a verification criterion.

### 5. Refuse to pad, and refuse to part-time

- If only two corners lost meaningful time, two opportunities are shown. If
  none did, the panel says so. A third "opportunity" worth two hundredths
  would train people to ignore the panel.
- A lap that missed a sector line is **refused, not part-timed**. Reporting
  partial splits would put a fast-looking sector against an incomplete lap.
- The **Ideal Lap is displayed but not selectable as a comparison reference**.
  Offering it would mean synthesising a trace nobody drove, and comparing a
  driver against a fiction. Circuit Tools permits this; we do not.

### 6. Do NOT build optimal-lap splicing

Garmin hold three live US patents on it, priority 2018-12-05, assignee Garmin
Switzerland GmbH:

| Patent | Splice constrained by | Expiry |
|---|---|---|
| **US 11,710,421** (parent) | **geolocation only** | **2041** |
| US 11,710,422 | geolocation + velocity, plus AR key points on video | 2040 |
| US 11,830,375 | geolocation + velocity; audio and spliced-video variants | — |

The common assumption — that plain best-sector splicing is free and only the
speed-compatibility check is novel — **is wrong**. The parent claims the broad
geolocation-only splice; velocity matching is the *narrowing* added in the
continuations.

Two gaps exist and are worth recording:

- **There are no method claims and no computer-readable-medium claims anywhere
  in the family.** Every independent claim is an apparatus claim reciting "a
  memory device… an output device… a processing element". A desktop analysis
  product is a materially weaker target than a windscreen-mounted device.
- **Every claim requires compositing the driver's own multiple laps.** The
  specification is explicit that this is deliberate. Comparison against a
  *single external reference* — a pro's lap, a community best, a physics model
  — falls outside the family.

Both continuations still pending in 2023 went **abandoned**, so no new scope
can be minted against that priority date.

None of this is a freedom-to-operate opinion, and claim construction stretches
further than plain words. But the practical conclusion stands: corner-phase
attribution, sequence-aware prioritisation and reference-lap comparison are all
outside the family and all more differentiated than splicing. **Optimal-lap
splicing is the least valuable thing we could build and the only one carrying
real legal risk.** Garmin's own paid consultant concedes its ceiling — it can
only reflect back what the driver has already demonstrated.

## Consequences

- The analysis layer (`gpFindCorners`, `gpCornerPhases`, `gpCompareLaps`,
  `gpDeltaSeries`, `gpSectorTimes`) is pure — no DOM, no Leaflet — so it is
  exercisable from the host harness. `scratchpad/coach_check.js` drives a
  point-mass simulator around a fixed geometric path so two laps share exactly
  the same racing line and differ only in speed, which isolates time
  attribution from line differences.
- Corner detection judges a speed minimum against the peaks on **both** sides,
  except at a lap boundary where only one side exists. Without that exception
  the last corner before the start/finish line is silently dropped from every
  lap — which it was, until the harness caught it.
- The strip's static layers are cached offscreen keyed on
  `(lap, reference, zoom, width)`; a full lap is ~2,250 samples and redrawing
  it per playback frame is ~45,000 fill operations a second.
- This app has **no semantic colour tokens** — only `--accent`. `var(--danger)`
  and `var(--success)` silently resolve to black. Use the literals the rest of
  the GPS UI uses: `#e05d52` bad, `#6FBF73` ok, `#FF9F0A` warn.

## Addendum 2026-07-28 (later the same day): sequence-aware insights

Built. One layer above the per-corner comparison (`gpCoachInsights`), producing
three kinds of finding: **corner**, **chain** (corners the driver compromised
in sequence), and **straight** (a loss out there that the corner before did
not cause). The coaching rule it feeds is the sim world's best idea, per
Coach Dave's Delta: *fix the earliest mistake in a sequence first.*

The causal claims are gated, not assumed — this is the part worth defending in
review:

- A straight's loss is attributed to the corner before it **only when** the
  exit was slower AND the deficit is still present at the fastest point of the
  straight. If the car recovered in between, no attribution.
- Two corners link into a chain **only when** the first was exited slower AND
  the deficit survives to the second's braking point. Where corners are
  adjacent (no measurable segment between), the carry is the speed deficit at
  the hand-off itself.
- A straight that lost time behind a **clean** exit is its own finding, never
  blamed on the corner — and if the analysed lap contains a braking event
  there that the reference lap does not, the card says "lift" and quotes the
  dip in km/h.

Verified in the harness with three scenarios that must not confuse each other:
a slow apex whose deficit carries into the next corner (chains, and the
straight is *not* blamed); a mid-plateau lift with a clean exit (straight
finding with the lift named, and *no* chain invented); a 40 m chicane where
slowing the first corner links the pair and the advice reads "fix here first".
Identical laps produce zero findings of any kind.

Two implementation notes for whoever touches `gpFindCorners` next:

- The exit/entry walks use `>=` (they must cross flat sections — the apex
  itself is one, since the car holds apex speed through the arc), then **trim
  the trailing flat** off each end so a flat-out plateau is not swallowed into
  the corner's exit phase. A strict-rise walk finds zero corners; an untrimmed
  `>=` walk blames a lift half a kilometre away on the corner. Both failure
  modes were hit and are covered by tests.
- The lap-boundary `edge` flag must be captured **before** the trim, or the
  last corner of every lap disappears again.

Also in the same change: the comparison lap auto-defaults to the session's
fastest when the lap set changes (Circuit Tools' behaviour), comparing a lap
against itself is refused with a message rather than a flat zero delta, and
re-splitting laps invalidates every derived cache — the caches are keyed on
lap *indices*, which mean different laps after a re-download.

## Addendum 2026-07-28 (evening): the map answers "where", at a glance

Three colour modes on the session map — Pace, Speed, and **vs Ref** (the
track coloured by the *rate* of time gained or lost against the comparison
lap, the channel MoTeC users build by hand as a maths expression) — plus
cumulative delta at the playback cursor, keyboard scrubbing, and four
at-a-glance summary tiles (best, average, spread, time left on the table).
The one decision worth recording: **vs Ref** falls back to Pace rather than
draw a lie when no comparison lap is chosen — a track coloured by a delta
that doesn't exist would look exactly as confident as one that does.

Two defects the harness caught before shipping, both real coupling made
explicit rather than newly introduced: `gpCoachJump` reached `gpScrubTo` as a
bare identifier (works only because a browser's `window` is the global
object), and `gpTrackById` dereferenced `gp.tracks.tracks` unguarded one line
after its own caller guarded `gp.tracks &&`.

## Addendum 2026-07-29: the lane rack, and what the recording cannot tell us

The Session view now carries a stacked lane rack under the map — speed,
braking/acceleration, lateral g, yaw rate and delta on a shared x-axis with one
playhead and a per-lane value at the cursor.

**Lateral g and yaw rate are derived, not recorded, and that is legitimate.**
Yaw rate is heading change per second, unwrapped across the 359→0 seam; lateral
g follows as `v·ω/9.81`. Verified against the harness's own geometry rather than
by eye: on 60° arcs of 57.3 m radius the rack reports **31.3 °/s** peak yaw
against ~31 predicted, and **1.70 g** against **1.66 g** from v²/r at 110 km/h.
Heading is forced to zero below 3 km/h — the receiver freezes course-over-ground
at a standstill and the seam-unwrap would otherwise turn parking-lot noise into
a spike dwarfing every real corner.

**Four channels are declared but cannot be filled, and are drawn as empty
frames rather than omitted or faked:**

| Lane | Blocked on |
|---|---|
| Throttle, Brake, Steering | a CAN channel from the car |
| Roll & pitch | **IMU logging on the node** |

That last one is a firmware finding worth recording: `trace_sample_t` is 12
bytes — lat, lon, speed, heading. **The puck has an IMU and reports it live over
`gps.status`, but the recorder does not store it.** Attitude graphs are a node
change, not a Studio one. The storage arithmetic from the earlier session
applies: position-only fits a full track day, position+IMU does not.

Empty lanes get a frame, a label, a dashed midline and a plain statement of what
has to happen first. A flat zero line would look like a car that never turned;
omitting them hides the shape of what the product is going to be.

Two defects caught while building it:

- The scrub handler mapped clicks across the whole canvas, but the rack puts a
  74 px label gutter on the left — every click landed a fixed distance early.
  Fractions are now measured against the plot area.
- The test simulator emitted `hdg: 0` for every sample, so the yaw and lateral-g
  checks passed **vacuously against zero**. The simulator now derives heading
  from the path tangent the way a receiver derives course over ground. Worth
  remembering: a physics test that agrees 0.00 with 0.00 is not a passing test.

### The standstill guard, and why synthetic data could not have found it

The first yaw guard was `kph < 3`. A real 331,882-sample recording pulled off
the puck — 3.7 hours of it sitting on a desk — **peaked at 3.04 km/h of GPS
drift**, crossed that gate, and emitted **10.08 °/s of yaw from heading noise
alone**. Unguarded, the same data produces **524 °/s** in its first 5,000
samples. A synthetic car has no drift and could never have shown this.

The guard is now two gates, and the second one is the durable idea:

1. **8 km/h** — walking pace. Below it no car is cornering and
   course-over-ground is not trustworthy; above it, it is.
2. **3 g of implied lateral load** — physics rather than speed. A car on tarmac
   does not pull 3 g sideways, so anything past it is a bad fix, not a corner.
   Without this a single spike rescales the entire lane and hides every real
   corner in the flat middle.

`scratchpad/real_check.js` keeps this recording as a permanent regression. Any
future change to the channel maths gets tested against real receiver noise, not
only against a car that behaves.

### Envelope decimation, for the same reason

The same recording exposed a second problem synthetic laps hide. A full ring is
331,882 samples into a plot ~520 px wide: **638 samples fighting over every
pixel column, 1.33 million `lineTo` calls per redraw, 99.7% of it invisible.**
That stalls the UI to draw something nobody can see.

Past ~2 samples a column the rack now collapses each column to its **min and
max** and strokes the envelope. Averaging would have been the wrong choice —
it smooths away precisely the brake spikes and kerb strikes being looked for.
The envelope keeps every peak and only discards the wiggle *between* peaks that
lands inside one pixel.

The property worth defending in review is not "fewer points than samples" — on
a single lap the two are close. It is that **draw cost is bounded by pixel
width however long the recording gets**: 1,040 points whether the range holds
1,405 samples or 331,882. There is a test asserting a one-sample spike survives
638:1 decimation, which is the test that fails if someone later "optimises"
this into an average.

**Also found:** `trace.info` reports `used_samples` (331,862) greater than
`capacity_samples` (331,840) once the ring has wrapped, because
`trace_log_get_info()` adds the un-flushed RAM page on top of a full ring. Reads
genuinely can return those samples, so the node is not lying — but Studio was
rendering "101% capacity used". Clamped in the display rather than changing the
node's semantics, which are defensible.

## Still open

- Time-budgeted review depths (5 / 15 / 60 minutes), per Bentley's 5-15-1
  framing. The "three things" half exists; the budget framing does not.
- Automatic video sync by cross-correlating the camera's GPS speed against the
  puck's — see `CAMERA_CONTROL_PLAN_2026-07.md`. This is the loudest unmet
  demand in desktop analysis and needs no user action at all.
- The straight-card wording for a loss with no lift signature ("check shift
  points, or whether the car was down on top speed") is untested — a top-speed
  deficit that begins before the exit measurement point is indistinguishable
  from a slow exit with speed-only data, so the generic branch is hard to
  reach synthetically. Flagged rather than hidden.
