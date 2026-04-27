# ADR 0004 — Performance Budgets

**Status**: Proposed (documents existing implicit budgets; new budgets pending instrumentation)
**Context**: The firmware has accumulated a layered set of performance defences over the last year — frame-time caps, batch sizes, registry limits, JSON size limits, queue depths, watchdog timeouts. None of them are written down as a single budget. New contributors (and the original author six months later) re-derive them from comments and from the code each time they touch a performance-sensitive path. This ADR turns those defences into explicit numerical budgets, names the symptoms of violation, lists what we already measure, and lists what we still need to measure.

It is documentation only. No code changes here. Where a budget is "proposed but not yet committed," the proposal is a starting line for the discussion that lands the instrumentation.

## Why a budget document at all

The system runs on a 240 MHz dual-core SoC with 512 KB internal SRAM and 8 MB PSRAM, talking to an 800×480 RGB panel at 60 Hz, draining a CAN bus that can deliver several hundred frames per second. The performance envelope is real and we routinely brush against it — see [docs/handover/01-architecture.md](../handover/01-architecture.md) §threading-model for the layout. When a defence fails, the failure mode is almost always silent: dropped frames, dropped CAN messages, dropped HTTP handler registrations, layout JSON saves that 413, mDNS that can't allocate. Each of those was tracked down through a serial-log archaeology session that took hours. A documented budget plus a place where violations surface (proposed §6 below) would replace several of those sessions with a single glance at the diagnostics screen.

This ADR is the authoritative list. If a defence in the code disagrees with this file, one of the two needs to be fixed; do not silently let them drift.

## 1. Budgets we have effectively committed to

These are encoded in source today. The numbers are not negotiable without code changes; this section just makes the rationale explicit so future edits don't strip a defence by accident.

### 1.1 LVGL refresh period — 14 ms

**Source**: [main/lv_conf.h:27](../../main/lv_conf.h) — `LV_DISP_DEF_REFR_PERIOD 14`. Note that [docs/handover/01-architecture.md](../handover/01-architecture.md) cites 16 ms as a round-number reference to a "60 Hz refresh"; the real value is 14 ms (~71 Hz target).

**Rationale**: At 14 ms LVGL invalidates dirty regions and ships a new frame just over 70 times a second. Eye-perceptible jank starts when frame intervals exceed ~33 ms (30 fps). 14 ms gives 19 ms of slack before a single dropped frame is visible. The choice over 16 ms (60 Hz) gives one extra invalidation per second of headroom for paths that trigger on every frame (the FPS counter, sim updates).

**Symptom of violation**: `FPS` internal signal drops below ~60. Visible jank on meter sweeps and RPM-bar fills.

**How we measure it now**: `FPS` internal signal in [main/widgets/signal_internal.c:64-74](../../main/widgets/signal_internal.c) — flush-callback frame counter divided by elapsed time, sampled every 500 ms.

### 1.2 LVGL CPU idle ≥ 30 % (effectively committed; under-measured)

**Source**: [main/widgets/signal_internal.c:77](../../main/widgets/signal_internal.c) — `CPU_PERCENT = 100 - lv_timer_get_idle()`.

**Rationale**: `lv_timer_get_idle()` returns the LVGL task's idle percentage — i.e. the time it spent waiting between scheduled callbacks. Below ~30 % idle, the next layout reload, screenshot encode, or font load can push the LVGL task past the TWDT (§1.13). Above 70 % idle, we have plenty of headroom for transient spikes.

We do not currently fail-fast when this drops; the reading just streams to a signal. The 30 % number is the threshold we'd alarm on if we surfaced it (proposed §6).

**Symptom of violation**: TWDT-adjacent prints (`task_wdt: Task watchdog got triggered. The following tasks did not reset the watchdog…`) become more frequent. `FPS` drops in lockstep.

### 1.3 CAN dispatch batch — 32 frames per LVGL tick

**Source**: [main/can/can_manager.c:350](../../main/can/can_manager.c) — `const int max_batch = 32;`.

**Rationale**: `can_process_queued_frames()` runs on the LVGL task every iteration. Draining the entire queue would let a noisy CAN bus monopolise the LVGL task and starve the renderer. 32 frames at typical 250 kbit/s with 8-byte payloads is ~10 ms of bus traffic — comfortably less than one 14 ms refresh period. It also gives a soft upper bound on per-tick signal-callback work: 32 frames × at most 16 subscribers each = 512 callback invocations per tick worst case (almost never approached in practice).

**Symptom of violation**: Raise this number too high → frame jank under heavy CAN load. Lower it → CAN dispatch latency climbs, signals visibly lag behind reality. The current 32 has held up across all bitrates we've tested.

### 1.4 CAN RX queue depth — 64 frames

**Source**: [main/can/can_manager.c:264](../../main/can/can_manager.c) — `xQueueCreate(64, sizeof(twai_message_t))`.

