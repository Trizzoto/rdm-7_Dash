# ADR-0033: The catalogue travels with the page, and the poll is the watchdog

Date: 2026-08-13
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`, `main/ui/settings/preset_picker_data.c`,
`tools/native/gen_channel_catalog.c`, `tools/gen_channel_catalog.py`, CI),
rdm7-desktop (resynced base)
Follows ADR-0030/0031/0032. This is the "executed well" pass over the same
customer feedback.

## Context

Re-reading the customer's words against what had shipped found three places
where the letter was delivered and the substance wasn't.

**"It prompts when coming back online" — only if you reopened the page.**
The reconnect offer lived in `_fetchChannelsActive`, which runs on page or
modal *entry*. The 2 Hz poll swallowed every failure and never touched
`_chOnline` in either direction. Sitting on the Channels page: a dash dropping
left the pill claiming "Live on the dash" while edits sailed into a dead
socket; a dash returning never prompted. The tune-file model existed, but only
at the doorway.

**"Not possible to set up channels while offline" — still true from zero.**
The mirror+queue machinery (ADR-0030) needs a mirror, which needs one prior
connection. A fresh install — RDM Studio downloaded, dash still in its box —
had no canonical catalogue (the desktop local stub honestly answers an empty
set), no ECU list, and a queue that didn't render without a mirror to replay
over. The one user the offline story should serve best got a blank page.

**"Most of the settings related to channels are on that one page at the top" —
the top bar had a count and a menu.** No ECU preset, no bus state, nothing
car-level. That sentence of the feedback had been marked done and wasn't.

## Decision

### The catalogue is baked into the page, with drift made structural

`main/web/index.html` now carries `window.RDM_BAKED_CATALOG`: the full
canonical channel registry (135 entries) and the full ECU preconfig table
(350 rows), ~92 KB of JSON between AUTO-GENERATED markers.

The generator is the part that matters. `tools/native/gen_channel_catalog.c`
is a host tool that **links the real firmware translation units** —
`canonical_channels.c` and the new `preset_picker_data.c` — and prints their
contents as JSON. It parses nothing and copies nothing; it *is* the firmware
table, run on the host. `tools/gen_channel_catalog.py` compiles it, injects
the output, and `--check` recompiles + byte-compares in CI next to the other
codegen guards. There is no second copy to keep in sync, only one array read
twice. (Same mechanism as `tests/native` compiling `unit_convert.c` — the
house already trusted host-compiled firmware C.)

To make the preconfig table host-compilable it moved out of LVGL-entangled
`preset_picker.c` into `preset_picker_data.c` — pure code motion, one new
line in CMakeLists, and `check_preset_signedness.py` repointed (it hard-errors
on zero rows, exactly as designed; it fired the moment the table moved).

Fallback order everywhere canonical data is consumed: **live dash → mirror →
baked**. The mirror wins over baked when present because it carries the
connected dash's exact firmware build; baked is the floor that makes
from-zero work.

### From-zero setup actually composes

- The catalogue browses, previews (ADR-0032) and **adds** offline — the
  activate queues, and the queue now replays over an *empty* set when there is
  no mirror, so a channel added before ever meeting a dash survives reload.
- "From a pre-defined ECU…" works offline: the picker rebuilds its
  make→version→signal structure from the baked preconfig rows, and the import
  queues `/api/channels/import-preset` **verbatim** — replay runs the
  firmware's own alias resolution, the same reasoning that put the resolver in
  firmware in the first place (ADR-0030).
- Queued imports render as a banner — "Waiting for the dash — 41 channels
  from Link ECU Generic Dash will be added when it reconnects" — never as
  invented rows, because canonical-vs-custom resolution is the dash's call
  and the page must not guess it.

### The poll is the connection watchdog

`_pollChannelLive` now owns both transitions, in place: on failure while
online it flips the pill, blanks every value (a stale number shown as current
is how someone trusts a reading the car never sent), disables the dash-only
controls; on success while offline it runs the full reconnect path, which
fires the send-your-changes prompt.

And the bug underneath the bug: the poll's kill-switch still gated on the
**modal** being visible. On the page mount `channelsModal` is `display:none`,
so the very first tick silently killed the loop — the page had been living on
its entry fetch, with no live values and no watchdog, since ADR-0031. Found
because the reconnect prompt refused to fire in the flip test. The gate now
accepts either mount.

### The context strip

Above the table, page mount only: **ECU preset** (click opens the preset
picker; "None — pick one" when unset), **CAN bus** state, **N of M signals
live**. ECU and bus state fetch on entry and on reconnect — they change
rarely and deserve no timer; the live count rides the existing poll for free.
Offline the strip shows em-dashes and the ECU chip disables — the pill above
it already explains why.

## Consequences

- index.html grows ~92 KB raw (gzip crushes repetitive JSON; flash headroom
  ~515 KB). The desktop dist inherits the catalogue through the normal sync.
- `schema-check.yml` gains the catalogue guard and watches the two data files
  and both tool sources.
- A dash with an *older* preset catalog than the page can refuse a replayed
  import; `_chSendQueue` reports it with the firmware's reason instead of
  pretending ("Sent 2, 1 refused by the dash").
- The offline menu note now names the one thing that still needs the dash:
  restore-from-file.

## Verification

Three environments, console errors trapped throughout, zero seen.

**From zero** (`--local-stub` dev-server mode — new flag simulating RDM
Studio's local mode with no dash; localStorage cleared first): page opens with
135 baked canonical channels and an honest "Offline · nothing saved yet" pill.
Browsed the catalogue, previewed `oil_pressure`, added it (queued), set its
low-warn to 1.5 bar (queued as 150 kPa native — the display-unit contract
held), imported all 41 Link ECU signals (queued, banner shown). Reloaded:
everything intact, values blanked, "Offline · 3 changes waiting".

**The flip** (server killed/started under a sitting page): recovery noticed
within a tick, prompt fired in place, "Send to dash" replayed — 2 sent and
confirmed server-side, the Link import refused by the mock's smaller catalog
and reported with its reason. Then the drop: pill flipped, values blanked, ECU
chip disabled, poll alive — in ~1.5 s, no reload.

**Real dash** (RDM-DCB4-D926 via `--device` proxy): strip reads live truth —
"ECU preset: None — pick one · CAN bus: connected · 9 of 28 signals live" —
chip opens the preset picker (11 presets). 32 rows, poll alive on the page.

311 native tests pass; `check_preset_signedness` agrees on 98 rows over the
moved table; `gen_channel_catalog.py --check` green; firmware builds and the
desktop merge holds all 28 anchors.
