# 06 — UI Layer: Screens, Dashboard, Settings, Touch

What runs on the LCD: every screen, the dashboard core that hosts widgets, the settings overlays, the config modal, the touch input model, and the night-mode controller.

## The screen list

Every screen lives under [main/ui/screens/](../../main/ui/screens/) or [main/ui/](../../main/ui/).

| Screen | File | Purpose | Launched from |
|---|---|---|---|
| **Splash** | [splash_screen.c/h](../../main/ui/splash_screen.c) | Boot animation. Loads `_splash_<active>.json` if available; auto-fades to dashboard. | `app_main` after init |
| **First-run wizard** | [first_run_wizard.c/h](../../main/ui/screens/first_run_wizard.c) | 3-step onboarding (CAN auto-detect → WiFi connect → ECU pick). Sets NVS `first_run/done = true` on Finish. | Splash, when `first_run_done == false` |
| **ui_Screen3** (main dashboard) | [ui_Screen3.c/h](../../main/ui/screens/ui_Screen3.c) | Hosts the widget layout. MENU button (top-right), CAN-silent badge, widget tap routing. | After splash / wizard |
| **Device Settings** | [device_settings.c/h](../../main/ui/settings/device_settings.c) | Settings overlay: NETWORK, CAN BUS, DATA LOGGING, PEAK HOLD, TESTING, ROTATION, NIGHT MODE. | MENU button on ui_Screen3 |
| **WiFi Manager** | [ui_wifi.c/h](../../main/ui/screens/ui_wifi.c) | Multi-SSID connect/forget, scan, AP toggle. | "Connect WiFi" button in Settings |
| **Diagnostics** | [ui_diagnostics.c/h](../../main/ui/screens/ui_diagnostics.c) | Read-only system health: 5 cards (CAN, WiFi, System, SD, Signals). 1 s auto-refresh. | "Diagnostics" button in Settings |
| **Peaks** | [ui_peaks.c/h](../../main/ui/screens/ui_peaks.c) | Scrollable table of every signal's current/min/max. Per-row reset, Reset All. 100 ms refresh. | "View Peaks" in Settings |
| **ECU Picker** | [ui_ecu_picker.c/h](../../main/ui/screens/ui_ecu_picker.c) | Two-step picker: Make → Version. 8 ECU presets + Custom. | First-run step 2; "ECU" row in Settings |
| **Config Modal** | [config_modal.c/h](../../main/ui/menu/config_modal.c) + [menu_screen.c/h](../../main/ui/menu/menu_screen.c) | Per-widget editor, two tabs (Signal, Alerts). | Long-press on a signal-bound widget |
| **Gear Setup** | (modal in `device_settings.c` + `gear_config.c`) | Configure CALCULATED_GEAR (wheel, ratios, final drive). | Hamburger Tools or first-time pick of CALCULATED_GEAR |
| **OTA Update Dialog** | [ota_update_dialog.c/h](../../main/net/ota_update_dialog.c) | Modal showing OTA progress + release notes. | "Check Updates" or auto-check |
| **Update Banner** | (in dashboard) | Small banner appearing when OTA detects new version. | Periodic `check_for_update` |
| **ui_Screen1, ui_Screen2, ui_Screen4** | `.c` only | Legacy screen stubs not integrated with the layout system. | **Avoid using.** |

`ui_Screen3_screen_init()` is the canonical entry into dashboard rendering. After splash and wizard, control flows there.

## Dashboard core

[main/ui/dashboard.c/h](../../main/ui/dashboard.c) hosts the widget layout. Two entry points:

```c
void dashboard_init(lv_obj_t *parent);                    // first boot, also called on layout reload
void dashboard_apply_layout_json(cJSON *root, lv_obj_t *parent);  // hot reload (web save path)
void dashboard_persist_layout(void);                      // serialise widgets → save to active layout file
```

What `dashboard_init` does, in order:

1. `widget_registry_reset()` — clear pointers from any previous load.
2. `signal_registry_reset()` — same for signals.
3. `night_mode_clear_subscribers()` — drop stale callbacks. **Critical** — without this, the new layout's widgets can collide with subscriber slots from the old layout, and freed memory gets called.
4. `layout_manager_init()` (idempotent) — mount LittleFS, seed defaults if first boot.
5. `layout_manager_load(active, parent)` — full lifecycle from §03.
6. Fall back to hardcoded `widget_panel/rpm_bar/bar/etc.` if load fails.
7. `widget_registry_snapshot()` → `s_widgets[]`.
8. `_register_widget_long_press()` — wire up touch handlers + long-press routing into the config modal.
9. `signal_internal_start()` — start the 500 ms timer that refreshes synthetic signals.
10. `dimmer_subscribe()` — bind brightness dimmer to its configured signal (see [08-aux-systems.md](08-aux-systems.md) §dimmer).
11. `_setup_night_trigger()` — if layout has `night_mode.signal_name`, subscribe and switch on threshold cross.
12. `signal_check_timeouts` LVGL timer (500 ms) installed once.
13. `remote_touch_init(disp)` — **lazy-init virtual touch indev** (see §touch below).

