# RDM-7 Dash — LVGL WASM Preview Build Guide

**Goal:** Compile the RDM-7 widget system to WebAssembly so the web editor can show a pixel-perfect LVGL preview of dashboard layouts in the browser, without needing the physical ESP32 device.

**Target:** 800x480 RGB565 display, LVGL v8.3, 12 widget types, dynamic TTF fonts, custom RDMIMG image format.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Project Structure](#2-project-structure)
3. [Which Firmware Files to Include](#3-which-firmware-files-to-include)
4. [ESP-IDF Stubs Required](#4-esp-idf-stubs-required)
5. [LVGL v8 Display Driver for Emscripten](#5-lvgl-v8-display-driver-for-emscripten)
6. [Main Entry Point](#6-main-entry-point)
7. [JavaScript ↔ WASM Bridge API](#7-javascript--wasm-bridge-api)
8. [Font System Adaptation](#8-font-system-adaptation)
9. [Image System (RDMIMG Format)](#9-image-system-rdmimg-format)
10. [Color System (RGB565)](#10-color-system-rgb565)
11. [Complete JSON Layout Format](#11-complete-json-layout-format)
12. [Signal System Adaptation](#12-signal-system-adaptation)
13. [Widget Type Reference](#13-widget-type-reference)
14. [Coordinate System](#14-coordinate-system)
15. [Build System (CMake + Emscripten)](#15-build-system-cmake--emscripten)
16. [Web Integration](#16-web-integration)
17. [Testing Checklist](#17-testing-checklist)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│  Web Editor (index.html)                                │
│  ┌──────────────────────┐  ┌─────────────────────────┐  │
│  │  Layout Editor UI    │  │  LVGL Preview Canvas    │  │
│  │  (existing drag/drop │  │  (new WASM module)      │  │
│  │   + inspector)       │  │                         │  │
│  │                      │  │  ┌───────────────────┐  │  │
│  │  User edits widget   │──►  │ load_layout_json()│  │  │
│  │  config/position     │  │  │ (C, via ccall)    │  │  │
│  │                      │  │  └────────┬──────────┘  │  │
│  │                      │  │           ▼             │  │
│  │                      │  │  LVGL v8 renders to     │  │
│  │                      │  │  SDL2 → WebGL canvas    │  │
│  └──────────────────────┘  └─────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

**How it works:**
1. User designs layout in the existing web editor (drag widgets, configure properties)
2. On every change, the editor calls `load_layout_json(jsonString)` into the WASM module
3. The WASM module runs the real LVGL v8 + widget C code to render the layout
4. LVGL renders to an SDL2 surface → Emscripten SDL2 port → WebGL → HTML `<canvas>`
5. The result is pixel-identical to what the ESP32 would display

---

## 2. Project Structure

```
rdm7-wasm-preview/
├── CMakeLists.txt              # Emscripten build
├── main.c                      # Entry point + JS bridge
├── esp_stubs.h                 # ESP-IDF stub definitions
├── esp_stubs.c                 # ESP-IDF stub implementations
├── lv_conf.h                   # LVGL v8 config (adapted from firmware)
├── lv_drv_conf.h               # lv_drivers SDL config
├── shell.html                  # Custom HTML template
├── lvgl/                       # LVGL v8.3 source (git submodule)
├── lv_drivers/                 # lv_drivers (git submodule, for SDL)
├── widgets/                    # Symlinks or copies from firmware
│   ├── widget_panel.c/h
│   ├── widget_bar.c/h
│   ├── widget_text.c/h
│   ├── widget_button.c/h
│   ├── widget_toggle.c/h
│   ├── widget_arc.c/h
│   ├── widget_meter.c/h
│   ├── widget_image.c/h
│   ├── widget_warning.c/h
│   ├── widget_shift_light.c/h
│   ├── widget_types.c/h
│   ├── widget_registry.c/h
│   ├── widget_rules.c/h
│   ├── signal.c/h
│   ├── font_manager.c/h
│   └── rdm_image.c/h          # If separate from widget_image
├── layout/
│   ├── layout_manager.c/h      # Needs adaptation (remove LittleFS)
│   └── default_layout.c        # Optional
├── can/
│   └── can_decode.c/h          # Pure stateless, no adaptation needed
├── fonts/                      # Embedded TTF files
│   └── *.ttf
└── dist/                       # Build output
    ├── index.html
    ├── index.js
    └── index.wasm
```

---

## 3. Which Firmware Files to Include

### Include directly (no/minimal changes):
| File | Notes |
|------|-------|
| `widgets/widget_panel.c/h` | All 12 widget types |
| `widgets/widget_bar.c/h` | |
| `widgets/widget_text.c/h` | |
| `widgets/widget_button.c/h` | Stub CAN TX to no-op |
| `widgets/widget_toggle.c/h` | Stub CAN TX to no-op |
| `widgets/widget_arc.c/h` | |
| `widgets/widget_meter.c/h` | |
| `widgets/widget_image.c/h` | |
| `widgets/widget_warning.c/h` | |
| `widgets/widget_shift_light.c/h` | |
| `widgets/widget_types.c/h` | |
| `widgets/widget_registry.c/h` | |
| `widgets/widget_rules.c/h` | |
| `widgets/signal.c/h` | Stub timeout timer |
| `widgets/font_manager.c/h` | Adapt filesystem calls |
| `can/can_decode.c/h` | Pure math, zero changes |

### Include with adaptation:
| File | What to change |
|------|---------------|
| `layout/layout_manager.c/h` | Remove LittleFS/NVS, parse JSON from memory |
| `ui/theme.h` | Include as-is (just color defines) |

### Do NOT include:
| File | Why |
|------|-----|
| `main.c` | ESP32 hardware init, replaced by WASM main |
| `can/can_manager.c` | TWAI hardware driver |
| `net/*` | WiFi, web server, OTA |
| `storage/config_store.c` | NVS storage |
| `io/wire_inputs.c` | GPIO |
| `system/*` | Device ID, display capture |
| `ui/screens/*` | ESP32-specific screens |
| `ui/menu/*` | Touch menu |
| `ui/settings/*` | Device settings UI |
| `ui/menu/config_modal.c` | Touch config modal |
| `ui/callbacks/*` | Touch callbacks |

---

## 4. ESP-IDF Stubs Required

The widget code uses ESP-IDF APIs that must be stubbed:

```c
/* esp_stubs.h */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Logging ── */
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)  /* silent */

/* ── Error codes ── */
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL       -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_NOT_FOUND       0x105

const char *esp_err_to_name(esp_err_t err);

/* ── Memory ── */
/* PSRAM doesn't exist in browser — redirect to regular malloc */
#define MALLOC_CAP_SPIRAM   (1 << 0)
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}
static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}
static inline void heap_caps_free(void *ptr) { free(ptr); }

/* ── CAN TX stub ── */
static inline esp_err_t can_transmit_frame(uint32_t id, const uint8_t *data, uint8_t len) {
    (void)id; (void)data; (void)len;
    return ESP_OK;
}

/* ── Timer stubs ── */
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void*); const char *name; } esp_timer_create_args_t;
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) {
    (void)a; *h = NULL; return ESP_OK;
}
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) {
    (void)h; (void)us; return ESP_OK;
}
static inline esp_err_t esp_timer_stop(esp_timer_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t esp_timer_delete(esp_timer_handle_t h) { (void)h; return ESP_OK; }
static inline int64_t esp_timer_get_time(void) { return 0; }

/* ── NVS stubs ── */
typedef uint32_t nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY  0
static inline esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)ns; (void)mode; *h = 0; return ESP_OK;
}
static inline void nvs_close(nvs_handle_t h) { (void)h; }
static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) {
    (void)h; (void)k; (void)v; return ESP_OK;
}
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len) {
    (void)h; (void)k; (void)v; (void)len; return ESP_ERR_NOT_FOUND;
}
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

/* ── FreeRTOS stubs ── */
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFF
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void*)1; }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s, uint32_t t) {
    (void)s; (void)t; return 1;
}
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s) { (void)s; return 1; }
#define configASSERT(x) do { if(!(x)) { printf("ASSERT FAIL: %s\n", #x); abort(); } } while(0)
```

---

## 5. LVGL v8 Display Driver for Emscripten

Use the `lv_drivers` SDL backend. This is the standard approach for LVGL v8 on desktop/web.

```c
/* hal_init.c */
#include "lvgl.h"
#include "sdl/sdl.h"  /* from lv_drivers */

#define DISP_HOR_RES 800
#define DISP_VER_RES 480
#define DISP_BUF_LINES 40

static lv_disp_draw_buf_t disp_buf;
static lv_color_t buf_1[DISP_HOR_RES * DISP_BUF_LINES];
static lv_color_t buf_2[DISP_HOR_RES * DISP_BUF_LINES];

void hal_init(void) {
    /* Initialize SDL (creates the window/canvas) */
    sdl_init();

    /* Display driver */
    lv_disp_draw_buf_init(&disp_buf, buf_1, buf_2, DISP_HOR_RES * DISP_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf  = &disp_buf;
    disp_drv.flush_cb  = sdl_display_flush;
    disp_drv.hor_res   = DISP_HOR_RES;
    disp_drv.ver_res   = DISP_VER_RES;
    lv_disp_drv_register(&disp_drv);

    /* Mouse input (for testing — optional) */
    static lv_indev_drv_t mouse_drv;
    lv_indev_drv_init(&mouse_drv);
    mouse_drv.type    = LV_INDEV_TYPE_POINTER;
    mouse_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&mouse_drv);
}
```

**lv_drv_conf.h settings:**
```c
#define USE_SDL 1
#define SDL_HOR_RES 800
#define SDL_VER_RES 480
#define SDL_ZOOM 1
#define SDL_DOUBLE_BUFFERED 1
```

**lv_conf.h key settings (must match firmware):**
```c
#define LV_COLOR_DEPTH 16           /* RGB565, same as ESP32 */
#define LV_COLOR_16_SWAP 0          /* No byte swap for SDL */
#define LV_MEM_SIZE (256 * 1024)    /* 256KB for LVGL internal heap */
#define LV_USE_TINY_TTF 1           /* Dynamic TTF support */
#define LV_TINY_TTF_FILE_SUPPORT 1  /* File-based TTF loading */
#define LV_USE_SNAPSHOT 0           /* Not needed for preview */
#define LV_FONT_MONTSERRAT_8  1     /* Enable built-in fonts */
#define LV_FONT_MONTSERRAT_10 1     /* matching what firmware uses */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
```

---

## 6. Main Entry Point

```c
/* main.c */
#include <emscripten.h>
#include "lvgl.h"
#include "esp_stubs.h"
#include "widgets/signal.h"
#include "widgets/font_manager.h"
#include "widgets/widget_registry.h"
#include "layout/layout_manager.h"

extern void hal_init(void);

static lv_obj_t *s_preview_screen = NULL;

static void do_loop(void *arg) {
    (void)arg;
    lv_task_handler();
}

/* Called from JavaScript to render a layout */
EMSCRIPTEN_KEEPALIVE
void load_layout_json(const char *json_str) {
    if (!json_str) return;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        printf("JSON parse failed\n");
        return;
    }

    /* Clean up previous render */
    if (s_preview_screen) {
        lv_obj_del(s_preview_screen);
        s_preview_screen = NULL;
    }

    /* Reset subsystems */
    font_manager_reset_instances();
    font_manager_init();
    signal_registry_init();
    widget_registry_reset();

    /* Create fresh screen */
    s_preview_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_preview_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_preview_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_preview_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Load layout using existing firmware code */
    esp_err_t err = layout_manager_apply_json(root, s_preview_screen);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        printf("Layout apply failed: %d\n", err);
        return;
    }

    lv_scr_load(s_preview_screen);
}

/* Called from JavaScript to inject a signal value for preview */
EMSCRIPTEN_KEEPALIVE
void inject_signal(const char *name, float value) {
    int16_t idx = signal_find_by_name(name);
    if (idx >= 0) {
        signal_inject_value(idx, value);
    }
}

/* Called from JavaScript to upload a font into MEMFS */
EMSCRIPTEN_KEEPALIVE
void register_font(const char *path) {
    /* Font file should already be written to MEMFS by JS.
     * Just trigger font_manager to rescan. */
    font_manager_init();
}

/* Called from JavaScript to upload an RDMIMG into MEMFS */
EMSCRIPTEN_KEEPALIVE
void register_image(const char *name, const uint8_t *data, int len) {
    /* Write to virtual filesystem at /lfs/images/<name>.rdmimg */
    char path[80];
    snprintf(path, sizeof(path), "/lfs/images/%s.rdmimg", name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

int main(void) {
    lv_init();
    hal_init();

    /* Create directories in MEMFS */
    mkdir("/lfs", 0755);
    mkdir("/lfs/fonts", 0755);
    mkdir("/lfs/images", 0755);
    mkdir("/lfs/layouts", 0755);

    /* Initialize subsystems */
    font_manager_init();
    signal_registry_init();

    /* Create initial black screen */
    s_preview_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_preview_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_preview_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_preview_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(s_preview_screen);

    /* Start LVGL main loop (uses requestAnimationFrame) */
    emscripten_set_main_loop_arg(do_loop, NULL, -1, true);

    return 0;
}
```

---

## 7. JavaScript ↔ WASM Bridge API

From the web editor, call into WASM like this:

```javascript
// Wait for WASM module to initialize
const Module = await LVGLPreview();

// Load a layout (pass the firmware-format JSON)
function renderPreview(layoutJson) {
    // layoutJson is the output of buildFirmwarePayload()
    // Colors must be RGB565, signals in firmware format
    const jsonStr = JSON.stringify(layoutJson);
    Module.ccall('load_layout_json', null, ['string'], [jsonStr]);
}

// Inject a test signal value
function previewSignalValue(signalName, value) {
    Module.ccall('inject_signal', null, ['string', 'number'], [signalName, value]);
}

// Upload a font file for preview
async function uploadFontToPreview(fontName, ttfArrayBuffer) {
    const path = `/lfs/fonts/${fontName}.ttf`;
    Module.FS.writeFile(path, new Uint8Array(ttfArrayBuffer));
    Module.ccall('register_font', null, ['string'], [path]);
}

// Upload an image for preview
function uploadImageToPreview(imageName, rdmimgArrayBuffer) {
    const data = new Uint8Array(rdmimgArrayBuffer);
    const ptr = Module._malloc(data.length);
    Module.HEAPU8.set(data, ptr);
    Module.ccall('register_image', null, ['string', 'number', 'number'],
                 [imageName, ptr, data.length]);
    Module._free(ptr);
}
```

---

## 8. Font System Adaptation

The firmware scans `/lfs/fonts/*.ttf` on boot. In WASM, fonts live in Emscripten's MEMFS.

### Built-in fonts (compiled into LVGL):
| Name | Sizes | JSON value format |
|------|-------|-------------------|
| Montserrat | 8,10,12,14,16,18,20,22,24 | `"montserrat_16"` |
| Fugaz One | 14,17,28,56 | `"fugaz_14"` |
| Manrope Bold | 35,54 | `"manrope_35_bold"` |

These are resolved by `widget_resolve_font(name)` in the firmware. The same compiled-in font data works in WASM if the same `LV_FONT_*` defines are enabled in `lv_conf.h`.

### Custom fonts (TTF):
| JSON format | Example | Resolution |
|-------------|---------|------------|
| `"Family:size"` | `"Fugaz:28"` | `font_manager_get("Fugaz", 28)` → `lv_tiny_ttf` |

For WASM: the JS side must write the TTF file to MEMFS before loading a layout that uses it.

### font_manager adaptation:
- `font_manager_init()` scans `/lfs/fonts/` — works with MEMFS if the directory exists
- `font_manager_get(family, size)` — calls `lv_tiny_ttf_create_file()` — works if file is in MEMFS
- `font_manager_reset_instances()` — frees cached font instances — no changes needed
- The only adaptation: remove any `esp_partition_*` or LittleFS-specific mount calls

---

## 9. Image System (RDMIMG Format)

### Binary format:
```
Offset  Size  Field
0       4     Magic: "RDMI" (0x52 0x44 0x4D 0x49)
4       2     Width (uint16, little-endian)
6       2     Height (uint16, little-endian)
8       1     Color format = 5 (LV_IMG_CF_TRUE_COLOR_ALPHA)
9       3     Reserved (zeros)
12      W*H*3 Pixel data: for each pixel, 3 bytes:
              - byte 0: RGB565 low byte
              - byte 1: RGB565 high byte
              - byte 2: Alpha (0x00=transparent, 0xFF=opaque)
```

### How widgets use images:
1. Widget's `from_json` reads `image_name` field (just the name, no path/extension)
2. Widget's `create` calls `rdm_image_load(name)` which:
   - Opens `/lfs/images/<name>.rdmimg`
   - Reads header, allocates pixel buffer in PSRAM (→ malloc in WASM)
   - Returns `lv_img_dsc_t*` with data pointer
3. Widget sets image source: `lv_img_set_src(img_obj, dsc)`
4. Widget's `destroy` calls `rdm_image_free(dsc)` to free the pixel buffer

### WASM adaptation:
- `rdm_image_load()` uses `fopen`/`fread` — works with MEMFS, no changes needed
- Replace `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` with `malloc(size)` via stubs
- JS must write RDMIMG data to `/lfs/images/<name>.rdmimg` in MEMFS before loading layouts that use it

### Converting images in the browser (already in web editor):
The web editor has `convertImageToRDMIMG(img, targetW, targetH)` which:
1. Draws source image onto a canvas at target dimensions
2. Gets RGBA pixel data
3. Converts each pixel: RGBA → RGB565 (little-endian) + alpha byte
4. Writes the 12-byte header + pixel data

---

## 10. Color System (RGB565)

**CRITICAL:** The firmware stores all colors as RGB565 (16-bit). The web editor works in RGB888 (24-bit) and converts at load/save boundaries.

### Conversion functions (already in web editor):

```javascript
// RGB565 → RGB888 (on load from device/JSON)
function rgb565to888(val) {
    if (val > 0xFFFF) return val;  // already RGB888
    const r5 = (val >> 11) & 0x1F;
    const g6 = (val >> 5)  & 0x3F;
    const b5 = val & 0x1F;
    const r8 = (r5 << 3) | (r5 >> 2);
    const g8 = (g6 << 2) | (g6 >> 4);
    const b8 = (b5 << 3) | (b5 >> 2);
    return (r8 << 16) | (g8 << 8) | b8;
}

// RGB888 → RGB565 (before sending to WASM preview or device)
function rgb888to565(val) {
    const r = (val >> 16) & 0xFF;
    const g = (val >> 8)  & 0xFF;
    const b = val & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
```

**When sending JSON to the WASM preview:** colors must be in RGB565 format (same as firmware). The web editor's `buildFirmwarePayload()` already converts colors to RGB565. Use its output directly.

---

## 11. Complete JSON Layout Format

### Top-level structure:
```json
{
    "schema_version": 10,
    "name": "my_layout",
    "ecu": "Ford",
    "ecu_version": "BA/BF",
    "signals": [ ... ],
    "widgets": [ ... ]
}
```

### Signal entry:
```json
{
    "name": "RPM",
    "can_id": 1024,
    "bit_start": 0,
    "bit_length": 16,
    "scale": 1.0,
    "offset": 0.0,
    "is_signed": false,
    "endian": 1,
    "unit": "RPM"
}
```
- `endian`: 0 = Motorola (big-endian), 1 = Intel (little-endian)
- Signals are loaded BEFORE widgets so `signal_find_by_name()` works in `from_json`

### Widget entry:
```json
{
    "type": "panel",
    "id": "panel_0",
    "x": -200,
    "y": -100,
    "w": 155,
    "h": 92,
    "config": {
        "slot": 0,
        "label": "Boost",
        "signal_name": "MAP",
        "decimals": 1,
        "custom_text": "kPa",
        "border_color": 3026734,
        "bg_color": 0,
        ...
    }
}
```

### Widget coordinate system:
- `x`, `y` = center of widget, relative to screen center
- Screen is 800x480, so x ranges -400 to +400, y ranges -240 to +240
- (0, 0) = screen center

### Defaults-only serialization:
The firmware's `to_json` only writes fields that differ from factory defaults. `from_json` reads-if-present, else keeps defaults. This means many fields will be absent in the JSON — the WASM code handles this identically to the firmware.

---

## 12. Signal System Adaptation

The signal system needs minimal adaptation for WASM:

### What to keep:
- `signal_registry_init()` — resets the registry
- `signal_register()` — called by layout_manager when loading signals from JSON
- `signal_find_by_name()` — resolves signal names to indices
- `signal_subscribe()` / `signal_unsubscribe()` — widget callbacks
- `signal_inject_value()` — for preview test values from JS

### What to stub/remove:
- `signal_dispatch_frame()` — no CAN frames in browser; use inject instead
- `signal_check_timeouts()` — disable or make stale timeout very long
- `signal_internal_start()` / `signal_internal_stop()` — no GPIO/ADC
- Any LVGL timer registration for timeouts (or let it run harmlessly)

### For preview testing:
```javascript
// Inject a signal value from JS to see widgets react
Module.ccall('inject_signal', null, ['string', 'number'], ['RPM', 4500]);
Module.ccall('inject_signal', null, ['string', 'number'], ['MAP', 120.5]);
```

---

## 13. Widget Type Reference

### All 13 widget types with their JSON config fields:

(Colors are RGB565 integers in JSON. Defaults shown are firmware defaults.)

#### `panel` — Data display panel (max 8)
| Field | Type | Default | Notes |
|-------|------|---------|-------|
| slot | int | 0 | 0-7 |
| label | string | "Panel N" | Header text |
| custom_text | string | "" | Unit suffix |
| decimals | int | 0 | 0-5 |
| signal_name | string | "" | Signal binding |
| label_font | string | "" | "Family:size" or "montserrat_16" |
| value_font | string | "" | |
| border_radius | int | 7 | 0-100 |
| border_width | int | 3 | 0-20 |
| border_color | color | 0x2E2F2E→RGB565 | |
| bg_color | color | 0x000000→RGB565 | |
| bg_opa | int | 255 | 0-255 |
| label_color | color | 0xFFFFFF→RGB565 | |
| value_color | color | 0xFFFFFF→RGB565 | |
| label_y_offset | int | -28 | |
| value_y_offset | int | 9 | |
| custom_text_x_offset | int | 41 | |
| custom_text_y_offset | int | 32 | |
| warning_high_enabled | bool | false | |
| warning_high_threshold | float | 0 | |
| warning_high_color | color | 0 | |
| warning_high_apply_label | bool | true | |
| warning_high_apply_value | bool | true | |
| warning_high_apply_panel | bool | false | |
| warning_low_enabled | bool | false | |
| warning_low_threshold | float | 0 | |
| warning_low_color | color | 0 | |
| warning_low_apply_label | bool | true | |
| warning_low_apply_value | bool | true | |
| warning_low_apply_panel | bool | false | |

#### `rpm_bar` — RPM bar graph (singleton)
| Field | Type | Default |
|-------|------|---------|
| rpm_max | int | 8000 |
| redline | int | 6500 |
| bar_color | color | green |
| limiter_effect | int | 0 (0-6) |
| limiter_value | int | 7500 |
| limiter_color | color | red |
| lights_enabled | bool | false |
| background_enabled | bool | false |
| background_value | int | 0 |
| background_color | color | 0 |
| signal_name | string | "" |

#### `bar` — Horizontal bar graph (max 2)
| Field | Type | Default |
|-------|------|---------|
| slot | int | 0 |
| label | string | "BAR1" |
| bar_min | float | 0 |
| bar_max | float | 100 |
| bar_low | float | 0 |
| bar_high | float | 0 |
| bar_low_color | color | blue |
| bar_high_color | color | red |
| bar_in_range_color | color | green |
| show_bar_value | bool | false |
| invert_bar_value | bool | false |
| decimals | int | 0 |
| label_font | string | "" |
| value_font | string | "" |
| signal_name | string | "" |
| bar_bg_color | color | theme panel |
| bar_radius | int | 5 |
| bar_border_width | int | 2 |
| bar_border_color | color | theme panel |
| indicator_radius | int | 5 |
| label_color | color | white |
| value_color | color | white |
| bar_image | string | "" |
| bar_image_full | string | "" |

#### `text` — Static text or signal value
| Field | Type | Default |
|-------|------|---------|
| slot | int | 0 |
| decimals | int | 0 |
| static_text | string | "" |
| signal_name | string | "" |
| font | string | "" |
| text_color | color | white |
| rotation | int | 0 (degrees) |

#### `meter` — Analog gauge
| Field | Type | Default |
|-------|------|---------|
| slot | int | 0 |
| min | float | 0 |
| max | float | 100 |
| start_angle | int | 135 |
| end_angle | int | 45 |
| signal_name | string | "" |
| minor_tick_count | int | 21 |
| major_tick_every | int | 5 |
| minor_tick_width | int | 2 |
| minor_tick_length | int | 10 |
| major_tick_width | int | 4 |
| major_tick_length | int | 15 |
| minor_tick_color | color | grey |
| major_tick_color | color | white |
| needle_width | int | 4 |
| needle_color | color | white |
| needle_r_mod | int | -10 |
| needle_ball_size | int | 10 (0=hidden) |
| needle_ball_color | color | white |
| needle_image_name | string | "" |
| needle_pivot_x | int | 0 |
| needle_pivot_y | int | 0 |
| needle_angle_offset | int | 0 |
| bg_image_name | string | "" |
| border_width | int | 0 |
| border_color | color | black |
| border_opa | int | 255 |
| meter_bg_color | color | 0x3D3D3D |
| meter_bg_opa | int | 255 |
| scale_padding | int | 0 |
| label_gap | int | 10 |
| tick_label_font | string | "" |

#### `image` — Static image display
| Field | Type | Default |
|-------|------|---------|
| image_name | string | "" |
| opacity | int | 255 |
| recolor | color | black |
| recolor_opa | int | 0 |

#### `shape_panel` — Decorative rectangle
| Field | Type | Default |
|-------|------|---------|
| bg_color | color | 0x1A1A1A |
| bg_opa | int | 255 |
| border_color | color | 0x2E2F2E |
| border_width | int | 0 |
| border_radius | int | 10 |
| shadow_width | int | 0 |
| shadow_color | color | black |
| shadow_opa | int | 128 |
| shadow_ofs_x | int | 0 |
| shadow_ofs_y | int | 0 |

#### `arc` — Arc/ring gauge
| Field | Type | Default |
|-------|------|---------|
| start_angle | int | 135 |
| end_angle | int | 45 |
| arc_width | int | 10 |
| arc_color | color | green |
| bg_arc_color | color | 0x333333 |
| bg_arc_width | int | 10 |
| rounded_ends | bool | false |
| signal_name | string | "" |
| signal_min | float | 0 |
| signal_max | float | 100 |
| arc_image | string | "" |
| arc_image_full | string | "" |

#### `warning` — Alert circle/indicator (max 8)
| Field | Type | Default |
|-------|------|---------|
| slot | int | 0 |
| active_color | color | red |
| label | string | "Alert N" |
| is_momentary | bool | true |
| invert_toggle | bool | false |
| signal_name | string | "" |
| inactive_color | color | theme inactive |
| border_width | int | 0 |
| border_color_style | color | black |
| radius | int | 100 (=circle) |
| show_label | bool | true |
| label_color | color | white |
| image_name | string | "" |
| active_opa | int | 255 |
| inactive_opa | int | 80 |

#### `toggle` — Toggle switch with CAN TX
| Field | Type | Default |
|-------|------|---------|
| slot | int | 0 |
| label | string | "" |
| signal_name | string | "" |
| signal_on_threshold | float | 0.5 |
| tx_can_id | int | 0 (0=disabled) |
| tx_bit_start | int | 0 |
| tx_bit_length | int | 1 |
| tx_endian | int | 1 |
| active_color | color | green |
| inactive_color | color | 0x555555 |
| label_color | color | white |
| font | string | "" |
| label_align | int | 1 (0=L,1=C,2=R) |
| label_x | int | 0 |
| label_y | int | 0 |
| show_label | bool | true |
| image_name | string | "" |
| active_opa | int | 255 |
| inactive_opa | int | 100 |

#### `button` — Momentary/latch button with CAN TX
| Field | Type | Default |
|-------|------|---------|
| slot | int | 0 |
| label | string | "BTN" |
| tx_can_id | int | 0 |
| tx_bit_start | int | 0 |
| tx_bit_length | int | 1 |
| tx_endian | int | 1 |
| tx_send_release | bool | false |
| latch | bool | false |
| bg_color | color | 0x333333 |
| text_color | color | white |
| pressed_color | color | 0x555555 |
| border_radius | int | 5 |
| font | string | "" |
| label_align | int | 1 |
| label_x | int | 0 |
| label_y | int | 0 |
| show_label | bool | true |
| image_name | string | "" |

#### `indicator` — Turn signal indicator (max 2)
| Field | Type | Default |
|-------|------|---------|
| slot | int | 0 |
| input_source | int | 0 (0=wire,1=CAN) |
| animation | bool | true |
| is_momentary | bool | true |
| signal_name | string | "" |

#### `shift_light` — RPM shift light strip
| Field | Type | Default |
|-------|------|---------|
| signal_name | string | "" |
| led_count | int | 8 (4-16) |
| range_min | float | 4000 |
| range_max | float | 7000 |
| flash_threshold | float | 7200 |
| flash_speed | int | 200 (ms) |
| color_low | color | green |
| color_mid | color | yellow |
| color_high | color | red |
| color_off | color | 0x212121 |
| led_spacing | int | 2 |
| border_radius | int | 2 |
| led_width | int | 0 (0=auto) |
| led_height | int | 0 (0=auto) |
| fill_mode | int | 0 (0=L-R,1=outside-in) |
| threshold_mid | float | 0.5 |
| threshold_high | float | 0.8 |

### Slot limits:
| Type | Max slots |
|------|-----------|
| panel | 8 |
| bar | 2 |
| indicator | 2 |
| warning | 8 |
| rpm_bar | 1 (singleton) |
| All others | unlimited |

---

## 14. Coordinate System

```
Device coordinates:     Screen pixels:
(-400,-240) = top-left  (0,0) = top-left
(0,0) = center          (400,240) = center
(+400,+240) = bot-right (800,480) = bot-right

Conversion: screen_x = device_x + 400
            screen_y = device_y + 240
```

- Widget `x,y` in JSON = center of widget in device coordinates
- All widgets must call `lv_obj_set_align(obj, LV_ALIGN_CENTER)` before `lv_obj_set_pos(obj, x, y)`
- This makes LVGL interpret `(x,y)` as offset from screen center

---

## 15. Build System (CMake + Emscripten)

```cmake
cmake_minimum_required(VERSION 3.10)
project(rdm7_preview C)

# LVGL and lv_drivers as subdirectories
add_subdirectory(lvgl)
add_subdirectory(lv_drivers)

# Widget sources from firmware
set(WIDGET_SOURCES
    widgets/widget_panel.c
    widgets/widget_bar.c
    widgets/widget_text.c
    widgets/widget_button.c
    widgets/widget_toggle.c
    widgets/widget_arc.c
    widgets/widget_meter.c
    widgets/widget_image.c
    widgets/widget_warning.c
    widgets/widget_shift_light.c
    widgets/widget_types.c
    widgets/widget_registry.c
    widgets/widget_rules.c
    widgets/signal.c
    widgets/font_manager.c
    can/can_decode.c
)

set(LAYOUT_SOURCES
    layout/layout_manager.c
)

set(STUB_SOURCES
    esp_stubs.c
)

add_executable(index
    main.c
    ${WIDGET_SOURCES}
    ${LAYOUT_SOURCES}
    ${STUB_SOURCES}
)

target_include_directories(index PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/widgets
    ${CMAKE_SOURCE_DIR}/layout
)

target_link_libraries(index lvgl lvgl_drivers)

# Emscripten-specific flags
set_target_properties(index PROPERTIES
    SUFFIX ".html"
)

target_compile_options(index PRIVATE -O2)

target_link_options(index PRIVATE
    -s USE_SDL=2
    -s INITIAL_MEMORY=33554432      # 32 MB
    -s ALLOW_MEMORY_GROWTH=1
    -s NO_EXIT_RUNTIME=1
    -s "EXPORTED_FUNCTIONS=['_main','_load_layout_json','_inject_signal','_register_font','_register_image']"
    -s "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS']"
    -s MODULARIZE=1
    -s EXPORT_NAME='LVGLPreview'
    --embed-file fonts/@/lfs/fonts/
    --shell-file ${CMAKE_SOURCE_DIR}/shell.html
)
```

### Build commands:
```bash
# Setup (once)
source /path/to/emsdk/emsdk_env.sh

# Build
mkdir -p build && cd build
emcmake cmake ..
emmake make -j$(nproc)

# Output: build/index.html, build/index.js, build/index.wasm
# Serve: python3 -m http.server 8080 --directory build
```

---

## 16. Web Integration

### Embedding the preview in the web editor:

```html
<!-- Preview canvas (LVGL renders here) -->
<div id="previewContainer" style="width:800px; height:480px; border:1px solid #333;">
    <canvas id="canvas" width="800" height="480"></canvas>
</div>

<script>
let lvglModule = null;

async function initPreview() {
    // Load WASM module
    lvglModule = await LVGLPreview({
        canvas: document.getElementById('canvas')
    });
    console.log('LVGL Preview ready');
}

// Call this whenever the layout changes in the editor
function updatePreview() {
    if (!lvglModule) return;
    const payload = buildFirmwarePayload();  // existing function
    const jsonStr = JSON.stringify(payload);
    lvglModule.ccall('load_layout_json', null, ['string'], [jsonStr]);
}

// Upload fonts/images used by the layout
async function syncAssetsToPreview() {
    // For each custom font used in the layout
    for (const fontName of getUsedFonts()) {
        const resp = await fetch(`/api/font/data?name=${fontName}`);
        const buf = await resp.arrayBuffer();
        lvglModule.FS.writeFile(`/lfs/fonts/${fontName}.ttf`, new Uint8Array(buf));
    }

    // For each image used in the layout
    for (const imgName of getUsedImages()) {
        const resp = await fetch(`/api/image/data?name=${imgName}`);
        const buf = await resp.arrayBuffer();
        lvglModule.FS.writeFile(`/lfs/images/${imgName}.rdmimg`, new Uint8Array(buf));
    }

    lvglModule.ccall('register_font', null, ['string'], ['/lfs/fonts/']);
}

initPreview();
</script>
```

### Standalone mode (no device):
For offline use without a device, the editor stores layouts in localStorage/IndexedDB and images/fonts as blobs. The WASM preview renders purely client-side.

---

## 17. Testing Checklist

### Basic rendering:
- [ ] Black screen renders on init
- [ ] Panel widget renders with correct position, size, border, bg color
- [ ] Text widget renders with correct font, color, position
- [ ] Bar widget renders with fill level and label
- [ ] Meter widget renders with ticks, needle, ball
- [ ] Arc widget renders with correct angles and colors
- [ ] Warning widget renders as circle with label
- [ ] Shape panel renders with shadow and border
- [ ] Button/toggle render with label and colors
- [ ] Shift light renders LED strip
- [ ] Image widget renders RDMIMG
- [ ] RPM bar renders with redline zone

### Signal interaction:
- [ ] `inject_signal("RPM", 5000)` moves meter needle
- [ ] `inject_signal("MAP", 120)` updates panel value text
- [ ] Bar fill responds to signal injection
- [ ] Warning activates/deactivates on signal threshold
- [ ] Arc fill responds to signal values

### Coordinate system:
- [ ] Widget at (0,0) appears at screen center
- [ ] Widget at (-400,-240) appears at top-left corner
- [ ] Widget sizes match firmware rendering

### Colors:
- [ ] RGB565 colors display correctly (compare to device screenshot)
- [ ] Alert threshold colors trigger correctly

### Fonts:
- [ ] Built-in Montserrat renders at all sizes
- [ ] Built-in Fugaz renders
- [ ] Custom TTF fonts load from MEMFS and render correctly

### Images:
- [ ] RDMIMG images display correctly
- [ ] Image recolor/opacity works
- [ ] Meter needle image renders with pivot point
- [ ] Meter background image renders

### Layout reload:
- [ ] Calling `load_layout_json()` multiple times doesn't leak memory
- [ ] Switching between different layouts works cleanly
- [ ] Signal subscriptions are properly cleaned up on reload
