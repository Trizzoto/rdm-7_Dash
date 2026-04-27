# 03 — Widget System

The widget layer is the heart of the firmware's UI. It is data-driven: every widget instance is described in layout JSON, instantiated at boot, and re-instantiated whenever the layout reloads.

## What a widget is

A widget is:

- A **type** (one of 13 enums in `widget_type_t`).
- A **slot** (instance index — most types are slot-limited).
- A **`type_data`** pointer to per-instance config + state (defined in the widget's header so `config_modal.c` can `#include` and read the real struct directly).
- A **`root` `lv_obj_t *`** — the LVGL container, plus whatever children the widget builds underneath.
- A **vtable** of function pointers (`create`, `from_json`, `to_json`, `destroy`, `apply_overrides`, `apply_night_mode`, `resize`, `open_settings`).

```c
struct widget_t {
    widget_type_t type;
    lv_obj_t *root;
    int16_t x, y;
    uint16_t w, h;
    char id[16];
    uint8_t slot;
    void *type_data;
    widget_rule_t *rules;
    uint8_t rule_count;
    uint32_t last_rule_mask;

    /* vtable */
    widget_create_fn           create;
    widget_resize_fn           resize;
    widget_open_settings_fn    open_settings;
    widget_to_json_fn          to_json;
    widget_from_json_fn        from_json;
    widget_destroy_fn          destroy;
    widget_apply_overrides_fn  apply_overrides;
    widget_apply_night_mode_fn apply_night_mode;
};
```

Defined in [main/widgets/widget_types.h](../../main/widgets/widget_types.h). The vtable signatures are typedef'd at the top of the same file.

## The 13 widget types

| Type enum | Name | File | Slots | Special |
|---|---|---|---|---|
| `WIDGET_PANEL` | Panel | [widget_panel.c/h](../../main/widgets/widget_panel.c) | 8 (0–7) | Warning thresholds, peak label, alerts |
| `WIDGET_RPM_BAR` | RPM bar | [widget_rpm_bar.c/h](../../main/widgets/widget_rpm_bar.c) | 1 (singleton) | Limiter flash/solid effects, redline zone |
| `WIDGET_BAR` | Bar | [widget_bar.c/h](../../main/widgets/widget_bar.c) | 2 | Anchor-based scale, image-based fills |
| `WIDGET_INDICATOR` | Indicator | [widget_indicator.c/h](../../main/widgets/widget_indicator.c) | 8 | Round/rect status light, color overrides |
| `WIDGET_WARNING` | Alert | [widget_warning.c/h](../../main/widgets/widget_warning.c) | 8 | Triggered by panel thresholds; image swap night mode |
| `WIDGET_TEXT` | Text | [widget_text.c/h](../../main/widgets/widget_text.c) | 8 | Free signal-bound text label |
| `WIDGET_METER` | Meter | [widget_meter.c/h](../../main/widgets/widget_meter.c) | 2 | LVGL `lv_meter`, dual-object night |
| `WIDGET_IMAGE` | Image | [widget_image.c/h](../../main/widgets/widget_image.c) | 8 | Static or signal-driven; dual-object night |
| `WIDGET_SHAPE_PANEL` | Shape panel | [widget_shape_panel.c/h](../../main/widgets/widget_shape_panel.c) | 2 | Geometric container |
| `WIDGET_ARC` | Arc | [widget_arc.c/h](../../main/widgets/widget_arc.c) | 2 | LVGL `lv_arc` |
| `WIDGET_TOGGLE` | Toggle | [widget_toggle.c/h](../../main/widgets/widget_toggle.c) | 2 | CAN-TX only (no signal RX) |
| `WIDGET_BUTTON` | Button | [widget_button.c/h](../../main/widgets/widget_button.c) | 4 | CAN-TX command button |
| `WIDGET_SHIFT_LIGHT` | Shift light | [widget_shift_light.c/h](../../main/widgets/widget_shift_light.c) | 1 | RPM-driven LED bar |

Slot count enforced by the constraints table in [widget_types.c](../../main/widgets/widget_types.c) ~line 81. Pixel min/max sizes also live there.

The grand-total registry cap is **32 widgets** (`WIDGET_REGISTRY_MAX`) — slot caps and global cap together prevent layout JSON from exhausting memory.

## Lifecycle

Widget instantiation happens inside [layout_manager.c](../../main/layout/layout_manager.c) `_instantiate_widgets()`. Each step has a precise reason:

```
1.  signal_registry_reset()
2.  _load_signals(root)              register CAN bindings before any subscribe
3.  for each widget JSON object:
4.      type = _type_from_str("panel" / "meter" / ...)
5.      w = _factory(type, json)     allocates widget_t + type_data, wires vtable
6.      widget_registry_add(w)       fails fast if cap exceeded
7.      w->from_json(w, json)        deserialize fields; signal_find_by_name() lookups
8.      widget_rules_from_json(w, cfg)
9.      w->create(w, parent)         build LVGL objects on parent
10.     widget_rules_subscribe(w)    only NOW because rules need w->root
11.     night_mode_subscribe(...)    if widget has apply_night_mode
12.     lv_obj_set_x/y(w->root, w->x, w->y)
```

Skipping or reordering step 10 / 11 causes use-after-free if the rule signal fires before `w->root` is valid. **Always subscribe after `create`.**

Teardown (in `dashboard_init` or layout reload):

```
1.  widget_rules_free(w)             (called from destroy)
2.  signal_unsubscribe for each subscription
3.  lv_obj_del(w->root)              destroys whole subtree
4.  free(w->type_data)
5.  free(w)
```

`night_mode_clear_subscribers()` is called once **before** the new layout loads, to drop any stale callback pointers from the previous layout. Critical — see [08-aux-systems.md](08-aux-systems.md) §night-mode.

## The vtable

Each function pointer:

| Slot | Signature | What it does |
|---|---|---|
| `create` | `void (*)(widget_t *w, lv_obj_t *parent)` | Build LVGL objects on `parent`; assign `w->root`. Called after `from_json`. |
| `resize` | `void (*)(widget_t *w, uint16_t w, uint16_t h)` | Resize root + reflow children. |
| `open_settings` | `void (*)(widget_t *w)` | Launch the on-device config modal for this widget. |
| `to_json` | `bool (*)(widget_t *w, cJSON *out)` | Serialize position/size + type-specific config (defaults-only). |
| `from_json` | `bool (*)(widget_t *w, cJSON *in)` | Deserialize and populate `type_data`. Resolve signal name → index. |
| `destroy` | `void (*)(widget_t *w)` | Free LVGL objects + `type_data`. Caller frees `w` itself. |
| `apply_overrides` | `void (*)(widget_t *w, rule_override_t *o, uint8_t n)` | Apply rule-triggered field overrides to live LVGL objects. |
| `apply_night_mode` | `void (*)(widget_t *w, bool active)` | Toggle night-mode color/image overrides. NULL if widget has no night support. |

`apply_overrides` and `apply_night_mode` are NULL for widgets that don't need them — the dispatch checks for NULL before calling.

## type_data

Each widget defines its `type_data` struct in **its header**, not its `.c`, so that `config_modal.c` can `#include` the header and read the real types directly. Example (panel):

```c
typedef struct {
    char     signal_name[32];
    int16_t  signal_index;
    char     label[16];
    uint8_t  decimals;
    char     custom_text[32];
    int      show_peak;          // 0=Off, 1=Max, 2=Min, 3=Both

    /* alert thresholds */
    bool     warn_high_enabled;
    float    warn_high_threshold;
    lv_color_t warn_high_color;
    /* …and warn_low equivalents */

    /* appearance */
    lv_color_t border_color;
    int16_t    border_radius;
    /* …more visual fields */

    /* live LVGL pointers (never serialized) */
    lv_obj_t *value_label;
    lv_obj_t *unit_label;

    /* night overrides — see §night mode below */
    panel_night_overrides_t night;
} panel_data_t;
```

Conventions:

- Fields you never want to serialise (LVGL pointers, runtime state) sit at the bottom — they are clobbered on `from_json` but never written by `to_json`.
- The factory (`widget_panel_create_instance`) sets factory defaults. `to_json` writes a field only if it differs from those defaults — see §JSON budget below.

## Factory dispatch

[layout_manager.c](../../main/layout/layout_manager.c) ~line 159:

```c
switch (type) {
case WIDGET_PANEL:        w = widget_panel_create_instance(slot); break;
case WIDGET_RPM_BAR:      w = widget_rpm_bar_create_instance(); break;
case WIDGET_BAR:          w = widget_bar_create_instance(slot); break;
case WIDGET_INDICATOR:    w = widget_indicator_create_instance(slot); break;
case WIDGET_WARNING:      w = widget_warning_create_instance(slot); break;
case WIDGET_TEXT:         w = widget_text_create_instance(slot); break;
case WIDGET_METER:        w = widget_meter_create_instance(slot); break;
case WIDGET_IMAGE:        w = widget_image_create_instance(slot); break;
case WIDGET_SHAPE_PANEL:  w = widget_shape_panel_create_instance(slot); break;
case WIDGET_ARC:          w = widget_arc_create_instance(slot); break;
case WIDGET_TOGGLE:       w = widget_toggle_create_instance(slot); break;
case WIDGET_BUTTON:       w = widget_button_create_instance(slot); break;
case WIDGET_SHIFT_LIGHT:  w = widget_shift_light_create_instance(slot); break;
}
```

Each `_create_instance` allocates a `widget_t`, allocates and zeroes the type-specific `type_data`, wires up the vtable, sets factory defaults, and returns the pointer. NULL on allocation failure.

The string-to-enum mapping is `_type_from_str()` in the same file (~line 108).

## Widget registry

[main/widgets/widget_registry.c/h](../../main/widgets/widget_registry.h) is a flat array of pointers, max 32:

| Function | Purpose |
|---|---|
| `widget_registry_reset()` | Zero all pointers — called at top of every layout load. |
| `widget_registry_add(w)` | Append. Returns false if full. |
| `widget_registry_count()` | Count of live widgets. |
| `widget_registry_snapshot(out, max, *count)` | Copy pointers into caller buffer. Used by dashboard.c. |
| `widget_registry_find_by_id(id)` | Look up by `widget_t.id` string. |
| `widget_registry_find_by_type_and_slot(type, slot)` | Look up by type + slot (ignores slot for singletons). Used by all `widget_panel_set_warning_high(slot, ...)`-style external accessors. |

The registry has no ownership semantics — it just tracks pointers. Memory is freed by `destroy()` on layout reload.

## Conditional rules system

Defined in [widget_rules.c/h](../../main/widgets/widget_rules.c) and [widget_types.h](../../main/widgets/widget_types.h):

```c
typedef struct {
    char             signal_name[32];
    int16_t          signal_index;
    rule_operator_t  op;          // > < >= <= == != range
    float            threshold;
    float            range_min, range_max;
    rule_override_t  overrides[MAX_RULE_OVERRIDES]; // up to 16
    uint8_t          override_count;
    bool             is_active;
} widget_rule_t;

typedef struct {
    char              field_name[RULE_FIELD_NAME_LEN];
    rule_value_type_t value_type;     // number, color, bool, string
    union { float num; uint32_t color; bool flag; char str[32]; } value;
} rule_override_t;
```

How it works:

1. On `from_json`, `widget_rules_from_json` populates `w->rules`.
2. On `create`, `widget_rules_subscribe(w)` calls `signal_subscribe` for every distinct signal in the rules. Callback is `_rule_signal_cb`.
3. When any of those signals updates, `_rule_signal_cb` re-evaluates **all** rules (it's cheap), builds a bitmask of which are active, and compares to `last_rule_mask`. **No change → no apply** (early-out in the common steady state).
4. On change, the callback merges all active rules' overrides into one merged list (later rules win on field conflicts) and calls `w->apply_overrides(w, merged, count)`.

The widget's `apply_overrides` is responsible for translating `rule_override_t` items back into LVGL property changes — usually a `for (i; i < n; i++) if (strcmp(o[i].field_name, "border_color") == 0) lv_obj_set_style_border_color(...)`.

## JSON serialisation budget — defaults-only convention

The whole layout file must fit **32 KB** (`LAYOUT_MAX_FILE_BYTES`, [main/layout/layout_manager.h](../../main/layout/layout_manager.h)). With up to 32 widgets, that's ~1 KB per widget on average — much less if signals + rules are also stored.

Each widget's `to_json` therefore writes a field **only if it differs from the factory default**:

```c
/* Pattern from widget_panel.c _panel_to_json */
if (pd->border_radius != 7)
    cJSON_AddNumberToObject(cfg, "border_radius", pd->border_radius);
if (pd->show_peak != 0)
    cJSON_AddNumberToObject(cfg, "show_peak", pd->show_peak);
if (memcmp(&pd->night, &(panel_night_overrides_t){0}, sizeof(pd->night)) != 0)
    /* ...emit night sub-object... */
```

`from_json` does the inverse: reads the field if present, otherwise leaves the factory default.

If you add a field, **add a defaults check in to_json**, otherwise small layouts blow past 32 KB.

## Night mode

Per-widget night overrides are declared via macros in [widget_night_helpers.h](../../main/widgets/widget_night_helpers.h):

```c
typedef struct {
    NIGHT_FIELD_COLOR(border_color)   // expands to: bool has_border_color; lv_color_t border_color;
    NIGHT_FIELD_COLOR(value_color)
    NIGHT_FIELD_IMAGE(image_name, 32) // expands to: bool has_image_name; char image_name[32];
} panel_night_overrides_t;
```

JSON parse + serialize use `NIGHT_PARSE_COLOR`, `NIGHT_SERIALIZE_COLOR`, etc. — they handle the `has_*` flag automatically.

At runtime, `apply_night_mode(w, active)` is fired by [system/night_mode.c](../../main/system/night_mode.c) via `lv_async_call`. Inside, the widget uses `NIGHT_PICK_COLOR(active, night, value_color, default_value)` to choose the override or fall back.

### Dual-object pattern

Some LVGL v8 properties can't be cleanly mutated at runtime — image source, `lv_meter` tick colors, line needle color, and so on. For those, the widget builds **two sets of LVGL objects** at create time, each with its day/night values baked in, and `apply_night_mode` toggles their `LV_OBJ_FLAG_HIDDEN`.

Used by [widget_image.c](../../main/widgets/widget_image.c), [widget_meter.c](../../main/widgets/widget_meter.c), [widget_warning.c](../../main/widgets/widget_warning.c). The pattern doubles draw memory but eliminates flicker on toggle.

## Adding a new widget

You're adding the 14th widget. Steps:

1. **Pick a name and module prefix** — e.g. `widget_gauge`. Files: `main/widgets/widget_gauge.c` + `widget_gauge.h`.

2. **Define `gauge_data_t` in the header**, with all serialised fields, LVGL pointers, and (if applicable) `gauge_night_overrides_t`. Header should follow the pattern from `widget_panel.h` exactly.

3. **Implement the vtable** in `widget_gauge.c`:
    - `widget_gauge_create_instance(uint8_t slot)` — factory.
    - `_gauge_create(w, parent)` — build LVGL objects, assign to `w->root`, set up children.
    - `_gauge_to_json(w, cfg)` — emit defaults-only fields.
    - `_gauge_from_json(w, cfg)` — read fields, fall back to factory defaults.
    - `_gauge_destroy(w)` — `widget_rules_free(w)`, `lv_obj_del(w->root)`, `free(pd)`, `free(w)`.
    - `_gauge_apply_overrides(w, overrides, count)` — translate rule overrides to property setters.
    - `_gauge_apply_night_mode(w, active)` — apply night overrides (or NULL if not supported).
    - Wire the function pointers in `widget_gauge_create_instance`.

4. **Register in [main/widgets/widget_types.h](../../main/widgets/widget_types.h)**: add `WIDGET_GAUGE = 13` to the enum, before `WIDGET_TYPE_COUNT`.

5. **Update [widget_types.c](../../main/widgets/widget_types.c)**:
    - Add an entry to `widget_constraints[]` with min/max width and height.
    - Add a case in `widget_type_name()`.

6. **Add factory call to [layout_manager.c](../../main/layout/layout_manager.c)**:
    - In `_type_from_str`: `else if (strcmp(s, "gauge") == 0) return WIDGET_GAUGE;`
    - In `_factory` switch: `case WIDGET_GAUGE: w = widget_gauge_create_instance(slot); break;`

7. **Add `widget_gauge.c` to `SRCS` in [main/CMakeLists.txt](../../main/CMakeLists.txt).**

8. **Update web editor metadata in `WIDGET_DEFS`** in all three copies of `index.html` ([main/web/index.html](../../main/web/index.html), [data/web/index.html](../../data/web/index.html), and `../rdm7-desktop/src/index.html`):
    - Add a new entry with `type: 'gauge'`, `label`, `defaultSize`, `slots`, `fields[]` array.
    - The `fields[]` describes inspector inputs and gets used by `convertWidgetColors()` and `buildFirmwarePayload()`.

9. **Update `_wstGetTestBounds` in `index.html`** to handle the new type if it has a signal-driven test slider.

10. **Bump `LAYOUT_SCHEMA_VERSION`** in [main/layout/layout_manager.h](../../main/layout/layout_manager.h) (currently 13). Schema-breaking changes warrant a bump; add a note in the migration section if you also need to upgrade old layouts on load.

11. **(Optional) on-device editing**: if your widget should be tappable on-device for in-place editing, add a section to [config_modal.c](../../main/ui/menu/config_modal.c). `config_modal_open_for_widget(screen, w)` receives the live `widget_t *` directly — no indirection layer needed.

12. **Test** the full save/load round-trip:
    - Create an instance via web editor → save layout.
    - Power-cycle.
    - Layout reloads, widget renders identically.
    - Tap the widget on-device → config modal opens (if you wired step 11).
    - Bind a signal, send CAN frames, confirm widget updates.

## Common widget-author mistakes

- **Subscribing in `from_json` instead of `create`.** `w->root` doesn't exist yet — first signal callback would dereference NULL.
- **Forgetting `widget_rules_free` in destroy.** Slow leak across layout reloads.
- **Missing `lv_obj_set_align(w->root, LV_ALIGN_CENTER)` before `lv_obj_set_pos`.** Position is relative to top-left without it; widget appears in the wrong place.
- **Mutating shared `lv_style_t`.** Widgets must use per-instance `lv_obj_set_style_*()` calls. Sharing styles across widgets means one widget's tweak ripples to all of them.
- **Not zeroing `type_data` in factory.** Stale memory from a previous allocation leaks in.
- **Writing all fields in `to_json` regardless of default.** Pushes layouts past 32 KB.

## Reference: external accessors

Widgets expose targeted setters for code that has to reach in from outside (e.g., the warning system from `widget_panel.c`). Pattern:

```c
/* in widget_panel.h */
void widget_panel_set_warning_high(uint8_t slot, bool enabled, float threshold, lv_color_t color);

/* in widget_panel.c */
void widget_panel_set_warning_high(uint8_t slot, bool enabled, float threshold, lv_color_t color) {
    widget_t *w = widget_registry_find_by_type_and_slot(WIDGET_PANEL, slot);
    if (!w) return;
    panel_data_t *pd = (panel_data_t *)w->type_data;
    pd->warn_high_enabled = enabled;
    pd->warn_high_threshold = threshold;
    pd->warn_high_color = color;
    /* re-render if the value is currently above threshold */
}
```

External accessors are how the on-device config modal mutates a live widget without touching JSON.

---

Next: [04-signal-and-can.md](04-signal-and-can.md).
