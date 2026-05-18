# ADR 0005 — Three HTML copies: why they exist and the plan to collapse them

**Status**: Accepted (documents the current state) — migration plan deferred
**Context**: The same web editor HTML lives in two places (formerly three), the copies drift, and "why don't we just have one file?" surfaces every few sessions. This ADR exists so that question retires permanently and the future migration has a defined shape.

## The three copies

| Copy | Path | Role | Toolchain |
|---|---|---|---|
| Firmware | `main/web/index.html` | Single source of truth. Embedded into the firmware binary at build (gzipped → 181 KB, served at `/`). | ESP-IDF / CMake |
| Desktop | `../rdm7-desktop/src/index.html` | Tauri app's frontend. Loaded by the Tauri webview when the user launches the desktop app. | Tauri / Cargo |
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

## The decision (today)

**Keep the two copies.** Document the sync expectation in CLAUDE.md. Lean on commit-message archaeology to know what landed in which repo. Run the desktop-sync agent (see ADR 0003) when drift is noticed.

This is a working compromise, not a target end-state. The release-readiness cost of doing the proper fix now is too high relative to other items on the punch list.

## The migration target (future)

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

## Consequences accepted while we're at copies

- Every PR that touches `main/web/index.html` should be considered for desktop sync. A note in the commit message ("desktop sync needed: yes/no/intentionally diverging") helps the future sync agent.
- The desktop repo's `master` is allowed to lag the firmware. Releases of the desktop app should pin a known-synced firmware commit, not auto-track.
- MEMORY.md and CLAUDE.md will continue to flag "three copies" — this is fine and expected until the migration lands.

## References

- ADR 0003 — Desktop `index.html` sync plan (the tactical mitigation)
- `schema/widgets.schema.json` — partial codegen source for `WIDGET_DEFS`
- `tools/codegen_widget_defs.py` — current codegen pipeline (covers field metadata, not handlers)
- Sync agents have run at: commits `aa8a1e2` (desktop) and `f07ca2c` (desktop). The latter is the most recent state at the time of writing.
