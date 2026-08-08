# RDM-7 Technical Reference

**Product:** RDM-7 Dash — 7" ESP32-S3 automotive dashboard by Realtime Data Monitoring (RDM), plus its desktop companion app **RDM Studio**.
**Firmware version at time of writing:** 1.1.30 (`FIRMWARE_VERSION` in `main/include/version.h`). Layout schema **v17** (`LAYOUT_SCHEMA_VERSION` in `main/layout/layout_manager.h`). RDM Studio **v0.4.47**.
**Audience:** another Claude instance writing customer-facing documentation. This document is the source of truth about what the product actually does, including behaviour that diverges from what names/UI suggest. Dense and complete over readable. No marketing language.

**Out of scope (in development, do not document for customers):**
- **CAN button pads / keypads** (Blink PKP family). Firmware has zero keypad support today; RDM Studio ships a Keypad Configurator workspace in design/export mode only ("live keypad sync coming soon") — it generates provisioning CAN frames and DBC/setup exports but talks to no device.
- **GPS / lap timing.** The lap engine is being torn out/reworked on the current branch (`web_server_lap.c` deleted; `lap_engine.c` remains compiled but has no HTTP surface and is inert without a GPS node on the bus). RDM Studio ships a full "RDM GPS & Lap Timing" workspace that requires the separate in-development RDM GPS puck.

**Ship-state flags to be aware of (both affect what customers can actually do):**
- `RDM7_DEBUG_KEEP_CONSOLE = 1` is the current shipping default (`main.c`): `uart_protocol_init()` is **skipped**, so **RDM Studio's USB transport does not work against shipping firmware** — USB-connected users must use WiFi. (SECURITY.md documents this as accepted; the in-code comment says "TEMP debug — REVERT to 0".)
- `NIGHT_MODE_DISABLED = 1` (`night_mode.h`): the entire night-mode subsystem is currently a **no-op** — `night_mode_is_active()` always returns false, subscriptions are refused, widgets skip building night dual-objects. The full night-override machinery (documented in §4) is present but inert until this flag is flipped.
- Display rotation persists to NVS (`display_cfg/rot`) but is **never applied** — the UI button is hidden.

