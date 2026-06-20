# Widget Catalog — every type, every setting

> AUTO-GENERATED from `schema/widgets.schema.json` by `scripts/gen_catalog.py`. Do not hand-edit. Every layout JSON must carry `"schema_version": 15`.

Each widget is `{ "type", "id", "x", "y", "w", "h", "config": { ...fields below... } }`. Put a field in `config` only when it differs from the default (keeps the layout under the 32 KB budget). `*_color` fields are **RGB565 integers** (see 03-colors). Bind data with `config.signal_name` (see 04-channels).

**16 widget types.**

| Type | Name | Default size | Singleton |
|---|---|---|---|
| `rpm_bar` | RPM Bar | 800x55 | yes |
| `panel` | Panel | 155x92 |  |
| `bar` | Bar Graph | 300x30 |  |
| `indicator` | Turn Indicator | 50x50 |  |
| `warning` | Alert Light | 25x25 |  |
| `text` | Text / Value | 100x30 |  |
| `meter` | Meter | 300x300 |  |
| `image` | Image | 100x100 |  |
| `shape_panel` | Shape Panel | 200x100 |  |
| `line` | Line | 200x4 |  |
| `banner` | Alert Banner | 800x80 |  |
| `arc` | Arc Shape | 200x200 |  |
| `toggle` | Toggle Switch | 80x40 |  |
| `button` | Button | 100x40 |  |
| `shift_light` | Shift Light | 400x30 |  |
| `pathbar` | Path Bar | 400x260 |  |


## `rpm_bar` — RPM Bar

- default size 800×55 · default pos (0,-213) · **singleton** (only one allowed)

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `rpm_max` | number | 7000 |  | The highest RPM the bar shows. It fills from 0 up to here. |
| `redline` | number | 6000 |  | The RPM where the red warning zone starts - the bar changes colour past this point. |
| `limiter_value` | number | 7000 |  | The RPM where your rev limiter kicks in - the bar reacts here. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `smoothing_ms` | slider | 80 | 0..300 step 10 | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |
| `limiter_effect` | select | 0 | options: 0=None, 1=Bar Flash, 2=Bar Solid | What happens at the limiter - flash, or just change colour. |
| `flash_speed` | stepper | 200 | 50..1000 step 50; only when `limiter_effect=1` | How fast it flashes at the limiter (smaller = faster), in milliseconds. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bar_color` | color | #00FF00 |  | Colour of the bar below the redline. |
| `fill_dir` | select | 0 | options: 0=Left to Right, 1=Right to Left, 2=Center Out, 3=Edges In | Which way the bar fills (left, right, up or down). |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bar_bg_color` | color | #F0F0F0 |  | Colour of the empty background. |

**Redline**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `limiter_color` | color | #FF0000 |  | Colour shown when you hit the limiter. |

**Ticks**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_ticks` | checkbox | True |  | Show marks along the bar. |
| `tick_side` | select | 2 | options: 0=Top, 1=Bottom, 2=Both; only when `show_ticks` | Which side of the bar the marks sit on. |
| `tick_width` | stepper | 3 | 1..12; only when `show_ticks` | How thick the marks are. |
| `tick_length` | stepper | 12 | 1..40; only when `show_ticks` | How long the marks are. |
| `tick_color` | color | #000000 | only when `show_ticks` | Colour of the marks. |


## `panel` — Panel

- default size 155×92

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `label` | text |  |  | The title shown above the number (e.g. COOLANT). |
| `decimals` | number | 0 |  | How many digits after the decimal point to show. |

**Text**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `label_font` | font |  |  | Text style and size of the title. |
| `label_color` | color | #FFFFFF |  | Colour of the title. |
| `value_font` | font |  |  | Text style and size of the number. |
| `value_color` | color | #FFFFFF |  | Colour of the number. |
| `text_align` | select | 1 | options: 0=Left, 1=Center, 2=Right | Line the text up to the left, centre or right. |
| `label_y_offset` | stepper | -28 | -100..100 | Nudge the title up or down. |
| `value_y_offset` | stepper | 9 | -100..100 | Nudge the number up or down. |
| `show_peak` | select | 0 | options: 0=Off, 1=Max, 2=Min, 3=Min/Max | Also show the highest (or lowest) reading reached, held on screen. |
| `peak_font` | font |  | only when `show_peak` | Text style and size of the held reading. |
| `peak_x_offset` | stepper | 0 | -100..100; only when `show_peak` | Nudge the held reading left or right. |
| `peak_y_offset` | stepper | 31 | -100..100; only when `show_peak` | Nudge the held reading up or down. |
| `show_unit` | checkbox | False |  | Add the unit after the number, like °C or PSI. |
| `unit_size` | select | 2 | options: 0=Small, 1=Medium, 2=Full; only when `show_unit` | How big the unit is next to the number. |
| `custom_text` | text |  |  | Replace the unit with your own text. |
| `custom_text_x_offset` | stepper | 41 | -100..100 | Nudge that text left or right. |
| `custom_text_y_offset` | stepper | 32 | -100..100 | Nudge that text up or down. |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bg_color` | color | #000000 |  | Background colour of the panel. |
| `bg_opa` | stepper | 255 | 0..255 | How solid the background is (0 = see-through, 255 = solid). |
| `border_color` | color | #2E2F2E |  | Colour of the outline. |
| `border_width` | stepper | 3 | 0..20 | Thickness of the outline (0 = none). |
| `border_radius` | stepper | 7 | 0..100 | How rounded the corners are. |

