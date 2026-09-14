# ADR-0076: Live graphs, layout pictures, and a way out of an update

Date: 2026-09-14
Status: Accepted
Repos: RDM-7_Dash (`main/ui/screens/ui_graphs.c`, `main/ui/layout_thumbs.c`,
`main/ui/menu/main_menu.c`, `main/net/ota_update_dialog.c`,
`main/ui/settings/device_settings.c`, `main/ui/screens/ui_Screen3.c`,
`main/layout/layout_manager.c`)
Follows ADR-0075 (one kit for every menu). Prompted by the owner:
*"Bring back check updates and if an update fails … have a cancel or go
back button"*, *"under live data can we have live graphs … like lambda and
lambda target … a live tuning capability"*, and a screenshot of the Layouts
mockup: *"when you open dash layouts I want them visible in small versions"*.

## 1. Check for updates, and leaving a failed one

The on-glass "Check updates" button went missing in an earlier settings
redesign; updates were reachable only from the web editor.

- **This dash → Updates** tile runs `ota_update_dialog_check_now()`.
- No STA connection: it says so at once ("Updates download over the
  internet…") with Close / Try again, instead of a 30 s spinner that ends in
  "check failed" on a hotspot-only dash.
- A failed check shows **Close** and **Try again**.
- Cancelling a check bumps a token; the check task's late answer is dropped
  instead of popping a dialog the person walked away from.
- A failed **install** used to bring back only Install, with Later and
  Skip still hidden — the small X was the one way out. It now shows Close,
  Try again and Skip, hides the dead progress bar, and says plainly that
  nothing changed.

## 2. Live graphs

Launcher → **Live data** opens a graph screen (the min / max table is one
tap from it).

- Up to **four channels**; window **10 / 30 / 60 s**; Pause.
- **Same unit, same axis.** Lambda and target lambda share one scale so they
  can be compared; a different unit gets its own, and the axis numbers take
  that trace's colour when two scales share the plot.
- **Round axis steps** (1, 2, 2.5, 5 × 10ⁿ), never below zero when every
  reading is positive. A scale grows at once and shrinks only after the
  tighter one has held for 3 s, so the axis doesn't twitch.
- A channel whose id contains `target` is drawn **dashed** over the value it
  chases.
- "Add a channel" offers **tuning pairs** — lambda + target, wideband +
  target, AFR + target, boost + target, fuel trims, knock + timing, RPM +
  throttle — only when this car has both channels.
- A 60 s window squeezes 1,200 samples into ~470 columns by drawing each
  column's low and high **in the order they happened**, so a lean spike
  survives instead of being averaged away.
- Picks are remembered until restart.

### What it took to make it cheap

Measured on the bench dash with the simulator's steep RPM sawtooth (a worst
case):

| Version | CPU on the graph screen |
|---|---|
| LVGL line primitives, whole plot invalidated at 10 Hz | ~63 % (paused: 7 %) |
| Hand-drawn RGB565 buffer, cleared every frame | ~80 % |
| …erasing only last frame's trace pixels | ~68 % |
| …invalidating 12 strips, each only the rows its traces touched | ~55 % |
| …redraw paced by window (10 Hz at 10 s, 5 Hz at 30 s, 4 Hz at 60 s) | 57 % / 39 % / 38 % |

The lessons, both about PSRAM rather than drawing:

- **Writing a 424 KB buffer one pixel at a time took ~77 ms**; the traces
  themselves took 3–4 ms. The plot is cleared once; each frame restores only
  the columns and rows the traces wrote last time (grid included).
- **LVGL copying a large image area through PSRAM each frame** was most of
  what remained. So only the changed band of each of 12 vertical strips is
  invalidated.

Traces are anti-aliased column spans: each pixel column of a trace is
written exactly once with fractional coverage at both ends (no gaps or
doubled joins), and steep strokes lend coverage to their neighbour columns
so they are as heavy as flat ones.

