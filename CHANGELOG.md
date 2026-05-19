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
- **`web_server_name_is_safe` / `web_server_filename_is_safe` UTF-8 portability**: cast input byte to `(unsigned char)` before the `< 0x20` control-char check. Pins the signedness behaviour across host gcc and ARM toolchains so UTF-8 multi-byte sequences are consistently accepted everywhere. `tests/native/test_web_path_safety.c::test_name_utf8_is_accepted` (formerly `test_name_high_ascii_rejected_when_char_is_signed`) flipped to assert TRUE for `éclair` and `日本` and locks the cast in.

---

## [1.1.11] — 2026-05-19

First release-tracked build and the **opening of the 2-week stabilisation window** before customer release. Significant cleanup + documentation pass plus the cumulative work since 1.1.10. Boots clean on COM13; web editor serves with `Content-Encoding: gzip`; **141/141 native tests pass**; pre-release audit walked end-to-end.

### Added
- **Ford FG preset — chassis extras.** New optional ECU signal slots `BOOST`, `FUEL_LEVEL`, `PARK_BRAKE`, `YAW_RATE`, `LATERAL_G` (in `ecu_signal_slot_t`). FG preset wires these from `0x425` / `0x4B0` / `0x000` / `0x360` per the BigFalconSheet DBC tab. Most factory presets leave them `SIG_UNSUPPORTED`. (commit `40b2d5e`)
- **OTA "Skip This Version" button.** OTA prompt gains a third button alongside Later / Install. Stores the offered version in NVS namespace `ota_cfg`; popup stays silent until upstream version differs. Cleared by factory reset. (commit `02a2588`, prior release)
- **OBD2 Mode 02 freeze frame** — tap-DTC-to-see-conditions flow. (commit `a0de71c`, prior release)
- **Schema migration skeleton** (`_migrate_layout_root`). Defined landing site for future field renames / removals / semantic changes. Currently a no-op switch — every transition through `LAYOUT_SCHEMA_VERSION=14` is additive. (commit `94b578b`)
- **`widget_arc` redline / limiter polish** — redline-zone arc, limiter flash/solid, value text overlay. (commit `5fe5c87`, prior release)
- **`widget_meter` port** from RDM-7_Dash_P4 — rear-extension partial refresh, ext-draw-size hook, anchor-aware tick recolor, `tick_label_divisor` field. (commit `e9b28ad`, prior release)
- **OBD2 hamburger section** (web editor) with Setup / Trouble Codes / Vehicle Info modals routing through existing `/api/obd2/*`. (commit `d43c7fa`, prior release)
- **Web editor: Save As menu** + meter sweep slider (30..360) + `label_gap` range widened to `-150..150`. (commit `e6f1a9f`, prior release)
- **Native test suite expansion**: 76 → 141 tests (+86%). New files: `test_layout_migration.c` (schema gate + migration helper), `test_widget_arc_precedence.c` (19 tests for the fill-color precedence ladder including the just-fixed regression), `test_obd2_freeze_frame.c` (21 tests for Mode 02 parse + scale/offset), `test_ota_skip_version.c` (12 tests for the comparator), `test_calculated_gear.c` (13 tests for the CALCULATED_GEAR classifier), `test_web_path_safety.c` (19 tests for path-traversal guards at the HTTP API boundary). All mirror-pattern with explicit source-of-truth references.
- **GitHub Actions CI**: `.github/workflows/native-tests.yml` runs the 141-test suite on every push/PR. `.github/workflows/schema-check.yml` validates `schema/widgets.schema.json` and detects codegen drift in `main/web/index.html` / `main/widgets/widget_fields.gen.c` before it reaches master.
- **Documentation**: `tests/README.md` refreshed to current state; `CHANGELOG.md` (this file); `SECURITY.md` (release-surface security posture + closed pre-release audit checklist); `docs/README.md` (documentation map); `docs/adr/README.md` (ADR index); `docs/adr/0005-html-source-of-truth.md` (the three-HTML-copies decision); root `README.md` linked to the handover docs.