**Rationale**: The RX task on core 0 pushes; the LVGL task on core 1 drains in batches of 32. 64 = 2× batch size, so a single missed LVGL tick (e.g. a long redraw) can be absorbed without queue overflow. At a worst-case 1 Mbit/s bus that's ~5 ms of buffering — about half a refresh period.

**Symptom of violation**: `xQueueSendToBack` returns non-`pdPASS`; the RX task drops frames silently. Currently logged at `ESP_LOGD` level only. Bumping the queue depth past ~128 burns DRAM (~8 KB per 64 entries) and just defers the problem rather than fixing it; the right fix when this fails is usually to find what's blocking the LVGL task.

### 1.5 Signal subscriber early-out on unchanged value

**Source**: [main/widgets/signal.c:248-257](../../main/widgets/signal.c) — `if (was_stale || decoded != sig->current_value)` gate around `notify_subscribers`.

**Rationale**: Many CAN frames carry the same value as the previous frame (engine idle RPM, steady cooling temp). Re-running every subscriber callback for those would invalidate LVGL regions on every frame for no visible change. The early-out collapses identical updates to a single dispatch and lets `lv_obj_invalidate` mark only the truly-changed widgets dirty. The stale→fresh transition still fires so widgets can clear their stale-tint.

**Symptom of violation**: Removing this would visibly drop FPS at high CAN frame rates, even when on-screen values haven't changed. Has been tested by temporarily disabling the gate during refactor work.

### 1.6 Signals registered — ≤ 128

**Source**: [main/widgets/signal.h:23](../../main/widgets/signal.h) — `MAX_SIGNALS 128`. Allocated as a `heap_caps_calloc` array in [main/widgets/signal.c:43](../../main/widgets/signal.c); registration past the cap returns an error and logs `Signal registry full`.

**Rationale**: Signal lookup by name is a linear scan, used at layout-load time only (widget `from_json` resolves name→index once, then keeps the index). 128 entries × ~140 bytes of `signal_t` = ~18 KB of PSRAM. Most layouts use 30–80 signals; 128 gives ~50 % headroom. Pushing this higher would require either a hash-map lookup or accepting longer load times.

**Symptom of violation**: New signals fail to register at layout load. Surfaced as `ESP_LOGE` only — should also surface in `/api/system/health` (§6).

### 1.7 Signal subscribers per signal — ≤ 16

**Source**: [main/widgets/signal.h:24, 76](../../main/widgets/signal.h) — `MAX_SIGNAL_SUBSCRIBERS 16` and the inline `signal_subscriber_t subscribers[MAX_SIGNAL_SUBSCRIBERS]` array on every `signal_t`.

**Rationale**: A real-world signal has 1–4 subscribers (a panel, maybe an alert, maybe a meter). 16 covers pathological cases (e.g. `RPM` could feed an RPM bar, a meter, a shift light, a panel, a redline alert, plus night-mode mirrors of each). Inline storage avoids allocator pressure on subscribe paths that run during layout load. 16 × 16 bytes × 128 signals = 32 KB of unused-slot tax in the worst case.

**Symptom of violation**: `signal_subscribe` fails with `ESP_LOGW` and the widget silently never updates. The user sees a frozen value; serial log shows the warning.

### 1.8 Total widgets — ≤ 32

**Source**: [main/widgets/widget_registry.h:18](../../main/widgets/widget_registry.h) — `WIDGET_REGISTRY_MAX 32`. Enforced in [main/widgets/widget_registry.c:22](../../main/widgets/widget_registry.c) by `widget_registry_register` returning early.

**Rationale**: Empirical. A typical 800×480 dashboard shows ~12–18 widgets. 32 is generous and lets us keep `widget_t *s_widgets[WIDGET_REGISTRY_MAX]` as a flat fixed array — the registry is iterated on every layout reload, every night-mode toggle, and every signal-clear-test path, so flat-array iteration is the right shape.

**Symptom of violation**: Layout load silently truncates after the 32nd widget. Should also surface in `/api/system/health` (§6).

### 1.9 Per-type slot caps — panel 16, indicator 2, warning 8, others unbounded by-type

The slot cap is what limits a widget _type_; the registry cap (§1.8) limits the total. Per-type caps are scattered through the type-specific code (e.g. `widget_warning.c` checks `slot >= 8`). The numbers come from the on-screen real-estate budget rather than memory: with 800×480 and the panel layout we ship, 16 panels is the practical maximum before they overlap. The web editor's `WIDGET_DEFS` in [main/web/index.html](../../main/web/index.html) carries the same caps in JS.

These should not drift between firmware and the web editor. If they do, the web editor will let you create a widget that the firmware silently refuses to display.

### 1.10 Layout JSON — ≤ 32 KB

**Source**: [main/layout/layout_manager.h:33](../../main/layout/layout_manager.h) — `LAYOUT_MAX_FILE_BYTES 32768`. Enforced at write in [main/net/web_server.c](../../main/net/web_server.c) (`/api/layout/save` rejects > 32 KB with a structured 413), and at read in `layout_manager_load`.

