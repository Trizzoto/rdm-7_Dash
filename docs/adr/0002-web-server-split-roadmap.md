# ADR 0002 — `web_server.c` Split Roadmap

**Status**: Proposed (planning, not yet executed)
**Context**: `main/net/web_server.c` is ~4750 lines and houses all 86 HTTP endpoints, their helpers, the captive-portal probe handlers, the OPTIONS wildcard, and the registration block. Every endpoint addition shows up as a diff in the same file; merge conflicts are common; reasoning about a single domain (e.g., layout management) requires holding the whole file in your head. This ADR documents the proposed split, the cut lines we've identified, and the order to execute it in.

## Why we haven't done it yet

The cuts are all **mechanically safe** — no handler depends on a static helper from a different domain — but verifying that requires either an exhaustive read of the file or a build environment to catch a missed `static` helper. We don't have automated coverage of the 86 endpoints, and a regression in (say) `/api/layout/save` is silently bad: the editor would save without surfacing the failure.

The plan is therefore to do the split **with a human at a keyboard who can run `idf.py build` and exercise the endpoints**, not as part of a "do many things at once" pass.

## Target structure

After the split:

```
main/net/
  web_server.c            (~300 lines: web_server_start/_stop, REGISTER_URI macro,
                           cors_preflight_handler, _send_layout_too_large helper,
                           captive_portal_redirect_handler, the static httpd_uri_t
                           registrations are imported via *_register(server) calls)
  web_server_internal.h   (shared helpers — _name_is_safe, _send_layout_too_large,
                           the rdm_lvgl_lock externs, common limits, JSON helpers)
  web_server_layout.c     (≈ 9 endpoints, ~1200 lines)
  web_server_signals.c    (≈ 8 endpoints, fuel + sim + inject + clear)
  web_server_capture.c    (≈ 3 endpoints, /screenshot, /api/screenshot,
                           /api/capture/stream + dedup cache + FB-readback)
  web_server_touch.c      (≈ 5 endpoints, /api/touch GET+POST, /api/indicator/test,
                           /api/warning/test, /api/screen/switch)
  web_server_assets.c     (≈ 8 endpoints, image + font upload/list/delete/data)
  web_server_storage.c    (≈ 5 endpoints, sd, storage info)
  web_server_logger.c     (≈ 8 endpoints, log + replay)
  web_server_system.c     (≈ 5 endpoints, device/info, brightness, dimmer, can/config,
                           system/health, system/reboot)
  web_server_wifi.c       (2 endpoints, wifi/config GET+POST)
  web_server_ota.c        (3 endpoints)
  web_server_presets.c    (≈ 7 endpoints, ECU list/current/set, custom presets)
  web_server_splash.c     (4 endpoints)
  web_server_gear.c       (2 endpoints, gear/config GET+POST)
  web_server_captive.c    (9 captive-portal probe URIs; the redirect handler
                           moves here too)
```

## Cut lines (by line range in current `web_server.c`, approximate)

| New file | Source line range (approx) | Endpoints |
|---|---|---|
| `web_server.c` (kept) | 1–198, 4520–end | shared infra, cors, registration |
| `web_server_capture.c` | 199–457 | `/screenshot`, `/api/screenshot`, `/api/capture/stream`, dedup cache |
| `web_server_touch.c` | 459–611, 705–776, 3454–3540 | `/api/touch` (GET/POST), `/api/indicator/test`, `/api/warning/test`, `/api/screen/switch` |
| `web_server_captive.c` | 617–683 | 9 captive probe URIs + redirect handler |
| `web_server_gear.c` | 778–878 | `/api/gear/config` (GET/POST) |
| `web_server_layout.c` | 884–2004 | `/api/layout/version`, `/current`, `/raw`, `/save`, `/preview`, `/list`, `/set`, `/delete`, `/rename` |
| `web_server_presets.c` | 1285–1722 (interleaved with layout) | `/api/presets`, `/api/ecu/*`, `/api/presets/custom/*` |
| `web_server_splash.c` | 2009–2212 | `/api/splash/*` |
| `web_server_assets.c` | 2228–2700 (approx) | image + font upload/list/delete/data |
| `web_server_storage.c` | 3020–3454 | `/api/storage/info`, `/api/sd/*` |
| `web_server_system.c` | 4115–4294 | device/info, brightness, dimmer, can/config, system/health, reboot |
| `web_server_logger.c` | 3834–3990 | log + replay |
| `web_server_wifi.c` | 4488–4530 | wifi/config |
| `web_server_ota.c` | 4404–4470 | ota/* |
| `web_server_signals.c` | 4649–4733 | signals/values, signal/*, fuel/* |

Line ranges drift as the file is edited; treat them as starting hints, not gospel.

## Per-file structure

Each `web_server_<domain>.c` follows this pattern:

```c
/* web_server_layout.c — handlers for /api/layout/* */
#include "web_server_internal.h"
/* …other domain-specific includes */

/* (Static handlers — translated 1:1 from web_server.c) */
static esp_err_t layout_version_handler(httpd_req_t *req) { … }
static esp_err_t layout_current_handler(httpd_req_t *req) { … }
/* … */

/* (Static httpd_uri_t descriptors — translated 1:1) */
static const httpd_uri_t layout_version_uri = { … };
/* … */

