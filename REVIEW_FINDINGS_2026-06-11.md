# RDM-7 Project — Comprehensive Software Review

**Date:** 2026-06-11
**Scope:** Firmware (`RDM-7_Dash`), web editor (`main/web/index.html`), online studio (`rdm7-studio-web`), marketplace (`rdm7-marketplace`), docs (`docs/`, `rdm7-docs`), plus a live read-only probe of the bench device at `192.168.4.61`.
**Method:** 12 parallel domain reviewers (each reading real code with file:line evidence), a live-device probe, and a docs/marketplace/studio/arch second-opinion pass. The two CRITICAL findings were re-verified by hand against source. "Known/handled" items (FPS config, CAN-upload HMAC dev secret, desktop-index drift, COM13 staleness) were excluded by design.

> Severity legend: 🔴 Critical (data loss / crash / security breach, fix first) · 🟠 High · 🟡 Medium · ⚪ Low/polish.
> Effort: trivial (<1h) · small (1-3h) · medium (half-day+) · large (multi-day).

---

## Burndown status (updated 2026-06-12)

**CRITICAL — both closed:** C1 ✅ `f70a602` · C2 ✅ `d63f54d`.

**HIGH:**
- ✅ H1 `d63f54d` · H5 `5740780` · H6 `7b33f81` · H7 `d63f54d` · H8 `643add5` · H9 `d63f54d` · H10 *(2026-06-12: logger write-failure auto-stop + stop_reason in status + sd_manager health probe)* · H11+H13 `e9f38b9` · H12 `5f73420` · H14 `02bfe18` · H15 `edaee5b`+`390e9e6` · H20-22 `ef5c42d`
- 🔶 H2 (API auth) — RESOLVED-FOR-BETA `718e214`: per-device WPA2 hotspot password (derived from MAC, shown on dash; fleet-wide "rdm7dash" retired, stored copies auto-upgraded). Decision: token-auth on mutating endpoints deliberately NOT built — network access now requires the per-unit password, and a token layer would break every client for marginal gain at beta scale. Revisit for the home-WiFi malicious-device threat model post-beta.
- ⏸ H3/H4 — `rdm7-marketplace` repo, deferred (production-infra risk; H3 fix authored as `fbd7c2c` there, not applied).
- ⏸ H16-H19 — `rdm7-studio-web` repo, not started.

**Section 0 (testability):** ✅ all five endpoints + `/api/signal/inject` hardening `1e7436e` + `202479f`. Test-suite/CI gaps still open.

**MEDIUM (firmware):** ✅ units_display conversion `8b16cda`+`9d0c226`(+follow-ups) · send_json OOM guard `47ce7b3` · decode bounds validation `202479f` · channels.json export/import `a584596` · KEEP_CONSOLE default `beff0c2` · OTA power-save revert, WiFi STA slow-retry + scan-miss reschedule, runtime ESP_ERROR_CHECK soft-fail, widget_rules malformed-entry init, indicator apply_overrides, recv_json_body promoted to shared web_server_recv_body across ~22 handlers *(all 2026-06-12)*.
✅ also: OBD2 blocking-ctx refcount `0d4e300` (timeout path verified live — heap flat across forced 12 s snapshot timeouts) · HMAC secret rotated firmware-side `db78b86` (worker `wrangler secret put` pending — owner action).
⏳ Still open: response-envelope/CORS consistency · boot WiFi init off the LVGL task · channel-rebind/night-subscribe helper extraction · studio/marketplace mediums (other repos) · docs COM-port/handler-count sweep + check_doc_drift.py.

**LOW:** ✅ lv_obj_del_async `d63f54d` · log demotion `d63f54d` · replay path safety `bd4e9aa` · channels.json missing-array recovery *(2026-06-12)*. Rest open.

---

---

## 0. Your key ask — let an agent test the device end-to-end