### Changed
- **Embedded web editor is now gzipped** (`main/CMakeLists.txt` + `main/net/web_server.c`). 842 KB → 181 KB on the wire (4.65× reduction). Firmware image shrank from 0x31c1b0 → 0x27aa40 (-645 KB). **OTA partition free slack: 11% → 29%** (+18pp). Browsers handle `Content-Encoding: gzip` transparently. (commit `79c2789`)
- **CAN filter always includes the OBD2 response range** `0x7E8..0x7EF`, regardless of polling state. Previously gated on `obd2_is_running()`, which caused one-shot diagnostic endpoints (DTC read/clear, VIN, ECU name) to silently time out on native broadcast presets like FG or BA/BF. (commit `1c3f61b`)
- **`usb_cdc_protocol.c/h`** now flags its out-of-build status in a `BUILD STATUS` header block — won't show up as an "orphan" in future audits. USB CDC ACM and the USB Serial JTAG console can't coexist on the ESP32-S3, so this stub stays compiled-out until the console flips. (commit `a3ff0af`)
- **Tauri desktop bundle config** — `tauri.conf.json` now declares `targets: ["nsis", "msi"]` so `cargo tauri build` produces installers (.exe + .msi) alongside the standalone executable. (desktop repo)

### Fixed
- **`widget_arc` rule color survives limiter / redline zone exit.** When both a widget_rule arc_color override and the limiter were active on the same widget, the rule color was lost as soon as the zone cleared — `_arc_apply_fill_color` always restored `d->arc_color`, not the rule-overridden colour. Documented limitation deferred from `c338a6c`; now fixed by caching the rule colour in `arc_data_t._rule_arc_color` and reading it as the "normal" base inside `_arc_apply_fill_color`. (commit `c9262b8`)
- **`max_uri_handlers` 100 → 160** in `web_server.c` (prior release, retained). Current count is 100 handlers registered (62% of cap, 60 slots free).

### Security & Operational
- **Pre-release audit walked end-to-end** (2026-05-19). 4 checklist items in [`SECURITY.md`](SECURITY.md) closed (dependency CVE walk, reproducible build, factory layout safety, boot-log secret scan). 2 items deferred to v1.2 with documented reasoning (HMAC per-device derivation, OTA manifest signing). 2 waived with documented decision (Supabase Pro plan, flash encryption posture).
- **Reproducible build verified**: `idf.py fullclean && idf.py build` produces firmware size 0x27aa40 (2,599,488 bytes), stable across fresh checkouts. Binary SHA-256 varies run-to-run because ESP-IDF embeds build-time UUIDs/timestamps; size + partition layout are stable.
- **Boot-log audit**: no firmware code logs the CAN HMAC secret, WiFi passwords (only metadata: `"saved for '<ssid>'"`, `"updated"`, `"cleared"`), or OTA tokens at any log level.
- **Dependency CVE walk**: managed components (`esp_new_jpeg` 0.6.1, `littlefs` 1.20.4, `LVGL` 8.3.11) are current releases with no outstanding advisories. Desktop Cargo: initial visual review was clean; **`cargo audit` (run same-day after install completed) found 3 actual vulnerabilities** in `rustls-webpki 0.103.10` — TLS cert-validation flaws (RUSTSEC-2026-0098/-0099) and a CRL parse panic (RUSTSEC-2026-0104), all on the auto-updater TLS path. **Patched same-day** via `cargo update -p rustls-webpki` → 0.103.13 in the desktop repo. 20 "no longer maintained" advisories triaged and ignored in `src-tauri/.cargo/audit.toml` with per-entry rationale — all Linux-only GTK3 or build-time parsers, not exploitable on the Windows desktop path users run.
- **Factory layout safety**: factory default is compiled into `main/layout/default_layout.c` (widget positions only — no creds, no debug signal sources), not shipped as a JSON file.

### Found during audit (deferred fixes)
- _The portability bug logged here at v1.1.11 was fixed in the post-tag patch — see `[Unreleased] / Fixed` above._

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