**Alerts**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `warning_low_enabled` | checkbox | False |  | Warn when the reading drops too low. |
| `warning_low_threshold` | stepper | 0 | -99999..99999; only when `warning_low_enabled` | Warn once the reading falls to or below this. |
| `warning_low_color` | color | #0000FF | only when `warning_low_enabled` | The alert colour used when it's too low. |
| `warning_low_apply_label` | checkbox | True | only when `warning_low_enabled` | Turn the title the alert colour when it's too low. |
| `warning_low_apply_value` | checkbox | True | only when `warning_low_enabled` | Turn the number the alert colour when it's too low. |
| `warning_low_apply_panel` | checkbox | False | only when `warning_low_enabled` | Turn the panel's edge/background the alert colour when it's too low. |
| `warning_high_enabled` | checkbox | False |  | Warn when the reading climbs too high. |
| `warning_high_threshold` | stepper | 0 | -99999..99999; only when `warning_high_enabled` | Warn once the reading rises to or above this. |
| `warning_high_color` | color | #FF0000 | only when `warning_high_enabled` | The alert colour used when it's too high. |
| `warning_high_apply_label` | checkbox | True | only when `warning_high_enabled` | Turn the title the alert colour when it's too high. |
| `warning_high_apply_value` | checkbox | True | only when `warning_high_enabled` | Turn the number the alert colour when it's too high. |
| `warning_high_apply_panel` | checkbox | False | only when `warning_high_enabled` | Turn the panel's edge/background the alert colour when it's too high. |


## `bar` — Bar Graph

- default size 300×30

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `label` | text |  |  | Text shown with the bar. |
| `bar_min` | number | 0 |  | The reading at the empty end of the bar. |
| `bar_max` | number | 100 |  | The reading at the full end of the bar. |
| `decimals` | number | 0 |  | How many digits after the decimal point to show. |
| `anchor_enabled` | checkbox | False |  | Pin a chosen reading to a fixed spot on the bar instead of filling from the end. e.g. put your normal operating temperature in the middle, so anything hotter or colder is obvious at a glance. |
| `anchor_value` | number | 50 | only when `anchor_enabled` | The reading you want pinned in place - e.g. 90 for your normal coolant temp. |
| `anchor_position` | slider | 50 | 0..100; only when `anchor_enabled` | Where that reading sits along the bar (50 = middle, 0 = start, 100 = end). |
| `show_bar_value` | checkbox | False |  | Show the number on the bar. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `smoothing_ms` | slider | 80 | 0..300 step 10 | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `invert_bar_value` | checkbox | False |  | Flip it so a high reading empties the bar instead of filling it. |
| `center_fill` | checkbox | False |  | Fill out from the middle of the bar in both directions. |
| `fill_dir` | select | 0 | options: 0=Left to Right, 1=Right to Left, 2=Center Out, 3=Edges In | Which way the bar fills (left, right, up or down). |
| `bar_in_range_color` | color | #00FF00 |  | Colour of the bar. |
| `bar_image_full` | image_picker |  |  | Use a picture for the filled part instead of a plain colour. |
| `fill_edge_width` | stepper | 0 | 0..20 | How wide the bright tip at the current reading is (0 = off). |
| `fill_edge_color` | color | #FFFFFF | only when `fill_edge_width` | Colour of the bright tip that sits at the current reading. |
| `indicator_radius` | stepper | 5 | 0..50 | How rounded the filled part's corners are. |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bar_bg_color` | color | #2E2F2E |  | Colour of the empty part behind the fill. |
| `bar_image` | image_picker |  |  | Use a picture for the empty background instead of a plain colour. |
| `bar_radius` | stepper | 5 | 0..50 | How rounded the whole bar's corners are. |
| `bar_border_width` | stepper | 2 | 0..20 | Thickness of the outline (0 = none). |
| `bar_border_color` | color | #2E2F2E |  | Colour of the outline. |

**Text**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `label_font` | font |  |  | Text style and size of the label. |
| `label_color` | color | #FFFFFF |  | Colour of the label. |
| `value_font` | font |  |  | Text style and size of the number. |
| `value_color` | color | #FFFFFF |  | Colour of the number. |

