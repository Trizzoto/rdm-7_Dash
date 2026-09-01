# Architecture Decision Records

An ADR captures a single significant architectural decision — the context that forced it, the options considered, the choice made, and the consequences accepted. ADRs are immutable once accepted: when reality changes, write a new ADR that supersedes the old one rather than editing the original.

Read the relevant ADR before changing the area it covers. The "we already tried that" answers live here.

## Index

| # | Status | Title | Touches |
|---|---|---|---|
| [0001](0001-wifi-onboarding-reliability.md) | Accepted | Wi-Fi onboarding reliability — the layered fixes phones need | `main/net/wifi_manager.c`, `main/net/web_server_captive.c`, `main/net/dns_hijack.c`, `sdkconfig` |
| [0002](0002-web-server-split-roadmap.md) | Complete | Splitting the monolithic `web_server.c` by concern | `main/net/web_server*.c` |
| [0003](0003-desktop-index-sync-plan.md) | Superseded by 0007's landed pipeline | One-time hand-merge that first put the Tauri delta into `rdm7-desktop/src/index.html` | `../rdm7-desktop/src/transport.js` (the fetch-interceptor concept it introduced) |
| [0004](0004-performance-budgets.md) | Proposed | Documented performance budgets (heap, OTA partition, URI handlers, layout JSON) | repo-wide |
| [0005](0005-channel-owned-decode.md) | Accepted (implemented) | Channel-owned CAN decode for portable layouts (decode moves layout `signals[]` → `channels.json`) | `main/data/channel_manager.c`, `main/layout/*`, `schema/canonical_channels.md` |
| [0006](0006-channel-architecture-v2.md) | Accepted | Channel architecture v2 — canonical channel registry as the binding layer | `main/data/canonical_channels.c`, `main/data/channel_manager.c` |
| [0007](0007-html-source-of-truth.md) | Accepted — migration landed 2026-07-09 | Firmware HTML + a Tauri overlay, merged at build time — why the copies exist and how they're kept in sync now | `main/web/index.html`, `../rdm7-desktop/src/{firmware-base,tauri-overlay}.html`, `../rdm7-desktop/tools/merge_overlay.py`, `schema/widgets.schema.json` |
| [0008](0008-gps-lap-timing-integration.md) | Accepted | GPS lap timing — how the puck, the dash and the desktop suite fit together | `main/lap/`, `main/data/canonical_channels.c`, `main/can/can_manager.c`, `../rdm-gps-node/` |
| [0009](0009-rdm-io-mixed-precision-frontend.md) | Accepted | RDM IO mixed-precision analog front-end (4× 16-bit ΔΣ + 4× 12-bit) + PT Motorsport benchmark | `docs/PLATFORM_PLAN_2026-07.md` §6.3, `../rdm7-desktop/src/tauri-overlay.html` (IO workspace) |
| [0010](0010-rdm-io-three-tier-ladder.md) | Accepted | RDM IO three-tier ladder — Pico A$89 / Core A$219 / Pro A$449–579, shared firmware + emulation modes | `docs/PLATFORM_PLAN_2026-07.md` §4/§6.3, future firmware `profiles/` layer |
| [0011](0011-analyzer-no-synthetic-data.md) | Accepted | The CAN analyzer never fabricates traffic — demo generator deleted, every empty state names its cause | `../rdm7-desktop/src/tauri-overlay.html` (analyzer workspace), `main/net/web_server_test.c`, `main/net/web_server_obd2.c` |
| [0012](0012-corner-phase-attribution.md) | Accepted | Corner-phase time attribution (braking/entry/apex/exit), the lane rack, and the optimal-lap patent boundary | `../rdm7-desktop/src/tauri-overlay.html` (Session workspace) |
| [0013](0013-one-course-type.md) | Accepted | One course type — a finish line is a thing you add, not a mode you pick; the session splitter learned the second line | `../rdm7-desktop/src/tauri-overlay.html` (Tracks + Session), `main/lap/lap_core.{c,h}` (unchanged, documented) |
| [0014](0014-workspace-design-language.md) | Accepted | Workspace design language — chrome is monochrome, colour is data, the accent marks | `../rdm7-desktop/src/tauri-overlay.html` (shared `.ws-*` chrome) |
| [0015](0015-the-node-is-part-of-readiness.md) | Accepted | The node's own state is part of readiness — and dialogs must actually ask | `../rdm7-desktop/src/tauri-overlay.html` (GPS workspace), `../rdm-gps-node/` (status honesty + GNSS recovery) |
| [0016](0016-cancel-must-cancel.md) | Accepted | Cancel must cancel — the editor asks with its own overlay, because `window.confirm` under Tauri doesn't | `main/web/index.html` (`_showConfirmOverlay`/`confirmAsync`, all 19 guards) |
| [0017](0017-one-channel-list-two-ticks.md) | Accepted | One channel list, two ticks — Log and Graph | `../rdm7-desktop/src/tauri-overlay.html` (GPS workspace — Setup + rack popover), `../rdm7-desktop/tools/check_autotrack.js` |
| [0018](0018-any-source-defines-a-channel.md) | Accepted | Any source can define a channel, and the marks are not tickboxes | `../rdm7-desktop/src/tauri-overlay.html` (GPS workspace — channel definitions, DBC import), `../rdm7-desktop/tools/check_autotrack.js` |
| [0019](0019-splits-you-name.md) | Accepted | Splits you name — the name belongs to the gate that opens the stretch | `../rdm7-desktop/src/tauri-overlay.html` (Tracks inspector + Session split grid), `docs/LAP_ANALYSIS_REDESIGN_2026-07.md` Stage 5 |
| [0020](0020-recording-needs-nothing-pressed.md) | Accepted | Recording needs nothing pressed, and readiness is one line | `../rdm7-desktop/src/tauri-overlay.html` (Session readiness) — desktop-only, no firmware change |
| [0021](0021-restart-from-inside-studio.md) | Accepted | Everything can be restarted from inside Studio, and recording is red | `../rdm7-desktop/src-tauri/src/lib.rs`, `../rdm7-desktop/src/{tauri-overlay.html,transport.js}` |
| [0022](0022-record-is-a-bus-citizen.md) | Accepted | Record is a bus citizen (`RECORD` on 0x40E), and the GNSS wedge cannot end a day | `../rdm-gps-node/` (`d270059`), `../rdm7-desktop/src/tauri-overlay.html` (Studio button); RDM-7_Dash owes the dash-side button |
| [0023](0023-recording-is-a-choice.md) | Accepted | Recording is a choice — `record_on_boot` in NVS, and a REC lamp that holds still | `../rdm-gps-node/` (`record_on_boot` + RTC armed state), `../rdm7-desktop/src/tauri-overlay.html` (Setup toggle) |
| [0024](0024-the-lap-timer-wears-the-brand.md) | Accepted | The lap timer wears the brand — Industry light, RDM red, six flat views | `../rdm7-desktop/src/tauri-overlay.html` (GPS workspace only), `../rdm7-desktop/tools/merge_overlay.py` (asset list) |
| [0025](0025-analyse-is-a-grid-you-arrange.md) | Accepted | Analyse is a grid you arrange, not a layout we chose | `../rdm7-desktop/src/tauri-overlay.html` (GPS workspace, Analyse view) |
| [0026](0026-analyse-is-a-mosaic.md) | Accepted | Analyse is a mosaic, and it may be taller than the window — supersedes 0025's row-major grid | `../rdm7-desktop/src/tauri-overlay.html` (Analyse panel tree, drag-to-place, brand-bar session line) |
| [0027](0027-the-drawn-line-is-never-decimated.md) | Accepted | The drawn line is never decimated — the trace gets its own canvas layer, every sample at every zoom | `../rdm7-desktop/src/tauri-overlay.html` (Analyse + Corners maps) |
| [0030](0030-channels-are-one-page.md) | Accepted | Channels are one page, and it works with the dash unplugged — five Setup cards become one menu; offline edits queue as API calls and replay on reconnect | `main/web/index.html`, `main/net/web_server_channels.c`, `main/ui/screens/first_run_wizard.*`, `main/ui/settings/device_settings.c`, `../rdm7-desktop/src/transport.js` |
| [0031](0031-channels-is-a-page.md) | Accepted | Channels is a page, and it leads with the car you have — third Studio mode, one DOM block moved between page and modal, configured channels first with search still reaching the catalogue | `main/web/index.html` |
| [0032](0032-adding-a-channel-is-a-decision.md) | Accepted | Adding a channel is a decision, and destroying one leaves a copy behind — clicking a catalogue row previews instead of permanently activating; Restore exports the outgoing setup first | `main/web/index.html` |
| [0033](0033-the-catalogue-travels-with-the-page.md) | Accepted | The catalogue travels with the page, and the poll is the watchdog — firmware tables host-compiled into index.html for setup-from-zero; queued ECU imports; live↔offline detected in place | `main/web/index.html`, `main/ui/settings/preset_picker_data.c`, `tools/native/gen_channel_catalog.c`, `tools/gen_channel_catalog.py` |
| [0034](0034-removal-returns-to-the-catalogue.md) | Accepted | Removal returns a channel to the catalogue, on every surface — channel_manager_remove + allow_canonical; device editor gains preview-Add, two-tap Remove, and pool-owned row ids | `main/data/channel_manager.*`, `main/net/web_server_channels.c`, `main/ui/screens/first_run_wizard.c`, `main/web/index.html` |
| [0035](0035-the-source-column-speaks-provenance.md) | Accepted | The Source column speaks provenance, not plumbing — badge + frame id instead of registry names; Ctrl+Shift+S goes to Channels; signal-speak retired from user-facing copy; device pane follows the same rule | `main/web/index.html`, `main/ui/screens/first_run_wizard.c` |
| [0036](0036-layouts-get-the-tune-file-treatment-too.md) | Accepted | Layouts get the tune-file treatment too, but last-write-wins — offline saves stash per layout name and are offered on reconnect, reusing the existing screenshot/channels reconnect moments | `main/web/index.html` |
| [0037](0037-scanning-the-car-is-a-source.md) | Accepted | Scanning the car is a source, and diagnostics is a different job — `+ Add channels → From OBD2` on all three surfaces, one firmware resolver (`channel_obd2_matches`/`channel_apply_obd2`), polled PIDs pruned on unbind; Source column down to two tags, view chips retired | `main/data/channel_source_apply.*`, `main/data/canonical_channels.*`, `main/net/web_server_obd2.c`, `main/net/web_server_channels.c`, `main/ui/screens/first_run_wizard.c`, `main/ui/settings/device_settings.c`, `main/web/index.html` |
| [0038](0038-an-empty-save-is-not-an-instruction.md) | Accepted | An empty save is not an instruction — `/api/layout/save` refuses to replace a populated layout with zero widgets unless the client sends `allow_empty`, after the editor's pre-load placeholder silently destroyed a dashboard | `main/net/web_server_layout.c`, `main/web/index.html` |
| [0039](0039-setup-is-grouped-by-the-question-you-arrived-with.md) | Accepted | Setup is grouped by the question you arrived with — five sections mirrored on web and dash; ECU & CAN bus / WiFi / Odometer become real pages; modals lead with the outcome and hide the wiring | `main/web/index.html`, `main/ui/settings/device_settings.c` |
| [0040](0040-a-backup-has-to-stand-on-its-own.md) | Accepted | A backup has to stand on its own — the decode migration declared success after copying nothing, stamping v3 on channels that never got a decode, so restored backups came back unassigned (or silently re-homed onto OBD2) | `main/data/channel_manager.c` |
| [0041](0041-a-hook-decays-when-what-it-hooks-stops-being-used.md) | Accepted | A hook decays when the thing it hooks stops being used — Studio's WASM preview refresh hung off `triggerPreview()`, which widget moves stopped calling once the live-edit fast paths landed, so offline the canvas froze until a layout switch; `_flushLiveEdits` is hooked too | `rdm7-desktop/src/tauri-overlay.html` |
| [0042](0042-a-bundle-should-say-whether-it-carries-your-car.md) | Accepted | A bundle should say whether it carries your car — one `.rdm` container, two flavours (`.layout.rdm` / `.dashboard.rdm`) with a header flavour byte; the layout flavour omits `channels.json` entirely so a shared bundle cannot leak a bus configuration | `main/web/index.html` |
| [0043](0043-a-panel-is-as-tall-as-what-is-in-it.md) | Accepted | A panel is as tall as what is in it — Analyse panels are measured and given the height their contents need, so the page is the only thing that scrolls; spare height goes to the map and the rack, and a sub-48 px shortfall is absorbed by them rather than opening a scrollbar. Supersedes 0026's height rule | `rdm7-desktop/src/tauri-overlay.html` (Analyse mosaic + top bar), `rdm7-desktop/tools/check_gridfit.js` |
| [0044](0044-a-recording-carries-its-own-meaning.md) | Accepted | A recording carries its own meaning — the puck logs raw CAN counts (0008), so the definitions that make them readable are snapshotted into the recording at download, the dash's channel library is cached to disk, and one resolver/reader pair owns the decode (two's complement out where the field is signed); an undecoded column is labelled `counts` and tagged, never drawn as if it were a reading | `rdm7-desktop/src/tauri-overlay.html` (GPS workspace), `rdm7-desktop/tools/check_candecode.js` |
| [0045](0045-a-reading-can-be-corrected-after-the-drive.md) | Accepted | A reading can be corrected after the drive — a wrong scale is a wrong label on counts that are still there, so corrections outrank the definitions 0044 freezes into a recording, at two scopes (everywhere / this recording only); the wire position is history and is never editable, and the form shows what the column WOULD read rather than asking you to know the ECU's scaling | `rdm7-desktop/src/tauri-overlay.html` (GPS workspace), `rdm7-desktop/tools/check_chanfix.js` |
| [0046](0046-two-samples-are-not-always-a-line.md) | Accepted | Two samples are not always a line — a gap no driving connects is marked once at ingest and every reader honours it; the test is the recording's own Doppler speed (with a 12 m floor so a parked car never breaks its own trace), and the two marks are separated because only the one that says the clock is wrong can invent a lap time. Nothing is deleted, moved or interpolated — only what is claimed *between* the samples changes | `rdm7-desktop/src/tauri-overlay.html` (`gpMarkBreaks` in `gpComputeG`), `rdm7-desktop/tools/check_breaks.js`, `lap_core.c` |
| [0047](0047-the-bus-must-be-quiet-before-the-dash-is-ready.md) | Accepted | The bus must be quiet before the dash is ready — TWAI TX is GPIO20 (USB D+ on the S3) and floats out of reset with no pull-up, which the transceiver reads as dominant and holds the whole bus down for ~1.5 s of boot; a 2nd-stage bootloader hook parks the line recessive at ~25 ms, the app re-parks it, and the USB Serial/JTAG console is turned off so the PHY cannot re-claim the CAN pins. The bootloader is NOT OTA-updatable — a field fix needs USB | `bootloader_components/rdm_can_park/`, `main/can/can_manager.c` (`can_park_tx_line`), `main/main.c`, `sdkconfig.defaults` |
| [0048](0048-extracted-firmware-code-keeps-the-scope-it-was-written-for.md) | Accepted | Extracted firmware code keeps the scope it was written for — the overlay lifts `importRdm`'s body out of its file-input handler, so every `file.name` the firmware adds there becomes "Import failed: file is not defined" on the desktop. 0.7.0/0.7.1 could not import a `.rdm` at all. Give the extracted body a `file` stand-in instead of anchoring each mention; anchors catch text drift, not scope | `../rdm7-desktop/src/tauri-overlay.html` (`import-rdm-head`, the `file-opened` listener) |

