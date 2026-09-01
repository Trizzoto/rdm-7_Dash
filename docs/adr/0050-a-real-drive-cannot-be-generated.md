# ADR-0050: A real drive is the only fixture a synthetic one cannot replace

Date: 2026-09-01
Status: Proposed — decided and signed off 2026-09-01, not yet built
Repos: rdm7-desktop (`tools/`, `.github/workflows/`)
Rests on [0011](0011-analyzer-no-synthetic-data.md) — the product never
fabricates data; this says the *tests* cannot live on fabricated data either.
Plan: `rdm7-desktop/docs/briefs/02-golden-recordings.md`.

## Context

There are 34 harnesses in `tools/`, all green, and every one of them that
touches the analysis engine runs on data we generated.

`check_mallala.js` is 679 lines. Its fixture comes from
`tools/make_drift_fixture.js`, and every expectation comes from the
`.truth.json` written at generation time. It asserts, among 65 other things,
that the drift engine recovers a **planted 1.008 gyro scale and 0.42 °/s bias**
— and it does, exactly, every time.

The same engine, on the 23 August Mallala session, produced **+54° then −48°
inside one corner**. A generator cannot plant the fault it does not know about,
and synthetic data is precisely where this engine looks perfect.

The idea half-exists already, badly. Two harnesses do read real data, and they
disagree about what to do when it is missing:

- `check_laptime.js` reads a real Donington VBO out of a `Downloads` folder and
  `process.exit(2)` if it is absent — which `check_all.js` reports as a failure
  on every machine that is not his.
- `check_breaks.js` reads the real 22 August ring out of `Documents` and
  **silently skips** if it is absent — so it passes while testing nothing.

Neither file is in the repo. So the two most valuable tests in the suite either
die or quietly do nothing everywhere except one PC — and CI runs neither, because
CI does not run the harnesses at all. `frontend-merge-check.yml` re-implements
`check_syntax.js` as an inline one-liner and stops there.

## Decision

**Two or three real recordings are committed to the repo, with hand-blessed
answer sheets, and CI runs the harnesses.**

1. **A real drive is not regenerable, and that is the whole exception.** The
   `.gitignore` says regenerate-don't-commit, and that rule is right for
   `make_fixture.js` and `make_drift_fixture.js` output — nearly a megabyte
   that any checkout can rebuild byte-identically from a fixed seed. A drive
   cannot be rebuilt from a seed. It is the one class of fixture the rule does
   not cover.

2. **The cost is small in context.** `.git` is already ~101 MB, dominated by
   re-committing a 2.2–2.6 MB `tauri-overlay.html` across 197 revisions — 207 MB
   of blob history from one file. A few megabytes written once and never touched
   again are cheap beside that, and unlike the HTML they never produce a second
   revision.

3. **`tools/fixtures/`, gzipped, in whatever form its harness already reads.**
   `zlib` is in node and `check_breaks.js` already gunzips. Converting the 22
   August ring to `.rdmsession` would risk moving the sample count that harness
   pins exactly, for no benefit.

4. **The answer sheets are blessed by hand and never regenerated.**
   `make_expected.js` *prints* a sheet; `--bless` writes one, and on an existing
   sheet it must show a field-by-field diff first and refuse a silent
   overwrite. An expectation that regenerates itself on demand is not a test —
   the entire value is that a person looked at these numbers once and said yes.
   Re-blessing is a normal event (an intended engine improvement moves them) and
   should read like a decision in the commit log.

5. **Counts are exact; floats get a stated tolerance.** Sample count, lap count,
   `lapsBy`, break indices and corner counts are exact. Lap times ±1 ms. The
   fitted scale and bias ±0.001. Angle summaries ±0.5°. Donington against
   Circuit Tools 3 keeps `check_laptime.js`'s existing ±30 ms over the six
   flying laps.

6. **Failures print actual-vs-expected per field.** A golden test whose failure
   message is "3 failed" costs more than it saves.

7. **The external paths go.** `check_laptime.js` stops exiting 2;
   `check_breaks.js` loses its skip branch. Env-var overrides stay for working
   against a different file.

8. **CI runs `node tools/check_all.js`,** on a pinned node 22, replacing the
   inline re-implementation of `check_syntax.js`. Without this step the
   committed data is just files.

## Consequences

- The regression that motivated all of this becomes catchable: the 23 August
  Mallala recording's angle summary is pinned, and any engine change that moves
  it fails loudly instead of being noticed on a video months later.
- The suite stops being green-by-absence. Two harnesses currently report
  success or failure based on what is in one person's home directory.
- A few MB of immutable binary enters a repo whose stated rule is otherwise
  regenerate-don't-commit. Fixtures are never regenerated to make a test pass —
  a recording that needs replacing is a new file with a new name and a new sheet.
- Donington's `[comments]` block carries `(c) Racelogic`. It is a publicly
  distributed sample dataset, but that is a licence question and not a technical
  one, and it is put to him before it goes in. If the answer is no, the feature
  still works on his own two recordings and only loses the independent check —
  Donington is the one fixture whose expected lap times do not come from us.
- The u16 re-encode in `check_mallala.js:101-112` is not optional and must be
  repeated: a loaded session carries u16 and every reader decodes with the
  fitted `chanDefs`. A harness that feeds parsed floats straight in tests a path
  the app never takes.

## References
- Code: `rdm7-desktop/tools/check_all.js`, `check_mallala.js` (the working
  headless recipe, `:76-121`), `check_laptime.js`, `check_breaks.js`,
  `make_fixture.js`, `make_drift_fixture.js`
- New: `rdm7-desktop/tools/fixtures/`, `tools/make_expected.js`,
  `tools/check_golden.js`, `.github/workflows/frontend-merge-check.yml`
- Evidence: `rdm7-desktop/docs/IN_THE_CAR_2026-08-22.md` (the six breaks, by
  hand), `rdm7-desktop/docs/VIDEO_HUD_EXPORT_2026-08.md` (the slip-angle
  investigation)
- Related ADRs: 0011, 0046, 0049
