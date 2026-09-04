#include "web_server.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "system/crash_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "web_server_internal.h"

/* Fallback for static-analyser builds that don't see layout_manager.h's define. */
#ifndef LAYOUT_MAX_FILE_BYTES
#define LAYOUT_MAX_FILE_BYTES 32768
#endif

/* Embedded web UI — gzipped at CMake configure time and added to
 * EMBED_FILES in main/CMakeLists.txt as ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz.
 * Served verbatim with Content-Encoding: gzip; every modern browser
 * inflates transparently. Compressed payload is ~150 KB vs ~825 KB raw. */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

/* Embedded favicon (EMBED_FILES "web/favicon.ico" in CMakeLists.txt). */
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

/* Largest unread body we'll politely drain before returning an error. A
 * rejected layout is at most a few hundred KB even when abused via the editor;
 * past this we stop reading and accept that the socket resets. */
#define WEB_DRAIN_CAP (384 * 1024)

/* Max consecutive httpd recv timeouts before web_server_recv_body_raw() gives
 * up, so a stalled client can't wedge the single httpd task indefinitely. */
#define WEB_RECV_MAX_TIMEOUTS 3

/* Drain up to `max_drain` bytes of an unread request body.
 *
 * esp_http_server closes a connection with a TCP RST (not a clean FIN) when
 * there is still unconsumed RX data on the socket. So a handler that rejects a
 * large body *before* reading it (e.g. an oversize layout) leaves the client
 * seeing a connection reset instead of our 4xx — the stress test's 2000-widget
 * preview hit exactly this (ConnectionResetError instead of the 413). Draining
 * the body first lets the error response actually arrive. Bounded so a
 * maliciously huge body can't tie the worker up indefinitely. */
void web_server_drain_body(httpd_req_t *req, size_t max_drain) {
	size_t remaining = req->content_len;
	if (remaining > max_drain) remaining = max_drain;
	char sink[512];
	size_t got = 0;
	while (got < remaining) {
		size_t chunk = remaining - got;
		if (chunk > sizeof(sink)) chunk = sizeof(sink);
		int r = httpd_req_recv(req, sink, chunk);
		/* A recv timeout means the sender stalled. We bail rather than loop:
		 * httpd runs all handlers on ONE task, so spinning here on a no-data
		 * stall would wedge the entire web server (slowloris). Best-effort
		 * drain — if the client won't send, let the socket close (RST). */
		if (r <= 0) break;
		got += (size_t)r;
	}
}

/* 503 + Retry-After for *transient* conditions: the LVGL lock was momentarily
 * held, a heavy build is already in flight, or a short-lived OOM under
 * concurrent load. Unlike a 500, this tells the client the request itself is
 * fine and to retry shortly — which the editor's live-edit and poll loops do —
 * instead of surfacing a hard error toast. Callers `return web_server_send_busy(req);`. */
esp_err_t web_server_send_busy(httpd_req_t *req) {
	httpd_resp_set_status(req, "503 Service Unavailable");
	httpd_resp_set_hdr(req, "Retry-After", "1");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":false,\"error\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
	return ESP_OK;
}

/* Send a structured 413 Payload-Too-Large JSON error.
 *
 * Layout JSON > LAYOUT_MAX_FILE_BYTES is a real failure mode in the editor,
 * historically masked by an opaque 400. This helper returns a body the
 * editor can parse and surface inline:
 *     { "ok":false, "error":"layout_too_large", "max":32768, "actual":N }
 */
esp_err_t web_server_send_layout_too_large(httpd_req_t *req, size_t actual) {
	/* Drain the (rejected) body so the client reads this 413 instead of an RST. */
	web_server_drain_body(req, WEB_DRAIN_CAP);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_status(req, "413 Payload Too Large");
	char body[128];
	snprintf(body, sizeof(body),
			 "{\"ok\":false,\"error\":\"layout_too_large\","
			 "\"max\":%u,\"actual\":%u}",
			 (unsigned)LAYOUT_MAX_FILE_BYTES, (unsigned)actual);
	httpd_resp_sendstr(req, body);
	return ESP_FAIL;
}

/* Serialize `root`, send it as application/json, and free both. ALWAYS consumes
 * `root` (deletes it, even on error). If serialization OOMs
 * (cJSON_PrintUnformatted returns NULL) it sends a 500 instead of calling
 * httpd_resp_sendstr(NULL), which crashes. Callers must not free root or send a
 * response themselves. Does NOT set the CORS header: callers set it before
 * calling (the established per-handler pattern), and httpd_resp_set_hdr appends
 * rather than replaces, so setting it here too would emit a duplicate
 * Access-Control-Allow-Origin that strict browsers reject. */
