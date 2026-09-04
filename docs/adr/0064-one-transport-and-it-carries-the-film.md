# ADR-0064: One transport, and it carries the film

Date: 2026-09-04
Status: Accepted — built 2026-09-04
Repos: rdm7-desktop (`src/tauri-overlay.html`, `tools/check_lookbar.js`,
`tools/design/footage-timeline.html`)
Follows: ADR-0053 (footage arrives in sections), ADR-0061 (the working panel
and the bar that follows it)

## Context

Analyse could have **three transports on screen at once**, and which one you
were looking at depended on what you had last clicked.

| Where | What it was | When it appeared |
|---|---|---|
| `gpRenderDock` | five buttons, a scrub, six readouts | always |
| the same bar, `ctx === "video"` | the six readouts **replaced** by a compact timeline | when the working panel was the video |
| `gpPanelVideoHtml` | a second copy of that timeline | inside the video panel's control row, whenever there was more than one section |
| `.gpb-vidxport` | a **third** transport — its own play, its own scrub, its own step buttons | the moment you took the picture off the playhead |

The bar changing shape under you was already known to be wrong: ADR-0061
shipped `dock: true` and it was reversed to `false` within days, with the
reason written into `gpFocusOpt` — *"the transport you press changed shape
depending on which panel you had last clicked."* Turning the feature off made
the bar stable again and left the actual problem in place: the bar said nothing
about the footage, so the timeline had to be found somewhere else.

The timeline itself had two forms out of one renderer, and they had drifted
into two different products:

- **compact** — 16 px lanes, no toolbar at all. A read-only ribbon.
- **panel** — 30 px lanes under a toolbar carrying **twelve** controls, seven of
  which were the nudge cluster. The nudge acted on `gp.video` — whichever
  section the picture happened to be showing — so with three sections open you
  could not tell what you were about to nudge until you had nudged it.

And everywhere: lanes with no names, a section starting before the recording
drawn off the left edge with nothing to say it was there, and four colours for
a clip — steel blue, green for the live one, pink for the picked one, red-brown
for one that would not decode — with nothing on screen saying what any of them
meant.

## The study

Five shapes were drawn as a working prototype
(`tools/design/footage-timeline.html`) and driven against a generated day built
to be awkward: twelve laps with a pit stop in the middle, a lap that was slower
for traffic, four files with a five-second gap between two of them,
overlapping cameras through the middle, an uncovered tail, and one file that
will not decode. Two of the five were chosen and are the defaults.

| | Shape | Bar | Panel |
|---|---|---|---|
| 1 | **One bar** | 75 px | — |
| 2 | **Track sheet** | 51 px | 246 px |
| 3 | Filmstrip jog | 116 px | — |
| 4 | One ruler | 51 px | 172 px |
| 5 | Chapters | 61 px | — |

## Decision

**There is one transport, and it carries the film.** The bottom bar IS the
scrubber — there is no second slider that can hold a different opinion about
where the playhead is — and the panel is the workbench where footage is
arranged.

**Both shapes are chosen, in Setup → Analyse.** Which one is right depends on
whether you spend your time editing footage or watching it, and that is not a
question this file gets to answer for everybody. Defaults: **One bar** and
**Track sheet**. `Readouts` and `Lanes` are the old shapes, kept.

### One bar (the default bottom bar)

One surface, three bands, always the whole day:

- laps across the top, named and numbered, each one a destination
- the recording as a waveform, **lit where there is film and grey where there
  is not** — the picture is the answer to "does this lap have footage", and it
  was previously a sentence about seconds
- a 4 px ribbon per camera along the bottom, draggable, so lining a section up
  and watching the day are the same surface

### Track sheet (the default panel)

An editor, with the two things every editor has and this one did not:

- **a named gutter down the left.** A lane holding one file is named by that
  file; a lane holding several is "Lane 2 · 3 sections"; the spare lane says
  "+ drop a file". Its rows mirror the flex rules on the right exactly, so they
  cannot drift out of line as the panel resizes.
- **an inspector for one section**, carrying everything that acts on one
  section — the nudge cluster, the sync source, Remove — and naming it. The
  toolbar drops from twelve controls to four.
- **thumbnails inside each block**, built off a video element of their own so
  the picture on screen is never seeked out from under whoever is watching it.
  Progressive, which is also the only progress indicator it needs; a file that
  will not decode stalls one seek, gives up, and is never asked again.
- the waveform in a **row of its own** rather than painted behind the lanes,
  because a section you are dragging was covering exactly the stretch of trace
  you were trying to line it up against.

### Each surface states its own window

The panel zooms; the bottom bar is the overview and does not; the jog is
centre-locked on the playhead. They shared one window before, so putting the
panel into Close-up turned the bar into a twenty-second strip and the day
disappeared from the one place meant to always show it. `data-gp-tlwin` on the
surface, `gpTlViewOf` everywhere that positions anything.

### One description of a timeline

Every surface carries the same `data-gp-tl*` attributes. `gpTlBind` drives all
of them — drag, snap, lane change, Escape, pan, wheel-zoom, click-to-seek —
`gpTlDataDraw` paints every trace on one, and `gpTlSync` writes the playhead
into all of them at once. A second implementation would have disagreed with the
first inside a week; that is exactly how the compact strip and the panel came
to be two different things.

### One hue for film

In the new shapes a section is one colour with state on top: the one on screen
is the same colour lit brighter with a rule along its top, the one you picked
wears a white outline, and the only thing that changes hue is the one thing
that is not footage at all — a file the webview cannot decode. `Lanes` keeps
the old four.

## Consequences

- The bar shapes that draw the day subsume "which lap" and "how far in", so the
  dock's right-click menu stops offering them. The other readouts stay and are
  still per-user.
- `gpDockCtx` only lets the working panel change the bar for `Readouts`. The
  ADR-0061 reversal of `dock: false` no longer has a reason behind it, but is
  left as it is: it now governs only the classic bar.
- On a 1366 × 768 window with map, graph, lap times and the footage timeline
  all open, the sheet gets 168 px and its waveform sits on its 30 px floor. It
  no longer scrolls inside its own row, which it did until the inspector was
  made a single sideways-scrolling strip.

### Two things found on the way, and fixed

- **The Setup rail was matched to its groups by position** and had to name
  every one of them in order. It named nine of thirteen — `Car icon`,
  `Recording` and `What's on the bus` had never been added — so every item
  below `Display` scrolled to the wrong card, and clicking `Camera` took you to
  `Mounting`. The rail is now built **from** the headings on screen and cannot
  drift again.
- **`gpPanelWantH` for the clips panel** measured a set of bands the sheet does
  not have, so the panel asked for a height that counted neither its lap strip
  nor its inspector.

## Checks

`tools/check_lookbar.js` — 61 assertions: the fallbacks when the stored choice
names a shape this build has never heard of, the three windows, the sheet's
gutter row count and lane naming, the lap cells carrying the attribute the drag
steps over, chapter coverage arithmetic (two cameras over one lap count once),
the three words under the playhead in all four states, and the thumbnail
cache's bounds while it is still half-built.

`tools/check_clips.js` pins itself to `Lanes`, because it is about clip
arithmetic and the shape should not move under it. `tools/check_focus.js` had
two assertions describing the old dock and one describing a default that was
deliberately reversed a fortnight earlier and had been failing quietly ever
since; all three now describe what the code does.