~80% of the plumbing already exists (`signal_inject`, `signal_sim`, screenshot + shadow-FB seq counter, CONTROL/remote-touch, `inspector_get` on all 15 widgets, `widget_registry_snapshot`, `channel.last_zone`). Five small endpoints close the loop:

| New endpoint | Unlocks | Hook point | Effort |
|---|---|---|---|
| `GET /api/widgets` | Live widget-tree dump (type/pos/field values/active rules) — verify a rule fired or a color changed without a screenshot | `widget_registry_snapshot()` + `WIDGET_FIELDS` table + `inspector_get` vtables; serialize under `rdm_lvgl_lock` | ~150 lines / medium |
| `POST /api/can/inject` | Synthetic frame into the **real** RX queue → exercises `can_extract_bits`/scale/endian/dispatch/zone/OBD2 over WiFi, no bus | `xQueueSendToBack(s_can_queue,…)` (`can_manager.c:611/692`) | ~80 lines / small |
| `GET /api/screenshot/hash?x&y&w&h` | Deterministic region hash off RGB565 shadow FB → golden-image diff + tear detection | `display_capture_shadow_fb/seq` (`display_capture.h:73-80`) | ~70 lines / small |
| Channel `zone` field in `/api/channels` | `channel.last_zone` is tracked but not emitted → assert "EGT in high_warn" directly | `channel_to_full_json` (`web_server_channels.c:176`) | ~10 lines / trivial |
| `GET /api/selftest` | One GET replaces a serial session: URI-reg tally, FS, channels, CAN state, LVGL-lock latency, heap | aggregate existing getters; expose `REGISTER_URI` tally | ~120 lines / small |

**Also harden `POST /api/signal/inject`** — it returns `200 ok` for nonexistent signal names (silent no-op), so a typo'd test passes the POST then fails mysteriously. Validate with `signal_find_by_name()` and return `{injected:[…], unknown:[…]}`.

**Test-suite gaps:** API contract suite covers **~13 of ~106 endpoints** — zero for channels/gear/OBD2/presets/logger and the new layout→channels import (commit `8f4f439`). Priority new files: `test_channels_api.py`, `test_layout_import.py`. `channel_manager.c` is host-testable today (no LVGL/FreeRTOS deps beyond existing mocks) but untested despite being the source of the last three migration bug hunts. The widget `to_json` defaults-only/32 KB invariant has no host test.

**CI quick wins:** the pytest mock suite (`conftest.py` "CI-friendly") is never run in CI; there's no ESP-IDF compile check (a forgotten `CMakeLists` SRCS entry merges green); mocks pin `schema_version: 13` (firmware is 14); `tests/README.md` lists 7 test files but 9 exist.

---

## 1. 🔴 CRITICAL (both hand-verified)

### C1 — Layout saves are not atomic and never recover from `.bak` on corruption
**Storage · small · `layout_manager.c:1313,986,1029`; `dashboard.c:249`**
`layout_manager_save_raw` renames the live file → `.bak`, then writes new content in place with `O_CREAT|O_TRUNC` (`:1313-1317`). A power cut mid-write — **normal in a car** — leaves a truncated live file. On load, the `.bak` is consulted **only when `open()` fails** i.e. the file is *missing* (`:987-994`); when the file exists but `cJSON_Parse` fails (`:1029`), it returns `ESP_FAIL` and the good `.bak` sitting right beside it is never tried. `dashboard_init` then falls back to `default` and `set_active("default")` permanently — and `default.json` is only re-seeded when *missing*, so a corrupt default drops to the hardcoded fallback on every boot until factory reset.
**Fix:** port the `channel_manager.c:1383` idiom — write `{path}.tmp`, fsync, close, rename live→bak, rename tmp→live; on parse failure move bad file to `.corrupt` and retry from `.bak` before giving up; re-seed `default.json` when it exists but won't parse.

