# ADR-0020: Recording needs nothing pressed, and readiness is one line

Date: 2026-07-30
Status: Accepted
Repos: rdm7-desktop (`src/tauri-overlay.html`) — desktop-only UI, no firmware change

## Context

Stage 1.4 of the lap-analysis redesign turned Record into a readiness panel:
nine rows, each a fact, each failing row carrying the action that fixes it.
The rows were right. Three things around them were not.

**1. The product told you to press a button that changes nothing.** In the
node firmware, `s_recording` is initialised to `true`
(`rdm-gps-node/main/storage/trace_log.c:56`) and is never persisted, so the
puck logs from the moment it has power. It writes only above
`TRACE_IDLE_KPH_X100` (8.00 km/h), which is why a stationary car produces an
empty download. The only way to reach a non-recording puck is to press *Off*
during that same power cycle.

Yet three places said otherwise:

| Where | What it said |
|---|---|
| The checklist's Node row | `idle` · "press Record on before you drive" |
| The empty-download message | "Recording is OFF — press Record on before you drive" |
| `docs/IN_THE_CAR_2026-07-30.md` | "**Record on** (Session panel), then drive" |

So a puck that was already logging perfectly well was graded *not ready*, and
the fix offered for it was a button whose real job was something else
entirely. `gpTraceRecord(true)` armed the node **and** started Studio's live
view — the second half is why the button felt necessary, and why the panel
then had to explain a "recording" control that was not what made recording
happen.

**2. Nine rows of mostly-fine is a panel you read to learn nothing.** On a
good day every row said "ok" and the answer to the only question anyone had —
*will this record laps?* — had to be assembled by reading all nine.

**3. It was below the fold.** The checklist lived at the foot of the Session
aside, under the recordings list and the lap list: the thing you need
*before* a drive, printed under the things you read *after* one.

Two smaller instances of the same shape: whether a track is a circuit or a
time trial is stored implicitly as "does a finish gate exist" (ADR-0013) and
was explained only in a tooltip; and the order of operations for a new track
(pick → place the line → send it to the puck) was written in a doc and nowhere
in the app.

## Decision

**Recording and watching are two separate buttons, and only one touches the
node.** `gpTraceRecord(on)` now does nothing but pause and resume the node's
log; the new `gpLiveWatch(on)` owns the live view. The pair renders as
**Watch live** and **Pause logging**, labelled from live state. The checklist
row becomes `Logging · on · anything above 8 km/h`, or `paused · a power cycle
turns it back on` with a Resume button — the truth in both directions, and no
instruction for a prerequisite that does not exist.

**The checks become data, read at two zoom levels.** `gpReadyRows()` returns
one array of `{k, v, tone, sub, fix, wait}`; `gpReadyVerdict()` reduces it
worst-wins. The banner and the expanded list render the *same array*, so a
green summary sitting over a broken row is not expressible. `wait` marks a
check that has not answered yet — it withholds "Ready" without crying wolf.

**One rule for the card: collapsed shows what is WRONG, expanded shows
everything that was checked.** On a good day the wrong-list is empty and the
card is a single line — *Ready to record · Winton · 11 sats*. On a bad one the
failing row and its fix button are on screen without anyone having to know to
open anything. It renders into `#gpReadyCard` at the **top** of the Session
aside, via its own `gpRenderReady()` called from both the rail build and the
1 Hz inspector render — the panel that must be current cannot be the one only
redrawn when the rail happens to be rebuilt.

**Tracks states what it previously left to be inferred.** A `Circuit | Time
trial` segment writes the same single `finish` field the "+ Finish line"
button always did — a label for the state, not a second way to store it, so
ADR-0013 is untouched. Going back to a circuit destroys a placed gate and
therefore asks; becoming a trial only adds one and does not. A three-step
strip (`1 Circuit picked · 2 Put the line on the paint · 3 Send it to the
puck`) appears only while the track is unfinished, so a set-up track never
carries instructions for work already done.

## Consequences

- `gpReadyHtml()` is gone, replaced by `gpReadyRows()` / `gpReadyVerdict()` /
  `gpReadyCardHtml()` / `gpRenderReady()`. `gpReadyRow()` survives unchanged as
  the row markup.
- The Recording group in the Session inspector is now four buttons and a note.
  Its checks moved up; nothing else about it changed.