**Rationale**: `cJSON_Parse` of a 32 KB layout takes a few hundred ms on this part. We could go larger — the LittleFS partition is 8.8 MB — but parse time is the real cost, and LVGL isn't running while we parse. The cap is also load-bearing: the web editor enforces it client-side via `_checkLayoutSize` so the user gets a clear "your layout has grown past 32 KB, remove some widgets" error before the firmware sees the request, instead of a generic 413 at the end of the upload.

The defaults-only `to_json` discipline ([widget code §to_json patterns](../../main/widgets)) keeps real layouts under ~12 KB even with 30+ widgets. If a single layout is approaching 24 KB, the next thing to look at is whether `to_json` is emitting fields that match the factory default.

**Symptom of violation**: HTTP 413 from `/api/layout/save` with a JSON body explaining the exact byte counts.

### 1.11 HTTP handler registrations — ≤ 128

**Source**: [main/net/web_server.c:4593](../../main/net/web_server.c) — `config.max_uri_handlers = 128`. Currently 87 `REGISTER_URI(server, …)` calls, leaving ~40 slots of headroom.

**Rationale**: ESP-IDF's `httpd` rejects new handler registrations once the cap is hit, **silently** — the registration call fails but server start continues. Late-registered handlers fall through to the wildcard CORS preflight handler and return 405. Diagnosed once via `/api/signal/simulate` POST returning 405 in production. The fix was to bump from 80 to 128 and add a register-tally log line at the end of `web_server_start`. Each slot is ~32 bytes of static RAM; 128 costs ~4 KB.

**Effective sub-budget**: ≤ 110 actual `REGISTER_URI` calls. 18 slots of headroom for the next round of endpoint adds before this needs revisiting.

**Symptom of violation**: Specific endpoints return 405. Boot log shows `URI registration: X/N FAILED — bump max_uri_handlers`.

### 1.12 HTTP request-header buffer — 1024 bytes

**Source**: [sdkconfig](../../sdkconfig) — `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`.

**Rationale**: Default 512 was too small for Android webview headers; bumped to 1024 in [ADR 0001](0001-wifi-onboarding-reliability.md). 2048 was tried and rolled back — burned ~14 KB across 7 simultaneous sessions.

**Symptom of violation**: Lower than 1024 → Android phones fail to load the editor with HTTP 431 + a `newlib` lock-init abort downstream. Higher than 1024 → out-of-memory in `fopen` paths under load.

### 1.13 TWDT timeout — 15 s

**Source**: [sdkconfig:1152](../../sdkconfig) — `CONFIG_ESP_TASK_WDT_TIMEOUT_S=15`. Bumped from the IDF default 5 s during boot-path work in 2026-04-20 (see MEMORY.md `project_2026_04_20_screenshot_control.md`).

**Rationale**: Long-running boot operations (layout instantiation across 30 widgets, font loads, image loads from LittleFS) can each consume several hundred ms while holding the LVGL mutex. The original 5 s was too aggressive — boot routinely tripped the watchdog mid-`_instantiate_widgets`. 15 s is the smallest value at which all observed boot paths complete without firing, with the explicit yields described in [docs/handover/09-conventions-and-pitfalls.md](../handover/09-conventions-and-pitfalls.md). Going much higher hides real bugs: 15 s is already long enough that the user notices the device "feels frozen."

**Symptom of violation**: Lower than 15 s → boot reboots with a `task_wdt: Task watchdog got triggered` log on cold start. Higher than 15 s → the device pretends to be alive while LVGL is wedged.

### 1.14 Data logger rates — discrete set {1, 2, 5, 10, 20, 50, 100, 200 Hz, 0=Max}

**Source**: [main/storage/data_logger.c](../../main/storage/data_logger.c) — `data_logger_start_with_rate` clamps to `≤ 1000`; web UI exposes the discrete set.

**Rationale**: The logger writes to FAT-on-SD; SD cards have unpredictable write-stalls in the 50–200 ms range. Fixed-rate sampling lets the user understand exactly what they captured ("100 Hz means I have 10 ms granularity"). The Max mode (`rate_hz == 0`) runs the LVGL timer at 1 ms and coalesces writes — empirically lands at 70–200 Hz depending on board load.

**Symptom of violation**: Logger fall-behind at 200 Hz under heavy LVGL load (more than ~6 widgets binding to the same noisy signal). Currently surfaced only via the `Logger status` line in the diagnostics screen; should add a "samples dropped" counter (proposed §6).

### 1.15 mDNS — disabled (memory budget exceeded)

**Source**: [main/net/mdns_service.c](../../main/net/mdns_service.c) `RDM7_MDNS_DISABLED 1`. Component fully removed in commit `c395e36`.

**Rationale**: The `espressif/mdns` managed component is hardcoded to `CONFIG_MDNS_MEMORY_ALLOC_INTERNAL=y` and could not allocate from the tight internal-RAM pool after WiFi init, despite ~3.8 MB total heap free. Switching to `CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y` would have been the right fix; we chose to drop mDNS instead and rely on the QR-code-scanning Network panel (Device Settings) plus the captive portal flow from [ADR 0001](0001-wifi-onboarding-reliability.md).

