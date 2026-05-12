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
#include "storage/can_raw_logger.h"
#include "storage/can_upload.h"
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
	/* data_logger uses an LVGL timer, so start from LVGL task. SD card is
	 * preferred but not required — the logger falls back to LittleFS (with
	 * a per-file size cap) so users can record short traces without an SD. */
	if (data_logger_is_active()) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Already logging");
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
	const char *storage = active ? data_logger_get_storage()
	                              : (sd_manager_is_mounted() ? "sd" : "lfs");
	uint32_t lfs_cap = data_logger_lfs_max_bytes();

	/* Extract just the filename from the full path */
	const char *basename = file;
	const char *p = strrchr(file, '/');
	if (p) basename = p + 1;

	char buf[320];
	snprintf(buf, sizeof(buf),
			 "{\"active\":%s,\"file\":\"%s\",\"samples\":%lu,\"elapsed_ms\":%lu,"
			 "\"rate_hz\":%u,\"storage\":\"%s\",\"lfs_max_bytes\":%lu,"
			 "\"sd_mounted\":%s}",
			 active ? "true" : "false",
			 basename,
			 (unsigned long)samples, (unsigned long)elapsed,
			 (unsigned)rate,
			 storage,
			 (unsigned long)lfs_cap,
			 sd_manager_is_mounted() ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

/* Scan one log directory, append every *.csv into the array tagged with the
 * given storage tier. Silently no-ops if dir doesn't exist (e.g. SD not
 * mounted, or no logs ever written on LFS). */
static void _log_list_scan(cJSON *arr, const char *dir, const char *tier) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		size_t flen = strlen(de->d_name);
		if (flen <= 4 || strcmp(de->d_name + flen - 4, ".csv") != 0)
			continue;
		char path[160];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0) continue;

		cJSON *obj = cJSON_CreateObject();
		cJSON_AddStringToObject(obj, "name", de->d_name);
		cJSON_AddNumberToObject(obj, "size", st.st_size);
		cJSON_AddStringToObject(obj, "storage", tier);
		cJSON_AddItemToArray(arr, obj);
	}
	closedir(d);
}

static esp_err_t _log_list_handler(httpd_req_t *req) {
	/* Walk both tiers — SD (if mounted) and LFS — so the UI sees every
	 * downloadable log regardless of card state. */
	cJSON *arr = cJSON_CreateArray();
	if (sd_manager_is_mounted())
		_log_list_scan(arr, "/sdcard/logs", "sd");
	_log_list_scan(arr, "/lfs/logs", "lfs");

	char *json_str = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str ? json_str : "[]",
									HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

/* Resolve a basename (e.g. "log_42.csv") to a full path by checking both
 * tiers. Returns true if found, with out[] populated. */
static bool _log_resolve_path(const char *name, char *out, size_t out_len) {
	struct stat st;
	if (sd_manager_is_mounted()) {
		snprintf(out, out_len, "/sdcard/logs/%s", name);
		if (stat(out, &st) == 0) return true;
	}
	snprintf(out, out_len, "/lfs/logs/%s", name);
	if (stat(out, &st) == 0) return true;
	out[0] = '\0';
	return false;
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

	char path[160];
	if (!_log_resolve_path(name, path, sizeof(path))) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Log not found");
		return ESP_OK;
	}
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

	char path[160];
	if (!_log_resolve_path(name, path, sizeof(path)) || remove(path) != 0) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Log not found");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"deleted\"}");
	return ESP_OK;
}

/* ── Upload handler ──────────────────────────────────────────────────────────
 * POST /api/log/upload?name=foo.csv  — stream the request body into
 * /lfs/logs/<safename>, capped at LOG_LFS_MAX_BYTES so users can't fill the
 * shared partition by uploading a giant CSV. The uploaded file is then
 * playable via /api/replay/start with the basename. */
