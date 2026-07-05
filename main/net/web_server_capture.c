/* web_server_capture.c — screenshot + MJPEG stream endpoints
 *
 * Endpoints:
 *   GET /screenshot            legacy alias (retained for backwards compat)
 *   GET /api/screenshot        single JPEG (default) or raw RGB565 with ?raw=1
 *                              Optional ?q=1..100 (default 85), ?full=1, ?smooth=1
 *   GET /api/capture/stream    continuous multipart-MJPEG (OBS / ffmpeg / <img>) */
#include "web_server_internal.h"
#include "display_capture.h"
#include "system/screen_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server_capture";

/* Thin convenience wrapper bundling the CORS header every caller here wants. */
static esp_err_t _send_json_capture(httpd_req_t *req, cJSON *root) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

/* esp_http_server's httpd_err_code_t has no 503, so send one by hand. */
static esp_err_t _send_503(httpd_req_t *req, const char *msg) {
	httpd_resp_set_status(req, "503 Service Unavailable");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");
	char body[96];
	snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
	httpd_resp_sendstr(req, body);
	return ESP_FAIL;
}

/* ── Query-param helper ──────────────────────────────────────────────────── */

static int _query_int(httpd_req_t *req, const char *key, int fallback) {
	size_t qlen = httpd_req_get_url_query_len(req);
	if (qlen == 0 || qlen > 256) return fallback;
	char buf[257];
	if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) return fallback;
	char vbuf[16];
	if (httpd_query_key_value(buf, key, vbuf, sizeof(vbuf)) != ESP_OK) return fallback;
	int v = atoi(vbuf);
	return v ? v : fallback;
}

/* ── Screenshot dedup cache ─────────────────────────────────────────────────
 * CONTROL mode polls /api/screenshot once per stream-refresh tick. When
 * the dashboard is idle (no widget updates, no sim, no user input) the
 * LVGL flush tap isn't bumping s_shadow_seq, so re-encoding an
 * identical frame every poll is pure wasted CPU — each full-res encode
 * is ~140 ms best-case, multiplied across every connected browser.
 *
 * This cache remembers (seq, full, quality, smooth, raw) for the last
 * encoded frame. A poll with the same tuple and unchanged seq returns
 * the cached bytes directly — zero encode, zero shadow memcpy. The
 * mjpeg handler already does this per-connection; mirroring it on the
 * single-shot endpoint removes the dominant cause of the watchdog
 * trips in the recent log (repeated 10 s encodes under contention).
 *
 * Mutex protects the buffer lifetime. We hold it only across a short
 * memcpy into a fresh per-request allocation, then release — never
 * during the encode itself. */
static SemaphoreHandle_t s_ss_cache_mux = NULL;
static uint8_t  *s_ss_cache_buf   = NULL;
static size_t    s_ss_cache_size  = 0;
static uint32_t  s_ss_cache_seq   = 0;
static int       s_ss_cache_key_q = -1;
static bool      s_ss_cache_key_full   = false;
static bool      s_ss_cache_key_smooth = false;

/* ── /api/screenshot (+ legacy /screenshot) ─────────────────────────────── */

