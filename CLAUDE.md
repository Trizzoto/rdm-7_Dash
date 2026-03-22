# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# RDM-7 Dash

ESP32-S3 automotive dashboard built with **ESP-IDF v5.x** and **LVGL v8** (NOT v9).
800×480 RGB LCD, CAN bus data via signal-based pub/sub, JSON layouts on LittleFS,
web editor over WiFi. Target: ESP32-S3, 8 MB PSRAM, 8 MB flash, GT911 touch.

---

## Build & Flash

- **Build:** `idf.py build` (requires `source $IDF_PATH/export.sh`)
- **Flash:** `idf.py -p COMx flash monitor`
- **Do NOT run `idf.py build` without asking** — user builds externally
- All source files must be listed in `main/CMakeLists.txt` SRCS
- `main/web/index.html` embedded via `EMBED_TXTFILES` (symbols: `index_html_start/end`)
- **`-Werror=comment` is active** — no `/*` inside block comments
- Partition table in `partitions.csv`: dual OTA + LittleFS at `/lfs`

---

## Threading Rules (Critical)

- **LVGL is single-threaded.** All `lv_*` calls must hold LVGL mutex (`example_lvgl_lock`)
- LVGL task runs on core 1 (14 ms tick, priority 8)
- CAN RX task on core 0 (priority 7) enqueues to `s_can_queue`; LVGL task drains via `can_process_queued_frames()`
- Signal callbacks run on LVGL task — safe for direct LVGL calls
- Use `lv_async_call()` to defer work to LVGL task from other contexts (e.g., web server)

---

## Coordinate System

- **(0, 0) = screen center** on 800×480 display
- X: -400 (left) to +400 (right), Y: -240 (top) to +240 (bottom)
- Widget x,y = center of widget
- **All widgets must call `lv_obj_set_align(obj, LV_ALIGN_CENTER)` before `lv_obj_set_pos()`**
- Web editor converts via `devToWeb()`/`webToDev()` with `ORIGIN_X=400, ORIGIN_Y=240`

---

## Widget System

12 widget types defined in `widget_type_t` enum (`widget_types.h`). Slot-limited types:
panel (8), bar (2), indicator (2), warning (8). Others are unlimited.

### Lifecycle

1. **Factory** (`widget_X_create_instance(slot)`) — alloc `widget_t`, set vtable, alloc `type_data`, register in registry
2. **from_json()** — deserialize config from JSON; resolve signal name→index; does NOT create LVGL objects
3. **create()** — build LVGL objects; subscribe to signals **after** `w->root` is set
4. **to_json()** — serialize back to JSON (**defaults-only**: skip fields matching factory defaults to conserve 16 KB budget)
5. **destroy()** — free `type_data`, rules, `widget_t` (LVGL objects freed by parent deletion)

Updates are **push-based** via signal callbacks (no polling).

### Defaults-Only Serialization Pattern

- `to_json()` only writes fields that differ from factory defaults
- `from_json()` reads-if-present, else keeps defaults
- `create()` applies per-instance styles via `lv_obj_set_style_*()` (not shared `lv_style_t`)
- This ensures forward compatibility and conserves the 16 KB JSON file budget

### Conditional Rules

Widgets can override config fields based on signal conditions. See `widget_rules.c/h`.
- `from_json`: call `widget_rules_from_json(w, config)`
- `create`: call `widget_rules_subscribe(w)` after LVGL objects exist
- `to_json`: call `widget_rules_to_json(w, config)`
- `destroy`: call `widget_rules_free(w)`
- Widget must set `apply_overrides` vtable function to support rules

---

## Signal System

Signal registry (`main/widgets/signal.c/h`) is the **sole CAN decode layer**.
Each signal decodes a CAN bit-field and pushes results to subscribers.
Signals are defined in layout JSON and registered at layout load time.

### Widget-Signal Integration Pattern

```c
// In from_json(): resolve signal name to registry index
int16_t idx = signal_find_by_name("RPM");
type_data->signal_index = idx;

// In create(): subscribe after w->root is set
if (type_data->signal_index >= 0)
    signal_subscribe(type_data->signal_index, _on_signal_cb, widget);

// Callback runs on LVGL task (safe for direct LVGL calls)
static void _on_signal_cb(float value, bool is_stale, void *user_data) {
    widget_t *w = user_data;
    if (is_stale) { /* show "---" */ }
    else { /* update display with value */ }
}
```

