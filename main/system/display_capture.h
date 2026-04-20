#ifndef DISPLAY_CAPTURE_H
#define DISPLAY_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Display capture configuration — derived from screen_config.h */
#include "screen_config.h"
#define CAPTURE_WIDTH           SCREEN_W
#define CAPTURE_HEIGHT          SCREEN_H
#define CAPTURE_BYTES_PER_PIXEL 2   /* RGB565 */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_capture_init(void);

/**
 * Capture a full-screen RGB565 snapshot.
 *
 * Fast path: copies directly from the shadow framebuffer that's kept in sync
 * by the LVGL flush tap in main.c. Typical latency <5 ms.
 *
 * Fallback (only if shadow FB allocation failed): re-renders the whole screen
 * via lv_snapshot_take_to_buf() — hundreds of ms on 800x480.
 *
 * @param output_buffer [out] PSRAM-allocated buffer, caller must free via
 *                            display_capture_free_buffer()
 * @param output_size   [out] size in bytes (W * H * 2 for RGB565)
 */
esp_err_t display_capture_screenshot(uint8_t **output_buffer,
                                     size_t   *output_size);

/**
 * Capture + JPEG-encode a screen frame.
 *
 * `full_res` controls the output dimensions:
 *   - false (DEFAULT, CHEAP): 400x240 (2x nearest-neighbour downsample).
 *                             ~25 ms encode, ~10-20 KB output, ~75% less
 *                             PSRAM bandwidth than full res. Use for Live
 *                             preview polling so running screenshots in
 *                             parallel with SIM doesn't trip the task
 *                             watchdog from PSRAM bus contention.
 *   - true  (RECORDING):      800x480 native. ~100 ms encode, ~25-40 KB
 *                             output. Heavier — only use when the user
 *                             explicitly asks (e.g., ffmpeg capture for
 *                             tutorial recording).
 *
 * `smooth` only has effect when full_res=false. When true, the downsample
 * is a 2x2 box-average instead of nearest-neighbour — visibly smoother
 * when the browser scales the 400x240 frame back up to 800x480 in the
 * Live preview. Costs ~15 ms extra per frame (4x the PSRAM reads).
 *
 * @param quality        1-100, 75-85 is typical
 * @param full_res       false for downsampled 400x240, true for 800x480
 * @param smooth         true for 2x2 box-average (ignored when full_res=true)
 * @param output_buffer [out] heap-allocated JPEG bytes; caller frees via
 *                            display_capture_free_buffer()
 * @param output_size   [out] encoded JPEG size
 */
esp_err_t display_capture_screenshot_jpeg(int       quality,
                                          bool      full_res,
                                          bool      smooth,
                                          uint8_t **output_buffer,
                                          size_t   *output_size);

void display_capture_free_buffer(uint8_t *buffer);

/* Implemented in main.c — exposes the shadow framebuffer maintained by the
 * LVGL flush tap. `display_capture_shadow_ready()` is false until at least
 * one flush has completed after boot. `display_capture_shadow_seq()` ticks
 * on every flush; comparing it to a previously-captured value is a zero-cost
 * way to detect "nothing on screen has changed," which lets the MJPEG
 * streamer skip the encode + WiFi send entirely. */
uint16_t *display_capture_shadow_fb(void);
bool      display_capture_shadow_ready(void);
uint32_t  display_capture_shadow_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_CAPTURE_H */
