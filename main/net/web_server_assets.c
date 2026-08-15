/* web_server_assets.c — image, font, storage-info, and SD-card endpoints
 *
 * Endpoints:
 *   POST /api/image/upload    upload RDMIMG to LittleFS
 *   GET  /api/image/list      list images with dimensions + size
 *   POST /api/image/delete    delete image from LittleFS
 *   GET  /api/image/data      download raw RDMIMG
 *   POST /api/font/upload     upload TTF to LittleFS + register in font manager
 *   GET  /api/font/list       list font family names
 *   POST /api/font/delete     remove font from LittleFS + font manager
 *   GET  /api/font/data       download raw TTF
 *   GET  /api/storage/info    LittleFS + SD total/used/free bytes
 *   GET  /api/sd/status       SD mount status + space info
 *   GET  /api/sd/files        all SD files by category
 *   POST /api/sd/copy         copy file between LittleFS <-> SD
 *   POST /api/sd/delete       delete file from SD */
#include "web_server_internal.h"
#include "sd_file_ops.h"
#include "cJSON.h"
#include "can/rdm_bus.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "storage/sd_manager.h"
#include "storage/boot_assets.h"
#include "widgets/font_manager.h"
#include "system/rdm_lv_async.h"   /* rdm_async_call for atomic font re-resolve */
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>        /* fsync, fileno */

#include "widgets/track_map_geo.h"   /* one point cap, shared with the loader */

static const char *TAG = "web_server_assets";

/* ── Image endpoints ─────────────────────────────────────────────────────── */

#define LFS_IMAGE_DIR "/lfs/images"
/* Max RDMIMG size: SCREEN_W * SCREEN_H * 3 bytes/pixel + 12-byte header, rounded up */
#define IMAGE_MAX_SIZE (1200 * 1024)
/* Streaming upload chunk: bounds how much of the body we hold in RAM at once.
 * The body is written straight to flash chunk-by-chunk, so a multi-MB image
 * never needs a whole-file PSRAM buffer (which used to OOM at ~1.2 MB free). */
#define IMAGE_UPLOAD_CHUNK (8 * 1024)
/* Streaming download chunk: same idea for the GET side. The image + font data
 * handlers fread one buffer at a time and httpd_resp_send_chunk() it, so a
 * download never needs a whole-file PSRAM buffer either. Shared by both. */
#define ASSET_DOWNLOAD_CHUNK (8 * 1024)

static void _ensure_image_dir(void) {
	struct stat st;
	if (stat(LFS_IMAGE_DIR, &st) != 0)
		mkdir(LFS_IMAGE_DIR, 0755);
}

/* POST /api/image/upload?name=<name>
 * Body: raw RDMIMG binary data */
