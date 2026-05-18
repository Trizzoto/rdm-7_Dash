# CLAUDE.md

## Project Overview

**RDM-7 Dash** — ESP32-S3 automotive dashboard firmware. ESP-IDF v5.x + **LVGL v8** (NOT v9).
800×480 RGB LCD, CAN bus via signal pub/sub, JSON layouts on LittleFS, web editor over WiFi.

Part of the **RDM project**: this firmware + RDM Desktop Studio (Tauri) + RDM Web Studio + RDM Marketplace. They share layout JSON format, API contracts, and asset pipelines.

## Build

- ESP-IDF v5.3.1 at `C:\Espressif\frameworks\esp-idf-v5.3.1`. **Each shell call is fresh** — prefix every `idf.py` invocation with `. "$env:IDF_PATH\export.ps1" *> $null;` to load the toolchain. After that, run `idf.py build` / `idf.py -p COM13 flash monitor` directly.
- Dev port: **COM13** (the user's dash). Flash + monitor are pre-approved; only `erase_flash` or OTA still asks first.
- Long builds belong in `run_in_background` so the chat stays responsive. Monitor streams via the Monitor tool — kill it with TaskStop when done.
- All `.c` files must be listed in `main/CMakeLists.txt` SRCS
- `main/web/index.html` embedded via `EMBED_TXTFILES`
- `-Werror=comment` active — no `/*` inside block comments
- Partition: dual OTA (3.5 MB each) + LittleFS (~8.8 MB) on 16 MB flash

## Source Layout

```
main/
├── can/          CAN RX task + frame dispatch
├── io/           Wire inputs (GPIO/ADC)
├── layout/       layout_manager, layout_loader, default_layout, ecu_presets
├── net/          wifi_manager, web_server + modular endpoint files, dns_hijack, ota
├── storage/      config_store (NVS), data_logger, sd_manager, signal_replay, boot_assets
├── system/       display_capture, night_mode, remote_touch, screen_config, device_id
├── ui/           dashboard, config_modal, screens/, settings/, callbacks/, components/
├── web/          index.html (embedded), logo
└── widgets/      13 widget types + signal, font_manager, widget_rules, widget_registry
schema/           widgets.schema.json + codegen metadata
tools/            codegen_widget_defs.py, check_*.py, png_to_rdmimg.py, mobile-dev-server.js
tests/api/        pytest API contract suite
tests/native/     Unity C unit tests (CAN decode, layout migration, widget rules)
```

## Threading (Critical)

- **LVGL is single-threaded.** All `lv_*` calls require LVGL mutex (`rdm_lvgl_lock`). LVGL task: core 1, 14 ms tick.
- CAN RX task: core 0 → `s_can_queue` → `can_process_queued_frames()` (LVGL task).
- Signal callbacks run on LVGL task — safe for direct `lv_*` calls.
- Use `lv_async_call()` from any other context (web server, etc.).

## Coordinate System

- **(0,0) = screen center.** Use `SCREEN_W/H/ORIGIN_X/ORIGIN_Y` from `screen_config.h` — never hardcode 800/480.
- Always `lv_obj_set_align(obj, LV_ALIGN_CENTER)` **before** `lv_obj_set_pos()`.
- Web editor converts via `devToWeb()`/`webToDev()`.

## Widget System

13 types in `widget_type_t` (`widget_types.h`). Slot limits (per `SLOT_LIMITS` in `main/web/index.html` and firmware-side caps):

| Type | Web cap | Firmware cap | Notes |
|---|---|---|---|
| `panel` | unlimited | none | Slots 8+ skip auto-positioning |
| `bar` | unlimited | none | |
| `indicator` | 2 | `slot >= 2` | Hard cap, firmware drops higher |
| `warning` | 8 | `slot >= 8` | Hard cap |
| others | not slotted | — | Position-based, not slot-based |

**Lifecycle:** factory → `from_json()` → `create()` → signal callbacks → `to_json()` → `destroy()`

- `from_json`: resolve signal name→index via `signal_find_by_name()`, call `widget_rules_from_json()`
- `create`: build LVGL objects, subscribe signals **after** `w->root` is set
- `to_json`: **defaults-only** — omit fields matching factory defaults (32 KB JSON budget per `RDM_LAYOUT_MAX_BYTES`)
- `destroy`: `widget_rules_free()`, free `type_data`
- Per-instance styles via `lv_obj_set_style_*()` — not shared `lv_style_t`
- Conditional rules: `widget_rules.c/h` — requires `apply_overrides` vtable + `widget_rules_subscribe()` after create
- **Night mode**: per-widget `night_overrides_t` in header; helpers in `widget_night_helpers.h`. Add `apply_night_mode` vtable + `night_mode_subscribe()` after create. Use **dual-object pattern** for LVGL v8 baked-in properties (image source, needle color, tick colors) — hidden sibling built at create, visibility toggled on apply. See widget_image, widget_meter, widget_warning.

## Signal System

```
CAN RX (core 0) → s_can_queue → can_process_queued_frames() (LVGL task)
  → signal_dispatch_frame() → can_extract_bits() → scale/offset → notify subscribers
```

Registry in `widgets/signal.c/h`. Signals defined in layout JSON, registered at load. Stale after 2 s.
Internal signals (GPIO/ADC): `signal_internal.c`. Simulator: `signal_sim.c`.

## Layout Manager (`main/layout/`)

- `layout_manager.c/h` — load/save layouts, register widget factories, drive `dashboard_init()`
- `layout_loader.c/h` — JSON parse + widget instantiation
- `default_layout.c/h` — built-in fallback layout
- `ecu_presets.c/h` — OEM CAN signal presets (8 ECUs)
- Schema version: `LAYOUT_SCHEMA_VERSION` in `layout_manager.h` (currently **v13**)
- Hot-reload path: `POST /api/layout/save` → LittleFS → `lv_async_call()` → `dashboard_init()`

## Storage

| Data | Location | API |
|---|---|---|
| Layouts + signals | LittleFS `/lfs/layouts/` | `layout_manager_save/load()` |
| Images | LittleFS `/lfs/images/*.rdmimg` | 12-byte header + RGB565+alpha |
| Fonts (TTF) | LittleFS `/lfs/fonts/` | `font_manager_get(family, size)` |
| Settings | NVS | `config_store_*()` |
| SD card | FAT `/sdcard/` | layouts/images/fonts subdirs |
| Data logs | SD `/sdcard/logs/*.csv` | `data_logger_*()` |

All CAN signal config lives in layout JSON — not NVS.

## Color Conversion

Firmware: **RGB565**. Web editor: **RGB888**. Use `rgb565to888()` on load, `rgb888to565()` on save. `convertWidgetColors()` walks `WIDGET_DEFS` field metadata.

## Web Editor

- `WIDGET_DEFS` in `index.html` — widget metadata + `fields[]` per type
- `buildFirmwarePayload()` maps `w.signal` → `config.signal_name`, converts colors
- **`main/web/index.html` is the single source of truth.** Embedded in firmware
  via EMBED_TXTFILES, also served directly by `tools/mobile-dev-server.js` for
  browser-based dev without a device.
- The Tauri desktop copy at `../rdm7-desktop/src/index.html` (separate repo) has
  its own delta (USB transport, Tauri wrapper, auto-updater, ~1300 lines of
  edits) — keep that in sync manually when web-editor changes need to land
  there too. Custom PIDs feature lives only in firmware copy currently; the
  desktop OBD2 menu intentionally omits that item until the modal is ported.

## Fonts

Dynamic TTF via `lv_tiny_ttf`. Cache: 8 families (PSRAM) + 32 size instances.
JSON: `"Family:size"` (e.g. `"Fugaz:28"`) or legacy `"fugaz_28"`. Call `font_manager_reset_instances()` on layout reload.

## Adding a New Widget

1. Create `widget_X.c/h` — define `X_data_t` **in the header**
2. Implement vtable: `create`, `resize`, `open_settings`, `to_json`, `from_json`, `destroy` (+ `apply_overrides`)
3. Add factory `widget_X_create_instance(uint8_t slot)`
4. Register in: `widget_type_t` enum, `widget_constraints[]`, `widget_type_name()`, `_type_from_str()`, `_factory()` in `layout_manager.c`
5. Add `.c` to `main/CMakeLists.txt` SRCS
6. Add to `WIDGET_DEFS` in **both** `index.html` copies (firmware `main/web/`, desktop `../rdm7-desktop/src/`)
7. Bump `LAYOUT_SCHEMA_VERSION` if schema changes (current: **v13**)

## Coding Conventions

- C11, `snake_case`, `UPPER_SNAKE_CASE` macros. Module prefix: `widget_panel_`, `signal_`, etc.
- Logging: `ESP_LOGI/W/E/D` with static `TAG`. Headers: `#pragma once` + `extern "C"` guards.
- LVGL v8 API only: `lv_obj_set_style_*()` (not v9 style)
- Large allocs: `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`

## CAN Cloud Upload

Recorded Raw CAN traces can be uploaded to a shared R2 bucket for off-device
debugging via the "Share Raw CAN" button (data logger modal in the web editor).

- Pipeline: web UI → `POST /api/canraw/cloud_upload` → `can_upload_start()` task
  → HMAC-SHA256 over `"{make}\n{model}\n{deviceId}\n{ts}"` → HTTPS POST to
  `tools/cloudflare-ota-proxy/worker.js` → R2 bucket `rdm7-can-logs`
- Shared secret: `RDM7_CAN_UPLOAD_HMAC_SECRET` in `main/include/can_upload_secret.h`
  AND `wrangler secret put CAN_UPLOAD_HMAC_SECRET` on the worker. Must match.
- File key in R2: `{make_slug}/{model_slug}/{device_id}_{unix_ts}.csv`
- Max upload: 10 MB per trace (server-enforced). Per-second timestamp window
  is ±10 min to limit replay.
- Dev-phase: secret is in firmware repo for convenience. Rotate before
  customer rollout and consider deriving per-device keys from `device_id`.

## Common Pitfalls

- **Widget not updating:** signal missing from JSON, or `signal_subscribe()` not called after `w->root` set
- **LVGL crash:** missing mutex — all `lv_*` calls must hold `rdm_lvgl_lock`
- **Position wrong:** missing `lv_obj_set_align(obj, LV_ALIGN_CENTER)` before `set_pos`
- **Colors wrong:** check `convertWidgetColors()` direction (565↔888)
- **Field not saved:** check `to_json` defaults-only logic and that `from_json` reads it
- **Config modal field missing:** add a section in `config_modal.c` even if web editor already handles it
- **`pdMS_TO_TICKS(1) = 0`** at `CONFIG_FREERTOS_HZ=500` — use `vTaskDelay(1)` literal for real yields
- **`max_uri_handlers`** is 160 (~106 currently used) — count `REGISTER_URI` calls before adding endpoints; the REGISTER_URI macro tallies failures and shouts at boot if you hit the cap
- **`w->root` may be a container, not the widget's LVGL primitive** — e.g. `widget_arc` standard mode wraps `lv_arc` in a transparent container so siblings (redline arc, value label) can coexist. Don't assume `w->root` is the type-specific object; use `type_data->arc_obj` etc.
- **Layout > 32 KB silently truncates** — `to_json` must be defaults-only or the budget blows. Pre-save check in `_checkLayoutSize` catches it client-side, but firmware-side a non-default emit can sneak through.

## Diagnostic Tooling

- `system/heap_monitor.c/h` — periodic heap snapshot + esp_timer_dump. **Disabled by
  default in production**; flip `RDM_HEAP_MONITOR_ENABLED=1` (in `main.c` or via
  `-D` build flag) when chasing leaks. Code stays compiled-in so re-enabling is one flag.
- Boot log line `URI registration: N/M FAILED — bump max_uri_handlers` means
  endpoints fell through to the CORS wildcard and returned 405. Increase
  `config.max_uri_handlers` in `web_server.c`.
