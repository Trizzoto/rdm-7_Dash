# ADR-0025: Analyse is a grid you arrange, not a layout we chose

Date: 2026-08-09
Status: Accepted
Repos: rdm7-desktop (GPS workspace, Analyse view)

## Context

ADR-0024 gave Analyse a fixed shape: lap rail on the left, map above the
channel rack in the middle, report column on the right. It reads well and
it is the right default, but it is one answer to a question that has
several. Watching video against a lap wants the video big and the rack
small. Reading split times wants a table and nothing else. Comparing four
laps on one graph wants the graph full width. Every one of those is a
different arrangement of the same handful of panels, and a fixed layout
can only be right for one of them.

The owner's design (claude.ai/design "Analyse Workspace") answers it
directly: rows of panels, each panel a dropdown away from being something
else, drag the borders to resize, save the arrangements you keep coming
back to. Plus a bottom dock whose readouts you choose, and — the
functional change underneath the layout work — **more than two laps drawn
at once**.

## Decision

**The Analyse stage becomes a grid.** Rows of panels; each panel carries
a type dropdown, a duplicate and a close. Up to three panels per row,
unlimited rows in practice. Column borders and row borders drag to
resize. Four presets (Quad / Split / Video / Solo) and a row-emphasis
control (Top / Even / Bottom) for the common shapes; anything else is a
drag away. Arrangements can be named and saved, and are listed for
re-apply, rename and delete.

Panel types: **Track map, Graph, Video, Lap times, Lap report, Where the
time went, Splits.**

**The rail and the inspector go.** Their contents became panels: the lap
list is the Lap times panel, the report column is the Lap report panel.
A view whose whole job is arrangeable panels should not also carry two
fixed columns of the same material.

**Three panel types host a singleton and therefore cannot be duplicated:**
Track map (one Leaflet instance owning every layer), Graph (one rack
canvas), Video (one `<video>` element and one file). Rather than
disabling them — which leaves the user stuck when the panel they want is
occupied — choosing an already-placed singleton type **swaps**: the panel
that had it takes the type this one was showing. Predictable, and never
a dead end.

**Ticked laps draw; the analysed pair still drives the analysis.** The
design's Lap times panel ticks laps on and off and says "ticked laps draw
on every graph and map panel". That is a genuine capability change — the
workspace has drawn at most two laps since it existed. But every
computation underneath (the delta series, the coach, the corner report,
the split grid, sector chips) is defined as one lap **against** one
reference, and inventing an N-way version of "time lost" would be a much
larger change than a layout redesign should carry.

So the two ideas are kept separate and both are honest:
- `gp.shownLaps` — the set that gets DRAWN on graph and map panels, each
  in its own colour.
- `gp.selLap` / `gp.cmpLap` — the pair that is ANALYSED, unchanged.

Clicking a lap's name analyses it; clicking its REF tag makes it the
reference; the tick only ever controls drawing. The panels say which is
which rather than leaving it to be inferred.

**The transport moves to a dock along the bottom**, always showing the
play controls and the scrub, with everything else — lap, position,
delta, speed, RPM, throttle, rate — toggled from a right-click menu and
remembered. The at-cursor numbers stop being a card floating over the
map and become chrome, which is where a value you read continuously
belongs.

**Recording moves out of Analyse.** The readiness checklist and the
Download / Watch live / Start recording actions lived in the Analyse
inspector, which no longer exists. They belong to the views that are
about the device anyway: the checklist and the recording controls go to
**Live**, and downloading stays in **Sessions**.

## What this deliberately does not do

- No free-form drag-and-drop of panels between rows. Rows plus resize
  covers the arrangements people actually described; a full tiling
  manager is a different product.
- No per-panel lap selection except for Video (which is genuinely about
  one lap's footage). Every other panel reads the shared selection, so
  two panels can never disagree about what is being analysed.
- No N-way delta or N-way coaching. See above.
- No change to the stored data. Sessions, tracks, `.rdmsession`, the
  channel model — untouched. The grid is UI state in its own keys.

## Consequences

- `rdm7_gp_grid` (current arrangement), `rdm7_gp_grids` (saved ones) and
  `rdm7_gp_dock` join the workspace's localStorage keys. All three are
  presentation-only: losing them costs a layout, never data.
- The map, rack and video elements are re-parented into panels, the same
  mechanism `gpDockViewer` already used to move the map between the rail
  and the stage. Leaflet and the canvases both need a re-measure after a
  move, which the existing code already does.
- Anything that assumed Analyse has a rail or an inspector must be
  re-pointed. `gpRenderInspector` keeps its other three views.
