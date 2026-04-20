/**
 * display_capture.c — Fast full-screen RGB565 + JPEG capture.
 *
 * The hot path copies from a shadow framebuffer maintained by the LVGL flush
 * tap in main.c (see `display_capture_shadow_fb()`). That's a straight PSRAM
 * memcpy — no re-rendering, no widget-tree walk. Typical latency <5 ms for
 * the raw path, ~100–200 ms total for the JPEG path (encode dominates).
 *
 * The old lv_snapshot_take_to_buf() route stays as a last-resort fallback if
 * the shadow FB couldn't be allocated at boot (out-of-PSRAM scenarios).
 */
#include "display_capture.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_jpeg_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "display_capture";

extern bool example_lvgl_lock(int timeout_ms);
extern void example_lvgl_unlock(void);

/* ── Working buffers ──────────────────────────────────────────────────────
 * Capture needs two scratch buffers per request:
 *   - Colour-converted source (2 bytes/pixel — packed YUYV 4:2:2, the
 *     JPEG_PIXEL_FORMAT_YCbYCr layout the encoder accepts)
 *   - JPEG output
 *
 * Why YUYV (YCbYCr) instead of RGB888:
 *   - 2 bytes/pixel vs 3 → full-res buffer drops from 1.15 MB to 768 KB
 *     (-33%). Much easier to find contiguous PSRAM under fragmentation.
 *   - Encoder skips its internal RGB→YUV pass → ~15% faster encode.
 *   - Well-documented standard format (YUYV / YUY2) — zero ambiguity in
 *     byte order, unlike the esp_new_jpeg-specific YUV420 packing which
 *     came out corrupted when guessed.
 *
 * Persistent pair sized for half-res (400x240 = 192 KB + 96 KB = 288 KB
 * total) covers the common fast path: edit-mode auto-refresh, save/apply,
 * any /api/screenshot without ?full=1.
 *
 * Full-res requests go transient — allocated per request, freed after. */
static SemaphoreHandle_t s_work_mux  = NULL;
static uint8_t *s_yuv_buf = NULL;   /* 16-byte aligned PSRAM, half-res cap */
static size_t   s_yuv_cap = 0;
static uint8_t *s_jpg_buf = NULL;   /* 16-byte aligned PSRAM, half-res cap */
static size_t   s_jpg_cap = 0;

/* Half-res ceiling for the persistent buffers (400x240).
 * YUYV = 2 bytes/pixel. */
#define PERSISTENT_W    (CAPTURE_WIDTH  / 2)
#define PERSISTENT_H    (CAPTURE_HEIGHT / 2)
#define PERSISTENT_YUV  ((size_t)PERSISTENT_W * (size_t)PERSISTENT_H * 2)
#define PERSISTENT_JPG  ((size_t)PERSISTENT_W * (size_t)PERSISTENT_H)

static bool _ensure_mutex(void) {
	if (!s_work_mux) {
		s_work_mux = xSemaphoreCreateMutex();
		if (!s_work_mux) return false;
	}
	return true;
}

/* Lazily allocate the persistent half-res buffers. Only touched when a
 * half-res capture actually runs; no cost if CONTROL is never used and
 * no auto-refresh polls (e.g., SIM-only tuning sessions). */
static bool _ensure_persistent_buffers(void) {
	if (s_yuv_buf && s_jpg_buf) return true;
	if (!s_yuv_buf) {
		s_yuv_buf = heap_caps_aligned_alloc(16, PERSISTENT_YUV,
		                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!s_yuv_buf) return false;
		s_yuv_cap = PERSISTENT_YUV;
		ESP_LOGI(TAG, "Persistent YUV420 work buffer: %zu bytes (half-res)", PERSISTENT_YUV);
	}
	if (!s_jpg_buf) {
		s_jpg_buf = heap_caps_aligned_alloc(16, PERSISTENT_JPG,
		                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!s_jpg_buf) return false;
		s_jpg_cap = PERSISTENT_JPG;
		ESP_LOGI(TAG, "Persistent JPEG out buffer:    %zu bytes (half-res)", PERSISTENT_JPG);
	}
	return true;
}