static esp_err_t image_upload_handler(httpd_req_t *req) {
	_ensure_image_dir();

	/* Extract name from query string */
	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}

	/* httpd_query_key_value() returns the raw percent-encoded value — decode it
	 * so names with spaces ("Fugaz One", "Manrope Bold") resolve to the right
	 * file. Must run before the safety check + path build. */
	web_server_url_decode(name);

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	if (boot_assets_is_protected_image(name)) {
		httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
		                    "Cannot overwrite the built-in image — choose another name");
		return ESP_FAIL;
	}

	size_t content_len = req->content_len;
	if (content_len < 12 || content_len > IMAGE_MAX_SIZE) {
		/* Drain (bounded) so an oversize upload reads the 400, not a TCP reset. */
		if (content_len > IMAGE_MAX_SIZE) web_server_drain_body(req, 1024 * 1024);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
		return ESP_FAIL;
	}

	/* Free-space pre-check: the declared length is known up front, so reject a
	 * too-big upload before opening the file. (The stream below also fails
	 * safely — staged in .tmp, never published — if the FS fills mid-write.) */
	size_t total_bytes = 0, used_bytes = 0;
	if (esp_littlefs_info("littlefs", &total_bytes, &used_bytes) == ESP_OK) {
		size_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;
		if (content_len > free_bytes) {
			char err_msg[128];
			snprintf(err_msg, sizeof(err_msg),
					 "Not enough storage: need %u KB, only %u KB free",
					 (unsigned)(content_len / 1024), (unsigned)(free_bytes / 1024));
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
			return ESP_FAIL;
		}
	}

	/* Stream the body straight to flash in fixed-size chunks instead of
	 * buffering the whole image in PSRAM. The old handler allocated one
	 * content_len-sized PSRAM buffer, so a 1.15 MB full-screen .rdmimg OOM'd
	 * whenever a background image already held the ~1.2 MB of free PSRAM. Here
	 * the only RAM held is one IMAGE_UPLOAD_CHUNK buffer, so uploads succeed
	 * regardless of the active layout.
	 *
	 * Durability mirrors channel_manager.c's atomic save: write into a .tmp,
	 * fsync it, keep the previous image as .bak across the rename window, then
	 * atomically rename the .tmp over the live file. rename() is atomic on
	 * LittleFS, so a power loss leaves either the old or the new image intact —
	 * never a half-written .rdmimg. */
	char path[96], tmp_path[112], bak_path[112];
	snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	snprintf(bak_path, sizeof(bak_path), "%s.bak", path);

	uint8_t *chunk = malloc(IMAGE_UPLOAD_CHUNK);
	if (!chunk) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
		return ESP_FAIL;
	}

	FILE *f = fopen(tmp_path, "wb");
	if (!f) {
		free(chunk);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}

	/* recv -> validate RDMI magic on the first 12 bytes -> write, repeat.
	 * On any failure we stop, set err_status, and clean up the .tmp below. */
	size_t received = 0;
	uint8_t header[12];
	size_t header_have = 0;
	bool ok = true;
	const char *err_status = NULL;
	int http_err = HTTPD_500_INTERNAL_SERVER_ERROR;

	while (received < content_len) {
		size_t want = content_len - received;
		if (want > IMAGE_UPLOAD_CHUNK) want = IMAGE_UPLOAD_CHUNK;
		int ret = httpd_req_recv(req, (char *)chunk, want);
		if (ret <= 0) {
			ok = false;
			err_status = "Receive failed";
			break;
		}

		/* Accumulate the 12-byte header across however many segments it spans
		 * (content_len >= 12 is guaranteed above) and validate before we
		 * commit the bulk of the file. */
		if (header_have < sizeof(header)) {
			size_t copy = sizeof(header) - header_have;
			if (copy > (size_t)ret) copy = (size_t)ret;
			memcpy(header + header_have, chunk, copy);
			header_have += copy;
			if (header_have == sizeof(header) && memcmp(header, "RDMI", 4) != 0) {
				ok = false;
				err_status = "Invalid RDMIMG format";
				http_err = HTTPD_400_BAD_REQUEST;
				break;
			}
		}

		size_t nw = fwrite(chunk, 1, (size_t)ret, f);
		if (nw != (size_t)ret) {
			ok = false;
			err_status = "Write incomplete";
			break;
		}
		received += (size_t)ret;
	}
	free(chunk);

	/* Flush stdio + commit to flash before the rename so the published file is
	 * durable, then close exactly once regardless of the loop's outcome. */
	bool flush_ok = (fflush(f) == 0 && fsync(fileno(f)) == 0);
	if (fclose(f) != 0) flush_ok = false;
	if (ok && !flush_ok) {
		ok = false;
		err_status = "Write incomplete";
	}

	if (!ok) {
		remove(tmp_path);
		httpd_resp_send_err(req, http_err, err_status ? err_status : "Upload failed");
		return ESP_FAIL;
	}

	/* Atomic publish. Keep the previous image as .bak across the (microsecond)
	 * rename window so a crash can't lose it, then drop the .bak so we don't
	 * permanently double the storage of a multi-MB image. */
	rename(path, bak_path);
	if (rename(tmp_path, path) != 0) {
		rename(bak_path, path);  /* restore the previous image if we had one */
		remove(tmp_path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}
	remove(bak_path);  /* best-effort space reclaim; no-op on a first upload */

	ESP_LOGI(TAG, "Uploaded image '%s' (%u bytes, streamed)", name, (unsigned)received);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t image_upload_uri = {.uri = "/api/image/upload",
                                              .method = HTTP_POST,
                                              .handler = image_upload_handler,
                                              .user_ctx = NULL};

/* GET /api/image/list — returns JSON array of {name, width, height, size} */
static esp_err_t image_list_handler(httpd_req_t *req) {
	_ensure_image_dir();

	cJSON *arr = cJSON_CreateArray();
	DIR *d = opendir(LFS_IMAGE_DIR);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t flen = strlen(de->d_name);
			if (flen <= 7 || strcmp(de->d_name + flen - 7, ".rdmimg") != 0)
				continue;

			char path[80];
			snprintf(path, sizeof(path), "%s/%s", LFS_IMAGE_DIR, de->d_name);

			/* Read header to get dimensions */
			FILE *f = fopen(path, "rb");
			if (!f) continue;
			uint8_t hdr[12];
			size_t nr = fread(hdr, 1, 12, f);
			fseek(f, 0, SEEK_END);
			long file_size = ftell(f);
			fclose(f);

			if (nr < 12 || memcmp(hdr, "RDMI", 4) != 0)
				continue;

			uint16_t w = hdr[4] | (hdr[5] << 8);
			uint16_t h = hdr[6] | (hdr[7] << 8);

			/* Strip .rdmimg extension for name */
			char img_name[32];
			size_t copy = flen - 7;
			if (copy >= sizeof(img_name)) copy = sizeof(img_name) - 1;
			memcpy(img_name, de->d_name, copy);
			img_name[copy] = '\0';

			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "name", img_name);
			cJSON_AddNumberToObject(obj, "width", w);
			cJSON_AddNumberToObject(obj, "height", h);
			cJSON_AddNumberToObject(obj, "size", file_size);
			cJSON_AddItemToArray(arr, obj);
		}
		closedir(d);
	}

	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, arr);
}

static const httpd_uri_t image_list_uri = {.uri = "/api/image/list",
                                            .method = HTTP_GET,
                                            .handler = image_list_handler,
                                            .user_ctx = NULL};

/* POST /api/image/delete?name=<name> */
static esp_err_t image_delete_handler(httpd_req_t *req) {
	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}

	/* httpd_query_key_value() returns the raw percent-encoded value — decode it
	 * so names with spaces ("Fugaz One", "Manrope Bold") resolve to the right
	 * file. Must run before the safety check + path build. */
	web_server_url_decode(name);

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	if (boot_assets_is_protected_image(name)) {
		httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
		                    "Built-in image cannot be deleted");
		return ESP_FAIL;
	}

	char path[80];
	snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);
	if (remove(path) != 0) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Deleted image '%s'", name);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t image_delete_uri = {.uri = "/api/image/delete",
                                              .method = HTTP_POST,
                                              .handler = image_delete_handler,
                                              .user_ctx = NULL};

/* Stream a file to the client in fixed-size chunks via HTTP chunked transfer
 * encoding, instead of buffering the whole file in one PSRAM allocation. The
 * old image/font download handlers did heap_caps_malloc(file_size) + a single
 * httpd_resp_send(), so a 1.15 MB full-screen .rdmimg returned "500 Out of
 * memory" whenever a background image already held the ~1.2 MB of free PSRAM,
 * and a 4 MB TTF was worse. Here the only RAM held is one ASSET_DOWNLOAD_CHUNK
 * buffer regardless of file size. The caller has already validated the name and
 * built the path; we own opening the file, the size sanity check, and the
 * response.
 *
 * Returns ESP_OK on a fully-sent file. On a pre-send failure (not found / bad
 * size / OOM) it sends the matching httpd error and returns ESP_FAIL. If a read
 * or send fails mid-stream the headers are already out, so it just aborts the
 * connection (returns ESP_FAIL without the terminating zero-length chunk) — the
 * client then sees a truncated chunked stream rather than a silent short file. */
