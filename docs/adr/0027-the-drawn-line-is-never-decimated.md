# ADR-0027: The drawn line is never decimated

Date: 2026-08-09
Status: Accepted
Repos: rdm7-desktop (GPS workspace, Analyse and Corners maps)

## Context

The driven line on the Analyse map was assembled out of Leaflet
polylines, and its geometry was a function of the current zoom. Three
separate mechanisms threw position away, all of them in the name of
render speed:

1. **Pixel decimation.** `gpDrawTrace` kept about one sample per screen
   pixel (`stepM = metresPerPixel * 1.1`) and re-ran on every `zoomend`.
   The same corner was four points wide looking at the whole circuit and
   forty looking at the corner, so the drawn apex visibly *moved* as you
   scrolled the wheel.
2. **Leaflet's rounding.** `latLngToLayerPoint` rounds the projected
   point to a whole pixel — a quarter of a metre discarded at circuit
   zoom, before anything is drawn.
3. **Polyline simplification.** Every `L.polyline` carries its own
   `smoothFactor` pixel-space simplification on top of the above.

The cost was worst exactly where the tool earns its keep. Zoomed out to
see a whole lap, two laps a metre apart landed on the same pixels and
read as one line — and a metre is the entire subject of a line
comparison. The header comment above `gpSplitLaps` had promised "drawn
at the full 25 Hz … decimating for render speed would throw away exactly
the thing you are looking for" since the feature shipped; the
implementation had drifted away from it.

Speed was the reason, and it was a real one: one `L.polyline` per colour
segment meant ~1,500 map layers for one lap, and Leaflet walks all of
them on every frame.

## Decision

**The line gets its own canvas layer, and every recorded sample is
drawn at every zoom.**

`GpTraceLayer` is a small `L.Layer` on a dedicated pane (`gpTracePane`,
z-index 385 — above the imagery, below every vector and marker). It
owns one canvas and strokes the line into a 2D context directly:

- **No decimation, at any zoom.** What is drawn is the sample list.
- **Unrounded projection.** Points come from `map.project(ll, zoom)`
  minus the pixel origin, cached per zoom as absolute map pixels so a
  pan never re-projects.
- **No Leaflet simplification**, because there is no Leaflet path.
- **Colour runs, not colour segments.** Colour still varies per sample;
  consecutive samples whose colour rounds to the same RGB share one
  path. A 3,000-sample lap costs a few dozen strokes instead of 3,000
  map layers.

**And the smoother underneath it fits a curve instead of averaging.**
Decimation was the error you could see moving; the smoothing was the
larger error standing still. `gpSmoothPath` took a moving average, which
is a local *constant* fit: it cannot follow curvature at all, so on an
arc of radius R it pulls every point inward by L²/24R. Holding that
under a third of a metre is what forced the window narrow, and a narrow
window is a hard ceiling on how much receiver noise can ever come out.

A local *quadratic* follows curvature by construction. What is left is
the arc's quartic term, about h⁴/8R³ — so solving for the same tolerance
gives a half-width that grows as R^(3/4) instead of R^(1/2): roughly
twice the window at a hairpin, several times it through a fast sweeper,
for the same shape error. The extra samples are what buy the noise back.

Three supporting changes make that work:

- **The fit runs against speed-integrated distance, not summed position
  deltas.** `gpArcLength` has said why in one line since it was written:
  position deltas accumulate receiver noise into phantom metres. A car
  parked for twenty seconds sums 250 m of scribble that way, which told
  the old smoother it was travelling. Doppler speed says zero, the
  window swallows the whole thing, and the scribble collapses — 0.51 m
  across where the old code left 0.96 m.
- **The receiver's noise is measured off the recording, not assumed.**
  A fixed 0.30 m tolerance is wrong in both directions across the files
  Studio opens. Each fix's perpendicular offset from the chord through
  its neighbours, taken over the straightest half of the recording, is
  pure receiver noise; its median recovers an injected sigma to within
  8% from 0.15 m to 1.5 m. The tolerance is then half of it — never
  spend more shape error than the noise being removed.
- **A guard for logs with no usable speed column**, which would
  otherwise draw the lap as a blob. The test is net displacement rather
  than path length: a parked car and a dead speed channel look identical
  by path length, and only one of them covered ground.

