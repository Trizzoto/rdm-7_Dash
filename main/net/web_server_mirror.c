/* web_server_mirror.c — baked-in WASM live mirror.
 *
 * Serves a PC-side live view of the dash that renders locally at full
 * frame rate instead of streaming pixels: the browser loads the LVGL
 * WASM preview engine FROM THE DEVICE (works in hotspot mode, no
 * internet), fetches the active layout + device fonts/images, then polls
 * /api/signals/values and injects them — the PC renders, the dash pays
 * ~15 KB/s of JSON instead of JPEG encode + capture CPU.
 *
 *   GET  /mirror              embedded mirror.html (gzipped at build)
 *   GET  /mirror/index.js     /lfs/mirror/index.js.gz   (engine glue)
 *   GET  /mirror/index.wasm   /lfs/mirror/index.wasm.gz (engine, ~750 KB gz)
 *   POST /api/mirror/upload?name=index.js.gz|index.wasm.gz   one-time install
 *   GET  /api/mirror/status   {"installed":bool,"js_bytes":n,"wasm_bytes":n}
 *
 * The engine binaries live on LittleFS, not in the app image — the app
 * partition hasn't the ~850 KB of headroom, and LittleFS survives OTA so
 * the mirror stays installed across firmware updates. Install once over
 * WiFi with tools/install_mirror.ps1 (artifacts build from the
 * rdm7-wasm-editor repo); /mirror explains this when not installed. */

#include "web_server_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"

static const char *TAG = "web_mirror";

#define MIRROR_DIR       "/lfs/mirror"
#define MIRROR_MAX_BYTES (4 * 1024 * 1024) /* upload cap, well above the ~2 MB engine */
#define MIRROR_CHUNK     8192

extern const uint8_t mirror_html_gz_start[] asm("_binary_mirror_html_gz_start");
extern const uint8_t mirror_html_gz_end[]   asm("_binary_mirror_html_gz_end");

/* Only these two artifacts are ever served/written — fixed allow-list, so
 * no user-supplied path ever reaches the filesystem. */
static const char *MIRROR_FILES[] = { "index.js.gz", "index.wasm.gz" };

static bool _mirror_name_ok(const char *name) {
	for (size_t i = 0; i < sizeof(MIRROR_FILES) / sizeof(MIRROR_FILES[0]); i++)
		if (strcmp(name, MIRROR_FILES[i]) == 0) return true;
	return false;
}

static long _file_size(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return -1;
	return (long)st.st_size;
}

/* ── GET /mirror — embedded page ─────────────────────────────────────── */

static esp_err_t mirror_page_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html; charset=UTF-8");
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	size_t len = mirror_html_gz_end - mirror_html_gz_start;
	return httpd_resp_send(req, (const char *)mirror_html_gz_start, len);
}

/* ── Engine binaries from LittleFS (pre-gzipped, chunk-streamed) ─────── */

static esp_err_t _send_gz_file(httpd_req_t *req, const char *fname,
                               const char *content_type) {
	char path[64];
	snprintf(path, sizeof(path), MIRROR_DIR "/%s", fname);
	FILE *f = fopen(path, "rb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
		                    "Mirror engine not installed");
		return ESP_FAIL;
	}

	uint8_t *buf = heap_caps_malloc(MIRROR_CHUNK, MALLOC_CAP_SPIRAM);
	if (!buf) buf = malloc(MIRROR_CHUNK);
	if (!buf) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, content_type);
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	/* The engine binaries only change on an explicit re-install; let the
	 * browser cache them for a day so /mirror reloads are instant. */
	httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");

	esp_err_t res = ESP_OK;
	size_t n;
	while ((n = fread(buf, 1, MIRROR_CHUNK, f)) > 0) {
		if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
			res = ESP_FAIL; /* client went away — abort quietly */
			break;
		}
	}
	fclose(f);
	free(buf);
	if (res == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
	return res;
}

static esp_err_t mirror_js_handler(httpd_req_t *req) {
	return _send_gz_file(req, "index.js.gz", "application/javascript");
}

static esp_err_t mirror_wasm_handler(httpd_req_t *req) {
	return _send_gz_file(req, "index.wasm.gz", "application/wasm");
}