**Ticks**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_ticks` | checkbox | False |  | Show marks along the bar, like the lines on a fuel gauge. |
| `tick_side` | select | 2 | options: 0=Top, 1=Bottom, 2=Both; only when `show_ticks` | Which side of the bar the marks sit on. |
| `tick_count` | stepper | 5 | 2..30; only when `show_ticks` | How many marks to show along the bar. |
| `tick_width` | stepper | 2 | 1..10; only when `show_ticks` | How thick the marks are. |
| `tick_length` | stepper | 6 | 1..40; only when `show_ticks` | How long the marks are. |
| `tick_color` | color | #E8E8E8 | only when `show_ticks` | Colour of the marks. |

**Alerts**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bar_alerts_enabled` | checkbox | False |  | Change the bar colour when the reading goes too low or too high (set the points below). |
| `bar_low` | stepper | 0 | -99999..99999; only when `bar_alerts_enabled` | At or below this reading, the bar turns the Low colour. |
| `bar_low_color` | color | #0000FF | only when `bar_alerts_enabled` | Bar colour when the reading is low. |
| `bar_high` | stepper | 100 | -99999..99999; only when `bar_alerts_enabled` | At or above this reading, the bar turns the High colour. |
| `bar_high_color` | color | #FF0000 | only when `bar_alerts_enabled` | Bar colour when the reading is high. |


## `indicator` — Turn Indicator

- default size 50×50

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `slot` | select | 0 | options: 0=Left, 1=Right | Which side of the screen this light sits on. |
| `input_source` | select | 0 | options: 0=Wire, 1=CAN | What turns the light on - a sensor reading or a built-in source. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `color_on` | color | #FFBF00 |  | Colour when the light is on. |
| `opa_on` | stepper | 255 | 0..255 | How bright it is when on (0-255). |
| `color_off` | color | #333333 |  | Colour when the light is off. |
| `opa_off` | stepper | 70 | 0..255 | How bright it is when off (0-255). |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `animation` | checkbox | True |  | Fade smoothly between on and off. |
| `is_momentary` | checkbox | False |  | Only stay on while the signal is active, instead of latching on. |


## `warning` — Alert Light

- default size 25×25

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `label` | text |  |  | Text shown on the tile. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `active_color` | color | #FF0000 |  | Tile colour when the warning is on. |
| `active_opa` | stepper | 255 | 0..255 | How solid the tile is when on (0-255). |
| `inactive_color` | color | #292C29 |  | Tile colour when off. |
| `inactive_opa` | stepper | 180 | 0..255 | How solid the tile is when off (0-255). |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `radius` | stepper | 100 | 0..200 | How rounded the tile's corners are. |
| `border_width` | stepper | 0 | 0..20 | Thickness of the outline (0 = none). |
| `border_color_style` | color | #000000 |  | Colour of the outline. |

**Image**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `image_name` | image_picker |  |  | Icon shown on the tile. |
| `image_scale` | stepper | 100 | 10..200 | Resize the icon, as a percentage. |

**Text**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_label` | checkbox | True |  | Show the text on the tile. |
| `label_color` | color | #FFFFFF | only when `show_label` | Text colour. |
| `label_font` | font |  | only when `show_label` | Text style and size. |
| `label_y_offset` | slider | 0 | -100..100; only when `show_label` | Nudge the text up or down. |
| `label_text_align` | select | 1 | options: 0=Left, 1=Center, 2=Right; only when `show_label` | Line the text up left, centre or right. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `is_momentary` | checkbox | True |  | Only stay on while the signal is active, instead of latching on. |
| `invert_toggle` | checkbox | False |  | Flip it - the warning shows when the signal is OFF. |


## `text` — Text / Value

- default size 100×30

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `static_text` | text |  |  | Type fixed text to show. Leave blank to show the live reading instead. |
| `decimals` | number | 0 |  | How many digits after the decimal point when showing a reading. |

**Text**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `font` | font |  |  | Text style and size. |
| `text_color` | color | #FFFFFF |  | Text colour. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `rotation` | stepper | 0 | 0..359 | Turn the text on an angle, in degrees. |


## `meter` — Meter

- default size 300×300

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `min` | number | 0 |  | The reading at the start of the dial. |
| `max` | number | 100 |  | The reading at the end of the dial. |
| `start_angle_user` | stepper | 225 | 0..359 step 5 | Where the dial starts, as a clock position in degrees (0 = 3 o'clock, going clockwise). |
| `sweep_degrees` | slider | 270 | 30..360 step 5 | How far around the dial goes. 270 is a normal gauge; 360 is a full circle. |
| `reverse` | checkbox | False |  | Run the dial backwards, so the high reading sits at the start. |
| `anchor_enabled` | checkbox | False |  | Pin a chosen reading to a fixed spot on the dial instead of sweeping from the start. e.g. put your normal operating temperature at the middle, so the needle rests centre when warmed up and swings high or low if it drifts. |
| `anchor_value` | number | 50 | only when `anchor_enabled` | The reading you want pinned in place - e.g. 90 for your normal coolant temp. |
| `anchor_position` | slider | 50 | 0..100; only when `anchor_enabled` | Where that reading sits along the dial (50 = middle, 0 = start, 100 = end). |

**Ticks**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_ticks` | checkbox | True |  | Show the marks around the dial, like the lines on a speedometer. |
| `major_tick_step` | number | 50 |  | How far apart the biggest marks are, in reading value. On a 0-8000 rev counter, 1000 puts a big mark every 1000 RPM. |
| `mid_tick_step` | number | 0 | only when `show_ticks` | How far apart the medium marks are. 0 turns them off. |
| `minor_tick_step` | number | 10 |  | How far apart the smallest marks are. 0 turns them off. |
| `major_tick_width` | stepper | 4 | 1..20; only when `show_ticks` | How thick the biggest marks are. |
| `mid_tick_width` | stepper | 2 | 0..20; only when `show_ticks` | How thick the medium marks are (0 = off). |
| `minor_tick_width` | stepper | 2 | 1..20; only when `show_ticks` | How thick the smallest marks are. |
| `major_tick_length` | stepper | 15 | 1..50; only when `show_ticks` | How long the biggest marks are. |
| `mid_tick_length` | stepper | 13 | 0..100; only when `show_ticks` | How long the medium marks are (0 = off). |
| `minor_tick_length` | stepper | 10 | 1..50; only when `show_ticks` | How long the smallest marks are. |
| `major_tick_color` | color | #FFFFFF | only when `show_ticks` | Colour of the biggest marks. |
| `mid_tick_color` | color | #BDBDBD | only when `show_ticks` | Colour of the medium marks. |
| `minor_tick_color` | color | #9E9E9E | only when `show_ticks` | Colour of the smallest marks. |

