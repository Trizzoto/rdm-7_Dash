/* web_server_gear.c — /api/gear/config GET + POST
 *
 * Configures the CALCULATED_GEAR synthetic signal: wheel circumference,
 * final drive ratio, per-gear ratios, RPM/speed signal names, and enabled flag.
 * Config is persisted to NVS via signal_internal_set_gear_cal(). */
#include "web_server_internal.h"
#include "cJSON.h"
#include "storage/config_store.h"
#include "widgets/signal_internal.h"
#include <string.h>
#include <stdlib.h>

static esp_err_t api_gear_cfg_get_handler(httpd_req_t *req) {
	gear_cal_config_t cfg = {0};
	signal_internal_get_gear_cal(&cfg);
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "wheel_circumference_m", cfg.wheel_circumference_m);
	cJSON_AddNumberToObject(root, "final_drive", cfg.final_drive);
	cJSON_AddStringToObject(root, "rpm_signal", cfg.rpm_signal);
	cJSON_AddStringToObject(root, "speed_signal", cfg.speed_signal);
	cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
	cJSON *arr = cJSON_AddArrayToObject(root, "ratios");
	for (uint8_t i = 0; i < cfg.ratio_count && i < GEAR_CAL_MAX_GEARS; i++) {
		cJSON_AddItemToArray(arr, cJSON_CreateNumber(cfg.ratios[i]));
	}
	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t r = httpd_resp_send(req, body ? body : "{}", HTTPD_RESP_USE_STRLEN);
	if (body) free(body);
	return r;
}

static esp_err_t api_gear_cfg_post_handler(httpd_req_t *req) {
	char body[512];
	int total = req->content_len;
	if (total <= 0 || total >= (int)sizeof(body)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
		return ESP_FAIL;
	}
	int got = 0;
	while (got < total) {
		int r = httpd_req_recv(req, body + got, total - got);
		if (r <= 0) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed"); return ESP_FAIL; }
		got += r;
	}
	body[got] = '\0';
	cJSON *root = cJSON_Parse(body);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
		return ESP_FAIL;
	}
	gear_cal_config_t cfg = {0};
	signal_internal_get_gear_cal(&cfg);  /* start from current */

	cJSON *js;
	if ((js = cJSON_GetObjectItemCaseSensitive(root, "wheel_circumference_m")) && cJSON_IsNumber(js))
		cfg.wheel_circumference_m = (float)js->valuedouble;
	if ((js = cJSON_GetObjectItemCaseSensitive(root, "final_drive")) && cJSON_IsNumber(js))
		cfg.final_drive = (float)js->valuedouble;
	if ((js = cJSON_GetObjectItemCaseSensitive(root, "rpm_signal")) && cJSON_IsString(js)) {
		strncpy(cfg.rpm_signal, js->valuestring, sizeof(cfg.rpm_signal) - 1);
		cfg.rpm_signal[sizeof(cfg.rpm_signal) - 1] = '\0';
	}
	if ((js = cJSON_GetObjectItemCaseSensitive(root, "speed_signal")) && cJSON_IsString(js)) {
		strncpy(cfg.speed_signal, js->valuestring, sizeof(cfg.speed_signal) - 1);
		cfg.speed_signal[sizeof(cfg.speed_signal) - 1] = '\0';
	}
	if ((js = cJSON_GetObjectItemCaseSensitive(root, "enabled")) && cJSON_IsBool(js))
		cfg.enabled = cJSON_IsTrue(js);
	if ((js = cJSON_GetObjectItemCaseSensitive(root, "ratios")) && cJSON_IsArray(js)) {
		int n = cJSON_GetArraySize(js);
		if (n > GEAR_CAL_MAX_GEARS) n = GEAR_CAL_MAX_GEARS;
		cfg.ratio_count = (uint8_t)n;
		for (int i = 0; i < n; i++) {
			cJSON *it = cJSON_GetArrayItem(js, i);
			cfg.ratios[i] = cJSON_IsNumber(it) ? (float)it->valuedouble : 0.0f;
		}
	}
	cJSON_Delete(root);

	signal_internal_set_gear_cal(&cfg);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t api_gear_cfg_get_uri = {
    .uri = "/api/gear/config", .method = HTTP_GET,
    .handler = api_gear_cfg_get_handler, .user_ctx = NULL};
static const httpd_uri_t api_gear_cfg_post_uri = {
    .uri = "/api/gear/config", .method = HTTP_POST,
    .handler = api_gear_cfg_post_handler, .user_ctx = NULL};

void web_server_gear_register(httpd_handle_t server) {
	REGISTER_URI(server, &api_gear_cfg_get_uri);
	REGISTER_URI(server, &api_gear_cfg_post_uri);
}