### C2 — Conditional rules double-handled for arc & meter
**Widgets · small · `widget_arc.c:933,1140,1366`; `widget_meter.c:1360,1540,1736`; `layout_manager.c:927,957,1273`**
`layout_manager` parses/subscribes/serializes rules centrally for every widget. `widget_arc` and `widget_meter` **also** call all three themselves (verified: `widget_bar.c`, which is correct, calls none). Result when a layout has rules on an arc/meter: (1) `widget_rules_from_json` runs twice, reassigning `w->rules = calloc(...)` without freeing → PSRAM leak per load; (2) `widget_rules_subscribe` runs twice → duplicate `(cb,user_data)` subscriber; on destroy `widget_rules_free` unsubscribes once, leaving a **dangling subscriber pointing at the freed widget** → use-after-free on the next CAN frame (signals merge across reloads, so the stale sub survives); (3) `widget_rules_to_json` runs twice → duplicate `rules` key, doubling bytes against the 32 KB budget.
**Fix:** delete the three calls from `widget_arc.c` and `widget_meter.c`; let layout_manager own the lifecycle. Harden the engine: free/unsubscribe before realloc, make subscribe idempotent, bail in to_json if `rules` already present.

---

## 2. 🟠 HIGH

### Security
- **H1 `GET /api/wifi/config` serves the home WiFi password in plaintext** (`web_server_wifi.c:13,24`) — **confirmed live**: returned the actual SSID + password with `Access-Control-Allow-Origin: *`. Any web page on the same LAN reads it cross-origin. *Fix (trivial): return `has_password: true`, never the password; the UI only needs the boolean.*
- **H2 Entire API is unauthenticated** (`web_server.c:127`; reboot `web_server_system.c:322`, OTA `web_server_ota.c:88`, touch `web_server_touch.c:25`) — anyone in WiFi range can reboot, start OTA, or inject touch **while driving**, delete layouts, rewrite WiFi creds. *Fix (medium): random per-device AP WPA2 password shown on-screen + a token check on mutating (POST/DELETE) handlers via a shared helper; enable AP isolation.*
- **H3 Marketplace: layout validation is client-only, never enforced server-side** (`UploadForm.tsx:189,488`) — persistence is a direct client `supabase.from("layouts").insert({is_published:true})`; 32 KB cap / slot caps / unknown-widget rejection are advisory. *Fix: validate in a route handler/Edge Function (or DB trigger on `file_size_bytes`) before flipping `is_published`.*
- **H4 Marketplace: paid-layout paywall is bypassable** (`layout-detail/[id]/page.tsx:11,147`; bucket `public=true`) — `rdm_url` ships in page props for every layout and the bucket is public, so the file downloads straight from the network payload without `/api/checkout`. *Fix: private bucket + authenticated `/api/download/[id]` that checks a `purchases` row, returns a short-lived signed URL.*

### Threading / object lifetime
- **H5 `lv_async_call` from non-LVGL tasks without the mutex — systemic (~60 sites)** (`lv_async.c:44`, `main.c:405`; callers across httpd/WiFi/UART/OTA/timer tasks) — LVGL v8 async inserts into the global timer linked list unlocked while the LVGL task mutates it on core 1 → the intermittent "CORRUPT HEAP" class. *Fix (small, whole-class): one `rdm_async_call()` wrapper taking `rdm_lvgl_lock(-1)`; mechanically replace cross-thread call sites. Update CLAUDE.md's threading doctrine.*
- **H6 `channels.json` debounced save serializes live structs on the esp_timer task with no lock** (`channel_manager.c:1434,1372,358`) — `channel_manager_delete` frees + swap-removes while the save iterates → UAF / torn strings persisted. Fires after every channel edit. *Fix: build the cJSON under `rdm_lvgl_lock`, write the file unlocked.*
- **H7 `widget_text` is the one widget whose channel-changed handler never re-subscribes** (`widget_text.c:54,230`) — after a rebind it renders the old signal and `_text_destroy` unsubscribes the *new* index → dangling subscriber UAF on the old signal. *Fix (trivial): copy the standard unsubscribe-old/subscribe-new block.*
- **H8 Font upload/delete destroys live `lv_font_t` from the httpd task** (`font_manager.c:254,301`; `web_server_assets.c:414,499`) — no LVGL lock and no reload, so re-uploading a displayed family leaves every label pointing at freed memory. *Fix: take the lock + trigger a reload (or defer the destroy).*

