# 02 — Build, Flash, Configure

How to set up an ESP-IDF environment, build the firmware, flash a device, and reason about the partition table and Kconfig knobs.

## Prerequisites

| Tool | Version |
|---|---|
| ESP-IDF | 5.0 or newer (5.x branch). Project depends on TWAI, esp_lcd RGB, esp_https_ota and `esp_new_jpeg`. |
| Python | 3.8+ (bundled IDF Python venv is fine). |
| CMake | 3.16+ (bundled). |
| Toolchain | `xtensa-esp32s3-elf` (installed by IDF). |
| USB driver | CP2102 / CH340 / ESP USB-Serial-JTAG, depending on board revision. |

After installing IDF, **always export it before building**:

```bash
source $IDF_PATH/export.sh         # bash/zsh
. $IDF_PATH/export.fish            # fish
%IDF_PATH%\export.bat              # cmd
. $IDF_PATH/export.ps1             # PowerShell
```

The IDF Python venv must be active for `idf.py` to resolve.

## First build

```bash
cd /path/to/RDM-7_Dash
idf.py set-target esp32s3        # only first time
idf.py build
```

> **Note for Claude / agents working in this repo**: builds take 1–2 minutes and the user runs them externally. **Do not run `idf.py build` without asking.** See [09-conventions-and-pitfalls.md](09-conventions-and-pitfalls.md).

The first build pulls managed components from the ESP-IDF Component Manager. The lockfile is [dependencies.lock](../../dependencies.lock). Resolved components land in `managed_components/`. If the build complains about a missing component, run:

```bash
idf.py reconfigure
```

## Flash + monitor

```bash
idf.py -p COM5 flash monitor       # Windows
idf.py -p /dev/ttyUSB0 flash monitor   # Linux/macOS
```

`monitor` exits with `Ctrl+]`. The first flash erases NVS partitions; subsequent flashes preserve them. To wipe NVS (factory reset):

```bash
idf.py -p COM5 erase-flash
```

LittleFS contents survive `idf.py flash`. To wipe LittleFS specifically without nuking everything:

```bash
parttool.py --port COM5 erase_partition --partition-name=littlefs
```

## Source layout

`main/CMakeLists.txt` enumerates every translation unit under `SRCS`. Adding a new C file means appending it there — there is no glob.

Components used (REQUIRES list in `main/CMakeLists.txt`):

```
driver, esp_adc, esp_lcd, esp_lcd_touch, esp_lcd_touch_gt911, esp_timer,
lvgl, nvs_flash, fatfs, esp_wifi, esp_http_client, json, esp_https_ota,
app_update, mbedtls, esp-tls, esp_http_server,
joltwallet__littlefs
```

Embedded blobs:

| Symbol | Source | Notes |
|---|---|---|
| `EMBED_TXTFILES` | `main/web/index.html` | The web editor; ~707 KB. Must be UTF-8 plain text. |
| `EMBED_FILES` | `main/embed/RDM.rdmimg` | Boot logo image. |

Vendor components in `components/` (not managed): `lvgl__lvgl` (LVGL v8 source vendored), `espressif__esp_io_expander-v1.0.1`, `sd_card`, `espressif__esp_lcd_touch`, `espressif__esp_lcd_touch_gt911`. The vendored LVGL fork is unmodified at `8.3.11` — confirm by reading [main/idf_component.yml](../../main/idf_component.yml).

## Managed dependencies

[main/idf_component.yml](../../main/idf_component.yml):

```yaml
dependencies:
  lvgl/lvgl: 8.3.11                    # pinned exactly
  espressif/esp_lcd_touch_gt911: 1.1.0
  joltwallet/littlefs: ">=1.0.0"
  espressif/esp_new_jpeg: ^0.6.0       # screenshot encoder
  idf: ">=5.0.0"
```

Bumping LVGL to v9 will break the entire UI layer. The styling, event, and indev APIs are incompatible. Don't.

## Partition table

[partitions.csv](../../partitions.csv):

