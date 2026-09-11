# ADR-0069: One source picker, and a decode editor you can see

Date: 2026-09-12
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`, `tools/mobile-dev-server.js`),
rdm7-desktop (`src/firmware-base.html`, `src/transport.js`)
Follows ADR-0030/0031 (Channels is one page), ADR-0033 (the catalogue travels
with the page), ADR-0035 (the source column speaks provenance).

## Context

A review of the Channels surface produced four complaints. Two were not the
bug they looked like, and separating them is most of this ADR.

**"The catalogue isn't populated — Generic Dash is missing, it just says Link
G4X."** The firmware was fine. `GET /api/channels/source-options` enumerates
the 196-row `preconfig_items[]`: Link ECU with Generic Dash (41 signals) and
AiM Stream (14), Haltech Nexus (69), MaxxECU (100), and six more. What was on
screen came from `DEV_ECU_MAKES` in `tools/mobile-dev-server.js` — a hand-typed
stand-in "trimmed to a few signals each" whose Link entry was one version
called "G4X" with five signals spread over `0x3E8`/`0x3E9`/`0x3EA`. That is the
pre-1.4.1 model of Link as consecutive ids, which the firmware fixed *because*
it made a Link undetectable and decoded garbage.

So the review was conducted against a mock that re-teaches a fixed bug, and
concluded the opposite of the truth. **A mock that can teach you a fixed bug is
worse than no mock.**

**"No option for new/custom."** True. The picker had no create action of any
kind, and its "Custom" bucket listed only signals that already existed. Worse,
offline it did not work at all: `_openSignalPicker` fetched source-options with
no fallback, and the desktop's local router had no route for that path, so it
reached the unmapped-endpoint throw and rendered "Failed to load sources" —
while the bulk-import picker one menu item away fell back to the baked
catalogue and listed every ECU we ship.

**"Too many spots for new source, new channel."** Three separate
ECU → Version → Signal drilldowns read the same catalogue (bind one channel,
bulk-import an ECU, and the Signal Manager's preset browser), plus the Assign
modal, Create Preset and the ECU-preset chip's modal — twelve ways to author a
source or channel across six surfaces. Three lots of state, search and
selection code that had already drifted, which is how only one of them ended up
with an offline fallback.

**"The side menu thing when you click a channel can be bigger."** It was a flat
`440px` on every screen. On a 1900px display that is under a quarter of the
width while the table beside it had room to spare, and it squeezed the decode
grid's `<select>`s to 89px, so they read "Unsigne" and "Little (In". The CAN
decode block also sat collapsed under **More**, beneath Label — the one block a
CAN owner edits, below the one field nobody does — and it had none of the
new-channel form's live bus view. You got the "is my bit offset right" answer
for the first ten seconds of a channel's life and never again.

## Decision

**The mock is derived, not typed.** `_devEcuMakes()` parses
`window.RDM_BAKED_CATALOG` out of the very HTML it serves. That block is
codegen output from the firmware's own tables, guarded by `--check` in
`schema-check.yml`, so the mock *is* the firmware catalogue and cannot drift
again. The dev server's `bind-source` and `import-preset` mocks now install the
real decode, frame gate included — the former did not exist at all and fell
through to a catch-all `{ok:true}`, so a bind appeared to succeed while
changing nothing.

**One picker, two modes.** `#chSourcePicker` opens in `bind` (single select →
`/api/channels/bind-source`) or `add` (multi select → `/api/channels/import-preset`,
which runs the firmware's own alias → canonical → custom resolution rather
than a second copy of it). Both load the same catalogue and both fall back to
the baked one, which is the point of merging them.

**A rail, not three columns.** Which ECU the car runs is a *car-level* fact —
nobody binds coolant to a Haltech and oil temp to a MaxxECU — so it is a narrow
rail grouped by the question you arrived with (Your ECU / Other ECUs / The car
(OBD2) / Custom & DBC / Built in), the version is a chip row, and the signals
get the room. Search is global: typing filters every source at once, so you can
find "coolant" without knowing whether your ECU calls it ECT, and hits print
their make and version because they no longer share one.

**"Type in a CAN decode…" lives in the picker.** In bind mode it hands the
channel you were already editing to `_chAddCanSignal`, which mints the signal,
binds it, and lands you on the decode editor. Creating a *second* channel there
would be the wrong answer to "where does this one come from".

**The decode editor is a section, not a disclosure**, sized
`clamp(520px, 40vw, 760px)`, on an `auto-fit minmax(150px, 1fr)` grid — three
columns wide, one on a phone, with no breakpoint to keep in sync and no select
narrow enough to truncate. It carries the live bus probe and the mux fields.

**One probe, two consumers.** The new-channel form and the decode editor ask
the same question, so they share one implementation that takes a decode source
and an element id. Answering it twice would be two chances to get the mux gate
wrong, which is the failure that renders as a confident wrong number.

## Two bugs the redesign exposed

Both were invisible under the old layout and obvious under this one, which is
the best argument for it.

**Every same-named row claimed "in use".** The endpoint's `is_current` is a
*name* match: it flags every preset row whose derived signal name equals the
channel's bound signal. Survivable while you could see one ECU at a time;
plainly wrong the moment search shows all of them, where nine makes each have a
COOLANT TEMP and all nine said "in use" on a car running one ECU. The decode
settles it — the channel owns the one that is installed (ADR-0005) — so
`_srcPRowIsCurrent()` compares frame, bit offset and frame index, and falls
back to the name match only inside the car's active ECU.

**The probe labelled native values with the display unit.** It decodes raw →
native, then printed `c.units_display`. A channel storing kPa and displaying
bar rendered a raw 416 kPa as "416.00 bar" — off by 100×, in the one readout
whose whole job is catching that class of error, and it would have been read as
a decode fault and "fixed" by changing a scale that was right.

## Consequences

- Roughly 700 lines of superseded picker code deleted; the three drilldowns
  become one. The Signal Manager keeps its preset browser but is renamed
  **Preset library** and reframed as authoring, because presenting it as a
  third picker is how it kept getting opened by people who wanted a source.
- `_ecuMakesFromBaked`, `_renderSrcPickerColumns`, `_renderEcuImportCols`,
  `_pickSourceOption` and their state are gone. Add new persisted fields to the
  surviving path, not to a copy.
- The desktop copy was merged three-way rather than patched, because it already
  carried this session's multiplexing work; all seven conflicts resolved toward
  the firmware side and 17 touched functions are byte-identical.
- **Not fixed, found on the way:** the editor drawer renders entirely
  off-screen at phone widths. It reproduces at the old fixed 440px, so it
  predates this work; measurements were taken in a scaled preview pane whose
  `innerWidth` disagreed with `clientWidth`, so it wants re-checking in a real
  browser before being chased.
- **Deliberately unchanged:** `import-preset` and `bind-source` themselves —
  both endpoints were right; the front end just reached them through too many
  doors. The ECU & CAN bus page keeps its own modal: it answers "is the dash
  talking to my car", which is a different question with live match scores.
