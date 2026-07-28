# Architecture Decision Records

An ADR captures a single significant architectural decision — the context that forced it, the options considered, the choice made, and the consequences accepted. ADRs are immutable once accepted: when reality changes, write a new ADR that supersedes the old one rather than editing the original.

Read the relevant ADR before changing the area it covers. The "we already tried that" answers live here.

## Index

| # | Status | Title | Touches |
|---|---|---|---|
| [0001](0001-wifi-onboarding-reliability.md) | Accepted | Wi-Fi onboarding reliability — the layered fixes phones need | `main/net/wifi_manager.c`, `main/net/web_server_captive.c`, `main/net/dns_hijack.c`, `sdkconfig` |
| [0002](0002-web-server-split-roadmap.md) | Complete | Splitting the monolithic `web_server.c` by concern | `main/net/web_server*.c` |
| [0003](0003-desktop-index-sync-plan.md) | Implemented | Plan for syncing `rdm7-desktop/src/index.html` with the firmware copy | `../rdm7-desktop/src/index.html`, `../rdm7-desktop/src/transport.js` |
| [0004](0004-performance-budgets.md) | Proposed | Documented performance budgets (heap, OTA partition, URI handlers, layout JSON) | repo-wide |
| [0005](0005-channel-owned-decode.md) | Accepted | Channel-owned CAN decode for portable layouts (decode moves layout `signals[]` → `channels.json`) | `main/data/channel_manager.c`, `main/layout/*`, `schema/canonical_channels.md` |
| [0006](0006-channel-architecture-v2.md) | Accepted | Channel architecture v2 — canonical channel registry as the binding layer | `main/data/canonical_channels.c`, `main/data/channel_manager.c` |
| [0007](0007-html-source-of-truth.md) | Accepted | Three HTML copies — why they exist and the codegen plan to collapse them | `main/web/index.html`, `../rdm7-desktop/src/index.html`, `schema/widgets.schema.json` |
| [0008](0008-gps-lap-timing-integration.md) | Accepted | GPS lap timing — how the puck, the dash and the desktop suite fit together | `main/lap/`, `main/data/canonical_channels.c`, `main/can/can_manager.c`, `../rdm-gps-node/` |
| [0009](0009-rdm-io-mixed-precision-frontend.md) | Accepted | RDM IO mixed-precision analog front-end (4× 16-bit ΔΣ + 4× 12-bit) + PT Motorsport benchmark | `docs/PLATFORM_PLAN_2026-07.md` §6.3, `../rdm7-desktop/src/tauri-overlay.html` (IO workspace) |
| [0010](0010-rdm-io-three-tier-ladder.md) | Accepted | RDM IO three-tier ladder — Pico A$89 / Core A$219 / Pro A$449–579, shared firmware + emulation modes | `docs/PLATFORM_PLAN_2026-07.md` §4/§6.3, future firmware `profiles/` layer |
| [0011](0011-analyzer-no-synthetic-data.md) | Accepted | The CAN analyzer never fabricates traffic — demo generator deleted, every empty state names its cause | `../rdm7-desktop/src/tauri-overlay.html` (analyzer workspace), `main/net/web_server_test.c`, `main/net/web_server_obd2.c` |
| [0012](0012-corner-phase-attribution.md) | Accepted | Corner-phase time attribution (braking/entry/apex/exit), the lane rack, and the optimal-lap patent boundary | `../rdm7-desktop/src/tauri-overlay.html` (Session workspace) |
| [0013](0013-one-course-type.md) | Accepted | One course type — a finish line is a thing you add, not a mode you pick; the session splitter learned the second line | `../rdm7-desktop/src/tauri-overlay.html` (Tracks + Session), `main/lap/lap_core.{c,h}` (unchanged, documented) |

## When to write a new ADR

- Pick a decision that **will be questioned again**. Anything you'd find yourself re-explaining to a future contributor.
- One decision per ADR. If you're tempted to bundle two unrelated choices, that's two ADRs.
- Don't ADR every commit. The threshold is "future me will not understand why we did it this way."

Good ADR candidates that aren't yet written (open invitation):

- The dual-object pattern for night-mode LVGL v8 baked-in properties (`widget_image`, `widget_meter`, `widget_warning`).
- Why the embedded web editor is one 14k-line HTML file rather than a Vite/Webpack bundle — partially covered by ADR 0005, but the build-step trade-off itself is worth its own record.
- Why widget config JSON is "defaults-only" for the 32 KB layout budget.
- Why the layout schema migration helper is a static switch rather than a registry.

## File format

```
# ADR NNNN — short imperative title

**Status**: Accepted | Proposed | Superseded by NNNN | Deprecated
**Context**: One paragraph framing the problem.

## The problem we were solving
...

## Options considered
...

## Decision
...

## Consequences
- Good: ...
- Bad: ...
- Neutral: ...

## References
- Code: paths
- Commits: SHAs
- Related ADRs: NNNN
```

Existing ADRs vary slightly from this skeleton — none rigorous. Match the surrounding style, prioritise readability over template adherence.

## Numbering

Strictly sequential. The next ADR is `0014`. Don't reuse numbers, even if a draft is abandoned — leave a stub if needed (`0011-abandoned.md` with one line of explanation).