**Contents:** [1 Hardware](#1-hardware) · [2 Getting on the network](#2-getting-on-the-network) · [3 HTTP API](#3-http-api) · [4 Layout JSON](#4-layout-json) · [5 Channels & signals](#5-channels--signals) · [6 Storage & persistence](#6-storage--persistence-reference) · [7 RDM Studio](#7-rdm-studio-desktop-app) · [8 Appendix: documentation drift](#8-appendix-known-documentation-drift-repo-docs-that-contradict-the-code)

Where a fact is not stated anywhere in the repo (notably current draw, connector part numbers, enclosure specs, the CAN transceiver and panel part numbers), this document says so explicitly rather than guessing — see §1.5. Treat those as open questions for the hardware owner, not as things to write around.

---

## 1. Hardware

### 1.1 Compute

| Item | Value |
|---|---|
| SoC | ESP32-S3, dual-core Xtensa @ 240 MHz |
| Flash | 16 MB (DIO @ 80 MHz) |
| PSRAM | Octal PSRAM @ 80 MHz, XIP enabled. **8 MB** (per the user guide and code comments; sdkconfig uses `SPIRAM_TYPE_AUTO` so the size is not config-stated) |
| Watchdogs | Task WDT 15 s with panic; interrupt WDT 800 ms. Only the LVGL render task is TWDT-subscribed (armed on its first successful frame). The CAN RX task is deliberately not subscribed (a quiet bus must not panic) |
| FreeRTOS tick | 500 Hz — `pdMS_TO_TICKS(1) == 0`, a documented pitfall |
| Flash encryption / secure boot | **Not enabled** (explicitly waived in SECURITY.md for v1.x) |

### 1.2 Display

- **7" 800×480 16-bit parallel RGB565 LCD** (`esp_lcd_new_rgb_panel`, data_width 16), pixel clock 14 MHz, continuous refresh, two 768 KB framebuffers in PSRAM (double-FB).
- Touch: **GT911 capacitive** over I²C0 (SDA GPIO 8, SCL GPIO 9, 400 kHz, addr 0x5D latched via the INT line on GPIO 4). Init retried 3×; total failure → the dash runs without touch. A **CH422G I/O expander** on the same bus handles GT911 reset and the SD CS line.
- Backlight: **LEDC PWM on GPIO 16**, 5 kHz, 13-bit; brightness API clamps 1–100. No separate backlight-enable pin.
- LVGL **v8.3** (v9 deliberately not used), 16-bit colour, render task on **core 1** (16 KB stack, prio 8); two 120-line PSRAM draw buffers (~192 KB each) with SRAM fallback; a cross-core flush worker task on core 0 (prio 10). Refresh period 16 ms; realistic frame rates on heavy layouts are ~13–60 fps depending on layout cost (the "70 Hz" figure in the user guide is a target, not a measurement).
- Screen profiles are compile-time Kconfig options: 800×480 rect (default), 480×480, 720×720, rect/round shape. Firmware code uses `SCREEN_W/H/ORIGIN_X/ORIGIN_Y` from `screen_config.h`; **(0,0) is the screen centre** everywhere.

### 1.3 CAN

- ESP32-S3 **TWAI** controller, TX GPIO 20, RX GPIO 19. The external transceiver part number is not stated anywhere in the repo.
- Bitrates by index: 0=125k, 1=250k, 2=500k (default), 3=1M. Persisted in NVS `can_config/can_bitrate`. Changing it via the API takes effect **on next boot only**.
- ISO 11898; 11-bit and 29-bit (OBD2 extended, NVS `can_config/obd_ext`).
- Acceptance filter is dynamically narrowed to subscribed IDs; promiscuous mode (used by monitors/scans) forces accept-all.
- Listen-only bitrate auto-scan: 2 s per bitrate, up to 32 unique IDs tracked per rate, driver install retried 3× with backoff.
- Bus-off recovery: up to 10 retries, 100 ms → 5 s exponential backoff.
- **Termination:** built-in 120 Ω resistor selected by a jumper ("yellow terminator block, bottom-right corner under the rear cover" per the user guide). No firmware involvement.
- CAN RX task: prio 7, core 0, PSRAM stack; frames go via queue to the LVGL task, which decodes them under the LVGL mutex.

### 1.4 Other physical I/O

| Input | Pins | Details |
|---|---|---|
| Indicator wire inputs | GPIO 43 (left), GPIO 44 (right) | Input + pull-down, **active-high** 12 V-switched feeds (via external conditioning), polled at 20 Hz by a dedicated task. **Shared with UART1** (the desktop USB serial pads) — a physical hardware switch selects which peripheral owns the pads; the firmware side is the NVS flag `wire_input/enabled` (default false = UART owns them). Reboot required either way. Also published as registry signals `INDICATOR_LEFT`/`INDICATOR_RIGHT` at 2 Hz |
| Fuel sender | GPIO 6 (ADC1 ch 5) | 12-bit, 0–3.3 V, oneshot. Published as signal `FUEL_SENDER_V`; supports 2-point and up to 8-point calibration curves |
| SD card | SDSPI: MOSI 11, CLK 12, MISO 13, CS 4 (via CH422G) | 10 MHz, FAT, mounted at `/sdcard`, non-fatal if absent. Hot-removal detected (2 consecutive info failures marks the card dead); **re-insertion requires reboot** by design |
| USB | USB-C; UART bridge chip + ESP32-S3 native USB-Serial-JTAG as secondary console | The exact bridge part is not confirmed in the repo (dev machine observes a CH343; the handover doc says "CP2102 / CH340 / ESP USB-Serial-JTAG depending on board revision"). Console UART0 @ 115200 |

### 1.5 Power, wiring, what's in the box

Per `RDM-7_User_Guide.md` (the only source; **the repo states no current-draw figure, no operating voltage range, no connector part numbers, no enclosure dimensions/weight/IP/temp ratings — do not invent these**):

- Power: **12 V DC switched** ("Red → +12 V switched ignition source, not constant 12 V").
- Harness: 4-wire pigtail — Red +12 V switched, Black ground, Green CAN High, Yellow CAN Low.
- In the box: RDM-7 display unit (7" touchscreen, rear-mount connector), CAN + power harness, mounting hardware (brackets and screws for dash-panel install), USB-C cable if ordered with the Desktop Studio option.
- Mounting: any flat panel; supplied brackets; 0–20° off vertical is fine. There is no separately-priced mount accessory in the current docs.
- Pricing (from `docs/PLATFORM_PLAN_2026-07.md`): **A$599 RRP, A$299 beta**.

### 1.6 Partitions and boot

Partition table (16 MB, no factory partition — every boot runs from an OTA slot):

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 144 KB |
| otadata | data/ota | 0x2d000 | 8 KB |
| phy_init | data/phy | 0x2f000 | 4 KB |
| ota_0 | app | 0x30000 | 3.5 MB |
| ota_1 | app | 0x3B0000 | 3.5 MB |
| littlefs | data/spiffs | 0x730000 | 8.8125 MB (to end of flash) |

Boot sequence essentials: backlight PWM first → NVS → crash log → LittleFS mount + boot-asset seeding + default-layout validation (a corrupt `default.json` is **regenerated**, not just fallen back from) → channel manager → CAN driver install → display/touch bring-up → LVGL task on core 1 → CAN RX task → splash (itself a JSON layout, `_splash_<name>.json`) → dashboard reveal (fade or 16 px curtain sweep; signal dispatch paused during the sweep) → first-run wizard on virgin devices (4 steps: bitrate scan, ECU auto-detect, channel review, connection options) → WiFi via a one-shot timer at +4 s → web server.

Layered fallbacks (no single "safe mode"): touch-less operation, SRAM draw-buffer fallback, LittleFS auto-format on mount failure, default-layout regeneration, hardcoded fallback layout, SD-less operation with `/lfs/logs` logging, forced-AP when no credentials exist, AP-only hard drop after 2 auth-class STA failures, and OTA bootloader rollback for any image that fails to render 10 s (the LVGL task calls `esp_ota_mark_app_valid_cancel_rollback()` only after ≥10 s of healthy rendering — this also means **do not power-cycle a freshly flashed dash within ~10 s of first render** or it reverts to the previous slot).

Crash logging: NVS-backed (`crash_log` namespace) — previous reset reason, firmware, uptime, lifetime `panic_count`. No backtraces (no coredump partition). Exposed on the Diagnostics screen and `GET /api/selftest`.
---

## 2. Getting on the network

### 2.1 Access point (hotspot)

- SSID: **`RDM7-XXXX`** where XXXX is the last two bytes of the SoftAP MAC in hex.
- IP: **192.168.4.1**, channel 11 (deliberately not 1 — phone association timeouts), HT20, max 4 clients, WiFi power-save off.
- Password: **per-device derived** — SHA-256 of (SoftAP MAC ‖ salt `"rdm7-ap-pass-v1"`) mapped into a confusion-free alphabet (no 0/O/1/l/I), **10 characters**. WPA2-PSK when the stored password is ≥8 chars, otherwise the AP is **open**. The legacy fleet-wide password `rdm7dash` (pre-2026-06-12 units) is auto-upgraded to the derived one on load. The salt is public in firmware; this is documented as a casual-attacker deterrent only.
- The web editor lives at `http://192.168.4.1/` (port 80, plain HTTP, no TLS, **no authentication anywhere on the HTTP API**).

### 2.2 Captive portal

When the AP is up, a DNS hijack task answers **every** DNS query with A-record 192.168.4.1 (TTL 60 s), and DHCP advertises the dash as the DNS server. Nine HTTP probe paths (iOS `/hotspot-detect.html` + `/library/test/success.html`, Android `/generate_204` `/gen_204` `/generate204`, Windows `/connecttest.txt` `/ncsi.txt` `/redirect`, Firefox `/success.txt`) all 302-redirect to `http://rdm-7-dash/` with a meta-refresh body (some iOS versions ignore a bare 302). `rdm-7-dash` is cosmetic branding for the captive sheet's address bar — **it is not a real hostname**.

### 2.3 Joining a network (STA)

- **No mDNS, no hostname.** mDNS was removed 2026-04-27 (memory pressure). Discovery is by IP: the dash shows its DHCP IP in Device Settings and via QR; RDM Studio finds dashes with a parallel HTTP subnet sweep probing `/api/device/info` on every /24 the PC sits on (128-way concurrency, ~2 s for 254 hosts), matching devices by `serial`.
- Credentials: up to **5 saved networks** (NVS `wifi_cfg`, slots `ssid0..4`/`pw0..4`; slot 0 mirrored to legacy `ssid`/`password` keys). Same-SSID save overwrites; full list evicts oldest.
- Mode selection at boot: saved STA + AP-enabled → APSTA; AP only → AP; STA only → STA. Safety net: if AP is off **and** no STA credentials exist, the AP is force-enabled and persisted (a dash can never boot unreachable).
- Connect flow: active scan first (120/1500 ms per channel — tuned for lazy phone-hotspot beacons), connect only if the SSID is visible; `CONNECTED` state only on got-IP. On got-IP the dash also starts SNTP (pool.ntp.org — needed for the CAN-upload HMAC timestamp) and arms the OTA boot check (+15 s).
- Reconnect: 5 fast attempts with 2 s→30 s exponential backoff, rotating through the saved-network list, then a **5-minute slow retry forever** (state reads FAILED but retries continue). **Auth-class failures** (wrong password etc.) hard-stop after **2** strikes and drop to AP-only — this is an IDF PHY-init timer-slot leak workaround (3+ PHY cycles → `esp_timer` NO_MEM → abort); recovery is re-entering the password at 192.168.4.1.
- WiFi settings API (`/api/wifi/config`) never returns the stored password (`has_password` only), an empty-string `password` in a POST means *keep existing* (there is no API way to clear it), and a config POST does **not** reconnect — changes apply on next connect/boot.

### 2.4 USB serial

Two distinct serial surfaces:

1. **Console** — UART0 @ 115200 (plus native USB-Serial-JTAG as secondary). ESP-IDF logs.
2. **Desktop protocol** — UART1 @ **921600** on the same physical pads as GPIO 43/44 (hardware switch selects UART vs indicator-wire use). Framed protocol: `[STX 0x02][len u32 LE][payload][CRC16-CCITT LE][ETX 0x03]`; payload type byte 0x00=JSON (JSON-RPC style `{"id":n,"method":"layout.list","params":{}}`), 0x01=binary chunk (`[session u64][chunk_idx u16 LE][data]`, 4 KB chunks, per-chunk ACK, `upload.start`/`upload.abort`/`upload.finish` session methods). Max payload 64 KB. ESP_LOG output is interleaved as raw ASCII in the same stream; the desktop parser splits frames from log lines. ~52 RPC methods mirror the HTTP API (`layout.*`, `image.*`, `font.*`, `signal.*`, `log.*`, `sd.*`, `brightness.*`, etc.).
   **Currently disabled in shipping builds** (`RDM7_DEBUG_KEEP_CONSOLE=1` skips `uart_protocol_init`); see the ship-state flags at the top. USB CDC (native OTG) exists in-tree but is compiled out (PHY conflict with USB-Serial-JTAG).
---

## 3. HTTP API

Single `esp_http_server` instance, port **80**, plain HTTP, **no inbound authentication of any kind**, CORS `Access-Control-Allow-Origin: *` (where set — see below). One shared httpd task (5 KB stack, pinned to core 0) serves **everything sequentially** — any long handler blocks the whole API (MJPEG stream: indefinitely; OBD2 snapshot: up to 12 s; OBD2 scan: 8 s). `max_uri_handlers = 160`, currently **138 registered** (the in-code "148" comment predates the lap-endpoint deletion). Registration failures are tallied and exposed at `GET /api/selftest → uri_registration`; when the cap is hit, unregistered `/api/*` paths return **405 (not 404)** because they fall through to the OPTIONS wildcard with a method mismatch.

General behaviour:

- A single `OPTIONS /api/*` preflight handler returns 204 with permissive CORS. It only covers `/api/*` — `/`, `/screenshot`, `/favicon.ico`, and the captive-portal paths have no preflight.
- **CORS on responses is inconsistent.** Most endpoints set `Access-Control-Allow-Origin: *`; a batch do not (notably `/api/layout/version`, `/api/signals/values`, `/api/signal/simulate`, all `/api/fuel/*`, all `/api/log/*` except list/upload, all `/api/canraw/*`, all `/api/replay/*`). **All `httpd_resp_send_err()` error responses lack CORS entirely** — a browser on another origin sees an opaque network failure instead of the 4xx/5xx body.
- LVGL-lock contention produces either **503 busy** (+`Retry-After: 1`, `{"ok":false,"error":"busy"}`) or **500 "LVGL busy"** depending on the endpoint (the 500 variant: `POST /api/dimmer/config`, `GET /api/layout/current`). Lock timeouts range 100 ms–2 s per endpoint.
- Oversize bodies: layouts → **413** `{"ok":false,"error":"layout_too_large","max":32768,"actual":N}`; generic → 413 `{"ok":false,"error":"body_too_large"}`.
- Name/path validation: layout/asset/channel names reject `/`, `\`, any `.`, and control bytes; filenames (logs) allow dots but reject `..`. Names are URL-decoded before checking. **Silent truncation** is pervasive: 31 chars for layout/asset/channel/signal names, 23 for banner test ids, 7 for units.
- No WebSockets, no SSE. The only push transport is the MJPEG stream.

### 3.1 Root & system

| Endpoint | Behaviour |
|---|---|
| `GET /` | The embedded web editor. Always served gzipped (`Content-Encoding: gzip`, no Accept-Encoding negotiation), no-cache. |
| `GET /favicon.ico` | Embedded ICO, cached 24 h. |
| `GET /api/device/info` | Big read-only snapshot: `serial`, `schema` (=17), `display{width,height,shape}`, `hardware{chip,cores,psram_mb,flash_mb}`, `system{uptime_s,heap_free,heap_min_free,psram_free,logger_active,replay_active}`, `can{state,rx_pending,tx_errors,rx_errors,bus_errors,rx_missed}` (or `{state:"unavailable"}`), `wifi{state,ssid,sta_ip,ap_enabled,ap_ssid,ap_ip}`, `sd{mounted,total?,used?,free?}`, `signals{total,fresh,stale}`. **Deliberately omits the firmware version** — get it from `/api/ota/status → current_version`. Takes no LVGL lock. |
| `GET /api/system/health` | `{uptime_s, heap_free, heap_min_free, psram_free, wifi_rssi}` (rssi 0 when not STA). Cheap poll target. |
| `POST /api/system/reboot` | Replies `{"status":"rebooting"}` then reboots 500 ms later. No body, no confirmation. |
| `GET/POST /api/brightness` | `{"brightness":1..100}`. POST clamps server-side; not persisted here. |
| `GET/POST /api/can/config` | `{"bitrate":0..3}` (0=125k…3=1M). POST writes NVS only — **takes effect on next boot, does not re-init the driver.** |
| `GET/POST /api/dimmer/config` | Signal-driven brightness dimming: `{signal_name, threshold, is_momentary, invert, dim_brightness, enabled}`. POST is a partial patch; **the three bools use `cJSON_IsTrue`, so any non-`true` value (including `"true"` as a string) sets them false.** |

### 3.2 Screen capture / remote view

| Endpoint | Behaviour |
|---|---|
| `GET /api/screenshot` (alias `GET /screenshot`) | JPEG (or raw RGB565 with `?raw=1`). Query: `q=1..100` (default **100**), `full=0/1` (1=native 800×480, 0=downsampled 400×240), `smooth=0/1` (2×2 box filter). ETag conditional GET (304) and a PSRAM dedup cache keyed on frame seq+params. **`?q=0` etc. are indistinguishable from absent** (`v ? v : fallback`). If JPEG isn't compiled in, silently falls back to raw RGB565 `application/octet-stream`. Concurrent-encode → 503 with plaintext body `busy`. Capture reads the panel framebuffer directly; tearing is possible and accepted. ~100–200 ms per JPEG. |
| `GET /api/capture/stream` | MJPEG `multipart/x-mixed-replace`, `?fps=1..30` (default 5), same q/full/smooth. Frame-skips on unchanged content, 1 s keepalive resend. **Blocks the single httpd task for the connection's lifetime** — every other endpoint queues behind an open stream. |
| `GET /api/screenshot/hash?x&y&w&h` | FNV-1a/32 over a shadow-framebuffer rect → `{hash:"0x…", seq, x,y,w,h, torn}`; `torn:true` = frame changed mid-hash, retry. Overflow-safe clamping. |
| `GET/POST /api/touch` | Remote touch. POST body (≤256 B): `enabled:bool` and/or a pointer event `{x, y, state:"down"|"move"|"up"}` — **all three required or the event is silently dropped with a 200**; unknown `state` = release. Returns `{"enabled":bool}` (current state, even if the event was ignored). The virtual indev is created lazily on first POST; a 350 ms idle watchdog auto-releases stuck presses (kept below LVGL's 400 ms long-press threshold); physical touch always force-releases the virtual press. |

### 3.3 Widget live-edit & test endpoints

All are runtime-only (nothing persists) and fire-and-forget where async.

| Endpoint | Behaviour |
|---|---|
| `POST /api/widget/transform` | `{id, x?, y?, w?, h?}` — editor fast path, synchronous under the LVGL lock (503 busy on 1 s timeout). Response `{"ok":true,"found":bool}` — **`ok` is always true; check `found`.** |
| `POST /api/widget/set` | `{id, field, value}` — single-field live set, typed via the field schema (color→number masked 0xFFFFFF, checkbox→bool/number, text-ish→string, else int). Response `{"ok":true,"found":bool,"handled":bool}`; `handled:false` tells the editor to fall back to a full preview push. **`ok` always true.** |
| `POST /api/indicator/test` | `{slot:0|1, active}` — previews both lamps regardless of input_source. 200 even if no indicator exists. |
| `POST /api/warning/test` | `{slot:0..7, active}`. |
| `POST /api/banner/test` | `{id, active}` — keyed by widget id; **no existence check** (typo → 200, nothing happens). Reset on every layout rebuild. |
| `POST /api/screen/switch?screen=splash\|dashboard` | POST with a **query param**, matched by prefix (`splashfoo` works). Switches edit-mode screen. |
| `GET /api/widgets` | Full live widget dump under the LVGL lock: `{widgets:[{id,type,slot,x,y,w,h,hidden?,fields?{...},rules?[{signal,op,threshold,active}],rule_mask?}],count}`. Field typing mirrors the inspector; NULL string fields are omitted. Registry cap 64 widgets. |

### 3.4 Layouts, splash, presets, ECU

Key constants: layouts live at `/lfs/layouts/<name>.json`, cap **32768 bytes**, max 48 layouts, names ≤31 chars, schema v17.

| Endpoint | Behaviour |
|---|---|
| `GET /api/layout/version` | `{"v":u32}` — monotonic counter bumped on every save **and** load; cheap editor poll. No CORS. |
| `GET /api/layout/current` | Serializes the **live widget tree** (not the file); splash-aware (name becomes `_splash_<active>` in splash edit mode). 500 "LVGL busy" on lock timeout. |
| `GET /api/layout/raw?name=` | Raw file bytes. URL-decoded then safety-checked. 404 if absent. |
| `POST /api/layout/save[?apply=0]` | The main save path. Pipeline: parse-before-write (boot-loop prevention) → requires `name` + `widgets[]` → `schema_version` injected/overwritten if missing → splash detection by `_splash_` prefix → **ECU preset auto-apply** when `ecu`+`ecu_version` are set and `signals[]` is absent/empty (re-applies on *every* such save — deliberate) → **Studio "full config" import** when `signals[]` entries carry thresholds/decode (adopts them into channels.json and strips the threshold keys from the layout; **silently skipped with only a log if the LVGL lock times out — a real partial-apply with a 200**) → atomic write (`.tmp` + fsync + rename, `.bak` kept) → preset apply rewrites the file a second time if triggered (its failure is log-only: 200 with a layout that has no bindings) → if applying: set active + debounced (600 ms) screen rebuild. Factory-name protection is **client-side only** — curl can overwrite `default`. |
| `POST /api/layout/preview` | Applies JSON to the live screen **without persisting** (lost on reboot). Meter static-tick baking is suppressed during preview rebuilds (WDT protection). Response `{"status":"ok"}` without Content-Type/CORS. |
| `GET /api/layout/list[?details=1]` | `{active, layouts:[names]}` or `[{name,size}]`. Underscore-prefixed (system/splash) layouts hidden. `active` falls back to `"default"` on NVS failure. |
| `POST /api/layout/set` | `{name}` — sets active + debounced reload. **No existence check** (a bad name falls back at load time). |
| `POST /api/layout/delete` | `{name}`; `default` refused; deleting the active layout silently switches active to `default`. |
| `POST /api/layout/rename` | `{old_name,new_name}`. Rewrites the internal `name` field; verified write-then-delete. Quirks: unparseable file → bare `rename()` leaving the internal name stale; **`fopen` failure on the destination → nothing happens but `{"status":"ok"}` is still returned**; `old==new` early-OK lacks CORS. |
| `GET/POST /api/layout/switcher` | On-dash layout cycle list as `{"csv":"a,b,c"}` (≤319 chars). POSTing a missing/non-string `csv` **erases the NVS key** (reverts to filesystem-order cycling). |
| `GET /api/presets` | Flat array of the ~180-row built-in preconfig catalog: `{ecu, version, label, can_id(hex string), endianess, bit_start, bit_length, scale, offset, decimals, is_signed}`. |
| `GET /api/ecu/list` | `{presets:[{make,version,display,match_score:0..100}], match_threshold:30, auto_mode}` — match_score is the overlap between preset broadcast IDs and the last bus scan; 0 means "no overlap **or** never scanned". |
| `GET/POST /api/ecu/picker_mode` | `{"auto":bool}` — auto vs manual ECU picker. |
| `GET /api/ecu/current`, `POST /api/ecu/set` | `{make, version}`. Set with both empty clears; **one empty also clears** (asymmetry); unknown → 404; success rewrites the active layout's `signals[]` on disk + reload. |
| `GET /api/presets/custom` | Custom (user/DBC) presets from `/lfs/presets/*.json` (≤64 KB each), returned as a **flattened per-signal row array** (each row carries its parent ecu/version); empty presets emit a `{_empty:true}` placeholder row. Corrupt/oversize files silently skipped. `Cache-Control: no-store`. |
| `POST /api/presets/custom/save` | Raw JSON ≤64 KB, requires `ecu`, `version`, `signals[]`. Write is fsync'd, size-checked, **read back and re-parsed** before 200 (disk-full protection). Filename is a lossy sanitization of ecu+version (`"Link G4+"`/`"Link G4-"` collide). |
| `POST /api/presets/custom/delete?ecu=&version=` | POST with query params. |
| `GET /api/splash/list` | `{active, fade_enabled, enabled, boot_anim, boot_anim_style:"fade"\|"curtain", splashes:[…]}`. |
| `POST /api/splash/set` / `delete` / `fade` / `enabled` / `bootanim` | Set active splash (no existence check) / delete (active falls back to `Default`) / `{enabled:bool}` toggles (strict bool) / bootanim `{enabled(req), style?, preview?}` — any style string other than `"curtain"` silently means `fade`; `preview:true`+`enabled:true` plays the reveal animation live. |

### 3.5 Assets (images, fonts, storage, SD)

Image format: **RDMIMG** — 12-byte header (`"RDMI"`, u16 width, u16 height, u8 color-format=5 [true-color+alpha], 3 reserved) + RGB565+alpha pixel data. Files at `/lfs/images/<name>.rdmimg`. `tools/png_to_rdmimg.py` converts. Fonts: TTF at `/lfs/fonts/<Family>.ttf`, family name = filename.

| Endpoint | Behaviour |
|---|---|
| `POST /api/image/upload?name=` | Raw RDMIMG body, 12 B–1200 KB, magic validated mid-stream, free-space pre-checked, **streamed** in 8 KB chunks (no big buffer), atomic tmp/bak/rename publish. Built-in image (`RDM`) protected → 403. |
| `GET /api/image/list` | `[{name (ext stripped), width, height, size}]` — dims parsed from the header; invalid files silently skipped. |
| `POST /api/image/delete?name=` | POST with query param; protected → 403. |
| `GET /api/image/data?name=` | Chunked download; a mid-stream failure aborts without the terminating chunk (client sees truncation, not silent short data). |
| `POST /api/font/upload?name=` | Raw TTF ≤ **512 KB** (same cap as the loader — "subset the TTF first"), whole-buffered in PSRAM (cannot stream: the font manager needs contiguous data), atomic publish, then family-add + full screen rebuild happen **in one LVGL callback** (UAF protection for live labels). **200 is returned before the apply runs** — an add failure is log-only. |
| `GET /api/font/list[?details=1]` | `["Family",…]` or `[{name,size}]`. |
| `POST /api/font/delete?name=` | Removes the family + rebuilds screen — but **never deletes the .ttf file**; the family returns on next boot. Protected families (Montserrat, Fugaz One, Manrope Bold) → 403. |
| `GET /api/font/data?name=` | Chunked TTF download. |
| `GET /api/storage/info` | `{total, used, free, sd:{mounted, total?, used?, free?}}` (LittleFS + SD). |
| `GET /api/sd/status` | `{mounted, total?, used?, free?}`. |
| `GET /api/sd/files` | `{layouts:[…], images:[{name,width,height,size}], fonts:[…]}` from `/sdcard/{layouts,images,fonts}`. |
| `POST /api/sd/copy` | `{type:"layout"\|"image"\|"font", name, direction:"to_sd"\|"from_sd"}`. Protected assets blocked both directions (403). **Copying from SD does not register/reload the asset** — a copied-in font/image/layout is invisible until reboot. |
| `POST /api/sd/delete` | `{type, name}` — no protected-asset check (SD copies only). |

### 3.6 WiFi

| Endpoint | Behaviour |
|---|---|
| `GET /api/wifi/config` | `{ssid, has_password, auto_connect, wifi_on_boot}` — the password itself is never returned (unauthenticated API). |
| `POST /api/wifi/config` | Partial patch of the same fields + `password`. Empty-string password = keep existing. `auto_connect`/`wifi_on_boot` presence with any non-`true` value sets them false. **Does not reconnect/restart WiFi.** No HTTP scan endpoint exists (scanning is on-device UI only). |

### 3.7 OTA

Pull-only from the internet — there is **no firmware-upload HTTP endpoint** and no abort/rollback endpoint (rollback state is read-only via `/api/selftest → ota`).

| Endpoint | Behaviour |
|---|---|
| `GET /api/ota/status` | `{status: idle\|checking\|no_update\|available\|installing\|completed\|failed\|unknown, current_version, latest_version, release_notes, file_size_mb, progress (-1 failed / 100 completed / 0..100), update_available}`. Safe to poll. |
| `POST /api/ota/check` | 202 `{"status":"checking"}` (idempotent while checking). Background task queries the GitHub Releases API (`Trizzoto/potato-jubilee` latest), TLS via cert bundle, 30/45 s timeout by RSSI, 2–3 attempts; 403 = GitHub rate limit (60/hr unauthenticated). |
| `POST /api/ota/start` | Requires status `available` (else 409). 202 `{"status":"installing"}` — **also 202 when the internal busy-flag refuses the start** (looks like a success that never progresses; poll status). Download alternates proxy→GitHub→proxy across 3 attempts: even attempts use the Cloudflare Worker `https://rdm7-ota-proxy.rdm7-ota-proxy.workers.dev/<version>/esp32-firmware.bin` (single origin, single TLS, no redirect — the GitHub 302-to-CDN second TLS handshake was OOMing ~1% of dashes), odd attempts use GitHub's direct asset URL. AP is dropped during install to free RAM; CAN task priority reduced; success reboots after 3 s. |
| `POST /api/ota/upload` | **Local firmware push (2026-08).** Body = the raw `build/esp32-firmware.bin`, streamed 4 KB at a time straight into the inactive OTA slot; `esp_ota_end` validates, boot partition is set, and the dash reboots ~0.7 s after the JSON reply. Exists because `/api/ota/start` can ONLY install a *published GitHub release* and the only other `esp_ota_write` path is the USB-only `upload.*` serial RPC — so a cable-less dash previously had no way to receive a local build. Requires header `X-RDM-Device: <this board's serial>` (from `/api/device/info`): that is **not authentication** (the serial is public on the LAN), only a guard against a drive-by/CSRF flashing the wrong board — real OTA auth is still the open item from the 2026-07-05 hardening sweep. Rejects a non-`0xE9` first byte (catches the merged flash image), 409s against an in-flight download-OTA, and aborts cleanly on a dropped socket. Driver: `python tools/rdm_ota_push.py <ip>` (`--scan 192.168.4` to find the dash). **Hardware-verified 2026-08-08: 3,119,712 bytes in 77 s (~39 KB/s) over WiFi.** |

Auto-check: armed on first got-IP, fires 15 s later; honours a per-version "skip" (NVS `ota_cfg/skip_ver`); shows a dialog on the dash if an update exists. Rollback: dual-OTA + `BOOTLOADER_APP_ROLLBACK_ENABLE`; image marked valid only after 10 s of healthy rendering.

The Cloudflare worker also hosts the CAN-trace upload (below) and a `?resolve=1` legacy JSON endpoint; it caches release assets at the edge (24 h).

### 3.8 Data logger, raw CAN capture, replay, cloud upload

`/api/log/*` = the decoded-**signal** CSV logger; `/api/canraw/*` = the raw-**frame** logger. Both write to `/sdcard/logs` (preferred) or `/lfs/logs` (1 MB per-file cap), and **share the directory** — `/api/log/list|download|delete` also see raw captures (distinguish by filename: `log_*.csv` vs `canraw_*.csv`). Start/stop are deferred to the LVGL task (fire-and-forget: the 200 reflects the request, not the outcome — poll status and check `stop_reason`).

| Endpoint | Behaviour |
|---|---|
| `POST /api/log/start` | Optional body `{rate_hz:0..1000 (0="Max"), persist:bool (default true → saves rate to NVS)}`. 400 if already logging. CSV: header `timestamp_ms,<SIG1>,<SIG2>,…` (snapshot of all CAN-decoded signals at start, ≤64 columns), rows `%.4f`, stale cells empty. Flush every 100 samples / 2 s with fsync. 3 consecutive write failures auto-stops (`stop_reason:"write_failure"`). |
| `POST /api/log/stop` | 400 if not logging. |
| `GET /api/log/status` | `{active, file (basename), samples, elapsed_ms, rate_hz, storage:"sd"\|"lfs", lfs_max_bytes, sd_mounted, stop_reason}`. |
| `GET/POST /api/log/config` | `{rate_hz, is_max}`; POST applies live mid-log. |
| `GET /api/log/list` | `[{name,size,storage}]` across both tiers. |
| `GET /api/log/download?name=` | Streams `text/csv` with attachment disposition; happily streams the actively-written file. |
| `POST /api/log/delete?name=` | Refuses the signal logger's open file (400 "in use") — **but not the raw logger's open file** (deletable out from under it). |
| `POST /api/log/upload?name=` | Raw CSV → `/lfs/logs` only (never SD), `.csv` auto-appended, cap = the LFS byte cap enforced on declared **and** actual size. Uploaded files are immediately replayable. |
| `POST /api/canraw/start` | Raw frame capture in **SavvyCAN GVRET-CSV** format (`Time Stamp,ID,Extended,Bus,LEN,D1..D8`, µs timestamps, `0x` hex IDs). Refused while the signal logger runs (flash wear/CPU). |
| `POST /api/canraw/stop`, `GET /api/canraw/status` | Status mirrors log/status with `frames` instead of `samples`. |
| `POST /api/canraw/cloud_upload` | `{file, make, model, notes?}` → background HTTPS upload of the trace to the shared R2 bucket (`rdm7-can-logs`) via the Cloudflare worker. Waits ≤10 s for SNTP (server enforces a ±10 min HMAC timestamp window). Auth: HMAC-SHA256 over `"{make}\n{model}\n{device_id}\n{unix_ts}"` with the shared secret in `can_upload_secret.h`; headers `X-Make/X-Model/X-Device-Id/X-Timestamp/X-Signature/X-Notes`. CR/LF rejected in the metadata (header injection). 10 MB max. R2 key `{make_slug}/{model_slug}/{device_id}_{ts}.csv`. |
| `GET /api/canraw/cloud_upload/status` | `{state: idle\|running\|success\|failed, http_status, uploaded_bytes, message}`. |
| `POST /api/replay/start` | `{file, speed? (default 1.0, **not validated** — 0/negative passes through), loop?}`. Basename resolved SD-then-LFS; absolute paths must be under `/sdcard/` or `/lfs/`. Replay streams the CSV at 50 Hz virtual-time pacing (sparse and dense logs both play correctly); **replay updates peaks** (unlike the simulator). |
| `POST /api/replay/stop` | Always `{"status":"stopped"}` even if idle. |
| `GET /api/replay/status` | `{active, file, row, total_rows (0 = unknown for >256 KB files), speed}`. |

### 3.9 Signals, simulator, fuel calibration

| Endpoint | Behaviour |
|---|---|
| `GET /api/signals/values` | `{signals:[{name, value, stale, seen, can_id, source:"can"\|"obd2"\|"internal"}]}`. `seen` distinguishes never-received from received-then-stale (mirrors must not paint `--` widgets as `0.0`). **Takes no LVGL lock** — torn reads accepted for poll-friendliness. No CORS. |
| `GET/POST /api/signal/simulate` | The on-device signal simulator. POST `{enabled:bool}` (`cJSON_IsTrue` — absent = disable); response echoes the **requested** state, not the achieved one. GET returns live state. No CORS. |
| `POST /api/signal/inject` | `{signal, value}` and/or `{values:[{signal,value},…]}` (both shapes read from one body; hard cap 16 entries, silently truncated). Injected values are **pinned against live CAN** (test locks) until cleared. Response `{injected:[…], unknown:[…], ok}` (ok = ≥1 resolved) — but the actual injection is async and no-ops unknown names regardless. On LVGL-lock timeout the report optimistically claims everything injected. |
| `POST /api/signal/clear` | `{all:true}` or `{signal:name}` — releases test locks. |
| `POST /api/signal/update` | Live decode upsert: `{name (req), can_id≤0x1FFFFFFF, bit_start 0..63, bit_length 1..64 (start+len≤64), scale, offset, is_signed, endian\|is_little_endian, unit}`. Patches an existing registry entry or registers a new CAN signal. **Runtime-only — persistence rides on the next layout/channel save.** |
| `GET /api/fuel/status` | `{voltage, cal:{empty_v, full_v, full_value, enabled, points?:[{v,val}]}}` — `points` present only in multi-point mode (≥2). |
| `POST /api/fuel/set-empty` / `set-full` | No body; samples the current ADC voltage as the empty/full reference. Persists immediately. |
| `POST /api/fuel/set-points` | `{enabled? (default true), points:[{v,val}×2..8]}` — extras dropped, malformed entries skipped; response `count` is the stored truth. |

### 3.10 Calculated gear & odometer

| Endpoint | Behaviour |
|---|---|
| `GET/POST /api/gear/config` | Config for the `CALCULATED_GEAR` synthetic signal: `{wheel_circumference_m, final_drive, rpm_signal, speed_signal, enabled, ratios:[…≤GEAR_CAL_MAX_GEARS]}`. POST is a partial patch starting from the current config; persists to NVS (`gear_cal/cfg` blob). Body reader retries socket timeouts forever (slow-client hazard). |
| `GET /api/odometer` | `{km, speed_signal}` — speed source defaults to `VEHICLE_SPEED`; shares config with the gear page. |
| `POST /api/odometer` | `{"km": number}` — manual override; negative/non-finite clamped to 0; persists to NVS immediately (`vehicle/odo_km`). |

### 3.11 OBD2 diagnostics

All handlers bridge the httpd task to the LVGL task with semaphores + refcounting; each sets CORS. Every request goes to the broadcast/physical address per the locked-in scan result.

| Endpoint | Behaviour |
|---|---|
| `GET /api/obd2/dtcs?mode=stored\|pending\|permanent` | Mode 03/07/0A. Unknown mode silently = stored. `{mode, mode_name, ok, error?, count, codes:[{code,desc}]}` — descriptions from the built-in generic-P-code DB, `""` if unknown; ≤16 codes/request. Multi-ECU merged (see §5 OBD2). Waits ≤3 s. |
| `POST /api/obd2/clear` | Mode 04. `{ok}` or `{ok:false,error:"ECU rejected clear (try with engine off + ignition on)"}`. Clears stored+pending, never permanent. |
| `GET /api/obd2/vin` | `{ok, vin}` (Mode 09/02, ISO-TP multi-frame). |
| `GET /api/obd2/ecuname` | `{ok, ecu_name}` (Mode 09/0A). |
| `POST /api/obd2/snapshot` | Chains stored→pending→permanent DTCs→VIN→ECU-name, writes `/sdcard/diagnostics/snap_<ts or uptime>.json` (SD required) containing DTCs + a live signal snapshot. HTTP `{ok, path, bytes, summary{stored,pending,permanent,vin,ecu_name}}`. **There is no endpoint to download the snapshot file** — it is write-only from the API (the in-code pointer to `/api/sd/file` refers to an endpoint that does not exist). Waits ≤12 s, blocking all other HTTP. |
| `GET /api/obd2/protocols` | Per-service liveness (no bus traffic): `{fresh_window_ms:5000, protocols:[{service, name:"M01".."M22", fresh, age_ms\|null}]}` for services 01/02/03/07/09/0A/21/22. |
| `GET /api/obd2/pids` | `{pids:[{service,pid}]}` — currently-enabled polled PIDs (≤48). Lock-gated (503 busy). |
| `GET/POST /api/obd2/sim?on=0\|1` | The **virtual ECU** bench simulator: firmware answers its own OBD2 requests with synthetic sweeping values through the real decode path. `{ok, sim, pids_polled, signal_sim_active}` — `signal_sim_active:true` is a warning: the signal simulator drains injected frames, so it must be off for the virtual ECU to work. Not persisted; default off. |
| `POST /api/obd2/scan` | Runs OBD2 discovery/auto-search (Mode 01 PID 0x00 bitmask walk across 500k/250k and 11/29-bit addressing). `{ok, completed, count, pids:[…≤128]}` — **`ok:true, completed:false` = returned but unfinished**; `count` can exceed `pids.length` (array capped). Waits ≤8 s. |
| `POST /api/obd2/test_pid` | One-shot custom-PID test (powers the "Test" button): `{service (def 0x01), pid (u16 — Mode 22), request_id, data_offset, data_bytes, scale, offset, is_signed}` → `{ok, decoded, elapsed_ms, raw:[bytes]}` or `{ok:false,error}`. Registered in the signals file, not the obd2 file. Leaks a small bounded ctx on internal timeout (older pattern). |

### 3.12 Channels

See §5 for the channel model. `CHM_MAX = 128` channels; ~135 canonical definitions.

| Endpoint | Behaviour |
|---|---|
| `GET /api/channels` | `{count, channels:[<full channel>]}`. Full channel object: `id, label, tier, group, group_name, cardinality, is_canonical, signal, signal_index, units_native, units_display, decimals, source_units?[], min, max, low_warn?, high_warn?, color_normal?, color_low_warn?, color_high_warn?, decode?{can_id,bit_start,bit_length,scale,offset,is_signed,endian,unit}, math?{a,b,op}, source ("math"\|"obd2"\|"internal"\|"can"\|"unknown"\|"unbound" — from the bound signal's authoritative provenance, not name heuristics), current_value, is_stale, last_update_ms, zone, display_value?/display_min?/…` (display_* only when native≠display units and a conversion exists). Build is serialized behind a dedicated mutex (concurrent builds OOM'd) then the LVGL lock; 503 busy on either timeout. |
| `GET /api/channels/canonical` | The static picker catalog (~135 rows): `{id, label, group, group_name, tier, cardinality, units_native, units_display_default, decimals, min_default, max_default, low_warn?, high_warn?, color_normal, notes?, active}`. |
| `POST /api/channels/create` | Custom channel. Only `label` is required; `group`, `units` (used for both native+display), `decimals (0..3)`, `min`, `max`, optional `decode{}`. The channel **id** (`custom_<name>`) and **signal name** (derived: uppercase alphanumerics, runs of other chars → `_`) are server-derived — the caller never invents them. Duplicate label → 400; 128-cap → 500. |
| `POST /api/channels/activate` | `{id}` — activates a canonical channel (idempotent; unknown id → 404). |
| `POST /api/channels/update` | `{id, fields:{…}}` — partial patch. Recognized keys: `label`, `signal` (string or **null/"" to unbind**), `units_native`, `units_display`, `decimals`, `min`, `max`, `low_warn`/`high_warn` (number sets, null clears), `color_low_warn`/`color_normal`/`color_high_warn` (null = default), `decode{}` (**partial patch** — missing subfields keep current values), `math` (object or null to clear; operands are channel-id strings or numeric constants, `op:0..3`). **Unknown keys and type mismatches are silently ignored**; the applied-count is log-only — diff the echoed channel to detect no-ops. Auto-activates canonical ids. |
| `POST /api/channels/delete` | Custom channels only (canonical → 403 "clear the signal instead"). |
| `GET /api/channels/source-options?id=` | The Make → Version → Signal drilldown behind the source picker (~98 KB response). Three row kinds: `ecu` (preconfig rows with decode + `exists_in_layout`/`live_value`/`is_stale`), `obd2` (virtual make "OBD2", one row per PID with `service`, `pid`, `polled`), `custom` (live registry CAN signals). `is_current` is provenance-gated so a same-named OBD2 row never highlights for a CAN binding. Signals are snapshotted under the lock, then the JSON is built lock-free (~1.9 s UI freeze fixed). |
| `POST /api/channels/bind-source` | Discriminated by `source_type`: `"ecu_preset"` `{channel_id, make, version, label}` (applies the shared preconfig path — updates the runtime registry, persists to layout signals[], sets channel decode, reconfigures the CAN filter); `"obd2"` `{channel_id, obd2_service, obd2_pid, signal_name}` (appends to the layout's `polled_pids`, restarts OBD2 polling — **a save failure returns a 500 that means "bound for this session but won't survive reboot"**); `"custom"` `{channel_id, signal_name}` (binds an existing registry signal; CAN-sourced decodes are copied onto the channel and written into the layout for boot durability). Missing `source_type` is inferred for backward compat. |
| `GET /api/channels/export` | Raw `channels.json` bytes as an attachment. **Returns 500 for the benign "no channels.json yet" state.** |
| `POST /api/channels/import` | Raw channels.json ≤64 KB, validated + atomically written, then `{"status":"ok","reboot":true}` and **the device reboots** ~800 ms later (hot-swap would dangle widget subscriptions). |

### 3.13 Automation / introspection (agent endpoints)

Read-mostly or inject-only; none change persisted config.

| Endpoint | Behaviour |
|---|---|
| `POST /api/can/inject` | Inject frames into the **real RX path** (full decode/dispatch/zone evaluation): `{id (req), extd?, data: "hex string"\|[bytes] (≤8), dlc? (default = data len), count? 1..256}`. Response `{ok, queued, dlc, sim_active}`. **Critical quirk: while the signal simulator is active, injected frames are drained without dispatch — silently dropped, yet `ok:true, queued:N` is still returned.** Disable the sim first. |
| `GET /api/selftest` | One-shot post-flash health check. `{uri_registration{attempts,failures,ok}, littlefs{ok}, can{state (raw enum), bus_errors, rx_errors, rx_frames}, heap{free,min_free,psram_free,psram_largest_free}, lvgl{lock_acquired, lock_ms}, channels{count,decoded}, signals{count,fresh,stale}, fonts{families}, ota{running (partition label), state ("valid" healthy / "pending_verify" awaiting confirm / "undefined" USB-flashed), rollback_pending}, crash{panic_count, prev_was_crash, prev_reason, prev_uptime_s, prev_fw}, ok}`. `ok` = AND of URI registration + LittleFS + LVGL lock only (CAN/heap/OTA/crash do **not** affect it). When the lock probe fails, the `channels`/`signals`/`fonts` blocks are **absent entirely**. `lock_ms` doubles as a responsiveness probe; diffing `crash.panic_count` across an action detects crashes without a serial console. |
| `GET /api/perf` | Last-second LVGL render telemetry: `{seq, fps, frames, avg_px, avg_pct_screen, max_pct_screen, avg_render_ms, flush_per_frame, flush_us_per_frame}` — the supported way to measure render perf. |
| `GET /api/perf/history` | Chunked; the last 180 one-second windows since boot (`{t, fps, ms, max_ms, max_pct, px}`), including the pre-WiFi seconds no poller can observe. |
| `GET /api/perf/bigframe` | Chunked; dirty-rect forensics of the most recent ≥90%-screen frame (`seq, px, fullinv/invovf sequences + 8 PC hex strings each, rects[[x1,y1,x2,y2]…]`). |
| `POST /api/can/scan/start` | Starts the bitrate scan over WiFi. Always HTTP 200; `{started:bool, error?}`. |
| `GET /api/can/scan/status` | `{running, state, current_bitrate_idx, recommended_bitrate, results:[4 × {bitrate:"125k".."1M", frames, unique_ids, traffic, install_failed, install_err (the real per-bitrate esp_err name), bus_errors?, steps{…driver-call trace}}]}`. |
| `GET /api/can/monitor` | Live per-ID tracker (≤64 IDs): `{ids:[{id, ext?, dlc, data (hex), count, age_ms}], capacity:64}`. Lock-free (torn reads accepted); frame rate is computed client-side from `count` deltas. This is the read primitive that makes the dash a **WiFi↔CAN gateway** (no dongle needed for the desktop CAN analyzer). |

### 3.14 Quirk index (things that lie or half-apply)

**Fake/overstated success:** `can/inject` under an active signal sim (drops frames, reports queued); `widget/transform` + `widget/set` (`ok` hardcoded true — check `found`/`handled`); `layout/rename` with an unopenable destination (200, nothing happened); `layout/save` when the channel import was lock-skipped or the preset apply failed (200 + log only); `font/upload` (200 before the async apply); `ota/start` when internally busy (202, never progresses); every fire-and-forget start/stop/test endpoint (response reflects the request); `replay/stop` when idle; `channels/update` with zero applied fields; `channels/bind-source` obd2 path's 500-that-means-partial-success.

**Silent drops/truncation:** touch events missing x/y/state; >16 inject entries; >8 fuel points; unknown `channels/update` keys; bad `dtcs?mode`; bad bootanim style→fade; unreadable list entries everywhere; 31-char name truncation everywhere.

**Misleading names:** `device/info` omits the firmware version; `log/*` covers raw captures too; `screen/switch`, `log/delete`, `log/upload`, `image/delete`, `font/delete`, `presets/custom/delete`, `obd2/sim` are POST-with-query-params; `layout/preview` and `signal/update` don't persist; `can/config` doesn't re-init; `font/delete` doesn't delete the file; `sd/copy` doesn't load the copy; `channels/export` 500s when empty.

**Slow-client hazards:** the gear-config and `can/inject` body readers retry socket timeouts forever (a stalled client wedges the single httpd task); the touch-file readers treat a slow body as a fatal 500; the shared readers bound retries at 3.
---

## 4. Layout JSON

Single source of truth for widget field metadata: **`schema/widgets.schema.json`** (codegen regenerates `WIDGET_DEFS` in the web editor and `widget_fields.gen.c` in firmware; never hand-edit either — CI enforces).

### 4.1 Top-level shape

```jsonc
{
  "schema_version": 17,      // REQUIRED on disk load (>=1); injected by /api/layout/save if missing
  "name": "my_layout",       // REQUIRED on save; filename is the identity at load time; ≤31 chars
  "screen_w": 800,           // informational, written by firmware
  "screen_h": 480,
  "ecu": "",                 // optional; mirrored to NVS; with ecu_version + empty signals[] triggers preset auto-apply on save
  "ecu_version": "",
  "night_mode": { "signal_name": "HEADLIGHTS", "active_when": 1.0 },  // optional trigger; absent = manual only
  "signals": [ /* portable display metadata only — see 4.2 */ ],
  "polled_pids": [ 261 ],    // optional; OBD2 encoded (service<<8)|pid
  "custom_pids": [ /* user OBD2 PID defs — see §5 OBD2 */ ],
  "widgets": [ /* drawn in array order */ ]
}
```

There is **no root background field** — a background is a full-screen `shape_panel` or `image` widget. The only special handling: a full-screen image widget is auto-sliced into meter faces at load (`_autoslice_from_bg`).

Version history (all bumps were additive; `_migrate_layout_root` is currently an empty dispatch that just rewrites `schema_version`): v13 night overrides; v14 `config.channel` binding; v15 pathbar `smooth`; v16 button/toggle CAN-TX rework (`tx_send_release` dropped/ignored, `remember_state` added); v17 pathbar `label_side`/`label_along_offset` + label-offset clamp. A layout with `schema_version` **greater** than current loads with a warning; `< 1` is rejected.

### 4.2 `signals[]` — what it is now (post ADR-0005/0006)

**CAN decode does not live in layouts anymore.** It lives in the device-local channel registry (`/lfs/channels.json`). Layout `signals[]` survives as:

1. **Portable display metadata**: `value_map` entries (e.g. gear number→"N"/"1"… labels, ≤32 entries, labels ≤11 chars) and the `FUEL_SENDER_V` `fuel_cal` block (2-point `{empty_v, full_v, full_value, enabled}` or multi-point `{points:[{v,val}×2..8], enabled}` — points wins). On save, **only** these are emitted; pure-decode entries are dropped and the `signals` key is omitted entirely when nothing portable remains.
2. **Legacy/import decode**: old layouts and Studio/marketplace exports may still carry full decode (`can_id, bit_start, bit_length, scale (1.0), offset (0.0), is_signed (false), endian (1=little), unit, source:"internal"|"obd2"|absent→CAN, decimals`). At load these are registered into the signal registry (UPSERT — an existing name gets its decode updated in place, subscribers/value_map/peaks preserved, latest layout wins) and a one-time migration seeds channel decodes from them. On `/api/layout/save`, entries carrying thresholds or decode are the *signature of a Studio import*: thresholds are adopted into channels **and stripped from the layout**; decode is adopted only when the channel has none (never clobbers a device-edited decode).
3. Admission gate at load: an entry is skipped unless it is internal, has full decode, has a `value_map`, or is `FUEL_SENDER_V`.

The registry **merges across layout loads** (deliberately never reset except by an explicit ECU-preset switch) so channel bindings keep resolving by name across ECU/layout changes. Additionally, every widget-referenced `signal_name` not in the registry is pre-registered as a decode-less internal placeholder at load — this is why a fresh out-of-box dash still animates under the simulator, and why configuring a channel later is transparent (the real decode UPSERTs into the same slot).

### 4.3 Serialization rules and budget

- **Defaults-only:** every `to_json` emits a config key only when it differs from the factory default (editor and firmware defaults must agree or a save silently changes meaning — see §4.8 discrepancies). A handful of load-bearing fields are always emitted (slots, labels, ranges, key colours).
- **Thresholds live on the channel, not the layout**: panel/bar/arc warn thresholds and enables are not persisted; alert *colours* and apply-to flags are widget styling and do round-trip.
- Budget: **32768 bytes/file** (`RDM_LAYOUT_MAX_BYTES`). Files larger than the cap are rejected at save and **silently truncated at load** (then fail parsing). Related caps: 48 layouts, 64 widgets tracked in the registry, names ≤31 chars.
- Atomic save (`.tmp` + fsync + rename, `.bak` retained). Load falls back: corrupt primary → renamed `.corrupt` → `.bak` retried → not-found.

### 4.4 Widget envelope & coordinates

```jsonc
{ "type": "meter", "id": "coolant",        // id ≤15 chars
  "x": -140, "y": -62,                     // CENTRE of the widget, CENTRE-ORIGIN screen coords, px, int16
  "w": 300, "h": 300,                      // px, uint16 (not clamped at load; constraints are editor-side)
  "group": "g_a1b2c3d4",                   // optional editor grouping metadata, preserved on round-trip
  "config": { /* type-specific */ } }
