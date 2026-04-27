# 04 — Signals & CAN Pipeline

The signal registry is the **only** decode layer between CAN bytes and widget logic. Widgets work in physical units (kPa, °C, RPM), never in raw bytes. This document describes the registry, the CAN RX pipeline, internal/synthetic signals, the simulator, and replay.

## The signal_t struct

[main/widgets/signal.h](../../main/widgets/signal.h):

```c
typedef struct {
    char     name[32];          // unique identifier; widgets bind by name
    uint32_t can_id;             // CAN message ID this signal lives in
    uint8_t  bit_start;          // first bit (0–63)
    uint8_t  bit_length;         // bits to extract (1–64)
    float    scale;              // multiplier
    float    offset;             // constant added after scale
    bool     is_signed;          // sign-extend extracted bits
    uint8_t  endian;             // 0 = Motorola big-endian, 1 = Intel little
    char     unit[8];            // display unit ("kPa", "°C", "rpm")

    float    current_value;
    float    peak_value;         // all-time max, persisted in NVS
    float    min_value;          // all-time min, persisted in NVS
    float    session_peak;       // max this boot only — not persisted
    float    session_min;        // min this boot only — not persisted
    bool     tracking_active;
    bool     is_stale;           // no frame in 2 s
    bool     test_locked;        // injected value pinned, blocks CAN
    uint64_t last_update_ms;

    signal_subscriber_t subscribers[MAX_SIGNAL_SUBSCRIBERS]; // 16 max
    uint8_t  subscriber_count;
} signal_t;
```

Capacity: `MAX_SIGNALS = 128`, `MAX_SIGNAL_SUBSCRIBERS = 16` per signal, `SIGNAL_TIMEOUT_MS = 2000`.

## Public API

Most public functions in [main/widgets/signal.h](../../main/widgets/signal.h):

### Lifecycle

| Function | Purpose |
|---|---|
| `signal_registry_init()` | Allocate the signal array in PSRAM. Idempotent. |
| `signal_registry_reset()` | Zero every slot. Called at the top of every layout load. |

### Registration & lookup

| Function | Purpose |
|---|---|
| `signal_register(name, can_id, bit_start, bit_length, scale, offset, signed, endian, unit)` | Returns index, or -1 on full / duplicate. |
| `signal_find_by_name(name)` | Returns index, or -1 if not found. Used by widget `from_json`. |
| `signal_get_by_index(index)` | Returns pointer or NULL. |
| `signal_get_count()` | Number of registered signals. |

### Subscription

| Function | Purpose |
|---|---|
| `signal_subscribe(index, cb, user_data)` | Subscribe to value changes. Up to 16 subs/signal. |
| `signal_unsubscribe(index, cb, user_data)` | Match on (cb, user_data). Must be called from `destroy()` before LVGL teardown. |

Subscriber callback type:

```c
typedef void (*signal_update_cb_t)(float value, bool is_stale, void *user_data);
```

Callbacks fire on the LVGL task with `lvgl_mux` held. Safe for direct `lv_*` calls.

### Dispatch & timeouts

| Function | Purpose |
|---|---|
| `signal_dispatch_frame(can_id, data, dlc)` | Decode every signal in the frame; call from LVGL task only. |
| `signal_check_timeouts(now_ms)` | Iterate signals; mark stale if `now - last_update_ms > 2000` ms. Called from a 500 ms LVGL timer in [dashboard.c](../../main/ui/dashboard.c). |
| `signal_inject_test_value(name, value)` | Manually push a value (web editor test slider, sim, replay). |
| `signal_set_test_lock(name, bool)` | Pin a signal at the injected value, blocking CAN updates. |

### Peak/min

| Function | Purpose |
|---|---|
| `signal_reset_peaks()` | Zero all peak/min, persist to NVS immediately. |
| `signal_reset_peak(index)` | Single-signal reset. |
| `signal_get_peak/min(index)` | Read all-time peak/min (persisted). |
| `signal_get_session_peak/min(index)` | Read this-boot peak/min. |
| `signal_reset_session_peak/min(index)` | Clear this-boot peak/min. |
| `signal_reset_all_session_peaks()` | Mass reset. Used by panel widgets when `show_peak` toggles. |

### NVS persistence

| Function | Purpose |
|---|---|
| `signal_peaks_load()` | Load peak/min from NVS after signals registered. |
| `signal_peaks_save_now()` | Flush dirty peaks immediately. |
| `signal_peaks_start_autosave()` | Background timer (~30 s) that flushes if dirty. |

## CAN RX pipeline