**Numbers**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_tick_labels` | checkbox | True |  | Show numbers next to the biggest marks. |
| `tick_label_font` | font |  | only when `show_tick_labels` | Text style and size of those numbers. |
| `tick_label_color` | color | #FFFFFF | only when `show_tick_labels` | Colour of those numbers. |
| `label_gap` | stepper | 10 | -150..150; only when `show_tick_labels` | How far the numbers sit from the marks. |
| `tick_label_divisor` | stepper | 1 | 1..100000; only when `show_tick_labels` | Shrink the numbers by dividing them. Set 1000 and a 7000 mark just shows '7' - common on rev counters. |

**Redline**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `redline_enabled` | checkbox | False |  | Show a warning zone, like a rev counter's red zone. |
| `redline_threshold` | number | 80 | only when `redline_enabled` | The reading where the warning zone starts. |
| `redline_color` | color | #FF0000 | only when `redline_enabled` | Colour of the warning zone. |
| `redline_show_arc` | checkbox | True | only when `redline_enabled` | Draw a coloured band along the warning zone. |
| `redline_arc_width` | stepper | 6 | 1..30; only when `redline_enabled` | How thick that band is. |
| `redline_arc_r_mod` | stepper | 0 | -50..50; only when `redline_enabled` | Move the warning band in or out from the dial. |
| `redline_recolor_ticks` | checkbox | True | only when `redline_enabled` | Turn the marks inside the warning zone that colour too. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `smoothing_ms` | slider | 80 | 0..300 step 10 | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `meter_bg_color` | color | #3D3D3D |  | Background colour of the gauge face. |
| `meter_bg_opa` | stepper | 255 | 0..255 | How solid the gauge face is (0 = see-through, 255 = solid). |
| `bg_image_name` | image_picker |  |  | Use a picture as the gauge face. |
| `border_color` | color | #000000 |  | Colour of the outline around the gauge. |
| `border_width` | stepper | 0 | 0..20 | Thickness of the outline (0 = none). |
| `border_opa` | stepper | 255 | 0..255 | How solid the outline is (0-255). |
| `scale_padding` | stepper | 0 | 0..100 | Move the marks inward from the edge of the gauge. |

**Needle**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_needle` | checkbox | True |  | Show the pointer needle. |
| `needle_width` | stepper | 4 | 1..20; only when `show_needle` | How thick the needle is. |
| `needle_color` | color | #FFFFFF | only when `show_needle` | Colour of the needle. |
| `needle_r_mod` | stepper | -10 | -100..100; only when `show_needle` | Shorten the needle from its outer end. |
| `needle_rear_length` | stepper | 0 | 0..100; only when `show_needle` | How far the needle sticks out behind its centre, for a counterweight look. |
| `needle_inner_radius` | stepper | 0 | 0..400; only when `show_needle` | Start the needle away from the centre, leaving a gap in the middle. |
| `needle_tip_style` | select | 0 | options: 0=Flat, 1=Rounded, 2=Lance, 3=Dagger, 4=Spade, 5=Diamond; only when `show_needle` | The shape of the needle's tip. |
| `needle_tip_base_w` | stepper-auto | 0 | 0..30; only when `show_needle` | How wide the needle is at its base (leave at auto unless you want to set it). |
| `needle_tip_point_w` | stepper-auto | 0 | 0..20; only when `show_needle` | How wide the needle is at its tip. |
| `needle_tip_taper` | stepper-auto | 0 | 0..100; only when `show_needle` | How sharply the needle narrows toward the tip. |
| `needle_image_name` | image_picker |  | only when `show_needle` | Use a picture as the needle instead of a drawn one. |
| `needle_pivot_x` | stepper | 0 | -400..400; only when `needle_image_name` | Move the needle's centre point left or right. |
| `needle_pivot_y` | stepper | 0 | -400..400; only when `needle_image_name` | Move the needle's centre point up or down. |
| `needle_angle_offset` | stepper | 0 | -180..180; only when `needle_image_name` | Rotate where the needle rests, in degrees. |
| `show_needle_ball` | checkbox | True |  | Show a round hub in the middle of the needle. |
| `needle_ball_size` | stepper | 10 | 0..40; only when `show_needle_ball` | How big the hub is. |
| `needle_ball_color` | color | #FFFFFF | only when `show_needle_ball` | Colour of the hub. |