- Signals timeout after 2s (marked stale via `signal_check_timeouts()`)
- Internal signals (`signal_internal.c`) inject GPIO/ADC values via LVGL timer (500 ms)
- Signal simulator (`signal_sim.c`) enables testing without CAN hardware

---

## CAN Pipeline

```
CAN RX Task (core 0) → twai_receive() → s_can_queue (32 entries)
    ↓
LVGL Task (core 1) → can_process_queued_frames() (max 8/tick)
    → signal_dispatch_frame() → can_extract_bits() → scale/offset → notify subscribers
```

- `can_extract_bits()` in `can_decode.h`: pure stateless bit extraction (endian: 0=Motorola, 1=Intel)
- `build_twai_filter_from_signals()` builds hardware acceptance filter from registered signals
- CAN TX: toggle/button widgets can transmit (disabled when `tx_can_id` is 0)
- CAN config (bitrate 125/250/500/1000 kbps) stored in NVS

---

## Storage & Persistence

| Data                    | Storage       | Key API                                    |
|-------------------------|---------------|--------------------------------------------|
| Layouts + signals       | LittleFS JSON | `layout_manager_save/load()` at `/lfs/layouts/` |
| Images                  | LittleFS      | `/lfs/images/<name>.rdmimg`                |
| Fonts (TTF)             | LittleFS      | `/lfs/fonts/<name>.ttf`                    |
| Active layout name      | NVS           | `rdm_settings_set/get_active_layout()`     |
| Brightness, WiFi, CAN   | NVS           | `config_store_*()` functions               |
| SD card files           | FAT           | `/sdcard/layouts/`, `/sdcard/images/`, `/sdcard/fonts/` |

**All CAN signal config is in layout JSON** — not NVS. Editing a signal means saving the entire layout.

### Config Bridge (`config_bridge.c/h`)

Maps **value_id** (1–8: panels, 9: RPM bar, 10–11: bars) to widget `type_data` + signal registry.
Sole interface for touchscreen config modal. Uses real `type_data` structs directly via `#include`.
Widget lookup: `widget_registry_find_by_type_and_slot(type, slot)`.

---

## Config & Reload Flows

### Touchscreen Config
```
Widget tap → config_modal_open(value_id) → config_bridge reads type_data + signal
→ User edits → config_bridge writes back → dashboard_persist_layout() → LittleFS
```

### Web Editor Save → Hot Reload
```
POST /api/layout/save → validate JSON → layout_manager_save_raw() → LittleFS
→ lv_async_call() → dashboard_init() → font_manager_reset + signal_registry_reset
→ widget factory + from_json + create → build_twai_filter → signal_internal_start()
```

Web UI polls `/api/layout/current` every 3s for live sync.

---

## Color Conversion (Critical Gotcha)

Firmware uses **RGB565** (16-bit); web editor uses **RGB888** (24-bit):
- `rgb565to888()` — on load from device
- `rgb888to565()` — before sending to device
- `convertWidgetColors(widgets, convertFn)` — auto-walks all `type: 'color'` fields in `WIDGET_DEFS`

---

## Font System

- Dynamic TTF loading via `lv_tiny_ttf` — scans `/lfs/fonts/` on boot
- Cache: max 8 TTF families (PSRAM) + max 32 rendered size instances
- `font_manager_get(family, size)` → `lv_font_t*`
- `widget_resolve_font(name)` maps JSON strings:
  - New format: `"Family:size"` (e.g., `"Fugaz:28"`)
  - Legacy format: `"fugaz_28"`, `"montserrat_16"` — compiled-in fonts
- `font_manager_reset_instances()` called on layout reload

---

## Image System (RDMIMG)

Custom binary format: 12-byte header ("RDMI" magic + w/h/cf) + RGB565+alpha pixel data (3 bytes/pixel).
- Max upload: 1200 KB (`IMAGE_MAX_SIZE`)
- Pixel data in PSRAM — must free in widget `destroy()`
- Web editor converts PNG/JPG → RDMIMG client-side via canvas

---

## Web Editor Architecture