### Storage durability (automotive: mid-write power loss is normal)
- **H9 Data/CAN loggers `fflush` without `fsync`** (`data_logger.c:102`, `can_raw_logger.c:132`) — on FAT the dir entry commits only on `f_sync`, so a power cut loses the **entire** CSV, not the "~2 s" the comments claim. `channel_manager.c:1403` already shows the right idiom. *Fix (trivial): add `fsync(fileno(f))` after the flush (or every Nth).*
- **H10 No SD hot-removal handling** (`sd_manager.c:17,80`; `data_logger.c:83`) — mount state is set once at boot and never re-checked; pull the card and logger writes fail silently forever while the UI reports a healthy recording (and each failed FATFS access burns SPI-timeout time on the LVGL task). *Fix: check `fprintf` returns, auto-stop after N failures, periodic mount health probe.*

### OTA / CAN / system
- **H11 OTA rollback is documented but disabled** (`sdkconfig:415,2399`; no `esp_ota_mark_app_valid*` anywhere; `05-storage-and-persistence.md:316`, `RDM-7_User_Guide.md:645` claim "dual-slot rollback safety") — an OTA image that passes validation then crashes boot-loops until USB recovery. *Fix: enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, call `esp_ota_mark_app_valid_cancel_rollback()` after first healthy render; until then fix the docs.*
- **H12 CAN RX task created `WithCaps` but deleted with plain `vTaskDelete`** (`can_manager.c:284,381,522`) — leaks PSRAM stack + internal TCB on every filter rebuild, and the `WithCaps` recreations are unpinned so the task loses its core-0 affinity after the first rebind. *Fix: `vTaskDeleteWithCaps` + `xTaskCreatePinnedToCoreWithCaps(...,0,...)` via one shared spawn helper (mirror `can_bus_test.c`).*
- **H13 Task watchdog is effectively a no-op** (`sdkconfig:1149`; no `esp_task_wdt_add` on app tasks; `PANIC` off) — only idle tasks are watched and a trip only warns. A deadlocked LVGL/CAN task freezes the dash forever with no reboot. *Fix: subscribe the LVGL (and CAN) task, set `CONFIG_ESP_TASK_WDT_PANIC=y` (15 s timeout is generous).*

### Web editor / live device
- **H14 Channel threshold/decode edits silently lost on failed `/api/channels/update`** (`index.html:18770`) — pending fields cleared *before* the fetch; failure is console-only, so the input shows the typed value but `_chCache` reverts on next render. Silent data loss in the most safety-relevant editor. *Fix: re-queue on failure + toast.*
- **H15 `/api/channels/source-options` holds the LVGL lock ~1.9 s** (`web_server_channels.c:670-871`) — measured live; does LittleFS I/O + builds a 98 KB cJSON tree under the lock, freezing the dashboard ~2 s every time the source picker opens. *Fix: snapshot only what needs the lock (a few hundred bytes), build the JSON after unlock; cache the static ECU portion.*