```

- **(0,0) = screen centre**; +x right, +y down; visible centre range x ±400, y ±240 on 800×480.
- **Exception:** `pathbar.path` points are **absolute screen pixels** (not centre-origin). All other coordinate-ish config fields (`label_x/y`, `*_y_offset`, `needle_pivot_*`, `shadow_ofs_*`) are offsets from the widget's own centre.
- Angles: the schema/editor field `start_angle_user` uses **0° = 12 o'clock, CW**; the firmware fields `start_angle`/`end_angle` use LVGL coords (**0° = 3 o'clock, CW**); `lvgl = (user + 270) % 360`. Editor syncs derived fields on every change; firmware defaults `start_angle=135, end_angle=45` ≡ user 225°/sweep 270°.

**Colours:** on-device layout JSON stores **RGB565 decimal integers** everywhere a colour appears (config fields, `night` values, `grad_stops[].color`, rule override values). The web editor works in RGB888 and converts on load/save by walking the schema metadata. Hand-written layouts may use `"#RRGGBB"` strings (unambiguous, recommended for generated layouts) — a bare integer ≤0xFFFF is interpreted as RGB565, >0xFFFF as RGB888. RGB565 is lossy; don't round-trip conversions. Handy values: white 65535, red 63488, green 2016, blue 31, yellow 65504, cyan 2047, orange 64512, magenta 63519.

**Fonts:** every font field is a string. `""` → theme default. New format **`"Family:size"`** (size 8–500; family from `/lfs/fonts/*.ttf` via `lv_tiny_ttf`) — if the family isn't loaded or size is out of range the widget silently falls back to the theme font (**no** fallthrough to legacy names). Legacy compiled names still resolve: `montserrat_8..24` (even sizes), `fugaz_14/17/28/56`, `manrope_35_bold/54_bold`. Font cache: 8 families / 32 size-instances / 512 KB max TTF; instances reset between layout loads.

### 4.5 Universal config keys, slots, constraints

| key | notes |
|---|---|
| `signal_name` | The binding key (**not** `signal`) — the uppercase registry signal name (`RPM`, `COOLANT_TEMP`), not a channel id. Applies to all data widgets. |
| `channel` | v14 channel-id binding. When set, data semantics (signal, min/max, thresholds, decimals, unit) come from the channel; visual style stays widget-owned. |
| `slot` | See table below. |
| `rules` | Conditional rules (§4.7) — all types except pathbar and anim. |
| `night` | Night overrides (§4.7) — see per-type support. |

Slot semantics:

| type | cap | behaviour |
|---|---|---|
| `indicator` | **2** (0=Left, 1=Right) | Slot folded `&1`; the two lamps are global singletons; a third never renders correctly. Slot is read-only in the inspector. |
| `warning` | **8** (0–7) | Slot ≥8 collapses to 0; editor blocks a 9th. |
| `rpm_bar` | **1** (singleton) | `config.slot` ignored. |
| `panel` | unlimited | Slots 0–7 get auto-positions and legacy global mirrors; **slot ≥8 lands at (0,0)** for manual placement; rules label/value lookup covers slots <13. |
| `bar` | editor-unlimited, firmware masks `&1` for label/default-x/id | A `slot:5` bar takes slot-1 cosmetics but stores raw 5 — avoid slots >1. |
| `text`, `meter` | slot = value-slot index (<13), not a position | Historic panel0–7/RPM/BAR1/BAR2 mapping. |
| `toggle`, `button` | accepted, unused | |
| others | not slotted | arc, image, shape_panel, line, banner, shift_light, pathbar, anim. |

Size constraints (`widget_constraints[]`, advisory/editor-enforced): panel 80×40–250×130 (default 155×92); rpm_bar 300×30–800×80 (800×55 @ y −213); bar 120×15–450×50 (300×30); indicator 30–80 sq (50); warning 18–60 sq (20); text 40×20–400×100; meter 80–800 square (300); image/shape_panel/pathbar/anim 10 or 20 min → full screen; arc 30–800 (200); toggle 40×20–200×80; button 40×20–300×100; shift_light 100×15–800×60 (400×30); line 2×2–800×480 (schema says 10×1 — mismatch); banner 100×24–800×160 (800×80).

### 4.6 Widget catalogue (17 types)

`widget_type_t` order: panel(0), rpm_bar, bar, indicator, warning, text, meter, image, shape_panel, arc, toggle, button, shift_light, line, banner, pathbar, anim(16). `WIDGET_TYPE_COUNT` is the authority. Legend: **N** = night-overridable; **§** = firmware-accepted but absent from the schema (device-only/legacy); **E** = schema/editor-only (firmware never reads it).

#### panel — bordered box: header label + big value (+ unit / peak line). Alert-capable.
`label` ("" — factory seeds "Panel N"; always emitted) · `decimals` (0; omitted when == channel decimals) · `label_font`/`value_font`/`peak_font` ("") · `label_color`/`value_color` **N** (#FFFFFF) · `text_align` (1; 0/1/2 L/C/R) · `label_y_offset` (−28) · `value_y_offset` (9) · `show_peak` (0; 0 Off/1 Max/2 Min/3 Min+Max — session peaks, reset each boot) · `peak_x_offset` (0) · `peak_y_offset` (31; reconstructed as value_y_offset+22 when absent) · `show_unit` (false) · `unit_size` (2; 0 Small/1 Medium/2 Full=inline) · `custom_text` ("", replaces unit; always emitted) · `custom_text_x_offset` (41) / `custom_text_y_offset` (32) · `bg_color` **N** (#000000) · `bg_opa` (255) · `border_color` **N** (#2E2F2E) · `border_width` (3, 0–20) · `border_radius` (7, 0–100) · alerts: `warning_low/high_enabled` + `_threshold` (**not persisted** — channel-owned), `warning_low_color` (#0000FF) / `warning_high_color` (#FF0000) + `_apply_label` (true) / `_apply_value` (true) / `_apply_panel` (false) — colour+apply group persisted together.

#### rpm_bar — singleton full-width tach strip. Always-emitted: rpm_max, redline, bar_color, limiter_*, flash_speed.
`rpm_max` (7000) · `redline` (6000) · `limiter_value` (7000) · `smoothing_ms` (20, 0–300) · `bar_color` **N** (#00FF00) · `grad_stops` (§4.6x; suppressed during redline/limiter) · `fill_dir` (0; 0 L→R/1 R→L/2 Center-out/3 Edges-in — modes 2/3 render two mirrored half-bars) · `bar_bg_color` **N** (#F0F0F0) · `show_rpm_value` (false) + `rpm_value_font`/`rpm_value_color` **N** (#E8E8E8) · `limiter_color` **N** (#FF0000) · ticks: `show_ticks` (true), `tick_side` (2; 0 Top/1 Bottom/2 Both), `tick_width` (3), `tick_length` (12), `tick_color` **N** (#000000) · `limiter_effect` (0; 0 None/1 Bar Flash/2 Bar Solid) · `flash_speed` (200, 50–1000). § legacy `grad_enabled`/`grad_end_color` migrate to `grad_stops` on load.

#### bar — horizontal lv_bar + label + optional value/ticks/image-fill/anchor. Alert-capable.
`show_bar_label` (true) · `label` ("" — **empty label falls back to literal "BAR1"/"BAR2"**; use show_bar_label:false or " " for none) · `bar_min` (0) / `bar_max` (100) (always emitted) · `decimals` (0) · `anchor_enabled` (false) + `anchor_value` (mid-range) + `anchor_position` (50%) — two-segment value→fill map · `show_bar_value` (false; always emitted) · `smoothing_ms` (20) · `invert_bar_value` (false; always emitted) · `center_fill` (false; symmetric fill from zero — ignored when fill_dir 2/3) · `fill_dir` (0–3 as rpm_bar; **image-mode bars honour only 0/1**) · `bar_in_range_color` **N** (#00FF00; always emitted) · `grad_stops` · `bar_image_full` **N** ("") fill image · `fill_edge_width` (0, 0–20; bright tip) + `fill_edge_color` (#FFFFFF) · `indicator_radius` (5) · `bar_bg_color` **N** (#2E2F2E) · `bar_image` **N** ("") track image · `bar_radius` (5) · `bar_border_width` (2) · `bar_border_color` **N** · fonts/colors: `label_font`, `label_color` **N**, `value_font`, `value_color` **N** · ticks: `show_ticks` (false), `tick_side` (2), `tick_count` (5, 2–30), `tick_width` (2), `tick_length` (6), `tick_color` **N** (#E8E8E8) — drawn rectangles, ≤60 objects · alerts: `bar_alerts_enabled` **E** (editor-only, deleted on save), `bar_low`/`bar_high` (**not persisted**), `bar_low_color` **N** (#0000FF) / `bar_high_color` **N** (#FF0000) (always emitted). § `bar_bg_opa` (255) is a real firmware field missing from the schema.

#### indicator — turn telltale, cap 2.
`slot` (0 Left / 1 Right, read-only) · `input_source` (0 Wire / 1 CAN) · `color_on` **N** (#FFBF00) · `opa_on` (255) · `color_off` **N** (#333333) · `opa_off` (70) · `animation` (true; fade) · `is_momentary` (schema default false; **factory struct default is true** — discrepancy). Base/live colour twins keep active rules out of saves.

#### warning — round lamp (or image) + caption, cap 8.
`label` (textarea, multi-line; factory "Alert N"; always emitted) · `active_color` **N** (#FF0000) · `active_opa` (255) · `inactive_color` **N** (#292C29) · `inactive_opa` (180) · `radius` (100 = circle) · `border_width` (0) · `border_color_style` **N** (#000000 — JSON key really is `border_color_style`; night key + rules accept `border_color` too) · `image_name` **N** ("" ⇒ circle mode) · `image_scale` (100; **percent on the wire**, unlike widget image) · `show_label` (true) + `label_color` **N** + `label_font` + `label_y_offset` (11) + `label_text_align` (1) · `flash_mode` (0 Solid / 1 Flashing; flash timer only exists while flashing+active) · `flash_speed` (200) · `lamp_on` (**rule-only field**: force lamp on/off from a rule — the multi-threshold lamp-ladder building block) · `is_momentary` (true) · `invert_toggle` (false; show when signal OFF).
Rendering contract: `update_warning_ui_immediate` is the **only** paint path; rules write a dedicated override block (never base fields); precedence **rule > night > base**. Night image = hidden sibling object.

#### text — one label: static string or live value.
`static_text` ("" — non-empty wins over live value) · `decimals` (0; always emitted) · `font` · `text_color` **N** · `rotation` (0–359°). § `slot` (= value index). Duplicate-text memo avoids per-tick invalidation.

#### meter — lv_meter round gauge. Keep w==h.
Data: `min` (0) / `max` (100) · `reverse` (false) · `anchor_enabled/_value/_position` (piecewise value→angle; tick labels stay linear) · `smoothing_ms` (20) · redline: `redline_enabled` (false), `redline_threshold` (80), `redline_color` (#FF0000), `redline_show_arc` (true), `redline_arc_width` (6), `redline_arc_r_mod` (0), `redline_recolor_ticks` (true).
Angles/ticks — **editor fields** `start_angle_user` (225) **E**, `sweep_degrees` (270) **E**, `major_tick_step` (50) **E**, `minor_tick_step` (10) **E**; **device fields** § `start_angle` (135), `end_angle` (45), `minor_tick_count` (21), `major_tick_every` (5). `mid_tick_step` (0=off) **is** a real device field (second scale tier). Editor auto-derives "nice" steps until the first manual edit (`auto_ticks`, config-only).
Tick styling: `show_ticks` (true) · widths 4/2/2 · lengths 15/13/10 · colors `major_tick_color` **N** #FFFFFF, `mid_tick_color` #BDBDBD, `minor_tick_color` **N** #9E9E9E · per-tier tick **images** `major/mid/minor_tick_image_name` (author pointing up; stamped+rotated per tick) + shared `tick_image_scale` (100, 10–400%) · outline/glow: `tick_outline_strength` (0–255), `tick_outline_color`, `tick_outline_fade` (0–20 px) — drawn tiers only. All baked into the snapshot → zero runtime cost.
Numbers: `show_tick_labels` (true) · `tick_label_font` · `tick_label_color` **N** · `label_gap` (10, −150–150) · `tick_label_divisor` (1–100000).
Frame: `meter_bg_color` **N** (#3D3D3D) · `meter_bg_opa` (255) · `bg_image_name` **N** · `border_color` **N** / `border_width` (0) / `border_opa` (255) · `scale_padding` (0–100).
Needle: `show_needle` (true) · `needle_width` (4) · `needle_color` **N** · `needle_r_mod` (−10) · `needle_rear_length` (0) · `needle_inner_radius` (0–400; big perf win — hidden inner segment isn't drawn/invalidated) · tip: `needle_tip_style` (0 Flat/1 Rounded/2 Lance/3 Dagger/4 Spade/5 Diamond) + `needle_tip_base_w`/`_point_w`/`_taper` (0=auto per style) · `needle_image_name` **N** (overrides drawn needle; **image needles get no shadow**) + `needle_pivot_x/y` (±400) + `needle_angle_offset` (±180) · ball: `show_needle_ball` (true), `needle_ball_size` (10), `needle_ball_color` **N** · shadow: `shadow_enabled` (false), `shadow_dynamic` (true; offset scales with sin(angle)), `shadow_offset_x/y` (3/4), `shadow_opa` (120), `shadow_width_extra` (2), `shadow_color`.
§ `static_ticks` (default **true**): ticks/labels/background/redline snapshot once at create (~125 KB PSRAM per meter at 200×200; **4-meter bake cap**); only the needle redraws. Night mode with baked-in overrides builds a **second complete lv_meter** (dual-object) with its own snapshot.

#### image — single .rdmimg bitmap.
`image_name` **N** ("") · `image_scale` — **schema/editor = percent (10–200, default 100); firmware = raw LVGL zoom (256 = 100%)**; only the editor converts — hand-written layouts must use zoom units · `opacity` (255) · `recolor` **N** (#000000) + `recolor_opa` (0 = no tint). § `auto_size` (bool; scale-to-fit, suppresses image_scale). Night image = dual object. Slices are refcounted/cached.

#### shape_panel — drawn rect/circle/polygon; near-zero cost; the go-to background.
`shape_type` (string: `rectangle` (default), `circle`, `triangle`, `diamond`, `arrow_right/left`, `chevron_right/left`; firmware also accepts **`trapezoid`** which the schema omits — hand-written layouts only; unknown → rectangle) · `bg_color` **N** (#1A1A1A) · `bg_opa` (255) · `border_color` **N** / `border_width` (0) / `border_radius` (10) · shadow: `shadow_width` (0), `shadow_color` **N**, `shadow_opa` (128), `shadow_ofs_x/y` · `bake_into_gauge` (false — composites the shape **into** an overlapping meter's cached face: zero per-frame cost, renders below the needle, static only). § `taper` (20, 0–50 %) + `taper_side` ("bottom"|other) for trapezoid.

#### arc — lv_arc sweep gauge + overlay lv_meter for ticks/labels/value-line.
Modes by config: track+fill images → image mode (clipped fill, `arc_image_radial` for angular uncover); track image + signal → drawn fill over image; track image only → static; neither → standard. **In standard mode `w->root` is a container**, not the arc (sector-clip + arc + redline + overlay meter + snapshot + value label live inside). Rules/overlay rebuilds apply in standard mode only.
Value: `signal_min` (0) / `signal_max` (100) · tick window `tick_min`/`tick_max` (ticks confined to a sub-range while fill spans full range; ≤ disables) · anchor trio · `reverse` · `smoothing_ms` (20) · **E** `start_angle_user` (225) / `sweep_degrees` (270) → § `start_angle` (135) / `end_angle` (45).
Fill: `arc_width` (10, 1–50) · `arc_offset` (0–120 radial inset for stacking) · `arc_color` **N** (#00FF00) · `grad_stops` (**value-walked**, absolute colours) · `bg_arc_color` **N** (#333333) / `bg_arc_width` (10) · `rounded_ends` (false) · `fade_fill` (false) · lead edge: `lead_edge_enabled` (true), `lead_edge_color` (#E6FAFF), `lead_edge_width` (6, 0=off) · image params per layer: `arc_image[_full]` **N**, `_opa` (255), `_recolor` + `_recolor_opa`, `_blend` (0 Normal/1 Add/2 Sub/3 Multiply), `arc_image_radial` (false).
Redline: `redline_color` **N** · `redline_arc_width` (0 = use arc_width) · `redline_recolor_fill` (true) · § gate `redline_enabled` (false) + `redline_threshold` (80).
Ticks (all `enabled_by: show_ticks` (false)): **steps are real firmware fields here** — `major_tick_step` (50) / `mid_tick_step` (0) / `minor_tick_step` (10) derive counts over the active range; widths 4/2/2, lengths 15/13/10, colors major **N** #FFFFFF / mid #BDBDBD / minor **N** #9E9E9E; per-tier images with per-tier `_image_scale` (100)/`_opa`/`_recolor`/`_recolor_opa`/`_offset` (±60); outline trio. § legacy `minor_tick_count` (21)/`major_tick_every` (5)/`mid_tick_count`/`tick_image_scale`.
Numbers: `show_tick_labels` (true), `tick_label_font`, `tick_label_color` **N**, `label_gap` (10), `tick_label_divisor` (1), `ticks_on_top` (**schema default false, firmware default true — known mismatch**).
Alerts: `arc_alerts_enabled` (false — persisted, unlike bar's), `arc_low`/`arc_high` (not persisted, channel-owned), `arc_low_color` **N** (#0000FF) / `arc_high_color` **N** (#FF0000).
§ value overlay: `show_value` (false), `value_font`, `value_color` **N**, `value_y_offset`, `value_decimals`, `value_unit` · value line/needle: `show_value_line` (false), `value_line_width` (4), `value_line_color` **N**, `value_line_r_mod` (−10) · limiter: `limiter_effect` (0/1/2), `limiter_value` (90), `limiter_color`, `flash_speed_ms` (200).
Fill precedence: **rule > limiter > alerts > redline > normal** (rule colour is cached and restored when a limiter/redline zone clears). Perf: overlay ticks re-render on dirty-rect crossings (~16 ms/frame on 600 px arcs at full sweep) — mitigated by a sector-bbox tick snapshot (~100 KB for 600 px).

#### toggle — lv_switch (or image) with CAN TX and/or signal reflection.
`label` ("") + `show_label` (true) · `signal_on_threshold` (0.5) · TX: `tx_can_id` (0 = disabled), `tx_bit_start` (0–63), `tx_bit_length` (1–32), `tx_endian` (1 = little), `tx_rate_hz` (10, 0–50; 0 = on-change only, else keepalive while ON) · `remember_state` (false; NVS persist, non-momentary only) · `image_name` **N** ("" = switch mode) · colors: `active_color` **N** (#00FF00) / `active_opa` (255) / `inactive_color` **N** (#555555) / `inactive_opa` (100) / `label_color` **N** · `font`, `label_align` (1), `label_x/y` (0). § `momentary` (hold-to-activate).

#### button — momentary or latching CAN TX button. No signal binding at all.
`label` ("BTN") + `show_label` (true) · `latch` (false; **a momentary always sends OFF on release** — v16) · `remember_state` (false, latch only) · TX quad as toggle · `bg_color` **N** (#333333) / `text_color` **N** / `pressed_color` **N** (#555555) · `border_radius` (5) · `font`/`label_align`/`label_x/y` · `image_name` **N**. § `pressed_image_name`. Removed v16: `tx_send_release` (ignored if present).

#### shift_light — LED row toward redline, then flash. `SHL_MAX_LED = 45`.
`signal_name` (usually RPM) · `range_min` (4000) / `range_max` (7000) · `led_count` (8, 4–45) · `flash_threshold` (7200) · `flash_speed` (200) · `fill_mode` (0 L→R / 1 Outside-in) · colors **N**: `color_low` #00FF00, `color_mid` #FFFF00, `color_high` #FF0000, `color_off` #212121 · `threshold_mid` (0.5) / `threshold_high` (0.8) — **fractional positions** 0–1 · `led_spacing` (2) · `led_width`/`led_height` (0 = auto-fit) · `border_radius` (2).

#### line — straight or curved line.
`orientation` (string: `horizontal` (default), `vertical`, `diagonal_fwd` (/), `diagonal_bwd` (\\)) · `line_color` **N** (#FFFFFF) · `line_width` (4, 1–30) · `line_opa` (255) · `rounded` (false) · `dash_gap` (0 = solid) · `curvature` (0, −200…200; quadratic-bezier bow).

#### banner — full-width threshold message bar. One signal, one condition; layer banners for multi-condition.
`op` (0 `>` / 1 `<` / 2 `>=` / 3 `<=` / 4 `==` / 5 `!=` / 6 range / 7 always[test, needs no signal]; ==/!= use 0.001 epsilon) · `threshold` (0) · `range_min`/`range_max` (op 6) · `text` ("WARNING", ≤127) · `text_align` (1) · `bg_color` **N** (#FF0000) · `bg_opa` (128) · `text_color` **N** (#000000) · `font` · `border_width` (0) / `border_color` **N** · `radius` (0). Test override (`/api/banner/test`) resets on every layout rebuild.

#### pathbar — signal fill along an arbitrary polyline (Ford-style swept tach). ≤320 points, PSRAM-allocated. **No rules, no night support, no channel.**
`min` (0) / `max` (11000) · `redline` (10000; ≥max disables) · `lit_color` (#2EE4C8) / `dim_color` (#2E323A) / `dim_opa` (90) · `band_width` (22, clamped 1–80) · `rounded` (true) · `fade_fill` (false) · lead edge trio (on/#E6FAFF/6) · `redline_color` (#FF5C32) + `redline_recolor_ticks` (true) · `shape` (0 Custom / 1 L-Bend / 2 Straight / 3 45° Bend / 4 J-Hook — non-zero generates the path to fit the box) · `orientation` (0–3, corner or horiz/vert) · `corner_radius` (40; shapes 1/3) · `hook_angle` (120, 30–200; shape 4) · `smooth` (false; v15 — custom `path` points become Catmull-Rom anchors, anchors round-trip verbatim) · ticks: `show_ticks` (false; arc-length spaced), `major_tick_step`/`minor_tick_step` (0 = off, value spacing), widths 3/2, lengths 16/10, `major_tick_color` #FFFFFF / `tick_color` #9E9E9E · labels: `show_labels` (false), `label_font`, `label_color`, `label_gap` (14; v17 clamps resulting offset ≥0), `label_side` (0 Auto-centroid / 1 Side A / 2 Side B; v17), `label_along_offset` (0, ±400 px along the path; v17), `tick_label_divisor` (1) · `smoothing_ms` (20, 0–**500**).
§ `path`: flat number array `[x0,y0,x1,y1,…]` of **absolute screen pixels**; ≥2 points required; consecutive duplicates dropped.

#### anim — EXPERIMENTAL signal-driven image sequence. `hidden: true` (not in the add palette; loads/edits fine). **No rules, no night.**
Gate: `RDM_WIDGET_ANIM_ENABLED` (default **1**; build with 0 → the factory case disappears and layouts containing an anim widget drop that widget with a log, rest loads).
Frames: `/lfs/images/<prefix>_<i>.rdmimg`, ≤16, all resident in PSRAM (16 × 240×320 ≈ 3.7 MB worst case; 8–12 recommended); one lv_img swaps sources.
`anim_upload` **E** (editor upload affordance — slices GIF/video/stills and pushes frames) · `frame_prefix` ("") · `frame_count` (8, 1–16) · `mode` (0 "Follow value (scrub)" — frame tracks reading like a needle; 1 "Loop above threshold"; anything else coerced to scrub — no free-running mode) · `range_min` (0) / `range_max` (8000) (scrub) · `threshold` (6000) (loop) · `loop_fps` (12, 1–30) · `hyst_pct` (2, 0–10 % of range — anti-flicker). Loop timer paused while inactive.

#### Shared sub-objects
- **`grad_stops`** (bar, rpm_bar, arc): `[{pos:0..100, color:<RGB565 int>}, …]`, 2–8 stops, pre-sorted (loader sorts defensively), horizontal, no dither. Alert/redline/limiter states always paint solid over the gradient. Legacy `grad_enabled`+`grad_end_color`/`bar_grad_*` migrate to 2-stop arrays at load and are dropped on save.
- **`smoothing_ms`** (bar, rpm_bar, arc, meter, pathbar): displayed-value easing at display rate (0 = snap; default 20 ms; first value snaps; stale resets).

### 4.7 Conditional rules and night overrides

**Rules** (`config.rules`, all types except pathbar/anim; those parse-and-do-nothing):

```jsonc
"rules": [
  { "signal_name": "COOLANT_TEMP",          // UPPERCASE registry signal name — NOT a channel id.
    "op": ">=",                              // > < >= <= == != range  (unknown → ==; ==/!= use 0.001 eps)
    "threshold": 105,                        // or range_min/range_max for op "range"
    "overrides": [ { "field": "value_color", "type": "color",  "value": 63488 },
                   { "field": "lamp_on",     "type": "bool",   "value": true } ] } ]
```

- Override `type` ∈ number/color/bool/string; value read only if the JSON type matches (mismatch → silent 0). Colours are RGB565 ints. Limits: 16 rules/widget, 16 overrides/rule, 32 merged overrides, 32-char names/strings.
- A rule whose `signal_name` doesn't resolve is permanently inert (logged once). A **stale** signal never matches.
- Active rules merge in array order — later rules win per field — then one `apply_overrides` call; a `last_rule_mask` change-gate collapses steady-state cost to zero; one immediate evaluation fires right after subscribe (hot-reload correctness).
- **`count == 0` fully reverts**: every implementation restores from configured base values (base_*/cur_* twins or, for warning, a dedicated override block with rule > night > base precedence). Active rules are never persisted as configuration.
- Overridable fields per type: panel (bg/border/label/value colours+fonts, border width/radius) · bar (bg/border colours+width, label/value colours+fonts) · rpm_bar (bar_color, limiter_color) · indicator (colours+opacities) · warning (active/inactive/border/label colours, border_width, flash_mode/speed, **lamp_on**) · text (text_color, font) · meter (border/bg/needle/ball colours) · image (image_name, opacity, recolor±opa) · shape_panel (bg/border/shadow set) · arc (arc/bg colours+widths, standard mode only) · toggle/button (state colours) · shift_light (4 colours) · line (colour/opa/width/curvature) · banner (bg/opa/text colour). Editor stores overrides as an object `{field: value}` and converts to the array form on save.