static esp_err_t _send_file_chunked(httpd_req_t *req, const char *path,
                                    long max_size, const char *not_found_msg) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, not_found_msg);
		return ESP_FAIL;
	}

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0 || file_size > max_size) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid file size");
		return ESP_FAIL;
	}

	uint8_t *chunk = malloc(ASSET_DOWNLOAD_CHUNK);
	if (!chunk) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
		return ESP_FAIL;
	}

	/* Headers go out with the first chunk — set them before the loop. */
	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	long remaining = file_size;
	esp_err_t res = ESP_OK;
	while (remaining > 0) {
		size_t want = (remaining > ASSET_DOWNLOAD_CHUNK)
		                  ? ASSET_DOWNLOAD_CHUNK : (size_t)remaining;
		size_t nread = fread(chunk, 1, want, f);
		if (nread == 0) {
			res = ESP_FAIL;  /* short read before EOF — abort mid-stream */
			break;
		}
		if (httpd_resp_send_chunk(req, (const char *)chunk, nread) != ESP_OK) {
			res = ESP_FAIL;  /* client gone / send failed — abort mid-stream */
			break;
		}
		remaining -= (long)nread;
	}

	free(chunk);
	fclose(f);

	if (res == ESP_OK)
		httpd_resp_send_chunk(req, NULL, 0);  /* terminate the chunked response */
	return res;
}

/* GET /api/image/data?name=<name> — return raw RDMIMG binary (streamed) */
static esp_err_t image_data_handler(httpd_req_t *req) {
	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}

	/* httpd_query_key_value() returns the raw percent-encoded value — decode it
	 * so names with spaces ("Fugaz One", "Manrope Bold") resolve to the right
	 * file. Must run before the safety check + path build. */
	web_server_url_decode(name);

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	char path[80];
	snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);
	return _send_file_chunked(req, path, IMAGE_MAX_SIZE, "Image not found");
}

static const httpd_uri_t image_data_uri = {.uri = "/api/image/data",
                                            .method = HTTP_GET,
                                            .handler = image_data_handler,
                                            .user_ctx = NULL};

/* ── Font endpoints ──────────────────────────────────────────────────────── */

#define LFS_FONT_DIR  "/lfs/fonts"
/* FONT_MAX_FILE_SIZE comes from font_manager.h (shared with the loader). The
 * upload cap MUST match the loader's limit, or a font in (limit, old-4MB]
 * uploads OK but silently fails to load (and large ones OOM the upload malloc). */

static void _ensure_font_dir(void) {
	struct stat st;
	if (stat(LFS_FONT_DIR, &st) != 0)
		mkdir(LFS_FONT_DIR, 0755);
}

/* ── Atomic font apply (LVGL task) ──────────────────────────────────────────
 * font_manager_add_family/remove_family lv_tiny_ttf_destroy() the old font
 * instances, but live label widgets still hold that lv_font_t* in their style.
 * If we mutate the font manager and only later (or on a debounced reload)
 * rebuild the widgets, the render task draws a label with a freed font in
 * between -> use-after-free. These callbacks run on the LVGL task via
 * rdm_async_call and do the mutation AND the full screen rebuild in ONE
 * uninterrupted callback (lv_async callbacks complete before the render pass),
 * so no widget is ever drawn holding a destroyed font. */
typedef struct {
	char     name[FONT_NAME_LEN];
	uint8_t *data;   /* owned; freed here */
	size_t   size;
} font_apply_t;

static void _font_add_and_reload(void *arg) {
	font_apply_t *p = (font_apply_t *)arg;
	if (p) {
		if (!font_manager_add_family(p->name, p->data, p->size))
			ESP_LOGE(TAG, "font apply: add_family('%s') failed", p->name);
		free(p->data);
		web_server_rebuild_active_screen();   /* re-resolve every widget's font */
		free(p);
	}
}

static void _font_remove_and_reload(void *arg) {
	char *name = (char *)arg;
	if (name) {
		font_manager_remove_family(name);
		web_server_rebuild_active_screen();
		free(name);
	}
}

/* POST /api/font/upload?name=<family_name>
 * Body: raw TTF binary data */
