# Widget Catalog — every type, every setting

> AUTO-GENERATED from `schema/widgets.schema.json` (cross-checked against `main/widgets/*.c`) by `scripts/gen_catalog.py`. Do not hand-edit. Every layout JSON must carry `"schema_version": 18`.

Each widget is `{ "type", "id", "x", "y", "w", "h", "config": { ...fields below... } }`. Put a field in `config` only when it differs from the default (keeps the layout under the 32 KB budget). `*_color` fields are **RGB565 integers** (see 03-colors). Bind data with `config.signal_name` (see 04-channels).

**Flags column**

- **N** — night-overridable: put it in `config.night` to change it after dark (see 06-rules-and-night). A `` `key` `` after the N is the name to use inside `night` when it differs from the field name.
- **R** — *rule-only*: NOT a `config` field. It exists only as a rule override target (see 06-rules-and-night).
- **X** — **editor-only: the device's `from_json` never reads it.** Setting it in device JSON does nothing. The widget's section says so up front; 05-quirks has what to write instead.

**18 widget types.**

| Type | Name | Default size | Editor resize limits | Singleton |
|---|---|---|---|---|
| `rpm_bar` | RPM Bar | 800x55 | w 300..800, h 30..80 | yes |
| `panel` | Panel | 155x92 | w 80..250, h 40..130 |  |
| `bar` | Bar Graph | 300x30 | w 120..450, h 15..50 |  |
| `indicator` | Turn Indicator | 40x40 | w 30..80, h 30..80 |  |
| `warning` | Alert Light | 20x20 | w 18..60, h 18..60 |  |
| `text` | Text / Value | 100x30 | w 40..screen_origin_x, h 20..100 |  |
| `meter` | Meter | 300x300 | w 80..800, h 80..800 |  |
| `image` | Image | 100x100 | w 10..800, h 10..480 |  |
| `shape_panel` | Shape Panel | 200x100 | w 10..800, h 10..480 |  |
| `line` | Line | 200x4 | w 10..800, h 1..480 |  |
| `banner` | Alert Banner | 800x80 | w 100..800, h 24..160 |  |
| `arc` | Arc Shape | 200x200 | w 30..800, h 30..800 |  |
| `toggle` | Toggle Switch | 80x40 | w 40..200, h 20..80 |  |
| `button` | Button | 100x40 | w 40..300, h 20..100 |  |
| `shift_light` | Shift Light | 400x30 | w 100..800, h 15..60 |  |
| `pathbar` | Path Bar | 400x260 | w 20..800, h 20..480 |  |
| `anim` | Animation | 200x200 | w 20..800, h 20..480 |  |
| `track_map` | Track Map | 320x240 | w 60..800, h 60..480 |  |


## `rpm_bar` — RPM Bar

- default size 800×55 · default pos (0,-213) · editor resize limits: w 300..800, h 30..80 · **singleton** (only one allowed)

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `rpm_max` | number | 7000 |  |  | The highest RPM the bar shows. It fills from 0 up to here. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bar_color` | color | #00FF00 |  | **N** | Colour of the bar below the redline. |
| `grad_stops` | gradient_stops | [] |  |  | Blend the bar through multiple colours from empty to full - e.g. green through yellow to red. The redline and limiter colours still override it. |
| `fill_dir` | select | 0 | options: 0=Left to Right, 1=Right to Left, 2=Center Out, 3=Edges In |  | Which way the bar fills (left, right, up or down). |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bar_bg_color` | color | #F0F0F0 |  | **N** | Colour of the empty background. |

**Ticks**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_ticks` | checkbox | True |  |  | Show marks along the bar. |
| `label_every` | stepper | 1 | 1..10; only when `show_ticks` |  | How often to print a number. 1 labels every thousand; 2 gives 0 2 4 6 8. The tick marks are unaffected — only the numbers thin out. Useful for the Center Out and Edges In fills, which lay the scale twice in the same width. |
| `tick_side` | select | 2 | options: 0=Top, 1=Bottom, 2=Both; only when `show_ticks` |  | Which side of the bar the marks sit on. |
| `tick_width` | stepper | 3 | 1..12; only when `show_ticks` |  | How thick the marks are. |
| `tick_length` | stepper | 12 | 1..40; only when `show_ticks` |  | How long the marks are. |
| `tick_color` | color | #000000 | only when `show_ticks` | **N** | Colour of the marks. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_rpm_value` | checkbox | False |  |  | Show the RPM as a number on the bar. |
| `rpm_value_font` | font |  | only when `show_rpm_value` |  | Text style and size of the RPM number. |
| `rpm_value_color` | color | #E8E8E8 | only when `show_rpm_value` | **N** | Colour of the RPM number. |
| `rpm_value_x_offset` | stepper | 0 | -127..127; only when `show_rpm_value` |  | Nudge the number left or right. 0 is centred on the bar. |
| `rpm_value_y_offset` | stepper | 0 | -127..127; only when `show_rpm_value` |  | Nudge the number up or down. 0 sits it just under the bar. |

**Redline**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `redline` | number | 6000 |  |  | The RPM where the red warning zone starts - the bar changes colour past this point. |
| `limiter_value` | number | 7000 |  |  | The RPM where your rev limiter kicks in - the bar reacts here. |
| `limiter_color` | color | #FF0000 |  | **N** | Colour shown when you hit the limiter. |
| `limiter_effect` | select | 0 | options: 0=None, 1=Bar Flash, 2=Bar Solid |  | What happens at the limiter - flash, or just change colour. |
| `flash_speed` | stepper | 200 | 50..1000 step 50; only when `limiter_effect=1` |  | How fast it flashes at the limiter (smaller = faster), in milliseconds. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `smoothing_ms` | slider | 20 | 0..300 step 10 |  | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |


## `panel` — Panel

- default size 155×92 · editor resize limits: w 80..250, h 40..130

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `label` | text |  |  |  | The title shown above the number (e.g. COOLANT). |
| `decimals` | number | 0 |  |  | How many digits after the decimal point to show. |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bg_color` | color | #000000 |  | **N** | Background colour of the panel. |
| `bg_opa` | stepper | 255 | 0..255 |  | How solid the background is (0 = see-through, 255 = solid). |
| `border_color` | color | #2E2F2E |  | **N** | Colour of the outline. |
| `border_width` | stepper | 3 | 0..20 |  | Thickness of the outline (0 = none). |
| `border_radius` | stepper | 7 | 0..100 |  | How rounded the corners are. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `label_font` | font |  |  |  | Text style and size of the title. |
| `label_color` | color | #FFFFFF |  | **N** | Colour of the title. |
| `value_font` | font |  |  |  | Text style and size of the number. |
| `value_color` | color | #FFFFFF |  | **N** | Colour of the number. |
| `text_align` | select | 1 | options: 0=Left, 1=Center, 2=Right |  | Line the text up to the left, centre or right. |
| `label_y_offset` | stepper | -28 | -100..100 |  | Nudge the title up or down. |
| `label_x_offset` | stepper | 0 | -127..127 |  | Nudge the title left or right. 0 keeps it centred in the panel. |
| `value_y_offset` | stepper | 9 | -100..100 |  | Nudge the number up or down. |
| `value_x_offset` | stepper | 0 | -127..127 |  | Nudge the reading left or right. 0 keeps it centred in the panel. |
| `custom_text` | text |  |  |  | Your own text next to the reading, instead of the unit. Leave empty to use Show Unit Suffix. |
| `custom_text_x_offset` | stepper | 41 | -100..100; only when `custom_text` |  | Nudge your custom text left or right. |
| `custom_text_y_offset` | stepper | 32 | -100..100; only when `custom_text` |  | Nudge your custom text up or down. |

**Unit**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_unit` | checkbox | False |  |  | Add the unit after the number, like °C or PSI. |
| `unit_size` | select | 2 | options: 0=Small, 1=Medium, 2=Full; only when `show_unit` |  | How big the unit is next to the number. |

