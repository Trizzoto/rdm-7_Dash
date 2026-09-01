# ADR-0053: A circuit can be built from the drive that proves it

Date: 2026-09-01
Status: Accepted (implemented 2026-09-01)
Repos: rdm7-desktop (GPS workspace — `gpAutoSetUp`)
Rests on [0013](0013-one-course-type.md) (a finish line is a thing you add, not
a mode you pick) and [0046](0046-two-samples-are-not-always-a-line.md) — whose
run flags are what stops this from inventing lap times.

## Context

Getting a lap time out of a recording has always required a track to exist
first: in the library, with a line on it, near where the car actually drove.

Most of that is already automatic. `gpAutoSetUp` runs after any load that
produced no laps — on download, per stint, and on every session open — and it
already does a lot:

- `gpMatchTrack` recognises the circuit from where the car spent its time,
  against the library first and then against `GP_PLACES`, a built-in list of
  **117 named circuits**, matching within `GP_MATCH_KM` = 3 km. It will mint a
  named track from that list without being asked.
- If the recognised track has no line, `gpProposeLine` picks the fastest,
  straightest point on the first loop and lays a gate across the direction of
  travel, and `gpAutoLine` checks the proposal against the recording it came
  from before keeping it.

So the remaining gap was narrow and specific: **a circuit in neither the
library nor those 117.** A private test track, an airfield, a skidpan, a kart
circuit, a club day somewhere obscure, anywhere outside Australia's well-known
venues. For those, `gpMatchTrack` returned null and the answer was *"add the
circuit in Tracks and the laps appear"* — which is precisely the setup the rest
of this machinery exists to remove.

And the recording already contains the proof. A drive that comes back on itself
and then does it again, at the same times, is a circuit session whatever the
place is called.

## Decision

**When nothing in the world is recognised, build the track out of the drive —
but only when the drive proves it, and never at the cost of the library.**

1. **Last resort only.** `gpTrackFromDrive` runs after `gpMatchTrack` returns
   null, never before it. A named circuit is always preferred, so this can
   never shadow a real one or mint a second copy of somewhere that has a name.

2. **The proof is repetition, not shape.** `gpProposeLine` already refuses a
   drive that never comes back on itself (`gpLoopClosure`: 400 m minimum, back
   within 30 m, pointing within 45° — so an out-and-back on the same road does
   not qualify). On top of that: **three laps minimum, times within 25% of each
   other.**

3. **Clean laps only, and this is the load-bearing rule.** `gpSplitRows`
   already grades what it returns, and a run measured across a clock hole is
   flagged. Counting raw laps instead of clean ones let a four-run recording —
   the same ground driven four times with a stop between each — mint itself a
   circuit and put lap times on the board. That is the Mount Barker failure
   rebuilt from the other end: a road drive advertising times nothing in the
   recording can stand behind. Caught by an existing harness, which is why the
   run flags were worth writing.

4. **The bar is higher than `gpAutoLine`'s** — 25% scatter and three laps,
   against its 35% and one. `gpAutoLine` is placing a line on a circuit
   somebody already named; this is putting a **new object in somebody's
   library**, and the two do not deserve the same confidence.

5. **Rejection is exact.** The candidate track is built in memory and made
   active so `gpSplitRows` can see it, and `gpTracksSave` is not called until
   it has earned its place. A rejected guess removes the track, restores the
   previous active id, and writes nothing — `rdm7_tracks_v1` is a hand-placed
   gate library and must come back exactly as it was found.

6. **Named so it is obviously provisional.** *"Unnamed circuit"*, numbered if
   there is already one, `note: "made from a recording"`. Two unnamed circuits
   are two places, not a collision. The sentence it reports says what to do:
   *"Made a circuit from this recording — 5 laps off a line on the fastest
   straight. Name it and check the line in Tracks."*

7. **It says so, every time.** Minting a track is a change to the library made
   without being asked, and the same rule already applied to turning a gate
   round applies here: it is always said out loud, including on the happy path.

## Consequences

- A club day at somewhere that is not on any list now produces lap times with
  no setup at all. That is the whole point.
- The library can grow an entry you did not ask for. It is named to invite a
  rename, it only happens when nothing else matched, and it takes three
  consistent clean laps to happen at all — but it is a real consequence and it
  is why the rollback and the sentence both exist.
- A stop-split recording still cannot become a circuit, and still says "runs"
  rather than "laps". Point 3 is what holds that line, and it holds it on a
  principle rather than on a threshold.
- `gpAutoLine` and `gpTrackFromDrive` now both place lines, by the same
  `gpProposeLine`, at different confidence bars. If that proposer improves,
  both improve; if it regresses, `check_autotrack` fails in two places.
- Nothing here reaches the firmware. The dash decodes what the puck broadcasts
  (0008's 2026-07-30 addendum); track identity is Studio's problem entirely.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` — `gpTrackFromDrive`,
  `gpUnnamedTrackName`, `GP_DRIVE_MIN_LAPS`, `GP_DRIVE_SCATTER`, and the
  fall-through in `gpAutoSetUp`; the existing `gpMatchTrack`, `gpProposeLine`,
  `gpAutoLine`, `gpLoopClosure`
- Tests: `rdm7-desktop/tools/check_autotrack.js` — 396 checks, including the
  two-lap and scattered-times rejections, the untouched-library rollback with
  a `setItem` counter, second-circuit naming, that a known circuit is still
  recognised by name rather than rebuilt, and that a drive with stops in it
  mints nothing
- Related ADRs: 0008, 0013, 0046, 0049
