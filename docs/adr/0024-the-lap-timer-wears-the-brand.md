# ADR-0024: The lap timer wears the brand — Industry light, RDM red, six flat views

Date: 2026-08-08
Status: Accepted
Repos: rdm7-desktop (GPS workspace only — `src/tauri-overlay.html`, `tools/merge_overlay.py` asset list)

## Context

The GPS lap timer workspace grew inside Studio's dark editor chrome: a
`ws-modes` segment with Monitor / Tracks / Session / Setup, a rail that
holds a docked map plus every analysis panel in one 252 px column, and
canvas plots tuned for a near-black ground. It works, but it looks like a
diagnostics tool, and the owner has been designing what it should look
like instead: the claude.ai/design project "RDM Studio Redesign", turns
2–4. Turn 2 applies the brand (black header bar, RDM red as the single
accent, light "Industry" ground, Barlow type). Turn 3 explores working
layouts on that ground (map-first, engineer's stack, A/B built in). Turn 4
is function-first: a corner report that ranks every corner by time lost
and says what to change, in plain words. VBOX Circuit Tools is the
usability reference throughout: open a session, see laps, delta, map,
graph — nothing to configure first.

The instruction for this pass: redesign the whole GPS lap timer menu to
follow turns 2, 3 and 4 — function and style — with ease of use paramount.

## Decision

**Scope: the GPS workspace only.** Everything inside `#gpWorkspace` moves
to the design language; the dash layout editor, keypad, analyzer and IO
workspaces keep the dark chrome. All new CSS is scoped under
`#gpWorkspace` (the `ws-*` classes are shared with the other workspaces
and must not change globally). Authored in `src/tauri-overlay.html` per
STUDIO_SHELL_PLAN §2.0 — Studio owns this surface.

**The ground goes light, the bar goes black.** Inside the workspace:
Industry tokens (`#f2f2f3` ground, `#1d1f20` ink, hairline dividers at
16 % ink, square corners, transparent cards with 1 px borders). The top
bar becomes the brand bar: `#0c0d0e`, RDM logo, uppercase Barlow
Condensed nav, active view marked by a 3 px red underline — the accent
marks the active thing, it does not paint it (ADR-0013 held). RDM red
`#d2232a` (with a tonal ramp) is the only accent, spent only where it
means something: best lap, active state, time lost, REC.

**Type: Barlow + Barlow Condensed, bundled.** `src/fonts/*.woff2`
(latin), `fonts` added to `ASSET_DIRS`, `@font-face` in the workspace
CSS. Bundled rather than Google-hosted because the laptop at the track
has no internet, and the rest of Studio already degrades silently
offline. Numbers set tabular (`font-variant-numeric: tabular-nums`).

**Six flat views instead of four.** The nav reads
`Sessions · Live · Analyse · Corners · Tracks · Setup`:

- **Sessions** (new view) — the landing screen, turn 1c's function under
  turn 2's style: every saved recording as a table row (date, track,
  driver, car, laps, best, data badges), search, per-track personal best.
  One click opens Analyse. When a node is linked, the download action
  lives here too.
- **Live** — Monitor renamed. Same readiness cards, same live map,
  restyled. "Live" says what it is; "Monitor" said what it does.
- **Analyse** — Session renamed and re-laid per 2a/3b: laps rail on the
  left (lap cards with Δ vs best and a pace bar), map above the channel
  rack in the centre (the map comes back out of the rail), and the
  report on the right in the re-opened inspector column: sector table,
  stat tiles, "where the lap lost time", coach. The rack, navigator,
  transport, video, ghosts, channels popover all stay — repainted on a
  light well with an ink/red palette.
- **Corners** (new view) — turn 4a made real: every corner of the
  selected lap ranked by time lost against the reference, min speed
  against best, brake point in metres, an opportunity bar, and one plain
  sentence about what to change (`gpPrescribe` already writes it). A
  numbered map alongside, badges coloured by loss. Clicking a corner
  shows its phase table (Braking / Entry / Apex / Exit — all existing
  analysis) and jumps the graph and map to it. No new analysis engine:
  this is `gpFindCorners` / `gpCompareLaps` / `gpCoachInsights` given the
  front door they earned.
- **Tracks / Setup** — unchanged in function, restyled.

**Comparison stays inside Analyse and Corners, per turn 4.** Turn 2/3
showed a separate Compare screen; turn 4 (the latest) folded comparison
into the work itself — "Analyse L3 against Session best" as a picker
strip. The existing compare-lap and ghost-from-another-day machinery
already works exactly that way, so the A/B strip (3c style) renders at
the top of Analyse rather than as a seventh view.

**Canvas palettes become themable.** The rack, navigator and video
overlay read colours from one `GP_INK` table instead of literals: light
well, ink structure lines, red for the selected lap's delta and for
loss; channel hues re-tuned for a light ground. Map data ramps
(pace/speed/ref) keep their meaning; the flat "no imagery" field goes
light. What is drawn is still what was measured (ADR-0011 untouched).

## What this deliberately does not do

- No light theme for the rest of Studio, and no theme toggle. One
  workspace, one look.
- No workbook tabs (4c), no distance x-axis, no mixed sub-plots — the
  earlier redesign doc already deferred those; nothing here blocks them.
- No 4b literal two-line corner drawing. The map already draws both
  laps' real GPS lines; the corner detail view frames them and adds the
  phase numbers. Synthesising an idealised corner diagram would break
  ADR-0011.
- No data-format changes. IndexedDB stores, `.rdmsession` v1, track
  shapes, gate semantics, `meta.chanIds` — all untouched (the
  compatibility list in the 2026-08 workspace map holds).

## Consequences

- The GPS workspace stops inheriting future `ws-*` styling changes made
  for the other workspaces — accepted; the shared chrome was four rules
  deep and the divergence is the point.
- `gpSetView` gains two view names; anything that normalises unknown
  views to `monitor` now normalises to `sessions` when recordings exist.
- The rail no longer hosts the docked map in Analyse; `gpDockViewer`
  keeps the capability for narrow windows but the default is map-in-stage.
- Barlow adds ~156 KB of woff2 to the installer and to `src/dist/`.
