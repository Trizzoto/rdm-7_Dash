# ADR-0057: The editor wears the brand from outside

Date: 2026-09-02
Status: Accepted — built and shipped 2026-09-02
Repos: rdm7-desktop (`src/tauri-overlay.html` `dsb-*`, `tools/check_dash.js`)
Follows: ADR-0024 (the lap timer wears the brand), ADR-0056 (the keypad)

## Context

Three of the four things in RDM Studio now share one look: black brand bar,
white tab row with a red underline, a page headline. The fourth is the dash
editor, and it is the one that matters most — it is the hero card on Home and
the reason most people open the app. Leaving it on the Photoshop-blue dark
theme meant the suite looked like two products.

The editor is also the one that cannot be rewritten here. It is the
firmware's own page, synced verbatim from `RDM-7_Dash/main/web/index.html`,
and ADR-0007 is explicit: never edit `src/firmware-base.html`; desktop-only
changes go in the overlay. It is not a workspace with a root element either —
it *is* the base document, a `<header>`, three panels, a `<footer>` and about
fifty modals, all direct children of `<body>`.

Editing it in the firmware repo was the obvious alternative and is the wrong
trade twice over. It would change the editor the **device** serves over WiFi,
which is a different surface with different users and no reason to follow the
desktop suite's branding. And that file was under active edit by other work
at the time, which is a bad place to land a 200-line restyle.

## Decision

**Build the shell at runtime from the overlay, and MOVE the firmware
header's controls into it.**

- A class on `<body>` (`dsb-on`) carries the token re-bind — the same lever
  ADR-0024 and ADR-0056 used, and the reason 1,278 `var(--…)` usages across
  19 tokens change palette without a single rule being rewritten.
- `_dsbBuild()` inserts three bands before `<header>`, hides the header, and
  re-parents: the connection bar and Save into the black bar; the mode
  switcher into the tab row, restyled from a segmented switch into tabs; the
  live pills and undo/redo/reset/sync/help into the tab row's right side; the
  layout, screen and bezel controls into a tools strip under the headline.

**Moved, not copied.** Every one of these controls is wired by id or by an
inline `onclick`, and several are written to by name from elsewhere (the
save-state pill, the connection poller). A copy leaves all of that pointing
at the original inside a hidden header — the exact bug shape `#gpHold` and
`#gpDevHold` already exist to avoid. Re-parenting keeps `getElementById`
answering, keeps inline handlers attached, and keeps inline `display:none`
that other code has set (which is why the LIVE and CONTROL pills correctly
stay hidden while offline).

Two structural facts make this safe, and both were checked before it was
written: no firmware JavaScript reaches for a control through `header` (no
`closest('header')`, no `querySelector('header …')`), and the active mode is
readable from a class the firmware's own CSS depends on. The headline follows
that class through a `MutationObserver` rather than by wrapping
`setStudioMode` — a wrapper would be silently dropped by the next re-sync.

Red is spent on two things only: which section you are in, and Save. The
Bezel/Widgets/SIM toggles mark themselves with ink, because a row of red
toggles beside a red Save makes neither of them mean anything.

## Consequences

The device-served editor is untouched. Only Studio looks like this.

The failure mode of re-parenting by id is **silent**: `_dsbMove` on a missing
element is a no-op, so a firmware re-sync that renames `applyBtn` would leave
Save behind in a `display:none` header with nothing thrown, and you would
find out when somebody could not save. `tools/check_dash.js` exists for that
one reason. It scrapes the moved ids out of the overlay itself — so adding a
control to the shell adds it to what is checked — and asserts each still
exists in the built page, that most still come from the firmware base, that
the glyph-matched undo/redo arrows are still there, that the `data-mode`
attributes survive, and that no firmware JS has started walking up to
`<header>`. It also pins the theme class going off on every path that leaves
the editor, and pins `#suiteHome`, `#caWorkspace` and `#ioWorkspace` dark —
those three do not re-bind for themselves, so a class left on by a path
nobody thought of would render them unreadable.

Losing `header …` CSS is the other cost: those rules gave every control its
30 px height and padding. The bands restate that, and the harness checks they
still do.

Verified across all three modes, both the wizard and the ECU guide modals,
the widget palette, layers, properties and the canvas — and that Home and
every workspace come back dark. 50 harnesses pass.

**Amended the same day — grey, not white, and defined once.** The owner's
verdict on the result was "all too bright". The Industry light end had been
white cards on a `#f2f2f3` ground, copied verbatim into three scopes. It is
now a shared `:root { --ind-* }` palette — ground `#c8c9cc`, surface
`#d9dadd`, elevated `#e4e5e8` — that the lap timer, the keypad and the
editor all reference, so the palette moved in one edit and cannot drift
into three versions again. The black bar and the dark product stage are
deliberately outside it: they are what the grey sits between. The product
ground itself went dark earlier the same day for the same reason a dark
object should not float on paper.
