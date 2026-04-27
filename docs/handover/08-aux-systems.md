# 08 — Auxiliary Systems

Subsystems that aren't widgets, signals, or screens but are essential to the product: night mode, dimmer, data logger, replay, peaks, ECU presets, calculated gear, screenshot pipeline, and the captive-portal stack.

## Night mode

[main/system/night_mode.c/h](../../main/system/night_mode.c). Already covered in [03-widget-system.md](03-widget-system.md) §night-mode and [06-ui-and-screens.md](06-ui-and-screens.md) §night-mode-controller. Recap:

- Singleton state (active / inactive) + subscriber list (max 64).
- `night_mode_set_active(bool)` is thread-safe; dispatch via `lv_async_call`.
- Triggers: manual button in Device Settings, or layout-level CAN signal trigger.
- Widgets with night overrides subscribe via `night_mode_subscribe`. Use [widget_night_helpers.h](../../main/widgets/widget_night_helpers.h) macros for boilerplate.
- The dual-object pattern handles LVGL v8 properties that can't be cleanly mutated at runtime.

`night_mode_clear_subscribers()` must be called at the top of every `dashboard_init` (and is). Without it, subscriber slots from the previous layout get reused while still pointing at freed widgets.

## Brightness dimmer

The dashboard can auto-dim when a configured signal crosses a threshold (typically headlight-on). NVS namespace `dimmer_cfg` stores the configuration:

| Key | Type | Purpose |
|---|---|---|
| `sig_name` | str | Signal to watch |
| `thresh` | u16 | Threshold value |
| `is_mom` | u8 | Treat signal as momentary (rising edge → toggle) vs. level |
| `invert` | u8 | Invert active condition |
| `dim_br` | u8 | Brightness when dimmed (0–255) |
| `enabled` | u8 | Master enable |

`dimmer_subscribe()` (called from `dashboard_init`) subscribes to the configured signal. On threshold cross, it calls `lv_disp_set_*` brightness controls.

API exposed at `/api/dimmer/config` (GET/POST).

## Data logger

[main/storage/data_logger.c/h](../../main/storage/data_logger.c). Reference detail in [05-storage-and-persistence.md](05-storage-and-persistence.md) §data-logger. What's not there:

- **No dedicated FreeRTOS task.** Logger runs on an LVGL timer at the configured rate. The logger reads from `signal_t.current_value` for each registered CAN signal and writes a row.
- **Hybrid flush.** Every 100 samples or 2 s, whichever first, calls `fflush(file)` to limit data loss on power cut.
- **No auto-rotate.** A long log accumulates in one CSV. The user is responsible for stopping/starting if file size matters.
- **SD missing**: `data_logger_start` returns an error; UI greys out the toggle.

Shape of the CSV — header row lists every CAN signal at start; signals registered after a log starts will not appear in subsequent rows (file format is fixed at start).

## Signal replay

[main/storage/signal_replay.c/h](../../main/storage/signal_replay.c). Already detailed in [04-signal-and-can.md](04-signal-and-can.md) §replay. Use cases:

- Verify a layout against a recorded drive log without being in the car.
- Reproduce a specific incident captured on-device.
- Demo a layout in a sandbox / showroom.

When replay is active, `signal_inject_test_value` is used per row. This **does** update peaks (replay is treated like real CAN), unlike the simulator which doesn't.

## Peak hold

The `signal_t` struct always tracked `peak_value` and `min_value` — the feature was latent for years before being surfaced. Two views:

### On-device

[main/ui/screens/ui_peaks.c](../../main/ui/screens/ui_peaks.c) — full-screen scrollable table:

- Columns: Signal, Current, Peak, Min, Reset.
- 100 ms refresh on the table widget.
- Per-row reset (↺ button): `signal_reset_peak(index)`.
- "Reset All" footer button: `signal_reset_peaks()`. Persists to NVS immediately.

### Per-widget

Panel widgets have a `show_peak` field (`Off / Max / Min / Both`) that adds a small label below the value showing peak / min / both side by side. Read from `signal_t.session_peak` / `session_min` (reset on each boot), not all-time values, so a panel's "peak today" makes sense.

Reset paths:

| Reset | What it clears | Persistence |
|---|---|---|
| Per-row ↺ in Peaks screen | All-time + session for that signal | NVS write |
| "Reset All" in Peaks | All signals, all-time + session | NVS write |
| Power cycle | Session peaks only | (NVS holds all-time peaks) |
| `signal_reset_session_peak(idx)` | Session only | No NVS |

