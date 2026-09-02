# Conditional rules & night mode

Two ways a widget changes its own appearance without you writing any code: a
**rule** restyles it when a signal crosses a threshold, and a **night override**
restyles it when the dash goes dark. Both live in the widget's `config`.

---

# Conditional rules — `config.rules`

```jsonc
{
  "type": "panel", "id": "ect", "x": -160, "y": 60, "w": 155, "h": 92,
  "config": {
    "signal_name": "COOLANT_TEMP",
    "value_color": 65535,
    "rules": [
      { "signal_name": "COOLANT_TEMP", "op": ">", "threshold": 105,
        "overrides": [
          { "field": "value_color", "type": "color", "value": 63488 },
          { "field": "bg_color",    "type": "color", "value": 8192  }
        ] },
      { "signal_name": "COOLANT_TEMP", "op": "range",
        "range_min": 60, "range_max": 95,
        "overrides": [ { "field": "value_color", "type": "color", "value": 2016 } ] }
    ]
  }
}
```

**Rule object**

| key | required | meaning |
|---|---|---|
| `signal_name` | yes | the signal to watch — an **UPPERCASE registry name** from `/api/channels`, not a channel id. A rule whose `signal_name` is empty or unresolvable is inert and silently dropped. |
| `op` | yes | `">"` `"<"` `">="` `"<="` `"=="` `"!="` `"range"`. An unrecognised string falls back to `"=="` with a warning in the log. |
| `threshold` | for all but `range` | the value compared against |
| `range_min` / `range_max` | for `range` | inclusive bounds |
| `overrides` | yes | what to change while the rule is active |

**Override object**

| key | meaning |
|---|---|
| `field` | the config field name to override — max **32 chars** |
| `type` | `"number"` \| `"color"` \| `"bool"` \| `"string"` |
| `value` | a JSON number (colours are **RGB565 integers**), boolean, or string to match `type` |

### Semantics that bite

- **Later active rules win.** Overrides from all currently-active rules are
  merged in array order; if two active rules touch the same `field`, the one
  **later in the array** takes it. Order your rules least-specific first.
- **Deactivating restores the base value.** When no rule is active the widget
  re-applies its own `config` values. You don't need an "else" rule.
- **Caps:** 16 rules per widget, 16 overrides per rule, 32 distinct merged
  fields. Extras are dropped without complaint.
- **Not every field is overridable.** A rule can only change what the widget's
  `apply_overrides` handles — mostly colours, opacities and a few booleans.
  Geometry and asset fields generally aren't. When in doubt, apply it and look.
- **`pathbar` and `anim` have no rule support at all.** Rules on them are ignored.
- **Rule-only fields** exist: the catalog flags them **R**. `warning.lamp_on` is
  the current one — it is *not* a `config` key, only a rule override target. Use
  it to keep a warning lamp dark until a reading actually matters (e.g. force
  the lamp off below 90 °C, on above).
- A rule signal that is stale/absent leaves the rule inactive — the widget shows
  its base styling, not an error.

---

# Night mode

## Per-widget overrides — `config.night`

```jsonc
"config": {
  "value_color": 65535,
  "label_color": 36020,
  "night": {
    "value_color": 27568,
    "label_color": 16904
  }
}
```

Each key inside `night` is **the same name as the field it overrides**, and only
fields the catalog flags **N** are supported (70 of them across 16 widgets).
Where the night key differs from the field name, the catalog prints it after the
flag as **N**(`key`) — `warning.border_color_style` overrides as
`night.border_color` — so read the flag rather than assuming.

Omit a key and that field keeps its day value; `night` itself is optional. Only
emit the ones you actually change (defaults-only still applies).

**Exception:** `track_map` does **not** honour `config.night` — the firmware has
no night path for it, despite the schema marking its colours overridable.
`pathbar` has no night support either.

### Why some fields need the dual-object trick

LVGL v8 bakes certain properties in at create time (image source, needle colour,
tick colours). Those widgets build a hidden sibling at create and toggle
visibility on switch — so `night.needle_color`, `night.image_name` and friends
work, but they cost a second object. Nothing for you to do in the JSON; it's
just why the N-flag list is shorter than the colour-field list.

## The layout-level trigger — `night_mode`

A top-level key on the layout object, not on a widget:

```jsonc
{
  "schema_version": 18,
  "name": "my_layout",
  "night_mode": { "signal_name": "HEADLIGHTS", "active_when": 1 },
  "widgets": [ … ]
}
```

The dash goes to night when that signal reads `active_when`. Bind it to a
headlight/illumination channel (`headlights`, `high_beam`) — read the real
signal name out of `/api/channels`. Omit the key entirely and night mode is
driven only by the manual setting.

Time-of-day triggering is **not** available (no RTC sync on the dash).

## Verifying night mode while building

There's no `?night=1` on the screenshot endpoint. Drive it the way the car
would — inject the trigger signal, then screenshot:

```
POST /api/signal/inject {"signal":"HEADLIGHTS","value":1}
GET  /api/screenshot?full=1
POST /api/signal/inject {"signal":"HEADLIGHTS","value":0}
```

Check both states. A night palette that looks fine in isolation is often
unreadably dim next to the day one, and vice versa.