**Night overrides** (`config.night`, keyed by the day field name; **currently inert** — see the `NIGHT_MODE_DISABLED` flag): colour values RGB565 ints, image values strings; only explicitly-set keys are serialized; empty night objects never ship. Root trigger `night_mode:{signal_name, active_when}` (active when value ≥ active_when, default 1.0); manual toggle + (future) time-of-day are the other sources, most-recent-wins.
Per-type support: panel (border/bg/label/value) · rpm_bar (bar/limiter/tick/bg/value colours) · bar (7 colours + track/fill images) · indicator (on/off) · warning (active/inactive/border [stored as `border_color`]/label + image) · text · meter (tick colours, needle/ball/border/bg/label colours + needle & bg images) · image (recolor + image) · shape_panel (bg/border/shadow) · arc (10 colours + both images) · toggle/button (colours + image) · shift_light (4) · line (line_color) · banner (bg/text/border). pathbar/anim: none.
LVGL v8 can't live-swap several baked properties, so night uses the **dual-object pattern**: image → hidden sibling lv_img; warning → sibling image, colours applied to both; meter → an entire second lv_meter with its own ~125 KB snapshot; arc → overlay-meter rebuild on transition for baked tick/label colours. Cost implications: night meters double snapshot memory; night images double decode memory.