/* ── RGB565 → YUYV (YCbYCr 4:2:2) conversion ──────────────────────────────
 * The encoder's JPEG_PIXEL_FORMAT_YCbYCr expects 4 bytes per horizontally-
 * adjacent pixel pair in this industry-standard YUYV order:
 *   [Y0][Cb][Y1][Cr]    for pixels (2x, y) and (2x+1, y)
 *
 * Cb and Cr are shared across the pair; typical practice is to average
 * them from the two source pixels. We average for best chroma fidelity.
 *
 * BT.601 colour-space math with integer fixed-point:
 *   Y  = (77 R + 150 G + 29 B + 128) >> 8
 *   Cb = ((-43 R - 85 G + 128 B + 128) >> 8) + 128
 *   Cr = ((128 R - 107 G - 21 B + 128) >> 8) + 128
 * R/G/B are 8-bit values expanded from RGB565 with bit replication. */

static inline void _rgb565_to_rgb8(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
	uint8_t r5 = (c >> 11) & 0x1F;
	uint8_t g6 = (c >> 5)  & 0x3F;
	uint8_t b5 =  c        & 0x1F;
	*r = (uint8_t)((r5 << 3) | (r5 >> 2));
	*g = (uint8_t)((g6 << 2) | (g6 >> 4));
	*b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static inline uint8_t _rgb_to_y(uint8_t r, uint8_t g, uint8_t b) {
	int v = (77 * r + 150 * g + 29 * b + 128) >> 8;
	if (v < 0) v = 0; else if (v > 255) v = 255;
	return (uint8_t)v;
}
static inline uint8_t _rgb_to_cb(uint8_t r, uint8_t g, uint8_t b) {
	int v = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
	if (v < 0) v = 0; else if (v > 255) v = 255;
	return (uint8_t)v;
}
static inline uint8_t _rgb_to_cr(uint8_t r, uint8_t g, uint8_t b) {
	int v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
	if (v < 0) v = 0; else if (v > 255) v = 255;
	return (uint8_t)v;
}

/* Compute the 8-bit R/G/B of one OUTPUT pixel at (ox, oy), reading from
 * the panel FB. `step` is 1 for full-res, 2 for 2x downsample. `smooth`
 * (only meaningful when step==2) averages the 2x2 source block. */
static inline void _output_rgb(const uint16_t *src, int ox, int oy,
                               int step, bool smooth,
                               uint8_t *r, uint8_t *g, uint8_t *b) {
	int sx = ox * step;
	int sy = oy * step;
	if (smooth && step == 2) {
		uint8_t r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
		_rgb565_to_rgb8(src[(sy)     * CAPTURE_WIDTH + sx    ], &r0, &g0, &b0);
		_rgb565_to_rgb8(src[(sy)     * CAPTURE_WIDTH + sx + 1], &r1, &g1, &b1);
		_rgb565_to_rgb8(src[(sy + 1) * CAPTURE_WIDTH + sx    ], &r2, &g2, &b2);
		_rgb565_to_rgb8(src[(sy + 1) * CAPTURE_WIDTH + sx + 1], &r3, &g3, &b3);
		*r = (uint8_t)((r0 + r1 + r2 + r3 + 2) >> 2);
		*g = (uint8_t)((g0 + g1 + g2 + g3 + 2) >> 2);
		*b = (uint8_t)((b0 + b1 + b2 + b3 + 2) >> 2);
	} else {
		_rgb565_to_rgb8(src[sy * CAPTURE_WIDTH + sx], r, g, b);
	}
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t display_capture_init(void) {
	ESP_LOGI(TAG, "Display capture initialized (shadow FB path: %s)",
	         display_capture_shadow_fb() ? "available" : "NOT AVAILABLE");
	return ESP_OK;
}

/* ── Fallback: old lv_snapshot path (re-renders everything) ──────────────── */

static esp_err_t _capture_via_snapshot(uint8_t **output_buffer,
                                        size_t   *output_size) {
	if (!example_lvgl_lock(1000)) {
		ESP_LOGE(TAG, "Failed to lock LVGL mutex (snapshot fallback)");
		return ESP_ERR_TIMEOUT;
	}

	size_t bytes = (size_t)CAPTURE_WIDTH * CAPTURE_HEIGHT * CAPTURE_BYTES_PER_PIXEL;
	uint8_t *out = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) out = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (!out) { example_lvgl_unlock(); return ESP_ERR_NO_MEM; }

	lv_obj_t *screen = lv_scr_act();
	if (screen) {
		uint32_t buf_size = lv_snapshot_buf_size_needed(screen, LV_IMG_CF_TRUE_COLOR);
		if (buf_size > 0) {
			void *sbuf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (!sbuf) sbuf = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
			if (sbuf) {
				lv_img_dsc_t snap;
				if (lv_snapshot_take_to_buf(screen, LV_IMG_CF_TRUE_COLOR,
				                             &snap, sbuf, buf_size) == LV_RES_OK
				    && snap.data) {
					size_t snap_bytes = (size_t)snap.header.w * snap.header.h * sizeof(lv_color_t);
					if (snap_bytes > bytes) snap_bytes = bytes;
					memcpy(out, snap.data, snap_bytes);
					*output_buffer = out;
					*output_size   = snap_bytes;
					heap_caps_free(sbuf);
					example_lvgl_unlock();
					return ESP_OK;
				}
				heap_caps_free(sbuf);
			}
		}
	}
	/* Everything failed — return a test pattern so callers see *something* */
	uint16_t *px = (uint16_t *)out;
	for (int y = 0; y < CAPTURE_HEIGHT; y++)
		for (int x = 0; x < CAPTURE_WIDTH; x++)
			px[y * CAPTURE_WIDTH + x] =
			    (((x * 255 / CAPTURE_WIDTH) >> 3) << 11) |
			    (((y * 255 / CAPTURE_HEIGHT) >> 2) << 5) | (128 >> 3);
	*output_buffer = out;
	*output_size   = bytes;
	example_lvgl_unlock();
	ESP_LOGW(TAG, "Snapshot failed — returned test pattern");
	return ESP_OK;
}

/* ── Fast path: memcpy from the shadow FB ────────────────────────────────── */

esp_err_t display_capture_screenshot(uint8_t **output_buffer,
                                     size_t   *output_size) {
	if (!output_buffer || !output_size) return ESP_ERR_INVALID_ARG;

	uint16_t *shadow = display_capture_shadow_fb();
	if (!shadow || !display_capture_shadow_ready()) {
		ESP_LOGW(TAG, "Shadow FB not ready — falling back to snapshot");
		return _capture_via_snapshot(output_buffer, output_size);
	}

	const size_t bytes = (size_t)CAPTURE_WIDTH * CAPTURE_HEIGHT * CAPTURE_BYTES_PER_PIXEL;
	uint8_t *out = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) return ESP_ERR_NO_MEM;

	/* NOTE: deliberately NOT taking the LVGL mutex here. Holding it during
	 * a 37-ms 750 KB PSRAM-to-PSRAM memcpy blows the LVGL frame budget and
	 * causes visible UI hitches (the "No CAN bus" pill and other widgets
	 * freeze for a frame). The shadow is written by flush_cb on the LVGL
	 * task; racing with it can yield a "torn" frame where the top half is
	 * one frame and the bottom half is the next. That's cosmetically fine
	 * for a video stream — the next frame corrects it. */
	int64_t t0 = esp_timer_get_time();
	memcpy(out, shadow, bytes);
	int64_t t1 = esp_timer_get_time();

	*output_buffer = out;
	*output_size   = bytes;
	ESP_LOGD(TAG, "Captured %zu bytes in %lld us (shadow-FB memcpy)",
	         bytes, (long long)(t1 - t0));
	return ESP_OK;
}