static esp_err_t font_upload_handler(httpd_req_t *req) {
	_ensure_font_dir();

	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}

	/* httpd_query_key_value() returns the raw percent-encoded value — decode it
	 * so names with spaces ("Fugaz One", "Manrope Bold") resolve to the right
	 * file. Must run before the safety check + path build. */
	web_server_url_decode(name);

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	if (boot_assets_is_protected_font(name)) {
		httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
		                    "Cannot overwrite a built-in font — choose another name");
		return ESP_FAIL;
	}

	size_t content_len = req->content_len;
	if (content_len > FONT_MAX_FILE_SIZE) {
		/* Drain (bounded) so the client reads this 400 instead of a TCP reset. */
		web_server_drain_body(req, 1024 * 1024);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "Font too large (512 KB max — subset the TTF first)");
		return ESP_FAIL;
	}
	if (content_len < 12) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
		return ESP_FAIL;
	}

	/* Check free space */
	size_t total_bytes = 0, used_bytes = 0;
	if (esp_littlefs_info("littlefs", &total_bytes, &used_bytes) == ESP_OK) {
		size_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;
		if (content_len > free_bytes) {
			char err_msg[128];
			snprintf(err_msg, sizeof(err_msg),
					 "Not enough storage: need %u KB, only %u KB free",
					 (unsigned)(content_len / 1024), (unsigned)(free_bytes / 1024));
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
			return ESP_FAIL;
		}
	}

	/* Receive into PSRAM. Unlike the image upload (which streams straight to
	 * flash) and the image/font *downloads* (which stream from flash), this one
	 * can't be chunked away: font_manager_add_family() takes the full TTF as one
	 * contiguous (data,size) buffer — it memcpy's it into its own PSRAM copy and
	 * hands that to lv_tiny_ttf for the life of the family — so the whole font
	 * must be assembled in RAM before we can register it. We prefer PSRAM and
	 * only fall back to internal heap. */
	uint8_t *buf = heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
	if (!buf) {
		buf = malloc(content_len);
		if (!buf) {
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
			return ESP_FAIL;
		}
	}

	size_t received = 0;
	while (received < content_len) {
		int ret = httpd_req_recv(req, (char *)buf + received, content_len - received);
		if (ret <= 0) {
			free(buf);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
			return ESP_FAIL;
		}
		received += ret;
	}

	/* Write to LittleFS atomically — stage into .tmp, fsync, keep the previous
	 * font as .bak across the rename window, then rename over the live file.
	 * The old code fopen(path,"wb")'d in place, truncating any existing font of
	 * that name before the first byte and remove()'ing it on a short write —
	 * the atomic guarantee the image path (above) already has, missing here. */
	char path[96], tmp_path[112], bak_path[112];
	snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR, name);
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
	FILE *f = fopen(tmp_path, "wb");
	if (!f) {
		free(buf);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}
	size_t nw = fwrite(buf, 1, received, f);
	bool wok = (nw == received) && (fflush(f) == 0) && (fsync(fileno(f)) == 0);
	if (fclose(f) != 0) wok = false;

	if (!wok) {
		free(buf);
		remove(tmp_path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write incomplete");
		return ESP_FAIL;
	}

	rename(path, bak_path);                 /* keep previous good font */
	if (rename(tmp_path, path) != 0) {
		rename(bak_path, path);             /* restore on failure */
		remove(tmp_path);
		free(buf);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}
	remove(bak_path);                       /* best-effort space reclaim */

	/* Register + re-resolve widgets on the LVGL task. font_manager_add_family()
	 * destroys the replaced family's instances, so it must run on the LVGL task
	 * AND be paired atomically with a widget rebuild (see _font_add_and_reload)
	 * or live labels draw with a freed font. Hand the TTF buffer to the async
	 * callback (which frees it). The file is already on flash, so returning
	 * "ok" before the async apply is safe. */
	font_apply_t *ap = malloc(sizeof(*ap));
	if (!ap) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
	strncpy(ap->name, name, sizeof(ap->name) - 1);
	ap->name[sizeof(ap->name) - 1] = '\0';
	ap->data = buf;         /* ownership transferred to the callback */
	ap->size = received;
	rdm_async_call(_font_add_and_reload, ap);

	ESP_LOGI(TAG, "Uploaded font '%s' (%u bytes)", name, (unsigned)received);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t font_upload_uri = {.uri = "/api/font/upload",
                                             .method = HTTP_POST,
                                             .handler = font_upload_handler,
                                             .user_ctx = NULL};

/* GET /api/font/list
 *   default:        ["Family1", "Family2", ...]    (legacy shape, used by
 *                   font pickers / DBC import that only need names)
 *   ?details=1:     [{"name":"Family1","size":12345}, ...]  (file-manager
 *                   shape, lets the Storage Manager show per-font KB) */
static esp_err_t font_list_handler(httpd_req_t *req) {
	bool details = false;
	char query[32];
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
		char v[8];
		if (httpd_query_key_value(query, "details", v, sizeof(v)) == ESP_OK &&
		    (v[0] == '1' || v[0] == 't' || v[0] == 'T')) {
			details = true;
		}
	}

	cJSON *arr = cJSON_CreateArray();
	uint8_t count = font_manager_family_count();
	for (uint8_t i = 0; i < count; i++) {
		const char *fname = font_manager_family_name(i);
		if (!fname) continue;
		if (details) {
			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "name", fname);
			cJSON_AddNumberToObject(obj, "size",
			    (double)font_manager_family_size(i));
			cJSON_AddItemToArray(arr, obj);
		} else {
			cJSON_AddItemToArray(arr, cJSON_CreateString(fname));
		}
	}

	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, arr);
}

static const httpd_uri_t font_list_uri = {.uri = "/api/font/list",
                                           .method = HTTP_GET,
                                           .handler = font_list_handler,
                                           .user_ctx = NULL};

/* POST /api/font/delete?name=<family_name> */
static esp_err_t font_delete_handler(httpd_req_t *req) {
	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}

	/* httpd_query_key_value() returns the raw percent-encoded value — decode it
	 * so names with spaces ("Fugaz One", "Manrope Bold") resolve to the right
	 * file. Must run before the safety check + path build. */
	web_server_url_decode(name);

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	if (boot_assets_is_protected_font(name)) {
		httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
		                    "Built-in font cannot be deleted");
		return ESP_FAIL;
	}

	/* Confirm the font exists (under the lock — reads the family table), then do
	 * the destructive remove + widget rebuild atomically on the LVGL task via
	 * _font_remove_and_reload. Removing here on the httpd task then reloading
	 * later would leave live labels pointing at the destroyed font. */
	bool exists = false;
	if (!rdm_lvgl_lock(2000)) {
		ESP_LOGW(TAG, "font delete: LVGL busy — retry");
		return web_server_send_busy(req);
	}
	uint8_t nfam = font_manager_family_count();
	for (uint8_t i = 0; i < nfam; i++) {
		const char *fn = font_manager_family_name(i);
		if (fn && strcmp(fn, name) == 0) { exists = true; break; }
	}
	rdm_lvgl_unlock();
	if (!exists) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Font not found");
		return ESP_FAIL;
	}

	char *nm = strdup(name);
	if (!nm) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	rdm_async_call(_font_remove_and_reload, nm);   /* remove + rebuild atomically */

	ESP_LOGI(TAG, "Deleted font '%s'", name);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t font_delete_uri = {.uri = "/api/font/delete",
                                             .method = HTTP_POST,
                                             .handler = font_delete_handler,
                                             .user_ctx = NULL};

