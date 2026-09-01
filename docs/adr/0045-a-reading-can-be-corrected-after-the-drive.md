# ADR-0045: A reading can be corrected after the drive

Date: 2026-08-21
Status: Accepted
Repos: rdm7-desktop (GPS workspace)
Completes [0044](0044-a-recording-carries-its-own-meaning.md), which froze a
recording's channel definitions at download and would otherwise have frozen
mistakes with them. Rests on [0008](0008-gps-lap-timing-integration.md).

## Context

ADR-0008 has the puck log **raw CAN counts**. ADR-0044 has each recording carry
a snapshot of what those counts mean, taken at download, so a log is readable
years later with nothing plugged in.

Both are right, and together they create a problem neither has on its own: if
the definition was **wrong** at download time, ADR-0044 preserves the mistake,
permanently, in every copy of that recording. The snapshot is faithful to a
library that was itself incorrect.

That is not hypothetical. A definition is wrong whenever the scale was guessed,
whenever a channel was bound to the wrong signal in a DBC, and — the one that
looks like working data rather than like an error — whenever `is_signed` is
false on something that goes negative, which turns −6° of ignition advance into
65530 without anything on screen looking broken.

The cost of getting it wrong used to be a track day. There was no way to say
"that scale is wrong" short of re-binding the channel on the dash and driving
again — even though **the raw counts were sitting right there**, unchanged and
perfectly good, because the puck never applied the scale in the first place.

## Decision

**A wrong scale is a wrong label on data that is still there, so it is a label
you can change.**

1. **Corrections are a store, separate from both the library and the
   recording.** `rdm7_gp_chanfix`, keyed by channel id, holding name, unit,
   decimals, scale, offset and signedness.

2. **A correction outranks the recording's frozen definition.** Resolution
   order is library → the recording's `chanDefs` → global correction →
   per-recording correction. The freeze records what the library said *then*; a
   correction is the user saying that was wrong. Later and deliberate beats
   earlier and automatic. Without this rule ADR-0044 is a trap.

3. **Two scopes, because there are two different mistakes.** *Everywhere* — the
   definition was wrong, so every recording using that channel is corrected at
   once. *This recording only* — the definition is right now, but this log
   predates a change to the car. Saving at the wider scope clears the narrower
   one, or the narrower one keeps winning and the save appears to do nothing.

4. **The wire half is never correctable.** `can_id`, start bit and width chose
   which bits the puck copied, months ago; those are history, not settings, and
   offering to "fix" them after the fact would be offering to change what was
   recorded. They are carried through a correction untouched, which is also
   what keeps a corrected channel loggable.

5. **The form is built around the answer, not the inputs.** It shows the
   column's raw counts in the open recording and what they *would* read under
   whatever is in the boxes, plus one chip per plausible scale with its
   resulting range printed on it. You do not have to know the ECU's scaling to
   use it; you have to recognise a throttle when you see one, and ×0.1 giving
   0–99.8 next to ×1 giving 0–998 is not a hard choice. Nothing is chosen for
   you — a fitted scale is a guess with a number on it, so the chips offer and
   the user decides.

6. **A corrected channel says so**, on its row, next to the wire position.
   "Why does this read differently from the dash" needs an answer on screen
   rather than in someone's memory.

## Consequences

- Half a track day's channels being wrong costs a minute at the kitchen table
  instead of another session. Nothing is re-downloaded, because nothing needs
  to be: the counts never changed.
- Every correction is reversible, and removing one drops straight back to what
  the library says.
- A recording exported to CSV or VBO carries the corrected values, because the
  exporters read lanes and lanes read the resolver.
- The correction store is local to the PC. A recording shared with someone else
  carries its frozen `chanDefs`, not your corrections — which is the right
  default (they get what it was logged as) but is worth knowing before blaming
  the other machine.
- `tools/check_chanfix.js` covers the precedence chain, both scopes, what a
  correction must not touch, and the preview arithmetic including the signed
  case where the range has to be *measured* rather than derived — sign
  extension reorders the samples, so the signed range cannot be computed from
  the unsigned one.