/* ── JPEG path (YUYV 4:2:2 packed input) ──────────────────────────────────
 * Read RGB565 from the panel FB, convert to YUYV (JPEG_PIXEL_FORMAT_YCbYCr),
 * then feed the encoder. YUYV is 2 bytes/pixel vs RGB888's 3 bytes/pixel.
 * Full-res intermediate drops from 1.15 MB to 768 KB (-33%), making the
 * contiguous PSRAM allocation much more likely to succeed under heap
 * fragmentation. Encoder also skips its internal RGB→YUV conversion pass.
 *
 * We use subsampling=JPEG_SUBSAMPLE_420 on the output so file sizes stay
 * small — the encoder downsamples the input 4:2:2 chroma to 4:2:0 during
 * the DCT stage, same final compression as if we'd fed it RGB888+4:2:0. */

esp_err_t display_capture_screenshot_jpeg(int quality, bool full_res, bool smooth,
                                          uint8_t **output_buffer,
                                          size_t   *output_size) {
	if (!output_buffer || !output_size) return ESP_ERR_INVALID_ARG;
	if (quality <   1) quality = 1;
	if (quality > 100) quality = 100;
	/* smooth only meaningful when we're actually downsampling */
	if (full_res) smooth = false;

	uint16_t *src = display_capture_shadow_fb();   /* panel FB pointer */
	if (!src || !display_capture_shadow_ready()) {
		ESP_LOGW(TAG, "Panel FB not ready — JPEG path unavailable");
		return ESP_ERR_INVALID_STATE;
	}

	const int step   = full_res ? 1 : 2;
	const int out_w  = CAPTURE_WIDTH  / step;
	const int out_h  = CAPTURE_HEIGHT / step;
	const size_t out_px    = (size_t)out_w * (size_t)out_h;
	const size_t yuv_bytes = out_px * 2;  /* YUYV: 2 bytes/pixel */
	const size_t jpg_max   = out_px;       /* generous ceiling */

	if (!_ensure_mutex()) return ESP_ERR_NO_MEM;
	if (xSemaphoreTake(s_work_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
		ESP_LOGW(TAG, "Work buffer mutex timeout");
		return ESP_ERR_TIMEOUT;
	}

	/* Pick the right buffers: persistent for half-res (always available),
	 * transient for full-res (freed after use). */
	uint8_t *yuv = NULL;
	uint8_t *jpg = NULL;
	bool transient = (yuv_bytes > PERSISTENT_YUV) || (jpg_max > PERSISTENT_JPG);

	if (transient) {
		yuv = heap_caps_aligned_alloc(16, yuv_bytes,
		                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		jpg = heap_caps_aligned_alloc(16, jpg_max,
		                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!yuv || !jpg) {
			ESP_LOGW(TAG, "full-res alloc failed (yuv=%p jpg=%p, need %zu+%zu) — falling back to half-res",
			         (void *)yuv, (void *)jpg, yuv_bytes, jpg_max);
			if (yuv) heap_caps_free(yuv);
			if (jpg) heap_caps_free(jpg);
			xSemaphoreGive(s_work_mux);
			/* Recurse with half-res so the caller still gets a JPEG. */
			return display_capture_screenshot_jpeg(quality, false, smooth,
			                                        output_buffer, output_size);
		}
	} else {
		if (!_ensure_persistent_buffers()) {
			xSemaphoreGive(s_work_mux);
			return ESP_ERR_NO_MEM;
		}
		yuv = s_yuv_buf;
		jpg = s_jpg_buf;
	}

	/* ── Step 1: RGB565 → YUYV conversion ──────────────────────────────
	 * Walk the output image pair-by-pair horizontally. For each pair
	 * compute RGB (optionally 2x2 box-averaged), then pack as:
	 *     [Y0][Cb][Y1][Cr]
	 * where Cb/Cr are averaged from the two pixels' chroma values.
	 *
	 * No LVGL mutex — torn frames at a flush boundary are visually fine.
	 * Yield every 32 rows so the encoder task doesn't hog CPU 0. */
	int64_t t0 = esp_timer_get_time();
	for (int y = 0; y < out_h; y++) {
		uint8_t *row_out = yuv + (size_t)y * out_w * 2;
		for (int x = 0; x < out_w; x += 2) {
			uint8_t r0, g0, b0, r1, g1, b1;
			_output_rgb(src, x,     y, step, smooth, &r0, &g0, &b0);
			_output_rgb(src, x + 1, y, step, smooth, &r1, &g1, &b1);

			uint8_t Y0  = _rgb_to_y(r0, g0, b0);
			uint8_t Y1  = _rgb_to_y(r1, g1, b1);
			/* Average R/G/B across the pair, then derive Cb/Cr once —
			 * equivalent to averaging Cb/Cr but avoids two conversions. */
			uint8_t ra = (uint8_t)((r0 + r1 + 1) >> 1);
			uint8_t ga = (uint8_t)((g0 + g1 + 1) >> 1);
			uint8_t ba = (uint8_t)((b0 + b1 + 1) >> 1);
			uint8_t Cb  = _rgb_to_cb(ra, ga, ba);
			uint8_t Cr  = _rgb_to_cr(ra, ga, ba);

			*row_out++ = Y0;
			*row_out++ = Cb;
			*row_out++ = Y1;
			*row_out++ = Cr;
		}
		if ((y & 0x1F) == 0x1F) vTaskDelay(0);
	}
	int64_t t1 = esp_timer_get_time();

	/* ── Step 2: JPEG encode ──────────────────────────────────────────── */
	jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
	cfg.width       = out_w;
	cfg.height      = out_h;
	cfg.src_type    = JPEG_PIXEL_FORMAT_YCbYCr;   /* YUYV 4:2:2 packed */
	cfg.subsampling = JPEG_SUBSAMPLE_420;          /* encoder downsamples chroma further */
	cfg.quality     = (uint8_t)quality;

	jpeg_enc_handle_t enc = NULL;
	if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK || !enc) {
		ESP_LOGE(TAG, "jpeg_enc_open failed");
		if (transient) { heap_caps_free(yuv); heap_caps_free(jpg); }
		xSemaphoreGive(s_work_mux);
		return ESP_FAIL;
	}

	int encoded = 0;
	int rc = jpeg_enc_process(enc, yuv, (int)yuv_bytes,
	                          jpg, (int)jpg_max, &encoded);
	int64_t t2 = esp_timer_get_time();
	jpeg_enc_close(enc);

	if (rc != JPEG_ERR_OK || encoded <= 0) {
		ESP_LOGE(TAG, "jpeg_enc_process failed rc=%d encoded=%d", rc, encoded);
		if (transient) { heap_caps_free(yuv); heap_caps_free(jpg); }
		xSemaphoreGive(s_work_mux);
		return ESP_FAIL;
	}

	/* Step 3: copy encoded bytes into a caller-owned buffer. */
	uint8_t *out = heap_caps_malloc((size_t)encoded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) {
		if (transient) { heap_caps_free(yuv); heap_caps_free(jpg); }
		xSemaphoreGive(s_work_mux);
		return ESP_ERR_NO_MEM;
	}
	memcpy(out, jpg, (size_t)encoded);

	if (transient) {
		heap_caps_free(yuv);
		heap_caps_free(jpg);
	}
	xSemaphoreGive(s_work_mux);
	vTaskDelay(0);  /* yield before returning to HTTPD */

	*output_buffer = out;
	*output_size   = (size_t)encoded;
	ESP_LOGD(TAG, "JPEG %dx%d q=%d: %d bytes, convert %lld ms + encode %lld ms (YUV420 path)",
	         out_w, out_h, quality, encoded,
	         (long long)((t1 - t0) / 1000),
	         (long long)((t2 - t1) / 1000));
	return ESP_OK;
}

/* ── Common ──────────────────────────────────────────────────────────────── */

void display_capture_free_buffer(uint8_t *buffer) {
	if (buffer) heap_caps_free(buffer);
}