### Studio (online)
- **H16 Silent WASM-load failure** (`rdm7-studio-web/public/index.html:4032`) — if the `.wasm` 404s/is blocked, the catch only `console.error`s and the canvas stays black; a brand-new visitor (the entire target) sees a dead box with no message. *Fix: render an overlay ("preview couldn't load — editor & export still work").*
- **H17 Unguarded `localStorage` quota on save** (`transport.js:157`; autosave `index.html:5267` swallows the error) — layouts/splash/registries/presets still in localStorage; a few near-32 KB layouts + autosave hit the ~5 MB quota and saves silently fail. *Fix: try/catch `setItem`, toast on `QuotaExceededError`, point to .rdm export.*
- **H18 No onboarding** — no tour/tutorial/template gallery; `emptyStateHint` only appears *after* a widget is added (`index.html:3078`). `TOUR_SCRIPT.md` exists but was never wired in. *Fix: first-run overlay gated on a flag, 2-3 starter templates, a 4-step "add → bind → preview → export" walkthrough.*
- **H19 Schema-version drift vs firmware is unguarded** (`index.html:4275` hand-edited `LAYOUT_SCHEMA_VERSION=14`; no codegen tool or CI in the studio repo) — a firmware v15 bump keeps the studio emitting v14 with stale field metadata, silently dropping/mis-defaulting new fields. *Fix: a `package.json` codegen script + CI check against the firmware schema; at minimum a source-of-truth comment.*