**Peak**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_peak` | select | 0 | options: 0=Off, 1=Max, 2=Min, 3=Min/Max |  | Also show the highest (or lowest) reading reached, held on screen. |
| `peak_font` | font |  | only when `show_peak` |  | Text style and size of the held reading. |
| `peak_x_offset` | stepper | 0 | -100..100; only when `show_peak` |  | Nudge the held reading left or right. |
| `peak_y_offset` | stepper | 31 | -100..100; only when `show_peak` |  | Nudge the held reading up or down. |

**Alerts**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `warning_low_enabled` | checkbox | False |  |  | Warn when the reading drops too low. |
| `warning_low_threshold` | number | 0 | -99999..99999; only when `warning_low_enabled` |  | Warn once the reading falls to or below this. |
| `warning_low_color` | color | #0000FF | only when `warning_low_enabled` |  | The alert colour used when it's too low. |
| `warning_low_apply_label` | checkbox | True | only when `warning_low_enabled` |  | Turn the title the alert colour when it's too low. |
| `warning_low_apply_value` | checkbox | True | only when `warning_low_enabled` |  | Turn the number the alert colour when it's too low. |
| `warning_low_apply_panel` | checkbox | False | only when `warning_low_enabled` |  | Turn the panel's edge/background the alert colour when it's too low. |
| `warning_high_enabled` | checkbox | False |  |  | Warn when the reading climbs too high. |
| `warning_high_threshold` | number | 0 | -99999..99999; only when `warning_high_enabled` |  | Warn once the reading rises to or above this. |
| `warning_high_color` | color | #FF0000 | only when `warning_high_enabled` |  | The alert colour used when it's too high. |
| `warning_high_apply_label` | checkbox | True | only when `warning_high_enabled` |  | Turn the title the alert colour when it's too high. |
| `warning_high_apply_value` | checkbox | True | only when `warning_high_enabled` |  | Turn the number the alert colour when it's too high. |
| `warning_high_apply_panel` | checkbox | False | only when `warning_high_enabled` |  | Turn the panel's edge/background the alert colour when it's too high. |


## `bar` — Bar Graph

- default size 300×30 · editor resize limits: w 120..450, h 15..50

> ⚠ **Editor-only here (flagged X):** `bar_alerts_enabled` — not a `config` key; the device ignores these. See 05-quirks.

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bar_min` | number | 0 |  |  | The reading at the empty end of the bar. |
| `bar_max` | number | 100 |  |  | The reading at the full end of the bar. |
| `decimals` | number | 0 |  |  | How many digits after the decimal point to show. |
| `anchor_enabled` | checkbox | False |  |  | Pin a chosen reading to a fixed spot on the bar instead of filling from the end. e.g. put your normal operating temperature in the middle, so anything hotter or colder is obvious at a glance. |
| `anchor_value` | number | 50 | only when `anchor_enabled` |  | The reading you want pinned in place - e.g. 90 for your normal coolant temp. |
| `anchor_position` | slider | 50 | 0..100; only when `anchor_enabled` |  | Where that reading sits along the bar (50 = middle, 0 = start, 100 = end). |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `invert_bar_value` | checkbox | False |  |  | Flip it so a high reading empties the bar instead of filling it. |
| `center_fill` | checkbox | False |  |  | Fill out from the middle of the bar in both directions. |
| `fill_dir` | select | 0 | options: 0=Left to Right, 1=Right to Left, 2=Center Out, 3=Edges In |  | Which way the bar fills (left, right, up or down). |
| `bar_in_range_color` | color | #00FF00 |  | **N** | Colour of the bar. |
| `grad_stops` | gradient_stops | [] |  |  | Blend the fill through multiple colours from empty to full. The low/high alert colours still override it. |
| `bar_image_full` | image_picker |  |  | **N** | Use a picture for the filled part instead of a plain colour. |
| `fill_edge_width` | stepper | 0 | 0..20 |  | How wide the bright tip at the current reading is (0 = off). |
| `fill_edge_color` | color | #FFFFFF | only when `fill_edge_width` |  | Colour of the bright tip that sits at the current reading. |
| `indicator_radius` | stepper | 5 | 0..50 |  | How rounded the filled part's corners are. |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bar_bg_color` | color | #2E2F2E |  | **N** | Colour of the empty part behind the fill. |
| `bar_image` | image_picker |  |  | **N** | Use a picture for the empty background instead of a plain colour. |
| `bar_radius` | stepper | 5 | 0..50 |  | How rounded the whole bar's corners are. |
| `bar_border_width` | stepper | 2 | 0..20 |  | Thickness of the outline (0 = none). |
| `bar_border_color` | color | #2E2F2E |  | **N** | Colour of the outline. |

**Ticks**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_ticks` | checkbox | False |  |  | Show marks along the bar, like the lines on a fuel gauge. |
| `tick_side` | select | 2 | options: 0=Top, 1=Bottom, 2=Both; only when `show_ticks` |  | Which side of the bar the marks sit on. |
| `tick_count` | stepper | 5 | 2..30; only when `show_ticks` |  | How many marks to show along the bar. |
| `tick_width` | stepper | 2 | 1..10; only when `show_ticks` |  | How thick the marks are. |
| `tick_length` | stepper | 6 | 1..40; only when `show_ticks` |  | How long the marks are. |
| `tick_color` | color | #E8E8E8 | only when `show_ticks` | **N** | Colour of the marks. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_bar_label` | checkbox | True |  |  | Show the text label next to the bar. Turn off for a clean value-only bar. |
| `label` | text |  | only when `show_bar_label` |  | Text shown with the bar. |
| `show_bar_value` | checkbox | False |  |  | Show the number on the bar. |
| `label_font` | font |  |  |  | Text style and size of the label. |
| `label_color` | color | #FFFFFF |  | **N** | Colour of the label. |
| `value_font` | font |  |  |  | Text style and size of the number. |
| `value_color` | color | #FFFFFF |  | **N** | Colour of the number. |