What did NOT survive contact with the harness: interpolating each gap as
a circular arc through the recorded headings. It is a good idea on
paper — the chord between two fixes cuts the corner, and the receiver
knows which way the car was pointing — but measured, it changed the
error by nothing at all (0.170 m either way). At real sample spacings
the chord sag is already far below the residual noise. It is not in the
shipped code.

**And the other laps get a ghost.** While the playhead sits T seconds
into the analysed lap, every other lap on the map carries a faded dot at
*its own* position T seconds in. Aligned by time, not by distance:
distance alignment answers "who took the tighter line", which the Line
colouring already answers, while time alignment answers "who is ahead" —
and the dot falls further behind exactly where the lap lost the time. A
lap already finished parks its dot on its last sample.

## A colour per lap, and nothing moved

Laps drawn together need telling apart, and the palette opened with four
near-reds — `#9c1a1f`, `#dd4b45`, `#ec817b` and a grey. Four laps on the
map were four shades of the same thing. The palette is now eight hues
spread round the wheel in the order they are handed out, so the first six
laps land at 0°, 207°, 45°, 130°, 317° and 177°: no two close enough to
confuse. Every lap keeps its own colour at full strength, with a dark
hairline casing so a saturated line reads on a satellite photo of gravel.
The hierarchy that used to come from bleaching the other laps toward grey
now comes from WEIGHT — colour says which lap, thickness says which one
is the subject. The reference is told apart by being dashed rather than
by being white, because white said "reference" but stopped saying
"lap 3".

A fifth map mode, **Lap**, colours every line by which lap it is,
including the analysed one. The other modes answer "what was the car
doing here"; this one answers "whose line is this", and answering both at
once answers neither. In that mode the analysed lap drops to the same
weight as the rest: a 7.6 px stroke on top of neighbours one pixel away
does not emphasise it, it paints them out.

A key sits on the map, because a colour per lap is useless without one
and the lap list is not always on screen.

**What this deliberately does NOT do is magnify the difference.** An
earlier pass shipped exactly that — Spread, which pushed each lap away
from the analysed one by its own offset times a stated multiplier, so
lines a metre apart could be seen without zooming. It was measured,
labelled, and defaulted off, and it was still wrong: the first question
it drew was "what is this line?", and the answer — "somewhere your car
never went" — is not one an analysis tool should ever have to give. It
was reverted the day it landed.

The honest behaviour is that lines a metre apart need a zoom where a
metre is more than a pixel, and then they separate on their own. Measured
on five laps: two are distinguishable at 1.9 m/px, three at 0.96, and all
five by 0.48. That is the truth about the data, and the map now tells it
without embellishment.

## Switching laps holds your place

Clicking a different lap sent the playhead back to the start line and
re-framed the whole lap. So the way to compare one corner across five
laps was: zoom into turn six, click the next lap, watch the map fly back
to the start-finish straight, zoom in again. Five times.

Two things caused it. `gpSelectLap` reset `gp.playIdx` to the lap start
and called `gpFitTrace`; and the automatic re-fits in `gpDockViewer` and
`gpSetView` fired on every relayout, which the Analyse grid triggers on
every render because it re-parents the map into its panel.

Now the playhead moves to the same PLACE ON TRACK in the new lap —
`gpSameSpot`, the sibling of `gpLapIdxAtSecs`, which finds the same
moment. Position is what has to survive the switch; time is not, and
holding the time instead would land you somewhere else entirely on a lap
a second and a half slower. Distance round the lap gets close and a local
nearest-position search finishes it, because two laps that take a corner
differently cover slightly different ground getting to it. A corner zoom
on the rack travels the same way, for the same reason.

And a recording is framed ONCE. `gp.framed` is set by the first fit that
actually had something to fit into a container that had actually been
laid out — both conditions matter, because the 0 ms attempt of
`gpDockViewer`'s 0/60/250 ms retry runs before the aspect-ratio box has
resolved, and letting that one count would leave every recording framed
to a 0x0 map.

Measured: zoomed to 0.24 m/px on a corner and clicking through four laps,
the map centre moves 0.06 m and the zoom does not change, while the
playhead lands 1.0-2.9 m from where it was — which is the real gap
between those racing lines, not a positioning error.

## Playback runs off seconds

Counting samples needs a sample rate, and there isn't one. The playback
ticker advanced a fixed number of INDICES per frame from a hard-coded
25 Hz — right for a puck download and wrong for everything else. A VBO
import from another logger is 10 Hz, and the puck itself skips idle
samples on purpose. Measured in the app on a 10 Hz Sonoma log: the
playhead covered **28 recorded seconds in 10 seconds of wall clock**
with the button reading 1×, while the timecode beside it — which has
always read real timestamps — counted up in agreement with nothing on
screen.