static void _reboot_timer_cb(void *arg) {
	(void)arg;
	/* Mark the shutdown clean BEFORE restarting.
	 *
	 * esp_restart() on the S3 resets through the RTC watchdog, so the next
	 * boot reads ESP_RST_WDT — which crash_log counts as crash-class. Without
	 * this, every deliberate reboot through the API logged itself as a panic:
	 * panic_count climbed and /api/selftest reported prev_was_crash on a dash
	 * that had simply been asked to restart. The OTA path already did this;
	 * this one did not, and it is the path a user hits after any settings
	 * change or a bootloader self-update. */
	crash_log_mark_clean_shutdown();
	esp_restart();
}

bool web_server_schedule_reboot(uint32_t delay_ms) {
	static esp_timer_handle_t s_reboot_timer = NULL;

	if (!s_reboot_timer) {
		const esp_timer_create_args_t args = {
			.callback = _reboot_timer_cb,
			.name     = "reboot",
		};
		if (esp_timer_create(&args, &s_reboot_timer) != ESP_OK) {
			s_reboot_timer = NULL;
			return false;
		}
	}
	/* Already armed by an earlier request — that reboot is coming, so this
	 * one is satisfied too. Re-arming a running one-shot would error. */
	if (esp_timer_is_active(s_reboot_timer)) return true;

	return esp_timer_start_once(s_reboot_timer, (uint64_t)delay_ms * 1000) == ESP_OK;
}

esp_err_t web_server_send_json(httpd_req_t *req, cJSON *root) {
	char *json = root ? cJSON_PrintUnformatted(root) : NULL;
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	esp_err_t r = httpd_resp_sendstr(req, json);
	cJSON_free(json);
	return r;
}

/* ── Path-safety check for user-supplied names (no traversal) ──────────── */

/* The < 0x20 deny check needs an unsigned-char cast: the standard says
 * `char`'s signedness is implementation-defined, so signed-char compilers
 * (host gcc on x86) read high-ASCII bytes (0x80..0xFF, e.g. UTF-8
 * continuation bytes) as negative ints, which are always < 0x20 → byte
 * gets rejected. Unsigned-char compilers (some ARM toolchains) read them
 * as 128..255 → byte passes. Without the cast, the same firmware compiled
 * with two different `char` signedness settings would classify the same
 * UTF-8 name as "safe" or "unsafe" depending on the build. Casting to
 * `unsigned char` pins the behaviour: UTF-8 (and any other byte ≥ 0x20)
 * passes the control-char check, lifecycle is then enforced by the
 * caller's filesystem layer.
 *
 * Same fix applied to web_server_filename_is_safe below for the same
 * reason. See tests/native/test_web_path_safety.c. */

bool web_server_name_is_safe(const char *name) {
	if (!name || !name[0]) return false;
	for (const char *p = name; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '/' || c == '\\' || c == '.' || c < 0x20) return false;
	}
	return true;
}

/* Like web_server_name_is_safe but allows a single dot for file extension (e.g. ".csv") */
bool web_server_filename_is_safe(const char *name) {
	if (!name || !name[0]) return false;
	for (const char *p = name; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '/' || c == '\\' || c < 0x20) return false;
	}
	/* Reject ".." sequences */
	if (strstr(name, "..")) return false;
	return true;
}

/* In-place percent-decode of a query-string value: "%XX" -> the byte it
 * encodes, "+" -> space. httpd_query_key_value() hands back the raw,
 * percent-encoded value, so any name with a space (e.g. "Fugaz One",
 * "Manrope Bold") arrives as "Fugaz%20One" and never matches the file on
 * LittleFS. Decode it before the path-safety check + fopen path build.
 *
 * Decoding never grows the string (every escape collapses to one byte), so
 * decoding in place is safe. A malformed "%" escape (truncated or non-hex)
 * is copied through verbatim rather than dropped — the subsequent
 * web_server_name_is_safe() check still rejects anything dangerous. */
