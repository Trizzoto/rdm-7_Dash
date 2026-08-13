# ADR-0035: The Source column speaks provenance, not plumbing

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`)
Follows 0030–0034. Prompted by the owner asking whether the workflow now
matches the model — "you've got the channels and then you apply sources to
the channel, none of this extra signal stuff" — and an audit of where the
internal signal layer still leaked into the user's face.

## The audit

The core loop held up: widgets bind channels (the inspector's channel card
is the primary path; the raw-signal branch renders only for legacy widgets,
labelled as such, with an "Assign channel" upgrade button). Three leaks
contradicted the model:

1. **The Channels table's Source column printed the registry slot name** —
   `COOLANT_TEMP`, `IGNITION` — the one column whose entire job is to answer
   "where does this come from". Root cause was structural, not cosmetic: the
   merged canonical rows never copied `source`/`decode` from the live record,
   so the badge classifier had nothing to read and the renderer fell back to
   the raw name.
2. **Ctrl+Shift+S opened the legacy Signal Manager** — a keyboard shortcut
   straight into the surface the channels model replaced.
3. **The legacy signal-pick list's empty state** still told users to "use ECU
   Presets", a card that no longer exists.

## Decision

The Source cell carries the provenance badge (OBD2 / CAN / CALC / RDM-7 /
unbound) plus supplementary text only where it adds identity:

- **CAN** → the frame id (`0x360`) — the one fact that tells two same-named
  decodes apart, same rule as the pickers.
- **OBD2 / RDM-7 / CALC** → badge alone; the channel's own label already
  names the quantity, and `COOLANT_TEMP` under "Coolant Temp" said nothing.
- **`unknown`** (the firmware's honest answer when the bound signal isn't
  registered this boot — its frames simply haven't arrived) → keep the raw
  signal name, so the cell never goes blank. Blank under a "Source" heading
  reads as "no source", which is the one wrong answer.
- **Unbound** → the badge says so; previously the cell was empty.

Ctrl+Shift+S now opens the Channels page. The stale empty-state copy says
what the list actually is: a legacy surface for raw-signal widgets.

Search still matches the signal name — the data rides in the row, it just
stopped being the headline.

## What was deliberately left

- **Live Data & Logging keys by signal name.** That page shows the raw feed
  and the logger records signals; renaming rows to channel labels would be
  cosmetic there and misleading where several channels share a feed.
  Defensible as-is; revisit only if a customer trips on it.
- The Signal Manager itself stays reachable from the DBC/preset flows, where
  it is genuinely the ECU-preset browser and custom-signal author — that is
  source management, which is the model.

## The device editor, same rule (added 2026-08-14)

The dash's own channel pane had the identical leak: `via COOLANT_TEMP` under
a channel already labelled "Coolant Temp". It now prints provenance —
`OBD2`, `RDM-7 internal`, `CAN 0x360` — mirroring the derivation in
`channel_to_full_json()` rather than inventing a second rule: registry
source when the signal is registered, decode/OBD2-name fallback when it is
not, and `NAME (waiting)` when a channel is bound to something whose frames
have not arrived this boot. Both editors now answer "where does this come
from" the same way.

## Verification

Against the live dash through the `--device` proxy: 32 rows — 7×OBD2,
16×RDM-7, 6×unbound badges, 1 GEAR + 2 raw names for genuinely-unknown
provenance, **zero blank cells**; hotkey lands on the Channels page with the
Signal Manager closed; 0 console errors. Device pane checked on the glass
over remote touch.