**Alerts**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bar_alerts_enabled` | checkbox | False |  | **X** | Change the bar colour when the reading goes too low or too high (set the points below). |
| `bar_low` | number | 0 | -99999..99999; only when `bar_alerts_enabled` |  | At or below this reading, the bar turns the Low colour. |
| `bar_low_color` | color | #0000FF | only when `bar_alerts_enabled` | **N** | Bar colour when the reading is low. |
| `bar_high` | number | 100 | -99999..99999; only when `bar_alerts_enabled` |  | At or above this reading, the bar turns the High colour. |
| `bar_high_color` | color | #FF0000 | only when `bar_alerts_enabled` | **N** | Bar colour when the reading is high. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `smoothing_ms` | slider | 20 | 0..300 step 10 |  | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |


## `indicator` — Turn Indicator

- default size 40×40 · editor resize limits: w 30..80, h 30..80

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `slot` | select | 0 | options: 0=Left, 1=Right |  | Which side of the screen this light sits on. |
| `input_source` | select | 0 | options: 0=Wire, 1=CAN |  | What turns the light on - a sensor reading or a built-in source. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `color_on` | color | #00C853 |  | **N** | Colour when the light is on. |
| `opa_on` | stepper | 255 | 0..255 |  | How bright it is when on (0-255). |
| `color_off` | color | #06300A |  | **N** | Colour when the light is off. |
| `opa_off` | stepper | 255 | 0..255 |  | How bright it is when off (0-255). |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `animation` | checkbox | True |  |  | Fade smoothly between on and off. |
| `is_momentary` | checkbox | True |  |  | Only stay on while the signal is active, instead of latching on. |


## `warning` — Alert Light

- default size 20×20 · editor resize limits: w 18..60, h 18..60

> **Rule-only here (flagged R):** `lamp_on` — not a `config` key either, but the device DOES honour it as a rule override target. See 06-rules-and-night.

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `label` | textarea |  |  |  | Text shown on the tile. Press Enter for a new line. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `active_color` | color | #FF0000 |  | **N** | Tile colour when the warning is on. |
| `active_opa` | stepper | 255 | 0..255 |  | How solid the tile is when on (0-255). |
| `inactive_color` | color | #292C29 |  | **N** | Tile colour when off. |
| `inactive_opa` | stepper | 180 | 0..255 |  | How solid the tile is when off (0-255). |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `radius` | stepper | 100 | 0..200 |  | How rounded the tile's corners are. |
| `border_width` | stepper | 0 | 0..20 |  | Thickness of the outline (0 = none). |
| `border_color_style` | color | #000000 |  | **N**(`border_color`) | Colour of the outline. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_label` | checkbox | True |  |  | Show the text on the tile. |
| `label_color` | color | #FFFFFF | only when `show_label` | **N** | Text colour. |
| `label_font` | font |  | only when `show_label` |  | Text style and size. |
| `label_y_offset` | stepper | 11 | -100..100; only when `show_label` |  | Nudge the text up or down. |
| `label_text_align` | select | 1 | options: 0=Left, 1=Center, 2=Right; only when `show_label` |  | Line the text up left, centre or right. |

**Image**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `image_name` | image_picker |  |  | **N** | Icon shown on the tile. |
| `image_scale` | stepper | 100 | 10..200 |  | Resize the icon, as a percentage. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `flash_mode` | select | 0 | options: 0=Solid, 1=Flashing |  | Stay lit while the signal is on, or flash on and off. |
| `flash_speed` | stepper | 200 | 50..1000 step 50; only when `flash_mode=1` |  | How fast it flashes (smaller = faster), in milliseconds. |
| `lamp_on` | checkbox | True |  | **R** | Force the light on or off. Normally the light comes on whenever the channel reads anything other than zero — use this in a rule to keep it dark until a reading matters, e.g. off below 90. |
| `is_momentary` | checkbox | True |  |  | Only stay on while the signal is active, instead of latching on. |
| `invert_toggle` | checkbox | False |  |  | Flip it - the warning shows when the signal is OFF. |


## `text` — Text / Value

- default size 100×30 · editor resize limits: w 40..screen_origin_x, h 20..100

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `static_text` | text |  |  |  | Type fixed text to show. Leave blank to show the live reading instead. |
| `decimals` | number | 0 |  |  | How many digits after the decimal point when showing a reading. |

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `rotation` | stepper | 0 | 0..359 |  | Turn the text on an angle, in degrees. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `font` | font |  |  |  | Text style and size. |
| `text_color` | color | #FFFFFF |  | **N** | Text colour. |


## `meter` — Meter

- default size 300×300 · editor resize limits: w 80..800, h 80..800

> ⚠ **Editor-only here (flagged X):** `start_angle_user`, `sweep_degrees`, `major_tick_step`, `minor_tick_step` — not a `config` key; the device ignores these. See 05-quirks.

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `min` | number | 0 |  |  | The reading at the start of the dial. |
| `max` | number | 100 |  |  | The reading at the end of the dial. |
| `reverse` | checkbox | False |  |  | Run the dial backwards, so the high reading sits at the start. |
| `anchor_enabled` | checkbox | False |  |  | Pin a chosen reading to a fixed spot on the dial instead of sweeping from the start. e.g. put your normal operating temperature at the middle, so the needle rests centre when warmed up and swings high or low if it drifts. |
| `anchor_value` | number | 50 | only when `anchor_enabled` |  | The reading you want pinned in place - e.g. 90 for your normal coolant temp. |
| `anchor_position` | slider | 50 | 0..100; only when `anchor_enabled` |  | Where that reading sits along the dial (50 = middle, 0 = start, 100 = end). |

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `start_angle_user` | stepper | 225 | 0..359 step 5 | **X** | Where the dial begins. 0° is 12 o'clock and the angle runs clockwise, so the default 225° starts at the lower left. |
| `sweep_degrees` | stepper | 270 | 30..360 step 5 | **X** | How far round it goes from the start, clockwise. 270° from a 225° start ends at 135°, the lower right — the usual car gauge. |