`dashboard_apply_layout_json` does the same teardown + reload but starts from a cJSON tree (used by `/api/layout/save` hot reload).

`dashboard_persist_layout` walks the registry, calls each `to_json`, builds a cJSON root, and writes via `layout_manager_save_raw`. Called whenever the on-device config modal closes.

## Touch input

Two indev sources coexist:

### Real touch (GT911)

Initialised in `app_main` via `esp_lcd_touch_gt911`. Registered as the primary LVGL pointer indev. Interrupt-driven via INT pin (GPIO 4 reused as RST/INT).

### Virtual touch (`remote_touch.c/h`)

[main/system/remote_touch.c/h](../../main/system/remote_touch.c) — lets the web/desktop studio drive touch over HTTP. Used by **CONTROL mode** in the studio.

```c
void remote_touch_init(lv_disp_t *disp);                  // call from dashboard_init, NOT app_main
void remote_touch_set(int x, int y, bool pressed);        // thread-safe; web handler calls this
void remote_touch_force_release(void);                    // GT911 read_cb calls this when finger lands
void remote_touch_set_enabled(bool on);                   // disabling forces released state
```

Critical detail: `remote_touch_init` registers a **second LVGL indev**. This must happen **after** `dashboard_init` has built widgets, because LVGL's internal `lv_obj_get_screen()` walks the tree and infinite-loops if there's no widget to find.

Quirks the implementation handles:

- **Fast-click race**: web client sends down + up within one LVGL poll period. Without care, LVGL would never see the press. Fix: `s_release_requested` flag — defers the release for one read cycle so PR is reported, then REL.
- **Stuck-press watchdog**: if pressed for >350 ms with no further updates, force release. Prevents a network-dropped pointerup from leaving a phantom press blocking UI. 350 ms < LVGL's 400 ms long-press, so we don't trigger spurious long-presses.
- **Real touch wins**: when a physical finger lands, `force_release` cancels any virtual press in flight.

## Config modal — value_id mapping

The on-device editor maps a widget tap to a slot in the [config_bridge.c](../../main/ui/config_bridge.c) accessor table:

| value_id | Widget |
|---|---|
| 1–8 | Panel slots 0–7 |
| 9 | RPM bar (singleton) |
| 10–11 | (unused) |
| 12–13 | Bar slots 0–1 |

The bridge lookup `config_bridge_get_widget(value_id)` returns the live `widget_t *`. Field accessors (`config_bridge_get_can_id`, `config_bridge_set_warning_high_threshold`, …) read/write through `type_data` and into the signal registry where appropriate.

This is a **legacy mapping** — only panels, RPM bar, and bars are tap-editable on-device. Other widget types are edited only through the web editor. If you add on-device editing for a new widget type, extend value_id and add accessors.

### Config modal flow

```
GT911 long-press (>400 ms) on widget
    └─ _widget_long_press_cb (dashboard.c)
       └─ load_menu_screen_for_widget(w)
          └─ config_modal_open_for_widget(screen, w)
             ├─ Tab 1 (Signal): CAN ID, endian, bit start, bit length,
             │                  scale, offset, signed
             └─ Tab 2 (Alerts): high/low thresholds, colors
                                (only if widget_has_alert_support)

On Save:
    ├─ config_bridge writes back through type_data
    ├─ dashboard_persist_layout()        write JSON
    └─ reconfigure_can_filter()           refresh TWAI ID filter
```

## Device Settings sections

[device_settings.c](../../main/ui/settings/device_settings.c) is one big function that builds the settings overlay imperatively. Sections (current as of schema v13):

| Section | Controls |
|---|---|
| **NETWORK & UPDATES** | WiFi status + Connect button, web URL display, **Show QR** button, Check Updates button, device ID. |
| **CAN BUS** | Health dot + RX/TX/error counts (collapsible), bus scan overlay (auto-bitrate), ECU selector. |
| **DATA LOGGING** | Start/Stop log toggle, rate dropdown (1/2/5/10/20/50/100/200 Hz / Max), status label. |
| **PEAK HOLD** | View Peaks (opens overlay), Reset All. |
| **TESTING** | Sim ON/OFF toggle. |
| **DISPLAY** | Rotation cycle button (0°→90°→180°→270°), brightness slider. |
| **NIGHT MODE** | Manual toggle, signal-trigger config display. |
| **HOTSPOT** | "Hotspot on Boot" toggle (`wifi_boot_config_t.ap_enabled`). |
| **DANGER ZONE** | Factory reset, Reboot. |

Each control either pokes NVS via config_store, or triggers a sub-flow (WiFi UI, ECU picker, OTA dialog).

### QR code flow

Network section has a "Show QR" button. Backend:

1. `_build_web_url()` resolves the URL: prefer STA IP, fall back to AP IP, then `192.168.4.1`.
2. `_qr_btn_cb` opens a 400×440 modal on `lv_layer_top()`.
3. `lv_qrcode_create(parent, 280, black, white)` — uses `CONFIG_LV_USE_QRCODE`, no rebuild needed.
4. Modal close path NULLs the overlay pointer and validates with `lv_obj_is_valid` — guards against stale-pointer scenarios.

