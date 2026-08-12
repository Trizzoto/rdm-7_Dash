# ADR-0026: Analyse is a mosaic, and it may be taller than the window

Date: 2026-08-09
Status: Accepted
Repos: rdm7-desktop (GPS workspace, Analyse view)

> Written up retroactively on 2026-08-10 from the shipped implementation, its
> in-file commentary, and the status block in
> `rdm7-desktop/docs/LAP_ANALYSIS_REDESIGN_2026-07.md`. The number was already
> claimed by that code (`ADR-0026` appears in five places in
> `src/tauri-overlay.html`); this file is the record catching up to it, not a
> new decision.

## Context

ADR-0025 made Analyse a grid of panels you arrange. Four things were wrong
with it once it was full of real panels.

**It was giving a fifth of the window to chrome.** A 34 px band said "No RDM
GPS connected" — true, and irrelevant to reading a recording already on this
PC. A 41 px band under it carried the track name, the session line, the tags
and four arrangement controls. Add the add-a-row banner (35 px) and the
per-row add-a-panel column, and a quad arrangement in an 800 px window gave
each panel 223 px.

**Rows always won.** The model was strictly row-major: a row spanned the full
width, and a column could only ever be as tall as the row it was in. So "a map
down the left with a graph and the lap times stacked beside it" — an ordinary
thing to want — was not expressible at all.

**Panels could be re-typed but not re-placed.** Getting the video from the
bottom row to the top meant changing two dropdowns and hoping. There was no
gesture that moved a panel out of one row and into another.

**There was nowhere to put a fifth panel.** Adding a row divided the same
window height further, and the honest answer to "compress or scroll" had never
been given.

## Decision

**The arrangement is a tree, not rows.** A node is either a panel or a split
of other nodes:

```
{ id, type }                          a panel
{ id, dir: 'row'|'col', kids, sz }    a split
```

`sz[i]` is the size of kid i *along* `dir`, and every child — panel or nested
split — renders as `flex: sz[i] 1 0%`. That one rule is why the same numbers
work both ways: flex-grow divides whatever height the box has in proportion to
them, so when the box is exactly their sum they **are** pixels, and when it is
not they are ratios. Fitting the window and scrolling past it become the same
layout with a different box height, rather than two modes.

**The root is always a `col`** (`gpRootCol`), because the vertical spine is the
only one allowed to outgrow the window, and keeping it uniform makes "add a row
at the bottom" one operation instead of four cases.

**Every panel carries Split left/right and Split top/bottom**, and a `TOWER`
preset ships the arrangement that could not be drawn before:
`["col", 1, ["row", 1.15, "map", 0.85, ["col", 1, "graph", 1, "times"]]]`.
Presets are QUAD / SPLIT / TOWER (plus `video` and `solo` in `GP_PRESETS`).

**Panels are dragged by their header.** Drop on the middle of another panel
(within 22% of centre on both axes) to swap the two; drop on an edge to put the
panel on that side of it — which is the only route out of one row and into
another. The removal happens *before* the insertion, or a drop inside the
source's own parent sees the source twice and places the copy against a stale
index. Held within 44 px of the top or bottom of a scrolling grid, the drag
scrolls it, since the destination is exactly what may be off screen. Escape, a
lost pointer (`pointercancel`) and a window `blur` all cancel — each of those
also has to kill the edge-scroll timer, or the grid creeps on its own with
nothing holding it.

**When something would fall below the floor, the page extends and scrolls —
it does not squeeze.** Panels have a floor (`GP_ROW_MIN` 150 px, `GP_COL_MIN`
180 px), and `gpNodeMinH` gives a subtree its true minimum: a row is only as
short as its *tallest* child, while a column needs the *sum* of its children
plus the gaps between them. So a stack of three panels beside a map makes that
row three panels tall, rather than dividing one panel's worth of height three
ways and calling each result a panel. Dragging a divider takes height from the
next row, and from the one after it when that one is already at its floor, all
the way down; past the bottom the page grows. Drag back up and it sheds the
overflow first, so it snaps back to fitting.

`gpGridLayout` is the single measurement pass and **the only place that decides
whether the arrangement fits or outgrows the window**. `g.fit` is derived
there, never set as a mode: FIT and SCROLL are two ways of asking for a height.
The grid element (`.gpb-grid`) is the scrollport rather than the stage, because
the bottom dock has to stay pinned while the panels scroll past it and it is a
sibling of the grid, not a child.

