# 07 — Web Server & HTTP API

The firmware embeds a full layout editor and exposes ~86 HTTP endpoints. This document is the reference for every endpoint, plus the supporting infrastructure (captive portal, DNS hijack, the three `index.html` copies).

## Web server configuration

[main/net/web_server.c](../../main/net/web_server.c) (~4700 lines). Started from `app_main` after WiFi/netif is up.

| Setting | Value | Reason |
|---|---|---|
| Port | 80 | Standard HTTP. |
| `max_uri_handlers` | **100** | Was 80; bumped because we now register 86+ endpoints and ESP-IDF silently drops overflows. |
| `max_req_hdr_len` | 1024 (sdkconfig) | Android webviews exceed default 512 B. |
| `recv_wait_timeout` / `send_wait_timeout` | 30 s | Image uploads are slow over local AP. |
| Stack | 5120 B | |
| Core affinity | 0 | Keeps LVGL on core 1 free. |

### EMBED_TXTFILES mechanism

`main/web/index.html` is embedded as a binary blob via the CMake `EMBED_TXTFILES` target property. The linker exposes:

```c
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
```

The handler at `/` writes between those symbols straight to the response. **No filesystem read — the editor ships in flash.**

### The three copies of `index.html`

| Path | Used for | Synced by |
|---|---|---|
| [main/web/index.html](../../main/web/index.html) | Embedded in firmware (canonical) | Manual edits |
| [data/web/index.html](../../data/web/index.html) | LittleFS-served alternate; primary use is `tools/mobile-dev-server.js` for local browser testing | Manual copy from `main/web/` |
| `../rdm7-desktop/src/index.html` (Tauri app, separate repo) | Desktop Studio | Manual copy from `main/web/` |

There is **no automated sync**. Any edit to `main/web/index.html` must be propagated to the other two. The desktop copy has historically drifted; check `git log` on both files when investigating diffs.

`tools/mobile-dev-server.js` (Node) serves `main/web/index.html` and mocks all `/api/*` endpoints — useful for editor development without a device. Default port 8180. Wired to Claude Preview MCP via `.claude/launch.json` as `mobile-dev`.

### `WIDGET_DEFS` and `buildFirmwarePayload`

Inside `index.html`, `WIDGET_DEFS` is the metadata table that drives the editor: each widget type lists its display name, default size, allowed slots, and field schema (`fields[]` with type, label, range, etc.).

`buildFirmwarePayload(layout)` converts the in-editor representation to the firmware-expected JSON shape:

- `w.signal` (editor's name field) → `config.signal_name` (firmware's name field).
- Editor coordinates → device coordinates via `webToDev()`.
- Colors RGB888 (editor) → RGB565 (firmware), via `convertWidgetColors()` walking the field metadata.

`convertWidgetColors()` walks `fields[]` looking for entries with `type === 'color'`. Adding a color field to a widget that isn't in the metadata = the color survives unconverted. Always update `WIDGET_DEFS` when adding fields.

## API endpoint reference

Endpoints below are grouped by area. Method + path + body / response shape; line numbers are approximate against [main/net/web_server.c](../../main/net/web_server.c) at time of writing.

### Layout management

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/layout/version` | Lightweight version probe — `{"v": <long>}`. Studio polls this to detect external edits. |
| GET | `/api/layout/current` | Active layout JSON (post-merge of any in-memory edits). |
| GET | `/api/layout/raw?name=<n>` | Raw on-disk layout JSON (no merge). |
| POST | `/api/layout/save?apply=<0\|1>` | Save full layout. `apply=0` skips hot reload. |
| POST | `/api/layout/preview` | Apply layout in memory only — no save (used for "preview without save"). |
| GET | `/api/layout/list` | `{"active":"...","layouts":["a","b",...]}` |
| POST | `/api/layout/set` | `{"name":"<n>"}` — switch active. |
| POST | `/api/layout/delete` | `{"name":"<n>"}`. |
| POST | `/api/layout/rename` | `{"old":"...","new":"..."}` |

### Signals & fuel

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/signals/values` | Dynamic JSON of all signal current values. |
| GET / POST | `/api/signal/simulate` | Read or set simulator state. |
| POST | `/api/signal/inject` | `{"signal":"NAME","value":N}` — inject test value (bypasses CAN). |
| POST | `/api/signal/clear` | Clear all injected values. |
| GET | `/api/fuel/status` | Current fuel level + calibration. |
| POST | `/api/fuel/set-empty` | Calibrate empty point at current sender voltage. |
| POST | `/api/fuel/set-full` | Calibrate full point. |

