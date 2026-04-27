/* web_server_ota.c — OTA update endpoints
 *
 * Endpoints:
 *   GET  /api/ota/status  cached state: current + latest version, progress,
 *                         release notes, file size. Safe to poll.
 *   POST /api/ota/check   kick off check_for_update() on a background task.
 *                         Returns 202 immediately.
 *   POST /api/ota/start   begin download + flash via start_ota_update_task(). */
#include "web_server_internal.h"
#include "cJSON.h"
#include "net/ota_handler.h"
#include "version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *_ota_status_string(ota_status_t s) {
	switch (s) {
		case OTA_IDLE:                 return "idle";
		case OTA_CHECKING:             return "checking";
		case OTA_NO_UPDATE_AVAILABLE:  return "no_update";
		case OTA_UPDATE_AVAILABLE:     return "available";
		case OTA_UPDATE_IN_PROGRESS:   return "installing";
		case OTA_UPDATE_COMPLETED:     return "completed";
		case OTA_UPDATE_FAILED:        return "failed";
		default:                       return "unknown";
	}
}

static esp_err_t _ota_status_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	ota_status_t st = get_ota_status();
	const char *latest = get_latest_version();
	const char *notes  = get_release_notes();
	float size_mb      = get_update_file_size_mb();
	int progress       = get_ota_progress();

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "status",          _ota_status_string(st));
	cJSON_AddStringToObject(root, "current_version", FIRMWARE_VERSION);
	cJSON_AddStringToObject(root, "latest_version",  latest ? latest : "");
	cJSON_AddStringToObject(root, "release_notes",   notes  ? notes  : "");
	cJSON_AddNumberToObject(root, "file_size_mb",    size_mb);
	cJSON_AddNumberToObject(root, "progress",        progress);
	cJSON_AddBoolToObject  (root, "update_available", st == OTA_UPDATE_AVAILABLE);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_sendstr(req, json ? json : "{}");
	free(json);
	return ESP_OK;
}

/* Background-task wrapper: the HTTPD handler returns 202 immediately;
 * check_for_update runs here and mutates the shared OTA state. Stack
 * sized for HTTPS + cJSON parse of the GitHub release payload. */
static void _ota_check_task(void *arg) {
	(void)arg;
	check_for_update();
	vTaskDelete(NULL);
}

static esp_err_t _ota_check_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	/* Skip if a check is already in flight to avoid stacking tasks. */
	if (get_ota_status() == OTA_CHECKING) {
		httpd_resp_set_status(req, "202 Accepted");
		httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
		return ESP_OK;
	}

	BaseType_t ok = xTaskCreate(_ota_check_task, "ota_check", 6144,
	                            NULL, 5, NULL);
	if (ok != pdPASS) {
		httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_sendstr(req, "{\"error\":\"failed to start check task\"}");
		return ESP_OK;
	}
	httpd_resp_set_status(req, "202 Accepted");
	httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
	return ESP_OK;
}

static esp_err_t _ota_start_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	ota_status_t st = get_ota_status();
	if (st != OTA_UPDATE_AVAILABLE) {
		httpd_resp_set_status(req, "409 Conflict");
		httpd_resp_sendstr(req, "{\"error\":\"no update available \xe2\x80\x94 run /api/ota/check first\"}");
		return ESP_OK;
	}

	start_ota_update_task();
	httpd_resp_set_status(req, "202 Accepted");
	httpd_resp_sendstr(req, "{\"status\":\"installing\"}");
	return ESP_OK;
}

static const httpd_uri_t ota_status_uri = {
    .uri = "/api/ota/status", .method = HTTP_GET,
    .handler = _ota_status_handler, .user_ctx = NULL};
static const httpd_uri_t ota_check_uri = {
    .uri = "/api/ota/check", .method = HTTP_POST,
    .handler = _ota_check_handler, .user_ctx = NULL};
static const httpd_uri_t ota_start_uri = {
    .uri = "/api/ota/start", .method = HTTP_POST,
    .handler = _ota_start_handler, .user_ctx = NULL};

void web_server_ota_register(httpd_handle_t server) {
	REGISTER_URI(server, &ota_status_uri);
	REGISTER_URI(server, &ota_check_uri);
	REGISTER_URI(server, &ota_start_uri);
}
