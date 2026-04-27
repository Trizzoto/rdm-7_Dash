# 09 — Conventions & Pitfalls

The shape of code that lives well in this codebase, plus a catalogue of mistakes that have actually been made (and fixed) so you don't have to.

## Coding conventions

### Style

- **Language**: C11.
- **Identifiers**: `snake_case` for functions/vars, `UPPER_SNAKE_CASE` for macros and enum values, `CamelCase` only when forced by external APIs.
- **Module prefix**: every public function carries its module prefix — `widget_panel_*`, `signal_*`, `layout_manager_*`, `night_mode_*`, etc.
- **Files**: every implementation file `foo.c` has a paired header `foo.h` with `#pragma once` and `extern "C"` guards.
- **No Hungarian notation.** Type names are descriptive; pointer status is contextual.

### Logging

```c
static const char *TAG = "panel";   // module-level, near the top of the .c

ESP_LOGI(TAG, "loaded %d panels", count);
ESP_LOGW(TAG, "signal '%s' not registered, panel %d will be inert", name, slot);
ESP_LOGE(TAG, "alloc failed");
ESP_LOGD(TAG, "verbose detail visible only with esp_log_level_set");
```

Set per-module log level in `app_main` or via serial command at runtime. Do **not** `printf` directly — it bypasses log routing and breaks `idf.py monitor`'s panic decoder.

### Memory

- Internal SRAM is precious. Default to PSRAM for non-DMA, non-stack allocations:

  ```c
  void *buf = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
  ```

- Small-allocation threshold: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL = 8192`. Allocs ≤ 8 KB stay in internal RAM via plain `malloc`/`calloc`.
- DMA buffers and FreeRTOS task stacks must be in internal RAM (or the explicit `MALLOC_CAP_DMA` / `MALLOC_CAP_INTERNAL` flags). Exception: the DNS hijack stack lives in PSRAM via `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`.

### LVGL

- LVGL v8 styling API: `lv_obj_set_style_*(obj, value, selector)`. Do **not** import patterns from v9 documentation — the prop+selector form is incompatible.
- Always `lv_obj_set_align(obj, LV_ALIGN_CENTER)` **before** `lv_obj_set_pos(obj, x, y)`. The order matters because alignment changes the origin used by `set_pos`.
- Per-instance styles via `lv_obj_set_style_*()` — **never** create a single `lv_style_t` and reuse it across widgets. Mutating it from one widget invalidates the others.
- LVGL events: `lv_obj_add_event_cb(obj, cb, LV_EVENT_X, user_data)`. The user_data convention is "pointer to the owning `widget_t`".

### Threading

| Where you are | Rule |
|---|---|
| LVGL task (timers, signal callbacks, event handlers) | `lvgl_mux` already held — call `lv_*` freely. |
| HTTP handler, web upload | Lock with `example_lvgl_lock(timeout_ms)` before `lv_*`, or use `lv_async_call`. |
| CAN RX task | Never call `lv_*` directly. Enqueue, let LVGL drain. |
| Any other task | `example_lvgl_lock` or `lv_async_call`. |

`lv_async_call(cb, user_data)` is the safe escape hatch from any context. Callbacks run on the LVGL task with the mutex held.

### JSON

- Parse with cJSON. Always `cJSON_Delete` the root once done (or carefully transfer ownership).
- Serialise with cJSON — write defaults-only in widget `to_json` (see [03-widget-system.md](03-widget-system.md) §json-budget).
- Layout files cap at 32 KB — don't be cavalier with optional fields.

### Comments

Default to **none**. Only add a comment when the *why* is non-obvious — a hidden constraint, a workaround for a specific bug, an invariant that's easy to break.

Don't write comments that:
- Repeat the code (`/* increment counter */ counter++;`).
- Reference the current task or PR ("added for the speedo refactor").
- Explain stable concepts that are documented elsewhere ("this is LVGL's event system").

If you find yourself writing a multi-paragraph docstring on a function, the function is probably doing too much.

### Compiler flags

- `-Werror=comment` is active — `/*` inside a block comment fails the build. Watch out for:
  ```c
  /* outer comment
     /* nested */ <- BREAKS
   */
  ```
- `-Wno-format` is active — printf-spec mismatches don't fail the build. Don't rely on this; fix mismatches when you see them.

### Headers

```c
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// public API …

