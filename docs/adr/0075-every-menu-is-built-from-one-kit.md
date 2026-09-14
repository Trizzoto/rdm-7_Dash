# ADR-0075: Every menu on the glass is built from one kit

Date: 2026-09-14
Status: Accepted
Repos: RDM-7_Dash (`main/ui/kit/`, `main/ui/ui_theme.c`, `main/ui/theme.h`,
`main/ui/menu/main_menu.c`, `main/ui/settings/device_settings.c`,
`main/ui/screens/ui_Screen3.c`, the secondary screens and modals)
Follows ADR-0039 (Setup grouped by the question you arrived with).
Prompted by the owner, looking at the tap-for-menu card: *"that feels pretty
outdated now"* — six mockups were drawn (scratch HTML at 800×480), and he
picked two: a tile launcher (C) and a bottom dock (E). *"Apply this style to
every single menu… generic so every menu looks the same… in future we could
always do light and dark modes."*

## What was there

- A tap on the dashboard showed a blue **Menu** pill and two blue arrow
  buttons in the top-right corner. Menu opened a 340×380 card holding a
  Layout dropdown, a Splash dropdown and a **Device Settings** button.
- Device Settings was one scrolling 760×440 panel: three rows of 232×110
  cards (ADR-0039's groups) and four full-width action buttons.
- Every screen and popup styled itself inline — the same eight
  `lv_obj_set_style_*` lines per button, repeated a few hundred times — in a
  palette of mid greys with LVGL blue as the accent and a different title
  colour per section.
- Colours went through `THEME_COLOR_*` (≈1,400 uses, 12 literal hexes), but
  those were compile-time constants, and dashboard widgets used the same
  names — so recolouring the menus would have recoloured people's layouts.

## Decision

### 1. The tokens are a runtime palette; widgets get their own

`THEME_COLOR_*` now read `ui_pal`, a `ui_palette_t` chosen by
`ui_theme_set()`. Dark and light are two tables in `ui_theme.c`; the legacy
names map onto semantic slots (`surface`, `card`, `raised`, `line`,
`accent`, `danger`…). Because every menu already used the names, every menu
— the setup wizard included, without a line of it changing — took the new
colours at once.

Dashboard widgets (`widget_bar/indicator/panel/rpm_bar/warning`, `main.c`)
moved to `WIDGET_COLOR_*`, frozen at their old values: a layout's default
colours are part of the layout, must match Studio's WASM renderer, and must
not follow a menu theme.

The light table exists and is not offered yet. It is there so the swap is
proven and the next person picks colours rather than plumbing. A screen
already on the glass keeps its colours until rebuilt; menus are built on
open, so in practice that is the next tap.

### 2. One kit of parts (`main/ui/kit/ui_kit.h`)

`uk_screen`, `uk_bar` (RDM logo, caps title, status strip, Close/Back),
`uk_body`, `uk_grid` (tiles auto-place in reading order), `uk_tile` (icon,
name, optional description, a status line, a live badge), `uk_card`,
`uk_section`, `uk_row`, `uk_btn` (neutral / primary / danger / ghost / on),
`uk_popup` (modal card over a backdrop that eats taps), `uk_toast`, and
`uk_style_*` for the stock slider, switch, dropdown, text area, message box,
keyboard and list.

All looks are shared `lv_style_t`s rebuilt from the palette. Parts start from
`lv_obj_remove_style_all()`, so the LVGL default theme contributes nothing
and a part looks the same wherever it is used. Restyle helpers add on top of
the default theme instead, because those widgets rely on its part geometry —
and they only win where the caller has not set a local style, which is why
migrating a screen means deleting its inline style calls, not just adding a
helper.

### 3. Type and icons that fit the flash

- Titles are **Barlow Semi Condensed SemiBold**, subset to Latin basic
  (14 KB, `main/embed/fonts/barlow_ui.ttf`) and rasterised by
  `lv_tiny_ttf` at 26/21/16 px; body text stays on the built-in Montserrat.
  Montserrat has **no `·` or `—` glyph**, so the tile status separator is a
  drawn 3 px dot, and neither character may appear in body strings.
- The line icons from the mockups are baked by `tools/ui_kit/make_icons.py`
  (headless Edge, 8× supersampled) into 39 × 2 sizes of 4-bit **alpha**
  images (~29 KB). Alpha images take their colour from `img_recolor`, so one
  set serves every palette.
- The whole change cost ~61 KB of app partition (3 % left).

### 4. Launcher and dock

- **Dock** (`ui_Screen3.c`): tap the dashboard and a bar rises along the
  bottom for 10 s — ◀ layout name ▶ with a pip per layout, a brightness
  slider, **Dim**, **Rec** (shows elapsed time while recording), **Setup**.
  Any touch on it restarts the countdown. It is opaque on purpose: LVGL stops
  redrawing at the topmost opaque object, so gauges updating beneath don't
  re-composite through it. `ui_Menu_Button` now points at the dock so older
  "hide the menu button" callers still clear the chrome.
- **Dim** replaced the mockup's Night button. Night mode is compiled out
  (`NIGHT_MODE_DISABLED`), and a button that does nothing is worse than no
  button; Dim drops to the dimmer's dim level and back to exactly what it was.
- **Launcher** (`main_menu.c`): eight tiles, each with a live status —
  Layouts (hero), Screen, Recording, Live data, Channels, Your car, Connect,
  This dash. The bar's status shows the CAN rate with a dot that goes grey
  when frames stop.
- ADR-0039's three groups became three **pages** (Your car / This dash /
  Connect) instead of one scroll. The four action buttons became tiles on
  This dash; the two resets are danger tiles and still ask first.
