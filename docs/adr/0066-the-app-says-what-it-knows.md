# ADR-0066: The app says what it knows

Date: 2026-09-04
Status: Accepted — built 2026-09-04
Repos: rdm7-desktop (`src/tauri-overlay.html`, `src/transport.js`,
`tools/check_autosync.js`, `tools/check_pairs.js`)
Follows: [0049](0049-a-recording-says-how-much-to-trust-it.md) (a recording says
how much to trust it), [0011](0011-nothing-is-drawn-that-was-not-measured.md)
(nothing is drawn that was not measured)

## Context

The eight features of `EIGHT_FEATURES_PLAN_2026-09.md` all shipped, and every
one of them turned out to be *"already there and not being said"*. Asked what
was left, the same pattern held: the numbers exist and the app is quiet about
them. Seven things, all of them assembly or plumbing, none of them new physics.

## 1. Nothing judged the puck, only the recording

ADR-0049 made a recording say how much to trust it — the fix, the position, the
timing, the channels, the angle, the video. Nothing did that one level down.
`grep bus_off|tx_err|rx_err|tec` over the desktop returned **nothing**.

Meanwhile, off his own bench: the puck's CAN transmitter fails **around a third
of its transmits** and cycles bus-off about **once a second** at TEC 128,
RX 0 — a transmit-side fault with a perfect receive path. From Studio that looks
exactly like a car that has gone quiet, and it cost days of bisection.

The node was not the problem. `method_can_bus` in `rdm-gps-node` has reported
TEC, REC, failed transmits, lost arbitrations, bus errors, the controller state
**and a one-line verdict** the whole time. `transport.js` simply had no
`canBus()`, so nobody ever asked.

**Decided.** Studio asks, on the same surface that already answers *"what is the
car saying"* — because *"can the puck say anything back"* is the other half of
the same wire, and the two fail independently. The node's verdict is quoted
verbatim and attributed rather than restated: a second opinion on the same
counters is how two surfaces come to disagree about one bus. What is added here
is the arithmetic the node cannot do — counters are cumulative, so a *rate*
needs two samples, and "148 failed transmits" is a number where "failing about
one in three, now" is a finding.

### …and "carrying data" was a check a dropped channel passed

The trust panel's channel row was binary:

```js
add("Channels", ids.length + " carrying data", "ok",
    "Every channel spoke at some point in the recording.");
```

A channel arriving at a third of its rate passes that. Which is precisely the
failure above, in every recording already on disk.

A stored sample holds `GP_CHAN_STALE` for "gone quiet since the last one", so
freshness is in the recording already, per sample, per channel. Two numbers come
out of it and only the pair is useful:

| | |
|---|---|
| **median interval** | the rate it is really arriving at |
| **worst interval** | the discriminator |

The rate alone proves nothing — a 10 Hz channel in a 25 Hz log is fresh 40% of
the time and there is nothing wrong with it, and the recording does not carry a
configured rate to compare against. **A channel that is logged slowly is slow
evenly; one that is being dropped is bursty** — a stretch of nothing while the
transmitter is bus-off, then it comes back. So the finding is the *ratio*, and
the wording says both readings of it ("dropped, or configured that way")
because the recording genuinely cannot tell them apart.

## 2. Lining footage up by what is in the film

ADR-0064 gave the footage timeline a proper editor. It still could not do the
one tedious job: the first alignment, when the camera clock, the logger clock
and "they started together" are all wrong.

**What is measured: how much the picture changes from one frame to the next.**
Not the speed of the car — this makes no claim to have measured that, and must
not, because it has not. It is a number that rises when more of the frame moves,
and collapses when the car stops. That is enough to find *where* a speed trace
fits against it and nothing like enough to be a speed.

Three things fell out of building it that were not obvious:

- **It has to be consecutive frames.** Two frames a third of a second apart have
  completely changed at any speed; the difference is saturated and carries no
  information about how fast. That rules out the obvious implementation
  (seek, sample, seek, sample) and forces playback.
- **Playback needs the compositor.** `requestVideoFrameCallback` only fires for
  frames that are *presented*. Measured in the preview pane: **76 frames while
  the window was fronted, zero while it was not** — and the same is true of the
  real app the moment somebody switches to their browser mid-sync. So there are
  two paths: play (fast, dense, needs the window) and seek-pairs at 0.08 s
  (slow, coarser, always works). The fast one is given four and a half seconds
  to prove itself and the slow one takes over if it does not.
- **A circuit is a repeating pattern, so its correlation has a peak every lap.**
  Picking the tallest of several near-equal peaks would put the footage a whole
  lap out with total confidence. The runner-up from outside the winner's own
  hill must be clearly worse; where it is not, this says so, says *where* the
  best fit was, names the reason, and **moves nothing**.