/* GET /api/font/data?name=<family_name> — return raw TTF binary (streamed) */
static esp_err_t font_data_handler(httpd_req_t *req) {
	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}

	/* httpd_query_key_value() returns the raw percent-encoded value — decode it
	 * so names with spaces ("Fugaz One", "Manrope Bold") resolve to the right
	 * file. Must run before the safety check + path build. */
	web_server_url_decode(name);

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	char path[80];
	snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR, name);
	return _send_file_chunked(req, path, FONT_MAX_FILE_SIZE, "Font not found");
}

static const httpd_uri_t font_data_uri = {.uri = "/api/font/data",
                                           .method = HTTP_GET,
                                           .handler = font_data_handler,
                                           .user_ctx = NULL};

/* ── Storage info + SD card endpoints ───────────────────────────────────── */

/* GET /api/storage/info — returns total/used/free bytes for LittleFS + SD */
static esp_err_t storage_info_handler(httpd_req_t *req) {
	size_t total_bytes = 0, used_bytes = 0;
	esp_err_t err = esp_littlefs_info("littlefs", &total_bytes, &used_bytes);
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot read storage info");
		return ESP_FAIL;
	}
	size_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "total", total_bytes);
	cJSON_AddNumberToObject(root, "used", used_bytes);
	cJSON_AddNumberToObject(root, "free", free_bytes);

	cJSON *sd_obj = cJSON_AddObjectToObject(root, "sd");
	if (sd_manager_is_mounted()) {
		size_t sd_total = 0, sd_used = 0, sd_free = 0;
		if (sd_manager_get_info(&sd_total, &sd_used, &sd_free) == ESP_OK) {
			cJSON_AddBoolToObject(sd_obj, "mounted", true);
			cJSON_AddNumberToObject(sd_obj, "total", sd_total);
			cJSON_AddNumberToObject(sd_obj, "used", sd_used);
			cJSON_AddNumberToObject(sd_obj, "free", sd_free);
		} else {
			cJSON_AddBoolToObject(sd_obj, "mounted", false);
		}
	} else {
		cJSON_AddBoolToObject(sd_obj, "mounted", false);
	}

	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

static const httpd_uri_t storage_info_uri = {
    .uri = "/api/storage/info", .method = HTTP_GET,
    .handler = storage_info_handler, .user_ctx = NULL};

/* GET /api/sd/status — SD mount status + space info */
static esp_err_t sd_status_handler(httpd_req_t *req) {
	cJSON *root = cJSON_CreateObject();
	if (sd_manager_is_mounted()) {
		cJSON_AddBoolToObject(root, "mounted", true);
		size_t total = 0, used = 0, sd_free = 0;
		if (sd_manager_get_info(&total, &used, &sd_free) == ESP_OK) {
			cJSON_AddNumberToObject(root, "total", total);
			cJSON_AddNumberToObject(root, "used", used);
			cJSON_AddNumberToObject(root, "free", sd_free);
		}
	} else {
		cJSON_AddBoolToObject(root, "mounted", false);
	}

	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

static const httpd_uri_t sd_status_uri = {
    .uri = "/api/sd/status", .method = HTTP_GET,
    .handler = sd_status_handler, .user_ctx = NULL};

/* GET /api/sd/files — list all SD files by category */
static esp_err_t sd_files_handler(httpd_req_t *req) {
	if (!sd_manager_is_mounted()) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SD card not mounted");
		return ESP_FAIL;
	}

	cJSON *root = cJSON_CreateObject();

	/* Layouts (*.json) — shared with serial_commands.c's sd.files handler */
	cJSON *layouts = cJSON_AddArrayToObject(root, "layouts");
	sd_list_dir(layouts, SD_LAYOUT_DIR, ".json", 5);

	/* Images (*.rdmimg) — NOT shared: this listing validates the RDMIMG
	 * magic header and reports width/height, which the serial side's
	 * listing doesn't need. */
	cJSON *images = cJSON_AddArrayToObject(root, "images");
	DIR *d = opendir(SD_IMAGE_DIR);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t flen = strlen(de->d_name);
			if (flen <= 7 || strcmp(de->d_name + flen - 7, ".rdmimg") != 0)
				continue;
			char path[96];
			snprintf(path, sizeof(path), "%s/%s", SD_IMAGE_DIR, de->d_name);

			FILE *f = fopen(path, "rb");
			if (!f) continue;
			uint8_t hdr[12];
			size_t nr = fread(hdr, 1, 12, f);
			fseek(f, 0, SEEK_END);
			long file_size = ftell(f);
			fclose(f);
			if (nr < 12 || memcmp(hdr, "RDMI", 4) != 0) continue;

			uint16_t w = hdr[4] | (hdr[5] << 8);
			uint16_t h = hdr[6] | (hdr[7] << 8);

			char name[32];
			size_t copy = flen - 7;
			if (copy >= sizeof(name)) copy = sizeof(name) - 1;
			memcpy(name, de->d_name, copy);
			name[copy] = '\0';

			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "name", name);
			cJSON_AddNumberToObject(obj, "width", w);
			cJSON_AddNumberToObject(obj, "height", h);
			cJSON_AddNumberToObject(obj, "size", file_size);
			cJSON_AddItemToArray(images, obj);
		}
		closedir(d);
	}

	/* Fonts (*.ttf) — shared with serial_commands.c's sd.files handler */
	cJSON *fonts = cJSON_AddArrayToObject(root, "fonts");
	sd_list_dir(fonts, SD_FONT_DIR, ".ttf", 4);

	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

