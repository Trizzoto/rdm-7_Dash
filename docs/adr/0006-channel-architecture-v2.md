# ADR 0006 — Channel Architecture v2

**Status**: Accepted (2026-06-06). Implementation begins.
**Supersedes**: parts of widget signal-binding model (ADR-less, current v13 layout).
**See also**: `schema/canonical_channels.md` (canonical registry source-of-truth),
`docs/v2_channel_architecture.md` (detailed implementation plan).

## Context

The current architecture is widget-centric: each widget owns its signal
binding, display range, threshold values, and visual configuration. Users
configure the same data point repeatedly across widgets / screens / layouts.
Cross-ECU layout sharing is broken (the receiver's Haltech RPM signal has
a different name than the author's Link RPM signal). First-time setup
takes 20–40 minutes.

Three problems we need to solve:
1. **First-time setup pain** — new users abandon during configuration.
2. **Reconfig across screens** — change RPM redline in one place, want it
   reflected everywhere.
3. **Cross-ECU portability** — a layout authored on Haltech should run on
   Link, AEM, or Generic OBD2 without breaking.

## Decision

Introduce a **canonical channel registry** as a new abstraction layer
between signals (CAN decode) and widgets (visual presentation).

- A **canonical channel** is a well-known data identity (e.g. `rpm`,
  `coolant_temp`). The firmware ships a fixed registry of ~90 canonical
  channels covering engine, drivetrain, electrical, chassis, body, race,
  and OBD2-standard channels.
- The **user's channel configuration** lives in `/lfs/channels.json` —
  dash-owned, persistent, independent of any specific layout. Each
  channel has a signal source, display range, thresholds, units, colors.
- **Layouts reference channels by canonical ID**. Widgets carry a
  `channel` field pointing to a canonical ID. No per-widget signal name,
  range, or threshold needed.
- **Sharing layouts works automatically** because canonical IDs are
  universal. The receiver's dash auto-binds to its own channel for that
  ID. The author's Link redline at 7000 doesn't override the receiver's
  Haltech redline at 9000 — the receiver sees their own settings,
  visualized on the author's layout design.
- **Auto-create missing canonical channels** on layout import. If the
  layout references `egt_cyl_1` and the receiver hasn't configured it,
  the firmware creates a canonical-default entry; the widget renders with
  "—" until the user assigns a signal source. No similarity-matching
  algorithm; just a clear "this channel needs a signal" notification.
- **Custom channels** (user-defined, `custom_` prefix) ride along inside
  the layout file. On import, user opts in to recreate or skip.

## Why this over the alternatives

Eight architectures were considered (see brainstorm transcript). The
finalist alternatives:

- **Slot-based templates** (factory-dash model) — better for first-time
  UX but constrains layout freedom. RDM-7 is positioned for users who
  want a *canvas*, not a configurable preset. Rejected for the primary
  layout model; revisited as Phase 4 / 5 "template mode" layered on top.
- **Pure channels with similarity-matching wizard on every import** —
  considered first; rejected as too heavy. Algorithm is fine for compute
  but adds wizard friction to every layout download.
- **Widget presets** — solves the "configure once" problem but not
  cross-ECU sharing. Rejected as half-measure.

The chosen model is the only one that solves all three problems
simultaneously without constraining the design canvas.

## Consequences

### What gets built
- Firmware module `main/data/channel_manager.c/h` — owns channel state,
  JSON I/O, change-event listener pattern.
- Firmware module `main/data/canonical_channels.c/h` — generated from
  `schema/canonical_channels.md`. Read-only registry.
- New file `/lfs/channels.json` — user's channel configuration.
- Web UI new top-level structure: Setup mode / Design mode / Live mode.
  Channels tab lives under Setup. Migration from current single-mode
  editor.
- First-boot wizard rewrite to channel-aware flow.
- Each widget consuming channel data (meter, bar, panel, rpm_bar, arc,
  indicator, warning, text) gets a `channel_id` field and snapshot-on-
  channel-change behavior.
- Layout schema bumped v13 → v14.
- v13 → v14 migrator runs on layout load.
- ECU presets extended to seed channels (not just signals).

### What gets removed / cleaned up
Tracked in `docs/v2_cleanup_log.md` as we go. Initial expectations:
- Per-widget redundant min/max/threshold defaults become channel-derived;
  inspector fields collapse to "inherited from channel" badges.
- Some widget rules become channel events (threshold-crossed → event →
  all bound widgets react). Reduces per-widget rule evaluation overhead.
- ECU-specific signal naming on widgets disappears — replaced by canonical
  channel references.

### What stays
- Per-widget per-field override mechanism (escape hatch for custom range
  on a specific widget).
- Full free-canvas layout editor — widget position, size, visual style
  remain widget-owned.
- Signal subscription hot path unchanged — widgets still snapshot signal
  data per callback. Channel layer is metadata only, not in the
  per-frame path.

### Performance impact
Net zero or slightly positive. Channel events allow shared threshold
evaluation across multiple widgets bound to the same channel (today, each
widget evaluates independently). Memory cost ~15 KB total (registry +
user channels + per-widget snapshot). Hot path unchanged.

### User impact during migration
- Existing layouts auto-migrate on first load with the new firmware.
  Widgets get rebound to canonical channels matching their signal names.
  Inferred min/max/threshold values become the channel defaults.
- A one-time "your layout was upgraded — review the new Channels tab"
  prompt appears post-migration.
- Backwards compat: old `/lfs/channels.json`-less dashes get a baked-in
  Generic OBD2 channel set so default layout still renders.

## Implementation plan (5 phases)

1. **Foundation** — canonical registry, channel_manager, /lfs/channels.json
   I/O, migration code, one widget bound as proof. *4–5 days agent work.*
2. **Widget rollout + Web UI Channels tab** — replicate channel binding
   across all data-driven widgets; build Channels editor in Studio. *4–5 days.*
3. **First-boot wizard rewrite + Setup/Design/Live mode split** —
   onboarding flow, mode bar, layout templates. *3–4 days.*
4. **Layout sharing — auto-create canonical, import-modal for custom only**
   — marketplace metadata, embedded custom channel definitions, light
   import flow. *2–3 days.*
5. **Polish + cleanup pass** — Beginner/Pro mode toggle, inspector
   refinement, stale code removal, performance verification. *2–3 days.*

Total ~3 weeks calendar time including build/flash cycles and smoke tests.

## Rollback strategy

This commit (the one creating this ADR + the canonical channels markdown
+ all preceding shadow work) is tagged `backup/pre-channels-v2`. If
implementation goes off the rails:

```bash
git reset --hard backup/pre-channels-v2
```

The shadow feature, dynamic shadow, and pivot-anchored geometry all
remain intact at this tag.

## Open log

Decisions revisited and locked during planning:

- **2026-06-06**: Pressure tested against 8 alternative architectures.
  Slot-based factory-dash model was the strongest contender, rejected
  for v1 to preserve canvas freedom. Revisit as layered template mode in
  Phase 4/5.
- **2026-06-06**: Dropped similarity-matching algorithm for layout
  imports. Replaced with "auto-create canonical channels on import,
  user assigns signal source after." Simpler architecture, lighter UX.
- **2026-06-06**: Channels live at dash level, NOT inside layout JSON.
  Layouts only reference canonical IDs. This is the unlock for
  cross-ECU portability.
- **2026-06-06**: Canonical channel list locked at 90 channels (v1.1).
  Threshold model is bi-directional (low_critical / low_warn /
  high_warn / high_critical), each optional. Gear at Tier 1 with
  calculated-gear path universally available.

## Honest trade-offs accepted

1. First-time setup is 5–10 minutes, not zero. Wizard is short but
   exists.
2. Custom channels are second-class for marketplace sharing.
3. Channels concept adds conceptual surface area users encounter.
4. Cross-ECU layouts with semantic mismatches (8 EGT widgets on a
   single-probe dash) will work but look weird. No architectural fix —
   only good marketplace metadata helps.
5. Implementation effort is real — ~3 weeks. Worth it if marketplace
   layout-sharing economy grows.