### Capture (screenshot / streaming)

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/screenshot` | JPEG. Query: `full=1` (full res), `q=N` (quality), `smooth=1`, `raw=1`. Default is half-res for speed. |
| GET | `/screenshot` | Legacy alias. |
| GET | `/api/capture/stream` | MJPEG (multipart/x-mixed-replace) stream — for desktop Studio CONTROL mode (Safari uses polled `/api/screenshot` instead). |

Capture pipeline reads the panel framebuffer via `esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb0)` (single-pointer form, fb_num=1) and encodes via `esp_new_jpeg` with `JPEG_PIXEL_FORMAT_YCbYCr`. See [08-aux-systems.md](08-aux-systems.md) §screenshot.

### Touch / CONTROL mode

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/touch` | Read current state. |
| POST | `/api/touch` | `{"x":N,"y":N,"pressed":bool}` — inject event into virtual indev. |
| POST | `/api/indicator/test` | `{"slot":N,"active":bool}` — wire-mode indicator test. |
| POST | `/api/warning/test` | `{"slot":N,"active":bool}` — force alert active even without signal binding. |
| POST | `/api/screen/switch` | `{"screen":"<name>"}` — switch UI screen. |

CONTROL mode in the desktop Studio polls `/api/screenshot?full=1` every 1 s and forwards pointer events to `/api/touch`.

### Gear (CALCULATED_GEAR synthetic signal)

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/gear/config` | Current gear config. |
| POST | `/api/gear/config` | Set config; persists to NVS `gear_cal_cfg`. |

Body shape:

```json
{
  "wheel_circumference_m": 1.95,
  "final_drive": 4.11,
  "rpm_signal": "RPM",
  "speed_signal": "VEHICLE_SPEED",
  "ratios": [0.0, 3.321, 1.902, 1.308, 1.000, 0.759, 0.622],
  "enabled": true
}
```

### ECU

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/ecu/list` | Array of presets `[{"make":"...","version":"...","display":"..."}]`. |
| GET | `/api/ecu/current` | `{"make":"...","version":"..."}`. |
| POST | `/api/ecu/set` | Apply preset to active layout. |

8 presets shipped: AEM/Holley/Haltech/Link/MaxxECU/MS3-Pro/Ford BA-BF / Ford FG. Custom adds an additional "Custom" entry.

### Presets (custom signal sets)

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/presets` | Built-in presets from `ecu_presets.c`. |
| GET | `/api/presets/custom` | User-saved presets (LittleFS). |
| POST | `/api/presets/custom/save` | Save the active layout's signals as a preset. |
| POST | `/api/presets/custom/delete` | `{"ecu":"...","version":"..."}`. |

### Assets — images

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/image/upload` | Multipart form: `file`, `name`. Stored as `.rdmimg`. |
| GET | `/api/image/list` | `{"images":["..."]}` |
| POST | `/api/image/delete` | `{"name":"..."}` |
| GET | `/api/image/data?name=...` | Raw `.rdmimg` bytes (or PNG/JPG depending on encoder). |

### Assets — fonts

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/font/upload` | Multipart upload. TTF only. |
| GET | `/api/font/list` | `{"fonts":["..."]}` |
| POST | `/api/font/delete` | `{"name":"..."}` |
| GET | `/api/font/data?name=...` | Raw TTF binary. |

### Storage info

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/storage/info` | LittleFS used/total. |
| GET | `/api/sd/status` | SD mount + usage. |
| GET | `/api/sd/files?dir=...` | Directory listing. |
| POST | `/api/sd/copy` | `{"src":"...","dst":"..."}` |
| POST | `/api/sd/delete` | `{"path":"..."}` |

