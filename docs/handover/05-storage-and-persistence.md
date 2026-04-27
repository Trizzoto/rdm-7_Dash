# 05 — Storage & Persistence

Where every byte of state lives: LittleFS, NVS, SD card, image format, font cache, OTA partitions.

## Storage map

| Data | Where | Module |
|---|---|---|
| Layouts (JSON) | LittleFS `/lfs/layouts/` | [layout_manager.c](../../main/layout/layout_manager.c) |
| Splash layouts | LittleFS `/lfs/layouts/_splash_*.json` | layout_manager + splash_screen |
| Images | LittleFS `/lfs/images/*.rdmimg` | [rdm_image.c](../../main/ui/images/rdm_image.c) |
| Fonts (TTF) | LittleFS `/lfs/fonts/*.ttf` | [font_manager.c](../../main/widgets/font_manager.c) |
| Settings (small) | NVS | [config_store.c](../../main/storage/config_store.c) |
| WiFi credentials (multi-SSID) | NVS namespace `wifi_cfg` | [wifi_manager.c](../../main/net/wifi_manager.c) |
| Peak / min values | NVS | [signal.c](../../main/widgets/signal.c) `signal_peaks_*` |
| ECU + gear config | NVS | [ecu_presets.c](../../main/layout/ecu_presets.c), `signal_internal.c` |
| Data logs (CSV) | SD `/sdcard/logs/*.csv` | [data_logger.c](../../main/storage/data_logger.c) |
| Optional asset backups | SD `/sdcard/{layouts,images,fonts}` | [sd_manager.c](../../main/storage/sd_manager.c) |

## LittleFS

Mounted at `/lfs`, partition label `littlefs`, ~8.8 MB. Auto-format on first boot or if mount fails.

```
/lfs/
├── layouts/
│   ├── default.json
│   ├── _splash_Default.json
│   ├── _splash_Custom.json
│   ├── My Layout.json
│   └── …
├── images/
│   ├── logo.rdmimg
│   └── …
└── fonts/
    ├── Fugaz.ttf
    └── …
```

Use VFS calls (`fopen`, `fread`, `fwrite`) for everything. There is no streaming layout API — files read into a heap buffer first.

### Layout schema

Top-level keys (schema v13):

```json
{
  "schema_version": 13,
  "name": "My Layout",
  "screen_w": 800,
  "screen_h": 480,
  "ecu": "MaxxECU",
  "ecu_version": "1.2",
  "night_mode": {
    "signal_name": "HEADLIGHT",
    "active_when": ">=1"
  },
  "signals": [
    { "name": "RPM", "can_id": 256, "bit_start": 0, "bit_length": 16,
      "scale": 1.0, "offset": 0, "is_signed": false, "endian": 1, "unit": "rpm" }
  ],
  "widgets": [
    {
      "type": "panel", "id": "panel_0", "x": -200, "y": 80, "w": 220, "h": 100,
      "config": {
        "slot": 0,
        "signal_name": "RPM",
        "label": "RPM",
        "decimals": 0,
        "warn_high_threshold": 7000,
        "night": { "value_color": "#ff8800" }
      },
      "rules": [
        {
          "signal_name": "RPM",
          "op": ">=", "threshold": 7000,
          "overrides": [
            { "field": "border_color", "value": "#ff0000" }
          ]
        }
      ]
    }
  ]
}
```

`schema_version` is `LAYOUT_SCHEMA_VERSION` from [layout_manager.h](../../main/layout/layout_manager.h). Loader behavior:

- v0 / missing → reject layout, warn.
- Older valid versions → load; migration happens inline (see `_migrate_*` helpers in layout_manager.c — examples: RPM bar limiter enum collapse, splash filename rename).
- Newer than current → log a warning, **load anyway** (forward compatibility — newer fields just get ignored).

Bump the macro whenever a load-time migration is needed. Always test loading the previous version's layouts after a bump.

### Layout manager API

```c
esp_err_t layout_manager_init(void);
esp_err_t layout_manager_load(const char *name, lv_obj_t *parent);
esp_err_t layout_manager_save(const char *name, widget_t **widgets, size_t count);
esp_err_t layout_manager_save_raw(const char *name, cJSON *root);
esp_err_t layout_manager_delete(const char *name);
size_t    layout_manager_list(char names[][32], size_t max);
const char *layout_manager_get_active(void);
esp_err_t layout_manager_set_active(const char *name);
esp_err_t layout_manager_apply_json(cJSON *root, lv_obj_t *parent);
```

The on-device `dashboard_persist_layout()` round-trips: walks `widget_registry_snapshot` → each widget's `to_json` → cJSON tree → `layout_manager_save_raw`.

The web-server `/api/layout/save` path goes directly through `layout_manager_save_raw` with the JSON the editor posted.

### Default layout