## `image` — Image

- default size 100×100

**Image**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `image_name` | image_picker |  |  | The picture to show. |
| `image_scale` | stepper | 100 | 10..200 | Resize the picture, as a percentage. |
| `opacity` | stepper | 255 | 0..255 | How see-through the picture is (0 = invisible, 255 = solid). |
| `recolor` | color | #000000 |  | Tint the whole picture this colour. |
| `recolor_opa` | stepper | 0 | 0..255 | How strong the tint is (0 = none, 255 = full). Tinting a light icon white can make it disappear. |


## `shape_panel` — Shape Panel

- default size 200×100

**Shape**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `shape_type` | select | rectangle | options: rectangle=Rectangle, circle=Circle, triangle=Triangle, diamond=Diamond, arrow_right=Arrow ▶, arrow_left=Arrow ◀, chevron_right=Chevron ❯, chevron_left=Chevron ❮ | The shape to draw. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bg_color` | color | #1A1A1A |  | Fill colour. |
| `bg_opa` | stepper | 255 | 0..255 | How see-through the fill is (0 = invisible, 255 = solid). |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `border_color` | color | #2E2F2E |  | Colour of the outline. |
| `border_width` | stepper | 0 | 0..20 | Thickness of the outline (0 = none). |
| `border_radius` | stepper | 10 | 0..200 | How rounded the corners are (for rectangles). |

**Shadow**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `shadow_width` | stepper | 0 | 0..50 | How far the drop shadow spreads (0 = no shadow). |
| `shadow_color` | color | #000000 |  | Colour of the shadow. |
| `shadow_opa` | stepper | 128 | 0..255 | How dark the shadow is (0-255). |
| `shadow_ofs_x` | stepper | 0 | -100..100 | Move the shadow left or right. |
| `shadow_ofs_y` | stepper | 0 | -100..100 | Move the shadow up or down. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bake_into_gauge` | checkbox | False |  | Lock this shape into the background so it draws faster. It won't change while driving - use for decoration only. |


## `line` — Line

- default size 200×4

**Style**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `orientation` | select | horizontal | options: horizontal=Horizontal, vertical=Vertical, diagonal_fwd=Diagonal /, diagonal_bwd=Diagonal \ | Which way the line runs (across, up-down, or diagonal). |
| `line_color` | color | #FFFFFF |  | Colour of the line. |
| `line_width` | stepper | 4 | 1..30 | How thick the line is. |
| `line_opa` | stepper | 255 | 0..255 | How see-through the line is (0-255). |
| `rounded` | checkbox | False |  | Round the line's ends. |
| `dash_gap` | stepper | 0 | 0..40 | Gap between dashes (0 = a solid line). |


## `banner` — Alert Banner

- default size 800×80

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `op` | select | 0 | options: 0=>, 1=<, 2=>=, 3=<=, 4===, 5=!=, 6=range, 7=always (test) | How the reading is checked to pop up the message (over, under, equal to, or within a range). |
| `threshold` | number | 0 |  | The value the reading is checked against. |
| `range_min` | number | 0 |  | The bottom of the range (when using 'within a range'). |
| `range_max` | number | 0 |  | The top of the range (when using 'within a range'). |

