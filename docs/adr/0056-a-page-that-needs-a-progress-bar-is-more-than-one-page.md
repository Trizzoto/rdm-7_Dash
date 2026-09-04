# ADR-0056: A page that needs a progress bar is more than one page

Date: 2026-09-02
Status: Accepted — built and shipped 2026-09-02
Repos: rdm7-desktop (keypad workspace, `tools/check_keypad.js`)
Follows: ADR-0024 (the lap timer wears the brand), ADR-0055 (the keypad wizard)

## Context

The keypad workspace was one screen doing five jobs at once. A 172 px rail
held a model picker, an ECU picker, a connection panel and a lighting fold,
stacked, each with a paragraph of its own. The middle held the keypad. The
right held a per-key inspector. And along the bottom ran a five-step strip —
*Pick your keypad · Pick your ECU · Pick how it connects · Style your keys ·
Wire it up & set up your ECU* — with ticks that lit as you went.

The owner's words were "too cluttered and over complicated with lots of
writing", and asked for the lap timer's style. Both halves of that are the
same problem, and the strip is the tell: **a screen that needs a progress bar
to explain the order you are supposed to read it in is more than one screen.**
The strip was not navigation, it was an apology for the layout.

The writing was the same story from the other end. Counted before this pass,
the per-key inspector carried five explanatory blocks under *every* key — a
doorbell metaphor for what a button does, a "Tailored for <ECU> · change in
the sidebar" line, a branded "easiest path" card, a caveat about menu names,
and a closing tip about switching to Live. Sixteen keys on a PKP-3500 means
that text renders sixteen times. Each sentence was true and each was written
for a good reason; the total was unreadable.

## Decision

**Four sections, and the tabs are the flow.** DESIGN · LIVE · CONNECTION ·
ECU, in the lap timer's three-band shell: black brand bar, white tab row with
the red underline, then the page's own uppercase headline and one line of
orientation. Exactly the shape LIBRARY / LIVE / ANALYSE / DRIFT already has,
which needs no numbering either.

The mechanical part is copied from ADR-0024 deliberately: **re-bind the
firmware's dark theme variables to light values inside `#kpWorkspace`**, so
every existing `.kp-*` rule that says `var(--text)` or `var(--panel)` lands on
the Industry ground without being rewritten. The `.gpb-*` chrome classes are
re-declared under `#kpWorkspace` rather than shared, because the lap timer's
are scoped `#gpWorkspace` on purpose — the `ws-*` layer is what the analyzer
and IO share, and restyling that globally would drag them along uninvited.

Three rules decided where each sentence went:

- **A fact that is the same for every key is said once, on the ECU page.**
  What the keypad puts on the bus, and that it keeps no state at all, is one
  table there instead of a paragraph per key.
- **A fact about one control lives on that control**, as a tooltip. The
  swatch note explaining that amber and lime are backlight-only was
  explaining a disabled state to everyone, forever; it is now on the info dot
  beside the label.
- **A fact with more than two parts is a table, not a sentence.** Frame, bit,
  and what the ECU must do are three labelled rows now.

## Consequences

The five-step strip, the "Design / Live (sim)" mode switch in the chrome, and
the model-picker's "each model keeps its own layout" note are gone. Live is a
section. `kpFlowJump` and `kpUpdateFlowProgress` are gone as concepts —
the latter survives as a one-line alias because several edit handlers call it,
and it now just refreshes the page headline.

Both keypad modals — the setup wizard and the ECU guide — moved from
`document.body` into `#kpWorkspace`. On the body they kept the dark theme and
opened as dark boxes over a white page. `position: fixed` still resolves
against the viewport because `#kpWorkspace` carries no transform or filter.

The exported setup file's guide text referred to "the ⇩ button in the left
rail", which no longer exists; it says Connection → Files now. That class of
staleness is the standing cost of moving things, and the reason the harness
below checks for the *sentences* rather than only the structure.

`tools/check_keypad.js` gained a section that pins the shape and a **writing
budget**: the four section ids, the absence of the flow strip and the mode
toggle, that the theme is re-bound rather than re-styled, that the keypad does
not reach into the lap timer's scoped chrome — and, for each paragraph
removed, both that it is gone *and* that the fact it carried still exists
somewhere. A deletion that loses information is not a simplification, and
without the second half of that pair this harness would happily pass an empty
page. Those checks read the source with block comments blanked out, because
this file's own reasoning quotes the sentences it removed.

None of this is verified against the physical keypad; that remains true from
ADR-0055. What is verified is every section rendering, both modals opening in
the light theme, and 91 assertions passing.
