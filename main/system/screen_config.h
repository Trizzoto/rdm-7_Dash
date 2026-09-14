/**
 * screen_config.h — Screen dimension and hardware profile abstraction.
 *
 * Dimensions and shape are selected via menuconfig (Kconfig):
 *   idf.py menuconfig → "RDM-7 Display Configuration"
 *
 * Supported profiles:
 *   800×480  rectangle  (default — 7" RGB LCD)
 *   480×480  rect/round (4" square/round)
 *   720×720  round      (round high-res)
 *
 * Usage:
 *   #include "screen_config.h"
 *   lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
 *   lv_coord_t abs_x = SCREEN_ORIGIN_X + center_x - (w / 2);
 */
#pragma once

#include <stddef.h>   /* size_t — SCREEN_IMAGE_MAX_BYTES */
#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Screen shape enum ────────────────────────────────────────────────── */

typedef enum {
    SCREEN_SHAPE_RECT  = 0,   /**< Rectangular / square display */
    SCREEN_SHAPE_ROUND = 1,   /**< Circular display (needs clip mask) */
} screen_shape_t;

/* ─── Compile-time screen dimensions (from Kconfig) ────────────────────── */

#if defined(CONFIG_RDM_SCREEN_480X480)
#define SCREEN_W    480
#define SCREEN_H    480
#elif defined(CONFIG_RDM_SCREEN_720X720)
#define SCREEN_W    720
#define SCREEN_H    720
#else /* CONFIG_RDM_SCREEN_800X480 (default) */
#define SCREEN_W    800
#define SCREEN_H    480
#endif

#if defined(CONFIG_RDM_SHAPE_ROUND)
#define SCREEN_SHAPE    SCREEN_SHAPE_ROUND
#else
#define SCREEN_SHAPE    SCREEN_SHAPE_RECT
#endif

/** Half-width / half-height — the origin offset for centre-based coords. */
#define SCREEN_ORIGIN_X   (SCREEN_W / 2)
#define SCREEN_ORIGIN_Y   (SCREEN_H / 2)

/** Biggest .rdmimg we will store: one full screen plus the 12-byte header.
 *  The format is 3 bytes a pixel — RGB565 little-endian then one alpha byte
 *  (LV_IMG_CF_TRUE_COLOR_ALPHA), see convertImageToRDMIMG() in web/index.html.
 *  An image is never usefully bigger than the screen it is drawn on, and the
 *  editor produces exactly this at 100% of a full-screen picture.
 *
 *  This lived as a flat `1200 * 1024` in BOTH web_server_assets.c and
 *  serial_commands_internal.h, each with a comment already claiming it was
 *  SCREEN_W * SCREEN_H * 3 — true only for the 800x480 panel it was written
 *  on. On 720x720 (1,555,212 B) and on the 1920x720 flagship (4,147,212 B) a
 *  full-screen background was refused with "Invalid content length". It lives
 *  here now so the two upload paths cannot drift apart again: the same file
 *  has to be accepted over USB and over HTTP.
 *
 *  The old 1200 KB stays as a floor, so nothing that fit on a small panel
 *  before stops fitting now. */
#define SCREEN_IMAGE_MAX_BYTES                                          \
	((((size_t)SCREEN_W * (size_t)SCREEN_H * 3u) + 12u) > (1200u * 1024u) \
		? (((size_t)SCREEN_W * (size_t)SCREEN_H * 3u) + 12u)            \
		: (1200u * 1024u))

/* ─── Hardware profile struct ──────────────────────────────────────────── *
 *
 * Bundles all display geometry into one struct for runtime queries
 * (e.g., web API, layout validation).
 */

typedef struct {
    uint16_t       width;       /**< SCREEN_W  */
    uint16_t       height;      /**< SCREEN_H  */
    uint16_t       origin_x;    /**< SCREEN_ORIGIN_X */
    uint16_t       origin_y;    /**< SCREEN_ORIGIN_Y */
    screen_shape_t shape;       /**< SCREEN_SHAPE    */
} screen_profile_t;

/**
 * @brief Return the active screen profile (compile-time constants).
 *
 * Useful for serialising into JSON (e.g., /api/device/info) without
 * having to scatter #defines across multiple files.
 */
const screen_profile_t *screen_get_profile(void);

#ifdef __cplusplus
}
#endif
