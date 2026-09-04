# ADR-0062: The picture is the simulator, and a control demonstrates itself

Date: 2026-09-03
Status: Accepted — built 2026-09-03
Repos: rdm7-desktop (keypad workspace in `src/tauri-overlay.html`,
`tools/check_keydemo.js`, `tools/check_keypad.js`)
Follows: ADR-0058 (render in colour the hardware does not have),
ADR-0060 (a keypad is a device, not a model)

## Context

Studio's keypad workspace had two tabs showing the same picture, and the
picture disagreed with itself.

**Design** lit every assigned key in its colour, permanently. **Live** ran a
simulated car: rings lit only when the button was actually on, warnings fired
when a channel crossed a threshold, and the configuration panel was replaced by
*"Live mode — configuration locked. Switch to Design to edit."*

Neither half was wrong on its own. Together they taught people that the picture
was decoration, and the specific complaint that ended it was exact:

> in design its a little bit complicated that the green is always on even if
> its setup as momentary

That is not a nitpick. A momentary button — LAUNCH, LOG, PAGE — is dark in a car
until a thumb is on it. Design showed it lit forever; Live showed it dark; the
field that set the colour was labelled **Resting colour**, which agreed with the
tab that was lying. Someone reading the screen learned a wrong thing about their
own keypad, and the only way to find out was to switch tabs and lose the panel.

The second half of the complaint named the fix for the half that was true:

> only demo things while configuring them like when you setup flash to go red
> when coolant over 95 etc then the can demo plays and it goes over 95

## Decision

**One page. The ring shows what the dash would be driving.** Dark when the
function is off, lit when it is on, the warning colour when a condition has
tripped. Design absorbed Live; the tab is gone, the lock message with it.

**Pressing a key on the picture presses it.** Mouse down selects the key *and*
holds it: a momentary key is lit for exactly as long as the button is held, a
latching one flips, a cycle one steps. One gesture, both jobs — the panel
follows your thumb.

**A control demonstrates itself while you use it, and nothing else moves.**
This is the part that makes an honest picture usable, because two things a
truthful keypad cannot show are the colour you are picking for a button that is
currently off, and a warning that only fires when the coolant really does go
over 95. So:

| Touch | What plays |
|---|---|
| the colour swatches, or the blink control | that key holds its colour and blink |
| "How it works" | that key plays a press — held and released, or on then off, or stepping its positions |
| a warning row (hover is enough) | its channel walks across the threshold and back, and the ring flips |
| a cycle position | the keypad parks on that position |
| anything about the whole keypad's light — day/night brightness, the legend backlight, "Show every colour" | every assigned key holds its colour |

A demo lets go a few seconds after you stop touching the control, and a real
press beats a demo outright: you asked the button directly, so the answer should
be the button's.

**Consequently, warnings do not fire on their own any more.** They fire while
they are being configured. The dash is what watches these channels in a car;
a keypad blinking red at an empty room because a simulated oscillator wandered
over a number nobody was looking at taught no one anything.

**"Resting colour" is now "Colour when it's on"**, which is what the field
always set.

## Consequences

A keypad you have not touched is mostly dark, which is what it will be in the
car and is a change from what people saw before. Three things carry the weight
that the permanent lie used to: pressing a key lights it, editing anything about
a key lights that key, and one deliberate control — **Show every colour** in the
Lighting rail — lights the whole scheme for a few seconds. Notably the
brightness sliders could never be judged before on a keypad that is honestly
dark, so they light it too; that is a control that got *more* useful.

The demo bus is a single small state object (`kpDemo`: what, which key, a
clock, an expiry) read by `ledActive` / `ledColor` / `ledBehavior` and by
`_kpLitColorId`, which is what the LED-drive frame is built from — so what is
drawn and what would go on the wire cannot drift apart. Panels declare what they
demonstrate in the markup (`data-demo="light" | "mode" | "alert" | "position" |
"all"`) and one delegated listener per panel does the wiring, bound once rather
than once per edit.

Redraws stay honest about cost: a demo tick recomputes a signature of every
ring and only rebuilds the SVG when that signature changes, because rebuilding
mid-blink restarts the CSS animation and the blink looks broken. That is the
same rule the alert simulator has always used.

`tools/check_keydemo.js` is new (51 checks) and exists for the quiet failures:
the resting lie coming back because it looks nicer, a demo that never stops, a
demo leaking onto keys you are not editing, warnings firing on their own again,
and the picture disagreeing with the frame. `tools/check_keypad.js` asserts the
section list is four long and that no Live tab or locked panel can come back.