| Name | Type | SubType | Offset | Size | Purpose |
|---|---|---|---|---|---|
| `nvs` | data | nvs | 0x9000 | 0x24000 (144 KB) | Settings, WiFi creds, peaks, device ID |
| `otadata` | data | ota | 0x2D000 | 0x2000 (8 KB) | OTA boot metadata |
| `phy_init` | data | phy | 0x2F000 | 0x1000 (4 KB) | RF calibration |
| `ota_0` | app | ota_0 | 0x30000 | 0x380000 (3.5 MB) | Firmware slot A |
| `ota_1` | app | ota_1 | (auto) | 0x380000 (3.5 MB) | Firmware slot B (OTA target) |
| `littlefs` | data | spiffs | (auto) | 0x8D0000 (~8.8 MB) | Layouts, fonts, images |

OTA updates are atomic via `esp_https_ota`. Boot metadata in `otadata` decides which slot runs after a successful update. If both slots are corrupt, the bootloader falls back to `ota_0`. Use `idf.py partition-table` to print the resolved layout.

LittleFS is mounted at `/lfs`, partition label `littlefs`, with auto-format on mount failure. Do **not** rename the partition label — `layout_manager.c` and `font_manager.c` look it up by name.

## sdkconfig — non-default tweaks

The committed `sdkconfig` overrides several IDF defaults. The ones that matter:

### Memory

| Key | Value | Why |
|---|---|---|
| `CONFIG_SPIRAM` | y (octabus, 80 MHz) | 8 MB PSRAM enabled |
| `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` | y | Code in PSRAM (XIP) |
| `CONFIG_SPIRAM_RODATA` | y | Read-only data in PSRAM |
| `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` | y | DNS hijack stack lives in PSRAM |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 8192 | Small allocs stay in internal RAM |

### FreeRTOS

| Key | Value | Why |
|---|---|---|
| `CONFIG_FREERTOS_HZ` | 500 | 2 ms tick — fast LVGL responsiveness. **Causes `pdMS_TO_TICKS(1) == 0`.** |
| `CONFIG_FREERTOS_TIMER_TASK_AFFINITY` | 1 | Soft timers run on core 1 with LVGL |
| `CONFIG_FREERTOS_NUMBER_OF_CORES` | 2 | Both cores used |

### LVGL

| Key | Value |
|---|---|
| `CONFIG_LV_DISP_DEF_REFR_PERIOD` | 16 (62.5 Hz) |
| `CONFIG_LV_INDEV_DEF_READ_PERIOD` | 16 |
| `CONFIG_LV_DPI_DEF` | 130 |
| `CONFIG_LV_COLOR_DEPTH` | 16 (RGB565) |
| `CONFIG_LV_USE_QRCODE` | y |
| `CONFIG_LV_USE_SNAPSHOT` | y |
| `CONFIG_LV_USE_PNG`, `CONFIG_LV_USE_GIF` | y |
| `CONFIG_LV_USE_TINY_TTF` | y |
| `CONFIG_LV_LAYER_SIMPLE_BUF_SIZE` | 24576 |

