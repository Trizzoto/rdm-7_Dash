# Prompt for Claude Code — Build LVGL WASM Preview

Copy everything below this line and paste it as your first message in the new Claude Code session:

---

Build an LVGL v8 WebAssembly preview renderer for the RDM-7 automotive dashboard. The goal: compile the real firmware widget C code to WASM so a web page can show a pixel-perfect 800x480 dashboard preview in the browser — using the same look and feel as the existing web editor.

**Read `docs/WASM_PREVIEW_BUILD_GUIDE.md` FIRST** — it's your complete reference for architecture, JSON format, all 13 widget type schemas, coordinate system, color system (RGB565), font system, image format (RDMIMG), and build instructions. Follow it closely.

`firmware_src/` contains the actual ESP32 firmware C files: 12 widget types, signal system, font manager, layout manager, CAN decoder. These need to compile for Emscripten with ESP-IDF dependencies stubbed.

`web/index.html` is the existing web editor (8500+ lines, single-file SPA). This is the UI you should base the standalone editor on. It has the full widget palette, drag-and-drop, inspector panel, signal table, coordinate system, color conversion, and layout JSON building. Currently it connects to a physical ESP32 device for rendering (fetches screenshots via `/screenshot` endpoint) and saves/loads via device HTTP API.

`lv_conf_reference.h` is the firmware's LVGL config — match its settings.

## What to build

### Phase 1: WASM Preview Engine

**1. Git submodules** — LVGL v8.3 as `lvgl/` (use release/v8.3 branch) and lv_drivers as `lv_drivers/`.

**2. `esp_stubs.h`** — Stub all ESP-IDF APIs (see build guide section 4 for complete code): ESP_LOG macros → printf, heap_caps_malloc → malloc, esp_timer → no-ops, NVS → no-ops, FreeRTOS semaphores → no-ops, can_transmit_frame → no-op. Also `#define _Atomic` as empty if Emscripten complains.

**3. `lv_conf.h`** — LV_COLOR_DEPTH=16, enable Montserrat 8-24, LV_USE_TINY_TTF=1, LV_FONT_CUSTOM_DECLARE for firmware's built-in Fugaz/Manrope fonts.

**4. `lv_drv_conf.h`** — USE_SDL=1, 800x480, SDL_ZOOM=1.

**5. Adapted firmware sources** — Copy from `firmware_src/` into the build tree. Key adaptations:
- layout_manager.c: Remove LittleFS mount/init code, keep `layout_manager_apply_json()` and widget instantiation. Remove NVS calls. Remove the mutex (single-threaded in browser).
- signal.c: Stub LVGL timer for timeouts, stub signal_internal_start/stop
- font_manager.c: Remove ESP partition scanning, keep fopen/fread TTF loading (works with MEMFS)
- All widgets: Include esp_stubs.h to resolve ESP-IDF symbols
- theme.h needs `#include "lvgl.h"` at top since firmware relies on implicit includes

**6. `main.c`** — See build guide section 6. Init LVGL + SDL2 display driver, create MEMFS dirs, export via EMSCRIPTEN_KEEPALIVE:
- `load_layout_json(const char *json)` — full layout reload (reset signals, fonts, widgets, create fresh screen, apply JSON)
- `inject_signal(const char *name, float value)` — test signal injection
- `register_font(const char *path)` — rescan fonts after JS writes TTF to MEMFS
- `register_image(const char *name, const uint8_t *data, int len)` — write RDMIMG to MEMFS

**7. `CMakeLists.txt`** — See build guide section 15. Emscripten build with USE_SDL=2, INITIAL_MEMORY=33554432, ALLOW_MEMORY_GROWTH, MODULARIZE, EXPORT_NAME='LVGLPreview', exported functions + FS runtime method.

**8. Include cJSON** — download cJSON.c/.h (single-file, MIT licensed) into the project.

### Phase 2: Standalone Web Editor

Create `editor.html` — a standalone version of the web editor that works WITHOUT a device. Base it on `web/index.html` (same UI, same look and feel, same dark theme, same widget palette, same inspector panel, same signal table). The key changes:

**Replace device API calls with local storage + WASM preview:**

