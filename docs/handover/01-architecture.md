# 01 — Architecture

This document covers the system-level design: how the boot sequence assembles the firmware, which task runs where, how data flows from CAN to LCD, and why the threading model is shaped the way it is.

## Hardware target

| | |
|---|---|
| SoC | ESP32-S3 (dual-core Xtensa LX7, 240 MHz) |
| Flash | 16 MB (dual OTA 3.5 MB + LittleFS 8.8 MB) |
| PSRAM | 8 MB octal, 80 MHz, XIP enabled |
| Display | 800×480 RGB565 by default (480×480 / 720×720 selectable in Kconfig) |
| Touch | GT911 capacitive (I²C @ 0x5D) |
| CAN | TWAI peripheral, configurable bitrate (125k / 250k / 500k / 1M) |
| WiFi | STA + AP (concurrent), HT20 forced, AP channel 11 |
| SD | SPI mode (MOSI=11, CLK=12, MISO=13, CS=4) |
| Audio inputs | None |
| GPIO inputs | Wire indicators (left/right turn — GPIO 43/44), fuel sender ADC (GPIO 6) |

## Software stack

```
┌─────────────────────────────────────────────────────┐
│  Application: widgets, screens, layout, signals    │
├─────────────────────────────────────────────────────┤
│  LVGL v8.3.11 (single-threaded, mutex-protected)   │
├─────────────────────────────────────────────────────┤
│  ESP-IDF v5.x: TWAI, esp_lcd, esp_wifi,            │
│  esp_http_server, NVS, LittleFS, FATFS (SD),       │
│  esp_https_ota, mDNS                               │
├─────────────────────────────────────────────────────┤
│  FreeRTOS (500 Hz tick, dual-core, recursive       │
│  mutexes, queues)                                  │
├─────────────────────────────────────────────────────┤
│  ESP32-S3 hardware                                 │
└─────────────────────────────────────────────────────┘
```

LVGL is pinned to **8.3.11** in [main/idf_component.yml](../../main/idf_component.yml). **Do not upgrade to v9** — the API for styles, events, and indev callbacks differs significantly.

## Boot sequence (`app_main`)

The `app_main` function in [main/main.c](../../main/main.c) initialises subsystems in this order. The order matters: each step depends on something the previous step set up.

| Step | Subsystem | What it does |
|---|---|---|
| 1 | I²C master | 400 kHz on GPIO 8/9 (SDA/SCL) — needed by GT911 touch and any I²C peripherals. |
| 2 | GPIO init | Backlight pin, indicator-control GPIO. |
| 3 | ADC | Single-shot ADC on GPIO 6 (fuel sender). |
| 4 | SD card | SPI mount, FatFS at `/sdcard`. Non-fatal if absent. |
| 5 | NVS flash | `nvs_flash_init()`. Required by config_store, WiFi, device_id. |
| 6 | RGB LCD panel | 14 MHz pixel clock; flush callback registered. |
| 7 | GT911 touch | Reset sequence + I²C probe; LVGL indev registered. |
| 8 | LVGL display driver | `lv_init()`, register flush + read callbacks. |
| 9 | LVGL mutex | `xSemaphoreCreateRecursiveMutex()` — `lvgl_mux`. |
| 10 | Black screen | Paint solid black before tasks run (no white flash). |
| 11 | LVGL task | `xTaskCreatePinnedToCore` on core 1, prio 8, 16 KB stack. |
| 12 | Wire inputs init | Configure GPIO 43/44 with pull-ups. |
| 13 | CAN init + RX task | TWAI driver up, queue created, RX task on core 0 prio 7. |
| 14 | UI init (splash) | Lock LVGL → load splash layout → show splash → unlock. |
| 15 | Wire-input task | Polls GPIOs into `INDICATOR_LEFT`/`INDICATOR_RIGHT` signals. |
| 16 | Fuel ADC task | (No dedicated task — polled from `signal_internal.c` 500 ms timer.) |
| 17 | SD manager + data logger | Init only; log starts on user request. |
| 18 | UART protocol | `uart_protocol_init()` spawns `uart_rx_task`. |
| 19 | WiFi manager | `esp_netif_create_default_*`, register event handlers. |
| 20 | mDNS | Attaches to netif but **never registers** — see [main/net/mdns_service.c](../../main/net/mdns_service.c) line 28. |
| 21 | WiFi boot check | If `wifi_on_boot` is set, defer WiFi start by 4 s. |

After splash, control transitions to the dashboard via `splash_screen` → `first_run_wizard` (if `first_run_done == false`) → `ui_Screen3_screen_init()` → `dashboard_init()`. See [06-ui-and-screens.md](06-ui-and-screens.md).

## Threading model

