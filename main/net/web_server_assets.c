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
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "storage/sd_manager.h"
#include "storage/boot_assets.h"
#include "widgets/font_manager.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>        /* fsync, fileno */

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

	char *json_str = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str ? json_str : "[]", HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
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
#define FONT_MAX_FILE_SIZE (4 * 1024 * 1024)

static void _ensure_font_dir(void) {
	struct stat st;
	if (stat(LFS_FONT_DIR, &st) != 0)
		mkdir(LFS_FONT_DIR, 0755);
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
	if (content_len < 12 || content_len > FONT_MAX_FILE_SIZE) {
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

	/* Write to LittleFS */
	char path[80];
	snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(buf);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}
	size_t nw = fwrite(buf, 1, received, f);
	fclose(f);

	if (nw != received) {
		free(buf);
		remove(path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write incomplete");
		return ESP_FAIL;
	}

	/* Register in font manager UNDER the LVGL lock. font_manager_add_family()
	 * may lv_tiny_ttf_destroy() a replaced family, and font_manager's own
	 * contract is that its public functions run on the LVGL task / under the
	 * mutex — destroying a glyph cache while the render task is mid-draw with
	 * that font is a data race. (Runs on the httpd task here.) */
	if (rdm_lvgl_lock(2000)) {
		font_manager_add_family(name, buf, received);
		rdm_lvgl_unlock();
	} else {
		ESP_LOGW(TAG, "font upload: LVGL lock timeout — registering unguarded");
		font_manager_add_family(name, buf, received);
	}
	free(buf);

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

	char *json_str = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str ? json_str : "[]", HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
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

	/* Remove under the LVGL lock — lv_tiny_ttf_destroy() on a font the render
	 * task may be drawing with is a data race (see font_upload_handler). */
	bool removed;
	if (rdm_lvgl_lock(2000)) {
		removed = font_manager_remove_family(name);
		rdm_lvgl_unlock();
	} else {
		ESP_LOGW(TAG, "font delete: LVGL lock timeout — removing unguarded");
		removed = font_manager_remove_family(name);
	}
	if (!removed) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Font not found");
		return ESP_FAIL;
	}

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

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str ? json_str : "{}", HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
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

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str ? json_str : "{}", HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
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

	/* Layouts (*.json) */
	cJSON *layouts = cJSON_AddArrayToObject(root, "layouts");
	DIR *d = opendir(SD_LAYOUT_DIR);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t flen = strlen(de->d_name);
			if (flen <= 5 || strcmp(de->d_name + flen - 5, ".json") != 0)
				continue;
			char path[96];
			snprintf(path, sizeof(path), "%s/%s", SD_LAYOUT_DIR, de->d_name);
			struct stat st;
			if (stat(path, &st) != 0) continue;

			char name[64];
			size_t copy = flen - 5;
			if (copy >= sizeof(name)) copy = sizeof(name) - 1;
			memcpy(name, de->d_name, copy);
			name[copy] = '\0';

			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "name", name);
			cJSON_AddNumberToObject(obj, "size", st.st_size);
			cJSON_AddItemToArray(layouts, obj);
		}
		closedir(d);
	}

	/* Images (*.rdmimg) */
	cJSON *images = cJSON_AddArrayToObject(root, "images");
	d = opendir(SD_IMAGE_DIR);
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

	/* Fonts (*.ttf) */
	cJSON *fonts = cJSON_AddArrayToObject(root, "fonts");
	d = opendir(SD_FONT_DIR);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t flen = strlen(de->d_name);
			if (flen <= 4 || strcmp(de->d_name + flen - 4, ".ttf") != 0)
				continue;
			char path[96];
			snprintf(path, sizeof(path), "%s/%s", SD_FONT_DIR, de->d_name);
			struct stat st;
			if (stat(path, &st) != 0) continue;

			char name[32];
			size_t copy = flen - 4;
			if (copy >= sizeof(name)) copy = sizeof(name) - 1;
			memcpy(name, de->d_name, copy);
			name[copy] = '\0';

			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "name", name);
			cJSON_AddNumberToObject(obj, "size", st.st_size);
			cJSON_AddItemToArray(fonts, obj);
		}
		closedir(d);
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str ? json_str : "{}", HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

static const httpd_uri_t sd_files_uri = {
    .uri = "/api/sd/files", .method = HTTP_GET,
    .handler = sd_files_handler, .user_ctx = NULL};

/* Chunked file copy helper — heap-allocated 4KB buffer */
static esp_err_t _copy_file(const char *src, const char *dst) {
	FILE *fin = fopen(src, "rb");
	if (!fin) return ESP_ERR_NOT_FOUND;

	fseek(fin, 0, SEEK_END);
	long file_size = ftell(fin);
	fseek(fin, 0, SEEK_SET);

	if (file_size <= 0) {
		fclose(fin);
		return ESP_FAIL;
	}

	FILE *fout = fopen(dst, "wb");
	if (!fout) {
		fclose(fin);
		return ESP_FAIL;
	}

	char *buf = malloc(4096);
	if (!buf) {
		fclose(fin);
		fclose(fout);
		return ESP_FAIL;
	}

	size_t total_written = 0;
	while (total_written < (size_t)file_size) {
		size_t to_read = 4096;
		if (to_read > (size_t)file_size - total_written)
			to_read = (size_t)file_size - total_written;
		size_t nr = fread(buf, 1, to_read, fin);
		if (nr == 0) break;
		size_t nw = fwrite(buf, 1, nr, fout);
		if (nw != nr) {
			free(buf);
			fclose(fin);
			fclose(fout);
			remove(dst);
			return ESP_FAIL;
		}
		total_written += nw;
	}

	free(buf);
	fclose(fin);
	fclose(fout);
	return (total_written == (size_t)file_size) ? ESP_OK : ESP_FAIL;
}

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

	esp_err_t err = _copy_file(src, dst);
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
}
