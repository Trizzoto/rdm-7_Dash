# ADR-0014: Workspace design language — chrome is monochrome, colour is data

Status: accepted, 2026-07-29. Applied to the shared workspace chrome in RDM
Studio (`src/tauri-overlay.html`, the `.ws-*` / `.gp-*` layer used by the GPS,
keypad, IO and analyzer workspaces).

## Context

Direct feedback on the GPS workspace: *"the design feels very gimmicky…
we're building proper motorsport products now."* Measured against the tools
our customers already respect — MoTeC i2, McLaren ATLAS, and the NLE
transports (Resolve/Premiere) the playback model borrows from — the
observation was right, and the flash was all in the *chrome*: four
solid-accent button fills visible in one top bar, a glowing halo on every
healthy status dot, a drop-shadowed floating transport, an accent bar on
every informational note.

What those professional tools share, consistently:

- **Chrome is monochrome.** Panels, buttons, tabs and rails live in greys.
  The eye is spent on the data, never on the container.
- **Colour belongs to data and state.** i2 colours its channel traces
  saturately *because they are the data*; its chrome stays grey. Green/amber/
  red appear only as state, and quietly.
- **One accent, used as a mark.** Selection is an underline or a side bar on
  a neutral surface — never a filled pill. A fill is a primary action, and
  there is at most one of those per surface.
- **Nothing glows, nothing floats.** Hairline borders, flat surfaces, no
  halos; shadows only where something genuinely overlays (tooltips, menus).
- **Density over decoration.** Small caps micro-labels, tight paddings,
  monospace for numbers.

## Decision

One principle, enforced across the shared chrome: **chrome is monochrome;
colour belongs to data and state; the single accent marks the active thing
rather than painting it.**

Concretely:

1. **Tabs and segmented controls** (workspace views, unit toggle, map modes,
   lanes toggle): active = elevated neutral background + 2 px accent
   underline (`inset 0 -2px 0 var(--accent)`). Never a solid fill.
2. **List selection** (rails): 2 px accent *left* bar on an elevated neutral
   row. Never an accent outline.
3. **State colours are calm.** Ok `#5d8a62` (dots) / `#7da982` (text), warn
   `#c78f2e` / `#d99a3d`, bad `#d16358`. No halo rings on lamps. A healthy
   system reads as *quiet*, not as a light show.
4. **Notes are neutral** (grey left bar). A coloured bar on a note means
   something is actually wrong or worth attention (amber hint strip stays).
5. **No decorative shadows.** The sticky transport sits flat; tooltips keep
   theirs (they genuinely float).
6. **One filled primary per panel, at most** ("Connect over USB",
   "Download from node"). Everything else is a quiet outline button.
7. **Data keeps its colour.** Lane traces stay saturated per channel (that
   is the i2 idiom precisely), purple stays the session-fastest convention
   (broadcast timing standard), delta green/red stays semantic.

Tokens are unchanged — `--accent`, `--panel*`, `--border*`, 2/4 px radii were
already restrained; this ADR governs *how* they are used.

## Consequences

- Every future workspace inherits the rules by using the shared chrome
  classes; deviations need a reason stated in review.
- "Too flashy" now has an objective test: is any colour on screen neither
  data nor state? Is the accent filling anything other than the one primary
  action? Does anything glow?
- The RDM red stays in the brand block only — it is identity, not chrome.

## Sources

- [MoTeC i2 overview](https://www.motec.com.au/i2/i2overview/) and
  [feature set](https://www.motec.com.au/i2/i2features/) — the reference
  point for pro analysis UI: customisable, dense, chrome-neutral.
- McLaren ATLAS (via its
  [i2 import path](https://www.motec.com.au/i2/i2downloads/)) — same
  school: data saturated, container invisible.
- DaVinci Resolve / Premiere transport conventions — the J/K/L and monitor
  tiling model the Session view already follows.