**Effective budget recorded by this**: anything that allocates more than ~4 KB from internal RAM after WiFi init is at risk. This is a soft boundary, not a number that lives in code, but it has been violated three times now (mDNS, the early `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM` bump, the early dns_hijack attempt with stack-resident buffers) and worth recording.

### 1.16 Screenshot pipeline — no shadow framebuffer

**Source**: Removed in 2026-04-20 work. Capture path reads `esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb0)` directly. See MEMORY.md `project_2026_04_20_screenshot_control.md`.

**Rationale**: The original capture path allocated a 768 KB shadow framebuffer in `flush_cb` and copied each LVGL flush into it. The shadow was deleted in favour of reading directly from the panel's own framebuffer at capture time. **Saved 768 KB of permanent PSRAM**, traded for a ~5 ms FB-readback when the user requests a screenshot. The dedup cache in `web_server_capture.c` (when split per [ADR 0002](0002-web-server-split-roadmap.md)) keeps repeat screenshots cheap.

**Symptom of violation**: PSRAM steady-state drops by ~768 KB if a future contributor reintroduces the shadow buffer "just to be safe." Track `FREE_PSRAM_KB` against the budget in §2.4.

## 2. Budgets we should commit to but haven't

These are not encoded anywhere. The numbers are proposals; the symptom of violation is what would let us validate the proposal.

### 2.1 LVGL frame time, p99 — ≤ 14 ms

**Proposed value**: p99 frame interval ≤ 14 ms. Equivalent to "no more than 1 % of frames miss the refresh deadline."

**Rationale**: Driven by §1.1's refresh period. Each missed deadline = one dropped frame = potentially-visible jank. 1 % at 70 fps is ~42 missed frames per minute, the empirical threshold below which a sweep looks "smooth" rather than "subtly choppy."

**Symptom of violation**: User reports of "the meter feels laggy when I rev." Meter needles take >2 frames to advance.

**How it could be measured**: A new `FRAME_TIME_MS_P99` internal signal that records flush-callback intervals into a 256-entry ring, computes the p99 every 500 ms, and clamps when the ring underflows. Cost: ~1 KB SRAM, a few μs per flush.

**Uncertainty**: The 14 ms target is conservative; in practice the ESP32-S3 + LVGL v8 + 800×480 RGB565 panel combination delivers around 11 ms p50 / 17 ms p99 today, so the budget would be flagged "amber" most of the time. Tightening the budget to 17 ms would make the alarm real-world useful; 14 ms is what the architecture aspires to.

### 2.2 LVGL idle — ≥ 30 %

**Proposed value**: `lv_timer_get_idle() ≥ 30` over a 5 s window.

**Rationale**: See §1.2. Below 30 % the next transient (layout reload, font load, screenshot) can push us into TWDT territory.

**Symptom of violation**: TWDT triggers, FPS dropouts during routine operations like opening Device Settings.

**How it could be measured**: Already streamed as the `CPU_PERCENT` internal signal. Currently shown as a number on the diagnostics SYSTEM card. Proposal in §6 is to colour the card amber when it crosses 70 % CPU (i.e. < 30 % idle) for ≥ 5 s.

### 2.3 CAN dispatch latency, p99 — ≤ 23 ms

**Proposed value**: time from `can_receive_task` enqueue to last subscriber callback completion, p99 ≤ 23 ms.

**Rationale**: [docs/handover/01-architecture.md](../handover/01-architecture.md) calculates the worst case as `5 ms (CAN recv) + 2 ms (queue dispatch) + 16 ms (next refresh) ≈ 23 ms`. p99 at this number means we're meeting the architecture's stated SLA.

**Symptom of violation**: Visible lag between a real-world event (you blip the throttle) and the on-screen response. Hard to detect from the dash alone — usually first noticed by users comparing against a co-driver's GoPro.

