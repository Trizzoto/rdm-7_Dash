# ADR-0013: One course type — a finish line is a thing you add, not a mode you pick

Status: accepted, 2026-07-29. Implemented in RDM Studio's Tracks and Session
views; no firmware change.

## Context

`lap_track_t` has carried a `point_to_point` flag and a second `finish` line
since the lap engine landed (ADR-0008). The engine's rules are already right:

- **Circuit** — one line. The first crossing arms the timer (so there is no
  out-lap); every crossing after it closes a lap and opens the next.
- **Point-to-point** — two lines. Crossing the start arms a run, crossing the
  finish closes it and *disarms*, and re-crossing the start while a run is live
  restarts it rather than timing an attempt nothing closed.

Studio's first pass at exposing that copied the struct's shape into the UI: a
**Circuit / Sprint · hillclimb** segmented control at the top of the track
inspector, which had to be set *before* the finish-line button appeared. Below
it, a note explained the two timing models as if they were two features.

Two problems with that.

**It asks a question with no good moment to answer it.** You pick the course
type before you have placed anything, on a screen where nothing yet
distinguishes the two. The words carry the whole burden — and "sprint" and
"hillclimb" are not what an Australian time-attack driver calls the thing they
are doing on a circuit with a rolling start.

**It makes one product feel like two.** The market position is integration:
one app, one mental model, for the whole device family. Presenting lap timing
and time-attack timing as separate modes is the opposite — it teaches the user
that the tool has two halves, and that they must know which half they are in.

There is also a redundancy that guarantees drift: `point_to_point` and
`finish` are two representations of one fact, so a library can hold a "sprint"
with no finish line (armed, uncloseable) and a "circuit" carrying a stale one.
Both shapes existed in stored track libraries.

## Decision

### 1. There is one kind of track. The finish line decides what it is.

Studio stores no course-type flag. `gpIsTrial(t)` is `!!t.finish`, and
everything downstream reads that:

| track | UI says | timing |
| --- | --- | --- |
| start/finish only | `circuit` | every crossing closes a lap |
| start/finish + finish | `time trial` | start line → finish line, then nothing until the next start |

Adding the finish line **is** the gesture that changes the course type. It sits
next to `+ Split` in the gate list, and removing it is labelled *"Remove the
finish line — back to a circuit"* so the round trip is visible before you
commit to it.

The segmented control is gone. So is the explanatory note at the bottom of the
inspector: the whole model now lives in one hover lamp on the Gates group,
consistent with the density pass that moved standing text into lamps
everywhere else in the workspace.

### 2. `point_to_point` stays on the wire, derived at the boundary.

`lap_core.c` reads the flag, so `gpSendTrack` sets `point_to_point: true` when
`t.finish` exists and omits it otherwise. Deriving it at the one place it
crosses the wire means the two can never disagree — which is exactly what
storing both allowed.

The firmware is unchanged. This ADR describes a change of question, not of
capability.

### 3. Legacy libraries are normalised on load, not migrated on write.

`gpTracksNormalise` resolves both stale shapes every time the library loads:
a `point_to_point` track with no finish becomes a circuit, a non-p2p track
carrying a finish loses it, and the flag is dropped everywhere. Import does the
same. Files written since have no flag and are untouched.

### 4. "Time trial" is the word, and the session view uses it too.

`gpRunWord()` follows the active track, so a recording splits into **laps** on a
circuit and **runs** on a time trial — in the session rail, the compare list,
the ideal row (*"best sectors from run 2, 3"*), the coach header and the map
mode label. Calling a hillclimb attempt "Lap 3" is small, but it is the kind of
small wrongness that tells a user the tool was built for somebody else.

### 5. The session splitter learned the second line.

This is the part that was actually missing rather than merely mis-presented.
`gpSplitLaps` only ever split on start/finish, so a time-trial recording
produced **zero** laps and the entire Session workspace — corner analysis,
delta strip, coach, lane rack — was dead for every sprint and hillclimb.

It now mirrors `lap_core.c` exactly, including the parts that are easy to get
wrong:

- a crossing is a sign change from before to on/past, **inside the gate's
  half-width** — a directional gate, not a proximity radius;
- start line resolved before finish line, so a course whose two lines sit close
  together splits deterministically;
- on a time trial the start crossing can only *open* a run. A start crossing
  while one is already live **restarts** it, because an attempt that never
  reached the finish was abandoned — not a slow result;
- the trip back from the finish to the start is outside every run.

`course_check.js` drives synthetic paths across real gate geometry and asserts
all of the above (36 checks), including that the same recording splits into 1
lap or 2 runs depending only on whether the track has a finish line.

## Consequences

- Nothing is lost. Every course the segmented control could express is still
  expressible, in one fewer decision.
- A track can be a time trial with its start line unplaced. `gpSendTrack`
  already refuses to send a track with no start/finish, and the gate list
  labels it *"— not placed"*.
- `lap.capture` on the node accepts `what: "finish"`, so Studio now offers
  **Capture finish** beside Capture start and Capture split. This matters more
  than the map editor for a hillclimb, where the finish is a cone in a paddock
  rather than paint visible from orbit.
- Non-contiguity is new: a circuit's laps tile the recording, a time trial's
  runs do not. Everything downstream indexes into `gp.trace` by absolute
  sample, so nothing assumed otherwise — but new analysis code must not.

## Alternatives considered

**Keep the toggle, hide it until a finish line exists.** Still two
representations of one fact, and the toggle would then be a redundant control
that can only ever agree with the gate list.

**Name it by discipline (`Circuit` / `Sprint` / `Hillclimb` / `Autocross`).**
More words for the same one bit, and the taxonomy is regional. `lap_core.c`
does not distinguish them and neither should the UI.

**Infer the course type from the recording.** Tempting — a trace that never
re-crosses its start line is obviously point-to-point. But the track is
configuration that is sent to a device and used live, before any recording
exists. Inference would make the same track behave differently depending on
what you had driven, which is worse than asking.