- **A polling wedge was found and fixed in the same pass.** It was first
  described here as a stuck `gp.lapInFlight` latch; that was the wrong
  mechanism. `gpLapPoll` runs on a `setInterval` and `_gpOpen` clears its
  latch synchronously, so it cannot orphan. The real defect is in `gpTick`,
  and it is worse: that loop is **self-clocking** — the only thing that starts
  the next tick is the last reply landing — and `gpPollSchedule()` sat *behind*
  the epoch guard. A reply arriving after `_gpOpen()` bumped the epoch
  returned early and never re-armed, so **all** status polling stopped, with
  no interval to recover it. The comment directly beneath the guard already
  asserted the invariant the code was breaking: *"Re-arm FIRST: whatever the
  reply was, the loop has to keep running, and nothing below here is allowed
  to stop it."* The re-arm now precedes the guard; the state writes
  (`inFlight`, `_misses`) stay behind it, so a stale reply cannot release a
  live request's latch. Rescheduling from a stale reply is safe because
  `gpPollSchedule` clears any pending timer first.
- No firmware change. The 8 km/h idle gate and the boot-on default are
  described here, not altered.

## Verification

Merged dist served in the browser pane, with a fake `gps` transport attached
via `RDM.attach()` so the real poll loop drives real state:

- Card is `#gpRailBody`'s first child, and renders above the *Recordings*
  title (y=316 vs y=446). It fits the rail with no horizontal overflow.
- Everything set up → **48 px tall**: *Ready to record · Winton · 11 sats*,
  `.gp-ready` with no tone class, green lamp, **0 rows and 0 buttons**.
  Expanded → **261 px**, all six checks: Fix `3D fix · 11 sats · 0.8 m`,
  Track `Winton · circuit`, Start/finish `placed`, On the node
  `Winton · armed, watching for the line`, Logging `on · anything above
  8 km/h`, Recorded `30.0 min · 340 min still free`. That 48-vs-261 is the
  whole change.
- No track → *One thing to fix*, only the Track row, `.gp-ready.warn`.
- Node holding a different or stale track → *One thing to fix*, the
  `On the node` row, and a `Send "Winton" to the node` button. Both the
  `none` and `stale` branches were reached.
- `gpTraceRecord(false)` → *2 things to fix*, second row `Logging · paused`,
  two stacked fix buttons (measured at y=555 and y=590, 28 px tall — they do
  stack), group button flips to *Resume logging*, message reads "Logging
  paused. A power cycle turns it back on." `gpTraceRecord(true)` restores all
  of it.
- Tracks: steps strip renders `[done] 1 · [done] 2 · [todo] 3` with the node
  holding no track, and is absent entirely once `gpNodeTrackState()` is
  `match`. The segment flips Circuit ⇄ Time trial, the sub-heading follows
  (`circuit · 1 sector` → `time trial · 1 sector`), and the gate list becomes
  Start / Finish.
- `node --check` equivalent over all four inline scripts in the built
  `src/dist/index.html`: 0 syntax errors.

The readiness/Tracks work was not verified against real hardware — no puck was
attached for that part. The node-side facts it rests on (`s_recording = true`,
`TRACE_IDLE_KPH_X100`) were read from `rdm-gps-node`, not measured.

**The Monitor changes below were verified on real hardware**, on a puck whose
receiver had genuinely gone quiet (serial `1CDBD4880D18`, fw 0.1.0):
`Receiver data lost 0 · none` and `Receiver restarts 0 · none` rendered from
the node's own reply, and `Corrections` read *not reported until there is a
fix* rather than the old false accusation. The status loop kept polling after
the `gpTick` change (live IMU, live rate figures), which is the regression that
change could have caused.

## Addendum — two Monitor honesty fixes, same session

**The node reports `rx_overflow` and `rx_recover`; Studio rendered neither.**
They are emitted unconditionally by `serial_rpc.c`, and
`docs/IN_THE_CAR_2026-07-30.md` tells the reader to note both before
power-cycling — an instruction that could not be followed, because no surface
showed them. They are now the last two rows of the Satellite receiver card, in
words rather than field names: **Receiver data lost** and **Receiver restarts**.
A node too old to send them says so instead of showing a zero it never sent.

**`Corrections` accused current firmware of being ancient.** The row printed
*"node firmware predates this field"* whenever `diff` was absent — but `diff`
ships *inside* the reply's fix block, so it is missing from every reply made
without a fix. A searching receiver therefore produced a firmware diagnosis.
Position lives in that same block, so `lat_1e7 === undefined` now separates the
two cases: *not reported until there is a fix* versus the genuine old-firmware
message.

## References

- ADR-0013 (one course type — the segment labels it, does not replace it)
- ADR-0015 (the node is part of readiness — the "On the node" row)
- `docs/LAP_ANALYSIS_REDESIGN_2026-07.md` §Stage 1.4 (the panel this reworks)