**Sampling runs by the clock.** LVGL skips timer runs it missed rather than
catching up, so a slow frame dropped samples and stretched the time axis
(the simulator's 15 s cycle showed as 12 s). Every 50 ms that passed now gets
its sample.

## 3. Layout pictures

The Layouts page is a grid of picture cards: the one in use first, with a red
edge and an **In use** tag; a "More layouts from RDM Studio or the web
editor" card last; the start-up screen chooser moved into the top bar.

- A picture is the dashboard as it last looked, **box-filtered from the
  panel's own framebuffer** (224 × 134) — no second render.
- It is taken on a **tap on the dashboard, just before the dock appears**,
  and only when nothing is drawn over it (no popup, dock or wizard), at
  least 3 s after the layout loaded, at most every 30 s.
- Kept in PSRAM, and written to `/lfs/thumbs/<name>.thm` (RDMT header +
  RGB565) once per layout load, 1.5 s after the tap so the write doesn't
  land in the tap itself. It survives restarts.
- A layout that has never been on screen shows a placeholder saying so.
  Rendering one off-screen would build a second dashboard and clobber the
  live signal registry.
- `layout_manager_delete` removes the picture with the layout (file only —
  it can run off the LVGL task).

## 4. The dock's layout pills and drawer (same day, follow-up)

The owner, on the dock: the name should size itself, show "a number … like 1
larger and then of 9", and be its own pill that opens a layout drawer while
the arrows stay for quick switching.

- The layout group is three pills: **◀**, **name**, **▶**. The name pill is as
  wide as the name: the 26 px face when it fits in 180 px, the 21 px face
  when it doesn't, then one line cut with "…". File names show as words
  (`track_night` → TRACK NIGHT). 180 px is what keeps the widest dock on the
  screen; the brightness slider takes the rest, never under 150 px.
- Under the name, **3 of 9**: the position in the switcher cycle, the number
  a size up. Hidden with one layout.
- Tapping the name opens the **layout drawer**: a sheet along the bottom with
  the Layouts page's picture cards (one shared builder,
  `main_menu_layout_card`), scrolled so the one in use is in view. Tap a card
  to switch; tap the X or the dashboard above to close. The auto-hide timer
  doesn't run while it is open.

### Internal RAM is WiFi's

Adding the drawer's name table put the dash into a **boot loop**: `esp_wifi_init`
failed with `ESP_ERR_NO_MEM` (37,579 B internal DMA free, largest block
19,456 B) and its `ESP_ERROR_CHECK` aborted, on every layout. This session's
menu code had put ~8 KB of static arrays in internal DRAM (layout name
tables, graph erase tables, thumbnail slots, the kit's styles). All of them
are now `EXT_RAM_BSS_ATTR`; WiFi starts with 45,975 B free. The margin is
thin, so **any new static array in UI code goes in PSRAM**, and a separate
task was raised to stop a failed WiFi start from aborting boot.

## Options considered

- **LVGL chart widget** for graphs: one Y axis per side, no shared-unit
  grouping, and the same per-segment line cost.
- **Sweep ("oscilloscope") drawing** so only a narrow strip changes: the
  cost here is PSRAM rows, and a tall narrow strip is still every row.
- **Bigger draw buffers / direct mode** for the graph: display-wide changes
  measured and tuned for dashboards (see main.c); not worth risking for one
  screen.
- **Rendering layout pictures off-screen** from the JSON: accurate for any
  layout, but it rebuilds widgets and re-registers signals.

## Consequences

- Good: lambda vs target, boost vs target on the glass, no laptop.
- Good: the Layouts page shows what each layout looks like.
- Good: a failed update always has a way out.
- Bad: the 10 s window still costs ~55 % CPU on a steep, full-height trace.
  The dashboard isn't running then, but it is the heaviest menu screen.
- Bad: a picture is only as fresh as the last tap on that layout.
- Neutral: this bench dash's USB-serial bridge drops 256-byte bursts; test
  tooling now paces large frames at 64 B / 5 ms.

## References

- Code: `main/ui/screens/ui_graphs.c`, `main/ui/layout_thumbs.c`,
  `ota_update_dialog_check_now()` in `main/net/ota_update_dialog.c`
- Related ADRs: 0030 (live data and logging together), 0075 (the kit)