NVS load happens once after `signal_register` returns for each signal, inside `signal_peaks_load()`. Records that no longer match a registered signal are dropped at load time.

## Diagnostics

[main/ui/screens/ui_diagnostics.c](../../main/ui/screens/ui_diagnostics.c). 5-card grid (3×2 minus one), 257×150 px each, auto-refreshes every 1 s:

| Card | Shows |
|---|---|
| **CAN BUS** | Bus state, RX/TX/error counts, last RX ID, bitrate. Collapsible details. |
| **WI-FI** | Mode (STA/AP), SSID, IP address, RSSI, channel. |
| **SYSTEM** | Uptime, free heap, free PSRAM, logger status, replay status, ESP-IDF version. |
| **SD CARD** | Mount state, total/used/free MB. |
| **SIGNALS** | Total registered, fresh count, stale count. |

Body is scrollable as a safety net but content fits 800×480 without scrolling.

Launched from Device Settings → "System Diagnostics".

## ECU presets

[main/layout/ecu_presets.c/h](../../main/layout/ecu_presets.c).

### Built-in presets

8 ECUs (presented in the wizard's two-step picker — Make → Version):

| Make | Version(s) |
|---|---|
| AEM | (versions vary) |
| Holley | |
| Haltech | |
| Link | |
| MaxxECU | 1.2 |
| MS3-Pro | (with degF→°C, m/s→km/h, AFR→λ conversions baked in) |
| Ford | BA-BF, FG |
| Custom | (no preset — user defines signals manually) |

Each preset is an array of signal definitions (CAN ID, bit start/length, scale, offset, endian, unit). Applying a preset overwrites the active layout's `signals` array.

### Custom presets

User-saved presets (the "Save current as preset" flow in the editor) live on LittleFS and are exposed via `/api/presets/custom` (see [07-web-server-api.md](07-web-server-api.md)).

## Calculated gear

A synthetic signal `CALCULATED_GEAR` derived from RPM and vehicle speed. Produced inside [main/widgets/signal_internal.c](../../main/widgets/signal_internal.c).

Configuration (NVS namespace `gear_cal_cfg`):

| Field | Purpose |
|---|---|
| `enabled` | Master on/off |
| `wheel_circumference_m` | Drive-wheel circumference |
| `final_drive` | Differential ratio |
| `ratios[]` | Gearbox ratios, index = gear (0 = neutral, 1 = 1st, …) |
| `rpm_signal` | Source signal name for engine RPM |
| `speed_signal` | Source signal name for vehicle speed |

Algorithm (signal_internal.c, line ~135):

```c
if (speed < 5 km/h || rpm < 500) return 0;        /* neutral */

theoretical_speed[gear] = (rpm * wheel_circumference) / (60 * ratio[gear] * final_drive)
best_gear = argmin |theoretical_speed[gear] - actual_speed|

return best_gear;
```

UI:
- Hamburger Tools → "Gear Setup..." opens a modal.
- Auto-opens first time a widget binds CALCULATED_GEAR.
- API: `/api/gear/config` (GET/POST).

## Wire inputs (turn signals)

[main/io/wire_inputs.c](../../main/io/wire_inputs.c).

| GPIO | Signal | Use |
|---|---|---|
| 43 | `INDICATOR_LEFT` | Driven by external relay or vehicle indicator wire. Active level configurable. |
| 44 | `INDICATOR_RIGHT` | Same. |

Polled from a dedicated low-priority task (core 0, prio 3, 2 KB stack). Levels published to internal signals, consumed by indicator widgets in wire-mode.

## Screenshot / capture pipeline

The most subsystem-heavy thing in `system/`. [main/system/display_capture.c](../../main/system/display_capture.c).

Path:

```
                         RGB LCD panel
                              ▲
                              │ DMA
                              │
                       LVGL framebuffer
                              │
                              │ esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb0)
                              ▼
                      raw RGB565 buffer (single-FB form)
                              │
                              │ pixel-format conversion to YUYV (JPEG_PIXEL_FORMAT_YCbYCr)
                              ▼
                    persistent half-res buffer (288 KB)
                       — or transient full-res (960 KB) on `?full=1`
                              │
                              │ esp_new_jpeg_encode()
                              ▼
                          JPEG bytes
                              │
                              ▼
                         HTTP response
```

Why YUYV: chosen empirically — RGB888 path was 1.15 MB for full-res; YCbY2YCrY2 produced garbage (packing layout mismatch); YUYV is 2 bytes/px, behaves correctly with `esp_new_jpeg`.

Why fb_num=1 (single-pointer form): fb_num=2 form caused subtle issues. We also killed the 768 KB shadow framebuffer that previously sat in `flush_cb` — saved permanent PSRAM.

Endpoints already covered in [07-web-server-api.md](07-web-server-api.md) — `/api/screenshot`, `/api/capture/stream`. The streaming MJPEG handler frame-skips when slow, never buffers more than one frame ahead.

CONTROL mode in the desktop Studio uses polled `/api/screenshot?full=1` at 1 Hz (Safari compat) plus `/api/touch` for input.

## Captive portal stack

Already covered in [07-web-server-api.md](07-web-server-api.md) §captive-portal-probes and §dns-hijack. Recap of the full stack required for phones to land on the editor reliably:

1. **AP-only mode when no saved STA** ([wifi_manager.c](../../main/net/wifi_manager.c)) — prevents ESP-IDF's connection retry from starving the radio.
2. **AP channel 11** — channel 1 was too congested in some RF environments for 802.11 association.
3. **HT20 forced** — avoids HT40 negotiation failures with weak phone clients.
4. **Captive-portal HTTP handlers** — 9 OS probe URIs return 302 to `/`.
5. **DNS hijack on UDP:53** — answers `connectivitycheck.gstatic.com` etc. with the AP IP.
6. **`CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`** — Android headers exceed default 512 B.
7. **`CONFIG_LWIP_MAX_SOCKETS=12`** — headroom for parallel webview connections.

Things tried and reverted: STATIC_TX_BUFFER bump (caused `esp_wifi_init` NO_MEM), open auth (debug only), `MDNS_MEMORY_ALLOC_SPIRAM` (chose to drop mDNS entirely — files + dependency removed 2026-04-27, see [ADR 0001](../adr/0001-wifi-onboarding-reliability.md)).

## OTA

Stack already documented in [05-storage-and-persistence.md](05-storage-and-persistence.md) §OTA. Things to know about the user-facing flow:

- The dashboard polls `/api/ota/check` infrequently (not every boot).
- An update banner appears when `OTA_UPDATE_AVAILABLE`.
- The OTA dialog ([ota_update_dialog.c](../../main/net/ota_update_dialog.c)) shows release notes, download progress, and reboot countdown.
- Cloudflare proxy in [tools/cloudflare-ota-proxy/](../../tools/cloudflare-ota-proxy/) is one deployment option; not required.

## Serial command interface

[main/net/serial_protocol.c](../../main/net/serial_protocol.c) and [main/net/serial_commands.c](../../main/net/serial_commands.c) expose a CLI over UART:

| Command (example) | What it does |
|---|---|
| `signal.list` | Dump all registered signals + current values |
| `signal.inject NAME VALUE` | Same as `/api/signal/inject` |
| `log.start` / `log.config.set` / `log.config.get` | Data-logger control |
| `replay.start FILE [SPEED] [LOOP]` | CAN replay |
| `replay.stop` | |
| `image.upload NAME` | Receive base64 image |
| (others — see `serial_commands.c`) |

Useful for debugging without WiFi.

## UART protocol

[main/net/uart_protocol.c](../../main/net/uart_protocol.c) — the framing layer underneath `serial_commands`. Routed through UART1, default baud 115200, line-based. Spawns `uart_rx_task` on core 0 prio 5, 6 KB stack.

## Things you might mistake for "missing infrastructure"

- **No global event bus.** The signal registry serves that role for CAN-derived state; everything else is a direct call.
- **No state machine library.** Most stateful UI is implicit in the LVGL object tree.
- **No DI / IoC.** Module boundaries are header includes; lifetime is "init at boot, live forever".
- **No retained HTTP sessions.** Every API call is stateless.

This is fine — the firmware is small enough that an event bus would add more cognitive overhead than it saves. If you find yourself wanting one, look first at whether you can encode the new state as a synthetic signal.

---

Next: [09-conventions-and-pitfalls.md](09-conventions-and-pitfalls.md).
