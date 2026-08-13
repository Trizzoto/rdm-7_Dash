# ADR-0034: Removal returns a channel to the catalogue, on every surface

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/data/channel_manager.*`, `main/net/web_server_channels.c`,
`main/ui/screens/first_run_wizard.c`, `main/web/index.html`), rdm7-desktop
(resynced base)
Supersedes, in part, ADR-0032's "canonical channels remain undeletable" and its
"adding is permanent" wording. Closes that ADR's flagged open question.

## Context

ADR-0032 closed the accidental-add trap but left its flagged question open:
`channel_manager_delete()` refused any non-custom id, so a **deliberately**
added canonical channel still could not be removed. Prevention removed the
accident; cleanup stayed impossible. Someone who imports an ECU's worth of
channels and changes their mind holds those slots (of 128) until they restore
a whole-registry backup — the manoeuvre this session performed by hand three
times, which is exactly the evidence it shouldn't be the product's answer.

The stated blast-radius worry was "widgets bind by channel id". Examined, it
dissolves: a canonical channel's **definition** is const data in flash and
immortal. Removing its **record** simply returns it to the same
"in the catalogue, not set up" state that most canonical channels live in
from the factory — a state every consumer already handles, because 100+ of
135 are in it on a fresh dash. A widget bound to a removed channel waits,
identically to a widget bound to a channel never added. There was no hard
constraint, only a guard written when activation was one accidental click —
which ADR-0032 had already fixed.

The device editor had the mirror-image problems: the tap-to-activate trap
(worse on glass, where a scroll flick lands as taps), **no** remove of any
kind, and — found while adding removal — rows holding `const char *` pointers
**into the channel record itself** (`channel_t::id`), documented as safe
"for the channel's lifetime". Removal ends that lifetime; every reference
would have dangled (the rpm-bar stale-globals class).

## Decision

**Firmware.** `channel_manager_remove(id)` removes any active record;
canonical returns to catalogue state. `channel_manager_delete()` keeps its
custom-only contract unchanged, so no existing caller's behaviour widens by
accident — reaching removal requires naming the new function.
`POST /api/channels/delete` gains `allow_canonical: true` (single and bulk):
absent, canonical ids keep their old 403/skip semantics, so older clients are
untouched; a pre-ADR-0034 firmware receiving the new flag ignores the unknown
key and still 403s, which the web reports in the firmware's own words.

**Web.** The detail footer's custom-only "Delete" becomes one action with two
honestly different fates: "Remove from this dash…" for canonical (confirm
says it returns to the standard list and can be re-added; source, thresholds
and unit choices are discarded) and "Delete…" for custom (gone for good).
Select mode now covers every active channel; the bulk confirm counts the two
fates separately ("3 built-in channels return to the standard list; 2 custom
channels are deleted for good"). Removal queues offline like every other
mutation and replays with the flag. ADR-0032's preview note stops claiming
permanence: "You can remove it again from its editor — removing returns it
to this list."

**Device editor**, brought to the same rules as the web:

- Tapping a ghost row **selects** it; the detail pane renders a preview
  (units, typical range, group, notes) with an explicit "Add this channel"
  button carrying the old tap-activation body (activate → resolve →
  canonical auto-bind → persist decode).
- Every active channel's pane gains "Remove from this dash" — two taps, no
  timer: the first arms and relabels ("Tap again to remove"), any pane
  re-render disarms. Timer-free by design; the settings-screen timer
  lifecycle is a known crash class.
- Rows now copy their id into a wizard-owned pool (144 × 32 B of .bss)
  instead of pointing into the record. This fixes a **pre-existing** hazard,
  not just a new one: a custom channel deleted from the web while the device
  editor sat open already left its row's user_data dangling.

## Consequences

- The full lifecycle is closed on all three surfaces: browse → preview →
  add → configure → remove → re-add. Nothing about channels is one-way
  anymore except deleting a *custom* channel, which the copy says plainly.
- ADR-0032's prevention rule stands unchanged — browsing must not mutate —
  it just protects slots that are now reclaimable instead of irreplaceable.
- `channels.json` restore remains the disaster-recovery path, no longer the
  only cleanup path.
- A removed canonical channel's thresholds/source are genuinely discarded;
  re-adding starts from canonical defaults. The confirm says so.

## Verification

- Firmware: builds clean; 311 native tests pass.
- Web against the flashed dash: removed active canonical `ambient_temp` —
  count 32→31, confirm text as designed; re-added it from the catalogue
  preview, 31→32, **source starting clean** — the removed record owned its
  decode, so the binding is genuinely discarded and the preview's "pick its
  source afterwards" is the truth, not a caveat. Bulk Select over a mixed
  canonical+custom set reported the two fates separately; original
  32-channel setup restored byte-identical afterwards.
- Device editor driven over remote touch with screenshots: ghost tap shows
  the preview (no activation — count unchanged), Add creates and binds,
  Remove arms on first tap and removes on second, list rebuilds with the row
  back in ghost styling.
- Offline: queued canonical remove renders the row back to ghost from the
  replayed view and posts `allow_canonical` on reconnect.