| [0049](0049-a-recording-says-how-much-to-trust-it.md) | Proposed | A recording says how much to trust it — the signals that would have caught a +54°/−48° slip angle in seconds already existed, scattered across four surfaces; one Analyse panel, worst-wins over rows that each carry their own verdict, and the drift fit's own scale/bias/sample-count stop being invisible. The ranked-first "calibration run per car" folds in here as transparency, because `gpDriftAngle` already self-calibrates from ordinary driving and the fault it missed was not a scale error | `../rdm7-desktop/src/tauri-overlay.html` (GPS workspace — new `health` panel, `gpSessionMeta`), `../rdm7-desktop/tools/check_health.js` |
| [0050](0050-a-real-drive-cannot-be-generated.md) | Proposed | A real drive is the only fixture a synthetic one cannot replace — `check_mallala.js` recovers a *planted* 1.008 gyro scale perfectly while the real 23 Aug session produced +54°/−48°; two or three real recordings are committed with hand-blessed answer sheets that are never regenerated, the two harnesses that read real data stop dying or silently skipping, and CI actually runs `check_all.js` | `../rdm7-desktop/tools/fixtures/`, `tools/{make_expected,check_golden,check_laptime,check_breaks}.js`, `.github/workflows/frontend-merge-check.yml` |
| [0051](0051-one-layout-file-drawn-two-ways.md) | Proposed | One layout file, drawn two ways — the dash and the video overlay already share a widget vocabulary and their icons, so the `.rdm` bundle grows entry type `4` carrying the overlay placement (skipped gracefully by every existing reader, and kept out of the layout JSON's 32 KB ceiling). The widget list is shared; the geometry is not, because a dash layout copied onto video covers the footage | `main/web/index.html` (entry loop + format comment), `../rdm7-desktop/src/tauri-overlay.html` (`import-rdm-*`, `export-rdm-native`, `gpHudDashPlan`) |
| [0052](0052-an-export-is-a-snapshot-not-a-worker.md) | Proposed | An export is a snapshot, not a worker — decode and encode are already off-main-thread and the fast path already survives a hidden window (31.2 s of footage in ~18 s, throttled); what blocks the app is a full-screen modal and shared mutable state read live per frame. A Worker would have forked `gpHudRender` and lost the document fonts whose `measureText` drives layout, not just glyphs | `../rdm7-desktop/src/tauri-overlay.html` (export path, `.gp-expdlg`), `../rdm7-desktop/tools/check_export.js` |

| [0053](0053-a-circuit-can-be-built-from-the-drive-that-proves-it.md) | Accepted | A circuit can be built from the drive that proves it — `gpAutoSetUp` already recognises 117 named circuits plus the library, so the gap was a track in neither: a private test track, airfield, skidpan or kart circuit got "add the circuit in Tracks", which is the setup the rest of the machinery removes. Three CLEAN laps within 25% mints one from the drive itself; counting raw laps instead let a four-stop road drive mint a circuit and put lap times on the board, so the run flags of 0046 are what holds the line | `../rdm7-desktop/src/tauri-overlay.html` (`gpTrackFromDrive`, `gpAutoSetUp`), `../rdm7-desktop/tools/check_autotrack.js` |
| [0054](0054-prompt-does-not-come-back.md) | Accepted | `prompt()` does not come back — ADR-0016 left the browser prompt alone on the understanding that it "opened nothing"; measured over CDP it is the webview's own, unimplemented, and it NEVER RETURNS, so New dashboard… and a channel's Custom… unit froze the desktop app where they stood. `promptAsync()` wraps the editor's own overlay the way `confirmAsync()` wraps the confirm one, and the overlay now escapes what it is handed | `main/web/index.html` (`_showPromptOverlay`, `promptAsync`, `layoutEditorNew`, `_chUnitSelect`, `_chUnitFieldHTML`), `../rdm7-desktop/tools/check_dialogs.js` |

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

Strictly sequential. The next ADR is `0055`. Don't reuse numbers, even if a draft is abandoned — leave a stub if needed (`0011-abandoned.md` with one line of explanation).

`0029` is deliberately unclaimed: a draft of it exists outside this repo (the
GPS "a recording plays as a session" work). Take `0031`, not `0029`.

Numbers are sometimes claimed by code before the file is written — `ADR-0026`
lived in `rdm7-desktop/src/tauri-overlay.html` for a day before
`0026-analyse-is-a-mosaic.md` existed. Grep both repos for the next number
before you take it.
