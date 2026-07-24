# Animation widget (`anim`) — EXPERIMENTAL

Signal-driven image-sequence widget, added 2026-07-16. Deliberately built as a
self-contained, removable module in case the render/PSRAM cost isn't worth it.

## What it is

- N `.rdmimg` frames (`<prefix>_0` … `<prefix>_N-1` on LittleFS), all
  pre-loaded into PSRAM at widget create via the shared refcounted
  `rdm_image_load()` cache. A frame change is `lv_img_set_src()` between
  resident descriptors — one invalidate, no decode, no allocation.
- **Scrub mode** (default): frame index follows the bound channel across
  `[range_min, range_max]` like a needle. No timer; nothing redraws unless the
  frame actually changes.
- **Trigger mode**: frame 0 idle; at/above `threshold` all frames loop at
  `loop_fps` (LVGL timer, paused while inactive).
- `hyst_pct` damps frame-boundary / threshold chatter.
- Editor: "Upload animation…" on the widget slices a GIF (WebCodecs,
  Chromium-only), video (`<video>` seek), or a multi-select of stills into
  frames client-side, converts to RDMIMG and uploads via `/api/image/upload`.
  The device never decodes GIF/video.

## Cost model (why it might get pulled)

- PSRAM: `w × h × 3 bytes × frames` while a layout containing the widget is
  loaded (e.g. 200×200 × 10 frames ≈ 1.2 MB). Frames occupy slots in the
  24-entry image cache (`widget_image.c`); overflow loads unshared.
- Flash: same bytes again on LittleFS.
- Render: a frame change invalidates the widget rect — at scrub rates driven
  by a fast signal this behaves like a value-driven image swap, comparable to
  a fill-arc redraw of the same area. Watch panel-scan FPS with a big widget
  on a fast channel (see `layout-fps-cost-model` notes / `tools/_fpsctl.py`).

## Kill switch (no removal)

`RDM_WIDGET_ANIM_ENABLED` (default 1) in `main/widgets/widget_anim.h` — set to
0 there or build with `-DRDM_WIDGET_ANIM_ENABLED=0`. widget_anim.c compiles
empty and the factory case in layout_manager.c disappears; layouts containing
an `anim` widget drop it with a log line (same path as an unknown type).
Everything else that mentions WIDGET_ANIM is inert data (enum entry, name +
constraints rows, generated inspector table) and keeps compiling. The editor
palette still offers the widget (schema-driven) — remove the schema entry too
(below) if the flag-off state should be user-visible.

## Full removal checklist

1. Delete `main/widgets/widget_anim.c` + `widget_anim.h`; remove
   `"widgets/widget_anim.c"` from `main/CMakeLists.txt` SRCS.
2. `main/layout/layout_manager.c`: remove the `#include "widget_anim.h"`, the
   `"anim"` case in `_type_from_str()`, and the `#if RDM_WIDGET_ANIM_ENABLED`
   factory case.
3. `main/widgets/widget_types.h`: remove `WIDGET_ANIM` from the enum.
   `widget_types.c`: remove the include, the `"anim"` name row, the
   constraints row, and the three `case WIDGET_ANIM:` lines
   (signal-name / signal-index / channel-id helpers).
4. `main/ui/dashboard.c`: remove `WIDGET_ANIM` from the `is_decoration` check.
5. `schema/widgets.schema.json`: delete the `"anim"` widget entry, then rerun
   BOTH codegens (`python tools/codegen_widget_defs.py main/web/index.html`,
   `python tools/codegen_widget_inspector.py`). Also remove `"anim"` from
   `WIDGET_NAME_TO_ENUM` and `anim_frames` from `WEB_ONLY_TYPES` in
   `tools/codegen_widget_inspector.py`.
6. `main/web/index.html` (hand-written parts, all OUTSIDE the codegen block):
   `PALETTE_ICONS.anim`, `'anim'` in `NO_RULES_TYPES`, the `case 'anim'` in
   `_pvWidget` + `_pvAnim()`, the `anim_frames` branch in `renderFieldHTML`,
   and the "Animation frame slicer" section (`_animFramesFromGif` /
   `_animFramesFromVideo` / `_animFramesFromStills` / `openAnimUploadDialog` /
   `_animUploadFrames` + the `ANIM_*` consts).
7. Frames already uploaded to devices are ordinary images — delete via the
   image manager if wanted.

No `LAYOUT_SCHEMA_VERSION` bump was made for this feature (additive type;
old firmware simply drops unknown types), so removal needs no migration.