static esp_err_t _log_upload_handler(httpd_req_t *req) {
	char query[160] = "";
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
	/* Auto-append .csv if missing so users can drop arbitrary names. */
	size_t nlen = strlen(name);
	if (nlen < 4 || strcmp(name + nlen - 4, ".csv") != 0) {
		if (nlen + 4 >= sizeof(name)) {
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name too long");
			return ESP_OK;
		}
		strcat(name, ".csv");
	}

	uint32_t cap = data_logger_lfs_max_bytes();
	if (req->content_len > 0 && (uint32_t)req->content_len > cap) {
		char msg[96];
		snprintf(msg, sizeof(msg),
		         "Upload too large (cap %lu bytes)", (unsigned long)cap);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
		return ESP_OK;
	}

	/* Ensure the LFS log dir exists, then stream the body into it. */
	struct stat st;
	if (stat("/lfs/logs", &st) != 0) mkdir("/lfs/logs", 0755);

	char path[160];
	snprintf(path, sizeof(path), "/lfs/logs/%s", name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
		                    "Failed to open file for write");
		return ESP_OK;
	}

	char *buf = malloc(2048);
	if (!buf) {
		fclose(f);
		remove(path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_OK;
	}

	size_t total = 0;
	int received;
	while ((received = httpd_req_recv(req, buf, 2048)) > 0) {
		total += (size_t)received;
		/* Defend against bogus Content-Length by enforcing the cap on the
		 * actual byte count as we go. Truncate + 413 if exceeded. */
		if (total > cap) {
			fclose(f);
			remove(path);
			free(buf);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
			                    "Upload exceeded size cap");
			return ESP_OK;
		}
		if (fwrite(buf, 1, (size_t)received, f) != (size_t)received) {
			fclose(f);
			remove(path);
			free(buf);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
			                    "Write failed (LFS full?)");
			return ESP_OK;
		}
	}
	free(buf);
	fclose(f);

	if (received < 0) {
		remove(path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
		                    "Receive failed");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	char body[160];
	snprintf(body, sizeof(body),
	         "{\"status\":\"ok\",\"name\":\"%s\",\"size\":%u,\"storage\":\"lfs\"}",
	         name, (unsigned)total);
	httpd_resp_sendstr(req, body);
	return ESP_OK;
}

/* ── Raw CAN capture handlers ─────────────────────────────────────────────
 * Same start/stop/status shape as the /api/log endpoints but operates on
 * the raw frame logger (every CAN frame, no decoding) instead of the
 * decoded-signal one. File listing/download/delete reuse /api/log/list etc.
 * since both modules write into the same /sdcard/logs or /lfs/logs dir. */

static void _deferred_canraw_start(void *arg) {
	(void)arg;
	can_raw_logger_start();
}
static void _deferred_canraw_stop(void *arg) {
	(void)arg;
	can_raw_logger_stop();
}

static esp_err_t _canraw_start_handler(httpd_req_t *req) {
	if (can_raw_logger_is_active()) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Already capturing");
		return ESP_OK;
	}
	if (data_logger_is_active()) {
		/* Both loggers writing simultaneously is allowed at the API level but
		 * doubles flash wear and CPU time. Reject so users get a clear
		 * conflict signal — they can stop the other first. */
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "Signal logger active — stop it first");
		return ESP_OK;
	}
	lv_async_call(_deferred_canraw_start, NULL);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"started\"}");
	return ESP_OK;
}

static esp_err_t _canraw_stop_handler(httpd_req_t *req) {
	if (!can_raw_logger_is_active()) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not capturing");
		return ESP_OK;
	}
	lv_async_call(_deferred_canraw_stop, NULL);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
	return ESP_OK;
}

static esp_err_t _canraw_status_handler(httpd_req_t *req) {
	bool active = can_raw_logger_is_active();
	const char *file = can_raw_logger_current_file();
	uint32_t frames = can_raw_logger_frame_count();
	uint32_t elapsed = can_raw_logger_elapsed_ms();
	const char *storage = active ? can_raw_logger_get_storage()
	                              : (sd_manager_is_mounted() ? "sd" : "lfs");

	const char *basename = file;
	const char *p = strrchr(file, '/');
	if (p) basename = p + 1;

	char buf[256];
	snprintf(buf, sizeof(buf),
	         "{\"active\":%s,\"file\":\"%s\",\"frames\":%lu,\"elapsed_ms\":%lu,"
	         "\"storage\":\"%s\",\"lfs_max_bytes\":%lu,\"sd_mounted\":%s}",
	         active ? "true" : "false",
	         basename,
	         (unsigned long)frames, (unsigned long)elapsed,
	         storage,
	         (unsigned long)data_logger_lfs_max_bytes(),
	         sd_manager_is_mounted() ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

/* ── Raw CAN cloud upload ────────────────────────────────────────────────── */

/* POST /api/canraw/cloud_upload — body is JSON with:
 *   { "file": "canraw_123.csv", "make": "Toyota", "model": "Supra MK4",
 *     "notes": "optional" }
 * Spawns can_upload_start() and returns immediately; client polls
 * /api/canraw/cloud_upload/status. */
static esp_err_t _canraw_cloud_upload_handler(httpd_req_t *req)
{
	if (req->content_len <= 0 || req->content_len > 1024) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
		return ESP_OK;
	}

	char body[1024 + 1] = {0};
	int recvd = httpd_req_recv(req, body, sizeof(body) - 1);
	if (recvd <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body read failed");
		return ESP_OK;
	}

	cJSON *root = cJSON_Parse(body);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body is not JSON");
		return ESP_OK;
	}

	const cJSON *jf = cJSON_GetObjectItem(root, "file");
	const cJSON *jma = cJSON_GetObjectItem(root, "make");
	const cJSON *jmo = cJSON_GetObjectItem(root, "model");
	const cJSON *jn  = cJSON_GetObjectItem(root, "notes");
	if (!cJSON_IsString(jf) || !cJSON_IsString(jma) || !cJSON_IsString(jmo)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "Required: file, make, model");
		return ESP_OK;
	}

	const char *notes = (cJSON_IsString(jn) && jn->valuestring) ? jn->valuestring : "";
	esp_err_t err = can_upload_start(jf->valuestring, jma->valuestring,
	                                 jmo->valuestring, notes);
	cJSON_Delete(root);

	if (err == ESP_ERR_INVALID_STATE) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "Another upload is already running");
		return ESP_OK;
	}
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
		                    "Failed to start upload");
		return ESP_OK;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"started\"}");
	return ESP_OK;
}