**Needle**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_needle` | checkbox | True |  |  | Show the pointer needle. |
| `needle_width` | stepper | 4 | 1..20; only when `show_needle` |  | How thick the needle is. |
| `needle_color` | color | #FFFFFF | only when `show_needle` | **N** | Colour of the needle. |
| `needle_r_mod` | stepper | -10 | -100..100; only when `show_needle` |  | Shorten the needle from its outer end. |
| `needle_rear_length` | stepper | 0 | 0..100; only when `show_needle` |  | How far the needle sticks out behind its centre, for a counterweight look. |
| `needle_inner_radius` | stepper | 0 | 0..400; only when `show_needle` |  | Start the needle away from the centre, leaving a gap in the middle. |
| `needle_tip_style` | select | 0 | options: 0=Flat, 1=Rounded, 2=Lance, 3=Dagger, 4=Spade, 5=Diamond; only when `show_needle` |  | The shape of the needle's tip. |
| `needle_tip_base_w` | stepper-auto | 0 | 0..30; only when `show_needle` |  | How wide the needle is at its base (leave at auto unless you want to set it). |
| `needle_tip_point_w` | stepper-auto | 0 | 0..20; only when `show_needle` |  | How wide the needle is at its tip. |
| `needle_tip_taper` | stepper-auto | 0 | 0..100; only when `show_needle` |  | How sharply the needle narrows toward the tip. |
| `needle_image_name` | image_picker |  | only when `show_needle` | **N** | Use a picture as the needle instead of a drawn one. |
| `needle_pivot_x` | stepper | 0 | -400..400; only when `needle_image_name` |  | Move the needle's centre point left or right. |
| `needle_pivot_y` | stepper | 0 | -400..400; only when `needle_image_name` |  | Move the needle's centre point up or down. |
| `needle_angle_offset` | stepper | 0 | -180..180; only when `needle_image_name` |  | Rotate where the needle rests, in degrees. |
| `show_needle_ball` | checkbox | True |  |  | Show a round hub in the middle of the needle. |
| `needle_ball_size` | stepper | 10 | 0..40; only when `show_needle_ball` |  | How big the hub is. |
| `needle_ball_color` | color | #FFFFFF | only when `show_needle_ball` | **N** | Colour of the hub. |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `meter_bg_color` | color | #3D3D3D |  | **N** | Background colour of the gauge face. |
| `meter_bg_opa` | stepper | 255 | 0..255 |  | How solid the gauge face is (0 = see-through, 255 = solid). |
| `bg_image_name` | image_picker |  |  | **N** | Use a picture as the gauge face. |
| `border_color` | color | #000000 |  | **N** | Colour of the outline around the gauge. |
| `border_width` | stepper | 0 | 0..20 |  | Thickness of the outline (0 = none). |
| `border_opa` | stepper | 255 | 0..255 |  | How solid the outline is (0-255). |
| `scale_padding` | stepper | 0 | 0..100 |  | Move the marks inward from the edge of the gauge. |

**Shadow**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `shadow_enabled` | checkbox | False |  |  | Drop a soft shadow under the needle to lift it off the dial face. |
| `shadow_dynamic` | checkbox | True | only when `shadow_enabled` |  | Swing the shadow as the needle moves, as if lit from above. Turn off to keep it at a fixed offset. |
| `shadow_offset_x` | stepper | 3 | -32..32; only when `shadow_enabled` |  | How far the shadow sits to the side of the needle. |
| `shadow_offset_y` | stepper | 4 | -32..32; only when `shadow_enabled` |  | How far the shadow sits below the needle. |
| `shadow_opa` | stepper | 120 | 0..255; only when `shadow_enabled` |  | How dark the shadow is (255 = solid). |
| `shadow_width_extra` | stepper | 0 | 0..32; only when `shadow_enabled` |  | Spread the shadow into a soft penumbra. 0 keeps it the same shape as the needle. |
| `shadow_color` | color | #000000 | only when `shadow_enabled` |  | Colour of the needle's shadow. |

**Ticks**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_ticks` | checkbox | True |  |  | Show the marks around the dial, like the lines on a speedometer. |
| `major_tick_step` | number | 50 |  | **X** | How far apart the biggest marks are, in reading value. On a 0-8000 rev counter, 1000 puts a big mark every 1000 RPM. |
| `mid_tick_step` | number | 0 | only when `show_ticks` |  | How far apart the medium marks are. 0 turns them off. |
| `minor_tick_step` | number | 10 |  | **X** | How far apart the smallest marks are. 0 turns them off. |
| `major_tick_width` | stepper | 4 | 1..20; only when `show_ticks` |  | How thick the biggest marks are. |
| `mid_tick_width` | stepper | 2 | 0..20; only when `show_ticks` |  | How thick the medium marks are (0 = off). |
| `minor_tick_width` | stepper | 2 | 1..20; only when `show_ticks` |  | How thick the smallest marks are. |
| `major_tick_length` | stepper | 15 | 1..50; only when `show_ticks` |  | How long the biggest marks are. |
| `mid_tick_length` | stepper | 13 | 0..100; only when `show_ticks` |  | How long the medium marks are (0 = off). |
| `minor_tick_length` | stepper | 10 | 1..50; only when `show_ticks` |  | How long the smallest marks are. |
| `major_tick_color` | color | #FFFFFF | only when `show_ticks` | **N** | Colour of the biggest marks. |
| `mid_tick_color` | color | #BDBDBD | only when `show_ticks` |  | Colour of the medium marks. |
| `minor_tick_color` | color | #9E9E9E | only when `show_ticks` | **N** | Colour of the smallest marks. |
| `major_tick_image_name` | image_picker |  | only when `show_ticks` |  | Use a picture for each major (numbered) tick instead of a drawn line. Design it pointing up (tip toward the rim); it's rotated to each tick. |
| `mid_tick_image_name` | image_picker |  | only when `show_ticks` |  | Use a picture for each medium tick instead of a drawn line. Design it pointing up (tip toward the rim). |
| `minor_tick_image_name` | image_picker |  | only when `show_ticks` |  | Use a picture for each minor tick instead of a drawn line. Design it pointing up (tip toward the rim). |
| `tick_image_scale` | stepper | 100 | 10..400; only when `show_ticks` |  | Scale the tick images up or down (100 = original size). Applies to all three tick images. |
| `tick_outline_strength` | stepper | 0 | 0..255; only when `show_ticks` |  | Draw an outline/glow behind the drawn ticks so they pop (0 = off, 255 = solid). No image needed. |
| `tick_outline_color` | color | #000000 | only when `show_ticks` |  | Colour of the tick outline/glow (usually black for a crisp edge). |
| `tick_outline_fade` | stepper | 0 | 0..20; only when `show_ticks` |  | Soften the outline into a glow (0 = hard edge, higher = wider soft glow). |

**Numbers**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_tick_labels` | checkbox | True |  |  | Show numbers next to the biggest marks. |
| `tick_label_font` | font |  | only when `show_tick_labels` |  | Text style and size of those numbers. |
| `tick_label_color` | color | #FFFFFF | only when `show_tick_labels` | **N** | Colour of those numbers. |
| `label_gap` | stepper | 10 | -150..150; only when `show_tick_labels` |  | How far the numbers sit from the marks. |
| `tick_label_divisor` | number | 1 | 1..100000; only when `show_tick_labels` |  | Shrink the numbers by dividing them. Set 1000 and a 7000 mark just shows '7' - common on rev counters. |

**Redline**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `redline_enabled` | checkbox | False |  |  | Show a warning zone, like a rev counter's red zone. |
| `redline_threshold` | number | 80 | only when `redline_enabled` |  | The reading where the warning zone starts. |
| `redline_color` | color | #FF0000 | only when `redline_enabled` |  | Colour of the warning zone. |
| `redline_show_arc` | checkbox | True | only when `redline_enabled` |  | Draw a coloured band along the warning zone. |
| `redline_arc_width` | stepper | 6 | 1..30; only when `redline_enabled` |  | How thick that band is. |
| `redline_arc_r_mod` | stepper | 0 | -50..50; only when `redline_enabled` |  | Move the warning band in or out from the dial. |
| `redline_recolor_ticks` | checkbox | True | only when `redline_enabled` |  | Turn the marks inside the warning zone that colour too. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `smoothing_ms` | slider | 20 | 0..300 step 10 |  | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |


## `image` — Image

- default size 100×100 · editor resize limits: w 10..800, h 10..480

**Image**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `image_name` | image_picker |  |  | **N** | The picture to show. |
| `image_scale` | stepper | 100 | 10..200 |  | Resize the picture, as a percentage. |
| `opacity` | stepper | 255 | 0..255 |  | How see-through the picture is (0 = invisible, 255 = solid). |
| `recolor` | color | #000000 |  | **N** | Tint the whole picture this colour. |
| `recolor_opa` | stepper | 0 | 0..255 |  | How strong the tint is (0 = none, 255 = full). Tinting a light icon white can make it disappear. |


## `shape_panel` — Shape Panel

- default size 200×100 · editor resize limits: w 10..800, h 10..480

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `shape_type` | select | rectangle | options: rectangle=Rectangle, circle=Circle, triangle=Triangle, diamond=Diamond, arrow_right=Arrow ▶, arrow_left=Arrow ◀, chevron_right=Chevron ❯, chevron_left=Chevron ❮ |  | The shape to draw. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bg_color` | color | #1A1A1A |  | **N** | Fill colour. |
| `bg_opa` | stepper | 255 | 0..255 |  | How see-through the fill is (0 = invisible, 255 = solid). |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `border_color` | color | #2E2F2E |  | **N** | Colour of the outline. |
| `border_width` | stepper | 0 | 0..20 |  | Thickness of the outline (0 = none). |
| `border_radius` | stepper | 10 | 0..200 |  | How rounded the corners are (for rectangles). |