### 4.8 Known schema↔firmware discrepancies (defaults-only saves make these behavioural)

1. `arc.ticks_on_top`: schema false vs firmware true — silently changes z-order on saved arcs.
2. `line` min size: schema 10×1 vs firmware 2×2.
3. `indicator.is_momentary`: schema false vs factory true.
4. `shape_panel` `trapezoid` (+`taper`/`taper_side`): implemented, not selectable in the editor.
5. `bar.bar_bg_opa`: real firmware field, invisible in the inspector.
6. `image.image_scale`: percent (editor) vs LVGL zoom ×256 (wire) — only the editor converts; `warning.image_scale` is percent on the wire by design.
7. Meter `minor/major_tick_step` are editor-only (device wants count/every); arc and pathbar steps are real device fields; `mid_tick_step` is real on both.
8. `start_angle_user` help text says "0 = 3 o'clock"; the `_doc` and code say 12 o'clock (the `_doc` is right).
9. `bar.slot` editor-unlimited vs firmware `&1` cosmetics mask.
---

## 5. Channels & signals

The **channel system** (`main/data/`) is the binding layer and the canonical CAN-decode owner (ADR-0005/0006). A *channel* is a named, device-local binding (`rpm`, `coolant_temp`, `custom_turbo_speed`) that owns its CAN decode, thresholds, units, and provenance; widgets bind to channels by id (or directly to signals by name); the channel feeds the **signal registry**, which is the per-frame hot path. Channels live in `/lfs/channels.json` — device-local, NOT part of a portable layout. This split is what makes layouts portable: a marketplace layout references channel ids; any dash with those channels configured auto-binds.

### 5.1 Canonical channel registry (`canonical_channels.c`)

Compile-time constant table: **135 channels across 14 groups** (in-source comments claiming 92/12 are stale). Each def: `id`, `label`, `group`, `tier` (1/2/3 UI priority), `cardinality` (scalar/enum/boolean), `units_native` (what the ECU transmits), `units_display_def`, `decimals` (0–3), `min/max_default`, `low_warn`/`high_warn` (float sentinels ±2^31 when unset), `color_normal` (0xRRGGBB), `notes`.

Zones: `v <= low_warn` → LOW_WARN, `v >= high_warn` → HIGH_WARN, else NORMAL. Default zone colours when a channel's colour is the `CHANNEL_USE_DEFAULT_COLOR` sentinel: low 0x4080FF (blue), normal 0xC0C0C0, high 0xFFA000 (amber). **`coolant_temp` is the only canonical channel shipping non-unset thresholds (80/110).**

Groups (`channel_group_t` — **append-only**: the int is persisted in channels.json): Engine-Core(0), Engine-Exhaust, Drivetrain, Electrical, Chassis-Dynamics, Chassis-Per-Corner, Brakes&Inputs, Fuel&Distance, Environment, Lap-Timing, Body&Lights, Diagnostics, Dash-System, Position. Group display names are ASCII-only (device font constraint). Cardinality (`channel_cardinality_t`, also persisted numerically): 0 scalar, 1 enum, 2 boolean.

Custom channels use the reserved prefix `custom_`; `channel_id_is_custom()` is a prefix check. Lookup: `canonical_channel_find` (exact) and `_find_ci` (ASCII case-fold only — `CoolantTemp` does **not** match `coolant_temp`).

**Full canonical table** (`UL`/`UH` = unset threshold; units shown native→display; all thresholds unset except where noted):

**Engine – Core (33, tier 1 unless noted):** `rpm` RPM rpm 0dp 0–8000 #FF3030 (PID 0x0C) · `coolant_temp` °C 0–120 **warn 80/110** #FF8040 (0x05) · `oil_temp` °C 0–130 #FFA040 · `oil_pressure` kPa→**bar** 1dp 0–1000 #FFAA00 (<0.5 bar = engine death) · `intake_air_temp` °C −20–80 (0x0F) · `throttle_position` % 0–100 #FFD000 (0x11-noted; actually fed by 0x45 — see OBD2) · `engine_load` % 0–100 (0x04) · t2: `ignition_timing` °BTDC 1dp −10–50 (0x0E) · `fuel_pressure` kPa→bar 1dp 0–700 · `manifold_pressure` kPa 0–250 (0x0B) · `boost_pressure` kPa→bar 2dp −100–300 · `boost_target` kPa→bar · `mass_air_flow` g/s 1dp 0–500 (0x10) · `afr_bank1`/`afr_bank2` AFR 2dp 9–18 #80FF80 · `lambda_bank1`/`lambda_bank2` λ 2dp 0.7–1.3 ("λ×1000 internal" note) · `wideband_1`/`wideband_2` AFR 9–18 (deliberately distinct from lambda_bank* — Haltech-style numbered AFR sensors) · `target_afr` AFR · `target_lambda` λ · `injector_duty` % 1dp 0–100 (>85% = injector limit) · t3: `lambda_correction` % ±25 · `absolute_load` % 0–300 (0x43) · `fuel_rail_pressure` kPa→bar 0–20000 (0x23) · `fuel_pressure_diff` kPa→bar 2dp · `coolant_pressure` kPa→bar 2dp 0–400 · `knock_count` 0–99 #FF0000 · `knock_retard` ° 1dp 0–15 · `injector_pulse_width` ms 2dp 0–25 · `volumetric_efficiency` % 0–150 · `short_term_fuel_trim`/`long_term_fuel_trim` % 1dp ±25.

