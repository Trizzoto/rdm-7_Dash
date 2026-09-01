# ADR-0049: A recording says how much to trust it

Date: 2026-09-01
Status: Proposed — decided and signed off 2026-09-01, not yet built
Repos: rdm7-desktop (GPS workspace)
Rests on [0011](0011-analyzer-no-synthetic-data.md) (never fabricate, name the
cause), [0044](0044-a-recording-carries-its-own-meaning.md) and
[0046](0046-two-samples-are-not-always-a-line.md) — this is the panel that says
out loud what those two learned to detect.
Plan: `rdm7-desktop/docs/briefs/01-trust-panel.md`.

## Context

On the 23 August Mallala session the slip angle read **+54° then −48° inside
one corner, wearing a confident ±2.1°**, on footage that shows the car tracking
the racing line. Finding that took a day of bisection.

Every signal needed to suspect it was already being computed. GNSS breaks
(0046), quiet CAN channels, the drift fit's own scale and sample count, anchor
counts, leg closure error, run flags, the recording's own GPS position noise —
all of it derived, most of it invisible, and what was visible was scattered
across four surfaces. Two of those numbers had *never* been shown anywhere:
`gpSmoothNoise`'s sigma, and the download's count of holes it stepped over.

The feature list that came out of that incident ranked a **per-car calibration
run** first: a guided routine — straight line, slalom, steady corners —
measuring the gyro's scale and mounting offset against GPS and storing them
against the car. His own note on it was a question: *"is this necessary, can we
just get it from normal driving?"*

It is not necessary, and the code already answers the question.
`gpDriftAngle()` self-calibrates from ordinary driving on every session:

- It fits scale and bias by regressing the GPS-derived course rate against the
  smoothed body rate **the inverse way round** — `rp` on `rb`, then inverted —
  because least-squares the naive way biases the slope toward zero. Measured: a
  true 1.008 scale came back as **0.964**, turning a 38° corner into 41°.
- It refuses samples where the car is *holding* an angle. A steady drift has
  β̇≈0 and would otherwise dominate the fit: measured **0.936** against a true
  1.008 on a real Mallala drift lap. Two-pass masking, grip driving only,
  confirmed with a leaky integrator that forgets slow bias.
- A fitted scale outside **0.8–1.25** is refused outright as wrong units, wrong
  signal or mirrored mounting — not "corrected".

And the failure that prompted the ranking was not a scale error. The leg closed
perfectly at *both* anchors and was wrong in the middle, because anchor closure
only measures error where the car is provably going straight. A calibration
wizard would have found nothing wrong.

## Decision

**The recording says how much to trust it, in one panel, and the drift fit
stops keeping its own diagnostics to itself.**

1. **One panel in Analyse, `GP_PTYPES` id `health`.** Rows are the Ready card's
   shape — `{k, v, tone, sub, fix, wait, pin}`, each carrying its own verdict
   and optionally its own fix button — and the verdict over them is
   **worst-wins**, not a score. A score invites arguing with the number; a
   worst-wins verdict points at the row.

2. **One line, then only what is wrong.** The same rule the Ready card already
   states: *"nine rows of mostly-fine was a panel you had to read to learn
   nothing."* Expanded shows everything that was checked.

3. **The verdict function is copied, not shared.** `gpReadyVerdict`'s `wait`
   tone means "not yet known" and has no meaning for a finished recording;
   sharing the function would drag that concept in. The Ready card also only
   renders with a connected node, which is exactly the guard a session-health
   card must not inherit.

4. **Item 1 becomes transparency, not a wizard.** The per-session fit's
   `scale`, `bias`, `fitN`, `weak`, `anchors` and `worst` are shown — as
   expanded detail when the row is fine, as the verdict when it is not. A
   `weak` fit says what kind of driving would fix it ("a few corners driven on
   the limit without sliding") rather than offering a mode to switch to. This
   reverses the ranking those eight features arrived in, and was put to him
   explicitly rather than reordered quietly.

5. **Plain language, one fact per column.** No `weak`, no `fitN`, no σ on
   screen. "The scale could not be fitted", "measured from 4 100 samples",
   "typically ±1.4°".

6. **A badge on the Sessions list needs the verdict persisted.** The panel can
   derive everything from a loaded trace; the list reads metadata only and must
   stay that way. So `gpSessionMeta` gains `health: {tone, n}` and
   `ring: {wrapped, dropped, holes}`, recomputed by the same heal block that
   already rewrites `lapCount`/`bestLapS`/`corners` on reopen, and mirrored into
   the in-memory session list the same way — the trend reads `gp.sessions`, not
   the store.

7. **Absence is not health.** `gpMarkBreaks` reverts its own marking wholesale
   past 2% of steps (0046 §7), so "no breaks" can mean "the test did not apply
   to this import". That case gets its own wording. Old recordings with no
   `health` field badge nothing rather than badging green.

## Consequences

- The 23 August session would have been visibly suspect in seconds instead of a
  day: a weak-or-out-of-band scale row, an anchor count of zero on the leg, and
  a worst-leg error far outside the typical ±.
- Three numbers become visible for the first time — the recording's own GPS
  noise sigma, the download's hole count, and the whole drift fit. All three
  were already being computed and thrown away.
- `wrapped` and `dropped` stop being lost at save. They exist only while the
  puck is attached today; `gpSessionLoad` overwrites `gp.traceInfo` with a
  stub, so surfacing them post hoc requires copying them into the meta at
  download time. Recordings already saved cannot recover them.
- Two new meta fields on every future recording. Both are small on purpose:
  `gpStore.list()` deserialises every meta for the rail and the Sessions table.
- **Item 1b survives, deferred.** The gap the per-session fit cannot close is
  *mounting* — is Z really vertical, is the sign right, is the puck bolted in
  rotated — because the 0.8–1.25 refusal band papers that over by falling back
  to `scale = 1` rather than diagnosing it. The firmware already has the pieces
  (`imu_cal_t.dir[3]`, `imu_cal_valid`'s handedness check, a written stationary
  bias estimator in `imu_cal_acc_finish`), and `trace_log.h`'s own comment
  pre-authorises growing the sample struct past yaw-only. But it is a firmware
  change with a magic bump, and it is worth less than showing the number we
  already have.
- The panel is a diagnosis, not a repair. Nothing here corrects a recording;
  0045 owns that.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` — `gpDriftAngle` (the fit and its
  refusals), `gpMarkBreaks`/`gpRunBreakM`, `gpChanQuiet`, `gpSmoothNoise`,
  `gpNoLapsWhy`, `gpVideoCover`, `gpReadyRows`/`gpReadyVerdict`/`gpReadyCardHtml`
  (the shape being copied), `gpSessionMeta` and the `gpSessionLoad` heal block
- Tests: `rdm7-desktop/tools/check_health.js` (new) — including that a break
  absent because the marking self-reverted reads differently from a clean trace
- Evidence: `rdm7-desktop/docs/VIDEO_HUD_EXPORT_2026-08.md` (the slip-angle
  investigation), `rdm7-desktop/docs/STUDIO_IDEAS_2026-08.md` §1–2
- Related ADRs: 0011, 0044, 0045, 0046, 0050
