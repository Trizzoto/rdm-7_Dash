# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# RDM-7 Dash — Claude Code Project Guide

## Project Overview

RDM-7 Dash is an **ESP32-S3 automotive dashboard** built with **ESP-IDF** and **LVGL v8**.
It renders a configurable widget-based dashboard on an 800×480 RGB LCD, driven by real-time
CAN bus data via a **signal-based publish-subscribe system**. Layouts (widgets + signals)
are stored as JSON on a LittleFS partition and can be edited via the touchscreen config modal
or a built-in web UI served over WiFi. Custom images can be uploaded from the web editor
and rendered on the dashboard via the Image widget.

**Target hardware:** ESP32-S3 (dual-core Xtensa LX7), 8 MB PSRAM, 8 MB flash, GT911 touch.

---

## Build Environment

- **Framework:** ESP-IDF (v5.x) — C language, CMake-based build system
- **LVGL version:** 8.x (NOT v9) — `lv_conf.h` in `main/`, config via `sdkconfig.defaults`
- **Build command:** `idf.py build` (requires `source $IDF_PATH/export.sh` first)
- **Flash command:** `idf.py -p COMx flash monitor`
- **Partition table:** `partitions.csv` — dual OTA banks + LittleFS (see below)
- **LittleFS image:** NOT auto-built — no `littlefs_create_partition_image` in project CMake

### Important Build Notes

- User may build externally — do NOT run `idf.py build` without asking first
- All source files must be listed in `main/CMakeLists.txt` SRCS to be compiled
- Include paths are set in `main/CMakeLists.txt` INCLUDE_DIRS
- `web/index.html` is embedded via `EMBED_TXTFILES` (accessible as `index_html_start/end` symbols)
- Compile flag: `-Wno-format` set at project level
- **`-Werror=comment` is active** — avoid `/*` inside block comments

---

## Architecture

### Flash Partition Layout (`partitions.csv`)

| Partition  | Type   | Offset     | Size       | Purpose                    |
|------------|--------|------------|------------|----------------------------|
| nvs        | data   | 0x9000     | 0x24000    | NVS key-value storage      |
| otadata    | data   | 0x2D000    | 0x2000     | OTA boot selection         |
| phy_init   | data   | 0x2F000    | 0x1000     | WiFi PHY calibration       |
| ota_0      | app    | 0x30000    | 0x290000   | OTA app slot 0 (2.56 MB)  |
| ota_1      | app    | —          | 0x290000   | OTA app slot 1 (2.56 MB)  |
| littlefs   | data   | 0x550000   | 0x2B0000   | LittleFS (layouts, images) |

### Task Model (FreeRTOS)

| Task            | Core | Priority | Stack | Purpose                                |
|-----------------|------|----------|-------|----------------------------------------|
| LVGL            | 1    | 8        | 8 KB  | Display refresh @ 70 fps (14 ms tick)  |
| CAN RX          | 0    | 7        | 4 KB  | TWAI receive → `s_can_queue`           |
| indicator_wire  | 0    | 3        | 2 KB  | GPIO 43/44 analog indicator polling    |
| fuel_sender     | 0    | 3        | 3 KB  | ADC on GPIO 6 → bar widget update      |

### Threading Rules

- **LVGL is single-threaded.** All LVGL API calls must hold the LVGL mutex (`example_lvgl_lock`).
- CAN RX task enqueues to `s_can_queue`; the LVGL task drains it via `can_process_queued_frames()`.
- Signal callbacks run on the LVGL task — safe to call LVGL directly.
- Use `lv_async_call()` to defer work to the LVGL task from other contexts (e.g., web server save triggers screen reload).

---

## Directory Structure

