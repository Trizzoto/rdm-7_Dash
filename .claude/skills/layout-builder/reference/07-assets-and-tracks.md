# Assets — images, fonts, tracks

A layout references assets **by name**; the bytes live on the dash's LittleFS,
not in the layout JSON. So a layout that names an asset the target dash doesn't
have will render the widget's *name* as a fallback instead of the graphic.
Upload the asset first, then apply the layout.

Read `05-firmware-quirks.md` "Design philosophy" before adding any image: the
house style is procedural. Images are for **small icons and per-tick sprites**,
never gauge surfaces or backgrounds.

## Images — `.rdmimg`

| call | notes |
|---|---|
| `POST /api/image/upload?name=<name>` | body is the **raw `.rdmimg` bytes**, not multipart |
| `GET /api/image/list` | what's installed |
| `GET /api/image/data?name=<name>` | read one back |
| `POST /api/image/delete` | remove one |

Pipeline:

```bash
python tools/png_to_rdmimg.py in.png out.rdmimg
curl --data-binary @out.rdmimg "http://$DASH/api/image/upload?name=ic_oilp"
```

Then reference it as `"image_name": "ic_oilp"` (no extension). The format is a
12-byte header + RGB565 + alpha.

**Sizing discipline.** Decoding needs a *contiguous* PSRAM block, and once a
layout is loaded the largest free block is often under 0.5 MB. A big image
doesn't error — it silently fails to paint. Keep icons ≤ ~40 px.

## Per-tick images (the one gauge-surface exception)

`arc` and `meter` can stamp a small sprite at every tick, rotated to that tick's
radial angle, and **bake the result into the static-tick snapshot** — so it
costs nothing at runtime. That's what makes it acceptable where a dial-face
image isn't.

- Author the PNG **vertical, outer end at the top**. Include a soft glow halo in
  the PNG itself if you want neon.
- Convert + upload as above, then set `major_tick_image_name` /
  `mid_tick_image_name` / `minor_tick_image_name`.
- Size with `*_tick_image_scale` (percent, 100 = native px). Per-tier extras:
  `*_tick_image_offset`, `*_tick_image_opa`, `*_tick_image_recolor`,
  `*_tick_image_recolor_opa`.

Try the **drawn** tick outline/glow first (`tick_outline_strength` /
`_color` / `_fade`) — it needs no asset at all and reads nearly as well.

## Fonts — TTF

| call | notes |
|---|---|
| `POST /api/font/upload` | **whole-buffers the file** (unlike images, which stream) — keep TTFs small |
| `GET /api/font/list` · `GET /api/font/data` · `POST /api/font/delete` | |

Reference a font as `"Family:size"` — e.g. `"Fugaz:28"`. The legacy
`"fugaz_28"` form still parses. The cache holds 8 families and 32 size
instances; asking for a family that isn't installed falls back silently.

## Tracks — `.rdmtrk` (for the `track_map` widget)

| call | notes |
|---|---|
| `POST /api/track/upload?name=<name>[&rev=<n>]` | raw `.rdmtrk` body; re-uploading the same `name` **replaces** it |
| `GET /api/track/list` | `[{name, track, points, version, size}]` — `name` is the **filename a widget references**, `track` is the human-readable circuit name from inside the file |
| `GET /api/track/data?name=<name>` | read one back |
| `POST /api/track/delete` | remove one |

Then set `"track_asset": "<name>"` on the widget.

**The file format** (written by RDM Studio's GPS workspace → Tracks → Shape;
`track_map_geo.h` is the spec):

```
off  size  field
  0     6  magic "RDMTRK"
  6     1  u8   version (the loader REJECTS unknown versions)
  7     1  u8   flags — bit0 = the shape closes on itself
  8    32  char name[32], NUL-padded — the circuit name shown under the map
 40     4  i32  lat0_1e7   projection origin (bbox centre)
 44     4  i32  lon0_1e7
 48     2  u16  n_points
 50     2  u16  reserved (pad — keeps the point array 4-aligned)
 52   8·n  i32 lat_1e7, i32 lon_1e7  × n_points
```

Max **400 points**; header is 52 bytes. Tracks can also arrive over CAN from
another RDM device (`rdm_bus`), which is what `&rev=` versions.

**What `track_map` needs to actually show a car:** live `gps_latitude` /
`gps_longitude` channels (an RDM GPS on the bus). Override `lat_channel` /
`lon_channel` only if the position comes from something else. With no fix the
outline still draws and the marker dims. On the bench, inject the two channels
to move the dot.

**`track_map` ignores `config.night`** — see 05.

## Layout portability

- Asset **names** travel in the layout; asset **bytes** don't.
- CAN **decode** doesn't travel either — it lives in the device's channel
  registry (`channels.json`). See 04.
- A `.rdm` export bundle *can* carry `channels.json` alongside the layout, so a
  shared cluster arrives with its bindings. Check before assuming a hand-written
  layout JSON will bind on someone else's dash — it won't, unless their channels
  already use the same signal names.