```
TWAI peripheral
    │ ISR
    ▼
can_receive_task (core 0, prio 7)             [main/can/can_manager.c]
    └─ twai_receive(&msg, 5 ms)
       └─ xQueueSend(s_can_queue, &msg, 0)    drops oldest on overflow
                                              (queue size = 64)

LVGL task (core 1, prio 8)                    [main/main.c lvgl_port_task]
    └─ on each LVGL iteration:
       └─ can_process_queued_frames()
          └─ for up to 32 frames per call:
             ├─ xQueueReceive(s_can_queue, &msg, 0)
             └─ if simulator NOT active:
                signal_dispatch_frame(msg.identifier, msg.data, msg.data_length_code)
                                              [main/widgets/signal.c]

signal_dispatch_frame
    └─ for each signal where signal->can_id == frame_can_id:
       ├─ raw = can_extract_bits(data, bit_start, bit_length, endian, is_signed)
       │                                       [main/can/can_decode.c]
       ├─ value = (float)raw * scale + offset
       ├─ if value differs OR was stale:
       │    update current_value, last_update_ms, peak/min, session_peak/min
       │    notify_subscribers(signal_index, value, false)
       └─ continue
```

Why batch 32 frames per call: prevents the LVGL task from being starved when the bus is saturated (e.g., 1 Mbit with broadcast traffic). 32 frames × 0.1 ms decode each = ~3 ms — well under one refresh tick.

Why drop oldest in the queue: at sustained overload we'd rather see the newest values than block CAN RX.

## Internal / synthetic signals

[main/widgets/signal_internal.c](../../main/widgets/signal_internal.c) hosts a 500 ms LVGL timer that injects values for synthetic signals via `signal_inject_test_value()`:

| Signal name | Source |
|---|---|
| `FPS` | LVGL refresh count / timer period |
| `CPU_PERCENT` | `100 - lv_timer_get_idle()` |
| `FREE_HEAP_KB` | `esp_get_free_heap_size() / 1024` |
| `FREE_PSRAM_KB` | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024` |
| `UPTIME_S` | `esp_timer_get_time() / 1e6` |
| `CHIP_TEMP` | On-chip temperature sensor (°C) |
| `INDICATOR_LEFT` | GPIO 43 read (active-low pull-up). Only registered if wiring enabled. |
| `INDICATOR_RIGHT` | GPIO 44 read. |
| `FUEL_SENDER_V` | ADC voltage on GPIO 6, raw or calibrated via fuel calibration table. |
| `WIFI_RSSI` | `esp_wifi_sta_get_ap_info().rssi`. |
| `CALCULATED_GEAR` | Derived from `RPM` + `VEHICLE_SPEED` signals + user-configured gearbox ratios + final drive + wheel circumference. Returns 0 (neutral) below 5 km/h or 500 RPM. Config persisted in NVS namespace `gear_cal_cfg`. See [08-aux-systems.md](08-aux-systems.md) §gear. |

Internal signals are registered in `signal_internal_start()`, called from `dashboard_init`. They behave like CAN signals from a widget's perspective: subscribe by name, get a callback when value changes.

## Signal simulator

[main/widgets/signal_sim.c](../../main/widgets/signal_sim.c). When active:

- A 40 ms (25 Hz) LVGL timer generates a triangle-wave sweep over a 3 s cycle for every signal with `can_id != 0`.
- Min/max bounds inferred from widgets that bind the signal (e.g. meter ranges, panel warning thresholds). Default is 0–100.
- `can_process_queued_frames` drains the CAN queue but **skips dispatch** while simulator is active — so test values aren't overwritten by live frames.
- Peak/min tracking is **disabled** during simulation: injections set `current_value` and notify subscribers without touching `peak_value`/`min_value`.

Toggled from Device Settings → TESTING → Sim ON/OFF, or from the web editor.

## CAN replay

[main/storage/signal_replay.c/h](../../main/storage/signal_replay.c). Plays back CSVs from `data_logger`:

```
CSV format:
    timestamp_ms,Signal A,Signal B,...
    0,123,45.6
    50,124,45.7
