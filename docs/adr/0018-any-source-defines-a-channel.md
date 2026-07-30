# ADR-0018: Any source can define a channel, and the marks are not tickboxes

Date: 2026-07-30
Status: Accepted
Repo: rdm7-desktop (GPS workspace). Supersedes the "dash is the configuration"
half of ADR-0017's context.

## Context

Two pieces of direct feedback on the channel list from ADR-0017.

**1. "The check boxes are pretty ugly."** Correct, and worse than a taste
problem: twenty accent-filled squares on one screen breaks this app's own
design law. ADR-0014 says chrome is monochrome, the accent *marks* the active
thing rather than painting it, there is at most one filled primary per
surface, and colour belongs to data and state. A grid of solid `--accent`
tickboxes violates every clause of that.

**2. "The CAN bus doesn't have to be the dash — it can be any. We can create
custom channels or upload DBC or anything. But if someone's already set up a
dash then they're probably already going to have the channel set up too."**

Also correct, and this one was a functional dead end, not a cosmetic one. The
list read `/api/channels` from a connected RDM dash and nothing else, with copy
that said the dash "is the thing that knows what the bus means". So the entire
CAN-logging feature was unavailable to anyone whose car has someone else's
dash, or no dash yet, or a dash that simply is not plugged in at the moment.
The puck sniffs whatever is on the wire; it never cared who described it.

## Decision

**The marks.** Neither column is a checkbox.

- **Graph** is a short rule in the channel's *own trace colour* — the control
  looks like the line it governs, and doubles as the legend. That colour is
  legitimate under ADR-0014 rule 7 (data keeps its colour): it is data
  identity, not chrome.
- **Log** is a small neutral square, filled when the puck records it. State,
  in greys, calm.
- Locked ("always recorded") is a *dim fill*, and not-applicable is a bare
  dash with no box at all. Both must not read as an empty box awaiting a
  click, which is the opposite of what they mean. First attempt set the base
  glyph colour to transparent and both states rendered invisible, so "always
  recorded" looked identical to "not recorded"; the second attempt dimmed the
  lock to 0.7 opacity and it read as an empty box at 9 px. Caught both by
  zooming into the running app.
- The hit target is the whole cell, and hover elevates a neutral background —
  the same "active is an elevated neutral" idiom as the tabs and rails.

**The sources.** A channel definition is `{name, unit, decimals, decode:{
can_id, bit_start, bit_length, is_signed, endian, scale, offset}}` and can come
from any of three places, unified by `gpAllChans()`:

1. **A connected dash** — first, because it needs no setting up. If you own one
   it already has this car's bus decoded and named, which is exactly the
   reasoning in the feedback.
2. **A DBC file** — imported in Studio, reusing the firmware editor's existing
   `parseDbcFile` rather than carrying a second parser (it already handles the
   29-bit extended-id marker, and two parsers would eventually disagree about
   one file). Signals wider than the puck's 16-bit slot are excluded, counted
   and named in the confirm rather than dropped silently.
3. **Typed in here** — the same fields a DBC line carries, because that is the
   vocabulary anyone doing this already speaks. Validated before storing, with
   the reason: a definition that silently logs nothing is the worst outcome,
   because you find out after the track day.

Locally-defined ids are prefixed `my:` so they can never collide with a dash
channel — the id is what goes into the log selection and into a saved session,
so a collision would silently relabel someone's data. Rows are grouped by
source ("known by the dash" / "from a DBC file" / "you added these"), which
answers "where did this come from" without a legend, and each row prints its
frame and bit position — the only way to tell two same-named signals apart or
to spot a typo in one you typed.

## Consequences

- The feature works with no RDM dash present at all, which is most cars.
- A dash is still the fast path and is still offered first; nothing about that
  case got worse.
- Definitions you own can be deleted; deleting one that is ticked for logging
  unticks it, or the selection would keep an id nothing can name.
- `gpRingMinutes` was reading `capacity_samples` as always being for a 12-byte
  record. The node reports it for its *current* record, so once channels were
  actually sent the estimate shrank on its own: two channels on the puck read
  206 minutes when the partition still held 274. Now multiplied by the
  `record_bytes` the node reports alongside it. Found by watching the number
  change after a Send it had no business changing.

## Verification

227 harness checks (`tools/check_autotrack.js`), including: definitions offered
with no dash connected, per-source grouping, wire position in the row, `my:`
prefixing, CAN-id parsing in every form people type it (`0x360`, `360h`, bare
hex, decimal), every validation refusal and its reason, the form naming both
endianness options instead of On/Off, numeric fields selecting on focus, and
the marks carrying no accent fill.

Live on the bench, driving the laptop, with no dash attached:
- Added "Oil pressure" by hand → appeared grouped as yours, with `0x5F0 · bit
  8+16`, persisted across a reload.
- Imported a nine-signal DBC → "Add 8 channels from testcar.dbc?" (the one
  32-bit signal named as excluded), all eight arriving with the frame and bit
  positions the file specifies, including `BO_ 2566844926` correctly masked to
  `0x18FEF1FE`.
- Ticked a hand-typed channel and a DBC-imported extended-id/big-endian one,
  **sent both to the real puck**, and the node reported "2 channels · matches
  what's ticked", 4 bytes a sample, 274 min. Pressing Refresh re-read the
  table off the hardware and recovered both names by wire signature.
- Puck restored to 0 channels / 12-byte record afterwards; its reported
  capacity (548352 × 12 = 6.58 MB) confirms the `gpRingMinutes` arithmetic.