ESP32-S3 has two cores. The firmware uses both.

```
                CORE 0                              CORE 1
┌──────────────────────────────────┐  ┌──────────────────────────────────┐
│                                  │  │                                  │
│  can_receive_task   prio 7  4 KB │  │  LVGL task         prio 8  16 KB │
│    └─ TWAI RX → s_can_queue      │  │    ├─ Renders frame buffer       │
│                                  │  │    ├─ Drains s_can_queue         │
│  ind_wire           prio 3  2 KB │  │    │    via can_process_queued() │
│    └─ Polls GPIO 43/44           │  │    ├─ Runs lv_timer callbacks    │
│                                  │  │    ├─ Runs widget signal cbs     │
│  uart_rx_task       prio 5  6 KB │  │    └─ Holds lvgl_mux while doing │
│    └─ Serial command parser      │  │       all of the above           │
│                                  │  │                                  │
│  HTTP server        (system)     │  │  Soft timer task   (system)      │
│    └─ Spawned by httpd_start     │  │    └─ FREERTOS_TIMER_TASK_AFFINITY│
│                                  │  │       = CPU 1                    │
│  WiFi event task    (system)     │  │                                  │
│  DNS hijack         prio 3  3 KB │  │                                  │
│    (PSRAM stack — internal RAM   │  │                                  │
│     fragmented after WiFi init)  │  │                                  │
└──────────────────────────────────┘  └──────────────────────────────────┘
```

### LVGL mutex (`lvgl_mux`)

All `lv_*` calls require the lock. Helpers in [main/main.c](../../main/main.c) ~line 294:

```c
bool example_lvgl_lock(int timeout_ms);
void example_lvgl_unlock(void);
```

The mutex is **recursive** (`xSemaphoreCreateRecursiveMutex`), so re-entrant code paths are safe. Pass `-1` for `portMAX_DELAY`. CAN callbacks that need to update LVGL acquire it with `-1`; HTTP handlers use shorter timeouts (100–500 ms) and bail with `lv_async_call()` if they can't get it.

**Rules of thumb:**

- LVGL task code (callbacks fired from `lv_timer`, signal subscribers invoked via `signal_dispatch_frame`): **already locked**, do not re-lock outside the recursive case.
- Code on any other task (HTTP handler, web upload, CAN RX before queue): **lock before any `lv_*` call**, or use `lv_async_call()` to defer to LVGL task.

### `lv_async_call` — the cross-task escape hatch

`lv_async_call(callback, user_data)` schedules a callback to run on the LVGL task on the next iteration. The signal CAN-RX path uses this when posting from interrupt context; the web server uses it for hot-reload (`/api/layout/save` triggers `lv_async_call(_deferred_screen_reload, NULL)`).

### The 500 Hz tick gotcha

`CONFIG_FREERTOS_HZ = 500` means a tick is 2 ms. So `pdMS_TO_TICKS(1)` rounds down to **0** — a no-op. Anywhere you intend a "give other tasks a turn" yield, write `vTaskDelay(1)` (one tick = 2 ms) directly, not `vTaskDelay(pdMS_TO_TICKS(1))`.

This bug bit the boot path before the TWDT was raised from 5 s to 15 s. Yields are now sprinkled through `_instantiate_widgets` (every widget), `rdm_image_load` (after `fread`), and `font_manager_get` (after TTF create). See [09-conventions-and-pitfalls.md](09-conventions-and-pitfalls.md).

## Data flow

### CAN frame to widget update

```
TWAI peripheral
    │ ISR
    ▼
can_receive_task (core 0)
    │ twai_receive(5 ms timeout)
    ▼
s_can_queue (FreeRTOS queue, 64 frames, drops oldest on overflow)
    │
    ▼ (drained on LVGL task — core 1)
can_process_queued_frames()
    │
    ▼
signal_dispatch_frame(can_id, data, dlc)
    │ for each signal with matching can_id:
    │   raw = can_extract_bits(data, bit_start, bit_length, endian, is_signed)
    │   value = raw * scale + offset
    │   if changed or stale→fresh: notify_subscribers(signal_index)
    ▼
widget signal_cb(value, is_stale, w)
    │ updates w->type_data, calls lv_label_set_text(), etc.
    ▼
LVGL renders next frame at 16 ms refresh period (62.5 Hz)
```

The pipeline guarantees:

- **Single decoder.** Only `signal_dispatch_frame` knows about CAN bytes. Widgets work in physical units (kPa, °C, RPM).
- **Bounded latency.** Worst case is `5 ms (CAN recv) + 2 ms (queue dispatch) + 16 ms (next refresh) ≈ 23 ms`.
- **No callback in IRQ context.** Subscribers run on the LVGL task with `lvgl_mux` held, so they can call `lv_*` directly.

