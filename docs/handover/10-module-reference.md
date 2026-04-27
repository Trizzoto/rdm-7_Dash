# 10 — Module Reference

A one-line index of every C/H file under `main/`. Use this to find where things live; deeper detail is in the topic docs (linked where relevant).

## Top level

| File | Purpose |
|---|---|
| [main/main.c](../../main/main.c) | `app_main`. Boot init order, LVGL task spawn, mutex setup. |
| [main/lv_conf.h](../../main/lv_conf.h) | LVGL build-time config (color depth, fonts, etc.). |
| [main/Kconfig.projbuild](../../main/Kconfig.projbuild) | Project Kconfig: screen size, double-FB, bounce buffer. |
| [main/CMakeLists.txt](../../main/CMakeLists.txt) | Source list, EMBED_TXTFILES, REQUIRES. |
| [main/idf_component.yml](../../main/idf_component.yml) | Managed dependency manifest. |
| [main/include/](../../main/include/) | Project-wide headers (currently sparse). |
| [main/embed/](../../main/embed/) | Boot logo `.rdmimg`. |
| [main/web/index.html](../../main/web/index.html) | Embedded web editor (~707 KB). |

## `main/widgets/`

The widget layer + signal registry + font cache. See [03-widget-system.md](03-widget-system.md), [04-signal-and-can.md](04-signal-and-can.md).

| File | Purpose |
|---|---|
| `widget_types.h` | `widget_t` base struct, vtable typedefs, `widget_type_t` enum. |
| `widget_types.c` | Constraints table, font resolver, capability queries. |
| `widget_registry.c/h` | 32-slot registry, `find_by_id`, `find_by_type_and_slot`. |
| `widget_rules.c/h` | Conditional rules (signal × operator × override). |
| `widget_night_helpers.h` | Macros for night-override structs + parse/serialize/pick. |
| `widget_panel.c/h` | Panel widget (label + value + alerts + peak). 8 slots. |
| `widget_rpm_bar.c/h` | RPM bar with limiter effects. Singleton. |
| `widget_bar.c/h` | Horizontal bar with anchor scale + image fills. 2 slots. |
| `widget_indicator.c/h` | Status light (round/rect). 8 slots. |
| `widget_warning.c/h` | Alert symbol triggered by panel thresholds. 8 slots. |
| `widget_text.c/h` | Free signal-bound label. 8 slots. |
| `widget_meter.c/h` | LVGL `lv_meter` — needle + arc + ticks. 2 slots. |
| `widget_image.c/h` | Static or signal-driven image. 8 slots. |
| `widget_shape_panel.c/h` | Geometric container. 2 slots. |
| `widget_arc.c/h` | LVGL `lv_arc` progress. 2 slots. |
| `widget_toggle.c/h` | CAN-TX boolean switch. 2 slots. |
| `widget_button.c/h` | CAN-TX command button. 4 slots. |
| `widget_shift_light.c/h` | RPM-driven LED bar. Singleton. |
| `signal.c/h` | `signal_t` registry, dispatch, peak/min, NVS persistence. |
| `signal_internal.c/h` | Synthetic signals (FPS, CPU, RSSI, uptime, calculated gear, etc.). |
| `signal_sim.c/h` | Triangle-wave simulator. |
| `font_manager.c/h` | TTF cache (8 families, 32 instances). |

## `main/can/`

CAN bus driver and decoder. See [04-signal-and-can.md](04-signal-and-can.md).

| File | Purpose |
|---|---|
| `can_manager.c/h` | TWAI driver wrapper. RX task, queue, recovery. `can_start_task`. |
| `can_decode.c/h` | `can_extract_bits(data, start, len, endian, signed)` — pure helper. |
| `can_bus_test.c/h` | Bitrate auto-detection (used by first-run wizard + Settings). |

## `main/layout/`

Layout JSON persistence + ECU presets. See [05-storage-and-persistence.md](05-storage-and-persistence.md).

| File | Purpose |
|---|---|
| `layout_manager.c/h` | LittleFS layout I/O, schema version, `_load_signals`, `_instantiate_widgets`. |
| `default_layout.c/h` | Hardcoded fallback layout (seeded if `/lfs/layouts/default.json` missing). |
| `layout_loader.c/h` | Lower-level JSON helpers used by layout_manager. |
| `ecu_presets.c/h` | 8 built-in ECU presets + custom-preset I/O. |

## `main/storage/`

Filesystem-backed state. See [05-storage-and-persistence.md](05-storage-and-persistence.md).

| File | Purpose |
|---|---|
| `config_store.c/h` | NVS wrapper for app settings (rotation, log rate, dimmer, gear, etc.). |
| `boot_assets.c/h` | First-boot seeding of layouts/fonts/images. |
| `sd_manager.c/h` | SD card mount, info query. |
| `data_logger.c/h` | CSV logger to SD. Rate-selectable, NVS-persisted. |
| `signal_replay.c/h` | CSV → signal injection player. |

## `main/net/`

WiFi, web server, OTA, captive portal, serial. See [07-web-server-api.md](07-web-server-api.md).