void web_server_url_decode(char *s) {
	if (!s) return;
	char *dst = s;
	for (char *src = s; *src; ) {
		if (*src == '%' && isxdigit((unsigned char)src[1]) &&
		    isxdigit((unsigned char)src[2])) {
			char hex[3] = {src[1], src[2], '\0'};
			*dst++ = (char)strtol(hex, NULL, 16);
			src += 3;
		} else if (*src == '+') {
			*dst++ = ' ';
			src++;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
}

int web_server_recv_body_raw(httpd_req_t *req, char *buf, size_t cap) {
	size_t want = req->content_len;
	if (want == 0 || want >= cap) return -1;
	size_t total = 0;
	int timeouts = 0;
	while (total < want) {
		int r = httpd_req_recv(req, buf + total, want - total);
		if (r == HTTPD_SOCK_ERR_TIMEOUT) {
			/* Bound consecutive timeouts so a stalled or silently-dead client
			 * can't spin this loop forever and wedge the single httpd task
			 * (every web endpoint would then hang). Each timeout is the socket
			 * recv-wait interval (~5 s), so a few in a row is a generous
			 * deadline; any actual progress resets the counter. */
			if (++timeouts >= WEB_RECV_MAX_TIMEOUTS) return -1;
			continue;
		}
		if (r <= 0) return -1;
		total += (size_t)r;
		timeouts = 0;
	}
	buf[total] = '\0';
	return (int)total;
}

esp_err_t web_server_recv_body(httpd_req_t *req, char *buf, size_t cap) {
	if (req->content_len == 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
		return ESP_FAIL;
	}
	if (req->content_len >= cap) {
		/* Truncating JSON just produces a confusing parse error downstream —
		 * reject oversize bodies outright with the real reason. Drain first so
		 * the client receives this 413 cleanly instead of a TCP reset. */
		web_server_drain_body(req, WEB_DRAIN_CAP);
		httpd_resp_set_status(req, "413 Payload Too Large");
		httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"body_too_large\"}");
		return ESP_FAIL;
	}
	if (web_server_recv_body_raw(req, buf, cap) < 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
		return ESP_FAIL;
	}
	return ESP_OK;
}

/* HTTP handler for the main page — serves the gzipped web/index.html.
 *
 * Every browser shipped this decade negotiates gzip transparently, and
 * captive-portal probes that hit "/" (vs. the dedicated captive routes in
 * web_server_captive.c) all support it too. No Accept-Encoding sniffing —
 * we just always send gzip. If a non-gzip client ever shows up, the fix is
 * to add the negotiation here, not to ship a 5× larger payload by default.
 *
 * Cache-Control: no-cache makes the browser revalidate every page load.
 * Cheap on a local network and means firmware updates surface the new UI
 * immediately. */
static esp_err_t index_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html; charset=UTF-8");
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	size_t len = index_html_gz_end - index_html_gz_start;
	return httpd_resp_send(req, (const char *)index_html_gz_start, len);
}

// URI handlers
static const httpd_uri_t index_uri = {
	.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};

/* Favicon — embedded ICO (multi-size 16/32/48). Cached for a day so
 * browsers don't re-fetch on every page load. */
static esp_err_t favicon_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "image/x-icon");
	httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
	size_t len = favicon_ico_end - favicon_ico_start;
	return httpd_resp_send(req, (const char *)favicon_ico_start, len);
}
static const httpd_uri_t favicon_uri = {
	.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler, .user_ctx = NULL};

/* image/font/storage/SD endpoints moved to web_server_assets.c */


/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *  CORS Preflight Handler â€” responds to OPTIONS requests from cross-origin
 *  desktop apps (Tauri) so that POST requests with Content-Type: application/json
 *  pass the browser's preflight check.
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */
static esp_err_t cors_preflight_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
	httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
	httpd_resp_set_status(req, "204 No Content");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/* URI registration tally â€” REGISTER_URI macro is in web_server_internal.h.
 * These globals are declared extern there so domain register() functions
 * that include the header can increment the same counters. */
int web_server_uri_register_attempts = 0;
int web_server_uri_register_failures = 0;