### Layout save and hot reload

```
Web editor (Studio or rdm7-desktop) saves edits
    │
    ▼ POST /api/layout/save (full JSON body, may include ?apply=0)
web_server.c handler
    │ writes /lfs/layouts/<name>.json
    │ if apply=1 (or absent):
    │   lv_async_call(_deferred_screen_reload, NULL)
    ▼ on LVGL task:
dashboard_init(parent)
    ├─ widget_registry_reset()
    ├─ signal_registry_reset()
    ├─ night_mode_clear_subscribers()    ← critical: drop stale ptrs
    ├─ layout_manager_load("active")
    │    ├─ _load_signals()              register CAN bindings
    │    └─ _instantiate_widgets()       factory + from_json + create + subscribe
    └─ remote_touch_init(disp)           lazy init virtual indev
```

The studio polls `/api/layout/version` every few seconds. On version bump, it auto-reloads the editor canvas if no unsaved edits. With `apply=0`, the save is silent (no LVGL repaint) — used by the auto-save path during rapid editing.

### Widget tap to config modal

```
GT911 touch ISR
    │
    ▼ LVGL indev poll
LVGL event LV_EVENT_PRESSED on widget
    │
    ▼ short tap → screen3_touch_event_cb shows MENU button
    ▼ long press (>400 ms) → _widget_long_press_cb (dashboard.c)
load_menu_screen_for_widget(w)
    │
    ▼
config_modal_open_for_widget(screen, w)
    │ tabs: Signal (CAN ID, bits, scale, offset, endian), Alerts (thresholds)
    │ reads/writes via config_bridge_*() functions
    ▼ on Save:
dashboard_persist_layout()
    │ widget_t[].to_json() → cJSON → write /lfs/layouts/<active>.json
    └─ reconfigure_can_filter() if signal config changed
```

## Subsystem boundaries

```
┌──────────────────────────────────────────────────────────────────┐
│                          UI / Screens                            │
│  ui_Screen3 · settings · diagnostics · peaks · wifi · ecu picker │
│  · config_modal · first_run_wizard · splash_screen               │
└─────────────────┬────────────────────────────────────────────────┘
                  │
       ┌──────────▼──────────┐    ┌────────────────────┐
       │     Dashboard       │◀──▶│   night_mode       │
       │   (widget host,     │    │ (subscriber list)  │
       │    layout reload)   │    └────────────────────┘
       └──────────┬──────────┘
                  │
       ┌──────────▼─────────────────┐
       │   Widget registry          │  one widget_t per instance
       │   13 widget types          │  (panel, rpm_bar, bar, …)
       └──────────┬─────────────────┘
                  │ subscribe()
       ┌──────────▼──────────┐    ┌────────────────────┐
       │   Signal registry   │◀──▶│  Internal signals  │
       │   (signal_t × 128)  │    │  (FPS, CPU, gear)  │
       └──────────┬──────────┘    └────────────────────┘
                  │
       ┌──────────▼──────────┐
       │   CAN dispatch      │
       │  (queue → decode)   │
       └──────────┬──────────┘
                  │
       ┌──────────▼──────────┐
       │   TWAI driver       │
       └─────────────────────┘
```

A widget never reaches "down" the stack — it never opens a CAN frame, never reads NVS directly, never touches LittleFS. All cross-cutting concerns flow through the registries.

## Memory layout

| Region | Used for |
|---|---|
| Internal SRAM (~512 KB) | DMA buffers, WiFi, FreeRTOS, IDF stacks. **Keep allocs small.** |
| PSRAM (8 MB) | Heap for: widget allocations, font cache, frame buffer (when double-FB), JSON parsing, layout strings. Use `MALLOC_CAP_SPIRAM`. |
| Flash (16 MB) | Code (OTA-A or OTA-B, 3.5 MB each), partition tables (~150 KB), LittleFS (8.8 MB) |

Run-time heap inspection: `/api/system/health` returns `heap_free`, `heap_total`. The diagnostics screen exposes the same plus PSRAM free.

## What you do NOT have to know to be productive

- The exact RGB LCD timing values (set once in `main.c`, rarely touched).
- The TWAI bit-timing arithmetic (handled by `can_set_bitrate` indices).
- The internals of LVGL's draw pipeline (we only consume the v8 public API).
- The LittleFS block layout (just use the `fopen`/`fread`/`fwrite` interface).

But you **do** have to know the LVGL v8 styling API, the cJSON object model, FreeRTOS tasks/queues/mutexes, and how cooperative scheduling on a 500 Hz tick affects yields.

---

Next: [02-build-and-flash.md](02-build-and-flash.md).
