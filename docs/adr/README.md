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
| [0005](0005-html-source-of-truth.md) | Accepted | Three HTML copies — why they exist and the codegen plan to collapse them | `main/web/index.html`, `../rdm7-desktop/src/index.html`, `schema/widgets.schema.json` |

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

Strictly sequential. The next ADR is `0006`. Don't reuse numbers, even if a draft is abandoned — leave a stub if needed (`0006-abandoned.md` with one line of explanation).
