#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
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

/* Send a structured 413 Payload-Too-Large JSON error.
 *
 * Layout JSON > LAYOUT_MAX_FILE_BYTES is a real failure mode in the editor,
 * historically masked by an opaque 400. This helper returns a body the
 * editor can parse and surface inline:
 *     { "ok":false, "error":"layout_too_large", "max":32768, "actual":N }
 */
esp_err_t web_server_send_layout_too_large(httpd_req_t *req, size_t actual) {
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

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = WEB_SERVER_PORT;
	config.stack_size = 5120;
	/* Pin httpd to core 0. LVGL runs on core 1; if httpd also lands on
	 * core 1 and then jpeg_enc_process() runs for 10+ seconds (happens
	 * under load: CONTROL mode + stream + widget rebuild + layout save
	 * in parallel saturates PSRAM bandwidth), it preempts the LVGL task
	 * and starves IDLE1 â†’ task-watchdog trip â†’ eventual reboot. With
	 * httpd on core 0 the heavy encode work stays out of LVGL's way,
	 * and IDLE0 is less loaded than IDLE1. Observed in serial log:
	 * back-to-back WDT hits during /api/screenshot encode stalls. */
	config.core_id    = 0;
	/* 160 slots: 106 actual REGISTER_URI calls today (count with
	 * `grep -rn "REGISTER_URI\b" main/net/ | wc -l`), leaving ~54 slots
	 * of headroom for the next round of endpoint adds before we have to
	 * think about this again. ESP-IDF silently drops handlers registered
	 * past max_uri_handlers -- when we ran with 80, the last ~6 POST/OPTIONS
	 * handlers fell through to the wildcard CORS preflight and returned 405
	 * (e.g. `/api/signal/simulate` POST). Each slot is ~32 bytes of static
	 * RAM, so 160 costs ~5 KB. The REGISTER_URI macro tallies failures and
	 * logs at the end of web_server_start; a non-zero tally in the boot log
	 * means we hit the cap and need to bump it again. */
	config.max_uri_handlers = 160;
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
