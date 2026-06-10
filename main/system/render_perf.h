#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * render_perf — last completed ~1 s window of LVGL render telemetry, published
 * by rdm7_lvgl_monitor_cb (LVGL task, core 1) and read by the web server's
 * GET /api/perf. Replaces the old refr_diag serial log (now LOGD/compiled out)
 * as the way to measure FPS work on-device without flooding field logs.
 */
typedef struct {
	uint32_t seq;            /* increments per published window; 0 = none yet */
	uint32_t fps_x10;        /* frames-per-second * 10 over the window */
	uint32_t frames;         /* frames rendered in the window */
	uint32_t avg_px;         /* avg invalidated px/frame (post-join) */
	uint32_t avg_pct;        /* avg invalidated % of screen per frame */
	uint32_t max_pct;        /* worst single frame's invalidated % */
	uint32_t avg_render_ms;  /* avg LVGL elaps (draw+flush) per frame, ms */
	uint32_t flush_per_frame_x10; /* avg flush_cb calls per frame * 10 */
	uint32_t flush_us_per_frame;  /* avg flush_cb time per frame, µs */
} render_perf_t;

/* Copy the most recently published window into *out. Torn reads are possible
 * in principle (writer on the LVGL task, reader on httpd) but each field is a
 * 32-bit aligned write and the data is 1 s-granularity diagnostics — compare
 * `seq` across two reads if consistency matters. */
void render_perf_get(render_perf_t *out);

#ifdef __cplusplus
}
#endif