**Shadow**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `shadow_width` | stepper | 0 | 0..50 |  | How far the drop shadow spreads (0 = no shadow). |
| `shadow_color` | color | #000000 |  | **N** | Colour of the shadow. |
| `shadow_opa` | stepper | 128 | 0..255 |  | How dark the shadow is (0-255). |
| `shadow_ofs_x` | stepper | 0 | -100..100 |  | Move the shadow left or right. |
| `shadow_ofs_y` | stepper | 0 | -100..100 |  | Move the shadow up or down. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bake_into_gauge` | checkbox | False |  |  | Lock this shape into the background so it draws faster. It won't change while driving - use for decoration only. |


## `line` — Line

- default size 200×4 · editor resize limits: w 10..800, h 1..480

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `orientation` | select | horizontal | options: horizontal=Horizontal, vertical=Vertical, diagonal_fwd=Diagonal /, diagonal_bwd=Diagonal \ |  | Which way the line runs (across, up-down, or diagonal). |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `line_color` | color | #FFFFFF |  | **N** | Colour of the line. |
| `line_width` | stepper | 4 | 1..30 |  | How thick the line is. |
| `line_opa` | stepper | 255 | 0..255 |  | How see-through the line is (0-255). |
| `rounded` | checkbox | False |  |  | Round the line's ends. |
| `dash_gap` | stepper | 0 | 0..40 |  | Gap between dashes (0 = a solid line). |
| `curvature` | slider | 0 | -200..200 |  | Bend the line into a curve. 0 is straight; negative and positive bow it opposite ways. |


## `banner` — Alert Banner

- default size 800×80 · editor resize limits: w 100..800, h 24..160

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `op` | select | 0 | options: 0=>, 1=<, 2=>=, 3=<=, 4===, 5=!=, 6=range, 7=always (test) |  | How the reading is checked to pop up the message (over, under, equal to, or within a range). |
| `threshold` | number | 0 |  |  | The value the reading is checked against. |
| `range_min` | number | 0 |  |  | The bottom of the range (when using 'within a range'). |
| `range_max` | number | 0 |  |  | The top of the range (when using 'within a range'). |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bg_color` | color | #FF0000 |  | **N** | Background colour. |
| `bg_opa` | stepper | 128 | 0..255 |  | How solid the background is (0-255). |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `border_width` | stepper | 0 | 0..20 |  | Thickness of the outline (0 = none). |
| `border_color` | color | #000000 |  | **N** | Colour of the outline. |
| `radius` | stepper | 0 | 0..100 |  | How rounded the corners are. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `text` | text | WARNING |  |  | The message to show when it pops up. |
| `text_align` | select | 1 | options: 0=Left, 1=Center, 2=Right |  | Line the message up left, centre or right. |
| `text_color` | color | #000000 |  | **N** | Colour of the message. |
| `font` | font |  |  |  | Text style and size. |


## `arc` — Arc Shape

- default size 200×200 · editor resize limits: w 30..800, h 30..800

> ⚠ **Editor-only here (flagged X):** `start_angle_user`, `sweep_degrees`, `arc_low`, `arc_high` — not a `config` key; the device ignores these. See 05-quirks.

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `signal_min` | number | 0 |  |  | The reading that shows the gauge empty (the start). |
| `signal_max` | number | 100 |  |  | The reading that shows the gauge full (the end). |
| `tick_min` | number | 0 |  |  | The first number the marks count from. Usually the same as the empty value. |
| `tick_max` | number | 0 |  |  | The last number the marks count up to. Usually the same as the full value. |
| `anchor_enabled` | checkbox | False |  |  | Pin a chosen reading to a fixed spot on the gauge instead of filling from the empty end. e.g. put your normal operating temperature in the middle, so the gauge sits centre when warmed up and clearly runs high or low if it drifts. |
| `anchor_value` | number | 50 | only when `anchor_enabled` |  | The reading you want pinned in place - e.g. 90 for your normal coolant temp. |
| `anchor_position` | slider | 50 | 0..100; only when `anchor_enabled` |  | Where that reading sits along the gauge (50 = middle, 0 = start, 100 = end). |
| `reverse` | checkbox | False |  |  | Run the gauge backwards, so the full end becomes the empty end. |

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `start_angle_user` | stepper | 225 | 0..359 step 5 | **X** | Where the gauge begins — the empty end. 0° is 12 o'clock and the angle runs clockwise, so the default 225° starts at the lower left. |
| `sweep_degrees` | stepper | 270 | 30..360 step 5 | **X** | How far round it goes from the start, clockwise. 270° from a 225° start ends at 135°, the lower right — the usual car gauge. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `arc_width` | stepper | 10 | 1..50 |  | How thick the coloured (filled) part of the gauge is. |
| `arc_offset` | stepper | 0 | 0..120 |  | Moves the ring in or out from the centre. Use it to stack one gauge inside another. |
| `arc_color` | color | #00FF00 |  | **N** | Colour of the filled part. |
| `grad_stops` | gradient_stops | [] |  |  | Blend the arc through multiple colours as the reading climbs. The redline and alert colours still override it. |
| `bg_arc_color` | color | #333333 |  | **N** | Colour of the empty part behind the fill. |
| `bg_arc_width` | stepper | 10 | 1..50 |  | How thick the empty background ring is. Make it wider or narrower than the fill for a raised or recessed look. |
| `rounded_ends` | checkbox | False |  |  | Give the gauge rounded ends instead of flat-cut ones. |
| `fade_fill` | checkbox | False |  |  | Fade the colour from dim to bright along the filled part, for a glowing look. |
| `lead_edge_enabled` | checkbox | True |  |  | Add a bright tip that sits at the current reading, like the glowing end of a digital bar. |
| `lead_edge_color` | color | #E6FAFF | only when `lead_edge_enabled` |  | Colour of that bright tip. |
| `lead_edge_width` | stepper | 6 | 0..40; only when `lead_edge_enabled` |  | How wide the bright tip is. |
| `arc_image` | image_picker |  |  | **N** | Use a picture for the empty background instead of a plain colour. |
| `arc_image_full` | image_picker |  |  | **N** | Use a picture for the filled part instead of a plain colour. It's uncovered bit by bit as the reading rises. |
| `arc_image_opa` | stepper | 255 | 0..255 |  | How solid the track image is (255 = fully visible, 0 = invisible). |
| `arc_image_recolor` | color | #000000 |  |  | Colour blended into the track image (turn up the tint strength to use it). |
| `arc_image_recolor_opa` | stepper | 0 | 0..255 |  | How strongly the tint colour is mixed into the track image (0 = off). |
| `arc_image_blend` | select | 0 | options: 0=Normal, 1=Additive, 2=Subtractive, 3=Multiply |  | How the track image is layered over what's behind it (Additive glows, Multiply darkens). |
| `arc_image_full_opa` | stepper | 255 | 0..255 |  | How solid the fill image is (255 = fully visible, 0 = invisible). |
| `arc_image_full_recolor` | color | #000000 |  |  | Colour blended into the fill image (turn up the tint strength to use it). |
| `arc_image_full_recolor_opa` | stepper | 0 | 0..255 |  | How strongly the tint colour is mixed into the fill image (0 = off). |
| `arc_image_full_blend` | select | 0 | options: 0=Normal, 1=Additive, 2=Subtractive, 3=Multiply |  | How the fill image is layered over what's behind it (Additive glows, Multiply darkens). |
| `arc_image_radial` | checkbox | False |  |  | Uncover the fill image AROUND the ring (for round gauges) instead of sliding it in left-to-right. Use with a full ring fill image. |

