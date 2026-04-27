/* web_server_logger.c — Data logger and signal replay endpoints
 *
 * Data logger endpoints:
 *   POST /api/log/start          start logging (optional {"rate_hz":N,"persist":bool})
 *   POST /api/log/stop           stop logging
 *   GET  /api/log/status         active flag, current file, samples, elapsed, rate
 *   GET  /api/log/list           list CSV files on SD card
 *   GET  /api/log/download?name= stream a CSV file as attachment
 *   POST /api/log/delete?name=   delete a CSV log file
 *   GET  /api/log/config         current rate_hz + is_max flag
 *   POST /api/log/config         update rate mid-log: {"rate_hz":N}
 *
 * Signal replay endpoints:
 *   POST /api/replay/start       {"file":"log_42.csv","speed":1.0,"loop":false}
 *   POST /api/replay/stop        {}
 *   GET  /api/replay/status      active, file, row, total_rows, speed */
#include "web_server_internal.h"
#include "cJSON.h"
#include "storage/data_logger.h"
#include "storage/signal_replay.h"
#include "storage/sd_manager.h"
#include "lvgl.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Data Logger helpers ─────────────────────────────────────────────────── */

/* Deferred-start carries the rate via a heap-allocated payload; the LVGL
 * task picks it up, calls data_logger_start_with_rate, and frees the payload. */
typedef struct {
	uint16_t rate_hz;  /* 0 = Max, otherwise sample rate in Hz */
	bool     persist;  /* true = save the rate to NVS after starting */
} log_start_args_t;

static void _deferred_log_start(void *arg) {
	log_start_args_t *args = (log_start_args_t *)arg;
	if (args) {
		data_logger_start_with_rate(args->rate_hz, args->persist);
		free(args);
	} else {
		data_logger_start();
	}
}

static void _deferred_log_stop(void *arg) {
	(void)arg;
	data_logger_stop();
}

/* lv_async_call shim for runtime rate changes from /api/log/config (POST).
 * Takes ownership of the heap-allocated uint16_t in `arg`. */
static void _deferred_log_set_rate(void *arg) {
	uint16_t *p = (uint16_t *)arg;
	if (p) {
		data_logger_set_rate_hz(*p);
		free(p);
	}
}

/* ── Data Logger handlers ─────────────────────────────────────────────────── */

static esp_err_t _log_start_handler(httpd_req_t *req) {
	/* data_logger uses an LVGL timer, so start from LVGL task */
	if (data_logger_is_active()) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Already logging");
		return ESP_OK;
	}
	if (!sd_manager_is_mounted()) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"SD card not mounted");
		return ESP_OK;
	}

	/* Optional JSON body: {"rate_hz": N, "persist": true|false}.
	 * rate_hz: 0 = Max, otherwise sample rate in Hz (1..1000).
	 * persist: default true; saves the rate to NVS so future sessions reuse it.
	 * Body is optional — without it, we use the currently-configured rate. */
	log_start_args_t *args = NULL;
	if (req->content_len > 0 && req->content_len < 128) {
		char buf[128];
		int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
		if (received > 0) {
			buf[received] = '\0';
			cJSON *root = cJSON_Parse(buf);
			if (root) {
				cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "rate_hz");
				cJSON *pers = cJSON_GetObjectItemCaseSensitive(root, "persist");
				if (cJSON_IsNumber(rate)) {
					args = (log_start_args_t *)calloc(1, sizeof(*args));
					if (args) {
						int v = rate->valueint;
						if (v < 0)    v = 0;
						if (v > 1000) v = 1000;
						args->rate_hz = (uint16_t)v;
						args->persist = cJSON_IsBool(pers) ? cJSON_IsTrue(pers) : true;
					}
				}
				cJSON_Delete(root);
			}
		}
	}

	lv_async_call(_deferred_log_start, args);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"started\"}");
	return ESP_OK;
}

/* GET /api/log/config — return current rate and metadata.
 * POST /api/log/config with {"rate_hz": N} — update rate (works mid-log). */
static esp_err_t _log_config_get_handler(httpd_req_t *req) {
	uint16_t rate = data_logger_get_rate_hz();
	char buf[96];
	snprintf(buf, sizeof(buf), "{\"rate_hz\":%u,\"is_max\":%s}",
	         (unsigned)rate, rate == 0 ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

static esp_err_t _log_config_post_handler(httpd_req_t *req) {
	if (req->content_len <= 0 || req->content_len >= 128) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required");
		return ESP_OK;
	}
	char buf[128];
	int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (received <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
		return ESP_OK;
	}
	buf[received] = '\0';
	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_OK;
	}
	cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "rate_hz");
	if (!cJSON_IsNumber(rate)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing rate_hz");
		return ESP_OK;
	}
	int v = rate->valueint;
	if (v < 0)    v = 0;
	if (v > 1000) v = 1000;
	cJSON_Delete(root);

	/* data_logger_set_rate_hz touches the LVGL timer — defer to LVGL task. */
	uint16_t *hz_arg = (uint16_t *)malloc(sizeof(uint16_t));
	if (!hz_arg) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_OK;
	}
	*hz_arg = (uint16_t)v;
	lv_async_call(_deferred_log_set_rate, hz_arg);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
	return ESP_OK;
}