static const httpd_uri_t sd_files_uri = {
    .uri = "/api/sd/files", .method = HTTP_GET,
    .handler = sd_files_handler, .user_ctx = NULL};

/* POST /api/sd/copy — copy file between internal <-> SD */
static esp_err_t sd_copy_handler(httpd_req_t *req) {
	char buf[192];
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	cJSON *dir_item  = cJSON_GetObjectItemCaseSensitive(root, "direction");

	if (!cJSON_IsString(type_item) || !cJSON_IsString(name_item) ||
		!cJSON_IsString(dir_item)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Missing type/name/direction");
		return ESP_FAIL;
	}

	const char *type = type_item->valuestring;
	const char *name = name_item->valuestring;
	const char *direction = dir_item->valuestring;

	if (!sd_manager_is_mounted()) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SD card not mounted");
		return ESP_FAIL;
	}

	/* Validate name (no path traversal) */
	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == '.') {
			cJSON_Delete(root);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
			return ESP_FAIL;
		}
	}

	/* Build source and destination paths */
	char src[96], dst[96];
	const char *lfs_dir = NULL, *sd_dir = NULL, *ext = NULL;

	if (strcmp(type, "layout") == 0) {
		lfs_dir = "/lfs/layouts"; sd_dir = SD_LAYOUT_DIR; ext = ".json";
	} else if (strcmp(type, "image") == 0) {
		lfs_dir = LFS_IMAGE_DIR; sd_dir = SD_IMAGE_DIR; ext = ".rdmimg";
	} else if (strcmp(type, "font") == 0) {
		lfs_dir = LFS_FONT_DIR; sd_dir = SD_FONT_DIR; ext = ".ttf";
	} else {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid type");
		return ESP_FAIL;
	}

	/* Built-in assets are locked — never copy them to or from SD (a copy back
	 * from SD could overwrite the intact original with a corrupt one). */
	if ((strcmp(type, "font")   == 0 && boot_assets_is_protected_font(name)) ||
	    (strcmp(type, "layout") == 0 && boot_assets_is_protected_layout(name)) ||
	    (strcmp(type, "image")  == 0 && boot_assets_is_protected_image(name))) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
		                    "Built-in asset cannot be moved to/from SD");
		return ESP_FAIL;
	}

	bool to_sd = (strcmp(direction, "to_sd") == 0);
	cJSON_Delete(root);

	if (to_sd) {
		snprintf(src, sizeof(src), "%s/%s%s", lfs_dir, name, ext);
		snprintf(dst, sizeof(dst), "%s/%s%s", sd_dir, name, ext);
	} else if (strcmp(direction, "from_sd") == 0) {
		snprintf(src, sizeof(src), "%s/%s%s", sd_dir, name, ext);
		snprintf(dst, sizeof(dst), "%s/%s%s", lfs_dir, name, ext);

		/* Check internal free space */
		struct stat st;
		if (stat(src, &st) != 0) {
			httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Source not found");
			return ESP_FAIL;
		}
		size_t lfs_total = 0, lfs_used = 0;
		if (esp_littlefs_info("littlefs", &lfs_total, &lfs_used) == ESP_OK) {
			size_t lfs_free = (lfs_total > lfs_used) ? (lfs_total - lfs_used) : 0;
			if ((size_t)st.st_size > lfs_free) {
				char err_msg[128];
				snprintf(err_msg, sizeof(err_msg),
						 "Not enough internal storage: need %u KB, %u KB free",
						 (unsigned)(st.st_size / 1024), (unsigned)(lfs_free / 1024));
				httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
				return ESP_FAIL;
			}
		}
	} else {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"direction must be 'to_sd' or 'from_sd'");
		return ESP_FAIL;
	}

	esp_err_t err = sd_copy_file(src, dst);
	if (err == ESP_ERR_NOT_FOUND) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Source file not found");
		return ESP_FAIL;
	}
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Copy failed");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Copied %s '%s' %s", type, name, to_sd ? "to SD" : "from SD");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t sd_copy_uri = {
    .uri = "/api/sd/copy", .method = HTTP_POST,
    .handler = sd_copy_handler, .user_ctx = NULL};

/* POST /api/sd/delete — delete file from SD card */
static esp_err_t sd_delete_handler(httpd_req_t *req) {
	char buf[128];
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");

	if (!cJSON_IsString(type_item) || !cJSON_IsString(name_item)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing type/name");
		return ESP_FAIL;
	}

	const char *type = type_item->valuestring;
	const char *name = name_item->valuestring;

	if (!sd_manager_is_mounted()) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SD card not mounted");
		return ESP_FAIL;
	}

	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == '.') {
			cJSON_Delete(root);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
			return ESP_FAIL;
		}
	}

	char path[96];
	if (strcmp(type, "layout") == 0)
		snprintf(path, sizeof(path), "%s/%s.json", SD_LAYOUT_DIR, name);
	else if (strcmp(type, "image") == 0)
		snprintf(path, sizeof(path), "%s/%s.rdmimg", SD_IMAGE_DIR, name);
	else if (strcmp(type, "font") == 0)
		snprintf(path, sizeof(path), "%s/%s.ttf", SD_FONT_DIR, name);
	else {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid type");
		return ESP_FAIL;
	}
	cJSON_Delete(root);

	if (remove(path) != 0) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found on SD");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Deleted %s '%s' from SD", type, name);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t sd_delete_uri = {
    .uri = "/api/sd/delete", .method = HTTP_POST,
    .handler = sd_delete_handler, .user_ctx = NULL};