```
main/
├── main.c                          # app_main: HW init, task creation, boot sequence
├── CMakeLists.txt                  # Component registration (SRCS, INCLUDE_DIRS, REQUIRES)
├── lv_conf.h                       # LVGL 8 configuration header
├── include/
│   └── version.h                   # Firmware version defines
│
├── can/                            # CAN bus abstraction
│   ├── can_manager.c/h             # TWAI driver lifecycle, RX task, bitrate config, filter builder
│   └── can_decode.c/h              # Pure stateless bit extraction (host-testable)
│
├── widgets/                        # Widget system
│   ├── widget_types.c/h            # Core widget_t struct, vtable, size constraints, font resolver
│   ├── widget_registry.c/h         # Central widget instance tracking (max 32)
│   ├── widget_panel.c/h            # Data display boxes (8 slots, customizable appearance)
│   ├── widget_rpm_bar.c/h          # RPM gauge with redline zone (singleton)
│   ├── widget_bar.c/h              # Horizontal bar graphs (2 slots, customizable appearance)
│   ├── widget_indicator.c/h        # Turn indicators (2 slots: left, right)
│   ├── widget_warning.c/h          # Warning lights (8 slots, customizable appearance)
│   ├── widget_text.c/h             # Generic text value display
│   ├── widget_meter.c/h            # Analog dial meter (customizable ticks, needle, arc zones)
│   ├── widget_image.c/h            # Image display from LittleFS (RDMIMG format)
│   └── signal.c/h                  # Signal registry — pub/sub CAN decode layer
│
├── layout/                         # Layout persistence
│   ├── layout_manager.c/h          # Load/save JSON layouts on LittleFS
│   ├── layout_loader.c/h           # High-level layout loading API
│   └── default_layout.c/h          # First-boot default layout generator
│
├── ui/                             # UI layer
│   ├── ui.c/h                      # Main UI entry, screen transitions
│   ├── dashboard.c/h               # Dashboard coordinator — loads layout, signal timeout timer
│   ├── config_bridge.c/h           # Value ID ↔ widget type_data + signal registry mapper
│   ├── signals.c/h                 # Signal definitions (89 predefined signals)
│   ├── ui_styles.c/h               # LVGL style definitions
│   ├── ui_helpers.c/h              # UI utility functions
│   ├── utils.c/h                   # General utilities
│   ├── global.h                    # Global UI declarations
│   ├── theme.h                     # Theme constants (colors, fonts)
│   ├── panel_config.h              # Panel configuration types
│   ├── ui_events.h                 # UI event definitions
│   ├── lvgl_helpers.h              # LVGL helper macros
│   ├── callbacks/
│   │   └── ui_callbacks.c/h        # Touch/event callbacks
│   ├── components/
│   │   ├── ui_comp.c/h             # Custom LVGL components
│   │   ├── ui_comp_hook.c/h        # Component hook system
│   │   └── ui_comp_slider2.c/h     # Custom slider component
│   ├── config/
│   │   └── config_controls.c/h     # CAN config form controls
│   ├── screens/
│   │   ├── ui_Screen1.c/h          # Screen 1
│   │   ├── ui_Screen2.c            # Screen 2
│   │   ├── ui_Screen3.c/h          # Main dashboard screen
│   │   ├── ui_Screen4.c            # Screen 4
│   │   ├── splash_screen.c/h       # Boot splash
│   │   └── ui_wifi.c/h             # WiFi configuration screen
│   ├── settings/
│   │   ├── device_settings.c/h     # Device settings UI (brightness, WiFi, CAN bitrate)
│   │   ├── preset_picker.c/h       # Layout preset picker
│   │   └── settings_panel.c/h      # Settings panel UI
│   ├── menu/
│   │   ├── menu_screen.c/h         # Main config menu overlay
│   │   ├── config_modal.c/h        # CAN config modal dialog (3 tabs: Signal, Display, Alerts)
│   │   └── gear_config.c/h         # Gear ratio configuration
│   ├── fonts/                      # LVGL font C files (Fugaz, Manrope)
│   └── images/                     # LVGL image C files
│
├── net/                            # Network
│   ├── web_server.c/h              # HTTP server on port 80 (layout + image APIs)
│   ├── ota_handler.c/h             # OTA firmware updates
│   └── ota_update_dialog.c/h       # OTA progress UI
│
├── web/
│   └── index.html                  # Embedded main web UI (via EMBED_TXTFILES)
│
├── storage/
│   └── config_store.c/h            # NVS-backed persistence (brightness, WiFi, bitrate, ECU preset)
│
├── system/
│   ├── device_id.c/h               # Unique device identifier
│   ├── rdm_settings.c/h            # RDM-specific settings (active layout name in NVS)
│   ├── display_capture.c/h         # Screenshot capture
│   └── storage.h                   # Storage interface
│
├── io/
│   └── wire_inputs.c/h             # GPIO-based analog indicator control
│
└── design_mode/
    └── CMakeLists.txt              # Design mode component (stub, not yet implemented)

data/                               # LittleFS image source (mounted at /lfs)
├── editor.html                     # Standalone editor page
├── web/
│   └── index.html                  # Visual layout editor (copy of main/web/index.html — keep in sync)
└── layouts/
    └── rpm_meter_test.json         # Test layout preset
```

---

## Coordinate System

The dashboard uses a **center-origin** coordinate system:
- **(0, 0)** is the **center** of the 800×480 screen
- X range: -400 (left edge) to +400 (right edge)
- Y range: -240 (top edge) to +240 (bottom edge)
- Widget x,y values represent the **center** of the widget

All widgets must use `lv_obj_set_align(obj, LV_ALIGN_CENTER)` before `lv_obj_set_pos()`.
The web editor converts between center-origin device coords and top-left web canvas coords
via `devToWeb()`/`webToDev()` using `ORIGIN_X=400, ORIGIN_Y=240`.

