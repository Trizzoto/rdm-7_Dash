# RDM-7 Dash — Developer Handover

A working knowledge of this firmware, written for a new development team. Each document is self-contained, but they read best in order.

## What this firmware is

**RDM-7 Dash** is the firmware for an ESP32-S3-powered automotive dashboard. It renders a configurable instrument cluster (gauges, panels, RPM bar, indicators, alerts) on an 800×480 RGB LCD, decodes vehicle signals from CAN bus, and exposes a web editor over WiFi for remote layout authoring. Layouts are JSON files on LittleFS; widgets, signals, and rules are all data-driven.

It is one of four repositories in the **RDM project family**:

| Repo | Purpose |
|---|---|
| **RDM-7 Dash** (this repo) | Device firmware (ESP-IDF v5.x + LVGL v8) |
| **RDM Desktop Studio** (`rdm7-desktop`) | Tauri app — full-featured layout editor |
| **RDM Web Studio** (`rdm7-studio-web`) | Browser-based layout editor |
| **RDM Marketplace** | Supabase-backed layout sharing platform |

These share the layout JSON schema, the widget definition table (`WIDGET_DEFS`), and the asset format (`.rdmimg`). Schema-breaking changes ripple through all four — bump `LAYOUT_SCHEMA_VERSION` in [main/layout/layout_manager.h](../../main/layout/layout_manager.h) when you change anything.

## Reading order

1. [01-architecture.md](01-architecture.md) — System design, threading model, data-flow diagrams. Read this first.
2. [02-build-and-flash.md](02-build-and-flash.md) — How to build, flash, monitor; partition layout; non-default sdkconfig.
3. [03-widget-system.md](03-widget-system.md) — The 13 widget types, vtable, lifecycle, how to add a 14th.
4. [04-signal-and-can.md](04-signal-and-can.md) — CAN RX pipeline, signal registry, internal/synthetic signals, simulator, replay.
5. [05-storage-and-persistence.md](05-storage-and-persistence.md) — LittleFS, NVS, SD card, image format, font cache, OTA.
6. [06-ui-and-screens.md](06-ui-and-screens.md) — Screens, dashboard core, config modal, settings, touch input, night mode.
7. [07-web-server-api.md](07-web-server-api.md) — Full HTTP API reference, captive portal, the three `index.html` copies.
8. [08-aux-systems.md](08-aux-systems.md) — Data logger, signal replay, peak hold, diagnostics, ECU presets, dimmer.
9. [09-conventions-and-pitfalls.md](09-conventions-and-pitfalls.md) — Coding conventions, threading rules, common mistakes.
10. [10-module-reference.md](10-module-reference.md) — One-line index of every C/H file in `main/`.

## The 60-second mental model

```
                                     LVGL task (core 1, prio 8)
                                  ┌──────────────────────────────┐
  CAN RX (core 0, prio 7)         │                              │
  ┌────────────┐    s_can_queue   │  can_process_queued_frames() │
  │ TWAI driver│────64 frames────▶│   → signal_dispatch_frame()  │
  └────────────┘                  │      → bit-extract + scale   │
                                  │      → notify subscribers    │
                                  │           │                  │
  GPIO/ADC                        │           ▼                  │
  internal ───▶ signal_inject ───▶│   widget callbacks update    │
  signals      _test_value()      │      LVGL objects            │
                                  │                              │
  HTTP server (core 0)            │                              │
  ┌────────────┐                  │   lv_async_call()            │
  │/api/touch  │──remote_touch───▶│   for cross-task UI changes  │
  │/api/layout │──lv_async_call──▶│                              │
  └────────────┘                  └──────────────────────────────┘
                                           │
                                           ▼
                                  ┌─────────────────┐
                                  │  RGB LCD panel  │
                                  │   (DMA flush)   │
                                  └─────────────────┘
```

**Three sentences that capture most of the design:**

1. **Everything LVGL is single-threaded** — all `lv_*` calls happen on the LVGL task on core 1, behind a recursive mutex (`example_lvgl_lock`). Cross-task UI changes use `lv_async_call()`.
2. **CAN bytes never reach widgets directly** — they flow through `signal_t` records in the signal registry; widgets subscribe to signal indices, not CAN IDs. This is the boundary that makes layouts portable across vehicles.
3. **The layout JSON is the source of truth** — widget positions, colors, signal definitions, and rules all live in `/lfs/layouts/<name>.json`. NVS only stores *which* layout is active. Editing is a save-and-reload cycle, not in-place mutation.

## Prerequisites for the next dev

- ESP-IDF v5.x installed and exported (`source $IDF_PATH/export.sh`).
- Comfort with C11, FreeRTOS primitives, and LVGL v8 (NOT v9 — the API differs significantly).
- Familiarity with cJSON for parsing layout JSON.
- Optional but useful: a CH340/CP2102 USB-UART adapter, a CAN bus analyser, and an SD card for log capture.

See [02-build-and-flash.md](02-build-and-flash.md) for first-build steps.

## Where things live (top-level)

