#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Feature flag ──────────────────────────────────────────────────────────
 * EXPERIMENTAL widget — signal-driven image sequence ("Animation").
 * Set to 0 (or build with -DRDM_WIDGET_ANIM_ENABLED=0) to compile the whole
 * module out: widget_anim.c compiles empty and the layout_manager factory
 * case disappears, so layouts containing an "anim" widget simply drop it
 * with a log line (same path as an unknown type). Everything else — the
 * WIDGET_ANIM enum entry, name/constraints table rows and the generated
 * inspector field table — is inert data and stays, so no other file needs
 * to change. Full removal steps: tools/WIDGET_ANIM_README.md. */
#ifndef RDM_WIDGET_ANIM_ENABLED
#define RDM_WIDGET_ANIM_ENABLED 1
#endif

/* Hard cap on frames per widget. 16 × a 240×320 RGB565+alpha frame
 * (~230 KB) ≈ 3.7 MB PSRAM worst case — stay well under this in practice
 * (8–12 frames recommended; the editor's uploader shows a size readout). */
#define ANIM_MAX_FRAMES 16

/* ── Per-instance state ──────────────────────────────────────────────────── */
typedef struct {
	/* Configured (serialised) */
	char     frame_prefix[24]; /* frames live at /lfs/images/<prefix>_<i>.rdmimg */
	uint8_t  frame_count;      /* default: 8 */
	uint8_t  mode;             /* 0 = scrub (frame follows value),
	                            * 1 = trigger loop (plays while value >= threshold).
	                            * default: 0. (A free-running "always play" mode is
	                            * not implemented — from_json/inspector coerce any
	                            * other value to scrub, and the schema offers only
	                            * these two.) */
	float    range_min;        /* scrub: value mapped to frame 0. default: 0 */
	float    range_max;        /* scrub: value mapped to last frame. default: 8000 */
	float    threshold;        /* trigger: loop while value >= threshold. default: 6000 */
	uint8_t  loop_fps;         /* trigger: playback rate. default: 12 */
	uint8_t  hyst_pct;         /* frame/threshold hysteresis, % of range. default: 2 */
	char     signal_name[32];
	char     channel_id[32];

	/* Runtime */
	int16_t       signal_index;
	lv_img_dsc_t *frames[ANIM_MAX_FRAMES]; /* PSRAM descriptors via rdm_image_load */
	uint8_t       loaded_count;            /* frames actually loaded (≤ frame_count) */
	lv_obj_t     *img_obj;                 /* single lv_img; source swaps between resident descriptors */
	lv_timer_t   *loop_timer;              /* trigger mode only; paused while inactive */
	uint8_t       cur_frame;
	uint8_t       loop_pos;
	bool          trigger_active;
} anim_data_t;

/**
 * Factory function.
 * Allocates and returns a widget_t wired with the anim vtable.
 * Only referenced when RDM_WIDGET_ANIM_ENABLED — the layout_manager
 * factory case is compiled out with the module.
 * @param slot  Unused (anim widgets are position-based).
 */
widget_t *widget_anim_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