---

## Widget System

### Widget Type Enum

```c
typedef enum {
    WIDGET_PANEL     = 0,   // Data display boxes (max 8)
    WIDGET_RPM_BAR   = 1,   // RPM gauge with redline (singleton)
    WIDGET_BAR       = 2,   // Horizontal bar (max 2: BAR1, BAR2)
    WIDGET_INDICATOR = 3,   // Turn indicators (max 2: left, right)
    WIDGET_WARNING   = 4,   // Warning lights (max 8)
    WIDGET_TEXT      = 5,   // Text value (any value slot)
    WIDGET_METER     = 6,   // Analog dial meter
    WIDGET_IMAGE     = 7,   // Image from LittleFS
    WIDGET_TYPE_COUNT
} widget_type_t;
```

### Widget Lifecycle

1. **Factory** (`widget_X_create_instance(slot)`) — allocates `widget_t`, sets vtable, allocates `type_data`, registers in registry
2. **from_json()** — deserializes x/y/w/h and type-specific config from JSON; resolves signal name→index; does NOT create LVGL objects
3. **create()** — builds LVGL object tree on parent; sets styles, callbacks; subscribes to signals after `w->root` is set
4. **to_json()** — serializes widget state back to JSON (defaults-only: skips fields matching factory defaults)
5. **destroy()** — frees `type_data` and `widget_t` (LVGL objects freed by parent deletion)

Widget updates are **push-based** via signal callbacks (no polling/update vtable function).

### Core Struct (`widget_t`)

```c
struct widget_t {
    widget_type_t type;
    lv_obj_t     *root;          // Top-level LVGL container
    int16_t       x, y;          // Layout position (center-origin, px)
    uint16_t      w, h;          // Layout size (px)
    char          id[16];        // Instance identifier ("panel_0", "bar_1", etc.)
    uint8_t       slot;          // Slot index (e.g. panel 0-7)
    void         *type_data;     // Per-instance state (cast to widget-specific struct)

    // vtable (no update function — widgets use signal callbacks)
    widget_create_fn        create;
    widget_resize_fn        resize;
    widget_open_settings_fn open_settings;
    widget_to_json_fn       to_json;
    widget_from_json_fn     from_json;
    widget_destroy_fn       destroy;
};
```

### Size Constraints (for editor validation)

| Widget     | Min W×H   | Max W×H    |
|------------|-----------|------------|
| Panel      | 80×40     | 250×130    |
| RPM Bar    | 300×30    | 800×80     |
| Bar        | 120×15    | 450×50     |
| Indicator  | 30×30     | 80×80      |
| Warning    | 18×18     | 60×60      |
| Text       | 40×20     | 400×100    |
| Meter      | 80×80     | 800×800    |
| Image      | 10×10     | 800×480    |

### Widget Appearance Customization

Widgets with per-instance LVGL style overrides use a **defaults-only serialization** pattern:
- `to_json()` only writes fields that differ from factory defaults (conserves 16 KB JSON budget)
- `from_json()` reads-if-present, else keeps factory defaults (v8 layouts load cleanly in v9)
- `create()` applies per-instance styles via `lv_obj_set_style_*()` calls (not shared style objects)

**Panel** appearance fields: `border_radius` (7), `border_width` (3), `border_color`, `bg_color`,
`bg_opa` (255), `label_color`, `value_color`, `label_y_offset` (-28), `value_y_offset` (9)

**Bar** appearance fields: `bar_bg_color`, `bar_radius` (5), `bar_border_width` (2),
`bar_border_color`, `indicator_radius` (5), `label_color`, `value_color`

**Warning** appearance fields: `inactive_color`, `border_width` (0), `border_color`,
`radius` (100=circle), `show_label` (true), `label_color`
*(Note: JSON key for warning border color is `"border_color_style"` to avoid collision)*

**Meter** appearance fields: tick config (count, width, length, colors for major/minor),
needle (width, color, r_mod), value/ID label (show, x/y offset, color), background (color, opa),
3 arc color zones (enabled, start, end, color for green/yellow/red)

### Font Resolution

`widget_resolve_font(name)` in `widget_types.c` maps JSON font name strings to `lv_font_t*`:
- Montserrat system fonts: `"montserrat_8"` through `"montserrat_24"`
- Custom fonts: `"fugaz_14"`, `"fugaz_17"`, `"fugaz_28"`, `"fugaz_56"`, `"manrope_35_bold"`, `"manrope_54_bold"`

---

## Image System

### RDMIMG Binary Format