### Display & dimmer

| Method | Path | Purpose |
|---|---|---|
| GET / POST | `/api/brightness` | `{"brightness": 0..255}`. |
| GET / POST | `/api/dimmer/config` | `{signal_name, threshold, is_momentary, invert, dim_brightness, enabled}` — auto-dim driven by a signal (e.g. headlights). |

### Data logger

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/log/start` | Optional `{"rate_hz":N,"persist":bool}`. |
| POST | `/api/log/stop` | |
| GET | `/api/log/status` | `{active, file, samples, elapsed_ms, rate_hz}`. |
| GET | `/api/log/list` | `{"logs":[{"name","size_bytes","samples"}]}` |
| GET | `/api/log/download?file=...` | CSV bytes. |
| POST | `/api/log/delete` | `{"file":"..."}` |
| GET / POST | `/api/log/config` | `{"rate_hz":N}` — change rate mid-log. |

### Replay

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/replay/start` | `{"file":"...","speed":1.0,"loop":bool}`. |
| POST | `/api/replay/stop` | |
| GET | `/api/replay/status` | `{active, file, progress}`. |

### System & diagnostics

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/device/info` | `{device_id, firmware_version, serial, ip}`. |
| GET / POST | `/api/can/config` | Bitrate, enable/disable. |
| GET | `/api/system/health` | `{heap_free, heap_total, uptime_ms, temp_c, …}`. |
| POST | `/api/system/reboot` | Triggers reboot after a 1 s deferred task. |

### WiFi

| Method | Path | Purpose |
|---|---|---|
| GET / POST | `/api/wifi/config` | `{ssid, password, mode}`. POST switches mode/SSID. |

The multi-SSID UI uses additional internal endpoints (saved-list, scan, forget) — see `wifi_manager.c` for the full set.

### OTA

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/ota/status` | `{updating, progress, error}`. |
| POST | `/api/ota/check` | `{update_available, version}`. |
| POST | `/api/ota/start` | Begin OTA download + install. |