**How it could be measured**: Stamp `xQueueSendToBack` time into the `twai_message_t` (we don't use the spare bytes), record the delta in `can_process_queued_frames` after dispatch, log p99 every second. Cost: 8 bytes per queued frame plus a small histogram in PSRAM.

**Uncertainty**: We have never actually measured this. It could be much better than 23 ms in practice (queue is rarely full, dispatch costs are cheap), or much worse if a single noisy signal has many subscribers.

### 2.4 Steady-state heap floors — internal ≥ 50 KB, PSRAM ≥ 1 MB

**Proposed values**:
- `esp_get_free_heap_size()` ≥ 50 KB after dashboard is up, no transient operations in flight.
- `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` ≥ 1 MB at the same point.

**Rationale**:
- 50 KB internal floor leaves room for: one HTTP-request body buffer (up to LAYOUT_MAX_FILE_BYTES — see §1.10 — but most allocs land in PSRAM), TLS handshake state for OTA, a font load, a JSON parse. Below 50 KB we hit `fopen` failures because newlib opens stdio buffers from internal RAM.
- 1 MB PSRAM floor leaves room for: a layout reload (allocates a new layout-string buffer), a screenshot full-resolution encode (~960 KB transient), and one deferred `lv_async_call` queue. Below 1 MB the screenshot fall-back to half-res kicks in (already coded), but some other PSRAM-hungry path will fail next.

**Symptom of violation**:
- Internal < 50 KB: `fopen` fails, image loads fail, OTA fails to start. Often shows as "the SD card stopped working" because `f_open` is the visible call that errors.
- PSRAM < 1 MB: layout reloads start truncating, font loads fail, screenshots silently degrade.

**How it could be measured**: Both are already reported every 500 ms via `FREE_HEAP_KB` and `FREE_PSRAM_KB` internal signals. The work is: define "steady state" (proposed: 30 s of no `dashboard_init`, no `/api/screenshot`, no OTA, no layout save), watch the floor over that window, alarm when the floor dips below the budget. Currently nothing alarms.

### 2.5 Boot-to-dashboard time — ≤ 5 s

**Proposed value**: Time from `app_main` entry to the first frame painted by `dashboard_init`, ≤ 5 s.

**Rationale**: The user feels anything beyond ~3 s as a long boot; ~7 s is when they start to wonder whether the device is broken. 5 s is the comfortable middle. We're observed to land at ~3.5–4.0 s on a warm flash with a typical 18-widget layout; the budget gives ~1 s of headroom for adding a widget type or a new boot-time subsystem.

**Symptom of violation**: User-perceived "this thing takes ages to start." Manifests differently on cold-boot SD-card init delays.

**How it could be measured**: Stamp `esp_timer_get_time` at `app_main` entry; emit on `dashboard_init` first-frame; `signal_inject_test_value("BOOT_MS", …)` once per boot. Reference value would land in `/api/system/health`.

### 2.6 LVGL task stack high-water — ≥ 2 KB headroom

**Proposed value**: `uxTaskGetStackHighWaterMark(lvgl_task_handle)` ≥ 2 KB at steady state.

**Rationale**: The LVGL task is 16 KB stack ([main.c](../../main/main.c) §step 11). Calling deep into widget hierarchies, font cache, image loaders, and `cJSON_Parse` (during `apply_overrides` rule eval) all eat stack. A 2 KB floor catches creeping stack growth before it overflows. We've never had an LVGL stack overflow in production, but we've also never measured the headroom.

**Symptom of violation**: Stack overflow → `Guru Meditation Error: ... Cause: 0x36 (StoreProhibited)`. Different error from a canonical TWDT.

**How it could be measured**: Sample `uxTaskGetStackHighWaterMark` every 5 s in `signal_internal.c`; expose as `LVGL_STACK_FREE` internal signal.

**Uncertainty**: 2 KB is a guess. We could reasonably hold the line at 1 KB; below that, a single recursive call could clip the canary.

### 2.7 Per-widget redraw count — ≤ 1 per frame

**Proposed value**: Each widget root invalidates the LVGL framebuffer at most once per refresh period.

**Rationale**: LVGL coalesces multiple invalidations of the same region within a single refresh cycle into one redraw, but the bookkeeping cost grows with invalidation count. A single hyperactive widget that calls `lv_obj_invalidate` from every frame's signal callback can dominate redraw cost. We have no data on how often this happens.

**Symptom of violation**: FPS drops when a specific widget is on-screen. Currently impossible to attribute without staring at LVGL source.

**How it could be measured**: New per-widget invalidation counter incremented from a wrapper on `lv_obj_invalidate`; reset every refresh; expose top-3 offenders via a debug endpoint. Significant work — defer until we have a reproducible FPS-drop case.

### 2.8 Signal dispatch latency, p99 — ≤ 1 ms

**Proposed value**: p99 time from `notify_subscribers` entry to last subscriber callback exit ≤ 1 ms.

**Rationale**: At 16 subscribers per signal (§1.7) and a typical signal callback (a label `set_text`, maybe a colour change, no allocation), 1 ms gives ~62 μs per subscriber — a comfortable budget. Anything past 1 ms means a subscriber is doing real work it shouldn't be doing on the LVGL hot path (allocating, calling into LittleFS, holding a mutex).

**Symptom of violation**: A specific signal causes FPS dips. Caught today only by removing widgets one at a time until the dip stops.

**How it could be measured**: Wrap `notify_subscribers` with `esp_timer_get_time` deltas; histogram per-signal in PSRAM; expose top offenders via `/api/diagnostics/signals`.

### 2.9 LittleFS free space — ≥ 500 KB

**Proposed value**: LittleFS partition free ≥ 500 KB at steady state.

**Rationale**: 500 KB headroom covers: one full image upload (currently capped at ~200 KB), one font upload (TTF can be ~200 KB), a few layouts, with margin. Below this the user starts hitting `ENOSPC` on uploads and the editor's error UX is poor.

**Symptom of violation**: Uploads fail. Surfaces today as a vague "Upload failed" toast in the editor; would benefit from a pre-upload size check against `/api/storage/info`.

**How it could be measured**: `/api/storage/info` already returns LittleFS free bytes; the budget would be enforced in the editor's pre-upload check rather than on the firmware.

## 3. Instruments we have

| Instrument | Source | Update rate | Available where |
|---|---|---|---|
| `FPS` internal signal | [signal_internal.c:64-74](../../main/widgets/signal_internal.c) | 2 Hz | Anywhere subscribers run; diagnostics SYSTEM card |
| `CPU_PERCENT` internal signal | [signal_internal.c:77](../../main/widgets/signal_internal.c) | 2 Hz | As above |
| `FREE_HEAP_KB` internal signal | [signal_internal.c:81](../../main/widgets/signal_internal.c) | 2 Hz | As above |
| `FREE_PSRAM_KB` internal signal | [signal_internal.c:85](../../main/widgets/signal_internal.c) | 2 Hz | As above |
| `UPTIME_S` internal signal | [signal_internal.c:89](../../main/widgets/signal_internal.c) | 2 Hz | As above |
| `CHIP_TEMP` internal signal | [signal_internal.c:93](../../main/widgets/signal_internal.c) | 2 Hz | As above |
| Diagnostics screen | [ui/screens/ui_diagnostics.c](../../main/ui/screens/ui_diagnostics.c) | 1 Hz refresh | Device Settings → System Diagnostics |
| `/api/system/health` | [web_server.c:4272-4304](../../main/net/web_server.c) | On request | Web editor / curl |
| Web editor "Stream" rate | `_BG_REFRESH_INTERVAL_MS` | User-selectable 1/3/5/10/30 s | Any web client |
| `signal_t.peak_value` / `min_value` / `session_peak` / `session_min` | [signal.c:240-243](../../main/widgets/signal.c) | Per-frame | Any subscriber, Peaks screen |
| Boot log `URI registration: N handlers registered (cap M)` | [web_server.c:4756-4768](../../main/net/web_server.c) | Once per boot | `idf.py monitor` |
| `task_wdt:` panic prints | ESP-IDF | On TWDT fire | `idf.py monitor`, crash dump |

The cluster of internal signals from `signal_internal.c` is the existing sensor mesh. Anything we add a budget for in §2 should land here so it's available to widgets, the diagnostics screen, and `/api/system/health` uniformly.

## 4. Instruments we don't have

| Missing instrument | What it would unlock | Cost (rough) |
|---|---|---|
| Per-task CPU breakdown | Attribute "low CPU idle" to LVGL vs HTTP vs CAN-RX | `vTaskGetRunTimeStats`; needs `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` (already on) and `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` (would need enabling) |
| Per-task stack high-water | Catch creeping stack growth before overflow | `uxTaskGetStackHighWaterMark` per task, polled every 5 s |
| Frame-time histogram (p50 / p99 / max) | §2.1 budget | 256-entry ring per task; ~1 KB |
| CAN dispatch-latency histogram | §2.3 budget | Timestamp at enqueue, delta on dispatch; per-signal histograms ~500 bytes each |
| Per-widget invalidation count | §2.7 budget | Wrap `lv_obj_invalidate`; significant; defer |
| Per-signal dispatch-time histogram | §2.8 budget | Wrap `notify_subscribers`; ~500 bytes per signal |
| LittleFS write latency | Catch SD/flash slowing under wear | `esp_timer_get_time` deltas around `fwrite` |
| HTTP handler time-in-handler | Catch handlers that hold the LVGL lock too long | Wrap REGISTER_URI dispatch; ~50 bytes per handler |
| RX queue high-water | Catch CAN load that approaches §1.4 cap | `uxQueueMessagesWaiting` polled every 100 ms; cheap |
| Heap fragmentation index | Catch bad allocator behaviour in long uptimes | `multi_heap_get_info`; already available, just unsurfaced |

The cheapest wins from this list: RX queue high-water and per-task stack high-water. Both are single function calls per measurement, both expose risks we currently can't see.

The most expensive: per-widget invalidation count and per-task CPU breakdown. Both add nontrivial overhead to every redraw or scheduler tick respectively. Defer until we have a concrete bug those instruments would solve.

## 5. Failure modes today

When a budget is violated today, the alarm surfaces in one of these places, mostly by accident:

| Budget | Where violation surfaces today |
|---|---|
| §1.1 frame time | Nowhere automatic. User says "looks janky." |
| §1.3 CAN batch / queue overflow | `ESP_LOGD` only. Effectively invisible. |
| §1.6 signals registered | `ESP_LOGE` from `signal_register`. Visible only on `idf.py monitor`. |
| §1.7 subscribers | `ESP_LOGW`. Same. |
| §1.8 widgets | Silent. Layout truncates. |
| §1.10 layout JSON | HTTP 413 with body; web editor's `_checkLayoutSize` catches this client-side first. Reasonable today. |
| §1.11 handler cap | Boot-log tally; `URI registration: X/N FAILED`. Caught only if developer reads the log. |
| §1.13 TWDT | Reboot + crash dump. Loud but late. |
| §2.4 heap floor | Nothing. Symptoms manifest as random `fopen` / `lv_*_create` failures elsewhere. |

The pattern: most violations surface as `ESP_LOG*` lines visible only on a connected serial console, OR they reboot the device after the fact. Neither is a path a non-developer user can act on.

## 6. Proposed failure-detection workflow

For every budget that has a measurable instrument, surface violations in three places consistently:

### 6.1 `ESP_LOGW` on transition

Log a single warning line on the budget-crossed transition (not every frame). Idempotent — once the budget is back inside spec, log a single `ESP_LOGI` recovery line. Don't spam the log when the system is hovering at the threshold.

Convention:

```c
ESP_LOGW(TAG, "perf: %s budget exceeded — %s = %.1f, budget %.1f%s",
         "lvgl_idle", "cpu_pct", cpu_pct, 70.0,
         " (>70 = idle <30 = §1.2)");
```

The `§1.2` reference points back to this ADR so the developer who sees the line in `idf.py monitor` can find the rationale without grepping.

### 6.2 Diagnostics screen highlight

The existing diagnostics screen has 5 cards (CAN BUS / WI-FI / SYSTEM / SD CARD / SIGNALS). Add a subtle colour-shift on the relevant card when one of its budgets is currently in violation:

| Card | Highlight when |
|---|---|
| CAN BUS | RX queue high-water > 32 (= half-full, §1.4), or `rx_missed > 0` |
| SYSTEM | CPU idle < 30 % (§1.2, §2.2), or free heap < 50 KB (§2.4), or PSRAM < 1 MB (§2.4), or LVGL stack < 2 KB headroom (§2.6) |
| SIGNALS | Any signal at `MAX_SIGNAL_SUBSCRIBERS` (§1.7), or registry past 90 % full (§1.6) |
| SD CARD | LittleFS free < 500 KB (§2.9) |
| WI-FI | RSSI < -80 dBm (already amber-worthy; not in this ADR's scope) |

The existing accent colour scheme (`THEME_COLOR_ACCENT_BLUE` for nominal, `THEME_COLOR_ACCENT_AMBER` for warning) is already in use ([ui_diagnostics.c:444](../../main/ui/screens/ui_diagnostics.c) shows SD CARD already starts in amber). The pattern: a card glows amber while at least one of its sub-budgets is in violation.

### 6.3 `/api/system/health` field

The endpoint already returns `uptime_s`, `heap_free`, `heap_min_free`, `psram_free`, `wifi_rssi`. Add a single string field `budgets` whose value is a comma-separated list of budgets currently in violation. Empty string means all green.

```json
{
  "uptime_s": 3742,
  "heap_free": 41250,
  "heap_min_free": 28000,
  "psram_free": 980000,
  "wifi_rssi": -65,
  "budgets": "heap_floor,psram_floor"
}
```

The web editor and the desktop app can poll this and show a banner when `budgets` is non-empty. Today neither does — but `/api/system/health` is already polled by `_dmRefreshHealth` in the desktop app's Device Manager modal.

### 6.4 Why this layered surfacing

Each layer catches a different audience:

- `ESP_LOGW` is for the developer who has a serial console open during dev. It records *when* the crossing happened so you can correlate with what you just changed.
- The diagnostics card is for the user (or shop technician) standing in front of the dash. It says "something is off" in a place they're already looking when they suspect a problem.
- `/api/system/health` is for tooling (the desktop app, the web editor, future fleet-monitoring) to detect violations without a screen present.

Today only the third audience has any signal at all (the raw heap numbers). The first two have nothing.

## 7. Trade-offs

**Picking numbers without measurement.** The §2 numbers are educated guesses. Instrumenting before alarming is the right approach: each new budget should land in two commits — first the measurement, run it for a few days to see real distributions, *then* add the threshold. Anything else risks adding noisy alarms that get ignored.

**Performance overhead of measurement.** Every histogram and timestamp costs cycles. The §4 cheap wins (RX queue high-water, per-task stack) are essentially free. The expensive ones (per-widget invalidation count) need their own ROI conversation. This ADR doesn't authorise the expensive ones.

**Drift between firmware and the editor.** Budgets like §1.7 and §1.10 are also enforced in the web editor. If we change the cap in firmware, the editor must change too. Today this is caught by the failing save round-trip; a `/api/limits` endpoint that returns the current caps would let the editor stay in lockstep automatically. Not in scope here.

**False alarms during transient operations.** OTA, layout reload, and screenshot all spike heap and CPU briefly. The threshold logic in §6 needs hysteresis (≥ 5 s in violation, not instantaneous) or it'll fire on every layout reload.

**The "alarm fatigue" risk.** Adding violation surfaces only helps if developers act on them. If we add a `budgets` field that's almost always non-empty, we'll just tune it out. The numbers should be conservative enough that a non-empty list genuinely means something is wrong, not "the device is being used."

## 8. How to verify

This ADR is a planning document; verification means cross-checking the numbers against the code today.

### Mechanical cross-checks (no instrumentation needed)

```bash
# §1.1 — verify LVGL refresh period
grep LV_DISP_DEF_REFR_PERIOD main/lv_conf.h
# expected: 14

# §1.3, §1.4 — verify CAN batch + queue
grep -E 'max_batch|xQueueCreate.*twai_message_t' main/can/can_manager.c
# expected: max_batch = 32, xQueueCreate(64, ...)

# §1.5 — verify subscriber early-out
grep -A2 'was_stale ||' main/widgets/signal.c
# expected: notify_subscribers gated by (was_stale || decoded != current_value)

# §1.6, §1.7 — verify signal caps
grep -E 'MAX_SIGNALS |MAX_SIGNAL_SUBSCRIBERS' main/widgets/signal.h
# expected: MAX_SIGNALS 128, MAX_SIGNAL_SUBSCRIBERS 16

# §1.8 — verify widget cap
grep WIDGET_REGISTRY_MAX main/widgets/widget_registry.h
# expected: 32

# §1.10 — verify layout JSON cap
grep LAYOUT_MAX_FILE_BYTES main/layout/layout_manager.h
# expected: 32768

# §1.11 — verify URI handler cap + actual count
grep max_uri_handlers main/net/web_server.c
grep -c 'REGISTER_URI(server' main/net/web_server.c
# expected: 128, ~87

# §1.12, §1.13 — verify sdkconfig values
grep -E 'HTTPD_MAX_REQ_HDR_LEN|TASK_WDT_TIMEOUT_S' sdkconfig
# expected: 1024, 15
```

If any of those return a different value than this ADR claims, fix this ADR (or the code, depending on whose number is right) before merging changes.

### Field verification (when instrumented)

For each budget added under §2, the verification ritual is:

1. Subscribe to the budget's instrument in the diagnostics screen.
2. Trigger the worst-case scenario for the budget (e.g. for §2.4 PSRAM floor: load the most expensive layout, take a full-resolution screenshot, watch `FREE_PSRAM_KB`).
3. Confirm the budget is not exceeded under that scenario.
4. Tighten or relax the budget number in this ADR if reality says different.

Don't ship a budget that isn't reflective of observed behaviour. That just trains people to ignore the alarm.

## 9. What we did NOT do (and why)

- **Did not instrument anything in this ADR.** The point is to write the budgets down before sprinkling counters across the code. Instrumentation lands in follow-up commits.
- **Did not propose a /api/perf/* endpoint family.** Once instrumentation lands, a dedicated perf endpoint may be useful for tooling, but the existing `/api/system/health` plus internal-signal subscriptions covers the planned surfacing without a new family of URIs.
- **Did not budget the WiFi/AP layer.** [ADR 0001](0001-wifi-onboarding-reliability.md) already covers the WiFi reliability budget (channel, bandwidth, captive portal, DNS). Don't duplicate.
- **Did not budget the OTA layer.** OTA has its own backoff/timeout discipline; a separate ADR would be more useful than smuggling it in here.
- **Did not propose a real-time scheduler.** Cooperative scheduling on a 500 Hz tick (see [docs/handover/01-architecture.md](../handover/01-architecture.md) §the-500-hz-tick-gotcha) is the model. Switching to a preemptive priority scheme for LVGL would invalidate every budget here.

## Related

- [ADR 0001 — Wi-Fi Onboarding Reliability](0001-wifi-onboarding-reliability.md) — sibling ADR for the network layer's empirical budgets (HT20, channel 11, header buffer 1024).
- [ADR 0002 — `web_server.c` Split Roadmap](0002-web-server-split-roadmap.md) — the §1.11 HTTP handler cap is the load-bearing budget for that split's safety check.
- [ADR 0003 — Desktop `index.html` Sync Plan](0003-desktop-index-sync-plan.md) — the desktop's `/api/system/health` poller is one of the consumers of §6.3.
- [docs/handover/01-architecture.md](../handover/01-architecture.md) — threading model, CAN dispatch latency calculation, memory layout. The ground truth this ADR's budgets are derived from.
- [docs/handover/04-signal-and-can.md](../handover/04-signal-and-can.md) — signal registry detail, dispatch path, subscriber model.
- [docs/handover/08-aux-systems.md](../handover/08-aux-systems.md) — data logger and replay rates referenced in §1.14.
- [docs/handover/09-conventions-and-pitfalls.md](../handover/09-conventions-and-pitfalls.md) — the 500 Hz tick gotcha, yield discipline. Several of the §1 defences exist because of these conventions.
- [main/widgets/signal_internal.c](../../main/widgets/signal_internal.c) — the existing instrument home; new budget instruments should land here.
- [main/ui/screens/ui_diagnostics.c](../../main/ui/screens/ui_diagnostics.c) — the existing display surface; §6.2 hooks into it.
- [main/net/web_server.c](../../main/net/web_server.c) (`_system_health_handler`) — the existing API surface; §6.3 extends it.