Custom binary format stored at `/lfs/images/<name>.rdmimg`:

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];    // "RDMI"
    uint16_t width;       // little-endian
    uint16_t height;      // little-endian
    uint8_t  cf;          // LV_IMG_CF_TRUE_COLOR_ALPHA = 5
    uint8_t  reserved[3];
} rdm_image_header_t;
// Followed by width * height * 3 bytes (RGB565 + alpha)
```

- RGB565 + alpha = 3 bytes/pixel. 100×100 image ≈ 30 KB, 200×200 ≈ 120 KB
- Max file size: 200 KB (`IMAGE_MAX_SIZE` in web_server.c)
- Pixel data stored in PSRAM at runtime via `lv_img_dsc_t`

### Browser-Side Image Conversion

The web editor converts PNG/JPG to RDMIMG client-side:
1. User picks image file → resize dialog with width/height controls and aspect ratio lock
2. Image drawn to `<canvas>` at target dimensions → `getImageData()` for RGBA8888 pixels
3. JavaScript converts RGBA8888 → RGB565 + alpha, prepends RDMIMG header
4. Binary POSTed to `/api/image/upload?name=<name>`

### Image Widget (`widget_image.c/h`)

```c
typedef struct {
    char          image_name[32];   // Filename (without .rdmimg extension)
    uint8_t       opacity;          // 0-255, default: 255
    lv_img_dsc_t *img_dsc;          // Runtime: PSRAM-loaded descriptor
    lv_obj_t     *img_obj;          // Runtime: LVGL image object
} image_data_t;
```

- `create()`: Reads RDMIMG from `/lfs/images/`, validates magic, allocates PSRAM pixel buffer,
  creates `lv_img_dsc_t` with `LV_IMG_CF_TRUE_COLOR_ALPHA`, shows placeholder if not found
- `destroy()`: Frees PSRAM pixel data + descriptor

---

## CAN Bus Pipeline

### Data Flow

```
CAN RX Task (core 0, priority 7)
  → twai_receive() every 5 ms
  → enqueue to s_can_queue (FreeRTOS queue, 32 entries)
      ↓
LVGL Task (core 1, 14 ms refresh)
  → can_process_queued_frames()          [drains queue, max 8 frames per tick]
      → signal_dispatch_frame(id, data, dlc)  [decode + notify subscribers]
          → can_extract_bits()                [stateless bit extraction]
          → apply scale/offset                [decoded = raw * scale + offset]
          → notify_subscribers()              [push value to all widget callbacks]
              → widget signal callbacks       [direct LVGL updates, safe on LVGL task]
```

**All CAN processing is signal-centric.** There is no legacy dispatch table or widget routing —
every CAN frame is decoded by the signal registry and pushed to subscribed widgets.

### CAN Decode (`can_decode.h`)

Pure stateless extraction — no ESP-IDF or LVGL dependencies:
```c
int64_t can_extract_bits(const uint8_t *data, uint8_t bit_offset,
                         uint8_t bit_length, int endian, bool is_signed);
// endian: 0 = Motorola (big-endian), 1 = Intel (little-endian)
```

### CAN Hardware Filter

`build_twai_filter_from_signals()` in `can_manager.c` scans all registered signals to build
a TWAI hardware acceptance filter. Only CAN IDs with registered signals pass through to firmware.
Called after layout load and when CAN bitrate changes.

### CAN Configuration (NVS)

- Bitrate stored in NVS (`can_config` namespace): 125/250/500/1000 kbps (default 500)
- TX pin: GPIO 20, RX pin: GPIO 19
- Standard 11-bit CAN IDs (0x000–0x7FF)

---

## Signal System

### Overview

The signal registry (`signal.c/h` in `main/widgets/`) is the **sole CAN decode layer**.
Each signal decodes its CAN bit-field once per frame and pushes the result to all subscribers.
Signals are defined in the layout JSON and registered at layout load time.

### Key Types

```c
typedef void (*signal_update_cb_t)(float value, bool is_stale, void *user_data);