```

API:

| Function | Purpose |
|---|---|
| `signal_replay_start(path, speed, loop)` | Start replay. `speed` is multiplier (1.0 = real-time, 2.0 = 2×). |
| `signal_replay_stop()` | Stop. Safe no-op if inactive. |
| `signal_replay_is_active()` | Status. |
| `signal_replay_get_row()` / `_get_total_rows()` / `_get_file()` / `_get_speed()` | Progress. |

50 Hz LVGL timer paces injections to match the source timestamps. Empty CSV cells skip injection. Loop = restart on EOF.

Use cases:
- Verify a layout against last week's drive log without being in the car.
- Reproduce a specific incident captured on-device.

## Stale / timeout logic

```c
#define SIGNAL_TIMEOUT_MS  2000   // signal.h
```

A 500 ms LVGL timer in [dashboard.c](../../main/ui/dashboard.c) calls `signal_check_timeouts(now_ms)`:

- For each signal not currently stale, not test-locked: if `now - last_update_ms > 2000`, mark stale and notify subscribers with `is_stale = true`.
- Skipped entirely while simulator is active (sim keeps signals fresh by definition).

Widgets typically respond to staleness by greying out the value or showing a "—" indicator.

## Layout signal loading

[layout_manager.c](../../main/layout/layout_manager.c) `_load_signals(root)` runs after `signal_registry_reset()` and before any widget instantiation:

```json
{
  "signals": [
    { "name": "RPM",          "can_id": 256, "bit_start": 0,  "bit_length": 16,
      "scale": 1.0,           "offset": 0,   "is_signed": false, "endian": 1, "unit": "rpm" },
    { "name": "VEHICLE_SPEED","can_id": 256, "bit_start": 16, "bit_length": 16,
      "scale": 0.1,           "offset": 0,   "is_signed": false, "endian": 1, "unit": "km/h" }
  ]
}
```

Each entry produces one `signal_register` call. Duplicate names rejected. After loading, `signal_peaks_load()` populates peaks from NVS (stale records — those with no matching signal — are dropped).

## Widget subscription

The exact pattern every widget follows:

```c
/* widget_panel.c — _panel_create */
panel_data_t *pd = (panel_data_t *)w->type_data;

/* … build LVGL objects … */
w->root = lv_obj_create(parent);
pd->value_label = lv_label_create(w->root);
/* … */

/* signal subscription LAST — only after w->root is set */
if (pd->signal_index >= 0) {
    signal_subscribe(pd->signal_index, _panel_on_signal, w);
}
```

```c
/* widget_panel.c — callback */
static void _panel_on_signal(float value, bool is_stale, void *user_data) {
    widget_t *w = (widget_t *)user_data;
    if (!w || !w->root) return;
    panel_data_t *pd = (panel_data_t *)w->type_data;

    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", pd->decimals, value);
    lv_label_set_text(pd->value_label, buf);

    if (is_stale) lv_obj_set_style_text_color(pd->value_label, COLOR_STALE, 0);
    else          lv_obj_set_style_text_color(pd->value_label, pd->value_color, 0);
}
```

Destroy must unsubscribe:

```c
static void _panel_destroy(widget_t *w) {
    panel_data_t *pd = (panel_data_t *)w->type_data;
    if (pd->signal_index >= 0)
        signal_unsubscribe(pd->signal_index, _panel_on_signal, w);
    /* … free LVGL + type_data … */
}
```

Skip the unsubscribe and the next CAN frame after layout reload calls into freed memory.

## Things that update peaks vs. things that don't

| Source | Updates peak/min? |
|---|---|
| CAN frame dispatch | Yes |
| `signal_inject_test_value` (web editor test slider) | Yes — unless simulator active OR signal test-locked |
| Simulator | No |
| Replay | Yes (replay calls `signal_inject_test_value` per row) |
| Internal-signal timer | Yes (treated like CAN) |

Session peaks (`session_peak`/`session_min`) are tracked alongside all-time peaks but never persisted. Power-cycle clears them.

## Common signal-related pitfalls

- **Signal not in layout JSON.** A widget binds by name; if the signal isn't registered, `signal_find_by_name` returns -1 and nothing is subscribed. Widget displays factory default forever.
- **Subscribing in `from_json` instead of `create`.** Uses-after-free on first frame.
- **Assuming subscription order matches notification order.** It does today, but treat callbacks as asynchronous.
- **Reading `current_value` from a non-LVGL task.** No mutex around individual signal reads — race with the dispatch path. If you need a snapshot from a worker, post via `lv_async_call` or pull through the registry under `lvgl_mux`.
- **Re-registering a signal name with different parameters.** `signal_register` rejects duplicates. Edit the JSON and reload.

## Debugging signals

| Tool | What it gives you |
|---|---|
| Diagnostics screen → SIGNALS card | Counts of fresh / stale / total. |
| Peaks screen ([06-ui-and-screens.md](06-ui-and-screens.md)) | Live current/min/max table. |
| `/api/signals/values` | JSON dump of every signal's current value. |
| Web editor → left sidebar live signal cards | Per-signal value + decoded CAN bits expanded. |
| Serial: `signal.list` (see `serial_commands.c`) | Console dump. |
| Data logger CSV | After-the-fact analysis. |

---

Next: [05-storage-and-persistence.md](05-storage-and-persistence.md).