**Ticks**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_ticks` | checkbox | False |  |  | Show the marks around the gauge, like the lines on a speedometer. |
| `major_tick_step` | number | 50 | only when `show_ticks` |  | How far apart the biggest marks are, in reading value. On a 0-8000 rev counter, 1000 puts a big mark every 1000 RPM. |
| `mid_tick_step` | number | 0 | only when `show_ticks` |  | How far apart the medium marks are. 0 turns them off. |
| `minor_tick_step` | number | 10 | only when `show_ticks` |  | How far apart the smallest marks are. 0 turns them off. |
| `major_tick_width` | stepper | 4 | 1..20; only when `show_ticks` |  | How thick the biggest marks are. |
| `mid_tick_width` | stepper | 2 | 0..20; only when `show_ticks` |  | How thick the medium marks are. |
| `minor_tick_width` | stepper | 2 | 1..20; only when `show_ticks` |  | How thick the smallest marks are. |
| `major_tick_length` | stepper | 15 | 0..50; only when `show_ticks` |  | How long the biggest marks are. |
| `mid_tick_length` | stepper | 13 | 0..100; only when `show_ticks` |  | How long the medium marks are. |
| `minor_tick_length` | stepper | 10 | 0..50; only when `show_ticks` |  | How long the smallest marks are. |
| `major_tick_color` | color | #FFFFFF | only when `show_ticks` | **N** | Colour of the biggest marks. |
| `mid_tick_color` | color | #BDBDBD | only when `show_ticks` |  | Colour of the medium marks. |
| `minor_tick_color` | color | #9E9E9E | only when `show_ticks` | **N** | Colour of the smallest marks. |
| `major_tick_image_name` | image_picker |  | only when `show_ticks` |  | Use a picture for each major tick instead of a drawn line. Design it pointing up (tip toward the rim); it's rotated to each tick. |
| `mid_tick_image_name` | image_picker |  | only when `show_ticks` |  | Use a picture for each medium tick instead of a drawn line. Design it pointing up (tip toward the rim). |
| `minor_tick_image_name` | image_picker |  | only when `show_ticks` |  | Use a picture for each minor tick instead of a drawn line. Design it pointing up (tip toward the rim). |
| `major_tick_image_scale` | stepper | 100 | 10..400; only when `show_ticks` |  | Scale the major tick image (100 = original size). |
| `mid_tick_image_scale` | stepper | 100 | 10..400; only when `show_ticks` |  | Scale the medium tick image (100 = original size). |
| `minor_tick_image_scale` | stepper | 100 | 10..400; only when `show_ticks` |  | Scale the minor tick image (100 = original size). |
| `major_tick_image_opa` | stepper | 255 | 0..255; only when `show_ticks` |  | Fade the major tick image (255 = solid, 0 = invisible). |
| `mid_tick_image_opa` | stepper | 255 | 0..255; only when `show_ticks` |  | Fade the medium tick image (255 = solid, 0 = invisible). |
| `minor_tick_image_opa` | stepper | 255 | 0..255; only when `show_ticks` |  | Fade the minor tick image (255 = solid, 0 = invisible). |
| `major_tick_image_recolor` | color | #000000 | only when `show_ticks` |  | Tint colour for the major tick image (set Tint Strength above 0 to apply). |
| `major_tick_image_recolor_opa` | stepper | 0 | 0..255; only when `show_ticks` |  | How strongly to tint the major tick image (0 = none, 255 = full tint colour). |
| `mid_tick_image_recolor` | color | #000000 | only when `show_ticks` |  | Tint colour for the medium tick image (set Tint Strength above 0 to apply). |
| `mid_tick_image_recolor_opa` | stepper | 0 | 0..255; only when `show_ticks` |  | How strongly to tint the medium tick image (0 = none, 255 = full tint colour). |
| `minor_tick_image_recolor` | color | #000000 | only when `show_ticks` |  | Tint colour for the minor tick image (set Tint Strength above 0 to apply). |
| `minor_tick_image_recolor_opa` | stepper | 0 | 0..255; only when `show_ticks` |  | How strongly to tint the minor tick image (0 = none, 255 = full tint colour). |
| `major_tick_image_offset` | stepper | 0 | -60..60; only when `show_ticks` |  | Move the major tick image inward (negative) or outward (positive), in pixels. |
| `mid_tick_image_offset` | stepper | 0 | -60..60; only when `show_ticks` |  | Move the medium tick image inward (negative) or outward (positive), in pixels. |
| `minor_tick_image_offset` | stepper | 0 | -60..60; only when `show_ticks` |  | Move the minor tick image inward (negative) or outward (positive), in pixels. |
| `tick_outline_strength` | stepper | 0 | 0..255; only when `show_ticks` |  | Draw an outline/glow behind the drawn ticks so they pop (0 = off, 255 = solid). No image needed. |
| `tick_outline_color` | color | #000000 | only when `show_ticks` |  | Colour of the tick outline/glow (usually black for a crisp edge). |
| `tick_outline_fade` | stepper | 0 | 0..20; only when `show_ticks` |  | Soften the outline into a glow (0 = hard edge, higher = wider soft glow). |

**Numbers**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_tick_labels` | checkbox | True |  |  | Show numbers next to the biggest marks. |
| `tick_label_font` | font |  | only when `show_tick_labels` |  | Text style and size of those numbers. |
| `tick_label_color` | color | #FFFFFF | only when `show_tick_labels` | **N** | Colour of those numbers. |
| `label_gap` | stepper | 10 | -150..150; only when `show_tick_labels` |  | How far the numbers sit from the marks. |
| `tick_label_divisor` | number | 1 | 1..100000; only when `show_tick_labels` |  | Shrink the numbers by dividing them. Set 1000 and a 7000 mark just shows '7' - common on rev counters. |
| `ticks_on_top` | checkbox | False |  |  | Draw the marks and numbers on top of the fill instead of behind it. |

**Redline**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `redline_enabled` | checkbox | False |  |  | Show a warning zone, like a rev counter's red zone. A channel that has a high threshold set switches this on by itself. |
| `redline_threshold` | number | 80 | only when `redline_enabled` |  | The reading where the warning zone starts. If the bound channel carries a high threshold, the channel's value wins. |
| `redline_color` | color | #FF0000 | only when `redline_enabled` |  | Colour of the warning zone (like a rev counter's red zone). Where it starts is set by this reading's high-warning level. |
| `redline_arc_width` | stepper | 0 | 0..50; only when `redline_enabled` |  | How thick the warning-zone band is. |
| `redline_recolor_fill` | checkbox | True | only when `redline_enabled` |  | When the reading reaches the warning zone, turn the whole fill that colour - not just the band. |

