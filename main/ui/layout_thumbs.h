/*
 * layout_thumbs.h — small pictures of each dashboard layout (ADR-0076).
 *
 * A thumbnail is the dashboard as it last looked on the glass, scaled down
 * from the panel's own framebuffer — no second render, so it costs one
 * box-filter pass. It is taken when the dashboard is tapped (just before the
 * dock appears, so the dock is not in it), kept in PSRAM, and written to
 * /lfs/thumbs once per layout load so the Layouts page has pictures after a
 * restart too. A layout that has never been on screen has no picture.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAYOUT_THUMB_W 224
#define LAYOUT_THUMB_H 134

/** The dashboard for @p name was just (re)built: its next capture must be
 *  saved to flash, whatever the RAM copy says. */
void layout_thumbs_mark_loaded(const char *name);

/** Capture the active layout from the framebuffer if the dashboard alone is
 *  on the glass and the copy is missing or old. Cheap to call on every tap. */
void layout_thumbs_capture_if_due(const char *name);

/** The picture for @p name, from RAM or flash; NULL when there is none. The
 *  descriptor stays valid until layout_thumbs_forget(@p name). */
const lv_img_dsc_t *layout_thumbs_get(const char *name);

/** Drop the picture for a deleted or renamed layout (RAM and flash). */
void layout_thumbs_forget(const char *name);

#ifdef __cplusplus
}
#endif
