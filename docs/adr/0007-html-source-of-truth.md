# ADR 0007 — Three HTML copies: why they exist and the plan to collapse them

> Renumbered from 0005 → 0007 (2026-06-11): the channel ADRs (0005 channel-owned
> decode, 0006 channel architecture v2) had already claimed 0005/0006.

**Status**: Accepted — **the migration plan below has landed.** `rdm7-desktop` commit `355f5ff` (2026-07-09) built it, citing this ADR by name in the commit message, about four weeks after this ADR was last edited. It shipped under different tool names than predicted here — Python instead of a Cargo `build.rs`, `src/dist/index.html` instead of `src-tauri/target/web/index.html` — but the shape (one edited source, a delta as an anchored overlay, a build step that fails loudly on drift) is exactly what was proposed. See the update appended to "The migration target" section below.
**Context**: The same web editor HTML used to live in two hand-synced places (formerly three), the copies drifted, and "why don't we just have one file?" surfaced every few sessions. The mechanism that answered that question is now built (see below); the historical reasoning is kept here because the underlying tension — firmware ships one file, desktop ships a Tauri app with its own needs — hasn't gone away, only the sync mechanism has changed.

## The three copies

| Copy | Path | Role | Toolchain |
|---|---|---|---|
| Firmware | `main/web/index.html` | Single source of truth. Embedded into the firmware binary at build (gzipped → 181 KB, served at `/`). | ESP-IDF / CMake |
| Desktop | ~~`../rdm7-desktop/src/index.html`~~ — now `src/firmware-base.html` + `src/tauri-overlay.html`, merged to `src/dist/index.html` (see the 2026-07-09 update below) | Tauri app's frontend. Loaded by the Tauri webview when the user launches the desktop app. | Tauri / Cargo, plus a Python merge step |
| ~~Browser~~ | ~~`../rdm7-studio-web/public/index.html`~~ | (Historical. Now serves the firmware copy directly via the WASM editor and a static host.) | — |

The firmware and desktop copies share **~95% of their content**: every widget definition, every property field, every drag/snap/inspector handler, every signal modal. They diverge on **~300 lines of Tauri-specific UI** that has no meaning on the device:

- USB-serial transport selector + transport.js wiring
- Device Manager modal (memory / OTA partition / WiFi diagnostics)
- Native file dialogs (Save As, Import) via Tauri's `saveFileDialog`
- ZIP backup/restore powered by a Rust command
- Auto-updater banner + manifest fetch
- Serial Logs drawer (USB-only — pipes ESP_LOGx through CDC ACM)
- Download progress overlay for OTA flashes
- WASM preview hooks for offline layout testing

There's also one **intentional omission**: the desktop OBD2 menu does not include "Custom PIDs," because the Custom PIDs editor flow is firmware-only for now.

## Why we have copies at all

Each repo serves a different distribution channel:

- **Firmware** ships flashed to the device. Browser fetches the HTML from `/` over WiFi or hotspot. No build step beyond CMake's `EMBED_FILES`. Single-file gzipped HTML is the smallest, fastest, most rugged form for this.
- **Desktop** ships as a signed installer (NSIS / MSI). Bundles a Rust binary with its own webview, file system access, USB-serial transport, auto-updater. The webview loads `index.html` from disk at startup. The Tauri delta needs to be present in the HTML the webview sees, not stripped at build.

A naive "make them one file with `if (TAURI) {…}` branches" works tactically (and is more or less how the current sync is reconciled) but doesn't fix the structural problem: **firmware authors writing new widget fields, signal modal tweaks, etc. have to remember to mirror their change into the desktop copy in a separate repo, with a separate review, and a separate build/test cycle.** Drift is automatic; sync is manual.

## What we tried

| Attempt | Outcome |
|---|---|
| Hand-sync per-feature | Works for a session or two, drifts within a month. ADR 0003 documents the sync plan; in practice it lags 2-5 commits behind the firmware side at any given time. |
| Codegen `WIDGET_DEFS` table from `schema/widgets.schema.json` | Done in Wave 1 (commit 95ae13c). Removes a chunk of the per-widget hand-mirror surface. Doesn't address the JS handler code, only the widget property metadata. |
| Build-time string replace | Considered. Rejected because the Tauri delta is structural (whole modal sections, fetch interceptors), not just constants. A templating step would need to understand the actual structure. |
| WASM editor sharing the firmware copy | The browser WASM editor already does this — it serves the firmware copy as-is. Proves the firmware HTML can run standalone in a browser. |

## The decision, as it stood before the migration landed

**Keep the two copies, hand-synced.** Document the sync expectation in CLAUDE.md. Lean on commit-message archaeology to know what landed in which repo. Run the desktop-sync agent (see ADR 0003) when drift is noticed.

This was a working compromise, not a target end-state — see "The migration target" below for what replaced it on 2026-07-09.

## The migration target (landed 2026-07-09, under different names)

**Single source of truth: `main/web/index.html` in the firmware repo.** Desktop builds via a Tauri-side preprocessing step that injects its delta at build time.

Shape:

1. The firmware HTML is the canonical file. Edit only there.
2. The Tauri delta lives in `rdm7-desktop/src/tauri-overlay.html` — a fragment file containing only the divergent sections (Device Manager modal, Serial Logs drawer, etc.) with explicit insertion markers like `<!-- TAURI:INSERT-AFTER #hamburgerMenu -->`.
3. A Cargo build script (`build.rs` in `src-tauri/`) reads both, applies the overlay, writes the merged file to `src-tauri/target/web/index.html`.
4. Tauri's `frontendDist` points to the merged output, not the raw `src/`.
5. CI on the desktop repo pulls the firmware HTML from a known commit (pin or follow `main`), runs the merge, runs a smoke test on the result.

Acceptance criteria for the migration:

- Editing `main/web/index.html` alone produces both firmware + desktop builds with the new behaviour, no manual desktop commit needed.
- The Tauri overlay file is ≤ 500 lines and consists only of additive blocks (no edits to firmware code).
- The build fails loudly if an overlay insertion marker is missing from the firmware HTML — drift is detected, not silently absorbed.

**Why this isn't done yet**: the firmware HTML isn't structured for overlay insertion. The Tauri delta currently weaves through firmware code in places (e.g. inside `_menuIcons`, inside the `WIDGET_DEFS` field list for `auto_size`). Pulling those threads out requires:

- A pass over `main/web/index.html` to add `<!-- TAURI:INSERT-AFTER ... -->` markers at every divergence point (~20-30 of them).
- A pass over `rdm7-desktop/src/index.html` to extract each delta into the overlay file in the same order.
- A merge tool that's robust to firmware-side reordering (use unique IDs, not line numbers).
- A way to verify the merge result is byte-identical to today's desktop file as a regression baseline.

Estimated cost: one full day of focused work + a follow-up week of squashing the inevitable edge cases. Worth doing **once the firmware schema and editor surface stabilise post-release** — doing it before then means doing it twice.

### Update — what actually shipped (2026-07-09)

`rdm7-desktop` commit `355f5ff` built this, citing this ADR by name. The shape matches almost exactly; every proper noun above turned out wrong:

| Predicted here | Shipped |
|---|---|
| `tauri-overlay.html` with `<!-- TAURI:INSERT-AFTER ... -->` markers | Same filename, but a `##[ block ]## / ##[ anchor ]## / ##[ insert-after\|insert-before\|replace-with ]## / ##[ end ]##` directive syntax |
| A Cargo `build.rs` in `src-tauri/` | A Python script, `rdm7-desktop/tools/merge_overlay.py`, run via Tauri's `beforeDevCommand`/`beforeBuildCommand` |
| Merged output at `src-tauri/target/web/index.html` | Merged output at `rdm7-desktop/src/dist/index.html` (gitignored) |
| CI pulls the firmware HTML from a pinned/followed commit | `rdm7-desktop/tools/sync_firmware.py` copies `main/web/index.html` into a checked-in `src/firmware-base.html` and stamps the source commit in `src/firmware-base.commit`; a human runs it on demand, it isn't yet a CI job |
| — | The old hand-maintained `rdm7-desktop/src/index.html` (the subject of ADR 0003) no longer exists at all — not split into an overlay, just gone |

All three acceptance criteria above are met: editing the firmware HTML plus a `sync_firmware.py` pull is enough to update both builds, the overlay file is pure anchored additive blocks, and `merge_overlay.py` exits non-zero when an anchor no longer matches the firmware base — drift fails the build instead of being silently absorbed. This has been under continuous active use since (117+ commits touching the overlay/merge files as of 2026-07-30). See `rdm7-desktop/CLAUDE.md` ("Frontend is BUILT, not edited (ADR-0007)") for the day-to-day mechanics; this ADR remains the record of why it exists.

## Consequences, updated post-migration

- `sync_firmware.py` is a manual pull, not a CI job — the desktop repo can still lag the firmware between syncs. Releases of the desktop app should pin a known-synced firmware commit (recorded in `src/firmware-base.commit`), not assume `master` is current.
- The overlay's anchor syntax makes drift loud instead of silent: a firmware-side edit that moves or removes an anchor point fails the merge rather than quietly dropping the Tauri delta. This is the acceptance criterion that mattered most in practice.
- The vendored `schema/` + `codegen_widget_defs.py` copy that briefly lived in `rdm7-desktop` (added 2026-04-27 alongside ADR 0003's completion) was deleted in the same 2026-07-09 commit — `WIDGET_DEFS` now arrives for free as part of the whole-file firmware copy, so that mitigation is no longer needed.

## References

- ADR 0003 — Desktop `index.html` sync plan (the tactical mitigation this migration replaced; its hand-merged output file no longer exists)
- `schema/widgets.schema.json` — partial codegen source for `WIDGET_DEFS` (firmware repo only; still current)
- `tools/codegen_widget_defs.py` — current codegen pipeline (covers field metadata, not handlers)
- Sync agents ran by hand at commits `aa8a1e2` and `f07ca2c` (desktop) before the automated pipeline existed.
- `rdm7-desktop` commit `355f5ff` (2026-07-09) — built the migration target above; see `tools/merge_overlay.py`, `tools/sync_firmware.py`, `src/tauri-overlay.html`.