**Alerts**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `arc_alerts_enabled` | checkbox | False |  |  | Change the fill colour when the reading goes too low or too high (set the points below). |
| `arc_low` | number | 0 | -99999..99999; only when `arc_alerts_enabled` | **X** | At or below this reading, the fill turns the Low colour. |
| `arc_low_color` | color | #0000FF | only when `arc_alerts_enabled` | **N** | Fill colour when the reading is low. |
| `arc_high` | number | 100 | -99999..99999; only when `arc_alerts_enabled` | **X** | At or above this reading, the fill turns the High colour. |
| `arc_high_color` | color | #FF0000 | only when `arc_alerts_enabled` | **N** | Fill colour when the reading is high. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `smoothing_ms` | slider | 20 | 0..300 step 10 |  | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw, instant value. |


## `toggle` — Toggle Switch

- default size 80×40 · editor resize limits: w 40..200, h 20..80

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `signal_on_threshold` | number | 0.5 |  |  | The reading at or above which the switch shows as ON. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `active_color` | color | #00FF00 |  | **N** | Colour when ON. |
| `active_opa` | stepper | 255 | 0..255 |  | How solid it is when ON (0-255). |
| `inactive_color` | color | #555555 |  | **N** | Colour when OFF. |
| `inactive_opa` | stepper | 100 | 0..255 |  | How solid it is when OFF (0-255). |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `label` | textarea |  |  |  | Text shown on the switch. |
| `show_label` | checkbox | True |  |  | Show the text. |
| `label_color` | color | #FFFFFF | only when `show_label` | **N** | Text colour. |
| `font` | font |  | only when `show_label` |  | Text style and size. |
| `label_align` | select | 1 | options: 0=Left, 1=Center, 2=Right; only when `show_label` |  | Line the text up left, centre or right. |
| `label_x` | stepper | 0 | -400..400; only when `show_label` |  | Nudge the text left or right. |
| `label_y` | stepper | 0 | -240..240; only when `show_label` |  | Nudge the text up or down. |

**Image**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `image_name` | image_picker |  |  | **N** | Icon shown on the switch. |

**CAN TX**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `tx_can_id` | can_id | 0 |  |  | Advanced: the CAN message ID this switch sends. Leave alone unless you're wiring it to the car's CAN bus. |
| `tx_bit_start` | select | 0 |  |  | Advanced: which bit in the CAN message to set. |
| `tx_bit_length` | select | 1 |  |  | Advanced: how many bits to send. |
| `tx_endian` | select | 1 | options: 0=Big Endian, 1=Little Endian |  | Advanced: byte order of the sent value. |
| `tx_rate_hz` | stepper | 10 | 0..50 |  | Advanced: how often the message is resent while ON, per second, as a keepalive (0 = send once on change). |
| `remember_state` | checkbox | False |  |  | Remember on/off across a reboot so a CAN-controlled output comes back the way you left it. |


## `button` — Button

- default size 100×40 · editor resize limits: w 40..300, h 20..100

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `bg_color` | color | #333333 |  | **N** | Colour of the button. |
| `text_color` | color | #FFFFFF |  | **N** | Text colour. |
| `pressed_color` | color | #555555 |  | **N** | Colour while you're pressing it. |

**Frame**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `border_radius` | stepper | 5 | 0..100 |  | How rounded the corners are. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `label` | textarea | BTN |  |  | Text shown on the button. |
| `show_label` | checkbox | True |  |  | Show the text. |
| `font` | font |  | only when `show_label` |  | Text style and size. |
| `label_align` | select | 1 | options: 0=Left, 1=Center, 2=Right; only when `show_label` |  | Line the text up left, centre or right. |
| `label_x` | stepper | 0 | -400..400; only when `show_label` |  | Nudge the text left or right. |
| `label_y` | stepper | 0 | -240..240; only when `show_label` |  | Nudge the text up or down. |

**Image**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `image_name` | image_picker |  |  | **N** | Icon shown on the button. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `latch` | checkbox | False |  |  | Stay on after a press (toggle) instead of only while held. Momentary always sends OFF when you let go. |
| `remember_state` | checkbox | False | only when `latch` |  | Latch only: remember on/off across a reboot so the output comes back the way you left it. |

**CAN TX**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `tx_can_id` | can_id | 0 |  |  | Advanced: the CAN message ID this button sends. Leave alone unless wiring it to the car's CAN bus. |
| `tx_bit_start` | select | 0 |  |  | Advanced: which bit in the CAN message to set. |
| `tx_bit_length` | select | 1 |  |  | Advanced: how many bits to send. |
| `tx_endian` | select | 1 | options: 0=Big Endian, 1=Little Endian |  | Advanced: byte order of the sent value. |
| `tx_rate_hz` | stepper | 10 | 0..50 |  | Advanced: how often the message is resent while held, per second, as a keepalive (0 = send once). A momentary always sends OFF on release regardless. |


## `shift_light` — Shift Light

- default size 400×30 · editor resize limits: w 100..800, h 15..60

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `signal_name` | text |  |  |  | What drives the lights - usually RPM. |
| `range_min` | number | 4000 |  |  | The reading where the first light comes on. |
| `range_max` | number | 7000 |  |  | The reading where every light is on. |

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `led_count` | stepper | 8 | 4..45 step 1 |  | How many lights in the row. |
| `led_spacing` | stepper | 2 | 0..200 step 1 |  | Gap between the lights. |
| `led_width` | stepper | 0 | 0..100 step 1 |  | How wide each light is (0 = fit automatically). |
| `led_height` | stepper | 0 | 0..100 step 1 |  | How tall each light is (0 = fit automatically). |
| `border_radius` | stepper | 2 | 0..255 step 1 |  | How rounded each light's corners are. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `color_low` | color | #00FF00 |  | **N** | Colour of the first (low) lights. |
| `color_mid` | color | #FFFF00 |  | **N** | Colour of the middle lights. |
| `color_high` | color | #FF0000 |  | **N** | Colour of the top lights. |
| `color_off` | color | #212121 |  | **N** | Colour of lights that are off. |
| `threshold_mid` | number | 0.5 | 0..1 step 0.05 |  | Where the lights switch to the middle colour, as a fraction of the range from 0 to 1. 0.5 is halfway. |
| `threshold_high` | number | 0.8 | 0..1 step 0.05 |  | Where the lights switch to the top colour, as a fraction of the range from 0 to 1. 0.8 is four fifths of the way up. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `flash_threshold` | number | 7200 |  |  | The reading where the whole row flashes — your shift point. This one is a real reading, not a fraction. |
| `flash_speed` | stepper | 200 | 50..1000 step 10 |  | How fast it flashes (smaller = faster), in milliseconds. |
| `fill_mode` | select | 0 | options: 0=Left to Right, 1=Outside In |  | How the lights fill up as the reading rises. |


## `pathbar` — Path Bar

- default size 400×260 · editor resize limits: w 20..800, h 20..480

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `min` | number | 0 |  |  | The reading at the start of the bar. |
| `max` | number | 11000 |  |  | The reading at the end of the bar. |

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `shape` | select | 0 | options: 0=Custom, 1=L-Bend, 2=Straight, 3=45° Bend, 4=J-Hook |  | Pick a ready-made shape (straight, L-bend, 45°, or the curved J-hook tacho) sized to fit the box, or Custom to draw your own path. Default matches the firmware factory default (0 = Custom): defaults-only saves omit this field, so a mismatched editor default silently relabels saved custom bars. |
| `orientation` | select | 0 | options: 0=Top-Left / Horizontal, 1=Top-Right / Vertical, 2=Bottom-Left, 3=Bottom-Right |  | Which way the bar runs and fills. |
| `corner_radius` | stepper | 40 | 0..400; only when `shape=1,3` |  | How big the rounded corner of the bend is (or the bevel, on the 45° shape). |
| `hook_angle` | stepper | 120 | 30..200 step 5; only when `shape=4` |  | J-hook shape only: how far the curve sweeps. 90 is a quarter turn, 180 a half turn. |
| `smooth` | checkbox | False | only when `shape=0` |  | Custom shapes only: smooth your drawn points into a flowing curve. |