The QR replaces the mDNS `rdm7.local` UX (removed 2026-04-27, see [ADR 0001](../adr/0001-wifi-onboarding-reliability.md)).

## First-run wizard

[first_run_wizard.c](../../main/ui/screens/first_run_wizard.c). Three steps:

1. **CAN Auto-detect**: scans 125k / 250k / 500k / 1M. The first bitrate that produces frames within ~2 s is selected and persisted.
2. **WiFi Connect**: routes to `wifi_ui_show()` for SSID + password entry. Persisted in `wifi_cfg`.
3. **ECU**: opens `ecu_picker_open("default", allow_skip=true, callback)`. Selected preset overwrites the active layout's signal definitions.

On Finish, sets `first_run_done = true`. Subsequent boots skip the wizard. The wizard overlay is **hidden, not destroyed** during WiFi setup — a 200 ms poll re-reveals it once `wifi_ui_is_active()` returns false.

## Night mode controller

[main/system/night_mode.c/h](../../main/system/night_mode.c) is a singleton + subscriber list:

```c
void night_mode_init(void);                                   // idempotent
void night_mode_set_active(bool active);                       // thread-safe
bool night_mode_is_active(void);
void night_mode_subscribe(night_mode_cb_t cb, void *user_data);   // max 64
void night_mode_unsubscribe(night_mode_cb_t cb, void *user_data);
void night_mode_clear_subscribers(void);                       // dashboard_init calls this
```

State changes are dispatched via `lv_async_call` so callbacks always fire on the LVGL task with the mutex held.

Three trigger sources:

1. **Manual** — Settings toggle calls `night_mode_set_active(...)`.
2. **CAN signal** — Layout-level config (`night_mode.signal_name`, `active_when` operator/threshold). `_setup_night_trigger` in dashboard.c subscribes to that signal; on threshold cross it calls `night_mode_set_active`.
3. **Time-of-day** — Not implemented (needs RTC).

How widgets consume it: see [03-widget-system.md](03-widget-system.md) §night-mode and the dual-object pattern.

## Screen rotation & shape

Compile-time:
- `RDM_SCREEN_SIZE` (Kconfig) → `SCREEN_W`/`SCREEN_H` (system/screen_config.h).
- `RDM_SCREEN_SHAPE` → `SCREEN_SHAPE` (RECT or ROUND).

Runtime:
- Rotation 0/90/180/270 persisted in NVS `display_cfg/rotation`.
- Settings → "Rotate" cycles through and applies via `lv_disp_set_rotation(disp, rotation)`.
- Layouts are stored in the **logical** coordinate system (always with `(0,0)` at centre); rotation re-paints LVGL on the fly without rewriting layouts.

## Coordinate system

`(0,0)` = screen centre, **not** top-left. `SCREEN_ORIGIN_X` / `_Y` from [screen_config.h](../../main/system/screen_config.h) are convenience macros.

Pattern for placing any widget:

```c
lv_obj_set_size(obj, w, h);
lv_obj_set_align(obj, LV_ALIGN_CENTER);   /* MUST be before set_pos */
lv_obj_set_pos(obj, x, y);                /* x, y relative to centre */
```

Web editor converts via `devToWeb(x)` and `webToDev(x)`. Don't bake screen-space coordinates anywhere — always pull from `SCREEN_W/H` or `screen_get_profile()`.

## Where to extend the UI

| You want to | Touch this |
|---|---|
| Add a new screen overlay | `main/ui/screens/` — pattern is `<name>_show(parent)` and a static `_close_btn_cb` to destroy. |
| Add a settings toggle | Append a button in `device_settings.c`, wire to a `config_store_save_*` call. |
| Add a config-modal field | Extend `config_bridge` and `config_modal` for the relevant value_id. |
| Add a long-press action on a widget | Wire it into `_widget_long_press_cb` in `dashboard.c`. |
| Make night mode override a property | Use the macros in `widget_night_helpers.h`; add `apply_night_mode` to the widget vtable. |
| Add a soft button to ui_Screen3 | Edit `ui_Screen3.c`, anchor relative to `SCREEN_W/H`. |

## UI debugging

| Problem | First-line check |
|---|---|
| Tap doesn't open modal | `_widget_long_press_cb` not registered → `_register_widget_long_press` skipped because widget had no signal binding. Or LV_OBJ_FLAG_CLICKABLE is missing on `w->root`. |
| Settings overlay won't close | A child object holds focus. Check for a modal-layer override that wasn't NULLed. |
| Touch coords look offset | LCD rotation mismatch, or `lv_obj_set_align(LV_ALIGN_CENTER)` was skipped. |
| Night mode flickers some widgets | Widget uses `apply_night_mode` to mutate a property LVGL doesn't cleanly invalidate — should use the dual-object pattern. |
| Layout reload causes a brief blank screen | Expected — full teardown + rebuild. The black flash is shorter than 200 ms typically. |

---

Next: [07-web-server-api.md](07-web-server-api.md).