```
RDM-7_Dash/
├── main/                  Firmware source (start here)
│   ├── main.c             app_main, boot init order
│   ├── widgets/           13 widget types + signal registry + font manager
│   ├── layout/            layout_manager (LittleFS JSON), default_layout, ECU presets
│   ├── can/               TWAI driver, decode, bus-bitrate auto-detect
│   ├── storage/           SD card, data logger, signal replay, config_store (NVS)
│   ├── net/               web_server, wifi_manager, OTA, DNS hijack, mDNS (disabled)
│   ├── system/            screen_config, night_mode, remote_touch, device_id
│   ├── ui/                screens, settings, config modal, dashboard, fonts, images
│   ├── io/                wire input GPIOs (turn signals)
│   ├── design_mode/       Layout editor companion code (used by Studio integration)
│   ├── embed/             Boot logo .rdmimg
│   ├── web/index.html     Embedded web editor (~707 KB)
│   ├── lv_conf.h          LVGL build-time config
│   ├── Kconfig.projbuild  Project Kconfig (screen size, double-FB, etc.)
│   └── CMakeLists.txt     Source list, EMBED_TXTFILES
├── components/            Vendor components (LVGL fork, touch driver, SD card, etc.)
├── managed_components/    ESP-IDF Component Manager managed deps
├── data/web/index.html    Mirror of main/web/index.html (used by mobile-dev-server)
├── tools/                 png_to_rdmimg.py, mobile-dev-server.js, OTA proxy, WASM build
├── docs/                  Documentation (you are here)
├── partitions.csv         Flash partition layout
├── sdkconfig              ESP-IDF compile-time config (committed)
├── sdkconfig.defaults     Defaults applied at fresh checkout
├── version.h              Firmware version string
└── CLAUDE.md              Project instructions (also useful for humans)
```

## Conventions cheat sheet

- C11, `snake_case`, `UPPER_SNAKE_CASE` macros, module prefix per directory (`widget_panel_`, `signal_`, `layout_manager_`).
- LVGL v8 styling API: `lv_obj_set_style_*(obj, value, selector)` — **not** the v9 prop+selector form.
- `(0,0)` = screen centre. Use `SCREEN_W/H/ORIGIN_X/ORIGIN_Y` from `system/screen_config.h`. Always `lv_obj_set_align(obj, LV_ALIGN_CENTER)` **before** `lv_obj_set_pos()`.
- Logging: `ESP_LOGI/W/E/D` with a static `TAG`. Headers: `#pragma once` + `extern "C"` guards.
- Large allocations: `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`. Internal RAM is tight after WiFi init.
- Layouts must fit 32 KB. Widget `to_json` writes **defaults-only** — every field at its factory default is omitted.

See [09-conventions-and-pitfalls.md](09-conventions-and-pitfalls.md) for the full list with rationale.

## Where to ask "how does X work?"

| Question | Doc |
|---|---|
| Why does my widget not update? | [04-signal-and-can.md](04-signal-and-can.md) §subscription |
| How do I add a new widget type? | [03-widget-system.md](03-widget-system.md) §adding-a-widget |
| What screens exist and how are they launched? | [06-ui-and-screens.md](06-ui-and-screens.md) |
| What HTTP endpoints does the firmware expose? | [07-web-server-api.md](07-web-server-api.md) |
| Where is X persisted? | [05-storage-and-persistence.md](05-storage-and-persistence.md) |
| What runs on which core? | [01-architecture.md](01-architecture.md) §threading |
| Why is the 5-second TWDT now 15 seconds? | [02-build-and-flash.md](02-build-and-flash.md) §watchdog |
| What is the `.rdmimg` format? | [05-storage-and-persistence.md](05-storage-and-persistence.md) §image-format |
| How does night mode know when to switch? | [08-aux-systems.md](08-aux-systems.md) §night-mode |

## Things that will surprise you

A handful of non-obvious gotchas, each documented further inside:

- **mDNS is permanently disabled** in [main/net/mdns_service.c](../../main/net/mdns_service.c). The managed component can't allocate from internal RAM after WiFi init. QR code + IP fallback replace it.
- **`pdMS_TO_TICKS(1) == 0`** at `CONFIG_FREERTOS_HZ=500`. Use `vTaskDelay(1)` literal for real yields.
- **Three copies of `index.html` must stay in sync** (firmware-embedded, `data/web/`, desktop). There is no automated sync.
- **`remote_touch_init()` must be called from `dashboard_init()`**, not `app_main`. Registering a 2nd pointer indev before widgets exist makes `lv_obj_get_screen()` infinite-loop on first widget create.
- **`max_uri_handlers = 100`** (was 80). ESP-IDF silently drops registrations past the cap. Count `httpd_register_uri_handler` calls before adding endpoints.

## Test plan after handover

Before assuming a build is healthy, run through this:

1. Boot → splash → main dashboard renders (no white flash).
2. Tap a panel widget → MENU button appears. Long-press → config modal opens.
3. Connect to `RDM7-XXXX` AP → captive portal pops up (iOS/Android) → web editor loads.
4. Edit a panel label → save → see the live device update within ~3 s.
5. Power-cycle → layout persists, last-active layout is reloaded.
6. Inject a CAN frame for a registered signal → corresponding panel updates within one frame.
7. Pull SD card → data logger gracefully reports SD missing; doesn't crash.
8. Toggle night mode → colors flip; flip back; widgets repaint cleanly.

If any of those break, start with [09-conventions-and-pitfalls.md](09-conventions-and-pitfalls.md) — the failure mode is probably documented there.
