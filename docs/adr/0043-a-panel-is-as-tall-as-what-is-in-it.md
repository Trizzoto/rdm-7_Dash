# ADR-0043: A panel is as tall as what is in it

Date: 2026-08-20
Status: Accepted
Repos: rdm7-desktop (GPS workspace, Analyse view)
Supersedes the height half of [0026](0026-analyse-is-a-mosaic.md); the panel
tree, the drag-to-place gesture and the brand-bar session line all stand.

## Context

ADR-0026 gave the Analyse mosaic a rule for height: divide the window between
the rows, and let a row fall no lower than `GP_ROW_MIN`. Below that floor the
page extends past the fold and scrolls. That answered "what happens when you
add a sixth panel", which was the question being asked at the time.

It did not answer what happens to a panel that is given less height than its
contents need, and the answer it had was the wrong one: the panel scrolls
inside itself. Four panels is therefore four scrollports plus the page, and
the wheel goes to whichever one is under the pointer, which is not reliably
the one you meant.

Worse than fiddly — it hides things silently. Opening the Donington session in
the shipped `QUAD` at 1280×690 gave the Lap times panel 223 px for 312 px of
list, so it opened parked on **Lap 2**: Lap 1 was above its own fold with
nothing on screen saying so. There is no visual difference between "this
session starts at lap 2" and "this list is scrolled", and the panel offered
none.

The same rule was quietly deciding how tall the rack could be. `gpFitLaneH`
divides whatever the panel was handed by the number of lanes, so the row split
chosen for the map decided how tall a speed trace was — and at six lanes in a
quad it was already clamped at its own 26 px floor.

Three defects surfaced while measuring it, all of them the same shape — the
bar being narrower than what was in it:

- **Arrange was unreachable.** It was the last child of `.gpb-sesbar`, which
  is `flex: 1 1 auto; overflow: hidden`. At 1400 px the circuit name and the
  tags overran the bar on their own, so the button rendered at x=1091 in a box
  that ended at 1067 and was clipped off entirely. Every preset, every saved
  arrangement and the fit rule itself were behind a control that could not be
  clicked at the width Studio is actually used at.
- **"Connect over USB" wrapped to three lines** inside a 30 px button and
  spilled out under a 50 px bar.
- **The circuit search box sat over the map's bottom-left corner** in every
  Analyse arrangement. Finding a circuit is how you set a track up; it has
  nothing to do with reading a lap you have already driven.

## Decision

**A panel that holds a document is measured, and gets exactly that height.
The page is the only thing that scrolls.**

`gpPanelWantH` asks each panel what it needs:

- a **document** (lap times, report, corners, splits, history) — its header
  plus where its content actually ends, capped at `GP_PANEL_MAX` (2400 px), past
  which even a document scrolls;
- the **rack** — all of its lanes at `GP_LANE_H`, the height below which a
  trace is a smudge, rather than whatever is left over;
- a **map or a loaded video** — about half its own width, since a picture has
  no natural height, only a shape;
- an **empty video panel** — the floor. A Load video card is a card, not a
  picture.

`gpNodeWantH` asks the same question of a subtree: a row wants its tallest
child, a column wants the sum plus its dividers.

**Content height is where the content ends, not `scrollHeight`.** At rest
`scrollHeight` *is* the box, so measuring with it alone would ratchet every
panel up to whatever height it last had and never let one come back down —
open a seven-lap session after a forty-lap one and the list would keep the
taller session's height forever. `gpFlowH` walks the children for the bottom
of the last one, and falls back to `scrollHeight` only while the box is
genuinely overflowing.

**Spare height goes to the panels that can use it.** A lap list handed another
80 px shows 80 px of nothing; a map shows more circuit and the rack shows
taller lanes. `gpSpineFill` distributes the window's surplus over the elastic
rows only — and takes a *shortfall* out of the same rows, up to `GP_SNAP`
(48 px) each, because a page that scrolls by thirteen pixels is a worse answer
than a rack whose lanes are thirteen pixels shorter. Past that the shortfall is
real and the page scrolls rather than squashing an instrument to hide it.

**`FIT | TALL` becomes `FULL | FIT`, and FULL is the default.** FULL is the
above. FIT is ADR-0026's behaviour, kept because packing everything into one
screen is a legitimate thing to want: the rows are re-fitted to the window and
whatever no longer has room scrolls on its own. `TALL` — every row given a flat
260 px up front — was a number nobody chose and is gone. Dragging a divider
between rows is the one gesture that contradicts auto height, so it is the one
that turns it off (`g.auto = false`); FULL hands it back. A horizontal drag, or
one inside a nested stack, changes nothing about how tall the rows are and
leaves it on.

**Panel headers are sticky.** The page is now the thing that scrolls, so a
panel taller than the window would otherwise slide its own name off the top and
leave you reading an unlabelled table. They sit at `z-index: 1010` — above
Leaflet's controls at 1000, or the map's + and − punch through the header as
they pass under it. `.gpb-splitmenu` was already losing that fight at
`z-index: 30` and moves to 1020.

And the three bar defects: **Arrange is a sibling of the session bar, not a
child of it**, so it cannot be clipped by a box that shrinks. Buttons and chips
in the top bar are `flex: none; white-space: nowrap`. Below 1580 px the
`GPS & LAP TIMING` brand label is dropped — the screen says ANALYSE across the
middle of it — and below 1400 px so are the spread tag, the ideal tag and the
date/driver/car line, because ellipsising only reads as ellipsising while a
word survives it: that line was rendering as the two characters `8.`, which
looks like a bug rather than a truncation. The circuit search is hidden inside
an Analyse panel and stays in Tracks and Live, where it is the point.

## Consequences

At 1280×690 — Studio at half a 3840 ultrawide, which is how it is actually
used — the Donington quad goes from *four panels, two of them scrolling
internally, one of them silently parked past its own first row* to: map and
rack 397 px, lap times and report 368 px, nothing scrolling inside anything,
and one 810/593 page scroll. At 1920×1000 the whole arrangement fits with no
scroll at all and the surplus goes to the map.

The map is smaller than it was in a quad (397 px against 476 px) because the
lap list beside it is no longer being clipped to pay for it. That trade is the
decision.

A panel can no longer be dragged shorter than its contents *while auto height
is on* — the drag turns it off, and then it can, and then it scrolls inside
itself again, which is the honest consequence of asking for it.

`gpGridLayout` now reads the DOM on every pass, including every `pointermove`
of a horizontal divider drag. Measured at 2.9 ms per move on the Donington
quad, inside a 16 ms frame with room to spare.

## Verification

`tools/check_gridfit.js` — 29 checks over the three pieces that are arithmetic
rather than DOM: `gpFlowH` (content shorter than its box measures the content,
not the box; taller measures the overflow; correct while scrolled away from the
top), `gpNodeWantH` (a row maxes, a column sums, nothing goes below the floor),
`gpNodeElastic` (a map yes, a lap list no, an empty video panel no, a loaded
one yes), and `gpSpineFill` (surplus to the elastic rows only, a sub-snap
shortfall absorbed, a real one left alone, the floor never breached, the input
never mutated).

In the running app over CDP, at 1280×690 and 1920×1000: every one of the eight
panel types placed in turn, each reporting `scrollHeight - clientHeight === 0`;
the page scrolling as one; headers sticking with the map's zoom control
overlapping and losing; a synthesised divider drag turning auto off and
de-lighting both segment buttons; and FULL putting it back.
