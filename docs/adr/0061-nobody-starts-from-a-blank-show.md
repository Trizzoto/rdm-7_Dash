# ADR-0061: Nobody starts from a blank show

Date: 2026-09-02
Status: Accepted — built 2026-09-02
Repos: rdm7-desktop (keypad workspace `kpfx*` in `src/tauri-overlay.html`,
`tools/check_lightshow.js`)
Feature doc: `rdm7-desktop/docs/KEYPAD_LIGHTSHOW_2026-09.md`
Follows: ADR-0058 (render in colour the hardware does not have),
ADR-0059 (the last frame is the one that stays)

## Context

The Lightshow, as built, is six lanes × up to four layers × thirty effects ×
two knobs × eight palettes. Every one of those choices is defensible and the
gallery animates so nothing has to be read. It is still, on arrival, an empty
grid with a rail of six words down the left, and the first question it asks a
customer who fits keypads for a living but has never used a lighting tool is
*"which of the six moments would you like to author first?"*

That is a question for someone who already has a mental model of the product.
Everyone else looks at it, decides the feature is not for them, and ships the
keypad with the factory flash.

The engine is not the problem. The problem is that the shortest path from
opening the section to a keypad that looks good is roughly forty clicks, and
none of the forty is wrong.

## Decision

**The section opens on finished work. A row of complete, named shows sits above
everything else, one click applies one to every lane, and the knobs are still
right there underneath for anyone who wants them.**

Eight looks, each a whole show — power-up, resting, night, press, alert and
sign-off — written in the same data the designer writes, so a look is a
starting point rather than a mode. Apply one and every lane fills in; change
anything and it is your show now.

Three rules keep them honest:

**A look must survive every model.** The same look is applied to a 2×2 and a
3×5, so nothing may depend on a key count or a key position. Effects already
render against a normalised grid (ADR-0058); the looks inherit that and the
harness applies all eight to all four models and asserts every one produces a
lit frame.

**A look must be affordable.** Every look is baked and rate-checked in the
harness against the same budget the inspector shows, so no ready-made show can
be the thing that earns the dash's refusal. A look that cannot be afforded is
not a look, it is a trap with a nice name.

**One is deliberately nothing.** *Just the buttons* is the first tile: no
animation at all, each key lit in the colour it was assigned on the Design
page. It is the honest answer for most vehicles, and offering it first means
the feature does not read as "you must now have a light show".

### The second half: say what it will do, in words

Applying a look answers *what*. It does not answer *what will actually
happen*, which is the question a non-technical person is really holding, and
which the six-lane rail answers only if you already understand lanes.

So the section carries a plain-English summary that describes the current
show as a sequence of sentences — the same information as the rail, in the
order the keypad will do it, naming the effect, the colour and the number of
seconds:

> Turn the key and the buttons sweep red to amber for 2.6 seconds.
> Then they settle to each button's own colour.
> Press a button and a white ripple runs out from it.
> If an alert trips, the whole keypad flashes red until it clears.
> When Studio stops, the keypad holds each button's own colour.

It is generated from the show, so it cannot drift from it, and it is the one
thing on the page a customer can read to someone else over the phone.

## Consequences

- Adding a look is adding one entry to `KPFX_LOOKS`. It inherits the tile, the
  preview, the apply path, and the harness covers it without being edited —
  the same property ADR-0058 gave effects.
- The looks are stored as show data, not as code that builds show data, so
  exporting a look and exporting a hand-built show are the same file, and a
  user's own show can become a look by being pasted into the list.
- The summary is a renderer over the show, not a field on it. Nothing writes
  it, so nothing can leave it stale.
- Applying a look overwrites every lane. One level of undo is offered on the
  toast; a confirmation dialog was rejected because the action is cheap to
  reverse and a dialog on a "try it" gesture is what makes people stop trying
  things.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` — `KPFX_LOOKS`,
  `kpfxApplyLook`, `kpfxRenderLooks`, `kpfxSummary`
- Tests: `rdm7-desktop/tools/check_lightshow.js`
- Related ADRs: 0058, 0059, 0060