**Text**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `text` | text | WARNING |  | The message to show when it pops up. |
| `text_align` | select | 1 | options: 0=Left, 1=Center, 2=Right | Line the message up left, centre or right. |
| `text_color` | color | #000000 |  | Colour of the message. |
| `font` | font |  |  | Text style and size. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bg_color` | color | #FF0000 |  | Background colour. |
| `bg_opa` | slider | 128 | 0..255 | How solid the background is (0-255). |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `border_width` | stepper | 0 | 0..20 | Thickness of the outline (0 = none). |
| `border_color` | color | #000000 |  | Colour of the outline. |
| `radius` | stepper | 0 | 0..100 | How rounded the corners are. |


## `arc` — Arc Shape

- default size 200×200

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `start_angle_user` | stepper | 225 | 0..359 step 5 | Where the gauge starts, as a clock position in degrees (0 = 3 o'clock, going clockwise). This is the empty end. |
| `sweep_degrees` | slider | 270 | 30..360 step 5 | How far around the circle the gauge goes. 270 covers most of a circle like a normal gauge; 360 is a full circle. |
| `signal_min` | number | 0 |  | The reading that shows the gauge empty (the start). |
| `signal_max` | number | 100 |  | The reading that shows the gauge full (the end). |
| `tick_min` | number | 0 |  | The first number the marks count from. Usually the same as the empty value. |
| `tick_max` | number | 0 |  | The last number the marks count up to. Usually the same as the full value. |
| `anchor_enabled` | checkbox | False |  | Pin a chosen reading to a fixed spot on the gauge instead of filling from the empty end. e.g. put your normal operating temperature in the middle, so the gauge sits centre when warmed up and clearly runs high or low if it drifts. |
| `anchor_value` | number | 50 | only when `anchor_enabled` | The reading you want pinned in place - e.g. 90 for your normal coolant temp. |
| `anchor_position` | slider | 50 | 0..100; only when `anchor_enabled` | Where that reading sits along the gauge (50 = middle, 0 = start, 100 = end). |
| `reverse` | checkbox | False |  | Run the gauge backwards, so the full end becomes the empty end. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `smoothing_ms` | slider | 80 | 0..300 step 10 | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw, instant value. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `arc_width` | stepper | 10 | 1..50 | How thick the coloured (filled) part of the gauge is. |
| `arc_offset` | stepper | 0 | 0..120 | Moves the ring in or out from the centre. Use it to stack one gauge inside another. |
| `arc_color` | color | #00FF00 |  | Colour of the filled part. |
| `bg_arc_color` | color | #333333 |  | Colour of the empty part behind the fill. |
| `bg_arc_width` | stepper | 10 | 1..50 | How thick the empty background ring is. Make it wider or narrower than the fill for a raised or recessed look. |
| `rounded_ends` | checkbox | False |  | Give the gauge rounded ends instead of flat-cut ones. |
| `fade_fill` | checkbox | False |  | Fade the colour from dim to bright along the filled part, for a glowing look. |
| `lead_edge_enabled` | checkbox | True |  | Add a bright tip that sits at the current reading, like the glowing end of a digital bar. |
| `lead_edge_color` | color | #E6FAFF | only when `lead_edge_enabled` | Colour of that bright tip. |
| `lead_edge_width` | stepper | 6 | 0..40; only when `lead_edge_enabled` | How wide the bright tip is. |
| `arc_image` | image_picker |  |  | Use a picture for the empty background instead of a plain colour. |
| `arc_image_full` | image_picker |  |  | Use a picture for the filled part instead of a plain colour. It's uncovered bit by bit as the reading rises. |

**Redline**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `redline_color` | color | #FF0000 |  | Colour of the warning zone (like a rev counter's red zone). Where it starts is set by this reading's high-warning level. |
| `redline_arc_width` | stepper | 0 | 0..50 | How thick the warning-zone band is. |
| `redline_recolor_fill` | checkbox | True |  | When the reading reaches the warning zone, turn the whole fill that colour - not just the band. |

**Ticks**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_ticks` | checkbox | False |  | Show the marks around the gauge, like the lines on a speedometer. |
| `major_tick_step` | number | 50 | only when `show_ticks` | How far apart the biggest marks are, in reading value. On a 0-8000 rev counter, 1000 puts a big mark every 1000 RPM. |
| `mid_tick_step` | number | 0 | only when `show_ticks` | How far apart the medium marks are. 0 turns them off. |
| `minor_tick_step` | number | 10 | only when `show_ticks` | How far apart the smallest marks are. 0 turns them off. |
| `major_tick_width` | stepper | 4 | 1..20; only when `show_ticks` | How thick the biggest marks are. |
| `mid_tick_width` | stepper | 2 | 0..20; only when `show_ticks` | How thick the medium marks are. |
| `minor_tick_width` | stepper | 2 | 1..20; only when `show_ticks` | How thick the smallest marks are. |
| `major_tick_length` | stepper | 15 | 0..50; only when `show_ticks` | How long the biggest marks are. |
| `mid_tick_length` | stepper | 13 | 0..100; only when `show_ticks` | How long the medium marks are. |
| `minor_tick_length` | stepper | 10 | 0..50; only when `show_ticks` | How long the smallest marks are. |
| `major_tick_color` | color | #FFFFFF | only when `show_ticks` | Colour of the biggest marks. |
| `mid_tick_color` | color | #BDBDBD | only when `show_ticks` | Colour of the medium marks. |
| `minor_tick_color` | color | #9E9E9E | only when `show_ticks` | Colour of the smallest marks. |

**Numbers**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_tick_labels` | checkbox | True |  | Show numbers next to the biggest marks. |
| `tick_label_font` | font |  | only when `show_tick_labels` | Text style and size of those numbers. |
| `tick_label_color` | color | #FFFFFF | only when `show_tick_labels` | Colour of those numbers. |
| `label_gap` | stepper | 10 | -150..150; only when `show_tick_labels` | How far the numbers sit from the marks. |
| `tick_label_divisor` | stepper | 1 | 1..100000; only when `show_tick_labels` | Shrink the numbers by dividing them. Set 1000 and a 7000 mark just shows '7' - common on rev counters. |
| `ticks_on_top` | checkbox | False |  | Draw the marks and numbers on top of the fill instead of behind it. |

**Needle**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_value_line` | checkbox | False |  | Show a thin pointer line at the current reading. |
| `value_line_width` | stepper | 4 | 1..20; only when `show_value_line` | How thick the pointer line is. |
| `value_line_color` | color | #FFFFFF | only when `show_value_line` | Colour of the pointer line. |
| `value_line_r_mod` | stepper | -10 | -50..50; only when `show_value_line` | Shorten the pointer line from the outer edge inward. |

