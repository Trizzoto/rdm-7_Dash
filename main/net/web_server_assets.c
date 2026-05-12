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

static const char *TAG = "web_server_assets";

/* ── Image endpoints ─────────────────────────────────────────────────────── */

#define LFS_IMAGE_DIR "/lfs/images"
/* Max RDMIMG size: SCREEN_W * SCREEN_H * 3 bytes/pixel + 12-byte header, rounded up */
#define IMAGE_MAX_SIZE (1200 * 1024)

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

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	size_t content_len = req->content_len;
	if (content_len < 12 || content_len > IMAGE_MAX_SIZE) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
		return ESP_FAIL;
	}

	/* Allocate receive buffer in PSRAM */
	uint8_t *buf = heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
	if (!buf) {
		buf = malloc(content_len);
		if (!buf) {
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
			return ESP_FAIL;
		}
	}

	/* Receive data in chunks */
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

	/* Validate RDMI magic */
	if (memcmp(buf, "RDMI", 4) != 0) {
		free(buf);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid RDMIMG format");
		return ESP_FAIL;
	}

	/* Check free space before writing */
	size_t total_bytes = 0, used_bytes = 0;
	if (esp_littlefs_info("littlefs", &total_bytes, &used_bytes) == ESP_OK) {
		size_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;
		if (content_len > free_bytes) {
			free(buf);
			char err_msg[128];
			snprintf(err_msg, sizeof(err_msg),
					 "Not enough storage: need %u KB, only %u KB free",
					 (unsigned)(content_len / 1024), (unsigned)(free_bytes / 1024));
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
			return ESP_FAIL;
		}
	}

	/* Write to LittleFS */
	char path[80];
	snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(buf);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
		return ESP_FAIL;
	}
	size_t nw = fwrite(buf, 1, received, f);
	fclose(f);
	free(buf);

	if (nw != received) {
		remove(path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write incomplete");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Uploaded image '%s' (%u bytes)", name, (unsigned)received);
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

/* GET /api/image/data?name=<name> — return raw RDMIMG binary */
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

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	char path[80];
	snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found");
		return ESP_FAIL;
	}

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0 || file_size > IMAGE_MAX_SIZE) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid file size");
		return ESP_FAIL;
	}

	uint8_t *buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
	if (!buf) buf = malloc(file_size);
	if (!buf) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
		return ESP_FAIL;
	}

	size_t nread = fread(buf, 1, file_size, f);
	fclose(f);

	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, (const char *)buf, nread);
	free(buf);
	return res;
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

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
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

	/* Receive into PSRAM */
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

	/* Register in font manager */
	font_manager_add_family(name, buf, received);
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

/* GET /api/font/list — returns JSON array of font family names */
static esp_err_t font_list_handler(httpd_req_t *req) {
	cJSON *arr = cJSON_CreateArray();
	uint8_t count = font_manager_family_count();
	for (uint8_t i = 0; i < count; i++) {
		const char *fname = font_manager_family_name(i);
		if (fname)
			cJSON_AddItemToArray(arr, cJSON_CreateString(fname));
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

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	if (!font_manager_remove_family(name)) {
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

/* GET /api/font/data?name=<family_name> — return raw TTF binary */
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

	if (!web_server_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	char path[80];
	snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Font not found");
		return ESP_FAIL;
	}

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0 || file_size > FONT_MAX_FILE_SIZE) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid file size");
		return ESP_FAIL;
	}

	uint8_t *buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
	if (!buf) buf = malloc(file_size);
	if (!buf) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
		return ESP_FAIL;
	}

	size_t nread = fread(buf, 1, file_size, f);
	fclose(f);

	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, (const char *)buf, nread);
	free(buf);
	return res;
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
	int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (received <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
		return ESP_FAIL;
	}
	buf[received] = '\0';

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
	int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (received <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
		return ESP_FAIL;
	}
	buf[received] = '\0';

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