- `WIDGET_DEFS` object defines widget metadata + `fields[]` array per type
- Field types: `text`, `number`, `stepper`, `color`, `hex`, `checkbox`, `select`, `font`, `image_picker`
- `buildFirmwarePayload()`: maps `w.signal` → `config.signal_name`, converts colors 888→565, converts TX data strings → byte arrays
- Slot management: `SLOT_LIMITS = { panel: 8, bar: 2, indicator: 2, warning: 8 }`
- **`data/web/index.html` is a copy of `main/web/index.html`** — keep them in sync

---

## Web Server Endpoints

HTTP server on port 80 (`web_server.c`). Key endpoint groups:
- **Layout:** `/api/layout/current`, `/api/layout/save`, `/api/layout/list`, `/api/layout/set`, `/api/layout/delete`
- **Images:** `/api/image/upload`, `/api/image/list`, `/api/image/delete`, `/api/image/data`
- **Fonts:** `/api/font/upload`, `/api/font/list`, `/api/font/delete`
- **SD Card:** `/api/sd/status`, `/api/sd/files`, `/api/sd/copy`, `/api/sd/delete`
- **Signals:** `/api/signal/simulate` (POST to toggle, GET to check status), `/api/signal/inject` (POST single/batch test values)
- **Other:** `/screenshot`, `/api/storage/info`, `/api/presets`

---

## Coding Conventions

- C11, `snake_case`, `UPPER_SNAKE_CASE` for macros/defines
- Module prefix: `widget_panel_`, `signal_`, `layout_manager_`, `config_bridge_`
- Logging: `ESP_LOGI/W/E/D` with static `TAG`
- Headers: `#pragma once` + `extern "C"` guards
- **LVGL v8** — use `lv_obj_set_style_*()` (not v9 `lv_obj_set_style()` with prop+selector)
- Per-instance styling via `lv_obj_set_style_*()` directly, not shared `lv_style_t`
- Memory: `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` for large allocs, standard `calloc` for small

---

## Adding a New Widget Type

1. Create `widget_newtype.c/h` — define `newtype_data_t` **in the header**
2. Implement vtable: `create`, `resize`, `open_settings`, `to_json`, `from_json`, `destroy` (+ optional `apply_overrides`)
3. In `create`: `lv_obj_set_align(obj, LV_ALIGN_CENTER)` before `lv_obj_set_pos()`, subscribe signals after `w->root` set
4. In `from_json`: `signal_find_by_name()` + `widget_rules_from_json()`
5. In `to_json`: defaults-only serialization + `widget_rules_to_json()`
6. In `destroy`: `widget_rules_free()`
7. Add factory: `widget_newtype_create_instance(uint8_t slot)`
8. Register in: `widget_type_t` enum (`widget_types.h`), `widget_constraints[]` + `widget_type_name()` (`widget_types.c`), `_type_from_str()` + `_factory()` (`layout_manager.c`)
9. Add `.c` to `main/CMakeLists.txt` SRCS
10. Add to `WIDGET_DEFS` in `main/web/index.html` + sync `data/web/index.html`
11. Bump `LAYOUT_SCHEMA_VERSION` in `layout_manager.h` if schema changes
12. Optional: add value_id mapping in `config_bridge.c` for touchscreen config

---

## Key Debugging Tips

- **Widget not updating:** Signal not registered in JSON, or `signal_subscribe()` not called in `create()`, or CAN ID mismatch
- **Signal stale:** CAN frame not received in 2s — check hardware filter (`build_twai_filter_from_signals()`)
- **LVGL crash:** Threading issue — ensure LVGL mutex held for all `lv_*` calls
- **Widget position wrong:** Missing `lv_obj_set_align(obj, LV_ALIGN_CENTER)` call
- **Colors wrong after save:** Check `convertWidgetColors()` direction (565→888 on load, 888→565 on save)
- **New appearance field not saved:** Check `to_json` defaults-only logic and `from_json` reads it
- **Config bridge breaks:** Uses real `type_data` structs — field changes need config_bridge update
- **Rules not working:** Check `apply_overrides` vtable set + `widget_rules_subscribe()` called after `create()`
- **Layout not loading:** Check LittleFS mounted, schema version matches `LAYOUT_SCHEMA_VERSION`
- **Config changes lost:** Ensure `dashboard_persist_layout()` called after touchscreen edits