Proved end to end against a film generated to a known speed profile and then
deliberately misaligned by 7.30 s: found −7.00 at r = 0.90 on the coarse seek
path — and refused to apply it, correctly, because that fixture is six identical
laps of seven evenly-spaced identical corners, which is the adversarial case for
exactly the ambiguity guard above.

## 3. Which corner you cannot do twice

Every comparison in the app was one lap against one other. That answers "where
did this lap go", and it is the wrong question for deciding what to practise: a
corner you were half a second down in **once** is a mistake, and one you are two
tenths down in **every** lap is a technique. The first fixes itself.

So: the same comparison run across the whole session against one reference, read
down the columns rather than across the rows. Per corner, how much the minimum
speed, the brake point and the exit speed moved from lap to lap.

**The spread is a trimmed range, not a standard deviation** — one spin must not
decide what a driver is told to practise, and a σ over five laps is a number
with no meaning. And the trim is **counted, not a percentile**: a percentile is
the obvious way to write it and it silently does nothing at the sizes that
occur. A session is seven laps; `floor(6 × 0.1)` is 0 and `ceil(6 × 0.9)` is 6,
so the "middle 80%" of seven laps is all seven of them. This shipped that way
for an hour and produced a 10.36 s spread on a lap that varies by 0.7.

Nothing is drawn from fewer than three laps: two laps have a difference, not a
spread.

## 4. The session, in the sentence a driver leaves with

Every number already existed. Nothing turned them into the one thing a debrief
produces. The panels answer "how quick", "where did it go", "what cannot you
repeat" — each correctly, each separately, and nobody reads three panels and
does the arithmetic between them.

Assembled, never derived. Every clause is a number some other surface is already
showing, and any clause whose number cannot be had is **dropped rather than
softened into a guess**. Each clause also has to stand alone for that reason —
the first draft ended "the spread is bigger than most of what is below" and on
half of all sessions there is nothing below.

## 5. What the day was like

History compares a best lap across months without knowing one of those days was
wet. Three facts fix it, and there are three because they are the three that
change a lap time and cannot be recovered from the recording: how much grip
there was, how hot it was, and what had been changed since last time.

Deliberately **not** a weather lookup. A forecast for the nearest town is not
the grip at the circuit, and a number nobody typed is a number nobody should
trust.

## 6. Two smaller things

**The cars are named for their shape now.** Four were called Skyline, Silvia,
AE86 and RX-7 — trademarks, on a product that is sold. They are also a promise
the medium cannot keep: the map clamps a car to 30 px, where one glyph unit is
**1.36 screen pixels** and the entire difference between the kamm tail and the
square one is four tenths of one. A name that says "Skyline" invites an
inspection the drawing cannot survive. The IDs are untouched, so no saved choice
moves. **The set was not cut** — with the size now shown honestly beside the
picker (ADR-0065) a person can see the shapes are close and choose on taste,
and deleting four of them would remove that choice without adding anything.

**A harness for lists that have to agree.** The Setup rail was a list of section
names written *beside* the sections and matched by **position**, with a comment
above it warning that this had already gone wrong once. By the time it was
found it named nine of thirteen, and clicking "Camera" scrolled to Mounting.
Nothing failed, nothing logged, and the file the rail lives in had not changed —
the drift came entirely from other code growing a group. `check_pairs.js` walks
every such pairing: the rail against the groups, the panel menu against the
render chain, the dock's defaults against its menu, the bar and timeline
registries against their dispatchers, and every car against the classes the
steering and braking engines look for.

## Consequences

- `gpSpanTrim` is now the only trimmed-range helper and every caller passes
  through it; the percentile version is gone.
- The bus health poll re-renders Setup **only when a counter changes**. Setup is
  a page of inputs and this polls every four seconds; rebuilding it on every
  reply would take the field under the cursor with it, for numbers that had not
  moved.
- Auto-sync's success path — applying the offset — is the one branch not proved
  end to end here, because the only fixture with ground truth is the one whose
  periodicity makes the guard refuse. Everything up to the decision is proved,
  and the decision's other branch is.

## Checks

`tools/check_autosync.js` — 26 assertions. The correlation is handed a motion
series built as a *monotonic transform* of speed rather than a copy, so anything
that only works on the trace itself fails; it has to find an offset it was not
told, forwards and backwards, on both grids. Random motion must come back
unconfident, a film of a stationary car must normalise to nothing rather than to
noise, and the counted trim is pinned at seven values — the size that broke it.

`tools/check_pairs.js` — 20 assertions, above.

`check_health`, `check_clips`, `check_focus`, `check_carglyph`, `check_carown`,
`check_lookbar`, `check_laps`, `check_drift`, `check_playhead`, `check_shell`
and `check_gridfit` all still pass.