**Engine – Exhaust (12, all °C/kPa/% scalars, #FF6020 family):** `exhaust_gas_temp_avg` (t2) 0–1000 · `egt_cyl_1..8` (t3) 0–1000 · `exhaust_back_pressure` kPa 0–500 · `wastegate_duty` % 1dp · `egr_command` % (PID 0x2C).

**Drivetrain (9):** `vehicle_speed` (t1) km/h 0–300 #FFFFFF (0x0D; imperial → mph) · `gear` (t1, **enum −2..8**: −2=P, −1=R, 0=N, 1–8; calculated-gear path available) · `transmission_temp` (t2) °C 0–150 · `transmission_pressure` (t3) kPa→bar 0–2500 · booleans `clutch_switch`, `launch_control_active`, `antilag_active` · `differential_temp` °C · `traction_control_intervention` % 0–100.

**Electrical (5, V/A, #80A0FF):** `battery_voltage` (t1) V 1dp 10–16 · `alternator_voltage` (t2) · `alternator_current` A 0–150 · `ecu_voltage` V **2dp** · `system_current` A 0–250. **Voltage is stored as REAL volts (12.0 = 12.0, not 120) — never ×10 a new source**; every decode in the presets and OBD2 PID 0x42 (×0.001) confirms.

**Chassis – Dynamics (5, #C0C0C0):** `lateral_g`/`longitudinal_g`/`vertical_g` g 2dp range **±250** (range expressed as g×100 — internal inconsistency, flag when relevant) · `yaw_rate` °/s ±180 · `steering_angle` ° ±540.

**Chassis – Per-Corner (20, all t3):** `wheel_speed_{fl,fr,rl,rr}` km/h 0–300 · `tire_temp_*` °C 0–150 · `tire_pressure_*` kPa→**psi** 1dp 100–350 · `suspension_travel_*` mm 0–200 · `ride_height_*` mm 0–300.

**Brakes & Inputs (5):** `brake_pressure_front`/`rear` (t2) kPa→bar 0–20000 #FF4040 · `brake_pedal_position` % · `accel_pedal_position` (t2) % (note says PID 0x49; actual OBD2 map uses 0x5A) · boolean `handbrake`.

**Fuel & Distance (8):** `fuel_level` (t1) % 0–100 #40C040 (0x2F; "low warn at 15%" note but ships unset) · `fuel_remaining_distance` (t2) km 0–1000 (imperial → mi; km↔mi conversion **not** in the unit table) · `fuel_consumption_instant` L/h 1dp 0–60 · `fuel_consumption_avg` L/100km 0–40 · `odometer` (t2) km 0–9999999 · `trip_distance` (t2) km 1dp · `ethanol_pct` (t2) % (0x52) · `fuel_rate` L/h (0x5E).

**Environment (3):** `ambient_temp` (t2) °C −30–55 (0x46) · `cabin_temp` °C · `barometric_pressure` kPa 60–110 (0x33).

**Lap Timing (8, t3)** — registry entries exist (`lap_time_current/last/best`, `lap_number`, `lap_delta_best`, `sector_time_current`, `sector_number`, `lap_time_theoretical`; times formatted M:SS.sss ≥60 s) but the feature is in-development GPS territory — out of customer-doc scope.

**Body & Lights (6, t3 booleans):** `headlights`, `high_beam`, `turn_signal_left`/`right` #40C040, `hazards`, `door_open`.

**Diagnostics (6):** `check_engine` (t2, boolean, #FFA000) · `dtc_count` 0–99 · booleans `coolant_level_low`, `oil_level_low` · `distance_with_mil_on` km (0x21) · `runtime` s (0x1F).

**Dash Internals (7, t3, #4CC9F0)** — the dash's own runtime stats, **auto-activated and pre-bound on every boot**: `dash_fps`→signal `FPS` 0–60 · `dash_cpu`→`CPU_PERCENT` · `dash_free_heap`→`FREE_HEAP_KB` 0–400 · `dash_free_psram`→`FREE_PSRAM_KB` 0–8000 · `dash_uptime`→`UPTIME_S` · `dash_chip_temp`→`CHIP_TEMP` 0–120 · `dash_wifi_rssi`→`WIFI_RSSI` −90..−30 dBm.

**Position & GPS (8, #40C0FF)** — registry entries exist (`gps_latitude/longitude/speed/altitude/heading/satellites/fix_type/accuracy`); in-development GPS scope. (Design note kept for accuracy: lat/lon as float carry ~1.7 m quantisation, so lap timing never reads these channels.)

### 5.2 Channel data model & `channels.json`

Every channel carries: identity (`id[32]`, `label[32]`, tier, group, cardinality, `is_canonical`); source (`signal_name[32]`, `signal_index` −1=unbound); **CAN decode** (`can_id` 0=none, `bit_start`, `bit_length`, `decode_scale/offset`, `is_signed`, `endian` 0=Motorola/1=Intel, `decode_unit[8]`); format (`units_native[8]`, `units_display[8]`, `decimals`); range (`min/max` float) + sanity bounds (values **outside sanity are dropped as stale, not clamped**); thresholds `low_warn`/`high_warn` (float sentinels; the "critical" tier was retired); three zone colours (sentinel = use convention); **math** (enabled, operands a/b each a channel id or float constant, op 0..3 = + − × ÷); live state (`current_value`, `is_stale`, `last_update_ms`, `last_zone`); a listener list.

Listeners fire on **metadata changes only** (range/threshold/binding/colour edits) — never per-value (firing per-frame caused panel flicker). Widgets still subscribe to *signals* for per-frame updates; the channel layer is metadata/threshold/config, not a dispatch step.

**`/lfs/channels.json`** (schema v3): `{schema_version, channels:[…]}`, one object per active channel, **defaults-only for canonical channels** (only fields differing from the canonical def), everything for customs. Keys: `id`, `label?`, `group`/`card` (**customs only** — ints, hence append-only enums; without them customs regrouped to Diagnostic on reboot), `signal?`, `units_native?`, `units_display?`, `decimals?`, `min?`, `max?`, `sanity_min/max?`, `low_warn?`/`high_warn?`, `color`/`color_low_warn`/`color_high_warn?`, `math?{a,b,op}` (string = channel id, number = constant), `decode?{can_id,bit_start,bit_length,scale,offset,is_signed,endian,unit?}` (whole block or absent).

Load-side self-heal: enum ranges validated; invalid decode geometry **dropped** (better than decoding garbage); a threshold on the wrong side of the range (would be "always red") reset to unset; math requires both operands with ≥1 channel string; duplicate-id double-subscribe guarded (would corrupt memory on every CAN frame).

**Persistence idiom** (the durability reference for the firmware): serialize under a 500 ms LVGL-lock try (timeout → defer + re-arm, never iterate unlocked), write `.tmp` → fflush+fsync → `rename(live→.bak)` → `rename(.tmp→live)` (atomic on LittleFS). Saves are debounced 500 ms; **signal-binding and decode edits flush synchronously** (key-off can cut 12 V any time); `begin_bulk`/`end_bulk` (depth-counted) coalesce wizard-scale edits into one flush; a shutdown handler flushes on clean reboot. Crash recovery distinguishes transient I/O (3 retries, never demotes a good file) from corruption (live → `.corrupt` for diagnostics, then `.tmp`/`.bak` recovery, republished as live); "valid JSON but no channels array" is treated as corrupt too; the mid-rename power-loss window is recovered from `.tmp`. Export/import: `channel_manager_export_raw` / `import_raw` (validates parse + array + version ≤ current before atomically replacing; takes effect on the **next boot** — the API layer reboots).

Store limits: max **128** channels (PSRAM-allocated, swap-remove deletes → iteration order not stable).

### 5.3 Channel manager behaviour worth knowing

- `channel_manager_init` (after LittleFS, before layout load): load-or-seed. Fresh device seeds the **default OBD2 set** — `rpm, coolant_temp, vehicle_speed, throttle_position, engine_load, intake_air_temp, battery_voltage, fuel_level, gear, ambient_temp` — and is marked "born pre-v3" so the first layout load runs the decode migration. Dash-system channels are ensured (activated + bound) **every boot**.
- `activate(canonical_id)` is idempotent; `create_custom` enforces the `custom_` prefix, tier 2, decimals ≤3, thresholds unset.
- `set_units_native` **re-expresses** min/max/thresholds/sanity into the new unit when a conversion exists (stored numbers always live in native units); `units_display` is independent.
- `set_signal` clears the decode when the new binding isn't a CAN signal (prevents provenance corruption where an OBD2 signal would be re-registered as CAN with a bogus decode).
- `set_decode` is the single validating writer (can_id ≤ 0x1FFFFFFF; bits within 64) and immediately upserts + binds the runtime signal.
- `reset_to_defaults` (canonical only) preserves the signal binding.
- `resolve_signals` (after every registry change): empty name = explicit unbind (no auto-rebind); exact match first; on miss, a **canonical-vocabulary fallback** maps every registered signal name through the ECU-name→canonical dictionary and binds the first matching quantity — so channels light up across ECU vocabularies without re-running the wizard; the stored name is preserved on total miss (widget falls back to its own `signal_name`).
- One-time migrations: **v1→v2** heal of the shared-binding bug (families like `egt_cyl_1..8` all bound to one `EGT` signal — keeps the single genuine match, clears the rest, never touches customs, runs once); **v2→v3** decode migration (copies decode from the registry — i.e. the active layout's signals — into channels; channelizes unowned CAN signals as canonical-or-`custom_*` with a same-quantity/different-vocabulary reuse exception; only stamps v3 when nothing was skipped; skipped for splash layouts).
- Legacy v13 layouts: widgets call `record_legacy_widget` at load — two-step canonical match (case-fold, then ECU-vocabulary map), first-write-wins population of min/max/warn/colours so the registry auto-populates on the first v14 boot without clobbering later user edits.
- Display formatting (`channel_format_display_value`): lap-timing seconds ≥60 render `M:SS.sss` (round-before-split, zero-padded); otherwise unit-convert then `%.*f`; otherwise fall through to `signal_format_value` (which applies value→label maps). **Thresholds/zones always compare native values — only the rendered number converts.**

### 5.4 Unit conversion (`unit_convert.c`)

Linear only (`out = v*scale + offset`), each pair listed one direction and derived in reverse. Exact string match on the canonical UTF-8 unit literals. **Pass-through rule:** unknown pair, identity, or empty/NULL strings → value returned unchanged (a relabel never silently scales).

| from → to | scale | offset |
|---|---|---|
| kPa → bar | 0.01 | 0 |
| kPa → psi | 0.14503774 | 0 |
| kPa → MPa | 0.001 | 0 |
| kPa → inHg | 0.29529983 | 0 |
| bar → psi | 14.503774 | 0 |
| bar → MPa | 0.1 | 0 |
| °C → °F | 1.8 | 32 |
| °C → K | 1.0 | 273.15 |
| km/h → mph | 0.62137119 | 0 |
| m/s → km/h | 3.6 | 0 |
| m/s → mph | 2.23693629 | 0 |
| λ → AFR | 14.7 | 0 |

All usable in both directions. `unit_convert_supported` gates the UI picker (true for identity); `unit_convert_targets` enumerates alternatives (0 = hide the picker). Gaps: no km↔mi, mm↔in, L/100km↔mpg, g/s, L/h, V/A/ms/count conversions despite channel notes referencing some. **Edge:** ECU preset tables emit ASCII units (`degC`, `lambda`) for the device font while canonical channels use UTF-8 (`°C`, `λ`) — exact-match conversion means a preset-sourced `degC` native unit will *not* find the °C→°F conversion.

### 5.5 Derived / math channels (`channel_math.c`)

**There is no expression parser.** A math channel is strictly `<a> <op> <b>` where each operand is a channel id **or** a numeric constant, and `op ∈ {0 +, 1 −, 2 ×, 3 ÷}` (persisted numerically → append-only).

- Operands are **channel ids, not signal names**, so re-decoding or rebinding an operand keeps the math working.
- The math channel owns a synthetic registry signal **`MATH_<UPPERCASE_ID>`** (`SIGNAL_SOURCE_INTERNAL`), e.g. channel `boost_pressure` → signal `MATH_BOOST_PRESSURE`. It is re-registered by `channel_math_register_signals()`, which is called from `channel_manager_register_decoded_signals()` — so every load path that restores CAN decodes also restores math outputs.
- A **5 Hz LVGL timer** (200 ms — "math sources are gauges, not control loops") evaluates each enabled channel and pushes via `signal_set_external_value`, so peaks, staleness, subscriber notify and zone evaluation all run as for any live signal.
- **If either operand is unbound or stale the push is skipped** and the output goes stale through the normal 2 s timeout. Division by |b| < 1e-9 and non-finite results are also skipped (last value held, then stale).

Validation (`channel_math_set`): op ≤ 3; **not both operands constant** ("that's a fixed number, not a derived channel"); constants must be finite; a channel operand must be non-empty, **must not be the channel itself** (self-reference would integrate garbage one tick later), and must exist. **Chains through other math channels are explicitly allowed** (they evaluate one tick behind). Success re-registers the output signal and binds it via `set_signal` (synchronous persist).

Unit handling: two channel operands in different but convertible native units → B is converted into A's unit first (so `kPa − psi` means what it should). The result is then converted into the channel's own `units_native` **only when the dimension is preserved** — i.e. for `+`/`−`, or when one operand is a constant. **`channel × channel` and `channel ÷ channel` are not output-converted** (kPa², a ratio — a linear convert would silently mis-scale). Unknown pairs pass through.

Config persists as an additive `math` key in channels.json, so older firmware ignores it and the channel falls back to its persisted signal binding (which simply never updates).

### 5.6 ECU presets (`ecu_presets.c`)

An ECU preset maps **20 normalized signal slots** to the CAN decode parameters of a specific ECU's default broadcast stream. Applying one **replaces the active layout's `signals[]`**; widget `signal_name` bindings are preserved, so widgets immediately show live data. **All preset scales/offsets output metric** (kPa, °C, km/h, lambda) — conversions are baked in: °F→°C `scale×5/9, offset −17.7778`; AFR→λ `scale/14.7`; m/s→km/h `×3.6`; K→°C `offset −273.15`; kPa-abs→kPa-gauge `offset −101.325`.

The 20 slots (name → canonical channel): `RPM`→rpm, `MAP`→manifold_pressure, `THROTTLE`→throttle_position, `COOLANT_TEMP`, `INTAKE_AIR_TEMP`, `LAMBDA`→lambda_bank1, `OIL_TEMP`, `OIL_PRESSURE`, `FUEL_PRESSURE`, `IGNITION`→ignition_timing, `VEHICLE_SPEED`, `GEAR`, `BATTERY_VOLTAGE`, `FUEL_TRIM`→short_term_fuel_trim, `EGT`→exhaust_gas_temp_avg, `BOOST`→boost_pressure, `FUEL_LEVEL`, `PARK_BRAKE`→handbrake, `YAW_RATE`, `LATERAL_G`. `can_id == 0` = unsupported by that ECU's broadcast; the apply path still writes the slot as an **unbound** signal so the user can add a custom source later without a name collision. Each row also carries `decimals` (stamped onto matching panel/bar/text widgets on apply) and an optional `value_map_csv` (`"0=N,1=1,…"`; no current preset uses it).

**11 presets** (9 real + 2 pseudo):

| make / version | base + endianness | notable |
|---|---|---|
| ECU Master `Black/Classic` | 0x600, Intel | IDs {0x600,0x602,0x603,0x604}; lambda ×0.0078125; oil/fuel pressure ×6.25; battery ×0.027 |
| MegaSquirt `MS3-Pro` | 0x5F0, Motorola | 10 IDs; temps °F→°C baked; speed m/s→km/h (×0.36); lambda AFR→λ (×0.0068027); FUEL_TRIM = egocor1 (1/10 % centred on 100); oil temp/pressure unsupported (generic ADC only) |
| Haltech `Nexus` | 0x360, Motorola throughout | Kelvin offsets −273.15; pressures −101.325; **GEAR corrected to 0x470 bit 56** (0x360 b6–7 is Coolant Pressure — the old binding was wrong); MAP is absolute despite the gauge comment |
| MaxxECU `1.2` | 0x520, Intel | Subset of 1.3 (no oil temp/pressure, no fuel pressure) |
| Ford `BA/BF` | factory CAN, Motorola | Slot repurposing: MAP slot = **barometric** (0x44D b56), IAT slot = **ambient** (0x353 b32), FUEL_TRIM slot = **instant L/hr** (0x437 b8, ×0.51). RPM legitimately at bit 39. Odometer 0x4C0 / range+economy 0x553 need manual signals |
| Ford `FG` | hscan 500 k, Motorola | See verification notes below |
| MaxxECU `1.3` | 0x520, Intel | Full set incl. oil temp/pressure (0x536) and fuel pressure (0x537) |
| Link ECU `Generic Dash` | 0x3E8, Intel | **Temps transmitted as (°C + 50)** → offset −50; ignition offset −100; battery ×0.01; FUEL_TRIM/EGT unsupported |
| Toyota / Subaru `86 / BRZ` | HS-CAN 500 k, Intel | **Hardware-verified 2026-07-25**; see below |
| RDM-7 `Internal` | marker | All slots unsupported; marks the `CALCULATED_GEAR` flow. Apply **leaves `signals[]` untouched** (so RPM/SPEED bindings survive) and only updates the ecu identity |
| OBD2 `Standard` | polling | All slots unsupported (OBD2 doesn't bit-decode). Apply writes the default PID list into `polled_pids` and leaves `signals[]` **empty** — `obd2_start()` registers each PID as an external signal |

**Deliberate signedness deviations from vendor DBCs (both MaxxECU presets):** the official DBCs mark nearly every field `@1+` (unsigned), but the ECU puts every bidirectional quantity on the wire as two's-complement, which an unsigned read shows as ~6500 (or 65535 for reverse gear). Both tables therefore force **signed** on: `COOLANT_TEMP`, `INTAKE_AIR_TEMP`, `IGNITION`, `OIL_TEMP` (1.3), `GEAR` (reverse = −1) and `FUEL_TRIM` (±% centred on 0) — plus, in the preconfig catalog, knock correction, total ignition comp, lambda corrections A/B, accelerometer axes, VVT cam positions, wheel/target slip and the transmission/differential temps. Safe because signed decode is bit-identical to unsigned across every field's legitimate positive range (raw never sets bit 15). Temps/ignition confirmed in-vehicle; gear/fuel-trim misreads field-reported 2026-08. Lockstep between `ECU_PRESETS` and `preconfig_items` is enforced by `tools/check_preset_signedness.py` (CI: schema-check.yml). That checker matches the two tables by `(make, version)`, and the same ECU is not always spelled the same in both — the GT86 is `("Toyota / Subaru", "86 / BRZ")` in `ECU_PRESETS` but `("Toyota", "GT86 Gen 1")` in `preconfig_items`. Mismatched keys used to make the pair silently **uncompared** (CI green over a real `0x0D1 VEHICLE SPEED` divergence), so equivalent identities live in the script's `ALIASES` map and an alias that stops matching any row is a hard error rather than a silent skip. Add an entry there whenever a preset exists under two names.

**Ford FG verification state** (cross-referenced against `BigFalconSheet.xlsx` + live XR6T captures): **verified** RPM (bit **32**, not the previously-shipped 39 — that off-by-7 decoded ~15 000 rpm at idle), THROTTLE, COOLANT_TEMP, OIL_TEMP, BATTERY_VOLTAGE. **Position verified, magnitude pending a driving capture:** VEHICLE_SPEED (0x207 b32, scale 0.0078125; reads the 0xFFFF sentinel = 511.99 km/h while stationary) and GEAR (0x230 b0 — **an encoded value table, not a literal gear number**; byte 2 reads 0x68 in Park). **Unverified:** BOOST (0x425 b31 stuck at 0xFFFF even on a turbo — needs a WOT pull), FUEL_LEVEL (current bit 47/×0.392 reads plausibly but is mathematically driven by a fuel-consumption byte; real encoding likely byte 5 × 25 %/count), PARK_BRAKE (0x437 b16 is per-spec but 0x437 never appeared in captures). **Dropped to unsupported:** MAP, INTAKE_AIR_TEMP (was reading HVAC cabin temp on a bus the dash can't hear), FUEL_TRIM, LATERAL_G (0x4B0 b7 is wheel speed), YAW_RATE (the ID doesn't exist). Bus split for reference: hscan {0x12D,0x207,0x230,0x425,0x427,0x437,0x44D}, mscan {0x353,0x360,0x4B0}.

**Toyota 86 / BRZ corrections** (the old community-sourced rows "actively broke a working dash"): RPM length **16 → 14** (top 2 bits aren't rpm; a 16-bit read returns 17416 at 1023 rpm); THROTTLE moved 0x143 → **0x140 b48** (0x143 is **not broadcast at all**); SPEED 0x141 → **0x0D1** with scale 0.01 → **0.05625** (= 3.6/64 — the raw field is 1/64 m/s, so the old 1/64 scale emitted m/s labelled km/h, reading **3.6× low**: "speed stops at 30" was really 108 km/h; 0x141 b0–1 is a constant 0x8426 = 338 km/h parked); GEAR → unsupported (**not broadcast** — use `CALCULATED_GEAR` with a `value_map` so 0 renders "N"); COOLANT and OIL_TEMP **do broadcast natively on 0x360** (b24/b16) and no longer need OBD2; YAW_RATE and LATERAL_G added on 0x0D0. Genuinely OBD2-only on this platform: MAP 0x0B, IAT 0x0F, lambda 0x44, short fuel trim 0x06, timing 0x0E, battery 0x42, fuel level 0x2F — note the registry names OBD2 uses for the last two decode paths are `SHORT_FUEL_TRIM_1` and `TIMING_ADVANCE`, **not** `FUEL_TRIM`/`IGNITION`.

**Apply semantics** (`ecu_preset_apply_to_layout`): read+parse the layout → branch on preset kind → rewrite identity (`ecu`/`ecu_version`) → stamp widget `decimals` → save. Three branches: **OBD2-primary** clears `signals[]` to empty, deletes legacy `obd2_pids` *and* `polled_pids`, then writes a fresh `polled_pids` of every default-enabled PID; **marker** skips the signals rewrite; **native** rewrites all 20 slots and **preserves supplemental OBD2 gap-fillers** (`polled_pids` is cleared *only* when the previous preset was OBD2-primary — wiping it unconditionally "silently broke fuel-over-OBD2 every time the user re-applied their car's preset"; the runtime conflict guard makes keeping it safe). Per-slot apply (`ecu_preset_apply_slot_to_layout`) rewrites one signal and deliberately **does not** touch `ecu`/`ecu_version`. A batched writer (`open`/`upsert`×N/`commit`) exists because the single-signal writer does a full read-modify-write per call — 68 full-file LittleFS writes for a Haltech preset would stall LVGL for seconds and overflow the CAN RX queue. Upserts wipe stale decode params **including `value_map`** before rewriting (a new decode invalidates old value mappings).

**Match scoring** (`ecu_preset_match_score`, 0–100): `100 × |preset_ids ∩ IDs seen in the last ~2.5 s| / |preset_ids|`, round-to-nearest, from the always-on `can_id_tracker` — so it is **live**, not a one-shot scan: plug in the loom and the indicator lights within ~2.5 s; unplug and it returns to 0. Returns 0 for the OBD2 preset (no broadcast IDs) and when the tracker is empty. Threshold **30%** is deliberately forgiving (presets have 5–15 IDs and not every ID broadcasts every cycle).

**Alias resolution** (`ecu_signal_name_to_canonical`): direct case-sensitive slot match → underscore-stripped retry → an **exhaustive 62-entry explicit alias table** (EXACT match only). This replaced a fuzzy `_<SLOT>` suffix matcher that caused data corruption: `REV_LIMIT_RPM` suffix-matched `_RPM` and was renamed to `RPM`, **clobbering the real engine-RPM decode**. Unlisted names are intentionally left unmapped — "better an unbound channel the user can assign than a confidently-wrong auto-binding." Notable aliases: `ENGINE_RPM`/`ENGINE_SPEED`→rpm; `IGNITION_ANGLE`/`IGN_ANGLE_LEAD`→ignition_timing; `MGP`→boost_pressure; `WIDEBAND_1/2`→wideband_1/2 (**AFR-scaled, deliberately not the λ lambda_bank channels** — an AFR value in a λ channel is wrong units and wrong range); `EGT_SENSOR_1..8`/`EGT_1..2`→egt_cyl_*; both wheel-speed conventions (`WHEEL_SPD_FL` and `LF_WHEEL_SPEED`).

**The preconfig catalog** (`preset_picker.c`) is a second, finer-grained source list: **349 individual named signals** across 11 (ecu, version) families — MaxxECU 1.3 (100), Haltech Nexus (68), MaxxECU 1.2 (46), Link Generic Dash (41), Toyota GT86 Gen 1 (22), ECU Master (17), Ford BA/BF (16), Ford FG (14), MegaSquirt (13), RDM-7 Internal (9), RDM-7 GPIO (3). Differences from `ECU_PRESETS`: free-form `label` instead of a fixed slot; `can_id` is a **hex string** (`strtol(…,16)` at apply); fields named `value_offset`/`endianess`; **per-item `obd2_pid` + `obd2_service`** so a single channel can be sourced from a PID (bit/scale fields are then ignored — OBD2 decodes by polling); no value maps; applied via `channel_apply_preconfig`; never touches the layout's ecu identity. Identity strings differ between the two tables for the same platform (`Toyota`/`GT86 Gen 1` vs `Toyota / Subaru`/`86 / BRZ`) and the source comments mandate keeping the row data in lockstep — `tools/check_preset_signedness.py` (run in schema-check.yml CI) enforces the wire-format half of that (endian + is_signed) for every row the two tables share. Haltech baro: 0x372 bytes 6–7, 0.1 kPa, 10 Hz (shares the frame with battery voltage) — preconfig row added 2026-08; Haltech EGT frames 0x373–0x375 are 0.1 K and carry the −273.15 offset in both tables.

### 5.7 Signal registry (`signal.c`)

The per-frame hot path. `MAX_SIGNALS` **200** (PSRAM-backed; raised from 128 because signals **merge** across ECU/layout switches and a full registry silently broke OBD2 gap-fill), `MAX_SIGNAL_SUBSCRIBERS` **16** per signal, `SIGNAL_TIMEOUT_MS` **2000**, test-lock TTL **5 min**, value-map ≤32 entries with 12-char labels.

`signal_t`: `name[32]`, decode (`can_id`, `bit_start`, `bit_length`, `scale`, `offset`, `is_signed`, `endian`), `source` (**provenance, not decoder**: CAN / OBD2 / INTERNAL), `unit[8]`, optional `value_map`, and runtime state (`current_value`, `peak_value`, `min_value`, `session_peak`, `session_min`, `tracking_active`, `is_stale`, `test_locked`, `test_lock_ms`, `last_update_ms`, subscriber array).

**Peaks are session-only.** Despite a stale struct comment saying "persisted across reboot", the authoritative header note and the implementation agree: all four peak/min fields reset every boot and **signal.c contains no NVS access at all**. The "all-time vs session" distinction is purely reset granularity within one boot.

**Registration is UPSERT.** An existing name has its decode fields overwritten in place and its index returned — **latest layout wins** — while **subscribers, peak/min stats, the value_map and the index are preserved** (so channel bindings resolved by index stay attached). Re-registration also forces `is_stale = true` + `last_update_ms = 0` (so the next frame re-notifies; otherwise a still-fresh slot whose value happens to match the new decode would swallow the first update) and **releases any test lock** — preserving it created a *zombie*: dispatch skipped the signal (locked) and the staleness sweep skipped it too, so one forgotten editor Test Value plus any reload left the signal permanently showing `--`. The previous first-wins behaviour was a real bug: a splash layout carrying a frozen registry snapshot loaded before the dashboard would shadow the dashboard's authoritative decode, and the signal decoded garbage across reboots.

`signal_subscribe` **does not de-duplicate** — every caller must guard, which is why the channel decode registrar and the meter channel binder only re-subscribe when the index actually changed, and why the channels.json loader guards duplicate ids (a double subscription dangles after the channel is freed and corrupts memory on every later CAN frame).

**Dispatch** (`signal_dispatch_frame`, LVGL task, mutex held): skip signals with `can_id == 0 || bit_length == 0` (OBD2/internal signals are fed by their own producers — without this a legal CAN ID 0x000 frame would match every one of them); skip on ID mismatch; **skip while `test_locked`** (so an injected value stays pinned against live traffic); length-guard against short frames; extract bits, scale+offset; update all four peak/min fields when tracking; then the **change gate** — notify only when `was_stale || value != current_value`, which avoids redundant LVGL work on duplicate frames. A deadline-based `signal_dispatch_pause_ms()` suspends dispatch during the boot reveal animation (deadline rather than a flag so a torn-down animation can't wedge dispatch permanently).

**Three injection paths**, deliberately distinct:

| path | test lock | peak/min | used by |
|---|---|---|---|
| `signal_dispatch_frame` | drops the frame | yes (when tracking) | real CAN decode |
| `signal_inject_test_value` | **re-arms the TTL** | only when `tracking && !sim && !test_locked` | simulator, replay, internal signals, `/api/signal/inject` |
| `signal_set_external_value` | **returns early** | yes when `tracking && !sim` | OBD2 decode, internal synthesis, `MATH_*`, DTC monitor |

Replay deliberately **does** feed peaks (it plays back real logged data); the simulator and manual injects deliberately do not.

**Staleness** (`signal_check_timeouts`): returns immediately while the simulator is active ("sim keeps signals fresh"); test-locked signals are held fresh **but only while the editor keeps re-injecting** — after 5 min without an inject the lock self-releases with a log line, because a forgotten Test Value used to pin a gauge dead forever, "on a moving car a safety problem, not just a UX one". Otherwise a signal with no frame for 2 s is marked stale and subscribers are notified once.

**Value→label maps** live on the *signal*, so every widget rendering it gets labels for free (gear, drive modes, cruise state, lambda bands). Matching is epsilon-based (`|v − entry| < 0.001`), which handles both integer-coded and decimal-coded signals and absorbs `raw*scale+offset` float drift (a bit field of literally 7 decoding to 6.9999998). *(The header comment claiming exact `roundf` equality is stale.)* `signal_format_value` applies the map first, then `%d`/`%.*f` — widget callbacks should use it rather than inline snprintf so maps work uniformly.

### 5.8 Internal signals (`signal_internal.c`)

One **500 ms (2 Hz)** LVGL timer injects on-device metrics. Names must match what ECU presets generate (label → non-alphanumerics to `_` → uppercase). All registered `SIGNAL_SOURCE_INTERNAL` with `can_id = 0`, and pre-registered at start so the `dash_*` canonical channels can resolve even on layouts that don't enumerate them (duplicate registration is a no-op, so reloads are safe).

| signal | unit | value |
|---|---|---|
| `FPS` | fps | frame count ÷ elapsed (counted from the LVGL flush callback) |
| `CPU_PERCENT` | % | `100 − lv_timer_get_idle()` |
| `FREE_HEAP_KB` | kB | internal SRAM free |
| `FREE_PSRAM_KB` | kB | PSRAM free |
| `UPTIME_S` | s | since boot |
| `CHIP_TEMP` | °C | on-die temperature sensor (installed −10..80 range; skipped if install failed) |
| `WIFI_RSSI` | dBm | STA RSSI |
| `ODOMETER` | km | integrated speed (below) |
| `FUEL_SENDER_V` | V | ADC volts, or the calibrated tank value when calibration is enabled |
| `CALCULATED_GEAR` | gear | back-computed (below) |
| `INDICATOR_LEFT` / `INDICATOR_RIGHT` | — | GPIO level, only when wire-input mode owns the pins |

Also internal, registered elsewhere: **`DTC_COUNT`** (dtc_monitor.c) and **`MATH_*`** (channel_math.c). There is **no internal `BATTERY` signal** — battery voltage comes from a CAN preset or OBD2 PID 0x42. Start-up also hides LVGL's built-in perf-monitor label (FPS is exposed as a signal instead) and re-resolves channel bindings so `dash_*` channels wake up.

**Fuel calibration** — piecewise-linear against either a multipoint curve (2–8 points, insertion-sorted ascending by voltage, flat extrapolation outside the ends, zero-Δv guard) or the legacy 2-point line (`empty_v` 0.5 V, `full_v` 3.0 V, `full_value` 100, zero-range guard). Output is clamped to `[0, top]` so a noisy sender can't read negative or above full. Installing points keeps the legacy 2-point view in sync with the curve endpoints so older readers still get a sensible summary; switching back to 2-point drops the curve. When enabled, the calibrated value is injected as `FUEL_SENDER_V` itself (raw volts remain available via the API), so one signal is all a layout needs.

**`CALCULATED_GEAR`** — requires enabled + ≥2 ratios + wheel circumference and final drive > 0.01:
```
wheel_rps = (speed_kmh × 1000/3600) / wheel_circumference_m
overall   = (rpm / 60) / wheel_rps
gearbox   = overall / final_drive
gear      = index of the closest configured ratio (index 0 = N is skipped)
```
Emits **0 (Neutral)** when `speed < 5 km/h` or `rpm < 500`. If either configured source signal doesn't resolve it warns **once** naming the missing side (most common cause: the signal was renamed in the layout but the gear config wasn't updated) and stops updating; when disabled it emits nothing at all. Config persists as an NVS blob (`gear_cal/cfg`).

**`ODOMETER`** — integrates `speed × 0.5 s / 3600` per tick from `gear_cal.speed_signal` (default `VEHICLE_SPEED`, so the user sets the speed source once for both features). Skipped when the signal is absent (logged once) or stale ("a stale signal usually means the engine is off"). **Sanity cap 400 km/h**: any sample at or above it is treated as a decode error or an OEM "no data" sentinel and **not integrated**, warning once — Falcon FG's 0x207 reads 0xFFFF while stationary, which the preset scale turns into 511.99 km/h and would silently add ~8 km/min to the odometer while parked. Persistence flushes on ≥1 km unsaved **or** ≥5 min elapsed with any unsaved distance, so worst-case loss on an abrupt power cut is ~1 km at highway speed; a failed NVS write keeps the unsaved accumulator and retries. The value is injected every tick regardless of save timing. A manual set (`/api/odometer`) clamps non-finite/negative to 0, **commits to NVS immediately** (fitters commonly type in the existing reading and walk away), and publishes at once so open UI updates without waiting for the next tick.

### 5.9 Signal simulator (`signal_sim.c`)

Bench animation for a dash with no car attached. **16 ms timer** (aligned to the LVGL refresh window), **12 s** full min→max→min triangle sweep per signal (was 3 s — "a fast teleport that spiked redraw load and looked unnatural"), ≤128 signals.

**It drives only signals the active layout actually displays.** A `driven[]` map is rebuilt on every start from the widget registry: pass 1 takes precise bounds from gauges that expose a range (meter `min/max`, bar `bar_min/max`, rpm_bar `0..gauge_max`, warning `0..1`, arc `signal_min/max`, pathbar `val_min/max`, shift_light `0..max(flash_threshold, range_max)×1.05`, anim range-or-threshold); pass 2 adds number-only readouts (text, panel) using the **canonical channel display range** (fallback 0–100) without overriding a gauge's bounds. Consequence: undisplayed internal signals (`CALCULATED_GEAR`, `FUEL_SENDER_V`, `DTC_COUNT`, unbound `MATH_*`) are never touched, while placeholder signals with `can_id == 0` on a fresh device **do** animate.

Cost control: phase advances on **wall-clock dt** (capped at 30 ms) so the sweep speed is independent of tick rate; an **adaptive round-robin batch** injects every tick at ≤12 signals (~71 Hz), every 2nd at 13–24 (~35 Hz), every 4th at 25+ (~18 Hz), with phase advancing only inside the inject loop so each inject is exactly one step; and an **emit threshold** skips injection until the value has moved by ≥ range/200 (~one needle pixel) — below that the widget would round to the same integer and repaint for nothing, "the exact cost that made sim laggier than CAN". This mirrors real CAN, which only emits on a decoded value change.

Interaction rules that matter for testing:
- The sim injects via `signal_inject_test_value`, so it **does not set test locks** — real CAN would freely overwrite it.
- **Peak/min tracking is suppressed while the sim runs** (in both inject and external-push paths) so sweeps can't corrupt recorded peaks.
- **Staleness is suspended** entirely while the sim is active.
- **The whole CAN path is suppressed**: queued frames are drained but **not dispatched** while the sim runs — so real bus traffic, `/api/can/inject` test frames, and the OBD2 virtual ECU's synthetic responses are all discarded. Turn the signal simulator off before using any of those.

### 5.10 CAN manager (`main/can/can_manager.c`)

- TWAI on TX GPIO 20 / RX GPIO 19, `TWAI_MODE_NORMAL`. Bitrate index 0–3 = 125k/250k/500k/1M, default 2, NVS `can_config/can_bitrate`. `can_change_bitrate()` is transient RAM-only (stop task → reinstall driver → restart); `can_persist_bitrate()` writes NVS — the OBD2 auto-search changes transiently and persists only a confirmed lock. `can_resume()` reloads the *saved* bitrate.
- **Acceptance filter**: derived from the set of unique 11-bit IDs across all registered signals (classic single-filter code/mask derivation), rebuilt on binding changes. Falls back to ACCEPT_ALL when: OBD2 29-bit mode is on, no signals, any signal has a 29-bit ID, or >64 unique IDs. The filter **unconditionally includes the OBD2 response range 0x7E8–0x7EF** (one-shot diagnostics and the DTC monitor must work even on broadcast-native presets). **Promiscuous mode** (`can_set_promiscuous_mode`) forces ACCEPT_ALL and makes rebuilds no-ops while active — used by the wizard's ECU auto-detect so binding actions can't narrow the filter under an in-flight probe. A dead driver is never "already matching" (filter-wedge guard).
- 29-bit OBD2: `can_set_obd_extended()` is RAM-only (effective on the next filter rebuild); `can_persist_obd_extended()` adds NVS `can_config/obd_ext`.
- **RX architecture**: RX task (prio 7, core 0, 4 KB PSRAM stack) → `xQueueCreate(64, twai_message_t)` (send timeout 0, **drops on full**) → LVGL task drains via `can_process_queued_frames()` under the LVGL mutex.
- **Dispatch/coalescing** (`can_process_queued_frames`, batch ≤32/cycle): frames are coalesced into a 32-slot table keyed on **(id, extd)** — last value wins per id per render cycle. This is deliberate: at 300+ frames/s vs ~30 renders/s, dispatching every intermediate value flooded LVGL's dirty-rect buffer past `LV_INV_BUF_SIZE` and triggered silent full-screen redraws (the periodic 130–170 ms hitch frames). Gauges are pure value-state so last-wins is lossless. But the **OBD2 handler (ISO-TP is sequential!), the per-ID tracker, and the raw CAN logger see every frame** pre-coalesce. Nothing is dispatched while the signal simulator is active (frames drain undecoded).
- **Synthetic injection**: `can_inject_rx_frame(id, extd, data, dlc)` enqueues through the same queue as real frames (full decode path); waits up to 50 ms for space (httpd-safe); basis of `/api/can/inject` and the OBD2 bench sim. Same sim caveat.
- TX: `can_transmit_frame[_ext]()` — thread-safe, short timeout; `_ext` needed for 29-bit OBD2 requests. Used by toggle/button widgets and OBD2.
- Recovery: `can_recover()` (rate-limited 1/5 s) resumes/initiates bus-off recovery/reinstalls; `can_suspend/resume` hand the peripheral to the bus scanner.
- Bit extraction (`can_extract_bits`): endian convention everywhere is **0 = Motorola/big, 1 = Intel/little (default)**; consistent across signal_t, channels, presets, layout JSON, and the decoder.

### 5.11 OBD2 engine (`main/can/obd2.c`, `obd2_pids.c`)

SAE J1979 over ISO 15765-4 CAN. Runs **entirely on the LVGL task** (30 ms poll timer + RX hook from frame dispatch; no dedicated task). Decoded values are pushed into the signal registry **by name** (`signal_set_external_value`), so widgets bind to OBD2 data like any other signal. Two usage modes share one mechanism: primary preset ("OBD2 Standard", the 29-PID starter set) or **supplemental gap-fill** on top of a native ECU preset (a few slow-tier fillers: fuel level, distance-since-clear, ambient, oil temp, fuel rate).

**Key constants:** `OBD2_MAX_ENABLED` 48 · tick 30 ms (~33 req/s; Japanese ECUs throttle <10 ms spacing) · min inter-TX 5 ms (ELM327-STN-style floor) · FAST tier 100 ms, SLOW 1000 ms, dead (no answer 3 s) → 5 s probes · batch ≤6 PIDs/request · ISO-TP buffer 64 B, timeout 500 ms · DTC timeout 2 s + 350 ms multi-ECU merge window · clear/VIN/ECU-name 1.5 s · freeze 600 ms · scan window 250 ms/block.

**Addressing:** broadcast 0x7DF, physical 0x7E0+, responses 0x7E8–0x7EF; 29-bit extended (some 2006–2012 Hondas, some GM/Mazda answer only here): functional request `0x18DB33F1`, responses `0x18DAF1xx`, physical requests `0x18DAxxF1`. The extended flag is RAM-transient during probing, NVS-persisted (`can_config/obd_ext`) on lock.

**`polled_pids` encoding** (layout JSON): `value ≤ 0xFF` = legacy bare Mode 01 byte; `(service<<8)|pid` for 8-bit PIDs (Mode 01/21); `(service<<16)|pid` for 16-bit Mode 22 UDS PIDs. Encoders pick the smallest form (no JSON bloat). This disambiguates e.g. Mode 01 0x21 (DTC distance) vs Toyota Mode 21 0x21 (ATF temp).

**PID definition model:** each def carries pid, signal_name (registry key; NULL for packed defs), human_name, unit, bytes (1/2), scale, offset, tier, default_enabled, suggested_filler, service (0x01/0x21/0x22), category, optional `sub_fields[]` (packed multi-value: per-field signal/unit/byte_offset/bytes/is_signed/scale/offset), optional `request_id` (0 = broadcast; 0x7E0 = engine ECU direct, 0x7E1 = TCM), and **`resp_len`** — the ECU's actual response byte count when it exceeds the decode span. `resp_len` is load-bearing: the multi-PID batch walker advances by `1 + resp_len` per PID; a wrong value desyncs the walk and garbles every following PID in the batch. The bench simulator emits the same spans.

**Scheduling:** dispatch fires from the timer **and after every response** ("pending response" mode, like STN-chipset ELM327s — the bus runs at the ECU's natural rate, typically 30–50 Hz round-trips). Most-starved-PID-first. Mode 01 single-value alive PIDs batch up to 6/request (dead PIDs excluded — some ECUs NRC a whole batch over one unknown PID; packed PIDs always solo; non-01 services always solo). Batch health: two independent session latches disable batching (2 NRC strikes while batched / 4 alive→dead flap strikes on batched PIDs), both permanently suppressed once any batch ever decodes ≥2 PIDs (proves the car can, so a flaky secondary ECU can't latch them off). On disable, all PIDs re-probe immediately.

**Gap-fill conflict guard:** at `obd2_start`, a PID whose `signal_name` already resolves to a registry signal with `can_id != 0` is skipped — the native CAN preset owns it (RPM stays on 100 Hz CAN while fuel polls at 1 Hz OBD2). Not applied to packed PIDs (their sub-fields can overwrite preset-owned names — why Toyota Mode 21 0x80, whose sub-fields reuse canonical Mode 01 names, ships default-disabled). Poll freshness state is carried across restarts for unchanged PIDs.

**Framing:** single frames DLC 8 padded 0x55. Mode 01/21: `[0x02, service, pid]`; Mode 22: `[0x03, 0x22, pid_hi, pid_lo]`; multi-PID: `[n+1, 0x01, pids…]`; DTC modes `[0x01, mode]`; freeze `[0x02, 0x02, data_pid, frame_no]`. Flow control `[0x30, 0x00, 0x00]` (no block limit, no separation). Full ISO-TP reassembly: single 64-byte stream, FF/CF sequence checking, 500 ms abandon timer, DLC clamped ≤8, 1-byte SFs allowed (bare `0x44` clear acks).

**Response processing highlights:** per-service freshness stamps (drives the protocol-liveness dots; NRCs deliberately don't count as "fresh"); a DTC-mode NRC from one ECU doesn't fail the read (another module may still answer — merge window closes on quiet); Mode 09 NRCs carry no PID echo, so concurrent VIN+ECU-name requests are never cross-cancelled; VIN/ECU-name strip leading padding before applying their 17/23-byte windows and handle both with/without the data-identifier count byte. The Mode 01 batch walker stops (never guesses) on: unknown PID, zero span, short payload (decodes what arrived, then stops — real ECUs emit fewer packed slots than spec), or trailing bytes that aren't polled PIDs (a def understating `resp_len`).

**Multi-ECU DTC merge:** Modes 03/07/0A go to 0x7DF and *all* ECUs answer; results are merged over a 350 ms quiet window (2 s absolute), deduped (two modules legitimately report the same U-code), ≤16 codes. J2012 decode: 2 bits category P/C/B/U + digit + 3 hex. Count-byte heuristic handles both J1979 revisions. Mode 04 clears stored+pending only (permanent self-clears after ECU self-tests); typical rejection is NRC 0x22 (needs engine off/ignition on). A background **DTC monitor** registers internal signal `DTC_COUNT` and polls Mode 03 every 30 s (5 min after 3 misses, auto-recovering; last good count held while offline) — a warning widget bound `DTC_COUNT > 0` lights on any stored fault with zero PID config. Description DB: generic SAE Powertrain P0xxx only (≤~50-char rows); manufacturer ranges intentionally excluded.

**Other one-shots:** VIN (Mode 09/02) and ECU name (09/0A); freeze frames (Mode 02, raw per-PID reads mirroring Mode 01 layout — the tap-a-DTC conditions view); a one-shot **test** request (powers the custom-PID Test button, decodes with caller-supplied params, coexists with polling). **Discovery scan:** Mode 01 PID 0x00 support-bitmask walk (blocks 0x00→0xC0, 250 ms multi-ECU collection window each, ≤128 PIDs, ~2 s total). **Auto-search** (scoped ATSP0): probes {current rate, other rate} × {0x7DF, 0x7E0, 29-bit functional} until something answers Mode 01 0x00, then locks + persists bitrate and addressing; full state restoration on failure or mid-scan stop.

**Virtual ECU (bench sim, `/api/obd2/sim`):** `_obd2_tx` is the single TX choke point; when enabled, outgoing requests are intercepted and synthetic responses injected via `can_inject_rx_frame` — the **exact** real decode path end-to-end. Values are time-varying triangle sweeps; supported-PID bitmasks are answered (discovery finds the simulated car). Simulated: Modes 01 (incl. small packed), 02, 03/07/0A, 04, 09; not simulated: 21/22 (fall through, time out). Default off, not persisted. Gotchas: the **signal simulator drains injected frames** (must be off), and enabling with an empty poll list transiently starts the default PID set so gauges animate.

**Custom PIDs:** ≤32 user entries in layout JSON `custom_pids[]`: `{signal_name*, pid*, service (0x01), data_offset (0), data_bytes (1|2|4), scale (1.0), offset (0.0), is_signed (false), request_id (0=broadcast), unit (""), label (=signal_name), category, tier ("slow"|"fast" — only exact "fast" is fast)}`. Reset + re-registered at every layout load, before `obd2_start` so `polled_pids` referencing them resolve. Each becomes a single-sub-field packed def — identical treatment to built-ins in polling/picker/binding.

#### Built-in Mode 01 PID table (63 entries; 29 default-enabled ✔; fillers ★)

| PID | signal_name | name | unit | bytes | scale | offset | tier | dflt | notes |
|---|---|---|---|---|---|---|---|---|---|
| 0x04 | ENGINE_LOAD | Calculated Engine Load | % | 1 | 0.392157 | 0 | FAST | ✔ | |
| 0x05 | COOLANT_TEMP | Coolant Temperature | degC | 1 | 1.0 | −40 | SLOW | ✔ | |
| 0x06 | SHORT_FUEL_TRIM_1 | Short Fuel Trim B1 | % | 1 | 0.78125 | −100 | SLOW | ✔ | |
| 0x07 | LONG_FUEL_TRIM_1 | Long Fuel Trim B1 | % | 1 | 0.78125 | −100 | SLOW | ✔ | |
| 0x08/0x09 | SHORT/LONG_FUEL_TRIM_2 | Fuel Trim B2 | % | 1 | 0.78125 | −100 | SLOW | | |
| 0x0A | FUEL_PRESSURE | Fuel Pressure (abs) | kPa | 1 | 3.0 | 0 | SLOW | ✔ | |
| 0x0B | MAP | Intake Manifold Pressure | kPa | 1 | 1.0 | 0 | FAST | ✔ | |
| 0x0C | RPM | Engine RPM | rpm | 2 | 0.25 | 0 | FAST | ✔ | |
| 0x0D | VEHICLE_SPEED | Vehicle Speed | km/h | 1 | 1.0 | 0 | FAST | ✔ | |
| 0x0E | TIMING_ADVANCE | Timing Advance | deg | 1 | 0.5 | −64 | FAST | ✔ | |
| 0x0F | INTAKE_AIR_TEMP | Intake Air Temperature | degC | 1 | 1.0 | −40 | SLOW | ✔ | |
| 0x10 | MAF | Mass Airflow Rate | g/s | 2 | 0.01 | 0 | FAST | ✔ | |
| 0x11 | THROTTLE_ABS | Throttle Sensor (raw) | % | 1 | 0.392157 | 0 | FAST | ✗ | raw TPS, ~12–15% at idle — diagnostics only |
| 0x14 | O2_B1S1 | O2 Sensor B1S1 Voltage | V | 1 | 0.005 | 0 | SLOW | ✔ | resp_len **2** (A=V, B=trim) — mandatory for batch alignment |
| 0x1F | RUN_TIME | Run Time Since Engine On | s | 2 | 1.0 | 0 | SLOW | ✔ | |
| 0x21 | DTC_DISTANCE | Distance with MIL on | km | 2 | 1.0 | 0 | SLOW | ✔ | distinct from Toyota Mode 21 0x21 |
| 0x22 | FUEL_RAIL_REL_PRESSURE | Fuel Rail Pressure (rel) | kPa | 2 | 0.079 | 0 | SLOW | | |
| 0x23 | FUEL_RAIL_PRESSURE | Fuel Rail Pressure | kPa | 2 | 10.0 | 0 | SLOW | ✔ | |
| 0x2C | EGR_CMD | Commanded EGR | % | 1 | 0.392157 | 0 | SLOW | ✔ | |
| 0x2D | EGR_ERROR | EGR Error | % | 1 | 0.78125 | −100 | SLOW | ✗ | |
| 0x2F | FUEL_LEVEL | Fuel Level | % | 1 | 0.392157 | 0 | SLOW | ✔★ | |
| 0x30 | WARMUPS_SINCE_CLEAR | Warmups Since Cleared | count | 1 | 1.0 | 0 | SLOW | | |
| 0x31 | DISTANCE_SINCE_CLEAR | Distance Since Cleared | km | 2 | 1.0 | 0 | SLOW | ✔★ | |
| 0x32 | EVAP_VAPOR_PRESSURE | Evap Vapor Pressure | Pa | 2 | 0.25 | 0 | SLOW | | packed **signed** decode (two's-complement /4 Pa) |
| 0x33 | BAROMETRIC_PRESSURE | Barometric Pressure | kPa | 1 | 1.0 | 0 | SLOW | ✔ | |
| 0x34 | O2S1_LAMBDA | O2 Sensor 1 Lambda | λ | 2 | 0.0000305 | 0 | SLOW | | resp_len **4** (wide-range O2: AB=λ, CD=current) |
| 0x42 | BATTERY_VOLTAGE | Control Module Voltage | V | 2 | 0.001 | 0 | SLOW | ✔ | real volts |
| 0x43 | ABSOLUTE_LOAD | Absolute Engine Load | % | 2 | 0.392157 | 0 | FAST | ✔ | |
| 0x44 | LAMBDA | Commanded Equivalence Ratio | λ | 2 | 0.0000305 | 0 | FAST | ✔ | direct lambda 0–2 |
| 0x45 | THROTTLE | Throttle | % | 1 | 0.392157 | 0 | FAST | ✔ | **calibrated** relative throttle — owns the canonical THROTTLE name |
| 0x46 | AMBIENT_TEMP | Ambient Air Temperature | degC | 1 | 1.0 | −40 | SLOW | ✔★ | |
| 0x47/0x48 | ABS_THROTTLE_B/C | Absolute Throttle B/C | % | 1 | 0.392157 | 0 | SLOW | | |
| 0x49–0x4B | ACCEL_PEDAL_D/E/F | Accelerator Pedal D/E/F | % | 1 | 0.392157 | 0 | SLOW | | |
| 0x4C | THROTTLE_CMD | Commanded Throttle Actuator | % | 1 | 0.392157 | 0 | FAST | ✔ | |
| 0x4D/0x4E | MIL_ON_TIME / RUN_TIME_SINCE_CLEAR | MIL time / Run time since clear | min | 2 | 1.0 | 0 | SLOW | | |
| 0x51 | FUEL_TYPE | Fuel Type (enum) | | 1 | 1.0 | 0 | SLOW | | |
| 0x52 | ETHANOL_PCT | Ethanol Fuel % | % | 1 | 0.392157 | 0 | SLOW | ✔ | |
| 0x5A | PEDAL_POSITION | Accelerator Pedal Position | % | 1 | 0.392157 | 0 | FAST | ✔ | |
| 0x5B | HYBRID_BATTERY_PCT | Hybrid Battery Pack % | % | 1 | 0.392157 | 0 | SLOW | | |
| 0x5C | OIL_TEMP | Engine Oil Temperature | degC | 1 | 1.0 | −40 | SLOW | ✔★ | |
| 0x5D | FUEL_INJECTION_TIMING | Fuel Injection Timing | deg | 2 | 0.0078125 | −210 | SLOW | | |
| 0x5E | FUEL_RATE | Engine Fuel Rate | L/h | 2 | 0.05 | 0 | SLOW | ✔★ | |
| 0x61/0x62 | ENGINE_TORQUE_DEMAND/ACT | Torque demand/actual | % | 1 | 1.0 | −125 | SLOW | | |
| 0x63 | ENGINE_REF_TORQUE | Engine Reference Torque | Nm | 2 | 1.0 | 0 | SLOW | | |
| 0x01 | MIL_DTC_STATUS | MIL + DTC Count (raw) | | 1 | 1.0 | 0 | SLOW | | resp_len 4; byte A = MIL bit7 + count bits0–6 (widget rule: `>=128` → MIL) |
| 0x03 | FUEL_SYSTEM_STATUS | Fuel System Status | | 1 | 1.0 | 0 | SLOW | | resp_len 2 |
| 0x1C | OBD_STANDARD | OBD Standard (enum) | | 1 | 1.0 | 0 | SLOW | | |

Diesel J1979-DA packed PIDs (byte offsets skip a leading support byte; unsupported slots read as the floor, e.g. −40 °C): **0x77** Charge Air Cooler Temp (`CHARGE_AIR_TEMP`, resp_len 5); **0x78** EGT Bank 1 (`EGT_1..4`, °C, (A·256+B)/10 − 40, FAST, span 9 — typically pre-turbo/post-turbo/pre-DPF/post-DPF); **0x7A** DPF Differential Pressure (`DPF_DELTA_P`, kPa, signed, ×0.01 — clean ~0, regen-due 5–20). Verified on a 2024 Toyota HiAce 1GD-FTV. Tier-3 **experimental** diesel decodes (spec-ambiguous, first slot only): **0x70** `BOOST_PRESSURE_ABS` (kPa ×0.03125, resp_len 10), **0x6D** `RAIL_PRESSURE_CMD` (kPa ×10), **0x71** `VGT_POSITION` (% — some manufacturers invert the convention).

Toyota Mode 21 (service 0x21, all default-disabled, experimental): **0x80** engine block — one ISO-TP message to 0x7E0 packing RPM (×0.25 @2), THROTTLE (@4), VEHICLE_SPEED (@5), COOLANT_TEMP (@6), INTAKE_AIR_TEMP (@7), MAF (×0.01 @8), BATTERY_VOLTAGE (×0.1 @12) — sub-fields deliberately reuse canonical names (enable it **or** Mode 01, not both; last-write-wins); **0x21** `ATF_TEMP` (−40 offset, addressed 0x7E1/TCM); **0xA8** `KNOCK_RETARD` (0.5°/count); **0xC1** `PEDAL_RAW` (0.5%/count).

### 5.12 Wire inputs (`main/io/wire_inputs.c`)

- Indicator wires: GPIO 43 (left) / 44 (right), input + pull-down, active-high, 10 ms settle. Gated by NVS `wire_input/enabled` (default **false** — UART1 owns the pads; a physical switch selects the electrical routing; reboot required either way). Task `ind_wire` (2 KB, prio 3, core 0) polls at 20 Hz and forwards via `indicator_apply_analog_state()` under a 20 ms lock try; it self-deletes when wire mode is off. `indicator_apply_analog_state` internally skips CAN-sourced indicators.
- The same GPIOs are independently published as registry signals `INDICATOR_LEFT`/`INDICATOR_RIGHT` at 2 Hz by the internal-signal timer — bindable to any widget/channel like any signal.
- Fuel sender: GPIO 6 / ADC1 ch5, sampled every 500 ms, published as `FUEL_SENDER_V` (raw volts, or calibrated tank value when calibration is enabled).
- The brightness dimmer binds to a **signal by name** (NVS `dimmer_cfg`), so it can be driven from CAN, OBD2, or any internal signal — it owns no GPIO.

### 5.13 Channel source binding (`main/data/channel_source_apply.c`) — caller must hold the LVGL mutex

Three explicit paths by which a channel acquires a source (OBD2 binding is a fourth, implicit — the channel binds to an OBD2-registered name and carries no decode):

1. **Preconfig apply** (`channel_apply_preconfig`) — the single shared path used by both `/api/channels/bind-source` and the first-run wizard (previously duplicated, drifted). Signal name is derived from the label (uppercase alphanumerics, other runs → `_`). Order is load-bearing: (a) patch/register the runtime signal **first** (with a freshness reset so the first identical value isn't swallowed by the change-gate); (b) persist into the active layout's `signals[]` (harmless local cache); (c) `channel_manager_set_signal` + `set_decode` (the channel copy is authoritative and survives Phase-B stripping); (d) re-resolve; (e) **`reconfigure_can_filter()`** unless promiscuous — without this a newly bound CAN ID can be dropped *in hardware* and the channel never updates.
2. **Studio "full config" layout import** (`channel_import_from_layout_signals`, triggered by `/api/layout/save` when `signals[]` entries carry thresholds or decode — the signature of an off-device export, since the on-device editor strips those keys). Bulk-wrapped (one channels.json flush). Per entry: find the channel bound to that signal (case-insensitive) or create one (canonical mapping → collision-guarded → `custom_<name>`); **decode adopted only when the channel has none** (never clobbers device edits); **thresholds always applied** (import = explicit intent) and then **stripped from the layout JSON** so they can't re-assert over later on-device edits; CAN filter reconfigured if any decode was adopted.
3. **Custom bind** (`bind-source` type `custom`) — binds an existing registry signal; CAN-sourced decode is copied onto the channel + written into the layout for boot durability (without this, a DBC-import bind stored only the name and died at reboot).

### 5.14 Threading model (consolidated)

| Context | Owns |
|---|---|
| CAN RX task (prio 7, core 0, PSRAM stack) | `twai_receive` → 64-deep queue. Never touches LVGL. |
| **LVGL task (core 1, prio 8)** | Everything else: frame drain/decode/dispatch, all `signal_*` and `channel_manager_*` APIs, OBD2 entirely (timer + RX hook), channel math (5 Hz timer), internal signals (2 Hz), signal simulator (16 ms), DTC monitor, data logger (LVGL timer), replay (50 Hz), widget/rule/night callbacks. Signal callbacks run here — direct `lv_*` calls are safe. |
| wire-inputs task (prio 3, core 0) | 20 Hz GPIO poll under a 20 ms lock try. |
| httpd task (core 0, prio default) | All HTTP; marshals under `rdm_lvgl_lock` or defers via `rdm_async_call`; `can_inject_rx_frame` is the thread-safe injection door (50 ms queue wait). |
| esp_timer task | channels.json debounced save (defers + re-arms on lock contention). |

The LVGL mutex is recursive. `rdm_async_call` (never raw `lv_async_call`) is the mandated cross-thread door — raw calls from ~60 sites once corrupted the LVGL timer list (the intermittent "CORRUPT HEAP" class).
---

## 6. Storage & persistence reference

### 6.1 LittleFS layout (`/lfs`, ~8.8 MB)

| Path | Contents |
|---|---|
| `/lfs/layouts/*.json` | Layouts; `default.json` seeded/regenerated; splash layouts as `_splash_<name>.json` (`_splash_Default.json` seeded; legacy `_splash.json` migrated) |
| `/lfs/images/*.rdmimg` | Images (12-byte RDMIMG header + RGB565+alpha). `RDM.rdmimg` (logo) seeded and protected |
| `/lfs/fonts/*.ttf` | TTF fonts; `Montserrat.ttf`, `Fugaz One.ttf`, `Manrope Bold.ttf` seeded and protected |
| `/lfs/channels.json` | The channel registry (device-local, NOT part of a portable layout) |
| `/lfs/presets/*.json` | Custom ECU presets (DBC imports etc.) |
| `/lfs/dbc/` | User signal defs |
| `/lfs/logs/*.csv` | Logger fallback tier (1 MB/file cap) |

Boot-asset seeding runs every boot (`_write_if_missing` + partial-write cleanup), so factory reset is self-healing. Factory reset = full NVS erase + LittleFS format + reboot.

SD card (`/sdcard`): `layouts/`, `images/`, `fonts/` auto-created; `logs/`, `diagnostics/` lazily. FAT, LFN 63.

### 6.2 NVS namespaces/keys (authoritative; the handover doc's table is stale)

| Namespace | Keys | Meaning |
|---|---|---|
| `can_config` | `can_bitrate` (u8, default 2=500k), `obd_ext` (u8) | CAN bitrate index; OBD2 29-bit addressing |
| `wifi_cfg` | `ssid`, `password`, `auto_con` (legacy/slot-0 mirror); `list_count`, `ssid0..4`, `pw0..4`; `on_boot`, `ap_en` | STA credentials (5 slots) + boot behaviour |
| `wifi_ap_cfg` | `enabled` (ignored — `wifi_cfg/ap_en` wins), `password` | AP password (derived default) |
| `splash_cfg` | `fade`, `enabled`, `bootanim`, `bootanimstyle` (0 fade/1 curtain) | Splash/boot-reveal |
| `dataloggr` | `rate_hz` (u16, default 10, 0=Max) | Logger rate |
| `dimmer_cfg` | `sig_name`, `thresh` (u16 ×100), `is_mom`, `invert`, `dim_br`, `enabled` | Signal-driven dimming |
| `display_cfg` | `rot` (persisted, never applied), `nm_en`, `nm_manual`, `nm_bright` (5–100) | Display + night mode |
| `editor` | `step_px` (1/5/10, default 5) | On-device editor nudge |
| `ecu_cfg` / `ecu_pick` | `make`, `version` / `auto` | ECU preset selection |
| `layswit` | `csv` | Layout switcher cycle list |
| `gear_cal` | `cfg` (blob) | Calculated-gear config |
| `vehicle` | `odo_km` (blob) | Odometer |
| `first_run` | `done` | Wizard dismissed |
| `wire_input` | `enabled` | GPIO 43/44 = indicators (vs UART1) |
| `ota_cfg` | `skip_ver` | Per-version OTA dismissal |
| `wdglatch` | `<canid>_<bit>` | Button/toggle latch persistence |
| `layout_mgr` | `active`, `active_spl` | Active layout / splash (survives reboot) |
| `crash_log` | `last_reason`, `last_fw`, `last_uptime_s`, `panic_count` | Crash record |

**Signal peaks are NOT persisted.** Verified: `signal.c` contains no NVS access at all and no `signal_peaks` namespace exists anywhere in `main/`. All four peak/min fields reset every boot; the "all-time vs session" naming in the API is purely about reset granularity within one boot. Don't promise persistent peak/min records to customers.

---

## 7. RDM Studio (desktop app)

**RDM Studio v0.4.47** (repo `rdm7-desktop`), Tauri 2, bundle id `com.rdm7.designer` (kept from the old "RDM-7 Visual Designer" name so self-update and app data carry over). Window 1400×900 (opens maximized, min 1024×600). `.rdm` file association ("RDM-7 Layout", `application/x-rdm`). Bundles: Windows NSIS + MSI, macOS DMG (arm64 + x64), Linux AppImage/deb/rpm. No package.json/bundler — build is `cargo tauri dev/build`; frontend assembly is Python.

### 7.1 Architecture — the overlay model (ADR-0007)

```
src/firmware-base.html   verbatim byte copy of RDM-7_Dash/main/web/index.html (~24k lines)
+ src/tauri-overlay.html the entire desktop delta: 28 anchored blocks, ~17k lines
= src/dist/index.html    what the webview loads (gitignored, ~41k lines)
```

- `tools/sync_firmware.py` copies the firmware editor byte-for-byte and records provenance (`src/firmware-base.commit`: firmware SHA + date). `tools/merge_overlay.py` applies the overlay blocks — each block is an anchor (a line sequence that must match the base **exactly once**; 0 or 2+ matches fails the merge, which is the drift detector) plus insert-after / insert-before / replace-with content — then injects the app version and copies static assets. Runs as Tauri's beforeDev/beforeBuild command; CI (`frontend-merge-check.yml`) re-runs the merge and syntax-checks every inline script on every push.
- Rules: never hand-edit `src/dist/` or `firmware-base.html`; shared editor features are authored in the firmware repo and re-synced; desktop-only behaviour goes in the overlay, `transport.js`, or the Rust backend.
- Because the base is verbatim, **the full firmware editor feature set ships on desktop** — including custom OBD2 PIDs (the historical "custom PIDs omitted on desktop" note predates ADR-0007 and is no longer true).
- Backend: `src-tauri/src/lib.rs`, 24 Tauri commands; a single global serial connection (`static SERIAL: LazyLock<Mutex<…>>`).

### 7.2 Connection model

`window.RDM` (transport.js) is the single frontend surface. Modes: **local** (offline "This PC"), **wifi** (LAN dash), **hotspot** (dash's own AP at 192.168.4.1), **usb** (serial). Settings persist in `localStorage['rdm7_connection_settings']`; local is persisted like any other mode (an earlier bug yanked deliberate Offline users back to WiFi at launch). WiFi remembers the dash's `serial` so reconnect survives DHCP changes.

- **All HTTP goes through Rust** (CORS bypass): `http_fetch` / `http_fetch_binary` / `http_upload_binary`. The device client does **not follow redirects** — a captive portal answering 303 must surface as a failure, not a bogus "connected" (a real field bug). A `window.fetch` interceptor rewrites the firmware editor's relative `/api/...` calls per mode; binary bodies (font/image uploads) are preserved as bytes (the old JSON.stringify path destroyed every uploaded asset); screenshot/font-data/image-data GETs go through the binary path (string transport mangles bytes).
- **Discovery** (no mDNS): Rust subnet sweep — every /24 on every interface, 128-way concurrency, 400 ms connect / 2.5 s request timeouts, `/api/device/info` probe, dedupe by serial, progress events. Known devices (≤8, by serial) in `localStorage['rdm7_known_devices']`.
- **Auto-reconnect**: 4 s health check (`getDeviceInfo` with 3.5 s timeout; two consecutive misses = down; single-flight), 6 s recovery loop (fast re-probe of the last IP; every 3rd tick a full sweep by serial). Hotspot identity is the address, not the serial.
- **Multi-device attach**: the dash is the PRIMARY connection; other RDM devices (GPS puck, keypad, IO expander — all in development) attach alongside by role on separate pipes. Device type is discriminated by `device_type` in `device.info` (dash firmware predates the field; `serial`/`schema` presence implies dash). Dash-only API calls are short-circuited with truthful empty stubs when the connected device isn't a dash.
- **USB serial**: mirrors the firmware frame protocol (§2.4); interleaved ESP_LOG lines are split out to a **Serial Logs drawer** (1000-line ring, ANSI-stripped, USB only); port classification by VID/PID (UART bridges + native CDC); probe retries once (DTR toggle resets the ESP32); chunked ACKed uploads with per-chunk 10 s / finish 30 s timeouts and abort-on-failure. One serial port at a time (a USB dash and a USB puck are mutually exclusive; dash-on-WiFi + puck-on-USB is the normal pattern). **Note: shipping firmware currently has the USB protocol disabled** (`RDM7_DEBUG_KEEP_CONSOLE=1`), so USB mode works only against dev builds.

### 7.3 Offline mode ("This PC") and the WASM preview

- `LocalTransport` stores layouts/splashes/presets in localStorage (`rdm7_layout_<name>`, active in `rdm7_local_active`) and binary assets in IndexedDB. The local API router explicitly implements 21 endpoints (layout CRUD incl. both rename body shapes, image/font list/data/upload/delete, signals, sim, storage/device info, selftest) and answers everything else `{ok:true}` as a harmless no-op; genuinely device-only ops (OTA, SD) throw honestly. First boot seeds the firmware's real default layout (Haltech-Nexus-style, byte-synced with `default_layout.c`) and the RDM logo.
- **WASM preview engine** (`rdm7-wasm-editor` repo): the real firmware widget C code + LVGL 8.3 compiled with Emscripten (SDL2→WebGL, 800×480), driven via ccalls `load_layout_json`, `load_channels_json`, `inject_signal`, `register_image`, `register_font`, `set_display_size`. Pixel-accurate offline rendering; the editor's SVG approximation layer yields whenever the WASM backdrop is active. Canvas CSS size is pinned to defeat devicePixelRatio scaling. Assets and the channel registry are synced into the module (anim frames out-of-band to avoid minute-long stalls).
- **Three-plus-one simulation systems**: (1) the firmware's own device sim (toggled via `/api/signal/simulate`); (2) the desktop **WASM bench sim** — offline only, 12.5 Hz tri-wave sweep across each widget's bound range (~10 s period), reads the binding from top-level `w.signal` (not just config); (3) the editor's SVG approximation sim (yields to WASM); plus per-channel **test sliders** (a desktop panel enumerating every bound channel with a range slider driving `setTestValue` + `injectWasmSignal`) that override everything. When connected, a 5 Hz **live mirror** polls `/api/signals/values` and injects into the WASM canvas — skipping `seen:false` signals (never-received stays `--`) but mirroring stale values (parity with the dash, which keeps painting last values).

### 7.4 Desktop-only features

- **Native file dialogs** replace `<input type="file">` where ported — WebView2 opens file inputs in a nested modal loop that freezes the whole app, often with the dialog hidden behind the maximized window. Ported: layout JSON export, `.rdm` export/import, animation upload (delegated capture-phase click hook), ZIP backup/restore. **Not ported (still freeze the app on Windows): image upload, font upload, DBC import, log CSV upload, custom-PID import, pathbar trace image.**
- **Known bug:** `.rdm` file association / CLI open / drag-drop is currently dead under Tauri — the `file-opened` handler feeds a file input whose `onchange` is never registered on the Tauri path (menu File → Open works).
- **Native menu**: File (New/Open .rdm/Save/Save As/Delete/Export JSON/Export .rdm/Backup All/Restore), Edit (undo/redo + OS clipboard), View (Home, Fit, Reset, Toggle Simulator, Control Device Screen), Device (Connect over WiFi, Scan, GPS & Lap Timing, CAN Keypad, IO Expander, CAN Bus Analyzer, Transfer Layouts, Device Manager, Boot Animation submenu, Go Offline), Help (Shortcuts, Check for Updates, About).
- **Suite shell** (~14k lines of the overlay): a splash + Home launcher with product cards → workspaces. The **Dash editor** is the hero card. **CAN Bus Analyzer** works today against the dash (dash = WiFi↔CAN gateway, no dongle): live ID table with change-flash, bus-load estimate, dim-idle, snapshot "Mark" byte-diffing, isolate/hide per ID, bit-probe (drag a run of bits, watch it plot), and **Match OBD** — correlates candidate CAN fields against live OBD2 readings (from `/api/signals/values` where `source=="obd2"`) to find which raw bits carry a value, so it can be read at 100 Hz off raw CAN instead of OBD2 polling; it detects the firmware's virtual ECU and offers one-click disable (matching against synthetic data correlates the dash with itself). CSV export. The **Keypad Configurator** and **GPS & Lap Timing** workspaces target in-development hardware (see scope note); the **IO Expander** workspace likewise targets an unbuilt product (design mode, saves locally).
- **Device Manager** modal: identity/connection/storage/brightness slider/system health (5 s refresh)/reboot.
- **Test signals panel**: the per-channel slider rack over the editor (above).
- **Save/Revert model**: edits stream to the dash live and auto-persist (~1 s debounce), so the old Save/Apply button is replaced by a saved/edited pill + **Save** (creates a restore point via a full apply-save) + **Revert** (confirm-guarded restore of the last save point, history reset). Meter drawn-needles are auto-**baked** to crisp rotating images on deliberate Save only (injected into the outgoing payload; the editable drawn needle stays).
- **Fast layout switch**: `POST /api/layout/set` is fired without awaiting (the firmware rebuild blocks ~0.15–1.8 s) while `GET /api/layout/raw?name=` is read in parallel — preview renders in ~30 ms; falls back to the ordered path on old firmware. Side effect: a failing device-side switch is silent when `/raw` succeeds.
- **Backup/restore**: all device layouts ⇄ a `.rdmbackup` ZIP (Rust zip/unzip; overwrite-confirmed restore).
- **Transfer Layouts** modal: This PC ⇄ Device, both stores visible at once, copies referenced images/fonts along with the layout.
- **Updater — two independent systems, one banner**: desktop self-update via tauri-plugin-updater (GitHub `Trizzoto/rdm7-desktop` latest, minisign-verified `latest.json`, passive Windows install, checked ~3 s after launch, per-version skip) and firmware update (GitHub `Trizzoto/potato-jubilee`; over USB it downloads the .bin and pushes it via the chunked `'ota'` upload; the setup card routes to the firmware's own WiFi OTA flow instead). Release flow: bump version in `tauri.conf.json`, tag `v*`, GitHub Actions matrix builds serially (parallel jobs raced on `latest.json`). Windows Authenticode signing is not yet done (SmartScreen "unknown publisher").

### 7.5 Notable internal quirks

- Offline storage meter is decorative: two disagreeing budgets (5 MiB vs 8 MiB) and a hard-coded `totalBytes: 0` in one path.
- `importLocalToDevice()` setup card is a stub (toast only); the real path is Transfer Layouts.
- USB HTTP-bridge translates the sim contract (HTTP `{enabled}` ⇄ serial `{enable}`/`{active}`) — an earlier mismatch made USB always turn the sim off.
- Many transport helpers swallow errors by design (return `[]`/`null`/`false`) — debugging connection issues needs the console, not return values.

## 8. Appendix: known documentation drift (repo docs that contradict the code)

For the guide author: trust this document / the code over these sources.

1. `RDM-7_User_Guide.md` is dated to firmware v1.1.1 (current 1.1.30); says the first-run wizard has 3 steps (it has 4); claims 70 Hz refresh (target, not measurement).
2. `docs/handover/05-storage-and-persistence.md` NVS table names several wrong namespaces/keys and shows layout schema v15 (current v17).
3. `docs/handover/08-aux-systems.md` says UART protocol baud 115200 (actual 921600); says the logger errors without SD (it now falls back to LittleFS); describes night mode as live (currently kill-switched).
4. ADR-0001 describes DNS-hijack behaviour (PSRAM stack, live AP-IP lookup, AAAA NOERROR) the code doesn't implement (internal-RAM stack, hardcoded 192.168.4.1, A-answer for every qtype).
5. `web_server.c` comment claims 148 registered URIs (actual 138 after the lap-endpoint deletion).
6. `canonical_channels.c` header says "92 channels across 12 groups" (actual 135 / 14); obd2 headers say "30 starter PIDs" (actual 29 default-enabled).
7. `rdm7-desktop/CLAUDE.md`: says ~18 Tauri commands (24), ~22k merged lines (~41k), mentions mDNS (removed).
8. CLAUDE.md (firmware) says "LVGL task: core 1, 14 ms tick" — the task targets a ~2 ms loop with a custom tick source and a 16 ms refresh period.
---