- **Layouts** page replaces both dropdowns: tap a layout to drive with it;
  pick what shows at start-up, including "None".
- Close returns to the dashboard without rebuilding it unless a page that
  shapes the dashboard (Your car, Channels) was opened — then it saves and
  rebuilds exactly as Device Settings' Close always did.

### Screen lifetime rules this relies on

- Menu screens replace each other through `main_menu_swap_to()`, which
  deletes the outgoing menu screen (never the dashboard).
- The settings session (its 1–2 s status timers and popup teardown) attaches
  to whichever menu screen is current. The new screen attaches before the
  old one is deleted, so the delete handler ignores any screen that is no
  longer the attached one.
- Picking a layout from a menu frees the dashboard it replaces; the load
  animation's auto-delete only frees the *active* screen, which there is the
  menu.
- Leaving for a full-screen tool (diagnostics, the wizard) deletes the menu
  screen instead of leaving it and its timers alive behind the dashboard.

## Options considered

- **Keep constant tokens, restyle values only.** Cheapest, but it recolours
  dashboard widgets and closes the door on a light theme.
- **A per-file compile flag that remaps the tokens for menu files.** No file
  edits, but the same name meaning two things depending on the translation
  unit is a trap for the next reader.
- **LVGL's default theme, re-coloured.** It still draws its own shadows,
  transitions and paddings, and gives nothing for tiles, bars or popups.
- **Precompiled bitmap Barlow fonts.** Crisper at small sizes but needs
  `lv_font_conv` (not installed) and ~3× the flash for three sizes;
  tiny_ttf is already in the build for layout fonts.

## Consequences

- Good: a new menu is a few kit calls and looks like every other menu.
- Good: a light theme is a table plus a toggle.
- Good: Device Settings lost ~700 lines of repeated inline styling.
- Bad: tiny_ttf rasterises each title glyph on first draw (cached after);
  the first open of the launcher does that work.
- Bad: 3 % of the app partition left.
- Neutral: the setup wizard takes the palette but keeps its own structure;
  its file carried someone else's uncommitted work at the time.
- Neutral: the on-device layout editor (`edit_mode.c`, `inspector.c`,
  `config_modal.c`) is disabled and was not migrated.

## References

- Kit: `main/ui/kit/ui_kit.{h,c}`, `main/ui/kit/uk_icons.{h,c}`,
  `tools/ui_kit/make_icons.py`
- Palette: `main/ui/theme.h`, `main/ui/ui_theme.c`
- Launcher and pages: `main/ui/menu/main_menu.c`,
  `device_settings_open_page()` in `main/ui/settings/device_settings.c`
- Dock: `_dock_create()` in `main/ui/screens/ui_Screen3.c`
- Related ADRs: 0030 (live data and logging together), 0039 (Setup groups)
