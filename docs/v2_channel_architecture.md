# v2 Channel Architecture — implementation plan (2026-06-06; shipped)

**Status**: Shipped. This was the detailed implementation plan for ADR-0006;
the data model landed substantially as designed. Treat this document as
historical rationale, not a spec — the live registry, the ADR, and the code
below are the sources of truth now.

Pairs with:
- [ADR-0006](adr/0006-channel-architecture-v2.md) — the decision record (also
  covers "why this over the alternatives," consequences, and migration —
  read that first; this doc doesn't repeat it)
- [ADR-0005](adr/0005-channel-owned-decode.md) — channel-owned CAN decode
- [`schema/canonical_channels.md`](../schema/canonical_channels.md) — the
  live canonical registry data
- [`docs/v2_cleanup_log.md`](v2_cleanup_log.md) — was meant to track removals
  as this landed; wasn't kept up, see that file's header

## What this decided, in short

A **canonical channel** (`rpm`, `coolant_temp`, ...) sits between signals
(CAN decode) and widgets (visual presentation): the firmware ships a fixed
registry of well-known channel identities, the user's live configuration
lives in `/lfs/channels.json` (source, range, thresholds, units, colors),
and widgets reference a channel by canonical ID instead of owning their own
signal name/range/thresholds. That's what makes a layout shared between two
different ECUs auto-bind correctly, and what makes "change the redline once"
apply everywhere that channel is used.

## What shipped vs. what this plan got wrong or missed

- **Phase 1 (Foundation) and Phase 2 (widget rollout): done.**
  `main/data/canonical_channels.c/h` and `channel_manager.c/h` have existed
  since 2026-06-08; nearly every data-driven widget (`panel`, `bar`,
  `rpm_bar`, `meter`, `arc`, `indicator`, `text`, `warning`, `anim`) reads a
  channel via `channel_manager_get()` / `channel_id`, matching the plan.
- **The layout schema moved well past this plan's v14 target** — it's v17
  now (`LAYOUT_SCHEMA_VERSION` in `layout_manager.h` is the authority).
- **Two subsystems shipped that this plan never anticipated:**
  `main/data/channel_math.c/h` (derived/"calculated" channels, e.g.
  `boost = manifold_pressure - barometric_pressure`) and
  `main/data/unit_convert.c/h` (native→display unit conversion). Both are
  now part of the channel system's real surface — see CLAUDE.md's "Channel
  System" section.
- **Phase 3's specific UI ("Setup / Design / Live" three-mode top bar) did
  not ship as written.** The web editor has a two-mode `studioModeSwitcher`
  (`setup` / `design`) instead — the "Live" idea folded into a live-preview
  toggle inside Design mode rather than becoming its own top-level mode.
  `docs/STUDIO_SHELL_PLAN_2026-07.md` (2026-07-27) supersedes this plan's
  navigation thinking anyway, with a device/capability sidebar instead of a
  mode bar.
- **Phase 4/5 status (marketplace metadata auto-derivation, the "map signal
  to multiple channels" tool, beginner/pro mode toggle) wasn't re-verified
  in this pass** — check the web editor's Channels tab and `main/data/`
  directly rather than trusting either "done" or "not done" here.

## Design decisions still worth knowing (not duplicated in the ADR)

A few implementation-level choices from the original plan that a future
change should stay consistent with, unless deliberately revisited:

- **No similarity-matching on channel import.** A layout referencing a
  channel the receiving dash hasn't configured gets that channel
  auto-created with canonical defaults and an empty signal source — never a
  guessed binding. Explicit beats clever here.
- **Defaults-only JSON emit.** `channels.json` only serializes fields that
  differ from the canonical default, to keep it small and the diff against
  future canonical defaults visible.
- **Custom channels travel inside the layout file** (`embedded_custom_channels[]`),
  not as a separate share-out mechanism.
- **Per-widget override stays** as an escape hatch for one-off custom
  ranges, rather than making the channel binding all-or-nothing.

For current field names, JSON shape, and the actual API surface, read
`main/data/channel_manager.h` and `main/data/canonical_channels.h` directly.
