/* web_server_wifi.c — WiFi configuration endpoints
 *
 * Endpoints:
 *   GET  /api/wifi/config   return current WiFi creds + boot flags
 *   POST /api/wifi/config   update WiFi creds + boot flags */
#include "web_server_internal.h"
#include "cJSON.h"
#include "storage/config_store.h"
#include <string.h>
#include <stdlib.h>

static esp_err_t _wifi_config_get_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	wifi_credentials_t creds = {0};
	config_store_load_wifi(&creds);

	wifi_boot_config_t boot = {0};
	config_store_load_wifi_boot(&boot);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "ssid", creds.ssid);
	cJSON_AddStringToObject(root, "password", creds.password);
	cJSON_AddBoolToObject(root, "auto_connect", creds.auto_connect);
	cJSON_AddBoolToObject(root, "wifi_on_boot", boot.wifi_on_boot);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

static esp_err_t _wifi_config_post_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	char buf[256];
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

	wifi_credentials_t creds = {0};
	config_store_load_wifi(&creds);

	cJSON *j;
	if ((j = cJSON_GetObjectItem(root, "ssid")) && cJSON_IsString(j)) {
		strncpy(creds.ssid, j->valuestring, sizeof(creds.ssid) - 1);
		creds.ssid[sizeof(creds.ssid) - 1] = '\0';
	}
	if ((j = cJSON_GetObjectItem(root, "password")) && cJSON_IsString(j)) {
		strncpy(creds.password, j->valuestring, sizeof(creds.password) - 1);
		creds.password[sizeof(creds.password) - 1] = '\0';
	}
	if ((j = cJSON_GetObjectItem(root, "auto_connect")))
		creds.auto_connect = cJSON_IsTrue(j);
	config_store_save_wifi(&creds);

	if ((j = cJSON_GetObjectItem(root, "wifi_on_boot"))) {
		wifi_boot_config_t boot = {0};
		config_store_load_wifi_boot(&boot);
		boot.wifi_on_boot = cJSON_IsTrue(j);
		config_store_save_wifi_boot(&boot);
	}

	cJSON_Delete(root);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
	return ESP_OK;
}

static const httpd_uri_t wifi_config_get_uri = {
    .uri = "/api/wifi/config", .method = HTTP_GET,
    .handler = _wifi_config_get_handler, .user_ctx = NULL};
static const httpd_uri_t wifi_config_post_uri = {
    .uri = "/api/wifi/config", .method = HTTP_POST,
    .handler = _wifi_config_post_handler, .user_ctx = NULL};

void web_server_wifi_register(httpd_handle_t server) {
	REGISTER_URI(server, &wifi_config_get_uri);
	REGISTER_URI(server, &wifi_config_post_uri);
}
