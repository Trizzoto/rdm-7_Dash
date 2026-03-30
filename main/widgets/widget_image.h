#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── RDMIMG binary header (stored on LittleFS at /lfs/images/<name>.rdmimg) ── */
typedef struct __attribute__((packed)) {
	uint8_t  magic[4];    /* "RDMI" */
	uint16_t width;
	uint16_t height;
	uint8_t  cf;          /* LV_IMG_CF_TRUE_COLOR_ALPHA = 5 */
	uint8_t  reserved[3];
} rdm_image_header_t;

/* ── Per-instance state for image widget ──────────────────────────────── */
typedef struct {
	char          image_name[32];
	uint8_t       opacity;        /* default: 255 */
	lv_color_t    recolor;        /* default: 0x000000 (no effect at opa 0) */
	uint8_t       recolor_opa;    /* default: 0 (disabled), 255 = full overlay */
	uint16_t      image_scale;    /* LVGL zoom: 256 = 100%, 128 = 50%, 512 = 200% */
	lv_img_dsc_t *img_dsc;        /* runtime: PSRAM-loaded image descriptor */
	lv_obj_t     *img_obj;        /* runtime: LVGL image object */
} image_data_t;

/**
 * Load an RDMIMG file from LittleFS into a PSRAM-backed lv_img_dsc_t.
 * Returns a heap-allocated descriptor on success, or NULL on failure.
 * The caller must free both img_dsc->data and img_dsc when done.
 */
lv_img_dsc_t *rdm_image_load(const char *name);

/**
 * Free an image descriptor returned by rdm_image_load().
 */
void rdm_image_free(lv_img_dsc_t *dsc);

/**
 * Factory function.
 * Allocates and returns a widget_t wired with the image vtable.
 * @param slot  Unused for images (always 0).
 * @return      Heap-allocated widget_t *, caller must eventually call w->destroy(w).
 */
widget_t *widget_image_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
