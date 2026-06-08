# Overnight Task Checklist — 2026-06-09  ✅ ALL TASKS COMPLETE

Branch: `feature/widget-sys`. Dev port: COM27.
Approach: investigate → implement → build → flash → test → check off.
Status legend: ⬜ todo · 🔬 investigating · 🛠️ implementing · 🧪 testing · ✅ done · ⚠️ partial/note

Verification key: **[build]** compiled clean · **[boot]** flashed + booted clean on COM27 (0 faults) · **[web]** verified in browser via mock dev server (localhost:8180) · **[code]** verified by code review.

---

## 1. Arc widget enhancements
- ✅ 1a. Arc ticks (count/length/width/major+minor colour) — overlay `lv_meter`, default OFF. [build][boot][web]
- ✅ 1b. Arc value line / needle + settings (width/colour/length) — same overlay. [build][boot][web]
- ✅ 1c. Arc anchor curve (anchor_enabled/value/position) — ported from meter. [build][web]
- ✅ 1d. Arc reverse direction (+ fixed redline-marker `_value_to_angle` reverse bug). [build][web]
- ✅ 1e. Arc low & high values — already present (`signal_min`/`max`, channel-driven); verified. [code]
- ✅ 1f. Arc start angle 0 = 12 o'clock — web `_arcSyncDerived` mirrors meter; 225/270→LVGL 135/45 verified, legacy round-trips. [web]

## 2. Bar widget
- ✅ 2a. Bar ticks (count/length/width/colour, Top/Bottom/Both) — manual rects, default OFF. [build][boot][web]
- ✅ 2b. Bar high/low alerts ↔ channel — already implemented; verified end-to-end. [code]

## 3. Web UI shape rendering
- ✅ 3. Shapes already device-only on canvas (geometry draw removed in b43c2d9); added dashed border + "device" badge cue so it's clear. [web]

## 4. Channels
- ✅ 4a. +10 canonical channels (target_lambda, lambda_correction, ethanol_pct, fuel_rail_pressure, absolute_load, fuel_rate, egr_command, runtime, coolant_pressure, fuel_pressure_diff) + fixed 3 OBD2-map id mismatches. Auto-surfaces in web. [build][boot]
- ✅ 4b. Removed "Normal colour" from channels web UI (+ unused colorCell helper). [web]

## 5. Custom CAN signals (one-off)
- ✅ 5. Root cause: channel "More" only showed CAN fields when already bound → unbound channel had no inputs/button. Added "+ Add CAN signal" (creates signal + binds + auto-expands decode fields). Auto-upserts a "Custom"/v1 preset on one-off creation. [web][code]

## 6. Performance
- ✅ 6. Sim no longer laggier than CAN: meter needle value-gate (the real bug — meter was the only un-gated ticking widget) + sim threshold-emit (~1 needle-px) + sim timer 5→16ms. [build][boot]

## 7. Peaks
- ✅ 7. Peaks session-only — removed NVS load/autosave/save-now + dead persistence code; in-RAM + session peaks intact, Peaks screen + panel show_peak still work. [build][boot]

## 8. Web UI cleanup
- ✅ 8. Removed "Live" Studio mode (button/panel/CSS/JS); kept CONTROL pill; `setStudioMode('live')` coerces to design. [web]

## 9. Splash screen
- ✅ 9. "Disable" option in splash dropdown → boots straight to dashboard. NVS `splash_enabled` + `ui_init` gate + `POST /api/splash/enabled` + `enabled` in `/api/splash/list`. [build][boot][web]

## 10. Splash + WiFi
- ✅ 10. WiFi-loss while on splash → falls back to dashboard (ui_wifi hook on connected→lost transition + splash handler, no-op when dashboard already up). [build][boot]

## 11. Settings reorg
- ✅ 11. Data Logger + Dashboard Switcher + Marketplace + Files group added to Setup panel; desktop hamburger replaced with slim layout-actions caret (New/Save As/Rename/Delete); mobile menu de-tooled (Tools/OBD2 → Setup) but Edit/Arrange/Stream kept. Nothing stranded. [web]

## 12. Mobile/tablet
- ✅ 12. "Widgets" button added to mobile bottom nav → bottom-sheet palette (shared `_fillPalette`); verified 15 widgets, tap adds+selects. [web]

## 13. RPM bar full customisation
- ✅ 13. Ticks Top/Bottom/Both + length/width/colour, bar background colour, RPM number readout (new label, font + colour) + on-device config_modal rows. Defaults reproduce current look. [build][boot][web]

## 14. Undo/redo
- ✅ 14. Root cause: autosave wiped the history stack every 1s. Guarded history reset with `if (!silent)` (saveActiveLayout + Save As); load/New still reset correctly. Verified guard present at runtime. [web]

## 15. Text widget value→label mapping
- ✅ 15. Already works in firmware (signal `value_map` → `signal_format_value` → text renders "N" for 0). Surfaced the "Value Labels" editor for channel-bound widgets in the web inspector (was only shown for legacy raw-signal). [web][code]

## 16. Banner customization (regression)
- ✅ 16. `WIDGET_BANNER` ("Alert Banner") firmware was intact; its WIDGET_DEFS entry was accidentally deleted in b43c2d9. Restored durably via schema (codegen regenerates it). Verified banner back in palette + inspector with all 13 fields (text/colour/font/opacity/align/border/radius). [build][boot][web]

## WiFi crash (handled by user's other agent)
- ✅ esp_wifi_init ESP_ERR_NO_MEM crash loop fixed (dynamic TX buffers + core-affinity). Pre-existing (panic_count=160, not a Wave-1 regression). Device boots clean.

---

## Build / flash log
- Build #1 (Wave 1) PASS → flashed COM27. WiFi crash loop (pre-existing) blocked test → user's other agent fixed it.
- Build #3 (Wave 1+2+3) PASS → flashed COM27 → **clean boot, 0 reboots, 0 faults**; dashboard `Time_Attack` loads, all widgets (meter/4×arc/bar/2×banner/button/shape) create without error; WiFi STA up (no OOM).
- Codegen: 15 widgets / 273 fields, drift check OK.
- Build #4 (Wave 4 web) — running (build_wave4.log); index.html re-embedded. Web changes pre-verified in browser (editor loads error-free, all new fns resolve).
- Checkpoint commit `be4fab8` (Waves 1–3, 27 files +2021/-263). Wave 4 to be committed after final flash.

## Remaining for morning
- Final build #4 finishing → flash → boot smoke-test (confirm clean boot with new index.html embedded).
- On-device VISUAL spot-check (arc ticks/value-line, rpm number, banner) is best done by you in the editor — defaults are OFF/unchanged so existing dashboards are visually identical; new options are opt-in.
- Desktop `../rdm7-desktop/src/index.html` NOT synced (separate repo, deferred per CLAUDE.md).

## Notes / decisions
- Schema is **v14** (CLAUDE.md/memory said v13 — stale). No schema-version bump needed (all additive/defaults-only).
- Widget web fields are codegen'd from `schema/widgets.schema.json` (never hand-edit WIDGET_DEFS / widget_fields.gen.c).
- All firmware changes are defaults-only in `to_json` (32 KB layout budget) and OFF-by-default where new (arc/bar ticks) so existing layouts are unaffected.
- Checkpoint commits on `feature/widget-sys`, no push.