esp_err_t web_server_start(void) {
	if (server != NULL) {
		ESP_LOGW(TAG, "Web server already running");
		return ESP_OK;
	}

	web_server_uri_register_attempts = 0;
	web_server_uri_register_failures = 0;

	ESP_LOGI(TAG, "Starting web server (%u B internal DMA free, largest block %u B)",
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = WEB_SERVER_PORT;
	config.stack_size = 4096; /* IDF default; 5120 no longer fits alongside BLE */
	/* Pin httpd to core 0. LVGL runs on core 1; if httpd also lands on
	 * core 1 and then jpeg_enc_process() runs for 10+ seconds (happens
	 * under load: CONTROL mode + stream + widget rebuild + layout save
	 * in parallel saturates PSRAM bandwidth), it preempts the LVGL task
	 * and starves IDLE1 â†’ task-watchdog trip â†’ eventual reboot. With
	 * httpd on core 0 the heavy encode work stays out of LVGL's way,
	 * and IDLE0 is less loaded than IDLE1. Observed in serial log:
	 * back-to-back WDT hits during /api/screenshot encode stalls. */
	config.core_id    = 0;
	/* 192 slots: 153 actual REGISTER_URI calls today (count with
	 * `grep -rn "REGISTER_URI(server" main/net/ | wc -l`), leaving ~39 slots
	 * of headroom -- re-count before adding new endpoints and bump this
	 * cap if headroom is running low. Was 160 against a count of 148, and
	 * the CAN gateway's three endpoints took that to 7 slots spare, which is
	 * one feature away from the silent failure described below.
	 * ESP-IDF silently drops handlers registered
	 * past max_uri_handlers -- when we ran with 80, the last ~6 POST/OPTIONS
	 * handlers fell through to the wildcard CORS preflight and returned 405
	 * (e.g. `/api/signal/simulate` POST). Each slot is ~32 bytes of static
	 * RAM, so 192 costs ~6 KB. The REGISTER_URI macro tallies failures and
	 * logs at the end of web_server_start; a non-zero tally in the boot log
	 * means we hit the cap and need to bump it again. */
	config.max_uri_handlers = 192;
	config.max_resp_headers = 8;
	config.lru_purge_enable = true;
	config.recv_wait_timeout = 30; /* 30s for image uploads */
	config.send_wait_timeout = 30;
	config.uri_match_fn = httpd_uri_match_wildcard;

	ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

	esp_err_t ret = httpd_start(&server, &config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
		return ret;
	}

	/* Register CORS preflight handler for all API OPTIONS requests */
	static const httpd_uri_t cors_options_uri = {
		.uri = "/api/*",
		.method = HTTP_OPTIONS,
		.handler = cors_preflight_handler,
		.user_ctx = NULL,
	};
	REGISTER_URI(server, &cors_options_uri);

	/* Captive portal probe URIs â€” register before any wildcard handler
	 * so the specific paths aren't shadowed. */
	web_server_captive_register(server);

	// Register URI handlers
	REGISTER_URI(server, &index_uri);
	REGISTER_URI(server, &favicon_uri);
	web_server_capture_register(server);
	web_server_touch_register(server);
	web_server_gear_register(server);
	web_server_layout_register(server);
	web_server_assets_register(server);
	web_server_system_register(server);
	web_server_ota_register(server);
	web_server_wifi_register(server);
	web_server_logger_register(server);
	web_server_signals_register(server);
	web_server_obd2_register(server);
	web_server_channels_register(server);
	web_server_can_register(server);
	web_server_keypad_register(server);
	web_server_test_register(server);

	/* Final registration tally. If any registration failed (almost always
	 * because max_uri_handlers is too low), shout loudly so the developer
	 * who just added an endpoint notices in `idf.py monitor` instead of
	 * chasing a phantom 405 in DevTools later. */
	if (web_server_uri_register_failures > 0) {
		ESP_LOGE(TAG,
				 "URI registration: %d/%d FAILED â€” bump max_uri_handlers "
				 "(currently %d) in web_server_start. Failed endpoints will "
				 "return 405 via the OPTIONS wildcard.",
				 web_server_uri_register_failures, web_server_uri_register_attempts,
				 (int)config.max_uri_handlers);
	} else {
		ESP_LOGI(TAG, "URI registration: %d handlers registered (cap %d)",
				 web_server_uri_register_attempts, (int)config.max_uri_handlers);
	}

	ESP_LOGI(TAG, "Web server started successfully");
	return ESP_OK;
}

esp_err_t web_server_stop(void) {
	if (server == NULL) {
		ESP_LOGW(TAG, "Web server not running");
		return ESP_OK;
	}

	esp_err_t ret = httpd_stop(server);
	if (ret == ESP_OK) {
		server = NULL;
		ESP_LOGI(TAG, "Web server stopped");
	} else {
		ESP_LOGE(TAG, "Failed to stop web server: %s", esp_err_to_name(ret));
	}

	return ret;
}

bool web_server_is_running(void) { return (server != NULL); }