static esp_err_t screenshot_handler(httpd_req_t *req) {
	/* Default to JPEG; raw=1 forces the original RGB565 output.
	 *   ?q=N        JPEG quality 1-100 (default 100)
	 *   ?full=1     native 800x480 (recording); omit for downsampled 400x240
	 *   ?smooth=1   2x2 box-average downsample (smoother but ~15 ms slower)
	 *   ?raw=1      raw RGB565 instead of JPEG */
	bool want_raw = (_query_int(req, "raw",    0) == 1);
	bool full_res = (_query_int(req, "full",   0) == 1);
	bool smooth   = (_query_int(req, "smooth", 0) == 1);
	int  quality  = _query_int(req, "q", 100);

	/* Conditional GET (JPEG only). The shadow-FB seq plus the render params
	 * uniquely identify the frame's bytes, so build an ETag from them. If the
	 * client already holds this exact frame (If-None-Match matches), return 304
	 * and skip the encode AND the transmit entirely — the dominant idle-poll
	 * cost. A stale ETag can only cause an extra fetch, never a wrong 304. */
	char etag[48] = {0};
	if (!want_raw && display_capture_shadow_ready()) {
		snprintf(etag, sizeof(etag), "\"%lu-%d-%d-%d\"",
		         (unsigned long)display_capture_shadow_seq(),
		         quality, full_res ? 1 : 0, smooth ? 1 : 0);
		char inm[48];
		if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm))
		        == ESP_OK && strcmp(inm, etag) == 0) {
			httpd_resp_set_status(req, "304 Not Modified");
			httpd_resp_set_hdr(req, "ETag", etag);
			httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
			httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
			return httpd_resp_send(req, NULL, 0);
		}
	}

	/* Dedup cache check — only for JPEG requests (raw is uncommon and
	 * big enough that caching the full RGB565 blob isn't worthwhile). */
	uint8_t *buf = NULL;
	size_t   size = 0;
	esp_err_t ret = ESP_OK;
	bool from_cache = false;
	if (!want_raw) {
		if (!s_ss_cache_mux) s_ss_cache_mux = xSemaphoreCreateMutex();
		if (s_ss_cache_mux && xSemaphoreTake(s_ss_cache_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
			uint32_t cur_seq = display_capture_shadow_seq();
			if (s_ss_cache_buf
			    && s_ss_cache_seq        == cur_seq
			    && s_ss_cache_key_q      == quality
			    && s_ss_cache_key_full   == full_res
			    && s_ss_cache_key_smooth == smooth) {
				buf = heap_caps_malloc(s_ss_cache_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
				if (buf) {
					memcpy(buf, s_ss_cache_buf, s_ss_cache_size);
					size = s_ss_cache_size;
					from_cache = true;
				}
			}
			xSemaphoreGive(s_ss_cache_mux);
		}
	}

	if (!from_cache) {
		ret = want_raw
		    ? display_capture_screenshot(&buf, &size)
		    : display_capture_screenshot_jpeg(quality, full_res, smooth, &buf, &size);
	}

	/* If JPEG encoding isn't compiled in yet (pre-component-fetch state),
	 * gracefully fall through to raw so the endpoint still works. */
	if (!want_raw && ret == ESP_ERR_NOT_SUPPORTED) {
		ESP_LOGW(TAG, "JPEG not available, falling back to raw");
		ret = display_capture_screenshot(&buf, &size);
		want_raw = true;
	}

	if (ret != ESP_OK) {
		/* ESP_ERR_TIMEOUT = another encode already in flight. Tell the
		 * client to retry rather than escalating to 500 (which would
		 * fire error toasts in Studio and trigger its 500 ms back-off).
		 * Code 503 + short Retry-After lets the poll loop try again on
		 * the next natural tick. */
		if (ret == ESP_ERR_TIMEOUT) {
			httpd_resp_set_status(req, "503 Service Unavailable");
			httpd_resp_set_hdr(req, "Retry-After", "1");
			httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
			httpd_resp_send(req, "busy", 4);
			return ESP_OK;
		}
		ESP_LOGE(TAG, "Capture failed: %s", esp_err_to_name(ret));
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
		                     "Screenshot capture failed");
		return ESP_FAIL;
	}

	/* Populate the dedup cache for JPEGs we just encoded (not cache hits,
	 * not fallbacks to raw). Next poll with the same params + unchanged
	 * shadow_seq will serve from cache instead of re-encoding. */
	if (!want_raw && !from_cache && s_ss_cache_mux
	    && xSemaphoreTake(s_ss_cache_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
		if (s_ss_cache_buf) heap_caps_free(s_ss_cache_buf);
		s_ss_cache_buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (s_ss_cache_buf) {
			memcpy(s_ss_cache_buf, buf, size);
			s_ss_cache_size       = size;
			s_ss_cache_seq        = display_capture_shadow_seq();
			s_ss_cache_key_q      = quality;
			s_ss_cache_key_full   = full_res;
			s_ss_cache_key_smooth = smooth;
		} else {
			s_ss_cache_size = 0;  /* mark invalid on alloc fail */
		}
		xSemaphoreGive(s_ss_cache_mux);
	}

	httpd_resp_set_type(req, want_raw ? "application/octet-stream" : "image/jpeg");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	/* ETag lets the client conditionally GET the next frame (304 when the
	 * dashboard hasn't changed). must-revalidate + no-store would forbid the
	 * client from replaying a body it never keeps anyway, but we DO want it to
	 * revalidate via If-None-Match, so advertise the tag and keep no-cache. */
	if (etag[0]) httpd_resp_set_hdr(req, "ETag", etag);
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

	esp_err_t send_ret = httpd_resp_send(req, (const char *)buf, size);
	display_capture_free_buffer(buf);
	ESP_LOGI(TAG, "Screenshot sent: %zu bytes (%s%s)", size,
	         want_raw ? "raw" : "jpeg",
	         from_cache ? ", cached" : "");
	return send_ret;
}

/* ── /api/capture/stream — continuous MJPEG ─────────────────────────────────
 * Content-Type: multipart/x-mixed-replace; boundary=frame
 *
 * Defaults are tuned for ambient "see what's on the dash from Studio" use:
 *     fps=5, q=100 → ~30 KB/frame, ~70 ms encode.
 * Override per-URL for recording:
 *     /api/capture/stream?fps=15&q=80
 *
 * Frame-skip: compares display_capture_shadow_seq() to the last encode.
 * If the counter hasn't moved the dashboard hasn't drawn anything new and
 * we resend the previously-cached JPEG unchanged, skipping the ~70-150 ms
 * encode + shadow memcpy entirely. */
#define MJPEG_BOUNDARY "\r\n--frame\r\n"
static esp_err_t mjpeg_stream_handler(httpd_req_t *req) {
	int quality    = _query_int(req, "q",     100);
	int target_fps = _query_int(req, "fps",   5);
	bool full_res  = (_query_int(req, "full",   0) == 1);
	bool smooth    = (_query_int(req, "smooth", 0) == 1);
	if (target_fps < 1)  target_fps = 1;
	if (target_fps > 30) target_fps = 30;
	int frame_budget_ms = 1000 / target_fps;

	/* Keepalive: even when nothing changes, send the cached frame every
	 * keepalive_ms so the browser doesn't stall its <img> repaint and
	 * proxies don't decide the connection is dead. */
	const int keepalive_ms = 1000;

	httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
	httpd_resp_set_hdr(req, "Pragma", "no-cache");

	ESP_LOGI(TAG, "MJPEG stream starting (q=%d, target_fps=%d, frame-skip on)",
	         quality, target_fps);

	uint8_t *cached_jpg    = NULL;
	size_t   cached_size   = 0;
	uint32_t last_seq      = 0xFFFFFFFFu;  /* force first encode */
	int64_t  last_send_us  = 0;

	while (1) {
		int64_t loop_start = esp_timer_get_time();
		uint32_t cur_seq   = display_capture_shadow_seq();
		bool     ui_changed = (cur_seq != last_seq);
		int64_t  since_send_ms = (loop_start - last_send_us) / 1000;
		bool     need_keepalive = (since_send_ms >= keepalive_ms);

		/* Re-encode only if UI actually changed OR we have no cache yet. */
		if (ui_changed || !cached_jpg) {
			uint8_t *fresh = NULL;
			size_t   fresh_size = 0;
			esp_err_t r = display_capture_screenshot_jpeg(quality, full_res, smooth, &fresh, &fresh_size);
			if (r == ESP_OK) {
				if (cached_jpg) display_capture_free_buffer(cached_jpg);
				cached_jpg  = fresh;
				cached_size = fresh_size;
				last_seq    = cur_seq;
			} else if (!cached_jpg) {
				ESP_LOGW(TAG, "MJPEG: first capture failed (%s), ending",
				         esp_err_to_name(r));
				break;
			}
			/* else: keep serving the stale cache until the next change */
		} else if (!need_keepalive) {
			vTaskDelay(pdMS_TO_TICKS(frame_budget_ms));
			continue;
		}

		char hdr[128];
		int hdr_len = snprintf(hdr, sizeof(hdr),
		    MJPEG_BOUNDARY
		    "Content-Type: image/jpeg\r\n"
		    "Content-Length: %u\r\n\r\n",
		    (unsigned)cached_size);

		if (httpd_resp_send_chunk(req, hdr, hdr_len) != ESP_OK ||
		    httpd_resp_send_chunk(req, (const char *)cached_jpg, cached_size) != ESP_OK) {
			ESP_LOGI(TAG, "MJPEG client disconnected");
			break;
		}
		last_send_us = esp_timer_get_time();

		int64_t elapsed_ms = (last_send_us - loop_start) / 1000;
		if (elapsed_ms < frame_budget_ms) {
			vTaskDelay(pdMS_TO_TICKS(frame_budget_ms - elapsed_ms));
		}
	}

	if (cached_jpg) display_capture_free_buffer(cached_jpg);
	httpd_resp_send_chunk(req, NULL, 0);
	return ESP_OK;
}

/* ── GET /api/screenshot/hash?x=&y=&w=&h= ────────────────────────────────────
 *
 * Deterministic FNV-1a hash over a rectangular region of the persistent RGB565
 * shadow framebuffer (the one the LVGL flush tap keeps in sync). Unlike the
 * JPEG screenshot this is lossless and stable, so an agent can golden-compare a
 * region or cheaply detect "did anything change" via the seq counter. Coords
 * are in framebuffer space (0,0 = top-left, SCREEN_W×SCREEN_H); defaults to the
 * whole screen. `torn` is true if the flush tap wrote to the FB mid-hash (the
 * client should retry). */
static esp_err_t _screenshot_hash_handler(httpd_req_t *req) {
	if (!display_capture_shadow_ready())
		return _send_503(req, "shadow framebuffer not ready");

	int x = _query_int(req, "x", 0);
	int y = _query_int(req, "y", 0);
	int w = _query_int(req, "w", SCREEN_W);
	int h = _query_int(req, "h", SCREEN_H);
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= SCREEN_W) x = SCREEN_W - 1;
	if (y >= SCREEN_H) y = SCREEN_H - 1;
	/* Bound against remaining width/height, never `x + w` — with an attacker-
	 * supplied w near INT_MAX the addition overflows to a negative int, sails
	 * past this clamp, and the FNV loop below reads far off the framebuffer.
	 * SCREEN_W - x is safe because x is already clamped to [0, SCREEN_W-1]. */
	if (w <= 0 || w > SCREEN_W - x) w = SCREEN_W - x;
	if (h <= 0 || h > SCREEN_H - y) h = SCREEN_H - y;

	const uint16_t *fb = display_capture_shadow_fb();
	if (!fb)
		return _send_503(req, "no shadow framebuffer");

	uint32_t seq_before = display_capture_shadow_seq();
	uint32_t hash = 2166136261u;  /* FNV-1a/32 */
	for (int row = y; row < y + h; row++) {
		const uint16_t *p = fb + (size_t)row * SCREEN_W + x;
		for (int col = 0; col < w; col++) {
			uint16_t px = p[col];
			hash ^= (px & 0xFF);        hash *= 16777619u;
			hash ^= ((px >> 8) & 0xFF); hash *= 16777619u;
		}
	}
	uint32_t seq_after = display_capture_shadow_seq();

	char hexhash[11];
	snprintf(hexhash, sizeof(hexhash), "0x%08lX", (unsigned long)hash);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "hash", hexhash);
	cJSON_AddNumberToObject(root, "seq", seq_after);
	cJSON_AddNumberToObject(root, "x", x);
	cJSON_AddNumberToObject(root, "y", y);
	cJSON_AddNumberToObject(root, "w", w);
	cJSON_AddNumberToObject(root, "h", h);
	cJSON_AddBoolToObject(root, "torn", seq_before != seq_after);
	return _send_json_capture(req, root);
}

/* ── URI descriptors ─────────────────────────────────────────────────────── */

static const httpd_uri_t screenshot_uri = {
    .uri = "/screenshot", .method = HTTP_GET,
    .handler = screenshot_handler, .user_ctx = NULL};

static const httpd_uri_t api_screenshot_hash_uri = {
    .uri = "/api/screenshot/hash", .method = HTTP_GET,
    .handler = _screenshot_hash_handler, .user_ctx = NULL};

static const httpd_uri_t api_screenshot_uri = {
    .uri = "/api/screenshot", .method = HTTP_GET,
    .handler = screenshot_handler, .user_ctx = NULL};

static const httpd_uri_t api_capture_stream_uri = {
    .uri = "/api/capture/stream", .method = HTTP_GET,
    .handler = mjpeg_stream_handler, .user_ctx = NULL};

void web_server_capture_register(httpd_handle_t server) {
	REGISTER_URI(server, &screenshot_uri);
	REGISTER_URI(server, &api_screenshot_uri);
	REGISTER_URI(server, &api_screenshot_hash_uri);
	REGISTER_URI(server, &api_capture_stream_uri);
}