typedef struct {
    char     name[32];
    uint32_t can_id;
    uint8_t  bit_start, bit_length;
    float    scale, offset;
    bool     is_signed;
    uint8_t  endian;           // 0 = Motorola (big-endian), 1 = Intel (little-endian)
    float    current_value;
    bool     is_stale;
    uint64_t last_update_ms;
    signal_subscriber_t subscribers[MAX_SIGNAL_SUBSCRIBERS];  // max 8 per signal
    uint8_t  subscriber_count;
} signal_t;
```

### API

- `signal_registry_init()` — allocate PSRAM-backed array (MAX_SIGNALS=128)
- `signal_registry_reset()` — clear all signals (called at top of `layout_manager_load()`)
- `signal_register(name, can_id, start, len, scale, offset, is_signed, endian)` → index
- `signal_find_by_name(name)` → index or -1
- `signal_subscribe(index, callback, user_data)` → bool
- `signal_dispatch_frame(can_id, data, dlc)` — called from LVGL task per CAN frame
- `signal_check_timeouts(current_time_ms)` — mark stale after SIGNAL_TIMEOUT_MS (2000 ms)

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

### Signal Timeout

`dashboard.c` creates an LVGL timer (500 ms) that calls `signal_check_timeouts()`.
Signals not updated within 2 seconds are marked stale, triggering subscriber callbacks
with `is_stale=true` so widgets can show fallback displays.

---

## Configuration & Persistence

### What's Stored Where

| Data                    | Storage          | Mechanism                              |
|-------------------------|------------------|----------------------------------------|
| Widget layout + signals | LittleFS JSON    | `layout_manager_save/load()` at `/lfs/layouts/` |
| Uploaded images         | LittleFS binary  | `/lfs/images/<name>.rdmimg`            |
| Active layout name      | NVS              | `rdm_settings_set/get_active_layout()` |
| Brightness/dimmer       | NVS              | `config_store_save/load_dimmer()`      |
| WiFi credentials        | NVS              | `config_store_save/load_wifi()`        |
| CAN bitrate             | NVS              | `config_store_save/load_bitrate()`     |
| ECU preset selection    | NVS              | `config_store_save/load_ecu_preset()`  |

**All CAN signal config (IDs, bit fields, scale, offset) is stored in the layout JSON** —
not in NVS. When a user edits a signal via the config modal or web UI, the change is
persisted by saving the entire layout JSON to LittleFS.

### Config Bridge (`config_bridge.c/h`)

The config bridge maps **value_id** (1–11) to the corresponding widget's `type_data`
and signal registry entry. This is the **sole interface** used by the touchscreen config
modal to read/write widget and signal settings.

**Value ID mapping:**
| Value ID | Widget Type    | Slot |
|----------|---------------|------|
| 1–8      | WIDGET_PANEL  | 0–7  |
| 9        | WIDGET_RPM_BAR| —    |
| 10–11    | WIDGET_BAR    | 0–1  |

**Key functions:**
- `config_bridge_get_widget(value_id)` — find widget_t by value_id
- `config_bridge_ensure_signal(value_id)` — auto-create signal in registry if missing
- Signal accessors: `get/set_can_id()`, `get/set_bit_start()`, `get/set_bit_length()`, `get/set_scale()`, `get/set_offset()`, `get/set_endian()`, `get/set_is_signed()`
- Display accessors: `get/set_label()`, `get/set_decimals()`, `get/set_custom_text()`
- Bar-specific: `get/set_bar_min/max/low/high()`, fuel sender accessors
- RPM-specific: `get_rpm_bar_color()`, `get_rpm_limiter_*()`, `get_rpm_background_*()`
- Panel alert: `get/set_warning_high/low_threshold/color/enabled()`

**Note:** Config bridge includes widget headers directly and casts `type_data` to the real
`type_data` structs (e.g., `panel_data_t`, `bar_data_t`). No mirror structs — changes to
widget `type_data` fields are immediately visible to config_bridge.
Widget lookup uses `widget_registry_find_by_type_and_slot(type, slot)`.

### Touchscreen Config Flow

```
User taps widget → config_modal_open(screen, value_id)
  → config_bridge reads widget type_data + signal registry
  → 3-tab modal: CAN Signal | Display | Alerts
  → User edits values
  → config_bridge writes back to type_data + signal registry
  → dashboard_persist_layout() → layout_manager_save() → LittleFS JSON
```

### Web UI Config Flow

```
Web editor loads → GET /api/layout/current (live in-memory state)
  → rgb565to888() converts all color fields for browser display
  → maps config.signal_name → w.signal for web editor display
  → User edits widgets + signals in browser
  → buildFirmwarePayload() maps w.signal → config.signal_name
  → rgb888to565() converts colors back for firmware
  → POST /api/layout/save → layout_manager_save_raw()
  → lv_async_call() defers screen reload to LVGL task
  → ui_Screen3_screen_init() → dashboard_init() → full re-creation
