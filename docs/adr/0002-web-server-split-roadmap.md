# ADR 0002 — `web_server.c` Split Roadmap

**Status**: Implemented (2026-04-27), with deviations — see "What shipped" below.
**Context**: `main/net/web_server.c` was ~4750 lines and housed all 86 HTTP endpoints, their helpers, the captive-portal probe handlers, the OPTIONS wildcard, and the registration block. Every endpoint addition showed up as a diff in the same file; merge conflicts were common; reasoning about a single domain (e.g., layout management) required holding the whole file in your head. There was no automated coverage of the 86 endpoints, so a regression in (say) `/api/layout/save` would be silently bad — the editor would save without surfacing the failure.

## The decision

Split `web_server.c` by domain into `main/net/web_server_<domain>.c` files, each with its own `<domain>_register(server)` entry point called from `web_server_start()`, sharing common helpers (the path-traversal guard, the layout-too-large sender, the `REGISTER_URI` macro, the registration tally) through a new `web_server_internal.h`.

Do the cuts as **separate commits, one domain at a time, with a human at a keyboard** who can run `idf.py build` and exercise the endpoints after each step — not as part of a "do many things at once" pass. The cuts are mechanically safe (no handler depends on a static helper from a different domain), but verifying that safety requires either an exhaustive read of the file or a build environment to catch a missed `static` helper.

Rules for whoever does a cut like this (still applicable to future domain splits):

- **Don't rename handlers** while moving them. Keeps `git log --follow` useful and avoids stacking risk on top of a mechanical move.
- **Don't change endpoint behaviour** in the same commit as the move — bug fixes go in their own commits, before or after.
- **Don't combine unrelated cleanups.** "While I'm here" is how splits become unreviewable.
- **Don't merge the captive probe URIs into one handler.** Each platform's probe URL is deliberately its own entry; see [ADR 0001](0001-wifi-onboarding-reliability.md).
- **Don't split the OPTIONS wildcard** out of `web_server.c`. It must stay registered last so it doesn't shadow the specific paths.

After each step: build, flash, and watch the boot log's `URI registration: N handlers registered (cap M)` line — a dropping count means a `_register()` call was forgotten — then exercise representative endpoints from the new domain via `curl` or the studio.

## What shipped

13 of the 16 planned files landed: `web_server_internal.h`, `web_server_captive.c`, `web_server_gear.c`, `web_server_touch.c`, `web_server_capture.c`, `web_server_signals.c`, `web_server_assets.c`, `web_server_logger.c`, `web_server_system.c`, `web_server_wifi.c`, `web_server_ota.c`, and `web_server_layout.c`. `web_server_storage.c`, `web_server_presets.c`, and `web_server_splash.c` were never split out as separately-named files — their endpoints remain elsewhere in the current tree.

`web_server.c` itself is down to ~409 lines (from ~4750) — close to the ~300-line target, and it also now dispatches to domains the original plan never anticipated: `web_server_lap.c`, `web_server_obd2.c`, `web_server_test.c`, and `web_server_channels.c` were added later as those features shipped, following the same per-domain pattern this ADR established. The pattern outlived the specific plan; across all `web_server_*.c` files combined there are now 144 `REGISTER_URI(` calls (see [ADR 0004](0004-performance-budgets.md) §1.11 for the handler-cap budget this counts against).

## Related

- [ADR 0001 — Wi-Fi Onboarding Reliability](0001-wifi-onboarding-reliability.md) — the captive probe URIs this split relocated into `web_server_captive.c`.
- [ADR 0004 — Performance Budgets](0004-performance-budgets.md) §1.11 — the HTTP handler cap this split's `REGISTER_URI` tally protects.
