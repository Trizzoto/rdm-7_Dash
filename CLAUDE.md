# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**RDM-7 Dash** — ESP32-S3 automotive dashboard firmware. ESP-IDF v5.x + **LVGL v8** (NOT v9).
800x480 RGB LCD (configurable via Kconfig), CAN bus data via signal pub/sub, JSON layouts on LittleFS, web editor over WiFi.

This repo is the firmware component of the **RDM project**, which spans four repos: **RDM-7 Dash** (this firmware), **RDM Desktop Studio** (Tauri app), **RDM Web Studio**, and **RDM Marketplace**. They share layout JSON format, API contracts, and asset pipelines.

## Build

- `idf.py build` (requires `source $IDF_PATH/export.sh`) — **do NOT run without asking**, user builds externally
- `idf.py -p COMx flash monitor`
- All `.c` files must be listed in `main/CMakeLists.txt` SRCS
- `main/web/index.html` embedded via `EMBED_TXTFILES`
- `-Werror=comment` is active — no `/*` inside block comments
- Partition: dual OTA (3.5 MB each) + LittleFS (~8.8 MB) on 16 MB flash

## Threading (Critical)

- **LVGL is single-threaded.** All `lv_*` calls require LVGL mutex (`example_lvgl_lock`). LVGL task: core 1, 14 ms tick.
- CAN RX task: core 0, enqueues to `s_can_queue`; LVGL task drains via `can_process_queued_frames()`.
- Signal callbacks run on LVGL task — safe for direct LVGL calls.
- Use `lv_async_call()` from other contexts (web server, etc.).

## Coordinate System

- **(0,0) = screen center**. Use `SCREEN_W/H/ORIGIN_X/ORIGIN_Y` from `screen_config.h` — never hardcode 800/480.
- All widgets: `lv_obj_set_align(obj, LV_ALIGN_CENTER)` **before** `lv_obj_set_pos()`.
- Web editor converts via `devToWeb()`/`webToDev()`.

## Widget System

13 types in `widget_type_t` enum (`widget_types.h`). Slot-limited: panel(16), bar(2), indicator(2), warning(8).

**Lifecycle:** factory → `from_json()` → `create()` → updates via signal callbacks → `to_json()` → `destroy()`

Key patterns:
- `from_json`: resolve signal name→index via `signal_find_by_name()`, call `widget_rules_from_json()`
- `create`: build LVGL objects, subscribe signals **after** `w->root` is set
- `to_json`: **defaults-only** — skip fields matching factory defaults (16 KB JSON budget)
- `destroy`: `widget_rules_free()`, free `type_data`
- Per-instance styles via `lv_obj_set_style_*()` — not shared `lv_style_t`
- Conditional rules: `widget_rules.c/h`, requires `apply_overrides` vtable + `widget_rules_subscribe()` after create

## Signal System

Signal registry (`widgets/signal.c/h`) is the sole CAN decode layer. Signals defined in layout JSON, registered at load time. Timeout after 2s (stale). Internal signals (`signal_internal.c`) for GPIO/ADC. Simulator: `signal_sim.c`.

```
CAN RX (core 0) → s_can_queue → can_process_queued_frames() (LVGL task)
  → signal_dispatch_frame() → can_extract_bits() → scale/offset → notify subscribers
```

## Storage

| Data | Where | API |
|---|---|---|
| Layouts + signals | LittleFS `/lfs/layouts/` | `layout_manager_save/load()` |
| Images | LittleFS `/lfs/images/*.rdmimg` | Custom binary: 12-byte header + RGB565+alpha |
| Fonts (TTF) | LittleFS `/lfs/fonts/` | `font_manager_get(family, size)` |
| Settings | NVS | `rdm_settings_*()`, `config_store_*()` |
| SD card | FAT `/sdcard/` | layouts/images/fonts subdirs |

All CAN signal config is in layout JSON — not NVS.

## Config & Reload Flows

**Touchscreen:** widget tap → `config_modal_open(value_id)` → `config_bridge` reads/writes `type_data` → `dashboard_persist_layout()`

**Web save → hot reload:** `POST /api/layout/save` → LittleFS → `lv_async_call()` → `dashboard_init()` → full re-create. Web UI polls `/api/layout/current` every 3s.

**Config bridge** (`ui/config_bridge.c/h`): maps value_id (1-13) to widget `type_data` + signal registry. Uses real `type_data` structs via `#include`.

## Color Conversion

Firmware: **RGB565**. Web editor: **RGB888**. Use `rgb565to888()` on load, `rgb888to565()` on save. `convertWidgetColors()` walks `WIDGET_DEFS` field metadata.

## Web Editor

- `WIDGET_DEFS` defines widget metadata + `fields[]` per type
- `buildFirmwarePayload()`: maps `w.signal` → `config.signal_name`, converts colors, converts TX data
- **`data/web/index.html` is a copy of `main/web/index.html`** — keep in sync

## Fonts

- Dynamic TTF via `lv_tiny_ttf`, cache: 8 families (PSRAM) + 32 size instances
- JSON format: `"Family:size"` (e.g., `"Fugaz:28"`) or legacy `"fugaz_28"`
- `font_manager_reset_instances()` on layout reload

## Adding a New Widget

1. Create `widget_X.c/h` — define `X_data_t` **in the header**
2. Implement vtable: `create`, `resize`, `open_settings`, `to_json`, `from_json`, `destroy` (+ `apply_overrides`)
3. Add factory `widget_X_create_instance(uint8_t slot)`
4. Register in: `widget_type_t` enum, `widget_constraints[]` + `widget_type_name()`, `_type_from_str()` + `_factory()` in `layout_manager.c`
5. Add `.c` to `main/CMakeLists.txt` SRCS
6. Add to `WIDGET_DEFS` in `main/web/index.html` + sync `data/web/index.html`
7. Bump `LAYOUT_SCHEMA_VERSION` in `layout_manager.h` if schema changes

## Coding Conventions

- C11, `snake_case`, `UPPER_SNAKE_CASE` for macros. Module prefix: `widget_panel_`, `signal_`, etc.
- Logging: `ESP_LOGI/W/E/D` with static `TAG`. Headers: `#pragma once` + `extern "C"` guards.
- LVGL v8 API: `lv_obj_set_style_*()` (not v9 prop+selector style)
- Memory: `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` for large allocs

## Common Pitfalls

- **Widget not updating:** signal not in JSON, or `signal_subscribe()` not called after `w->root` set
- **LVGL crash:** threading — ensure mutex held for all `lv_*` calls
- **Position wrong:** missing `lv_obj_set_align(obj, LV_ALIGN_CENTER)`
- **Colors wrong:** check `convertWidgetColors()` direction (565↔888)
- **Field not saved:** check `to_json` defaults-only logic + `from_json` reads it
- **Config bridge breaks:** uses real `type_data` structs — field changes need config_bridge update