/* Single registration entry point called from web_server_start. */
void web_server_layout_register(httpd_handle_t server) {
    REGISTER_URI(server, &layout_version_uri);
    REGISTER_URI(server, &layout_current_uri);
    /* … */
}
```

`web_server_internal.h` exposes:

```c
/* Layout JSON cap helper (used by layout + preview handlers). */
esp_err_t web_server_send_layout_too_large(httpd_req_t *req, size_t actual);

/* Path-traversal guard. */
bool web_server_name_is_safe(const char *name);

/* Counter macro + tally externs (so each register() function increments). */
extern int s_uri_register_attempts;
extern int s_uri_register_failures;
#define REGISTER_URI(svr, uri_ptr) /* … same as today … */

/* LVGL lock externs (kept as #include "ui/lvgl_helpers.h"). */

/* The various lv_async_call deferred entry points if they're shared. */
void web_server_deferred_screen_reload(void *arg);
/* … */
```

`web_server.c` boils down to:

```c
esp_err_t web_server_start(void) {
    /* …config + httpd_start as today… */

    REGISTER_URI(server, &cors_options_uri);  /* OPTIONS wildcard */
    web_server_captive_register(server);
    web_server_layout_register(server);
    web_server_signals_register(server);
    web_server_capture_register(server);
    web_server_touch_register(server);
    web_server_assets_register(server);
    web_server_storage_register(server);
    web_server_logger_register(server);
    web_server_system_register(server);
    web_server_wifi_register(server);
    web_server_ota_register(server);
    web_server_presets_register(server);
    web_server_splash_register(server);
    web_server_gear_register(server);

    /* …registration tally log + return… */
}
```

## Order of execution

Do these in **separate commits**, one per domain, in this order. After each, build and exercise endpoints from that domain via `curl` or the studio.

1. **Add `web_server_internal.h`.** Move shared helpers (`_send_layout_too_large`, `_name_is_safe`, the REGISTER_URI macro, the rdm_lvgl_lock externs). Keep the originals in `web_server.c` as `static` wrappers calling the shared versions until the rest of the split is done. Build, verify nothing changed.

2. **Split `web_server_captive.c`** first — smallest domain (9 single-line redirects + 1 handler), zero shared state. Lowest risk.

3. **Split `web_server_gear.c`** — 2 endpoints, talks only to `signal_internal_set_gear_cal` / `_get_gear_cal`.

4. **Split `web_server_touch.c`** — 5 endpoints, talks to `remote_touch.h`, indicator helpers, `widget_warning_apply_test_state`. Self-contained.

5. **Split `web_server_capture.c`** — 3 endpoints + the dedup cache. Touches `display_capture.c`. Self-contained.

6. **Split `web_server_signals.c`** — fuel + sim + inject + clear. Talks to signal registry + `signal_internal_set_fuel_cal`.

7. **Split `web_server_assets.c`** — image + font I/O. Self-contained.

8. **Split `web_server_storage.c`** — sd helpers. Self-contained.

9. **Split `web_server_logger.c`** — data logger + replay. Self-contained.

10. **Split `web_server_system.c`** — device info, brightness, can config, health, reboot.

11. **Split `web_server_wifi.c`** — 2 endpoints.

12. **Split `web_server_ota.c`** — 3 endpoints.

13. **Split `web_server_splash.c`** — 4 endpoints.

14. **Split `web_server_presets.c`** — ECU + custom presets.

15. **Split `web_server_layout.c`** — biggest, last. By the time you get here, the pattern is well-rehearsed.

After every step:

```bash
idf.py build                             # must produce zero warnings
idf.py -p COM5 flash monitor             # boot, watch for "URI registration: N handlers registered (cap 128)"
                                         # any "URI registration: X/N FAILED" means your register() function isn't wired in
curl http://192.168.4.1/api/<endpoint>   # exercise representative endpoints from the new domain
```

The web_server_start tally is the shibboleth: if the count drops between commits, a register() call was forgotten.

## Things to NOT do during the split

- **Don't rename handlers.** Keep `layout_save_handler` named `layout_save_handler`, even though `layout_handle_save` would be more idiomatic in the new file. Renames make `git log --follow` painful and add risk for no benefit.
- **Don't change endpoint behaviour.** Bug fixes go in their own commits, before or after the split, never inside.
- **Don't combine unrelated cleanups.** "While I'm here" is how splits become unreviewable.
- **Don't merge the captive probe URIs into one handler.** Each platform's probe URL is a separate entry in the table for reasons documented in [ADR 0001](0001-wifi-onboarding-reliability.md).
- **Don't split the OPTIONS wildcard.** It must stay registered last in `web_server.c` so it doesn't shadow the specific paths.

## Estimated scope

Roughly:
- 14 new `.c` files plus `web_server_internal.h`.
- ~4400 lines moved across files; net delta near zero (a few helpers extracted as functions in the header, otherwise a 1:1 move).
- One main/CMakeLists.txt edit to add the new SRCS.
- 14 commits of ~300 lines each on average. The captive split commit is ~80 lines; the layout split is ~1200.

## Why not split now

The author of this ADR (Claude, doing a remediation pass) cannot reliably build the project nor exercise the live endpoints without the developer running things by hand. The right time to do this work is during a session where the dev is iterating on web-API features anyway — the build/test cycle is already warm.

If you (the future dev) inherit this and want to start: do step 1 (`web_server_internal.h`) and step 2 (`web_server_captive.c`) on a quiet afternoon. If those land cleanly, the rest is mechanical.