1. **Canvas rendering**: Instead of fetching screenshots from `/screenshot`, the LVGL WASM canvas IS the preview. Place the SDL canvas (created by Emscripten) in the same position as the current editor canvas. Overlay the widget bounding boxes / selection handles on top of it exactly like the current editor does.

2. **Layout storage**: Replace `/api/layout/save` and `/api/layout/current` with localStorage or IndexedDB. Layouts are saved/loaded locally as JSON.

3. **Live preview**: Whenever the user moves a widget, changes a property, or modifies anything, call `buildFirmwarePayload()` (already exists in the editor) to generate the firmware-format JSON, then call `Module.ccall('load_layout_json', null, ['string'], [JSON.stringify(payload)])` to re-render the WASM preview.

4. **Image handling**: Images uploaded in the editor get converted to RDMIMG format (the conversion code already exists in the editor as `convertImageToRDMIMG()`), stored in IndexedDB, and written to Emscripten MEMFS via `Module.FS.writeFile()`.

5. **Font handling**: Custom TTF fonts uploaded get stored in IndexedDB and written to MEMFS. Built-in fonts (Montserrat, Fugaz, Manrope) are compiled into the WASM binary.

6. **Signal testing**: The existing signal inject UI (`setTestValue()`) calls `Module.ccall('inject_signal', ...)` instead of `POST /api/signal/inject`.

7. **Export/Import**: Add "Export JSON" (download layout as .json file) and "Import JSON" (upload .json file) buttons so users can share layouts and later upload them to a real device.

8. **No device sync**: Remove `syncFromDevice()` polling, remove connection status dot, remove screenshot capture. The WASM canvas is always live.

**Keep these from the original editor exactly as-is:**
- All CSS styling and dark theme
- Widget palette (sidebar with draggable widget types)
- Inspector panel (property editor with all fields, categories, tabs)
- WIDGET_DEFS object (all widget type definitions and field metadata)
- Coordinate system (devToWeb/webToDev with ORIGIN_X=400, ORIGIN_Y=240)
- Color conversion (rgb565to888 / rgb888to565 / convertWidgetColors)
- buildFirmwarePayload() function
- Widget drag, resize, selection, multi-select, snap grid
- Undo/redo system
- Signal table editing
- Slot management (SLOT_LIMITS)
- Keyboard shortcuts

### Phase 3: Build and Verify

- Build with `emcmake cmake .. && emmake make`
- Serve with `python3 -m http.server 8080` from the build output directory
- Open `editor.html` in Chrome
- Add a panel widget from the palette, configure it, verify the LVGL canvas shows it
- Test signal injection — add a signal, bind it to the panel, inject a value, verify the panel updates
- Test moving/resizing widgets — verify the LVGL preview updates in real-time

## Critical rules

- **LVGL v8.3 NOT v9** — `lv_task_handler()` not `lv_timer_handler()`, `lv_disp_drv_t` not `lv_display_t`
- **RGB565** — all colors in JSON are 16-bit RGB565 integers. The editor works in RGB888 and converts via `buildFirmwarePayload()` before sending to WASM.
- **Coordinates** — widget x,y = center of widget relative to screen center (0,0). All widgets call `lv_obj_set_align(obj, LV_ALIGN_CENTER)` then `lv_obj_set_pos()`
- **Defaults-only JSON** — missing fields in JSON means use factory default
- **Signals load before widgets** — signals array parsed first, then widgets resolve names via `signal_find_by_name()`
- **Do NOT modify files in `firmware_src/`** — create adapted copies
- **Do NOT use LVGL v9 APIs**
- **The editor.html must look and feel identical to web/index.html** — same theme, same layout, same UX. The only difference is it works standalone without a device.

## Test layout JSON

```json
{"schema_version":10,"name":"test","signals":[{"name":"RPM","can_id":1024,"bit_start":0,"bit_length":16,"scale":1.0,"offset":0,"endian":1,"unit":"RPM"}],"widgets":[{"type":"panel","id":"panel_0","x":-150,"y":-50,"w":155,"h":92,"config":{"slot":0,"label":"RPM","signal_name":"RPM","decimals":0,"custom_text":"RPM"}},{"type":"text","id":"text_0","x":100,"y":0,"w":200,"h":40,"config":{"slot":0,"static_text":"RDM-7 Preview","text_color":65535}}]}
```