#ifdef __cplusplus
}
#endif
```

Forward declarations preferred over including more headers. type_data structs are an exception — they live in widget headers because [config_bridge.c](../../main/ui/config_bridge.c) needs the real types.

## Workflow conventions

### Don't run builds without permission

Builds take 1–2 minutes and the user runs them externally. If you're an agent: do not run `idf.py build` unless the user explicitly asks. Verify your work by reading code and reasoning, not by spinning up `idf.py`.

### Three copies of `index.html`

Any change to widget metadata or web-editor logic must be propagated to:

1. [main/web/index.html](../../main/web/index.html) — embedded in firmware.
2. [data/web/index.html](../../data/web/index.html) — used by `tools/mobile-dev-server.js`.
3. `../rdm7-desktop/src/index.html` — separate Tauri app repo.

There's no automation. The desktop copy has historically drifted; eyeball-diff before pushing.

### Bump `LAYOUT_SCHEMA_VERSION` when needed

Currently 13. Bump it whenever layout JSON parsing changes in a non-backward-compatible way. Add a `_migrate_*` helper if old layouts need rewriting on load.

### `to_json` is defaults-only

Every widget's `to_json` writes only fields that differ from factory defaults. Forgetting this **silently** pushes layouts past 32 KB and they fail to save with no obvious error.

### Test the full save → reload cycle

Don't ship a widget change without verifying:
1. Save layout via the web editor.
2. Power-cycle the device.
3. Reopen the editor — the widget renders identically.

The bugs that hurt most live in the round-trip.

### Commit etiquette

- Conventional Commits style: `feat(meter): tick labels follow the anchor curve`.
- Subsystem in parentheses where it helps: `widget_*`, `signal`, `layout`, `web`, `ui`, `feat`, `fix`, `refactor`, `ui`.
- Keep commit messages descriptive — they're the historical record when memory fades.

## Common pitfalls — and what to do about them

A curated list of failure modes that have hit this project. Each entry: *symptom → cause → fix*.

### Threading

**Symptom**: random LVGL crash on CAN traffic spike.
- **Cause**: a signal subscriber called `lv_*` from CAN RX context.
- **Fix**: subscribers run on LVGL task already; you didn't break that — but make sure you don't call signals' subscriber list directly from another task.

**Symptom**: HTTP handler hangs.
- **Cause**: `example_lvgl_lock(-1)` with LVGL task blocked elsewhere.
- **Fix**: pass a finite timeout (100–500 ms) and bail with `lv_async_call` if it fails.

**Symptom**: TWDT firing during boot widget instantiation.
- **Cause**: insufficient yields on the 500 Hz tick (see below).
- **Fix**: `vTaskDelay(1)` literal yields are sprinkled through `_instantiate_widgets`, `rdm_image_load`, `font_manager_get`. TWDT was raised from 5 → 15 s as a backstop.

### `pdMS_TO_TICKS(1) == 0`

**Symptom**: a `vTaskDelay(pdMS_TO_TICKS(1))` produces no real yield.
- **Cause**: `CONFIG_FREERTOS_HZ = 500`. `pdMS_TO_TICKS(1) = (1 * 500) / 1000 = 0`.
- **Fix**: write `vTaskDelay(1)` (one tick = 2 ms) directly when you mean "yield to other tasks".

### Widget not updating

**Symptom**: signal value changes (visible in `/api/signals/values`) but a widget shows zero forever.
- **Cause #1**: the signal is missing from the layout's `signals` array, so `signal_find_by_name` returned -1 in `from_json`.
- **Cause #2**: `signal_subscribe` was called in `from_json` instead of `create` — the subscribe failed silently because the widget wasn't fully built. (Or worse, the callback fires before `w->root` exists, NULL-derefs.)
- **Fix #1**: add the signal to the layout JSON.
- **Fix #2**: subscribe **after** `create` in the lifecycle (see [03-widget-system.md](03-widget-system.md)).

### Widget appears in wrong position

**Symptom**: every widget is offset to the upper-left.
- **Cause**: `lv_obj_set_align(obj, LV_ALIGN_CENTER)` was missing or called *after* `lv_obj_set_pos`.
- **Fix**: align *before* set_pos, always.

### Colors look wrong

**Symptom**: a color that's red in the editor renders bluish on the device.
- **Cause #1**: `convertWidgetColors()` doesn't have an entry for the new field — it stays in RGB888.
- **Cause #2**: a literal hex was hand-written using LVGL's `lv_color_hex(0x...)` form when the editor sends the raw 24-bit int.
- **Fix #1**: add the field to `WIDGET_DEFS` `fields[]` with `type: 'color'`.
- **Fix #2**: use `rgb888to565()` / `rgb565to888()` consistently at the boundary.

### Field doesn't persist

**Symptom**: edit a field in the modal, save, reload — value reverts.
- **Cause #1**: `to_json` doesn't emit the field (defaults-only check is wrong).
- **Cause #2**: `from_json` doesn't read the field (typo'd JSON key).
- **Fix**: add a printf to verify both directions; the first one to be silent is the one to fix.

### Config bridge breaks after refactor

**Symptom**: editing a panel slider crashes or writes the wrong field.
- **Cause**: type_data struct field was renamed/removed; config_bridge still references the old layout.
- **Fix**: type_data structs live in widget headers, and config_bridge.c includes them directly. When you rename a field, fix every accessor in [main/ui/config_bridge.c](../../main/ui/config_bridge.c).

### LVGL crash after layout reload

**Symptom**: a CAN signal updates, LVGL hard-crashes shortly after a layout change.
- **Cause**: stale signal subscriber pointing at a freed widget. Likely missed `signal_unsubscribe` in widget destroy.
- **Fix**: every widget's `destroy` MUST `signal_unsubscribe` for every subscription it made. Pair every `signal_subscribe` with an explicit cleanup.

### Night mode flicker

**Symptom**: switching night mode, one widget pops back to day briefly before settling.
- **Cause**: widget's `apply_night_mode` mutates a property that LVGL doesn't cleanly invalidate (image source, gauge needle color, `lv_meter` tick color).
- **Fix**: use the dual-object pattern. Build a hidden sibling at create with night values baked in; `apply_night_mode` toggles `LV_OBJ_FLAG_HIDDEN`. See widget_image, widget_meter, widget_warning.

### `lv_obj_set_style_*` invalidates even when value is unchanged

**Symptom**: dropping FPS during steady-state.
- **Cause**: a widget calls `lv_obj_set_style_text_color(...)` every signal frame. The setter always invalidates even when the value matches.
- **Fix**: gate runtime style writes on state transitions. Cache `last_color`; only write when it changes.

### Touch boot bug

**Symptom**: touch is unresponsive after a clean boot.
- **Cause**: GT911 reset sequence not honoured (specific GPIO timing). Or `gpio_set_level` was called on a pin not configured as an output — silent no-op.
- **Fix**: ACK-probe the GT911 over I²C before touch driver init. Verify GPIO output config explicitly.

### Web editor saves all fields, layout exceeds 32 KB

**Symptom**: `/api/layout/save` returns 500 or the file silently truncates.
- **Cause**: a new field's `to_json` writes the field unconditionally, not "if differs from default".
- **Fix**: every `to_json` must compare against factory default and skip if equal.

### `httpd_register_uri_handler` returned ESP_ERR_HTTPD_HANDLERS_FULL

**Symptom**: a newly-added endpoint returns 405 / falls into the OPTIONS wildcard.
- **Cause**: `max_uri_handlers = 100` filled. ESP-IDF silently drops registrations past the cap.
- **Fix**: count `httpd_register_uri_handler` calls. Currently we have 86 — we can bump the cap or drop dead handlers.

### Phone won't connect to the AP / says "no internet"

**Symptom**: phone joins `RDM7-XXXX` but can't reach `192.168.4.1`, OS says "no internet".
- **Cause #1**: DNS hijack not running — Android probes DNS first, times out.
- **Cause #2**: Captive-portal HTTP probes return 200 with empty body — iOS marks "no internet".
- **Cause #3**: AP channel collision (channel 1 too noisy in some environments).
- **Fix**: confirm `dns_hijack_start_task` ran (boot log). Confirm captive probes return 302 with non-empty body. Confirm `wifi_manager` set channel 11 + HT20.

### Layout dropdown didn't apply

**Symptom**: select a layout in the editor, nothing happens until you click Save.
- **Cause**: dropdown `onchange` didn't `POST /api/layout/set` before fetching.
- **Fix**: this was fixed in `index.html`. If reintroducing similar UI, mirror the pattern.

### Hamburger select-clone bug

**Symptom**: cloned `<select>` shows the default option, not the live value.
- **Cause**: `cloneNode` preserves the HTML `selected` attribute, not the live `.value`.
- **Fix**: `clone.value = orig.value` immediately after cloneNode.

### Boot splash drifts back into desktop copy

**Symptom**: desktop Studio shows old splash + preconnects + odd header padding.
- **Cause**: `rdm7-desktop/src/index.html` was overwritten by the firmware copy.
- **Fix**: that's the documented sync direction; if desktop has additions (boot splash, preconnects), reapply them after each sync. Or make the desktop additions conditional (`<template>` blocks).

### CONTROL stuck press

**Symptom**: tapping in CONTROL mode opens config modal as if it were a long-press.
- **Cause**: `pointerup` could be dropped when the studio's `<img>` returned 0×0 rect, leaving firmware in pressed state. Next tap → firmware sees ~400 ms gap → long-press fires.
- **Fix**: client falls back img→canvas→cached pos→(0,0). Firmware has 350 ms idle watchdog that auto-releases. Both layered.

### `remote_touch_init` from `app_main`

**Symptom**: first widget create after boot infinite-loops in `lv_obj_get_screen`.
- **Cause**: `remote_touch_init` registered a second LVGL pointer indev before any widget existed. `get_screen` walks the tree looking for the active screen and never returns.
- **Fix**: call `remote_touch_init(disp)` from `dashboard_init`, after widgets are built. It's idempotent.

### mDNS not resolving

**Symptom**: `rdm7.local` doesn't resolve on the user's network.
- **Cause #1**: many consumer routers drop multicast / use AP isolation.
- **Cause #2**: in this firmware, mDNS is permanently disabled — see [main/net/mdns_service.c](../../main/net/mdns_service.c).
- **Fix**: don't depend on `rdm7.local`. Show the IP in Device Settings. Offer the QR code button. The labels in `device_settings.c` still mention `rdm7.local` — harmless cosmetic leftover, removable in a future cleanup.

### "Where did the 768 KB of PSRAM go?"

**Symptom**: free PSRAM is suddenly 768 KB lower than expected.
- **Cause**: someone re-introduced the shadow framebuffer in `flush_cb`. It was killed during the screenshot pipeline rewrite (panel framebuffer is read directly via `esp_lcd_rgb_panel_get_frame_buffer`).
- **Fix**: don't reintroduce it. If you need a shadow buffer, allocate it on demand and free it.

### Replay not respecting timestamps

**Symptom**: replay races through a CSV instead of pacing it.
- **Cause #1**: speed multiplier set very high.
- **Cause #2**: replay timer not on LVGL task — multiple injections collide.
- **Fix**: replay must run on LVGL task (currently does). Speed defaults to 1.0.

### Peak Hold "all-time" reset on power-cycle

**Symptom**: after a reboot, peaks are zero again.
- **Cause**: `signal_peaks_load()` not called at boot, OR the NVS namespace was wiped.
- **Fix**: confirm `signal_peaks_load` runs after `_load_signals`. If NVS was nuked (factory reset), it's expected to start fresh.

### Layout JSON >32 KB silently fails

**Symptom**: editor save returns ok, but next reboot uses an old layout.
- **Cause**: layout exceeded `LAYOUT_MAX_FILE_BYTES` (32 KB); save returned an error code that the studio dropped.
- **Fix**: enforce the size in the editor pre-save. Audit `to_json` defaults-only logic.

### Web editor "test" button does the wrong thing

**Symptom**: clicking test on an indicator widget injects via `/api/signal/inject` even though the widget is in wire-mode (no signal).
- **Cause**: properties panel `hasSignalBinding` exclusion list — wire-mode widgets fall through.
- **Fix**: route via `/api/indicator/test` (slot from `w.config.slot`, NOT `w.slot`) when no signal is bound.

### Hamburger / wizard rdm7.local references

**Symptom**: user sees `rdm7.local` in network status labels, expects it to work, doesn't.
- **Cause**: leftover labels from before mDNS was disabled.
- **Fix**: cosmetic only — remove from `device_settings.c:156, 166, 181` in a future pass. (Logged as cleanup work.)

## Things to verify before merging a non-trivial change

- [ ] Builds (when you ask the user to build).
- [ ] Save → power cycle → reload preserves state.
- [ ] No leak across layout reloads (check `/api/system/health` heap_free over 5 reloads).
- [ ] LVGL mutex held for every `lv_*` call in your new code path.
- [ ] If you added a widget field: `to_json` defaults-only, `from_json` reads it, `WIDGET_DEFS` updated in all 3 `index.html`, `convertWidgetColors` handles it (if color).
- [ ] If you added an HTTP endpoint: counted handlers (≤ 100), responds with JSON, errors return non-200 with `{ok:false,error:...}`.
- [ ] If you added a thread: pinned to a core, prio ≤ 8, stack sized appropriately, yields correctly.
- [ ] If you added a signal: timeout behavior is reasonable, peak/min tracking semantically correct.
- [ ] Logs are quiet at INFO level in steady state (no per-frame spam).

---

Next: [10-module-reference.md](10-module-reference.md).
