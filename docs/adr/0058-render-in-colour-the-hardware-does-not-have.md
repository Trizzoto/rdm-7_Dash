# ADR-0058: Render in colour the hardware does not have

Date: 2026-09-02
Status: Accepted — built 2026-09-02, reshaped twice the same day (second and third addenda) and given the dash designer's layout plus a speed knob on 2026-09-03 (fourth addendum); the third, fourth and fifth addenda together are the shape that stands
Repos: rdm7-desktop (keypad workspace `kpfx*` in `src/tauri-overlay.html`,
`tools/check_lightshow.js`)
Feature doc: `rdm7-desktop/docs/KEYPAD_LIGHTSHOW_2026-09.md`
Follows: ADR-0055 (a device on the wrong bit rate is invisible),
ADR-0056 (a page that needs a progress bar is more than one page)

## Context

A Blink PKP keypad's LEDs are addressable, and that word carries more weight
than it can hold. Each key ring is driven by three **bits** in one CAN frame —
one for red, one for green, one for blue. A ring is therefore one of seven
colours, or dark. There is no per-key dimmer. There is no gamma. There are no
intermediate values to send.

What there is: every key is independent, the frame that sets all of them is
eight bytes, and nothing stops a host sending that frame again a moment later.
A PKP-3500 is fifteen keys. Fifteen independently addressable elements,
refreshed ten times a second, is a display. A small one with a seven-colour
palette, but a display, and everything a WLED-style effect engine does to a
strip of pixels works on it.

The question this decision answers is what an effect is *written against*.
Two options, and the difference shows up everywhere downstream.

## Decision

**Effects render in full 24-bit RGB, as if the hardware were a strip of
WS2812s. A quantiser at the very end maps each key to the nearest colour a
ring can actually be. The preview can show either side of that boundary, and
says which one it is showing.**

The alternative — authoring directly in the seven colours, so that what you
write is what the wire carries — was rejected for two reasons.

**Effects stop being arithmetic.** A comet with a tail that fades out behind
the head is one expression in RGB: brightness falls with distance. In
seven-colour space it is a lookup table of special cases, and every new effect
needs its own. Thirty effects written that way is thirty separate small
problems; written in RGB it is thirty short functions over the same
normalised grid.

**The user cannot see the compromise.** This is the bigger one. A gradient
across a 2×2 keypad is four colours; across a 3×5 it is five. Selling a
"gradient" to a device that cannot draw one is only honest if the person
choosing it can see what it becomes. Authoring in seven colours hides the
compromise by making it unrepresentable; authoring in RGB and quantising makes
it a picture — the palette bar above, the colours it really lights below.

So the preview has a switch: **On the keypad** (default) and **As designed**.
The default is the truthful one. Each effect also carries one sentence about
what quantising does to it, shown on its own card — "the tail is the part the
rings cannot draw", "on a 2×2 there is only one ring, so this reads as a
flash" — so the loss is named before it is discovered on a bench.

### The corollary that bit immediately

Aim the quantiser at the colours **on screen**, not at the bit patterns behind
them. The bit pattern for blue is `(0,0,1)`; the blue in Studio's own swatch
row — the one the user clicks, the one every ring is drawn as, the one every
palette interpolates through — is `#3B82F6`, a soft azure. Quantising against
the bit patterns made Studio's own blue come out **cyan**: pick blue, watch
blue not happen. Found by `tools/check_lightshow.js` asserting that each ring
colour quantises back to itself, which it did not.

### Determinism is part of the contract

The same show is rendered in three independent places — the on-screen preview,
the baked frame export, and the live stream through the dash — and they are
the same show only if the frame at *t* is a function of *t* alone. So every
"random" effect (sparkle, twinkle, rain, drift, fire) is a hash of
`(key, time bucket)`, never `Math.random()`, and nothing in the effect engine
reads a wall clock. The harness asserts both against the source, with block
comments blanked out so the comment explaining the rule does not fail it.

### Frames are a cost, not a free resource

