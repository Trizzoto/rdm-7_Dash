# ADR-0070: The image cap is one screenful, and the dash is the one who says so

Date: 2026-09-13
Status: Accepted — built and flashed to the Lume 2026-09-13
Repos: RDM-7_Dash (`main/system/screen_config.h`, `main/net/web_server_assets.c`,
`main/net/serial_commands_internal.h`, `main/net/web_server_system.c`,
`main/web/index.html`), RDM-Lume-149 (the same five), rdm7-desktop
(`src/firmware-base.html`, via `tools/sync_firmware.py`)

## Context

Uploading a full-screen background to a 1920×720 Linux dash failed. Studio
said "Upload failed: Invalid content length" and nothing else.

The dash keeps an image as `.rdmimg`: three bytes a pixel — RGB565
little-endian then one alpha byte — behind a 12-byte header. A full 1920×720
screen is therefore **4,147,212 bytes**. The upload handler capped at
**1,228,800**, so anything past about 54% of the screen's linear size was
refused.

Reproduced against the connected dash (`RDM-494D-B781`):

| Body | Bytes | Reply |
|---|---|---|
| 1024×390 | 1,198,092 | 200 OK |
| 1024×410 | 1,259,532 | 400 Invalid content length |
| 1920×720 | 4,147,212 | 400 Invalid content length |

The cap was `#define IMAGE_MAX_SIZE (1200 * 1024)` — 800×480×3 rounded up,
written for the 7 inch panel. The comment directly above it already claimed it
was `SCREEN_W * SCREEN_H * 3`; only the literal had failed to follow the
hardware. It had been copied into `serial_commands_internal.h` as well, so the
USB path carried the same stale number independently.

It was not only the Linux flagship. **720×720 needs 1,555,212 B**, so the round
dash could not take a full-screen image either, and nobody had noticed because
nobody had tried.

Storage was never the constraint: the Lume reported 7.88 MB free of 9 MB.

## Decision

### 1. The cap is derived, and lives in one place

`SCREEN_IMAGE_MAX_BYTES` moves to `main/system/screen_config.h`, next to
`SCREEN_W`/`SCREEN_H` that define it:

```c
#define SCREEN_IMAGE_MAX_BYTES \
    ((((size_t)SCREEN_W * (size_t)SCREEN_H * 3u) + 12u) > (1200u * 1024u) \
        ? (((size_t)SCREEN_W * (size_t)SCREEN_H * 3u) + 12u)              \
        : (1200u * 1024u))
```

Both upload paths now reference it, so they cannot drift apart again — a file
that lands over WiFi has to be accepted over the cable.

The old 1200 KB stays as a **floor**, so no upload that fit on a small panel
stops fitting:

| Panel | Was | Now |
|---|---|---|
| 800×480 | 1200 KB | 1200 KB |
| 480×480 | 1200 KB | 1200 KB |
| 720×720 | 1200 KB | 1519 KB |
| 1024×600 | 1200 KB | 1800 KB |
| 1280×800 | 1200 KB | 3000 KB |
| 1920×720 | 1200 KB | 4050 KB |

### 2. The dash reports its limits; the editor stops guessing

`/api/device/info` gains `limits: { image_max, layout_max }`.

The editor had been carrying its own copy of both numbers — a hardcoded
1200 KB for images, and `RDM_LAYOUT_MAX_BYTES = 32768` with a "must match the
firmware" comment, which is a comment admitting the problem. A number the
firmware owns should come from the firmware. An older Studio ignores the new
object; a newer Studio against older firmware falls back to 1200 KB, because
that is what such a dash actually enforces.

### 3. A control that cannot ask for the impossible beats an error afterwards

In the image resize dialog the **slider's maximum is now the cap**, worked back
from bytes to a percentage (bytes go as the square of the scale, so the ceiling
is `sqrt(capPixels / originalPixels)`). When the cap is what binds, the dialog
says so in words rather than leaving a red number to be interpreted:

> The dash keeps an image at three bytes a pixel, so the most it will take is
> 4.0 MB — one whole 1920 × 720 screen. This picture reaches that at 93%, so
> the slider stops there.

The size readout also colours against *this dash's* cap instead of a threshold
written for the 7 inch panel.

`doImageUpload()` keeps a backstop check, because layout transfer and the
animation importer call into it without going through the dialog. And if
"Invalid content length" ever does come back, it is translated into both
numbers instead of passed through.

## Consequences

- A full-screen background on the flagship costs 4.1 MB of a 9 MB partition.
  One fits comfortably; **two do not**. The firmware's free-space pre-check is
  now the real guard, and it reports KB free in its error, which the old cap
  never let anyone reach.
- Raising a cap does not raise RAM use: uploads already streamed to flash in
  8 KB chunks, and the loader has no size limit of its own.
- The honest fix for the 4.1 MB is a compressed on-device format. The dash
  already vendors stb JPEG for the MJPEG preview, so accepting PNG/JPEG and
  decoding on arrival would cut a full-screen background to a few hundred KB.
  Not done here; this ADR only stops the dash refusing a picture it has the
  room for.

## Notes

The README's Lume build line is stale — it gives only
`-DRDM_BOARD_FBDEV=ON`, but this board runs DRM and needs
`-DRDM_BOARD_FBDEV=ON -DRDM_BOARD_DRM=ON`. `/dev/fb0` is fixed at 800×1280 by
U-Boot's handover buffer and cannot be resized, so an fbdev build cannot see
the 1920×720 panel at all. `linux/board/lume_start.sh` says this; the README
does not.