**Alerts**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `arc_alerts_enabled` | checkbox | False |  | Change the fill colour when the reading goes too low or too high (set the points below). |
| `arc_low` | stepper | 0 | -99999..99999; only when `arc_alerts_enabled` | At or below this reading, the fill turns the Low colour. |
| `arc_low_color` | color | #0000FF | only when `arc_alerts_enabled` | Fill colour when the reading is low. |
| `arc_high` | stepper | 100 | -99999..99999; only when `arc_alerts_enabled` | At or above this reading, the fill turns the High colour. |
| `arc_high_color` | color | #FF0000 | only when `arc_alerts_enabled` | Fill colour when the reading is high. |


## `toggle` — Toggle Switch

- default size 80×40

**Text**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `label` | textarea |  |  | Text shown on the switch. |
| `show_label` | checkbox | True |  | Show the text. |
| `label_color` | color | #FFFFFF | only when `show_label` | Text colour. |
| `font` | font |  | only when `show_label` | Text style and size. |
| `label_align` | select | 1 | options: 0=Left, 1=Center, 2=Right; only when `show_label` | Line the text up left, centre or right. |
| `label_x` | stepper | 0 | -400..400; only when `show_label` | Nudge the text left or right. |
| `label_y` | stepper | 0 | -240..240; only when `show_label` | Nudge the text up or down. |

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `signal_on_threshold` | number | 0.5 |  | The reading at or above which the switch shows as ON. |

**CAN TX**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `tx_can_id` | can_id | 0 |  | Advanced: the CAN message ID this switch sends. Leave alone unless you're wiring it to the car's CAN bus. |
| `tx_bit_start` | select | 0 |  | Advanced: which bit in the CAN message to set. |
| `tx_bit_length` | select | 1 |  | Advanced: how many bits to send. |
| `tx_endian` | select | 1 | options: 0=Big Endian, 1=Little Endian | Advanced: byte order of the sent value. |
| `tx_rate_hz` | stepper | 0 | 0..50 | Advanced: how often to resend the message, per second (0 = only when it changes). |

**Image**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `image_name` | image_picker |  |  | Icon shown on the switch. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `active_color` | color | #00FF00 |  | Colour when ON. |
| `active_opa` | stepper | 255 | 0..255 | How solid it is when ON (0-255). |
| `inactive_color` | color | #555555 |  | Colour when OFF. |
| `inactive_opa` | stepper | 100 | 0..255 | How solid it is when OFF (0-255). |


## `button` — Button

- default size 100×40

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `label` | textarea | BTN |  | Text shown on the button. |
| `show_label` | checkbox | True |  | Show the text. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `latch` | checkbox | False |  | Stay on after a press (toggle) instead of only while held. |

**CAN TX**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `tx_can_id` | can_id | 0 |  | Advanced: the CAN message ID this button sends. Leave alone unless wiring it to the car's CAN bus. |
| `tx_bit_start` | select | 0 |  | Advanced: which bit in the CAN message to set. |
| `tx_bit_length` | select | 1 |  | Advanced: how many bits to send. |
| `tx_endian` | select | 1 | options: 0=Big Endian, 1=Little Endian | Advanced: byte order of the sent value. |
| `tx_rate_hz` | stepper | 0 | 0..50 | Advanced: how often to resend while held, per second (0 = send once). |
| `tx_send_release` | checkbox | False | only when `!latch` | Advanced: also send a message when you let go. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `bg_color` | color | #333333 |  | Colour of the button. |
| `text_color` | color | #FFFFFF |  | Text colour. |
| `pressed_color` | color | #555555 |  | Colour while you're pressing it. |

**Frame**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `border_radius` | stepper | 5 | 0..100 | How rounded the corners are. |

**Text**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `font` | font |  | only when `show_label` | Text style and size. |
| `label_align` | select | 1 | options: 0=Left, 1=Center, 2=Right; only when `show_label` | Line the text up left, centre or right. |
| `label_x` | stepper | 0 | -400..400; only when `show_label` | Nudge the text left or right. |
| `label_y` | stepper | 0 | -240..240; only when `show_label` | Nudge the text up or down. |

**Image**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `image_name` | image_picker |  |  | Icon shown on the button. |


## `shift_light` — Shift Light

- default size 400×30

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `signal_name` | text |  |  | What drives the lights - usually RPM. |
| `range_min` | number | 4000 |  | The reading where the first light comes on. |
| `range_max` | number | 7000 |  | The reading where every light is on. |
| `led_count` | number | 8 | 4..45 step 1 | How many lights in the row. |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `flash_threshold` | number | 7200 |  | The reading where the whole row flashes - your shift point. |
| `flash_speed` | stepper | 200 | 50..1000 step 10 | How fast it flashes (smaller = faster), in milliseconds. |
| `fill_mode` | select | 0 | options: 0=Left to Right, 1=Outside In | How the lights fill up as the reading rises. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `color_low` | color | #00FF00 |  | Colour of the first (low) lights. |
| `color_mid` | color | #FFFF00 |  | Colour of the middle lights. |
| `color_high` | color | #FF0000 |  | Colour of the top lights. |
| `color_off` | color | #212121 |  | Colour of lights that are off. |
| `threshold_mid` | number | 0.5 | 0..1 step 0.05 | The reading where the lights switch to the middle colour. |
| `threshold_high` | number | 0.8 | 0..1 step 0.05 | The reading where the lights switch to the top colour. |