### Docs
- **H20 CLAUDE.md + handover predate the channel architecture** (`CLAUDE.md:81,104` "all CAN config lives in layout JSON"; Source-Layout tree omits `main/data/`; `04-signal-and-can.md:3` "signal registry is the only decode layer") — the canonical onboarding path never mentions channels/ADR-0005/0006, the most actively-developed subsystem. *Fix: add a "Channel System" section + `main/data/` to the tree; correct the data-flow claims.*
- **H21 CLAUDE.md "Adding a New Widget" tells you to hand-edit WIDGET_DEFS** (`CLAUDE.md:135,112`) — that block is now codegen output guarded by `schema-check.yml`, so following the docs produces a red CI check. *Fix: rewrite the steps around editing `schema/widgets.schema.json` + running the codegen.*
- **H22 Stale counts cluster** — schema "v13" (`CLAUDE.md:90,136`, code is 14), "13 widget types" (`CLAUDE.md:32,54`, `03-widget-system.md:42`, code is 15; the doc's "add `WIDGET_GAUGE=13`" example collides with `WIDGET_LINE=13`), ADR-0005 numbering collision (two `0005-*.md`), ADR index/`docs/README` omit the two channel ADRs. *Fix: sweep + reference macros (`LAYOUT_SCHEMA_VERSION`, `WIDGET_TYPE_COUNT`) instead of restating numbers; renumber the channel ADR.*

---

## 3. 🟡 MEDIUM (condensed)

**Firmware / API**
- `units_display` is label-only with no conversion → oil pressure shows **"210.2 bar"** for 210.2 kPa, **wrong out-of-the-box** (`canonical_channels.c:52`; `index.html:17458`). Implement a native→display table or make `units_display_def == units_native` and treat it as a pure label.
- `httpd_resp_sendstr` on possibly-NULL `cJSON_PrintUnformatted` → crash on OOM (`web_server_system.c:152,303`; `web_server_wifi.c:28`). Add a `send_json()` helper.
- `/api/signal/update` + channels decode patch accept unbounded `bit_start/bit_length/can_id` (`web_server_signals.c:396`, `web_server_channels.c:402`). Add one shared `validate_decode()`.
- ~15 POST handlers do single-shot `httpd_req_recv` → drop split TCP bodies (intermittent 400s). Promote `recv_json_body()` to a shared helper.
- Mixed response envelopes (`{"status":"ok"}` vs `{"ok":true}` + one-offs) and inconsistent CORS (7-8 GET endpoints missing the header, incl. high-traffic `/api/signals/values` and `/api/layout/version`). Add `send_ok()` / `rdm_resp_json_begin()` helpers.
- **Gap: no `channels.json` export/import endpoint** — per ADR-0005 channel config is device-local and unrecoverable on hardware swap / FS corruption. Add `GET /api/channels/export` + `POST /api/channels/import`.
- OBD2 snapshot handler leaks ctx + semaphore on every 12 s timeout; `lv_async_call` return unchecked (`web_server_obd2.c:574`). Refcount the ctx.

**Firmware / arch**
- OTA `restore_wifi_settings()` forces `WIFI_PS_MIN_MODEM` after every download, undoing the dash's deliberate `WIFI_PS_NONE` → degraded editor link until reboot (`ota_handler.c:416`).
- Boot WiFi radio init + 100+ `REGISTER_URI` run on the LVGL task → ~hundreds-of-ms render hitch ~4 s into boot (`main.c:582,1209`). Spawn a one-shot core-0 task.
- WiFi STA permanently gives up after ~60 s of non-auth reconnect failures (`wifi_manager.c:162`) — drive out of range >1 min and it never reconnects. Add a slow background retry tier.
- Runtime `ESP_ERROR_CHECK` in web/event-reachable WiFi paths aborts the whole device on a transient failure (`wifi_manager.c:732,794,923`). Log + return instead.
- `RDM7_DEBUG_KEEP_CONSOLE` defaults to **1** in source (`main.c:1143`) → production builds ship with the desktop-serial protocol disabled unless someone remembers the `-D`. Invert the default.

**Widgets**
- `widget_rules_from_json`: a malformed (non-object) rule entry leaves `signal_index=0` from calloc → subscribes to signal[0] and churns the mask (`widget_rules.c:221`). Init the slot before the validity check.
- Indicator is the only widget without `apply_overrides` → rules on indicators subscribe + evaluate but can never apply (silent no-op consuming subscriber slots) (`widget_indicator.c:1226`).
- The channel-rebind block is hand-copied across 8 widgets (the cause of H7) — extract `widget_channel_rebind_signal()`; same for the night-mode subscribe boilerplate in 14 create fns.

**Studio / marketplace**
- Studio has **no pre-export 32 KB size check** (firmware's `_checkLayoutSize` absent here) → a browser-valid layout silently truncates on the device (`index.html:11358`). Port `_checkLayoutSize` + `validateLayout`.
- Studio loads a render-blocking 2.3 MB preview engine in `<head>` with no `defer`/spinner → first-visit page can appear hung (`index.html:2647`).
- Marketplace: **no reporting/moderation** table anywhere; uploads auto-publish. Add a `reports` table + report UI + admin queue.
- Marketplace: admin "Remove" uses the anon client → RLS blocks it, 0 rows affected, but `error` is null so the UI says "removed successfully" (`admin/page.tsx`). Route admin mutations through the service-role client.
- Marketplace validator has no upper schema bound (`schema_version:999` passes) (`validate-layout.ts:64`).

**Docs / DX**
- Handover `02-build-and-flash.md:234` says `max_uri_handlers` is 100 (code is 160); CLAUDE.md's "~106 used" is ~119. Three docs cite three COM ports (13/5/27).
- No prose-vs-code doc check in CI. Add `tools/check_doc_drift.py` asserting the schema macro / widget count / handler cap match the docs, wired into `schema-check.yml`.

---

## 4. ⚪ LOW / polish (one-liners)

- Last raw `lv_obj_del_async` of the double-free class at `ui_Screen3.c:184` → use `rdm_obj_del_async`.
- Splash timer double-`esp_timer_delete` + double-run transition when WiFi drops at boot (`splash_screen.c:298,366`).
- Fuel/gear calibration mutated synchronously on the httpd task while the internal-signal timer reads it (`web_server_signals.c:121`, `web_server_gear.c:83`) → transient garbage.
- Wizard "Widget settings" passes a raw `widget_t*` through `lv_async_call` a reload can free first (`first_run_wizard.c:2032`) → re-resolve by slot+type.
- `refr_diag` render telemetry logs at **WARN every 1 s forever**; CAN TWAI heartbeat at **INFO every 5 s** — field-log spam (`main.c:248`, `can_manager.c:186`). Demote to LOGD behind a flag.
- `to_json` defaults-only broken in indicator (8 unconditional fields) + partial in text/panel/warning/bar; magic-literal default guards in panel/warning should be named constants (mirror `ARC_DEFAULT_*`).
- Panel infers staleness via `strcmp(value_str,"--")` — thread the `is_stale` bool instead (`widget_panel.c:99`).
- `signal_subscribe` failure (16-slot cap) is silent on-device — widgets ignore the return (`signal.c:281`). Log the widget id.
- Unknown API paths return **405 not 404** (CORS wildcard side-effect) → register an err handler so missing endpoints are distinguishable (`web_server.c:196`).
- Replay start accepts absolute paths verbatim, bypassing filename safety (`web_server_logger.c:670`).
- `compare_versions` parses any non-`X.Y.Z` tag as `0.0.0` → silently suppresses a real update (`ota_handler.c:448`). OTA image-header version check is dead/stubbed (`ota_handler.c:80`).
- `s_promiscuous_active` not cleared on bitrate-change/resume/recover → filter flag diverges from hardware until reboot (`can_manager.c:642`).
- Wire-input task always spawned then self-deletes when wire mode off (`main.c:1121`).
- `boot_assets` seed check is `st_size>0` not `==len` → a truncated seeded font/logo is never repaired (`boot_assets.c:78`). `user_signals.c:87` + custom-preset save (`web_server_layout.c:1074`) write in place (no temp+rename) → corruption wipes the library.
- `channels.json` that parses but lacks a `channels` array returns `ESP_OK` → bindings dropped, `.bak` recovery skipped (`channel_manager.c:1305`).
- ~300 lines of dead JS in the editor (fork-signal cluster, broken `importJson` referencing a removed DOM id, `_pollLiveSignalValues`, etc.); `saveLayout()` is never defined → 3 typeof-guarded no-ops should call `saveActiveLayout` (`index.html:17344`).
- Editor polling loops (screenshot, `_pollChannelLive` ×2/500 ms, fuel/OBD2) have no in-flight guard → pile up on slow links; 83 empty catches (a few on user actions like image-delete); zero `role="dialog"`/Escape on 23 modals; mobile menu `eval()`s onclick strings.
- Studio: `_DEFAULT_LAYOUT` stamped `schema_version:11`; marketplace handoff is a manual new-tab link; global error handler hijacks `document.title`.
- Marketplace: `increment_downloads` RPC is open to anyone (inflatable, still drives sort order); screenshot uploads have no enforced MIME/size; bare `<img>` with no `onError` → broken-image icon instead of placeholder; validator `panel` slot cap (16) contradicts firmware "unlimited".
- Live layout data: widget bound to truncated dead signal `LAMBDA_CORR_A_lambda_corre_2`; arc "ARC FUEL" bound to `VEHICLE_SPEED`; FPS text clipped off both screen edges; panel_3/panel_4 overlap 5 px. (Layout-data fixes, not firmware — but the editor could warn on out-of-bounds bounds + dead-signal bindings.)

---

## 5. Suggested sequencing

**Do now (trivial, high impact):** H1 WiFi-password mask · H7 widget_text re-subscribe · the last raw `lv_obj_del_async` · H9 logger fsync · refr_diag/CAN log demotion · C2 (delete 6 lines from arc/meter).

**Next (small, durability/safety):** C1 atomic layout save (port the channel_manager idiom) · H5 `rdm_async_call` wrapper · H6 channels.json save lock · H11+H13 OTA rollback + watchdog panic · H14 channel-edit re-queue · H15 source-options lock scope.

**Then (testing leverage — your key ask):** the 5 test endpoints + `test_channels_api.py`/`test_layout_import.py` + wire the pytest suite & an idf compile check into CI. These pay for themselves by letting an agent verify every subsequent fix on the device.

**Studio/marketplace (parallelizable):** H16-H19 (WASM overlay, quota toast, onboarding, schema-drift CI) · H3-H4 (server-side validation + private paid bucket).

**Docs sweep (trivial, batch):** H20-H22 + the count fixes + a `check_doc_drift.py` so it can't recur.