The position in each frame is now read off the wall clock and converted
through the recording's own timestamps, anchored at the moment play
starts and re-anchored whenever something else moves the playhead (a
scrub mid-play, J jumping back, a coach jump, a rate change). Measured
after: 93.0 recorded seconds across a 90 s wait. Reading the clock also
means a late frame catches up rather than quietly losing that time; the
old ticker could only ever fall behind.

The same "seconds are seconds" rule now covers the two transport
shortcuts that also counted samples at 25 Hz: J's five-second jump, and
Shift+arrow's one second.

**The default rate is 1×.** It was 4×, which had been hiding an
arithmetic bug: 25 Hz over a 20 fps ticker is 1.25 samples a frame, and
rounding that to 1 played every recording at 0.8× — invisible at 4×,
where five samples a frame is exactly right.

## What this deliberately does not do

- **It does not stop smoothing the line.** Raw fixes are noisy, and at a
  crawl the noise is larger than the movement. The smoother changed
  shape, not purpose.
- **It does not change any measurement.** Lap times, sector crossings,
  deltas and the coach still read raw fixes (ADR-0011). This is
  presentation only, and now presentation that discards less.
- **It does not add a "smoothing" slider.** The window is derived from
  the receiver's own measured noise and the local radius. A slider would
  be asking the user to guess at something the data already answers.
- **It does not add a quality setting.** "Finest accuracy" is the only
  mode; a setting here is a way to ship the old bug behind a checkbox.
- **It does not move markers to the canvas.** The playhead and the
  ghosts stay Leaflet `circleMarker`s, so a 20 fps playhead never
  triggers a full line redraw.

## Consequences

- Verified against a 2,350 m synthetic circuit, 8,290 samples: at every
  zoom from 13 to 21 (15.4 m/px down to 0.06 m/px), every probed sample
  of the analysed lap lands on a lit pixel of the drawn line — max
  deviation 0 px, zero misses. The line no longer changes when you zoom.
- `tools/check_line.js` scores the shipped smoother against a course
  whose true shape is known analytically, and takes a git ref to compare
  against. Distance from the drawn line to the real one, RMS metres:

  | rate | receiver noise | raw fixes | before | after |
  |------|---------------|-----------|--------|-------|
  | 10 Hz | 0.2 m | 0.190 | 0.164 | **0.089** |
  | 10 Hz | 0.4 m | 0.365 | 0.229 | **0.166** |
  | 10 Hz | 0.8 m | 0.675 | 0.386 | **0.313** |
  | 25 Hz | 0.2 m | 0.183 | 0.135 | **0.059** |
  | 25 Hz | 0.4 m | 0.333 | 0.157 | **0.106** |
  | 25 Hz | 0.8 m | 0.559 | 0.262 | **0.196** |

  Over the tightest 10% of the course — where a wider window was the
  thing to fear — 0.273 → 0.165 m at 10 Hz and 0.211 → 0.093 m at 25 Hz.
  The apex is better held, not worse.
- Cost: a 12,000-sample session 33 ms, a full 331,000-sample ring 730 ms,
  computed once per recording and cached. Two details were worth an
  order of magnitude between them — the window bounds must SLIDE rather
  than rescan from each sample, and the two medians behind the noise
  estimate are taken over a 20,000-sample stride rather than the whole
  ring.
- The `zoomend → gpDrawTrace` rebuild is gone; the canvas redraws itself,
  and a zoom animation is a CSS transform (Leaflet's own renderer trick)
  rather than a rebuild.
- `gpMetresPerPixel` had one caller and is deleted with it.
- `window.__gpTrace` joins `window.__gpMap` as an automation handle.
- **Default playback rate is now 1×**, which surfaced a bug the old 4×
  default had hidden: the 20 fps ticker rounded 25 Hz ÷ 20 = 1.25
  samples to 1, so 1× played every recording at 0.8× and 2× at 1.2×.
  The advance is accumulated now. Measured after: 23.7 / 49.9 / 99.5 /
  248.7 samples per second at 1× / 2× / 4× / 10× against an ideal 25 /
  50 / 100 / 250.
- **The dock's rate segment marks itself.** It was built once with 4×
  lit and never updated, so clicking it changed the rate with nothing on
  screen saying so, and the control read as broken.