### Splash

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/splash/list` | `{active, splashes:[]}`. |
| POST | `/api/splash/set` | `{"name":"..."}`. |
| POST | `/api/splash/delete` | `{"name":"..."}`. |
| POST | `/api/splash/fade` | `{"duration_ms":N}`. |

### Captive portal probes

A handful of OS connectivity probes get redirected to `/`:

| OS | Path |
|---|---|
| iOS | `/hotspot-detect.html`, `/library/test/success.html` |
| Android | `/generate_204`, `/gen_204`, `/generate204` |
| Windows | `/connecttest.txt`, `/ncsi.txt`, `/redirect` |
| Firefox | `/success.txt` |

Handler: `captive_portal_redirect_handler`. Returns `302` with `Location` built from the request's `Host` header, plus a non-empty body — both required to satisfy iOS, which otherwise marks the AP "no internet" silently.

### CORS

| Method | Path | Behavior |
|---|---|---|
| OPTIONS | `/api/*` | Wildcard preflight. `Access-Control-Allow-Origin: *`, `Methods: GET, POST, OPTIONS`, `Headers: Content-Type`. |

The wildcard handler sits at the end of registrations, so it's important `max_uri_handlers` is high enough to register all explicit handlers first. If it fills up, the wildcard catches everything and returns 405 / silent OK on what should have been real endpoints.

### Static

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Serves `index.html` from EMBED_TXTFILES. |

## Auto-save vs hot reload

Two-step pattern that the studio uses:

1. **Auto-save during editing**: every ~1 s of editing debounces into `POST /api/layout/save?apply=0`. The handler writes to LittleFS but does **not** trigger a reload — the edit is durable, but the device keeps showing the old layout.
2. **Manual save (or apply=1)**: triggers `lv_async_call(_deferred_screen_reload, NULL)`. The reload tears down all widgets and rebuilds from the now-saved JSON.

Why split: editing is rapid (drag to move, type to change a label); rebuilding LVGL on every keystroke would flicker and burn cycles. The user sees their changes when they hit Save explicitly.

## DNS hijack (captive portal helper)

[main/net/dns_hijack.c](../../main/net/dns_hijack.c). Started from `main.c` after `web_server_start()`.

- UDP server on port 53.
- For every A query, returns the AP IP (live from `esp_netif_get_ip_info(WIFI_AP_DEF)`, default `192.168.4.1`).
- AAAA + unknown qtypes → NOERROR with zero answers (suppresses retry loops).

Why it exists: phones (especially Android) probe DNS *before* sending HTTP captive-portal probes. Without a DNS responder, the probe times out and the OS marks the AP "no internet", refusing to route browser traffic.

The task stack is allocated in **PSRAM** (`xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`) because internal RAM is fragmented after WiFi init. TX/RX buffers in BSS, not on stack — keeps the stack at 3 KB.

## mDNS state

[main/net/mdns_service.c](../../main/net/mdns_service.c) defines `RDM7_MDNS_DISABLED 1` at line 28. The hostname registration code is intact but never executes.

Reason: the managed `espressif/mdns` component hardcodes `CONFIG_MDNS_MEMORY_ALLOC_INTERNAL=y` and can't allocate from internal RAM after WiFi init, despite ~3.8 MB total heap free. Switching the config to `MDNS_MEMORY_ALLOC_SPIRAM` would work, but the project chose to drop mDNS entirely — QR code + IP fallback cover the user experience.

`device_settings.c` still mentions `rdm7.local` in some status labels — harmless cosmetic leftover, removable in a future cleanup pass.

## WebSockets

**None.** Every API uses request-response HTTP. MJPEG stream is multipart, not WebSocket.

## Error handling conventions

| Cause | Status |
|---|---|
| Invalid JSON | 400 |
| Missing required field | 400 with `{"error":"..."}` |
| Out of memory | 500 |
| File not found | 404 |
| Path with `..`, `\\`, or `/` in a filename slot | 400 (rejected at handler) |

Most handlers return `{"ok":true}` on success and `{"ok":false,"error":"..."}` on failure. The studio treats non-`{ok:true}` as an error toast.

## Adding a new endpoint

1. Write the handler:

   ```c
   static esp_err_t my_handler(httpd_req_t *req) {
       /* read body if POST */
       /* validate */
       /* do work */
       return httpd_resp_sendstr(req, "{\"ok\":true}");
   }
   ```

2. Register it in `web_server_start`:

   ```c
   httpd_uri_t my_uri = {
       .uri = "/api/my-thing",
       .method = HTTP_POST,
       .handler = my_handler,
   };
   httpd_register_uri_handler(server, &my_uri);
   ```

3. **Count `httpd_register_uri_handler` calls** before adding. If you exceed `max_uri_handlers = 100`, the registration silently fails and the wildcard `/api/*` OPTIONS handler will catch your endpoint — you'll see 405 in DevTools and a confusing path through CORS preflight.

4. If the handler reads/writes from LVGL state, **lock first** or use `lv_async_call` to defer to the LVGL task.

5. If it writes to LittleFS, take the LittleFS lock if there is one (currently none — be aware of concurrent writers).

6. Add an entry to the table in this doc.

## Known endpoint quirks

- **`/api/screenshot?full=1`** allocates a 960 KB buffer in PSRAM. Two simultaneous full-res requests can OOM. The handler retries with half-res on failure.
- **`/api/capture/stream`** holds the connection open. Killing the client doesn't always tear down on the server side until the next frame attempt — short-lived clients are fine, long polling can pile up.
- **`/api/touch` GET** returns the current latched state, not a stream. Useful for diagnosis but not for feedback during CONTROL mode.

---

Next: [08-aux-systems.md](08-aux-systems.md).
