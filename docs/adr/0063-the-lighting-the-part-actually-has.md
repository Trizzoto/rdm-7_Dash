# ADR-0063: The lighting the part actually has, and a warning that does not hide the button

Date: 2026-09-03
Status: Accepted — built 2026-09-03
Repos: rdm7-desktop (keypad workspace in `src/tauri-overlay.html`,
`tools/check_keydemo.js`)
Follows: ADR-0062 (the picture is the simulator, and a control demonstrates
itself)
Hardware: `rdm7-desktop/docs/BLINK_MARINE_PKP2200_CANOPEN_2026-08-28.md`

## Context

With the keypad page telling the truth about button state (ADR-0062), the
lighting panel next to it was still describing a keypad we had invented.

**What Studio offered:** *Buttons — day* (0–63), *Buttons — night* (0–63),
*Legend backlight* colour, *Backlight level* (0–63), and a Day/Night preview.

**What a PKP-2200 has**, from the bench log and the manuals:

| Object | What it is |
|---|---|
| `2003h:05` | Default LED brightness — **one** level, every ring |
| `2003h:04` | Default backlight colour — one of nine, whole keypad |
| `2003h:06` | Default backlight brightness |
| `500h + node` | The runtime backlight PDO: byte0 level, byte1 colour |
| `200h + node` | The runtime ring PDO: red, green, blue **bitmaps** |

So there is no night value. There never was. `brightNight` was stored, was
offered as a slider, dimmed the picture — and was never written to anything,
because there is no object to write it to. The setup file has always sent
exactly three lighting values, and the panel claimed four.

Two more things were wrong in the same place:

- **The legend was drawn as near-white ink** (`#F0F4F8`) that only tinted
  toward the backlight colour at high levels, and its opacity was computed from
  the *ring* brightness. On the part, a legend is a window in a black cap lit by
  a separate lamp: unlit it is a dark silhouette, and the ring's brightness has
  nothing to do with it.
- **A warning replaced the button's own colour.** Latch a key on — it goes
  green. Trip a warning on it — it blinks red against *dark*, and while it is
  complaining you can no longer tell whether the thing is even switched on. On a
  keypad whose whole job is telling you what is on, that is the worst moment to
  stop saying so.

## Decision

**Three lighting settings, and they are the three objects.** Button brightness
(2003h:05), legend backlight colour (2003h:04), legend brightness (2003h:06).
The panel and the setup file cannot disagree because they are the same three
values. `brightNight` is gone; `brightDay` is `ledBright`, migrated on load.

**Preview is a room, not a setting.** Daylight / In the dark stands the picture
on a near-black stage. A backlight colour is a decision you cannot make in
daylight, and the honest way to help is to turn the lights off — not to invent
a second brightness for the keypad to store.

**One rule for a lit legend**, `kpLegendLook(bright, colour)`, used by the
Design picture and the boot preview: dark grey silhouette at zero, the
backlight's own colour at full, opacity from the backlight alone. Above about a
fifth of the level, the picture also draws a blurred copy of the legend behind
it, which is what a backlit window actually does to the eye.

**A warning is a layer over the button, not a replacement for it.** When a key
is on underneath, the picture stacks two rings: the button's own colour, steady,
and the warning over the top.

| Warning blink | What you see on a key that is already on |
|---|---|
| Slow | alternates warning colour ↔ the button's own colour |
| Fast | the same, faster |
| Solid | the warning holds, and steps aside for ~150 ms every 3 s so the button's colour shows through |

On a key that is *off* underneath, there is no second ring and a warning blinks
against the dark, exactly as before. The solid case needed its own answer
because a solid alarm has nothing to blink: `kp-alert-peek` is a 3-second
animation that is transparent for the first 5% of its cycle. It respects
`prefers-reduced-motion` alongside the other two.

## Consequences

`ledBase(i)` is now the single answer to "what is the button itself showing",
separate from `ledActive`/`ledColor`, which answer "what is the ring showing"
including any warning. The ring signature that decides when to redraw includes
both, so a change in the layer underneath still triggers a repaint — otherwise a
key that latched while complaining would keep the stale colour under the alarm.

A warning demo no longer overrides the ring at all: it drives the *channel*, and
`kpActiveAlert` lays the warning over whatever the button is really doing. That
is what makes the layering visible while you are configuring the warning, which
is the only time a warning fires (ADR-0062).

The setup file is unchanged on the wire — it already wrote these three objects.
What changed is that the panel stopped offering a fourth.

`tools/check_keydemo.js` grew to 73 checks. The new ones assert there is one
ring brightness and that the preview room does not change it; that an unlit
legend is a dark silhouette and a lit one is the backlight's colour; that
turning the rings down does not dim the legends; and that a latched key keeps
its colour underneath a slow, fast or solid warning, on a plain key and on a
cycle key parked on a lit position.
