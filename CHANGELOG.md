# Changelog

All notable changes to the RDM-7 Dash firmware. Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are reported as `FIRMWARE_VERSION` from `main/include/version.h`.

This file starts tracking from **1.1.11** (the first release-tracked build, 2026-05-19). Earlier changes live in `git log` and topic files under `.claude/projects/.../memory/`.

## [Unreleased]

Changes that have landed on `master` since the last tagged version.

### Added
- _nothing yet_

### Changed
- _nothing yet_

### Fixed
- _nothing yet_

---

## [1.1.11] — 2026-05-19

First release-tracked build. Significant cleanup + documentation pass plus the cumulative work since 1.1.10. Boots clean on COM13; web editor serves with `Content-Encoding: gzip`; 122/122 native tests pass.

### Added
- **Ford FG preset — chassis extras.** New optional ECU signal slots `BOOST`, `FUEL_LEVEL`, `PARK_BRAKE`, `YAW_RATE`, `LATERAL_G` (in `ecu_signal_slot_t`). FG preset wires these from `0x425` / `0x4B0` / `0x000` / `0x360` per the BigFalconSheet DBC tab. Most factory presets leave them `SIG_UNSUPPORTED`. (commit `40b2d5e`)
- **OTA "Skip This Version" button.** OTA prompt gains a third button alongside Later / Install. Stores the offered version in NVS namespace `ota_cfg`; popup stays silent until upstream version differs. Cleared by factory reset. (commit `02a2588`, prior release)
- **OBD2 Mode 02 freeze frame** — tap-DTC-to-see-conditions flow. (commit `a0de71c`, prior release)
- **Schema migration skeleton** (`_migrate_layout_root`). Defined landing site for future field renames / removals / semantic changes. Currently a no-op switch — every transition through `LAYOUT_SCHEMA_VERSION=14` is additive. (commit `94b578b`)
- **`widget_arc` redline / limiter polish** — redline-zone arc, limiter flash/solid, value text overlay. (commit `5fe5c87`, prior release)
- **`widget_meter` port** from RDM-7_Dash_P4 — rear-extension partial refresh, ext-draw-size hook, anchor-aware tick recolor, `tick_label_divisor` field. (commit `e9b28ad`, prior release)
- **OBD2 hamburger section** (web editor) with Setup / Trouble Codes / Vehicle Info modals routing through existing `/api/obd2/*`. (commit `d43c7fa`, prior release)
- **Web editor: Save As menu** + meter sweep slider (30..360) + `label_gap` range widened to `-150..150`. (commit `e6f1a9f`, prior release)
- **Native test suite expansion**: 76 → 122 tests. New files: `test_layout_migration.c` (schema gate + migration helper), `test_widget_arc_precedence.c` (19 tests for the fill-color precedence ladder including the just-fixed regression), `test_obd2_freeze_frame.c` (21 tests for Mode 02 parse + scale/offset), `test_ota_skip_version.c` (12 tests for the comparator), `test_calculated_gear.c` (13 tests for the CALCULATED_GEAR classifier). All mirror-pattern with explicit source-of-truth references. (commits `94b578b`, `4ec78ee`, `275d662`, `382eb13`, `c88a42c`)
- **Documentation**: `tests/README.md` refreshed to current state; `CHANGELOG.md` (this file); `SECURITY.md` (release-surface security posture); `docs/adr/README.md` (ADR index); `docs/adr/0005-html-source-of-truth.md` (the three-HTML-copies decision).

### Changed
- **Embedded web editor is now gzipped** (`main/CMakeLists.txt` + `main/net/web_server.c`). 842 KB → 181 KB on the wire (4.65× reduction). Firmware image shrank from 0x31c1b0 → 0x27aa40 (-645 KB). **OTA partition free slack: 11% → 29%** (+18pp). Browsers handle `Content-Encoding: gzip` transparently. (commit `79c2789`)
- **CAN filter always includes the OBD2 response range** `0x7E8..0x7EF`, regardless of polling state. Previously gated on `obd2_is_running()`, which caused one-shot diagnostic endpoints (DTC read/clear, VIN, ECU name) to silently time out on native broadcast presets like FG or BA/BF. (commit `1c3f61b`)
- **`usb_cdc_protocol.c/h`** now flags its out-of-build status in a `BUILD STATUS` header block — won't show up as an "orphan" in future audits. USB CDC ACM and the USB Serial JTAG console can't coexist on the ESP32-S3, so this stub stays compiled-out until the console flips. (commit `a3ff0af`)
- **Tauri desktop bundle config** — `tauri.conf.json` now declares `targets: ["nsis", "msi"]` so `cargo tauri build` produces installers (.exe + .msi) alongside the standalone executable. (desktop repo)

### Fixed
- **`widget_arc` rule color survives limiter / redline zone exit.** When both a widget_rule arc_color override and the limiter were active on the same widget, the rule color was lost as soon as the zone cleared — `_arc_apply_fill_color` always restored `d->arc_color`, not the rule-overridden colour. Documented limitation deferred from `c338a6c`; now fixed by caching the rule colour in `arc_data_t._rule_arc_color` and reading it as the "normal" base inside `_arc_apply_fill_color`. (commit `c9262b8`)
- **`max_uri_handlers` 100 → 160** in `web_server.c` (prior release, retained). Current count is 100 handlers registered (62% of cap, 60 slots free).

### Security & Operational
- _No changes from prior release._ Pre-release hardening items tracked in [`SECURITY.md`](SECURITY.md).

### Internal
- `feature/widget-sys` fast-forwarded to `master` at `c88a42c`. Both branches pushed to origin.
- Desktop repo carries a local-only commit `f07ca2c` from the most recent sync pass — image `auto_size`, button/toggle momentary fields, duplicate-reverse cleanup. Not pushed.

---

## How to update this file

When you land a change worth tracking:

1. Add a bullet under `## [Unreleased]` in the relevant subsection (Added / Changed / Fixed / Security & Operational / Internal).
2. Keep the bullet single-line where possible. Wider context goes in the commit message.
3. Reference the commit SHA in parentheses at the end of the line: `(commit \`abc1234\`)`.
4. When cutting a release, rename the `[Unreleased]` heading to `[X.Y.Z] — YYYY-MM-DD`, create a fresh empty `[Unreleased]` block at the top, and bump `FIRMWARE_VERSION` in `main/include/version.h`.

Skip changes that are pure-refactor / no-behavior-change / typo-fix unless they're load-bearing for a future release note.

## Versioning policy

Semantic-ish: MAJOR.MINOR.PATCH.

- **PATCH** — bug fix, doc, or test-only change; no behaviour change for the user.
- **MINOR** — additive feature; old layouts and old API clients keep working.
- **MAJOR** — breaking. Old layouts may need migration; old API clients may break.

`LAYOUT_SCHEMA_VERSION` (in `main/layout/layout_manager.h`) bumps independently — track it in the bullet that introduces the schema change.