| File | Purpose |
|---|---|
| `web_server.c/h` | HTTP server. ~86 endpoints, embedded `index.html`, captive portal. |
| `wifi_manager.c/h` | STA/AP/multi-SSID, channel pinning, HT20 force, boot config. |
| `dns_hijack.c/h` | UDP:53 captive-portal helper (PSRAM stack). |
| `mdns_service.c/h` | mDNS — **disabled** (`RDM7_MDNS_DISABLED 1`). Skeleton only. |
| `ota_handler.c/h` | OTA download + flash + status. |
| `ota_update_dialog.c/h` | LVGL modal for OTA progress. |
| `serial_protocol.c/h` | Framing layer over UART. |
| `serial_commands.c/h` | Command dispatch (signal.list, log.start, replay.start, …). |
| `uart_protocol.c/h` | UART1 setup + RX task spawn. |
| `usb_cdc_protocol.c/h` | USB-CDC serial alternative. |

## `main/system/`

Cross-cutting platform glue.

| File | Purpose |
|---|---|
| `screen_config.c/h` | `SCREEN_W/H/ORIGIN_X/ORIGIN_Y`, screen profile struct. |
| `night_mode.c/h` | Singleton + subscriber list, `lv_async_call` dispatch. |
| `remote_touch.c/h` | Virtual LVGL pointer indev for HTTP-driven CONTROL mode. |
| `device_id.c/h` | Per-device UUID generation/storage in NVS. |
| `rdm_settings.c/h` | Early/system NVS wrapper (separate from `config_store`). |
| `display_capture.c/h` | Screenshot pipeline (panel FB → YUYV → JPEG). |

## `main/ui/`

Screens, dashboard, settings, modal, theming, fonts, images.

| File | Purpose |
|---|---|
| `dashboard.c/h` | Main widget host. `dashboard_init`, hot reload, persist. |
| `ui.c/h` | Top-level UI init helpers. |
| `ui_styles.c/h` | Shared LVGL style descriptors (use sparingly). |
| `ui_helpers.c/h` | Misc helpers (color converters, widget tags). |
| `ui_events.h` | Shared LVGL event constants. |
| `utils.c/h` | Generic utilities (string formatting, math). |
| `signals.c/h` | UI-side signal helpers (subscription bookkeeping). |
| `config_bridge.c/h` | Maps `value_id` 1–13 → widget type_data + signal accessors. |
| `theme.h` | Color palette + style tokens. |
| `lvgl_helpers.h` | Small LVGL convenience inlines. |
| `panel_config.h` | Legacy panel-specific consts (kept for compat). |
| `global.h` | Cross-UI globals (sparingly used). |

### `main/ui/screens/`

| File | Purpose |
|---|---|
| `splash_screen.c/h` | Boot splash. Loads `_splash_<active>.json` if present. |
| `first_run_wizard.c/h` | 3-step onboarding (CAN auto-detect / WiFi / ECU). |
| `ui_Screen3.c/h` | Main dashboard host screen. |
| `ui_diagnostics.c/h` | 5-card system health overlay. |
| `ui_peaks.c/h` | All-signals peak/min table. |
| `ui_wifi.c/h` | Multi-SSID manager. |
| `ui_ecu_picker.c/h` | Two-step ECU preset picker. |
| `ui_Screen1.c/h`, `ui_Screen2.c`, `ui_Screen4.c` | **Legacy stubs.** Avoid using. |

### `main/ui/menu/`

| File | Purpose |
|---|---|
| `menu_screen.c/h` | `load_menu_screen_for_widget(w)` — long-press routing. |
| `config_modal.c/h` | The two-tab (Signal / Alerts) per-widget editor. |

### `main/ui/settings/`

| File | Purpose |
|---|---|
| `device_settings.c/h` | The big settings overlay (NETWORK, CAN, LOGGING, PEAKS, TESTING, NIGHT…). |
| `settings_panel.c/h` | Reusable settings panel widget. |
| `preset_picker.c/h` | Generic preset-list overlay. |

### `main/ui/components/`

| File | Purpose |
|---|---|
| `ui_comp.c/h` | Top-level component infrastructure (LVGL "components" feature). |
| `ui_comp_hook.c/h` | Component lifecycle hooks. |
| `ui_comp_slider2.c/h` | Custom slider component. |

### `main/ui/callbacks/`

| File | Purpose |
|---|---|
| `ui_callbacks.c/h` | LVGL event callbacks generated by SquareLine Studio (some still active). |

### `main/ui/fonts/` (compiled-in fonts)

LVGL bitmap fonts as C arrays. One file per family + size:

- `ui_font_Bahn_17/26/48/65.c`
- `ui_font_fugaz_14/17/18/28/31/33/56.c`
- `ui_font_Manrope_16_BOLD/35_BOLD/36_BOLD/54_BOLD/78_BOLD.c`
- `ui_font_RPM.c`

Custom fonts at runtime go through `font_manager` (TTF in PSRAM). The compiled-in ones are referenced by name in `widget_resolve_font`.

### `main/ui/images/` (compiled-in images)

LVGL image descriptors as C arrays. Most are UI iconography:

- Indicator-left/right (active/not-active)
- High-beam, handbrake, check-engine, launch-control, smart-car-key
- RPM indicator, gauge v0.1/v0.2/v0.3 background
- RDM logo (light / dark variants), Daihatsu logo, sticker variants

Newer assets are loaded at runtime via `rdm_image_load` from `/lfs/images/`. The compiled-in ones predate the runtime loader.

## `main/io/`

| File | Purpose |
|---|---|
| `wire_inputs.c/h` | GPIO 43/44 polling for left/right turn-signal indicator wires. |

## `main/design_mode/`

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Stub directory for design-mode integration with the studio. (Currently empty — placeholder for future expansion.) |

## Top-level resources

| File | Purpose |
|---|---|
| [partitions.csv](../../partitions.csv) | Flash partition layout (NVS, OTA × 2, LittleFS). |
| [sdkconfig](../../sdkconfig) | ESP-IDF compile-time config (committed). |
| [sdkconfig.defaults](../../sdkconfig.defaults), [sdkconfig.defaults.esp32s3](../../sdkconfig.defaults.esp32s3) | Defaults applied on fresh checkout. |
| `sdkconfig.ci.*` | CI variants for double-FB / bounce-buffer / no-bb. |
| [version.h](../../version.h) | Firmware version string (manually bumped). |
| [littlefs.bin](../../littlefs.bin) | Optional pre-built LittleFS image (rare; usually rebuilt by IDF). |
| [letsencrypt_r11.pem](../../letsencrypt_r11.pem), [vercel_cert.pem](../../vercel_cert.pem) | Trusted root certs for HTTPS (OTA download, marketplace). |

## `tools/`

| File | Purpose |
|---|---|
| [tools/png_to_rdmimg.py](../../tools/png_to_rdmimg.py) | Convert PNG → `.rdmimg` (RGB565 + alpha). |
| [tools/mobile-dev-server.js](../../tools/mobile-dev-server.js) | Node dev server for editor work without a device. |
| [tools/cloudflare-ota-proxy/](../../tools/cloudflare-ota-proxy/) | Cloudflare Workers proxy for OTA distribution. |
| [tools/setup_wasm_project.sh](../../tools/setup_wasm_project.sh) | Bootstrap WASM-LVGL preview project. |
| [tools/WASM_BUILD_PROMPT.md](../../tools/WASM_BUILD_PROMPT.md) | Notes on the WASM-compiled-LVGL preview pipeline. |

## `data/`

| Path | Purpose |
|---|---|
| [data/web/index.html](../../data/web/index.html) | Mirror of `main/web/index.html` for `mobile-dev-server.js`. |

## `docs/`

| File | Purpose |
|---|---|
| [docs/handover/](../) | This handover doc set. |
| [docs/WASM_PREVIEW_BUILD_GUIDE.md](../WASM_PREVIEW_BUILD_GUIDE.md) | Standalone guide for building the WASM preview. |

## `components/`

Vendor components NOT managed via Component Manager. Edit with caution; they ship as part of the source tree:

| Component | Notes |
|---|---|
| `lvgl__lvgl/` | LVGL 8.3.11, vendored unchanged. |
| `espressif__esp_io_expander-v1.0.1/` | I/O expander driver. |
| `sd_card/` | SPI SD card glue. |
| `espressif__esp_lcd_touch/` | Generic touch base. |
| `espressif__esp_lcd_touch_gt911/` | GT911 driver. |

## `managed_components/`

Pulled by the IDF Component Manager from the registry. Don't edit — let the lockfile drive updates. Resolved set:

- `lvgl/lvgl`
- `espressif/esp_lcd_touch_gt911`
- `joltwallet/littlefs`
- `espressif/mdns`
- `espressif/esp_new_jpeg`

## `bin/`

Historical pre-built binaries. Not part of the build pipeline.

---

## "Where do I look for…" index

| Question | File(s) |
|---|---|
| `app_main` boot order | [main/main.c](../../main/main.c) |
| Add a new widget | [03-widget-system.md](03-widget-system.md), then [main/widgets/widget_types.h](../../main/widgets/widget_types.h) + factory in [main/layout/layout_manager.c](../../main/layout/layout_manager.c) |
| New HTTP endpoint | [main/net/web_server.c](../../main/net/web_server.c) — count handlers, ≤ 100 |
| New NVS-backed setting | [main/storage/config_store.c/h](../../main/storage/config_store.c) |
| New synthetic signal | [main/widgets/signal_internal.c](../../main/widgets/signal_internal.c) |
| New ECU preset | [main/layout/ecu_presets.c](../../main/layout/ecu_presets.c) |
| Coordinate / screen-size questions | [main/system/screen_config.h](../../main/system/screen_config.h) |
| Night mode plumbing | [main/system/night_mode.c/h](../../main/system/night_mode.c) + [main/widgets/widget_night_helpers.h](../../main/widgets/widget_night_helpers.h) |
| LVGL mutex helpers | [main/main.c](../../main/main.c) ~line 294 (`example_lvgl_lock` / `_unlock`) |

That's the entire firmware. Everything else is build artefacts, vendored components, or repository hygiene.