```

Web UI polls `/api/layout/current` every 3 seconds for live sync with device changes.
Shows warning if local edits are pending when device state changes.

---

## Layout JSON Schema

### Current Schema (v9 — `LAYOUT_SCHEMA_VERSION` in `layout_manager.h`)

```json
{
  "schema_version": 9,
  "name": "layout_name",
  "ecu": "MaxxECU",
  "ecu_version": "v1.3",
  "widgets": [
    {
      "type": "panel",
      "id": "panel_0",
      "x": 10, "y": 20,
      "w": 150, "h": 90,
      "config": {
        "slot": 0,
        "signal_name": "OilPressure",
        "label": "OIL",
        "decimals": 1,
        "border_radius": 10,
        "bg_color": 1234
      }
    }
  ],
  "signals": [
    {
      "name": "OilPressure",
      "can_id": 1312,
      "bit_start": 0, "bit_length": 16,
      "scale": 0.1, "offset": 0.0,
      "is_signed": false, "endian": 1
    }
  ]
}
```

**Defaults-only serialization:** Config fields matching factory defaults are omitted from
JSON to conserve the 16 KB file size budget. `from_json()` reads-if-present, keeping
defaults otherwise. This ensures forward compatibility — v8 layouts load cleanly in v9.

### Widget Config Fields by Type

- **panel**: `slot` (0–7), `signal_name`, `label`, `custom_text`, `decimals`, warning thresholds/colors, label/value font, appearance overrides (border, bg, label/value color, y-offsets)
- **rpm_bar**: `signal_name`, `gauge_max`, `redline`, `bar_color`, limiter settings (effect/value/color), lights, background settings
- **bar**: `slot` (0–1), `signal_name`, `label`, `min`, `max`, `low`, `high`, colors, `decimals`, fuel sender settings, appearance overrides (bar bg/radius/border, indicator radius, label/value color)
- **indicator**: `slot` (0–1), `input_source` (wire/CAN), `signal_name`, `animation_enabled`, `is_momentary`
- **warning**: `slot` (0–7), `signal_name`, `active_color`, `label`, `is_momentary`, `invert_toggle`, appearance overrides (inactive color, border, radius, show_label, label_color)
- **text**: `value_idx`, `signal_name`, `decimals`, `static_text`, `font`
- **meter**: `value_idx`, `signal_name`, `min`, `max`, `start_angle`, `end_angle`, tick config (minor/major count/width/length/color), needle (width/color/r_mod), value/ID label (show/offset/color), background (color/opa), 3 arc color zones (enabled/start/end/color)
- **image**: `image_name`, `opacity` (0–255)

---

## Web Server Endpoints

| Method | Path                    | Purpose                                    |
|--------|-------------------------|--------------------------------------------|
| GET    | `/`                     | Embedded `index.html` (main web UI)        |
| GET    | `/screenshot`           | Display framebuffer capture                |
| GET    | `/api/layout/current`   | Live in-memory layout JSON                 |
| GET    | `/api/layout/raw`       | Raw layout JSON file by name               |
| POST   | `/api/layout/save`      | Save layout JSON (optional `apply=0`)      |
| POST   | `/api/layout/preview`   | Preview layout without saving              |
| GET    | `/api/layout/list`      | List saved layouts + active name           |
| POST   | `/api/layout/set`       | Set active layout by name                  |
| POST   | `/api/layout/delete`    | Delete layout (cannot delete "default")    |
| GET    | `/api/presets`          | List available preset layouts              |
| POST   | `/api/image/upload`     | Upload RDMIMG binary (`?name=<name>`, 200 KB max) |
| GET    | `/api/image/list`       | List images with name/width/height/size    |
| POST   | `/api/image/delete`     | Delete image file (`?name=<name>`)         |
| GET    | `/api/image/data`       | Return raw RDMIMG binary (`?name=<name>`)  |

Server config: `recv_wait_timeout=30s`, `send_wait_timeout=30s`, `max_uri_handlers=20`,
`stack_size=8192`.

### Hot-Reload Flow

```
Web editor save → POST /api/layout/save
  → validate JSON (reject invalid to prevent boot loops)
  → layout_manager_save_raw() → LittleFS
  → rdm_settings_set_active_layout() → NVS
  → lv_async_call() → ui_Screen3_screen_init()
  → dashboard_init() → signal_registry_reset()
  → _load_signals() → widget factory + from_json + create
  → build_twai_filter_from_signals() → reconfigure CAN hardware filter