[main/layout/default_layout.c](../../main/layout/default_layout.c) provides a hardcoded fallback: if no layouts exist, or the active layout fails to load, the firmware seeds a sensible default (RPM bar, a few panels, water/oil temp, etc.) and saves it as `default.json`. User edits to default.json are preserved on subsequent boots — the seed is one-time.

### Splash layouts

A separate concept: the splash screen at boot can also be a JSON layout (`_splash_<name>.json`). The currently-active splash is named in NVS key `active_spl`. Default is `_splash_Default.json`. The `_splash` legacy name is migrated on boot.

## Image format (`.rdmimg`)

Custom binary format optimised for direct LVGL blit:

```
Offset 0–3:  Magic   "RDMI" (4 ASCII bytes)
Offset 4–5:  Width   uint16 LE
Offset 6–7:  Height  uint16 LE
Offset 8:    Format  = 5 (LV_IMG_CF_TRUE_COLOR_ALPHA)
Offset 9–11: Reserved (zero)
Offset 12+:  Pixels — 3 bytes per pixel:
              bytes 0..1  RGB565 little-endian
              byte 2      8-bit alpha
```

Why this format: LVGL v8 can blit `LV_IMG_CF_TRUE_COLOR_ALPHA` images directly without per-pixel decode. PNG/JPG decode happens at runtime — fine for occasional use, costly for a frequently-shown gauge background.

Conversion tool: [tools/png_to_rdmimg.py](../../tools/png_to_rdmimg.py). Usage:

```bash
python tools/png_to_rdmimg.py input.png output.rdmimg --size 240x240
```

Loader: `rdm_image_load(name)` in `main/ui/images/rdm_image.c`. Reads from `/lfs/images/` and returns an `lv_img_dsc_t *` ready to assign to `lv_img_set_src()`.

## Font manager

[main/widgets/font_manager.c/h](../../main/widgets/font_manager.c).

Two-level cache:

| Level | Capacity | Lifetime |
|---|---|---|
| Family (TTF blob) | 8 max | Persist across layout reloads |
| Instance (compiled at size) | 32 max | Cleared on `font_manager_reset_instances()` |

```c
lv_font_t *font_manager_get(const char *family, uint16_t size);
```

Returns the `lv_font_t *` for a given family + size, creating it via `lv_tiny_ttf_create_data` if not cached. Fonts in cache go to PSRAM.

### Font name format in JSON

- Modern: `"Fugaz:28"` — family + size separated by `:`.
- Legacy: `"fugaz_28"` — still parsed by `widget_resolve_font()` for backward compatibility.

When in doubt, write the new form.

### Font lifecycle on reload

`font_manager_reset_instances()` is called from `dashboard_init()` so the new layout's font sizes are recompiled fresh — the family blobs themselves stay in cache.

## NVS settings

ESP-IDF NVS provides a key-value store on the `nvs` partition. One access wrapper:

- [main/storage/config_store.c](../../main/storage/config_store.c) — all application and system settings (`config_store_save_<thing>()` / `config_store_load_<thing>()`). Device ID is managed by `device_id.c` which uses NVS directly.

### Namespace map

| Namespace | Keys | Purpose |
|---|---|---|
| `can_config` | `can_bitrate` | Bitrate index (0=125k, 1=250k, 2=500k, 3=1M) |
| `dimmer_cfg` | `sig_name`, `thresh`, `is_mom`, `invert`, `dim_br`, `enabled` | Brightness dimmer configuration |
| `wifi_cfg` | `ssid`, `password`, `auto_connect`, `slots_0..4`, `count` | Up to 5 saved STA networks |
| `wifi_ap_cfg` | `enabled`, `password` | AP hotspot config |
| `wifi_boot` | `wifi_on_boot`, `ap_enabled` | Startup behaviour |
| `display_cfg` | `rotation`, `night_mode` | Rotation 0/90/180/270, night-mode settings |
| `layout_mgr` | `active`, `active_spl` | Active layout name + splash name |
| `data_logger` | `log_rate_hz` | Logging rate (0 = Max) |
| `ecu_cfg` | `make`, `version` | Selected ECU preset |
| `gear_cal_cfg` | `enabled`, `wheel_circ`, `final_drive`, `ratio_count`, `ratios[]`, `rpm_sig`, `speed_sig` | Calculated-gear config |
| `splash_fade` | `enabled` | Splash fade animation |
| `first_run` | `done` | First-run wizard completion flag |
| `signal_peaks` | per-signal float pair | All-time peak/min values |

Factory reset path: `config_store_factory_reset()` erases NVS + LittleFS user content.

## SD card

[main/storage/sd_manager.c](../../main/storage/sd_manager.c). SPI mode:

| Pin | GPIO |
|---|---|
| MOSI | 11 |
| CLK | 12 |
| MISO | 13 |
| CS | 4 |

Mounted at `/sdcard` via FATFS. Mount failure is **non-fatal** — the firmware boots without SD, and any feature that needs it (data logger, replay, asset backups) reports unavailable.

API:

```c
esp_err_t sd_manager_init(void);
bool      sd_manager_is_mounted(void);
esp_err_t sd_manager_get_info(uint64_t *total, uint64_t *used, uint64_t *free);
```

Conventional directories:

```
/sdcard/
├── layouts/    (optional backups)
├── images/     (optional backups)
├── fonts/      (optional backups)
└── logs/       (data_logger CSVs — primary use)
```

Web UI exposes SD operations via `/api/sd/*` (see [07-web-server-api.md](07-web-server-api.md)).

## Data logger

[main/storage/data_logger.c/h](../../main/storage/data_logger.c).

```c
esp_err_t data_logger_init(void);                              // load NVS rate
esp_err_t data_logger_start(void);                              // default rate
esp_err_t data_logger_start_with_rate(uint16_t hz, bool persist); // custom
esp_err_t data_logger_stop(void);
esp_err_t data_logger_set_rate_hz(uint16_t hz);                // mid-log re-rate
bool      data_logger_is_active(void);
const char *data_logger_current_file(void);
size_t    data_logger_get_sample_count(void);
uint64_t  data_logger_get_elapsed_ms(void);
```

Rate values:

- `0` = Max — runs every LVGL tick (~70–200 Hz coalesced).
- `1, 2, 5, 10, 20, 50, 100, 200, 1000` — fixed Hz timer.

Persisted in NVS namespace `data_logger`, key `log_rate_hz`.

CSV format:

```
timestamp_ms,Signal A,Signal B,Signal C
0,1234,87.5,
50,1240,87.5,
100,1250,87.6,12
```

- Header row lists every CAN signal.
- Each row: elapsed ms, then one float per signal. Stale signals → empty cell.
- File path: `/sdcard/logs/log_<unix_timestamp>.csv`.

Flush strategy: every 100 samples or every 2 s, whichever first. Reduces write amplification on the SD card.

## Signal replay

Companion to data logger. Already covered in [04-signal-and-can.md](04-signal-and-can.md) §replay.

## OTA

Partition layout (in [partitions.csv](../../partitions.csv)):

| | |
|---|---|
| `ota_0` | Firmware slot A (3.5 MB) |
| `ota_1` | Firmware slot B (3.5 MB) |
| `otadata` | Boot metadata (8 KB) — bootloader reads this to choose slot |

OTA implementation: [main/net/ota_handler.c/h](../../main/net/ota_handler.c).

```c
typedef enum {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_NO_UPDATE_AVAILABLE,
    OTA_UPDATE_AVAILABLE,
    OTA_UPDATE_IN_PROGRESS,
    OTA_UPDATE_COMPLETED,
    OTA_UPDATE_FAILED
} ota_status_t;
```

Public API:

```c
esp_err_t init_ota(void);
esp_err_t check_for_update(void);
esp_err_t start_ota_update(void);
esp_err_t start_ota_update_task(void);
ota_status_t get_ota_status(void);
const char *get_latest_version(void);
int   get_ota_progress(void);          // 0–100
float get_update_file_size_mb(void);
const char *get_release_notes(void);
void  ota_set_firmware_url(const char *url);
```

Default OTA URL is hardcoded; `ota_set_firmware_url` lets a deployment override it (e.g., point at the Cloudflare proxy under [tools/cloudflare-ota-proxy/](../../tools/cloudflare-ota-proxy/) for distribution control).

Rollback: a freshly-flashed image is "pending verification" until it calls `esp_ota_mark_app_valid_cancel_rollback()`. The dashboard does this once boot completes successfully (no early panic). If the new firmware crashes before that point, bootloader reverts to the previous slot.

## Boot assets

[main/storage/boot_assets.c](../../main/storage/boot_assets.c) seeds initial files on first boot:

- `default.json` layout if missing.
- `_splash_Default.json` if missing.
- Bundled fonts and images that ship with the firmware.

The seeding is one-shot — once the file exists, user edits are preserved across reflashes (only `idf.py erase-flash` will wipe them).

## Quick reference: where does X go?

| Question | Answer |
|---|---|
| Where is the active layout name? | NVS `layout_mgr/active`. |
| Where is layout content? | LittleFS `/lfs/layouts/<name>.json`. |
| Where do peak values persist? | NVS namespace `signal_peaks`. |
| Where do CAN logs go? | SD `/sdcard/logs/`. |
| Where are fonts loaded from? | LittleFS `/lfs/fonts/`. |
| Where are images loaded from? | LittleFS `/lfs/images/`. |
| Where is gear config? | NVS namespace `gear_cal_cfg`. |
| Where is the boot logo? | Embedded (`EMBED_FILES = main/embed/RDM.rdmimg` in CMake). |
| Where is the web UI? | Embedded (`EMBED_TXTFILES = main/web/index.html`). |
| What survives `idf.py flash`? | NVS, LittleFS. |
| What survives `idf.py erase-flash`? | Nothing. |

---

Next: [06-ui-and-screens.md](06-ui-and-screens.md).