static const char *_can_upload_state_str(can_upload_state_t s)
{
	switch (s) {
	case CAN_UPLOAD_IDLE:    return "idle";
	case CAN_UPLOAD_RUNNING: return "running";
	case CAN_UPLOAD_SUCCESS: return "success";
	case CAN_UPLOAD_FAILED:  return "failed";
	default:                 return "unknown";
	}
}

static esp_err_t _canraw_cloud_upload_status_handler(httpd_req_t *req)
{
	can_upload_status_t st;
	can_upload_get_status(&st);

	cJSON *j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "state", _can_upload_state_str(st.state));
	cJSON_AddNumberToObject(j, "http_status", st.http_status);
	cJSON_AddNumberToObject(j, "uploaded_bytes", st.uploaded_bytes);
	cJSON_AddStringToObject(j, "message", st.message);

	char *out = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (!out) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_OK;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, out);
	free(out);
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
	/* Allow either a basename ("log_42.csv") or a full path. Basenames are
	 * resolved against both storage tiers — SD first, LFS fallback — so the
	 * caller doesn't need to know where the log lives. Full paths are used
	 * verbatim. */
	const char *fn = file_item->valuestring;
	if (fn[0] == '/') {
		strncpy(a->path, fn, sizeof(a->path) - 1);
		a->path[sizeof(a->path) - 1] = '\0';
	} else if (!_log_resolve_path(fn, a->path, sizeof(a->path))) {
		/* Not found in either tier — surface a clear 404 instead of letting
		 * signal_replay_start fail silently. */
		free(a);
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Log not found");
		return ESP_OK;
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
static const httpd_uri_t log_upload_uri = {
	.uri = "/api/log/upload", .method = HTTP_POST,
	.handler = _log_upload_handler, .user_ctx = NULL
};
static const httpd_uri_t canraw_start_uri = {
	.uri = "/api/canraw/start", .method = HTTP_POST,
	.handler = _canraw_start_handler, .user_ctx = NULL
};
static const httpd_uri_t canraw_stop_uri = {
	.uri = "/api/canraw/stop", .method = HTTP_POST,
	.handler = _canraw_stop_handler, .user_ctx = NULL
};
static const httpd_uri_t canraw_status_uri = {
	.uri = "/api/canraw/status", .method = HTTP_GET,
	.handler = _canraw_status_handler, .user_ctx = NULL
};
static const httpd_uri_t canraw_cloud_upload_uri = {
	.uri = "/api/canraw/cloud_upload", .method = HTTP_POST,
	.handler = _canraw_cloud_upload_handler, .user_ctx = NULL
};
static const httpd_uri_t canraw_cloud_upload_status_uri = {
	.uri = "/api/canraw/cloud_upload/status", .method = HTTP_GET,
	.handler = _canraw_cloud_upload_status_handler, .user_ctx = NULL
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
	REGISTER_URI(server, &log_upload_uri);
	REGISTER_URI(server, &canraw_start_uri);
	REGISTER_URI(server, &canraw_stop_uri);
	REGISTER_URI(server, &canraw_status_uri);
	REGISTER_URI(server, &canraw_cloud_upload_uri);
	REGISTER_URI(server, &canraw_cloud_upload_status_uri);
	REGISTER_URI(server, &log_config_get_uri);
	REGISTER_URI(server, &log_config_post_uri);
	REGISTER_URI(server, &replay_start_uri);
	REGISTER_URI(server, &replay_stop_uri);
	REGISTER_URI(server, &replay_status_uri);
}