```

---

## Web Editor Architecture

### WIDGET_DEFS

The web editor defines widget metadata in a `WIDGET_DEFS` JavaScript object, keyed by
type name (`"panel"`, `"rpm_bar"`, `"bar"`, `"indicator"`, `"warning"`, `"text"`,
`"meter"`, `"image"`). Each entry specifies `displayName`, default dimensions, and a
`fields[]` array.

### Field Types

The inspector `renderFieldHTML()` supports these field types:

| Type           | Renders As                                              |
|----------------|---------------------------------------------------------|
| `text`         | `<input type="text">`                                   |
| `number`       | `<input type="number">`                                 |
| `color`        | Color picker + hex text input (stores RGB888 integer)   |
| `hex`          | Color picker + hex text input (stores hex string)       |
| `checkbox`     | Toggle switch                                           |
| `select`       | Dropdown (`options: [{v: value, l: label}]`)            |
| `image_picker` | Dropdown (from `/api/image/list`) + upload button       |

### Field Object Schema

```javascript
{
    name: "fieldName",           // config key in layout JSON
    label: "Display Label",
    type: "text|number|color|...",
    def: defaultValue,
    cat: "data|appearance|alerts",  // inspector section
    enabledBy: "parentField",       // conditional visibility
    inline: "groupName",           // inline toggle groups
    group: "groupName"             // visual grouping
}
```

### Color Conversion

Firmware uses RGB565 (16-bit); web editor uses RGB888 (24-bit):
- `rgb565to888(val)` — on load from device
- `rgb888to565(val)` — before sending to device
- `convertWidgetColors(widgets, convertFn)` — walks all `type: 'color'` fields in WIDGET_DEFS and applies conversion. New color fields added to WIDGET_DEFS are auto-converted.

### buildFirmwarePayload()

Prepares layout JSON for firmware:
- Maps `w.signal` (editor binding) → `config.signal_name` (firmware field)
- For bar widgets with disabled alerts, resets thresholds to min/max
- Removes web-editor-only field `bar_alerts_enabled`
- Calls `convertWidgetColors()` with `rgb888to565`

### Slot Management

- `SLOT_LIMITS = { panel: 8, bar: 2, indicator: 2, warning: 8 }`
- `nextAvailableSlot(type)` finds next free slot
- `assignSlot(w)` auto-assigns on widget creation
- `sanitizeClone(clone)` unbinds signal and assigns fresh slot on duplicate

---

## Dashboard Boot Sequence

```
app_main() [main.c]
  → can_init()                          # TWAI hardware init (before LVGL mutex)
  → display/touch init
  → create LVGL mutex
  → xTaskCreatePinnedToCore(LVGL task, core 1)
  → can_start_task()                    # CAN RX task on core 0
  → indicator_wire / fuel_sender tasks
  → web server start

dashboard_init() [dashboard.c]
  → signal_registry_init()
  → widget_registry_reset()
  → lv_timer_create(signal_check_timeouts, 500ms)
  → layout_manager_init()              # mount LittleFS, check schema version
  → rdm_settings_get_active_layout()   # read NVS
  → layout_manager_load(name, parent)
      → signal_registry_reset()
      → _load_signals()                # register all signals from JSON
      → for each widget: factory → from_json → create
      → build_twai_filter_from_signals()
  → fallback: _fallback_create_all()   # if load fails, default widget set
```

---

## Legacy Code Status

### Fully Removed

All legacy dispatch and routing infrastructure has been deleted:
- `can_dispatch.c/h`, `widget_dispatcher.c/h`, `widget_update_fn`, all `_update()` and `sync_from/to_legacy()` functions
- `values_config[]`, `warning_configs[]`, `indicator_configs[]`, `label_texts[]` globals and their type definitions from `ui_Screen3.c/h`
- Config bridge mirror structs (`cb_*_data_t`) — replaced by direct `#include` of widget headers
- Stubbed NVS functions (`config_store_save/load_values/warnings/indicators`)
- Speed and Gear widgets (never implemented — removed from enum)

### Remaining Cleanup Candidates

In `ui_Screen3.c`:

| Global                         | Status                              |
|-------------------------------|-------------------------------------|
| `value_offset_texts[13][64]`  | Declared/initialized, never read    |
| `previous_values[13][64]`     | Declared, only zeroed in init       |

---

## Coding Conventions

### C Style

- C11 standard, no C++ in main firmware
- `snake_case` for functions, variables, types
- `UPPER_SNAKE_CASE` for macros, enum values, defines
- Prefix module functions: `widget_panel_`, `signal_`, `layout_manager_`, `config_bridge_`
- Use `ESP_LOGI/W/E/D` for logging with a static `TAG` per file
- Guard all headers with `#pragma once` or include guards
- Use `#ifdef __cplusplus extern "C"` guards in all headers
- **No `/*` inside block comments** (`-Werror=comment` is active)

### LVGL v8 Conventions

- **NOT v9** — use `lv_obj_set_style_*` (not `lv_obj_set_style(obj, prop, val, selector)`)
- Widget creation: `lv_obj_create(parent)`, then set size/position/style
- **Positioning:** Always `lv_obj_set_align(obj, LV_ALIGN_CENTER)` then `lv_obj_set_pos(obj, x, y)` — coordinates are center-origin offsets
- **Per-instance styling:** Use `lv_obj_set_style_*()` directly (not shared `lv_style_t`) for widget appearance overrides
- Events: `lv_obj_add_event_cb(obj, cb, event_code, user_data)`
- Async work: `lv_async_call(cb, data)` — defers to LVGL task loop
- Timer: `lv_timer_create(cb, period_ms, user_data)`
- Colors: `lv_color_hex(0xRRGGBB)`, `lv_color_make(r, g, b)`
- Text: `lv_label_set_text(label, str)` or `lv_label_set_text_fmt(label, fmt, ...)`