The dash gateway refuses more than two dozen frames a second, on principle
(ADR-0055's `web_server_can.c`): a caller that has lost its wait loop is
putting sustained traffic on a bus a car is running on. So the engine dedupes
— a frame identical to the one before it is never sent, which makes a still
effect exactly one frame and then silence — the streamer keeps its own rolling
one-second window rather than earning the dash's refusal, and the inspector
shows the busiest second of the chosen effect before anyone presses play.

## Consequences

- Adding an effect is adding one function to `KPFX`. It inherits the
  quantiser, the preview, the gallery tile, the export and the bus budget,
  and the harness covers it without being edited.
- The seven-colour truth is now visible in the product rather than discovered
  on a bench, which is the same principle ADR-0055 arrived at from the other
  direction: say what the hardware is actually doing, especially when it is
  less than the user hoped.
- Studio can *play* a show while it is open, through the dash. It cannot yet
  make a keypad play one on its own — that needs the show file
  (`rdm_keypad_show.json`, a description rather than a recording, precisely so
  it can be re-timed and re-coloured) to be consumed by dash firmware. Until
  then the keypad's own built-in start-up show (CANopen object `2014h`) is the
  only animation that runs with no host, and the workspace says so.
- J1939 keypads cannot do any of this. The J1939 manual documents no LED
  command at all, so the section refuses with that reason rather than
  appearing to work.

## Addendum, same day: a lane is a stack, not an effect

The first cut gave each lane one effect and two sliders. The ask that came back
immediately was "more control", and the interesting part is that more sliders
on the effect would not have been it.

A single effect can only say one thing. "A slow blue wash, sparkles over the
top, and the two map keys always green" is three things, and no knob on the
wash reaches the other two. What was missing was not resolution on one idea, it
was somewhere to put a second — so a lane now holds up to four **layers**, and
each layer answers three questions independently:

- **what** it draws (any effect, its own colours, its own palette),
- **where** it is allowed to draw (every key · keys that do something · spare
  keys · hand-picked, and the hand-picking is done by clicking the keys on the
  keypad render rather than a grid of abstract squares beside it),
- **when** it is awake (wait before starting · then plays for · loop / once /
  there and back · start it part-way in).

That last group is what kept a **timeline out of the product**. A power-up
sweep that runs for a second and a settle that takes over after it is two
layers with a delay and a life — the same result as two tracks and four
keyframes, with nothing to learn and nothing to drag. A timeline would be more
powerful and nobody would fill it in.

Three smaller decisions fell out of the same pass, all of them consequences of
the original one:

- **Effects name their own knobs.** There is no "intensity" any more: Comet has
  *Tail* and *Fade*, Fire has *Heat* and *Flicker*. The engine still keeps two
  generic amounts; the UI reads the name off the effect. A slider that says the
  thing it does beats one that says the thing it is.
- **"How dim is dark" is a control, not a constant.** It was a hardcoded
  threshold. It is the single most consequential number in the whole system —
  a ring has no dimmer, so every fade in every effect becomes a decision at
  that line — so it belongs to the user, and it belongs to the *show* rather
  than a layer, because the whole keypad has to agree about it. Moving it
  redraws the quantised strip under the palette live; watching a ramp break up
  under the slider is the explanation.
- **Hardware blink became a lane setting.** The keypad blinks a key by itself
  from a bitmap sent once (`0x300 + node`). Offering that as a control rather
  than an effect is a frame-budget decision: a lane that blinks in hardware
  costs ONE frame instead of a stream, which is the difference between
  something safe to leave running on a car's bus and something that is not.

The migration is the part that needed the harness. Every show already saved is
a lane with one effect flat on it, and the obvious `Object.assign` migration
carried the LANE's own settings (`dur`, `bl`, `blink`) and the old knob name
onto the layer — junk that would then have been written into every file saved
from that point on. `tools/check_lightshow.js` (159 checks now) asserts that
nothing outside the layer's own field list survives the trip.

## Second addendum, same day: a show is one thing you pick

The layered version above lasted a few hours. The ask that came back was three
words — *simple, creative, customisable* — and a verdict: the idea was right,
the execution was not. It was not. A lane of four layers with blend modes,
area masks, delays, lives and loop modes is a compositor, and a keypad
animator is not a compositor. It could do more than what replaced it and
nobody would have used it.

**The shape now.** A SHOW is one thing you pick, the way you pick a ringtone.
Ten are built in, each a power-up, a resting look, a key-press reaction and an
alert that were designed to go together and tuned to look right on a 2×2 as
well as a 3×5. A show is made yours with two colours and one dial — *Energy*,
calm to wild, which is speed and density and brightness at once because "make
it calmer" is one thought, not three. Underneath, each of the four MOMENTS is
one EFFECT with at most two named knobs, and the power-up — the only moment
that is genuinely a sequence — is a **filmstrip** of steps, each an effect and
a length.

Three decisions inside that are worth recording, because each one reversed
something the layered version did:

- **A step's length is its speed.** The layered version paced once-through
  effects by the energy dial, so *Roll call* on a 3×5 got cut off at key nine.
  Now every once-through effect paces itself to the time it has been given —
  `prog` runs 0..1 over the first 85% of the step — and settles. The harness
  plays every prebuilt show on every model and checks that every step has
  finished before its time is up. The first draft failed that eleven times.
- **A reaction replaces, it does not mix.** Brightest-wins per channel — the
  layered version's default blend — turned a white key-press over a blue key
  purple. A reaction now takes the key outright wherever it is bright enough to
  light a ring, and the resting look shows through below that.
- **Two colours, no palettes.** Eight fixed ramps, a two-colour picker and a
  build-your-own stop editor became: main, accent, alert, and a row of themes
  that set the first two together. Fire and Rainbow flow keep their own ramps
  because fire is fire. Everything else draws from the show's two colours,
  which is what makes a theme recolour the whole show at once.

Also found by the harness on the way, and fixed: energy was read from the
global show rather than the one being rendered, so a show card in the rail was
paced by whatever was being edited; the alert colour was designed and never
wired into the composer; and the "match the show" backlight was sending a frame
every tick, doubling the bus cost of every moving alert — it now moves in eight
brightness steps and updates at most four times a second.

Three storage shapes now exist from a single day. The loader reads all of them
and carries across only what a person would notice losing: which effect each
moment used, the two colours, the speed. Layers, masks and blend modes have no
home in the new shape on purpose.

`tools/check_lightshow.js` was rewritten for the new model: 125 checks.

## Third addendum, same day: one thing, then add to it

The shows-and-moments version above lasted an afternoon. The verdict on it was
the same as on the layered one: *still stupidly over-complicated.* And the
brief that replaced it was one sentence: **a boot sequence that is fully
customisable; the rest is basic setup for now; we add onto it once basic is
set up properly.**

That sentence is the decision. The page is the boot sequence — a row of steps,
each an effect, a colour and a length — and nothing else. When the last step
ends the keys settle to the colours from the Design page. Six ready-made boots
to start from. Sixteen effects, eight that play once and settle and eight that
loop for as long as a step lasts. One colour row, one length slider, a
direction where an effect has one. Play it, save it, load it, frames for a
USB-CAN tool.

What was removed, and why each removal is right rather than merely smaller:

- **Resting looks, key-press reactions and alerts.** Each is a real want, and
  each is a separate one. Building all four at once is what made two pages of
  chaos; the shape now has a place for them (`idle` is a field, fixed to the
  key colours) and no UI for them until they are asked for.
- **The energy dial.** Looping effects run at one fixed pace. A step's length
  already IS its speed for the once-through effects, which are what a boot is
  made of; a second speed control was one more thing to explain.
- **Colour themes and a main/accent pair.** A step has one colour. White is the
  highlight, always. Sweep, Roll call and Crank end on the keys' own colours,
  which is the point of them.
- **Ten shows.** Six boots. A starting point, not a category: picking one
  replaces the steps, and the page says which one you started from until you
  change something.

Kept from the earlier versions, unchanged in principle: render in true RGB and
quantise at the end; a hash instead of a dice roll; a step paced by the time
it is given; dedupe every frame and stay under the bus budget; and the harness
that plays every ready-made boot on every model from cold. It also now checks
the thing the brief was about — that a once-through effect has settled by the
end of its own time on every model and is still going a quarter of the way in,
so the length is neither cut off nor ignored.

The loader reads all four of the day's shapes and carries across only the
steps and their colours. `tools/check_lightshow.js` was rewritten for the third
time.

## Fourth addendum, next day: give it the designer's shape, and a second knob

The boot-only page above was right about scope and wrong about shape. It was
one column on a sheet: keypad, then steps, then controls, then boots, then
send. Everything after the first screen pushed the keypad off the top, so the
thing being made was not on screen while it was being changed. The note back
was three things, and all three are the same complaint from different angles:
*duration and speed; make the demo better and more obvious; the keypad always
visible like the dash designer, sidebars scrollable, properties right, choices
left.*

**The shape is now the dash designer's**, reusing `.kp-body`, `.kp-rail`,
`.kp-stage` and `.kp-insp` outright rather than restyling copies — so the two
keypad pages are the same page with a different rail, and the existing 900 px
reflow rule catches this one without knowing it exists. Choices left, keypad
centre, properties right; both sides scroll and the middle never does, because
the keypad shrinks to fit instead.

**The demo and the editor are now one object.** The row of thumbnails became a
timeline: each step is a block as wide as it is long, so the shape of a boot is
visible without reading a number, and a trailing hatched block is the keys at
rest. The playhead is the clock; dragging it scrubs and picks up the step it
lands on. The clock only ever paints and moves the playhead — a rAF that
re-renders panels is a page you cannot type into, and that is asserted.

**Speed is the second knob**, and it is a genuinely different question from
length. How long is the slot the step takes; speed is how fast it runs inside
it. At 1× a once-through effect still fills its slot exactly, which is the rule
the last round was about. Above 1× it finishes early and *holds* what it made.
Below 1× the step ends before it finishes — a crank that never quite catches —
which is a real look and is allowed rather than clamped, with the panel saying
in one sentence which of the three is happening. For a looping effect speed is
simply its pace, and `render(fx, t, 2×)` is exactly `render(fx, 2t, 1×)`.

The cost of that on the bus was the thing to check, not to assume: every effect
wound to 4× for six seconds is still inside the dash's frame budget, because
dedupe and the 12 fps ceiling are upstream of the speed knob.

`tools/check_lightshow.js` grew from 98 to 122 checks — the speed rules, the
clamps on the way back in, and the page's shape itself, since a layout that
quietly reverts to one column is exactly what a firmware re-sync causes.

## Fifth addendum, next day: the two things the boot-only shape left a slot for

The third addendum ended with a promise: resting looks and key-press reactions
*"come back one at a time, once this is right"*, and the shape kept a place for
the first of them (`idle` was a field, fixed to the key colours, with no UI).
The boot held up for a day of use, so both came back.

They came back on the **row that was already there**, not on a new page — which
is the whole difference between this and the shows-and-moments version that had
to be thrown away. The block at the end of the timeline used to be a hatched
"Keys settle" marker; it is now a block you can open, and it holds both:

- **What it rests in** — any of the eight looping effects with its own colour
  and speed, or *Back to the buttons*, which is the default and hands the rings
  over to what each button is actually doing.
- **What a press does** — Nothing, Light while held, Flash it, Ripple out,
  Everything else out.

Three things kept this from becoming the second version again:

**A reaction belongs to the rest, not to a lane of its own.** During the boot
the boot is playing; once the rings are handed back to the buttons there is
nothing left to react with. So it is edited where it is true, which is also
where it is cheapest to explain.

**A reaction is drawn over the resting look, never instead of it.** A ripple
crosses a scanner rather than replacing it, and letting go leaves no seam. That
is one line in `kpfxCompose` and it is the reason the two features do not need
to know about each other.

**The panel says what each choice costs.** *Back to the buttons* streams
nothing at all — one frame, then silence. Anything else means the rings are
showing the animation rather than the buttons, and Studio has to stay open to
drive it. The dash is what plays a reaction in a car; Studio previews it
against your own thumb, because Studio streams frames blind and is never told
about a real press.

What still did not come back: the energy dial, the colour themes, the
main/accent pair and the alert lane. Warnings are a per-key thing on the Design
page (ADR-0062), which is where they belong — they are about one button's
condition, not about the keypad's mood.

What the two new moments cost elsewhere, all of it found by looking rather
than by breaking:

- **The frame script was lying.** Its header promised "the last frame is the
  keys at rest, and it holds", which stopped being true the moment a resting
  look could animate — replay an animation once and stop, and the keypad
  freezes on whatever frame it ended on. It now says which of the two it is,
  and the file format went to 4 to mark that the tail means something new. The
  exports are named `_lights.json` / `_lights_frames.txt`, because a file
  called `_boot` that carries a resting look and a reaction is the same lie in
  the filename.
- **A ready-made boot was wiping them.** `kpfxUsePreset` replaced the whole
  show, which was harmless when a show was only steps. The panel says
  "replaces the steps above", so now that is all it does.
- **The row stopped being reachable from a keyboard** when the blocks became
  divs. They are buttons again, with `aria-pressed` kept in step with the
  drawn state, and a keyboard click (`event.detail === 0`) opens a block
  without fighting the drag-to-scrub on the pointer path.
- **A finger is a pointer.** Pressing a key and dragging the playhead moved
  from mouse events to pointer events, which costs nothing and means a
  touchscreen works.

`tools/check_lightshow.js` went to 151 checks: every reaction on
every model draws a legal frame and lights something; a press during the boot is
ignored; "light while held" is lit exactly while held; a resting animation wound
to 4× is still inside the dash's frame budget while handing back costs one
frame; and the file carries all three moments and reads back identical.

## Sixth addendum, 2026-09-04: "back to the buttons" was not the buttons

The fifth addendum shipped *Back to the buttons* as the default rest and then
drew it with the `keys` effect — **every assigned key lit in its design
colour**. On the keypad most people build, every button is momentary, so at
rest every ring is dark; what the page showed instead was fifteen glowing
rings. It was the resting look nine boots out of ten end on, so the picture was
wrong nearly always, and it was wrong in the direction that flatters: the
prettiest frame in the app was the one telling the biggest lie.

`keys` and "back to the buttons" were never the same thing. `keys` is a boot
ENDING — *Sweep*, *Roll call* and *Crank* settle onto the colours you chose so
the show lands on the keypad you designed, and a boot that ends on nothing is
worse than one that ends on white. The rest is a STATE, and the state of a
button nobody is holding is off.

So the handover is its own effect now, `buttons`, and it reads the same button
state the Design page paints: a momentary ring is dark until it is held, a
latching one stays as you left it, a multi-position one shows the position it
is parked on (dark if that position is Off). Deliberately *not* `ledActive()`
— that folds in the editing demo and a tripped warning, which are things
happening on another page, not what the keypad rests in. `keys` stays, as the
boot-step effect it always was, and is no longer offered as a resting look.

Every boot ever saved spells the handover `"keys"` and means this, so a rest of
`keys` is read as `buttons` on load; the file format goes to 5 to mark it. The
tail contract from ADR-0059 is unchanged and now actually true: hand back and
the last frame holds — it just holds the buttons rather than a keypad-shaped
Christmas tree.

**And the setting you cannot find is a setting you do not have.** The three
lighting values a PKP keeps (ADR-0063) sat only in the Design page's rail,
under the model list. The question "why does it look like that at rest?" is
asked on the Lights page, in front of the keypad at rest — so the same three
fields are on that page's *After the boot* panel too, bound to the same `kp.*`
values, with the day/night room that makes a backlight colour judgeable. Both
panels now read the levels as a percentage rather than raw 0-63, which is what
every other part of Studio already reported them as. One control, two places
it is needed; not two controls.

`tools/check_lightshow.js` went to 165: with every button at rest nothing is
lit, latching one lights that ring and only that ring, a cycle key parked on
Off is dark and lit on the colour of the position it is parked on, `keys` is
still a boot step and no longer a rest, the handover cannot be used as a boot
step, and every saved `"keys"` rest reads back as the buttons.

## Seventh addendum, 2026-09-04: the lamp the show was not allowed to touch

A ring is three bits: seven colours and dark. The lamp behind the printed
legends is nine colours and sixty-four levels — and amber and lime are on it,
the two colours no ring can make. It was held still: `kpfxFrame` returned
`bl: { drive: false }`, `kpfxBlFrame` built a `0x500+node` frame nobody sent,
and `KPFX_BL_MIN_MS` throttled a lane with nothing in it. The widest expressive
range on the part, wired up and switched off.

A step can now set the legends, or leave them alone, and leaving them alone is
the default — a boot that repaints the legends by accident is worse than one
that never touches them. The REST always restores the standing value from the
Design page, because the legends are a setting the keypad keeps: a show that
borrows them owes it a way back, and the frame script's last legend frame is
that restore. Two ready-made boots drive them (Start lights goes red then
green, Fire up burns amber) so the lane is discoverable by picking one rather
than by reading about it.

## Eighth addendum, 2026-09-04: two answers to "what happens at ignition"

The keypad's own start-up show (`2014h`) is the only animation it can play with
nothing attached, and it lived on the Connection page behind a dropdown and a
**Change** button that called `kpfxGoLane()` — a function that went away with
the lanes. The one route to it threw.

It is a block at the head of the timeline now, before the first step, because
that is the order the two things happen in: the keypad's own show, then yours,
then the rest. The panel prints exactly that as three rows. Studio does not draw
the factory animation — it has never seen it, and a guess would be the one lie
this page exists to avoid — so the picture is the keypad as it is before
anything is talking to it: rings dark, legends on their own default.

## Ninth addendum, 2026-09-04: the rest of the list

Small, and all of it was found by using the page rather than by reading it.

- **Undo.** There was none anywhere in this workspace, and three buttons are one
  click from throwing work away: a ready-made boot replaces your steps, Remove
  deletes one, Start blank wipes the keypad. The snapshot is the whole keypad
  rather than a diff — it is a few kilobytes, and a partial undo that restores
  the steps but not the key you also changed is worse than none, because it
  looks like it worked. The stack belongs to one keypad and is dropped when you
  switch (ADR-0060), or the first Ctrl+Z after a switch would paste the other
  one's design over this one.
- **Duplicate, drag, keyboard.** Duplicate is the commonest edit on the page (a
  red flash then a green one) and used to mean re-picking the effect and redoing
  three fields. Reordering drags the block's colour bar rather than the block,
  because dragging the block already means scrub and one gesture cannot mean two
  things. Space, arrows, D and Delete, because it is a timeline.
- **Your own starting points.** Six fixed ready-mades and no way to keep the
  seventh — the one you spent the afternoon on. They are a library, not part of
  a keypad: the boot you liked on the wheel pad is the obvious thing to start the
  console one from. Steps only; the resting look and the reaction belong to the
  keypad they are set on.
- **A real thumb.** Studio streams blind and is never told about a press, so a
  reaction could only be previewed against your own mouse. The dash's frame
  tracker already holds the keypad's key frame — the same one the ECU reads — so
  **Watch the keypad** polls it, and pressing a key on the real keypad lights the
  ring here and fires the reaction. Two rules keep it honest: it only affects
  frames marked `live` (never a baked file), and it never writes the Design
  page's own sim state.
- **A blink blinks on screen and holds in the file.** The same `live` flag: the
  preview blinks a key set to blink, at the Design page's own rates, and the bake
  does not, because the tail has to be one frame that holds (ADR-0059).
- **A warning is still the dash's.** Deliberately NOT drawn at rest: a tripped
  warning is live, and it is not in the file. The panel says so rather than the
  code deciding quietly.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` (`kpfx*`, one contiguous block;
  `kpBuildSvg(fx)` for the paintable stage)
- Tests: `rdm7-desktop/tools/check_lightshow.js`
- Hardware log: `rdm7-desktop/docs/BLINK_MARINE_PKP2200_CANOPEN_2026-08-28.md`
- Related ADRs: 0055, 0056
