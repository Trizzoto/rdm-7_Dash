# RDM Studio — closing the analysis gaps (2026-07-29 plan; shipped 2026-07-30)

Seven gaps identified against top-tier tools (AiM Race Studio-class; see
`docs/research/2026-07-gps-laptimer-market.md` and ADR-0012). All seven were
built, in `rdm7-desktop`, on branch `claude/xenodochial-bhabha-883b42` —
merged into that repo's `master` via merge commit `5d2ade4`. Verified
2026-07-30: all six commit hashes in the table below are ancestors of
`rdm7-desktop`'s `master` (`git merge-base --is-ancestor <hash> master`).

**This document is now historical.** For the current state of Studio's lap
analysis — including substantial work that landed *after* this plan closed,
on top of the merge — see `rdm7-desktop/docs/LAP_ANALYSIS_REDESIGN_2026-07.md`.

## Why this order (still the right way to sequence this kind of feature set)

Persistence had to come first: history, comparison, and video are all
meaningless until a session is a stored object rather than memory that
vanishes when the node's download ring wraps. Units had to land before the
surfaces needing them multiplied (history charts, export headers, video
overlay text). Export and the track library were comparatively cheap once
persistence existed. Video was saved for last — the heaviest lift, and
nothing else was blocked behind it.

## What shipped

| # | Stage | Commit | What it closed |
|---|---|---|---|
| 1 | Session library — recordings persist | `f4c9fe6` | Sessions now survive a Studio restart; before this, `gp.trace` was memory-only and a re-download after the node's ring wrapped lost the data permanently |
| 2 | Units — metric / imperial | `e1ac3d5` | One formatter layer behind every speed/distance readout |
| 3 | Export — CSV + session file | `3a3ae52` | Per-lap/session CSV; `.rdmsession` for round-tripping between machines |
| 4 | History — per-track trend | `49fa0ad` | Best lap over time, consistency, corner-level improvement/regression — the trend view no incumbent shows |
| 5 | Track library — 108 circuits, searchable | `af45771` | Self-curated coordinates (not AiM's proprietary DB, not OSM's share-alike terms); community path still waits on the marketplace |
| 6 | Cross-session comparison — car, driver, day | `955ddad` | Compare any lap against any other lap in the library |
| 7 | Video — auto-sync, linked scrub, live gauges | — | Watch-side done (mvhd-clock auto-align, linked timeline, speed/delta/g overlay painted over footage); burned-in export and GPMF parsing deferred until a GoPro is on the bench |

## What's still open

Stage 7's deferred items (burned-in video export, GPMF parsing, multi-clip
sessions) — see `rdm7-desktop/docs/LAP_ANALYSIS_REDESIGN_2026-07.md` for
current status. That document also covers everything built *since* this plan
closed: auto-arm/track-recognition with a readiness panel, session
stint-splitting, a combined multi-channel plot with a draggable navigator,
bidirectional gate crossings, a lap-fixed y-axis, resizable telemetry lanes,
a zoom-aware map trace, a draggable rail/stage splitter, and a CAN
telemetry-channel picker (Studio half only — the node/firmware half is
specified there but unbuilt).

---

This plan's original per-stage design notes (IndexedDB record layout, GPS
dating strategy, GPMF field tables, ESP32/BLE detail) have been cut here —
they described intent, the shipped code in `rdm7-desktop` is now the
source of truth, and duplicating implementation detail in a historical doc
just gives it a second chance to go stale.