/* -- Track-map endpoints -------------------------------------------------- */

/* An .rdmtrk is a circuit outline: a few hundred geographic points that the
 * track_map widget projects onto the screen. It is a device ASSET rather than
 * layout config so that pushing a new circuit is ONE upload instead of
 * re-pushing a whole dash design.
 *
 * The size bound comes from the widget's own header, not from a number typed
 * twice. This file already carries the scar for that mistake in the font path:
 * "The upload cap MUST match the loader's limit, or a font in (limit, old-4MB]
 * uploads OK but silently fails to load." */
#define LFS_TRACK_DIR   TRACK_MAP_LFS_DIR
#define TRACK_MAX_SIZE  TRACK_MAP_MAX_FILE

static void _ensure_track_dir(void) {
	struct stat st;
	if (stat(LFS_TRACK_DIR, &st) != 0)
		mkdir(LFS_TRACK_DIR, 0755);
}

/* Reject a track upload BEFORE its body has been read.
 *
 * web_server.c's own comment spells out the hazard: httpd closes the socket
 * when a handler responds with RX data still unconsumed, so the client sees a
 * TCP reset instead of the 4xx that explains itself. Proven against the real
 * dash -- posting an 804-byte track under the name "../escape" produced
 * ConnectionResetError, not "Invalid name". Every early return in the upload
 * path has to drain first, not just the too-large one. */
static esp_err_t _track_reject(httpd_req_t *req, httpd_err_code_t code, const char *why) {
	web_server_drain_body(req, 1024 * 1024);
	httpd_resp_send_err(req, code, why);
	return ESP_FAIL;
}