`main/lv_conf.h` also exists, but where the two disagree, **sdkconfig wins** (IDF's LVGL component injects sdkconfig values into the build).

### HTTP server

| Key | Value | Why |
|---|---|---|
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | 1024 | Android webview headers exceed default 512 B and trigger a 431 + lock-init abort. |
| `CONFIG_HTTPD_MAX_URI_LEN` | 512 | URI cap. |

### LwIP

| Key | Value |
|---|---|
| `CONFIG_LWIP_MAX_SOCKETS` | 12 |
| `CONFIG_LWIP_MAX_ACTIVE_TCP` | 16 |
| `CONFIG_LWIP_MAX_UDP_PCBS` | 16 |

### Watchdog

| Key | Value | Why |
|---|---|---|
| `CONFIG_TASK_WDT` | y | Enabled. |
| `CONFIG_TASK_WDT_TIMEOUT_S` | **15** | Was 5 s; bumped because boot-time widget-instantiate yields were too coarse at 500 Hz. |
| `CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU0/1` | y | Idle tasks must check in. |

### CI variants

There are three CI sdkconfigs alongside the main one:

- `sdkconfig.ci.double_fb` — double frame buffer (2× 768 KB in PSRAM).
- `sdkconfig.ci.single_fb_no_bb` — single FB, no bounce buffer (used for capture-pipeline tests).
- `sdkconfig.ci.single_fb_with_bb` — single FB with bounce buffer (lower CPU but possible tearing).

The default build uses single FB **without** the shadow framebuffer — that 768 KB was reclaimed during the screenshot pipeline rewrite. See [08-aux-systems.md](08-aux-systems.md) §screenshot.

## Project Kconfig (`main/Kconfig.projbuild`)

```
menu "Example Configuration"
  EXAMPLE_DOUBLE_FB                  default n
  EXAMPLE_USE_BOUNCE_BUFFER          default n  (depends on !DOUBLE_FB)
  EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM default y  (disabled if DOUBLE_FB)

menu "RDM-7 Display Configuration"
  choice RDM_SCREEN_SIZE
    RDM_SCREEN_800X480   default
    RDM_SCREEN_480X480
    RDM_SCREEN_720X720
  choice RDM_SCREEN_SHAPE
    RDM_SHAPE_RECT
    RDM_SHAPE_ROUND     default for 720×720
```

Screen size + shape produce compile-time macros consumed by `system/screen_config.h`:

```c
#define SCREEN_W         800            // or 480 / 720
#define SCREEN_H         480
#define SCREEN_SHAPE     SCREEN_SHAPE_RECT
#define SCREEN_ORIGIN_X  (SCREEN_W / 2)
#define SCREEN_ORIGIN_Y  (SCREEN_H / 2)
```

Switching screen size requires a clean rebuild — many widget constraints reference these macros at compile time.

## OTA update path

1. Studio (or HTTP POST to `/api/ota/start`) → `start_ota_update_task`.
2. `esp_https_ota` downloads the new image into the inactive OTA slot. Progress reported via `/api/ota/status`.
3. `otadata` partition is updated to point at the new slot.
4. Reboot. Bootloader picks the new slot.
5. If the new firmware fails to call `esp_ota_mark_app_valid_cancel_rollback()` within its first run, bootloader rolls back.

The Cloudflare OTA proxy under `tools/cloudflare-ota-proxy/` lets you serve firmware from a CDN with auth and routing. `ota_set_firmware_url(url)` overrides the default URL.

## Common build problems

| Symptom | Likely cause |
|---|---|
| `'idf.py' not recognized` | IDF not exported. Run `export.sh`. |
| `LV_FONT_MONTSERRAT_24 undefined` | Font wasn't enabled in sdkconfig — check `CONFIG_LV_FONT_MONTSERRAT_24`. |
| `-Werror=comment` errors | A `/*` is nested inside a block comment somewhere. Find and fix. |
| `image > partition size` | Firmware exceeds 3.5 MB. Investigate `idf.py size-components` and `idf.py size-files`. |
| `httpd_register_uri_handler returned ESP_ERR_HTTPD_HANDLERS_FULL` | Hit `max_uri_handlers` cap. Currently 100 in `web_server.c`. Count `httpd_register_uri_handler` calls. |
| Silent boot loop | TWDT firing — check serial monitor. Boot-time yields may be missing. |
| White flash on boot | `app_main` no longer paints black before LVGL task starts. See main.c step 10. |

## Logging

Per-module log levels are runtime-tunable:

```c
esp_log_level_set("CAN", ESP_LOG_DEBUG);
esp_log_level_set("dashboard", ESP_LOG_VERBOSE);
```

Default level is `ESP_LOG_INFO` (set in sdkconfig). To debug a specific subsystem without rebuilding, send a serial command (see `serial_commands.c`) or set verbose levels in `app_main`.

## Build artifacts

- `build/RDM-7_Dash.bin` — application image, flashed to ota_0/ota_1.
- `build/bootloader/bootloader.bin` — second-stage bootloader.
- `build/partition_table/partition-table.bin` — partition table.
- `build/RDM-7_Dash.elf` — symbols for `idf.py monitor`'s panic decoder.
- `build/RDM-7_Dash.map` — full link map (useful for chasing static-storage bloat).

`bin/` at the repo root holds historical pre-built binaries; not part of the build pipeline.

---

Next: [03-widget-system.md](03-widget-system.md).