static esp_err_t _log_stop_handler(httpd_req_t *req) {
	if (!data_logger_is_active()) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not logging");
		return ESP_OK;
	}
	lv_async_call(_deferred_log_stop, NULL);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
	return ESP_OK;
}

static esp_err_t _log_status_handler(httpd_req_t *req) {
	bool active = data_logger_is_active();
	const char *file = data_logger_current_file();
	uint32_t samples = data_logger_get_sample_count();
	uint32_t elapsed = data_logger_get_elapsed_ms();
	uint16_t rate = data_logger_get_rate_hz();

	/* Extract just the filename from the full path */
	const char *basename = file;
	const char *p = strrchr(file, '/');
	if (p) basename = p + 1;

	char buf[224];
	snprintf(buf, sizeof(buf),
			 "{\"active\":%s,\"file\":\"%s\",\"samples\":%lu,\"elapsed_ms\":%lu,"
			 "\"rate_hz\":%u}",
			 active ? "true" : "false",
			 basename,
			 (unsigned long)samples, (unsigned long)elapsed,
			 (unsigned)rate);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

static esp_err_t _log_list_handler(httpd_req_t *req) {
	if (!sd_manager_is_mounted()) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SD card not mounted");
		return ESP_FAIL;
	}

	cJSON *arr = cJSON_CreateArray();
	DIR *d = opendir("/sdcard/logs");
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t flen = strlen(de->d_name);
			if (flen <= 4 || strcmp(de->d_name + flen - 4, ".csv") != 0)
				continue;
			char path[96];
			snprintf(path, sizeof(path), "/sdcard/logs/%s", de->d_name);
			struct stat st;
			if (stat(path, &st) != 0) continue;

			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "name", de->d_name);
			cJSON_AddNumberToObject(obj, "size", st.st_size);
			cJSON_AddItemToArray(arr, obj);
		}
		closedir(d);
	}

	char *json_str = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str ? json_str : "[]",
									HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

static esp_err_t _log_download_handler(httpd_req_t *req) {
	char query[128] = "";
	char name[64] = "";

	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
		httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
		name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name param");
		return ESP_OK;
	}

	/* Prevent path traversal */
	if (!web_server_filename_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_OK;
	}

	char path[96];
	snprintf(path, sizeof(path), "/sdcard/logs/%s", name);
	FILE *f = fopen(path, "r");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Log not found");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "text/csv");

	/* Set Content-Disposition for browser download */
	char disposition[128];
	snprintf(disposition, sizeof(disposition),
			 "attachment; filename=\"%s\"", name);
	httpd_resp_set_hdr(req, "Content-Disposition", disposition);

	char *buf = malloc(4096);
	if (!buf) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_OK;
	}
	size_t n;
	while ((n = fread(buf, 1, 4096, f)) > 0)
		httpd_resp_send_chunk(req, buf, n);
	httpd_resp_send_chunk(req, NULL, 0);
	free(buf);
	fclose(f);
	return ESP_OK;
}

static esp_err_t _log_delete_handler(httpd_req_t *req) {
	char query[128] = "";
	char name[64] = "";

	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
		httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
		name[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name param");
		return ESP_OK;
	}

	if (!web_server_filename_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_OK;
	}

	char path[96];
	snprintf(path, sizeof(path), "/sdcard/logs/%s", name);

	if (remove(path) != 0) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Log not found");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"deleted\"}");
	return ESP_OK;
}

/* ── Signal Replay helpers ───────────────────────────────────────────────── */

typedef struct {
	char  path[96];
	float speed;
	bool  loop;
} replay_start_args_t;

static void _deferred_replay_start(void *arg)
{
	replay_start_args_t *a = (replay_start_args_t *)arg;
	if (a) {
		signal_replay_start(a->path, a->speed, a->loop);
		free(a);
	}
}

static void _deferred_replay_stop(void *arg)
{
	(void)arg;
	signal_replay_stop();
}

/* ── Signal Replay handlers ──────────────────────────────────────────────── */