**Layout**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `led_spacing` | stepper | 2 | 0..200 step 1 | Gap between the lights. |
| `led_width` | stepper | 0 | 0..100 step 1 | How wide each light is (0 = fit automatically). |
| `led_height` | stepper | 0 | 0..100 step 1 | How tall each light is (0 = fit automatically). |
| `border_radius` | stepper | 2 | 0..255 step 1 | How rounded each light's corners are. |


## `pathbar` — Path Bar

- default size 400×260

**Value**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `min` | number | 0 |  | The reading at the start of the bar. |
| `max` | number | 11000 |  | The reading at the end of the bar. |
| `redline` | number | 10000 |  | The reading where the warning zone starts along the bar. |

**Fill**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `lit_color` | color | #2EE4C8 |  | Colour of the filled part. |
| `dim_color` | color | #2E323A |  | Colour of the empty part. |
| `dim_opa` | slider | 90 | 0..255 | How see-through the empty part is (0-255). |
| `band_width` | stepper | 22 | 2..80 | How thick the bar is. |
| `rounded` | checkbox | True |  | Round the bar's ends. |
| `fade_fill` | checkbox | False |  | Fade the colour from dim to bright along the filled part, for a glowing look. |
| `lead_edge_enabled` | checkbox | True |  | Add a bright tip at the current reading, like a glowing bar end. |
| `lead_edge_color` | color | #E6FAFF | only when `lead_edge_enabled` | Colour of that bright tip. |
| `lead_edge_width` | stepper | 6 | 0..40; only when `lead_edge_enabled` | How wide the bright tip is. |

**Redline**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `redline_color` | color | #FF5C32 |  | Colour of the warning zone. |
| `redline_recolor_ticks` | checkbox | True | only when `show_ticks` | Turn the marks inside the warning zone that colour too. |

**Shape**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `shape` | select | 1 | options: 0=Custom, 1=L-Bend, 2=Straight, 3=45° Bend, 4=J-Hook | Pick a ready-made shape (straight, L-bend, 45°, or the curved J-hook tacho) sized to fit the box, or Custom to draw your own path. |
| `orientation` | select | 0 | options: 0=Top-Left / Horizontal, 1=Top-Right / Vertical, 2=Bottom-Left, 3=Bottom-Right | Which way the bar runs and fills. |
| `corner_radius` | stepper | 40 | 0..400; only when `shape=1,3` | How big the rounded corner of the bend is (or the bevel, on the 45° shape). |
| `hook_angle` | stepper | 120 | 30..200 step 5; only when `shape=4` | J-hook shape only: how far the curve sweeps. 90 is a quarter turn, 180 a half turn. |
| `smooth` | checkbox | False | only when `shape=0` | Custom shapes only: smooth your drawn points into a flowing curve. |

**Ticks**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_ticks` | checkbox | False |  | Show marks along the bar. |
| `major_tick_step` | number | 0 | only when `show_ticks` | How far apart the biggest marks are, in reading value. |
| `minor_tick_step` | number | 0 | only when `show_ticks` | How far apart the smaller marks are. 0 turns them off. |
| `major_tick_width` | stepper | 3 | 0..40; only when `show_ticks` | How thick the biggest marks are. |
| `tick_width` | stepper | 2 | 0..40; only when `show_ticks` | How thick the smaller marks are. |
| `major_tick_len` | stepper | 16 | 0..100; only when `show_ticks` | How long the biggest marks are. |
| `tick_len` | stepper | 10 | 0..100; only when `show_ticks` | How long the smaller marks are. |
| `major_tick_color` | color | #FFFFFF | only when `show_ticks` | Colour of the biggest marks. |
| `tick_color` | color | #9E9E9E | only when `show_ticks` | Colour of the smaller marks. |

**Numbers**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `show_labels` | checkbox | False | only when `show_ticks` | Show numbers next to the biggest marks. |
| `label_font` | font |  | only when `show_labels` | Text style and size of those numbers. |
| `label_color` | color | #FFFFFF | only when `show_labels` | Colour of those numbers. |
| `label_gap` | stepper | 14 | -200..200; only when `show_labels` | How far the numbers sit from the bar. |
| `tick_label_divisor` | stepper | 1 | 1..100000; only when `show_labels` | Shrink the numbers by dividing them (1000 shows 7000 as '7'). |

**Behaviour**

| field | type | default | constraints | what it does |
|---|---|---|---|---|
| `smoothing_ms` | slider | 90 | 0..500 | Smooths out jumpy readings. Higher is smoother but slower to react; 0 shows the raw value. |