**The chrome moves into the brand bar's slack, which was empty.** The session
facts — track name, date/driver/car, GPS rate and CAN channel count, and the
Best / Ideal / Spread tags — render into `#gpCtx` inside ADR-0024's black bar.
The four arrangement controls collapse behind one **Arrange** button opening
`#gpArrPop`, because they are used when you set the view up and never again
while reading it. The connection hint is suppressed in Analyse. Net: **88 px
and a column back** to the panels — each panel in a quad goes from 223 px to
329 px at 800 px tall.

**v1 arrangements convert on read.** `gpGridAdopt` accepts a v2 mosaic, an
ADR-0025 row-major arrangement (`gpGridFromRows`, an exact equivalent —
nothing is approximated), or nothing. Only *nothing* is allowed to silently
become the default; an arrangement someone built is worth converting, and
losing it without a word is the failure mode that guard exists to avoid. The
same path runs for live state and for saved layouts, so `gpLayoutApply` needs
no special case.

### Three rack bugs went with it

- **Combined drew only the analysed lap.** Ticking two laps and switching to
  Combined silently dropped one. The other ticked laps now draw dashed on the
  same scale.
- **Combined printed no magnitudes at all**, and its gutter legend dropped any
  channel whose row fell past the bottom of a short panel — which is why RPM
  could be ticked and nowhere to be found. The legend now lays out to fit and
  carries each channel's own scale.
- **Stacked drew the analysed lap in the channel's colour while the legend gave
  it a lap colour**, so Lap 1's swatch was dark red and its speed trace was
  black. With more than one lap on the rack, colour means the lap, and the
  legend says which rule is in force.

## What this deliberately does not do

- **No fit/scroll mode flag.** `g.fit` is a measurement result. A flag here
  would be a way to keep both behaviours wrong independently.
- **No change to stored data.** Sessions, tracks, `.rdmsession`, the channel
  model — untouched. The mosaic is UI state in `rdm7_gp_grid`,
  `rdm7_gp_grids` and `rdm7_gp_dock`; losing all three costs a layout, never
  data.
- **No discarding of v1 arrangements.** Converting is cheap; the alternative
  is a user opening Analyse to find their layout replaced by the default with
  no explanation.
- **No change to what is measured or drawn.** Lap times, deltas and the coach
  still read raw fixes (ADR-0011). This is arrangement only.

## Consequences

- `rdm7_gp_grid` gains `v: 2` and a `root` tree; anything reading that key
  directly must go through `gpGridAdopt` rather than assuming `rows`.
- Every drag exit path — `pointerup`, `pointercancel`, `blur`, Escape — must
  route through the same teardown. Three of the four are failure paths that
  never fire in normal use, which is exactly why they were the ones that leaked
  a running timer.
- The GPS brand bar now carries data, not just chrome. Anything that assumed
  the bar's contents are static has to account for `#gpCtx` re-rendering on
  session, lap and view changes.
- Nested stacks must settle (`gpColBalance`) before the spine is measured
  against them, because a row's minimum depends on the shares inside it. The
  surplus taken always equals the shortfall handed out, so it settles in one
  pass — but the ordering inside `gpGridLayout` is load-bearing.
- ADR-0025's "no free-form drag-and-drop of panels between rows" is
  superseded by this ADR. Rows-plus-resize turned out not to cover the
  arrangements people described.

## References

- ADR-0024 (the brand bar this reclaims space from), ADR-0025 (the row-major
  grid this replaces), ADR-0011 (measured data untouched), ADR-0014 (the
  design language the Arrange button obeys)
- Code: `../rdm7-desktop/src/tauri-overlay.html` — `GP_PRESETS`,
  `gpNodeFromSpec`, `gpGridAdopt` / `gpGridFromRows`, `gpNodeNormalise`,
  `gpNodeMinH` / `gpColBalance`, `gpGridLayout` / `gpSpineToPx`,
  `gpDragPanel` / `gpPanelSwap` / `gpPanelMoveTo`, `gpRenderCtx` /
  `gpArrPopRender`
- `../rdm7-desktop/docs/LAP_ANALYSIS_REDESIGN_2026-07.md` §"ADR-0026,
  2026-08-09"