### Memory

- Use `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` for large allocations (PSRAM)
- Use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` with `malloc()` fallback for image data
- Use standard `calloc`/`malloc` for small allocations (internal SRAM)
- Always `free()` allocations; LVGL objects freed by parent deletion
- Widget `type_data` is freed in `destroy()` callback
- Image widget pixel data must be freed in `destroy()` (PSRAM-allocated)

### CAN Bus

- TWAI driver (ESP-IDF's CAN peripheral abstraction)
- Default 500 kbps, configurable to 125/250/1000 kbps
- TX pin: GPIO 20, RX pin: GPIO 19
- Always use `can_extract_bits()` for bit field extraction (handles endianness + sign)
- CAN IDs in this project are standard 11-bit (0x000–0x7FF)

---

## Adding a New Widget Type

1. Create `main/widgets/widget_newtype.c` and `widget_newtype.h`
2. Define `newtype_data_t` struct **in the header** (so config_bridge can include it directly)
3. Implement vtable functions: `create`, `resize`, `open_settings`, `to_json`, `from_json`, `destroy`
4. In `create`: use `lv_obj_set_align(obj, LV_ALIGN_CENTER)` before `lv_obj_set_pos()`
5. In `from_json`: resolve signal name→index via `signal_find_by_name()`
6. In `create`: subscribe to signal via `signal_subscribe(index, callback, widget)`
7. Implement signal callback for push-based updates (runs on LVGL task)
8. In `to_json`: use defaults-only serialization (skip fields matching factory defaults)
9. Add factory function: `widget_t *widget_newtype_create_instance(uint8_t slot)`
10. Add enum value to `widget_type_t` in `widget_types.h` (before `WIDGET_TYPE_COUNT`)
11. Add size constraints to `widget_constraints[]` in `widget_types.c` (order must match enum)
12. Add type name to `widget_type_name()` in `widget_types.c`
13. Register string→enum in `_type_from_str()` in `layout_manager.c`
14. Register enum→factory in `_factory()` in `layout_manager.c`
15. Add `.c` file to `main/CMakeLists.txt` SRCS list
16. If touchscreen config needed: add value_id mapping in `config_bridge.c`
17. Add widget type to `WIDGET_DEFS` in web editor (`main/web/index.html`)
18. Sync `data/web/index.html` with `main/web/index.html`
19. Bump `LAYOUT_SCHEMA_VERSION` in `layout_manager.h` if schema changes

---

## Key Debugging Tips

- **White screen at boot:** Check display init in `main.c`, ensure black background set before LVGL task starts
- **CAN not receiving:** Check `can_init()` called before `can_start_task()`, verify GPIO 19/20 wiring
- **Widget not updating:** Check signal is registered in layout JSON, verify `signal_subscribe()` called in `create()`, check CAN ID matches
- **Signal shows stale:** CAN frame not received within 2s timeout — check hardware filter includes the CAN ID (`build_twai_filter_from_signals()`)
- **Layout not loading:** Check LittleFS mounted (`layout_manager_init()`), verify JSON schema version matches `LAYOUT_SCHEMA_VERSION`
- **Crash in LVGL:** Almost always a threading issue — ensure LVGL mutex held for all `lv_*` calls
- **Config changes lost:** Ensure `dashboard_persist_layout()` called after touchscreen edits
- **Web/device out of sync:** Web polls every 3s — check `/api/layout/current` returns expected data
- **PSRAM allocation failure:** Check `sdkconfig` has SPIRAM enabled, reduce allocation sizes
- **OTA fails:** Check partition table has dual OTA banks, verify `otadata` partition exists
- **Widget position wrong:** Ensure `lv_obj_set_align(obj, LV_ALIGN_CENTER)` is called — coordinates are center-origin
- **Config bridge:** Uses real widget `type_data` structs directly — if widget struct fields change, config_bridge accessors may need updating
- **Image not showing:** Check `/lfs/images/` directory exists, verify RDMIMG magic header, check PSRAM free space
- **Image upload fails:** Check `recv_wait_timeout` (30s), verify `IMAGE_MAX_SIZE` (200 KB), check LittleFS free space
- **Widget colors wrong after save:** Ensure `convertWidgetColors()` with correct direction (565→888 on load, 888→565 on save)
- **New appearance field not serialized:** Check `to_json` defaults-only logic and that `from_json` reads the field