/* POST /api/track/upload?name=<name>   Body: raw RDMTRK binary */
static esp_err_t track_upload_handler(httpd_req_t *req) {
	_ensure_track_dir();

	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
		return _track_reject(req, HTTPD_400_BAD_REQUEST, "Missing query string");

	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0')
		return _track_reject(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");

	web_server_url_decode(name);
	if (!web_server_name_is_safe(name))
		return _track_reject(req, HTTPD_400_BAD_REQUEST, "Invalid name");

	size_t content_len = req->content_len;
	if (content_len < TRACK_MAP_HEADER || content_len > TRACK_MAX_SIZE)
		return _track_reject(req, HTTPD_400_BAD_REQUEST, "Invalid content length");

	size_t total_bytes = 0, used_bytes = 0;
	if (esp_littlefs_info("littlefs", &total_bytes, &used_bytes) == ESP_OK) {
		size_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;
		if (content_len > free_bytes)
			return _track_reject(req, HTTPD_400_BAD_REQUEST, "Not enough storage");
	}

	char path[96], tmp_path[112], bak_path[112];
	snprintf(path, sizeof(path), "%s/%s.rdmtrk", LFS_TRACK_DIR, name);
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	snprintf(bak_path, sizeof(bak_path), "%s.bak", path);

	/* A whole track is at most ~3 KB, so unlike an image it is read in one go.
	 * It is still staged in a .tmp and renamed, because a half-written outline
	 * that still passes the magic check would draw a circuit with its back half
	 * missing -- which looks like a track, not like a failure. */
	uint8_t *buf = malloc(content_len);
	if (!buf)
		return _track_reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");

	size_t received = 0;
	while (received < content_len) {
		int ret = httpd_req_recv(req, (char *)buf + received, content_len - received);
		if (ret <= 0) {
			free(buf);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
			return ESP_FAIL;
		}
		received += (size_t)ret;
	}

	/* Validate with the SAME parser the widget will use, before anything is
	 * published. Rejecting here turns "the widget silently shows nothing" into
	 * a 400 with a reason, at the only moment someone is watching. */
	track_map_file_t parsed;
	track_map_err_t perr = track_map_parse(buf, received, &parsed);
	if (perr != TRACK_MAP_OK) {
		free(buf);
		const char *why =
			perr == TRACK_MAP_ERR_MAGIC   ? "Not an RDMTRK file" :
			perr == TRACK_MAP_ERR_VERSION ? "RDMTRK version not supported by this firmware" :
			perr == TRACK_MAP_ERR_POINTS  ? "Track has too few or too many points" :
			perr == TRACK_MAP_ERR_RANGE   ? "Track contains an impossible coordinate" :
			                                "Track file is truncated";
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
		return ESP_FAIL;
	}

	FILE *f = fopen(tmp_path, "wb");
	if (!f) {
		free(buf);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}
	bool ok = (fwrite(buf, 1, received, f) == received);
	free(buf);
	if (ok) ok = (fflush(f) == 0 && fsync(fileno(f)) == 0);
	if (fclose(f) != 0) ok = false;
	if (!ok) {
		remove(tmp_path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write incomplete");
		return ESP_FAIL;
	}

	rename(path, bak_path);
	if (rename(tmp_path, path) != 0) {
		rename(bak_path, path);
		remove(tmp_path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}
	remove(bak_path);

	ESP_LOGI(TAG, "Uploaded track '%s' (%s, %u points, %u bytes)",
	         name, parsed.name, (unsigned)parsed.n_points, (unsigned)received);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	char resp[192];
	/* Tell the device bus what this dash now holds. Without this a track
	 * pushed over WiFi is invisible on CAN — the dash keeps announcing rev 0,
	 * the puck never sees anything newer, and "push to whichever device you can
	 * reach" quietly only works in one direction.
	 *
	 * `rev` is Studio's stamp and decides who wins. When it is absent (a plain
	 * curl, or an older Studio) the revision still has to move forward or the
	 * peer would ignore the new track as not-newer, so fall back to bumping
	 * what we had. */
	{
		char revs[16] = {0};
		uint32_t rev = 0;
		if (httpd_query_key_value(query, "rev", revs, sizeof(revs)) == ESP_OK)
			rev = (uint32_t)strtoul(revs, NULL, 10);
		if (rev == 0) rev = rdm_bus_local_rev() + 1;
		rdm_bus_set_local_track(name, rev);
	}

	snprintf(resp, sizeof(resp),
	         "{\"status\":\"ok\",\"name\":\"%s\",\"points\":%u,\"bytes\":%u}",
	         name, (unsigned)parsed.n_points, (unsigned)received);
	return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t track_upload_uri = {.uri = "/api/track/upload",
                                              .method = HTTP_POST,
                                              .handler = track_upload_handler,
                                              .user_ctx = NULL};

/* GET /api/track/list -- [{name, track, points, version, size}]
 * `name` is the filename (what a widget references); `track` is the circuit's
 * own name from inside the file, which is what a human recognises. */
static esp_err_t track_list_handler(httpd_req_t *req) {
	_ensure_track_dir();

	cJSON *arr = cJSON_CreateArray();
	DIR *d = opendir(LFS_TRACK_DIR);
	if (d) {
		struct dirent *ent;
		while ((ent = readdir(d)) != NULL) {
			const char *dot = strrchr(ent->d_name, '.');
			if (!dot || strcmp(dot, ".rdmtrk") != 0) continue;

			char base[64] = {0};
			size_t blen = (size_t)(dot - ent->d_name);
			if (blen >= sizeof(base)) blen = sizeof(base) - 1;
			memcpy(base, ent->d_name, blen);

			char path[128];
			snprintf(path, sizeof(path), "%s/%s", LFS_TRACK_DIR, ent->d_name);

			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "name", base);

			/* Only the header is read: the list is drawn far more often than a
			 * track is loaded, and the points are not needed to name one. */
			FILE *f = fopen(path, "rb");
			if (f) {
				uint8_t hdr[TRACK_MAP_HEADER];
				size_t got = fread(hdr, 1, sizeof(hdr), f);
				fclose(f);
				struct stat st;
				long sz = (stat(path, &st) == 0) ? (long)st.st_size : 0;
				if (got == sizeof(hdr) &&
				    memcmp(hdr, TRACK_MAP_MAGIC, TRACK_MAP_MAGIC_LEN) == 0) {
					char tname[TRACK_MAP_NAME_LEN];
					memcpy(tname, hdr + 8, TRACK_MAP_NAME_LEN - 1);
					tname[TRACK_MAP_NAME_LEN - 1] = '\0';
					cJSON_AddStringToObject(o, "track", tname);
					cJSON_AddNumberToObject(o, "points",
					    (double)((uint16_t)hdr[48] | ((uint16_t)hdr[49] << 8)));
					cJSON_AddNumberToObject(o, "version", hdr[6]);
				} else {
					cJSON_AddStringToObject(o, "track", "");
					cJSON_AddBoolToObject(o, "unreadable", true);
				}
				cJSON_AddNumberToObject(o, "size", (double)sz);
			}
			cJSON_AddItemToArray(arr, o);
		}
		closedir(d);
	}

	char *json = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t r = httpd_resp_send(req, json ? json : "[]", HTTPD_RESP_USE_STRLEN);
	if (json) free(json);
	return r;
}

static const httpd_uri_t track_list_uri = {.uri = "/api/track/list",
                                            .method = HTTP_GET,
                                            .handler = track_list_handler,
                                            .user_ctx = NULL};

/* GET /api/track/data?name=<name> -- the raw .rdmtrk */
static esp_err_t track_data_handler(httpd_req_t *req) {
	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}
	web_server_url_decode(name);
	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}
	char path[96];
	snprintf(path, sizeof(path), "%s/%s.rdmtrk", LFS_TRACK_DIR, name);
	return _send_file_chunked(req, path, TRACK_MAX_SIZE, "Track not found");
}

static const httpd_uri_t track_data_uri = {.uri = "/api/track/data",
                                            .method = HTTP_GET,
                                            .handler = track_data_handler,
                                            .user_ctx = NULL};

/* POST /api/track/delete?name=<name> */
static esp_err_t track_delete_handler(httpd_req_t *req) {
	char query[64] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}
	char name[32] = {0};
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' parameter");
		return ESP_FAIL;
	}
	web_server_url_decode(name);
	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}
	char path[96];
	snprintf(path, sizeof(path), "%s/%s.rdmtrk", LFS_TRACK_DIR, name);
	if (remove(path) != 0) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Track not found");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "Deleted track '%s'", name);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t track_delete_uri = {.uri = "/api/track/delete",
                                              .method = HTTP_POST,
                                              .handler = track_delete_handler,
                                              .user_ctx = NULL};

/* ── URI registration ────────────────────────────────────────────────────── */

void web_server_assets_register(httpd_handle_t server) {
	REGISTER_URI(server, &image_upload_uri);
	REGISTER_URI(server, &image_list_uri);
	REGISTER_URI(server, &image_delete_uri);
	REGISTER_URI(server, &image_data_uri);
	REGISTER_URI(server, &font_upload_uri);
	REGISTER_URI(server, &font_list_uri);
	REGISTER_URI(server, &font_delete_uri);
	REGISTER_URI(server, &font_data_uri);
	REGISTER_URI(server, &storage_info_uri);
	REGISTER_URI(server, &sd_status_uri);
	REGISTER_URI(server, &sd_files_uri);
	REGISTER_URI(server, &sd_copy_uri);
	REGISTER_URI(server, &sd_delete_uri);
	REGISTER_URI(server, &track_upload_uri);
	REGISTER_URI(server, &track_list_uri);
	REGISTER_URI(server, &track_data_uri);
	REGISTER_URI(server, &track_delete_uri);
}
