# ADR-0031: Channels is a page, and it leads with the car you have

Date: 2026-08-13
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`), rdm7-desktop (resynced base)
Follows ADR-0030, which made Channels one surface. This makes it a place.

## Context

ADR-0030 collapsed four Setup cards into one Channels card and gave it a
single "Add channels" menu. That fixed *what* the surface was, but left it
where it had always been: a modal, reached by clicking a card inside Setup,
sized 1100 px wide with a 300 px list strip down its left edge.

Two things were wrong with that once the dust settled.

**It was one click deeper than the things it dwarfs.** Studio had two modes,
Setup and Design. Setup is a grid of sixteen cards, most of which you touch
once ever — Dimmer, Gear Calc, Marketplace. Channels is where the setup time
actually goes, and it was a peer of "Export Screenshot".

**The list could not show what the customer asked for.** ADR-0030 quoted them
wanting "a complete list of channels and their values on the device
underneath". The 300 px strip could fit a name, a signal and a value, and
nothing else — no range, no warning thresholds. That point was recorded as
half-delivered, and it stayed that way because the modal had no room to
deliver it.

## Decision

**Channels is a third top-level mode**, beside Setup and Design. The Setup
card stays as a signpost that switches mode, rather than a second way in.

**One block of DOM, two mounts.** `#chShared` holds the entire channels UI and
is physically *moved* between the page (`#chPageHost`, its home) and the modal
shell (`#chModalHost`, borrowed when Design mode's inspector opens a specific
channel). Nothing is cloned. There is one set of markup, one renderer, one lot
of state — the ADR-0017 rule, "same markup, same handlers, so which screen was
it on has no wrong answer". A `.ch2-page` class on the wrapper selects the page
layout; every layout difference between the two mounts is pure CSS keyed off
that one class, so the renderer never learns which mount it is drawing into.

**One row markup, two layouts, via grid areas.** The row emits five cells —
name, source, value, range, warnings. In the modal, `grid-template-areas`
stacks name over source and drops the last two cells; on the page the same five
cells become table columns under real headings. Adding a column is a change in
one renderer and one grid template, not two row builders that drift.

**The editor is a pane in the modal and a drawer on the page.** Same element.
On the page it slides over the table from the right, so the list keeps its
scroll position — you do not lose your place halfway through setting a car up.

## The page leads with the car you have

This is the part that matters, and it is not a layout decision.

The list holds **135 rows on a real dash, but two unrelated populations**: the
~32 channels this car actually has, and ~103 canonical catalogue entries you
*could* activate. They were interleaved in one scroll, told apart only by being
greyed at 40% opacity. The customer's "complete list of channels and their
values" means the 32. The other 103 are a shopping list, and they were burying
the thing the page is for.

So the page defaults to configured channels. The catalogue is reached through
"+ Add channels", which is where activating one already belonged.

**Search still reaches everything.** Searching for something you have not set
up yet is precisely when you would search, so a query in the configured view
also matches the catalogue and appends the hits under their own heading — "Not
set up yet — 3 matches in the standard list. Click one to add it." The list
leads with your car; nothing is unreachable. Hiding rows behind a filter with
no way to discover them would have traded one confusion for a worse one.

## Consequences

- Studio has three modes. `setStudioMode` gained one value and one pair of
  enter/leave hooks; Setup and Design are untouched.
- The 2 Hz channel poll stops when the Channels page is not showing, rather
  than running for the life of the session.
- The modal keeps its close button (`.ch2-modal-only`); the page has no close —
  you leave by switching mode.
- `prefers-reduced-motion` disables the drawer slide.

## Verification

Driven against the real dash (`RDM-DCB4-D926`) through the dev server's
`--device` proxy — local HTML, live device data — trapping console errors
throughout. Zero errors across every check below.

- Three mode buttons; the panel shows; `#chShared` mounts into `chPageHost`
  with `.ch2-page`; filter defaults to configured.
- Columns render with real values off the dash: `Coolant Temp | COOLANT_TEMP |
  --°C | 0–120 | 80 / 110`. Headings match the cells.
- Drawer: opens on row click titled with the channel, editor populated;
  measured closed at x=1280, open at x=839, re-closed at x=1280.
- Search spillover: "oil pres" in the configured view produced "Not set up yet
  — 1 match in the standard list. Click one to add it."
- **The mount swap**, which was the real regression risk: Design → modal
  mounts into `chModalHost`, drops `.ch2-page`, hides the column headings and
  the range/warn cells, restores the inline editor and the close button; on
  close it returns to `chPageHost` and the page layout comes back.

### Two things worth writing down

**A stuck transition is not a broken cascade.** The drawer read as "open" in
the DOM while its computed transform stayed at the closed value, which looked
exactly like a specificity bug. It was not: this Browser pane was hidden, so no
frames composite and CSS transitions never advance. Disabling the transition
showed the cascade working perfectly. Diagnosing it by injecting
`transition: none !important` and re-measuring took a minute; guessing at
specificity would have produced a wrong "fix" to correct CSS.

**Blind `.click()` on "the first row" activates channels.** Two channels
(`afr_bank1`, `dtc_count`) appeared on the bench dash during automated sweeps.
The offline queue was ruled out (empty on both origins), and instrumenting
`window.fetch` and `_selectChannel` across a full mode-switch and modal cycle
recorded **zero** activate calls — so the page code does not do it. They came
from test scripts clicking whatever row was first, which sometimes landed on a
greyed one; clicking an inactive row activates it, which is long-standing
behaviour. Tester error, confirmed rather than assumed, and the dash was
restored from its backup afterwards.

That behaviour is deliberate and now labelled on the page ("Click one to add
it"), but in the All / Available views a single click still adds a channel
with no confirmation. Left as-is here; noted as a candidate.
