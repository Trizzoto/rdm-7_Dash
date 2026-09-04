# ADR-0059: The last frame is the one that stays

Date: 2026-09-02
Status: Accepted — built 2026-09-02
Repos: rdm7-desktop (keypad workspace `kpfx*` in `src/tauri-overlay.html`,
`tools/check_lightshow.js`)
Feature doc: `rdm7-desktop/docs/KEYPAD_LIGHTSHOW_2026-09.md`
Follows: ADR-0058 (render in colour the hardware does not have),
ADR-0055 (a device on the wrong bit rate is invisible, not broken)

## Context

A Blink PKP keypad has no idea whether anyone is talking to it. It lights
whatever the last `0x200 + node` frame told it to light, and it lights that
forever — through the host closing, the laptop sleeping, the USB coming out,
the dash rebooting. There is no host-timeout object in either manual. There is
no fallback pattern. The keypad is a frame buffer with no refresh requirement.

That is fine until you notice what it means: **a keypad nobody is driving looks
exactly like a keypad in the middle of a still effect.** Studio stops streaming
and the panel sits there in a perfectly plausible resting show. Every key is
the colour the designer chose. Nothing is wrong on screen and nothing is right
on the bus.

The first cut of the Lightshow already half-knew this. The streamer ends with a
comment that says *leave it somewhere deliberate* and then sends one hardcoded
frame — each key in its own colour — and explicitly **clears** the hardware
blink bitmap so the keypad is not left flashing. Which is to say: the most
consequential frame in the whole feature, the one that is on the panel for the
rest of the day, was a constant nobody could see or change.

The second thing this decision covers is a naming collision that had already
happened. There are two start-up shows:

| Which | Where it was | Needs a host | What it can do |
|---|---|---|---|
| The keypad's own | Connection → Advanced, object `2014h` | No | Off · Full show · Fast flash |
| The Power-up lane | Lightshow | Yes | Any of thirty effects, four layers |

Same words, two sections, opposite capabilities. Set the Power-up lane, unplug
the laptop, turn the key: the keypad does the factory flash. The product had
two answers to one question and put them three clicks apart.

## Decision

**Every frame that outlives the stream is a lane the user owns, and lanes that
outlive the stream live next to the lanes that do not.**

Three things follow.

### The sign-off is a lane

A sixth lane, **"Nothing is talking"**, holds the frame Studio sends last. It
is edited exactly like the other five — layers, colours, masks, hardware blink
— and it replaces the hardcoded constant in the streamer. Its default is the
old constant (each key in its own colour), so nobody's keypad changes
behaviour by upgrading; the point is that it is now visible and changeable
rather than that it is different.

Two things make this lane its own shape rather than a sixth copy of the
others:

**It is one frame, so timing is not offered.** Nothing will advance it. The
"when it plays" group is gone from its inspector and the preview holds `t = 0`,
because a lane that animates in the preview and freezes on the hardware is a
lie told in the most expensive place.

**It is the one lane where hardware blink is the whole point.** A blink bitmap
(`0x300 + node`) is the only animation a PKP will produce with nothing
attached. So on this lane, "the keypad blinks it" is not a bus-budget
optimisation, it is the only way to say *nothing is driving me* — and the
inspector says so in those words.

The bus panel for this lane reads **one frame, then nothing**, which is the
honest cost and also the argument.

### The keypad's own start-up show moves to the Power-up lane

Object `2014h` is now a field *inside* the Power-up lane, labelled for what it
actually is: **"With nothing attached, the keypad does…"**. Connection keeps
the wire settings — node, bit rate, protocol, key broadcast — and carries a
line pointing at the lane. One question, one place, and the two answers are
now stacked one above the other where the difference is unmissable.

### The limit is stated, not implied

Studio can only send a last frame when it knows it is stopping. A yanked cable
or a crashed host leaves whatever was on the panel, and no amount of design
fixes that from this side. The lane says so plainly rather than implying a
watchdog that does not exist:

> Studio sends this when the show stops. If the power is pulled instead,
> the keypad keeps whatever was on it — nothing can catch that.

This is the same rule ADR-0055 arrived at from the other direction: say what
the hardware is really doing, especially when it is less than the user hoped.

## Consequences

- The streamer's exit path renders a user lane instead of a literal, and
  honours that lane's blink setting instead of unconditionally clearing it.
  Clearing is now what an *empty* blink bitmap means, not what stopping means.
- `kpfxCompose` is untouched. The sign-off lane is deliberately not part of the
  running show's priority list — it is what happens *after* the show — so it is
  rendered through the resting slot for preview and bake, and composition
  never sees it.
- A show file gains a `signoff` slot. Files written before this read back
  through the normaliser and get the default, which is the old behaviour, so
  no saved show changes.
- The Connection page loses a control and gains a sentence. That is the right
  trade: the control was in the wrong room.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` — `KPFX_SLOTS` (`signoff`),
  `kpfxPlayLive` exit path, `kpfxRenderInsp`, `kpRenderConnection`
- Tests: `rdm7-desktop/tools/check_lightshow.js`
- Hardware log: `rdm7-desktop/docs/BLINK_MARINE_PKP2200_CANOPEN_2026-08-28.md`
  (no host-timeout object exists in either manual — checked)
- Related ADRs: 0055, 0058, 0060