**Fill**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `lit_color` | color | #2EE4C8 |  |  | Colour of the filled part. |
| `dim_color` | color | #2E323A |  |  | Colour of the empty part. |
| `dim_opa` | stepper | 90 | 0..255 |  | How see-through the empty part is (0-255). |
| `band_width` | stepper | 22 | 2..80 |  | How thick the bar is. |
| `rounded` | checkbox | True |  |  | Round the bar's ends. |
| `fade_fill` | checkbox | False |  |  | Fade the colour from dim to bright along the filled part, for a glowing look. |
| `lead_edge_enabled` | checkbox | True |  |  | Add a bright tip at the current reading, like a glowing bar end. |
| `lead_edge_color` | color | #E6FAFF | only when `lead_edge_enabled` |  | Colour of that bright tip. |
| `lead_edge_width` | stepper | 6 | 0..40; only when `lead_edge_enabled` |  | How wide the bright tip is. |

**Ticks**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_ticks` | checkbox | False |  |  | Show marks along the bar. |
| `major_tick_step` | number | 0 | only when `show_ticks` |  | How far apart the biggest marks are, in reading value. |
| `minor_tick_step` | number | 0 | only when `show_ticks` |  | How far apart the smaller marks are. 0 turns them off. |
| `major_tick_width` | stepper | 3 | 0..40; only when `show_ticks` |  | How thick the biggest marks are. |
| `tick_width` | stepper | 2 | 0..40; only when `show_ticks` |  | How thick the smaller marks are. |
| `major_tick_len` | stepper | 16 | 0..100; only when `show_ticks` |  | How long the biggest marks are. |
| `tick_len` | stepper | 10 | 0..100; only when `show_ticks` |  | How long the smaller marks are. |
| `major_tick_color` | color | #FFFFFF | only when `show_ticks` |  | Colour of the biggest marks. |
| `tick_color` | color | #9E9E9E | only when `show_ticks` |  | Colour of the smaller marks. |

**Numbers**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_labels` | checkbox | False | only when `show_ticks` |  | Show numbers next to the biggest marks. |
| `label_font` | font |  | only when `show_labels` |  | Text style and size of those numbers. |
| `label_color` | color | #FFFFFF | only when `show_labels` |  | Colour of those numbers. |
| `label_gap` | stepper | 14 | -200..200; only when `show_labels` |  | How far the numbers sit from the bar. |
| `label_side` | select | 0 | options: 0=Auto, 1=Side A, 2=Side B; only when `show_labels` |  | Which side of the bar the numbers sit on. Auto picks per number (can flip on sharp paths); Side A/B locks them all to one side. |
| `label_along_offset` | stepper | 0 | -400..400; only when `show_labels` |  | Slide the numbers along the bar (negative = toward the start, positive = toward the end). |
| `tick_label_divisor` | number | 1 | 1..100000; only when `show_labels` |  | Shrink the numbers by dividing them (1000 shows 7000 as '7'). |

**Redline**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `redline` | number | 10000 |  |  | The reading where the warning zone starts along the bar. |
| `redline_color` | color | #FF5C32 |  |  | Colour of the warning zone. |
| `redline_recolor_ticks` | checkbox | True | only when `show_ticks` |  | Turn the marks inside the warning zone that colour too. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `smoothing_ms` | slider | 20 | 0..500 step 10 |  | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |


## `anim` — Animation

- default size 200×200 · editor resize limits: w 20..800, h 20..480

> ⚠ **Editor-only here (flagged X):** `anim_upload` — not a `config` key; the device ignores these. See 05-quirks.

**Value**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `range_min` | number | 0 |  |  | Follow-value mode: the reading that shows the first frame. |
| `range_max` | number | 8000 |  |  | Follow-value mode: the reading that shows the last frame. |
| `threshold` | number | 6000 |  |  | Loop mode: the animation plays while the reading is at or above this value. |

**Frames**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `anim_upload` | anim_frames |  |  | **X** | Upload a GIF, short video clip, or a set of still images. The editor slices it into frames, converts them and sends them to the dash. |
| `frame_prefix` | text |  |  |  | Name prefix of the frame set on the device (frames are stored as prefix_0, prefix_1, ...). Set automatically when you upload an animation. |
| `frame_count` | stepper | 8 | 1..16 |  | How many frames are in the set. Set automatically by the upload; lower it to trim the end of the animation. |

**Behaviour**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `mode` | select | 0 | options: 0=Follow value (scrub), 1=Loop above threshold |  | Follow value: the frame tracks the reading across the range, like a needle. Loop: sits on frame 1 until the reading passes the threshold, then plays all frames on repeat. |
| `loop_fps` | stepper | 12 | 1..30 |  | Loop mode: how fast the frames play, in frames per second. |
| `hyst_pct` | slider | 2 | 0..10 step 1 |  | Stops the animation flickering when the reading sits right on a frame boundary or the threshold. Raise it if the frame chatters. |


## `track_map` — Track Map

- default size 320×240 · editor resize limits: w 60..800, h 60..480

**Track**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `track_asset` | track_picker |  |  |  | Which circuit to draw. Lists the tracks installed on this dash. Push one from RDM Studio (GPS workspace -> Tracks -> Shape), or upload a .rdmtrk here — sending the same track again replaces it. |
| `line_color` | color | #5A6472 |  | **N** | Colour of the circuit outline. |
| `line_width` | stepper | 3 | 1..20 |  | How thick the outline is drawn. |
| `show_start_finish` | checkbox | True |  |  | Draw a tick across the track where the lap starts. |
| `sf_color` | color | #F5F7FA | only when `show_start_finish` | **N** | Colour of the start/finish tick. |

**Face**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `rotation` | slider | 0 | 0..359 step 1 |  | Turn the whole circuit on screen. 0 puts north at the top. |

**Car**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_dot` | checkbox | True |  |  | Show where the car is on the track. Needs an RDM GPS on the bus; the marker dims when the position goes stale. |
| `dot_color` | color | #FF453A | only when `show_dot` | **N** | Colour of the moving car marker. |
| `show_trail` | checkbox | True | only when `show_dot` |  | Draw a short fading tail behind the car, so you can see the line you just took. |
| `dot_radius` | stepper | 5 | 2..24; only when `show_dot` |  | Radius of the car marker in pixels. |
| `lat_channel` | text | gps_latitude | only when `show_dot` |  | Channel carrying latitude. The default matches the RDM GPS; change it only if your position comes from something else. |
| `lon_channel` | text | gps_longitude | only when `show_dot` |  | Channel carrying longitude. |

**Text**

| field | type | default | constraints | flags | what it does |
|---|---|---|---|---|---|
| `show_name` | checkbox | True |  |  | Print the circuit's name under the map. The name comes from the track file, not from this layout. |
| `name_color` | color | #8A93A0 | only when `show_name` | **N** | Colour of the track name. |