static esp_err_t _replay_start_handler(httpd_req_t *req)
{
	if (!sd_manager_is_mounted()) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
		                     "SD card not mounted");
		return ESP_OK;
	}
	if (req->content_len <= 0 || req->content_len >= 256) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required");
		return ESP_OK;
	}
	char buf[256];
	int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (received <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
		return ESP_OK;
	}
	buf[received] = '\0';
	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_OK;
	}
	cJSON *file_item  = cJSON_GetObjectItemCaseSensitive(root, "file");
	cJSON *speed_item = cJSON_GetObjectItemCaseSensitive(root, "speed");
	cJSON *loop_item  = cJSON_GetObjectItemCaseSensitive(root, "loop");
	if (!cJSON_IsString(file_item) || !file_item->valuestring) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'file'");
		return ESP_OK;
	}
	replay_start_args_t *a = calloc(1, sizeof(*a));
	if (!a) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_OK;
	}
	/* Allow either a basename ("log_42.csv") or a full path. Normalize to a
	 * full /sdcard/logs/ path if it doesn't start with one. */
	const char *fn = file_item->valuestring;
	if (fn[0] == '/') {
		strncpy(a->path, fn, sizeof(a->path) - 1);
	} else {
		snprintf(a->path, sizeof(a->path), "/sdcard/logs/%s", fn);
	}
	a->speed = cJSON_IsNumber(speed_item) ? (float)speed_item->valuedouble : 1.0f;
	a->loop  = cJSON_IsBool(loop_item) && cJSON_IsTrue(loop_item);
	cJSON_Delete(root);

	lv_async_call(_deferred_replay_start, a);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"started\"}");
	return ESP_OK;
}

static esp_err_t _replay_stop_handler(httpd_req_t *req)
{
	lv_async_call(_deferred_replay_stop, NULL);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
	return ESP_OK;
}

static esp_err_t _replay_status_handler(httpd_req_t *req)
{
	bool active = signal_replay_is_active();
	const char *file = signal_replay_get_file();
	uint32_t row = signal_replay_get_row();
	uint32_t total = signal_replay_get_total_rows();
	float speed = signal_replay_get_speed();

	const char *basename = file;
	const char *p = strrchr(file, '/');
	if (p) basename = p + 1;

	char buf[224];
	snprintf(buf, sizeof(buf),
	         "{\"active\":%s,\"file\":\"%s\",\"row\":%lu,"
	         "\"total_rows\":%lu,\"speed\":%.2f}",
	         active ? "true" : "false", basename,
	         (unsigned long)row, (unsigned long)total, (double)speed);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

/* ── URI descriptors ────────────────────────────────────────────────────── */

static const httpd_uri_t log_start_uri = {
	.uri = "/api/log/start", .method = HTTP_POST,
	.handler = _log_start_handler, .user_ctx = NULL
};
static const httpd_uri_t log_stop_uri = {
	.uri = "/api/log/stop", .method = HTTP_POST,
	.handler = _log_stop_handler, .user_ctx = NULL
};
static const httpd_uri_t log_status_uri = {
	.uri = "/api/log/status", .method = HTTP_GET,
	.handler = _log_status_handler, .user_ctx = NULL
};
static const httpd_uri_t log_list_uri = {
	.uri = "/api/log/list", .method = HTTP_GET,
	.handler = _log_list_handler, .user_ctx = NULL
};
static const httpd_uri_t log_download_uri = {
	.uri = "/api/log/download", .method = HTTP_GET,
	.handler = _log_download_handler, .user_ctx = NULL
};
static const httpd_uri_t log_delete_uri = {
	.uri = "/api/log/delete", .method = HTTP_POST,
	.handler = _log_delete_handler, .user_ctx = NULL
};
static const httpd_uri_t log_config_get_uri = {
	.uri = "/api/log/config", .method = HTTP_GET,
	.handler = _log_config_get_handler, .user_ctx = NULL
};
static const httpd_uri_t log_config_post_uri = {
	.uri = "/api/log/config", .method = HTTP_POST,
	.handler = _log_config_post_handler, .user_ctx = NULL
};
static const httpd_uri_t replay_start_uri = {
	.uri = "/api/replay/start", .method = HTTP_POST,
	.handler = _replay_start_handler, .user_ctx = NULL
};
static const httpd_uri_t replay_stop_uri = {
	.uri = "/api/replay/stop", .method = HTTP_POST,
	.handler = _replay_stop_handler, .user_ctx = NULL
};
static const httpd_uri_t replay_status_uri = {
	.uri = "/api/replay/status", .method = HTTP_GET,
	.handler = _replay_status_handler, .user_ctx = NULL
};

void web_server_logger_register(httpd_handle_t server) {
	REGISTER_URI(server, &log_start_uri);
	REGISTER_URI(server, &log_stop_uri);
	REGISTER_URI(server, &log_status_uri);
	REGISTER_URI(server, &log_list_uri);
	REGISTER_URI(server, &log_download_uri);
	REGISTER_URI(server, &log_delete_uri);
	REGISTER_URI(server, &log_config_get_uri);
	REGISTER_URI(server, &log_config_post_uri);
	REGISTER_URI(server, &replay_start_uri);
	REGISTER_URI(server, &replay_stop_uri);
	REGISTER_URI(server, &replay_status_uri);
}