/* ── POST /api/mirror/upload?name=<file> ─────────────────────────────── */

static esp_err_t mirror_upload_handler(httpd_req_t *req) {
	char query[64] = {0};
	char name[32] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
	    httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
	    !_mirror_name_ok(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "name must be index.js.gz or index.wasm.gz");
		return ESP_FAIL;
	}
	if (req->content_len <= 0 || req->content_len > MIRROR_MAX_BYTES) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
		return ESP_FAIL;
	}

	mkdir(MIRROR_DIR, 0755); /* idempotent */

	/* Stream to a temp file, fsync, rename — the standard atomic-write
	 * idiom so a dropped upload never leaves a truncated engine that the
	 * browser would then fail to instantiate. */
	char tmp_path[64], final_path[64];
	snprintf(tmp_path, sizeof(tmp_path), MIRROR_DIR "/%s.tmp", name);
	snprintf(final_path, sizeof(final_path), MIRROR_DIR "/%s", name);

	FILE *f = fopen(tmp_path, "wb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
		                    "Cannot open temp file");
		return ESP_FAIL;
	}

	uint8_t *buf = heap_caps_malloc(MIRROR_CHUNK, MALLOC_CAP_SPIRAM);
	if (!buf) buf = malloc(MIRROR_CHUNK);
	if (!buf) {
		fclose(f);
		remove(tmp_path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}

	int remaining = req->content_len;
	bool ok = true;
	while (remaining > 0) {
		int r = httpd_req_recv(req, (char *)buf,
		                       remaining < MIRROR_CHUNK ? remaining : MIRROR_CHUNK);
		if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
		if (r <= 0) { ok = false; break; }
		if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { ok = false; break; }
		remaining -= r;
	}
	free(buf);
	if (ok) {
		fflush(f);
		fsync(fileno(f));
	}
	fclose(f);

	if (!ok) {
		remove(tmp_path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
		                    "Upload truncated");
		return ESP_FAIL;
	}
	remove(final_path); /* rename() on LittleFS won't clobber */
	if (rename(tmp_path, final_path) != 0) {
		remove(tmp_path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "mirror asset installed: %s (%ld bytes)",
	         name, _file_size(final_path));
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

/* ── GET /api/mirror/status ──────────────────────────────────────────── */

static esp_err_t mirror_status_handler(httpd_req_t *req) {
	long js = _file_size(MIRROR_DIR "/index.js.gz");
	long wasm = _file_size(MIRROR_DIR "/index.wasm.gz");
	char body[128];
	snprintf(body, sizeof(body),
	         "{\"installed\":%s,\"js_bytes\":%ld,\"wasm_bytes\":%ld}",
	         (js > 0 && wasm > 0) ? "true" : "false",
	         js > 0 ? js : 0, wasm > 0 ? wasm : 0);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_sendstr(req, body);
}

/* ── Registration ────────────────────────────────────────────────────── */

static const httpd_uri_t mirror_page_uri = {
	.uri = "/mirror", .method = HTTP_GET,
	.handler = mirror_page_handler, .user_ctx = NULL};
static const httpd_uri_t mirror_js_uri = {
	.uri = "/mirror/index.js", .method = HTTP_GET,
	.handler = mirror_js_handler, .user_ctx = NULL};
static const httpd_uri_t mirror_wasm_uri = {
	.uri = "/mirror/index.wasm", .method = HTTP_GET,
	.handler = mirror_wasm_handler, .user_ctx = NULL};
static const httpd_uri_t mirror_upload_uri = {
	.uri = "/api/mirror/upload", .method = HTTP_POST,
	.handler = mirror_upload_handler, .user_ctx = NULL};
static const httpd_uri_t mirror_status_uri = {
	.uri = "/api/mirror/status", .method = HTTP_GET,
	.handler = mirror_status_handler, .user_ctx = NULL};

void web_server_mirror_register(httpd_handle_t server) {
	REGISTER_URI(server, &mirror_page_uri);
	REGISTER_URI(server, &mirror_js_uri);
	REGISTER_URI(server, &mirror_wasm_uri);
	REGISTER_URI(server, &mirror_upload_uri);
	REGISTER_URI(server, &mirror_status_uri);
}
