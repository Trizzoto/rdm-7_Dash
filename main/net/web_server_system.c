/* web_server_system.c — device info, brightness, CAN config, system health,
 *                        system reboot, dimmer config
 *
 * Endpoints:
 *   GET  /api/device/info      full device/hw/system/signal snapshot
 *   GET  /api/brightness       current display brightness
 *   POST /api/brightness       set display brightness
 *   GET  /api/can/config       CAN bitrate index
 *   POST /api/can/config       update CAN bitrate index
 *   GET  /api/system/health    uptime, heap, psram, WiFi RSSI
 *   POST /api/system/reboot    deferred esp_restart()
 *   GET  /api/dimmer/config    brightness-dimmer signal config
 *   POST /api/dimmer/config    update dimmer config + re-subscribe */
#include "web_server_internal.h"
#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/twai.h"
#include "net/wifi_manager.h"
#include "storage/config_store.h"
#include "storage/sd_manager.h"
#include "storage/data_logger.h"
#include "storage/signal_replay.h"
#include "system/device_id.h"
#include "system/screen_config.h"
#include "layout/layout_manager.h"
#include "widgets/signal.h"
#include "ui/settings/device_settings.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>

/* ── Device Info ─────────────────────────────────────────────────────────── */

static esp_err_t _device_info_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	cJSON *root = cJSON_CreateObject();

	char serial[MAX_SERIAL_LENGTH];
	get_device_serial(serial);
	cJSON_AddStringToObject(root, "serial", serial);

	/* Firmware version intentionally omitted from this endpoint — UI hides it. */
	cJSON_AddNumberToObject(root, "schema", LAYOUT_SCHEMA_VERSION);

	const screen_profile_t *scr = screen_get_profile();
	cJSON *display = cJSON_AddObjectToObject(root, "display");
	cJSON_AddNumberToObject(display, "width", scr->width);
	cJSON_AddNumberToObject(display, "height", scr->height);
	cJSON_AddStringToObject(display, "shape",
		scr->shape == SCREEN_SHAPE_ROUND ? "round" : "rect");

	cJSON *hw = cJSON_AddObjectToObject(root, "hardware");
	esp_chip_info_t chip_info;
	esp_chip_info(&chip_info);
	cJSON_AddStringToObject(hw, "chip", CONFIG_IDF_TARGET);
	cJSON_AddNumberToObject(hw, "cores", chip_info.cores);
	cJSON_AddNumberToObject(hw, "psram_mb",
		(double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));
	uint32_t flash_size = 0;
	esp_flash_get_size(NULL, &flash_size);
	cJSON_AddNumberToObject(hw, "flash_mb",
		(double)flash_size / (1024 * 1024));

	/* Live system stats — what the on-device Diagnostics screen shows. */
	cJSON *sys = cJSON_AddObjectToObject(root, "system");
	cJSON_AddNumberToObject(sys, "uptime_s",
		(double)(esp_timer_get_time() / 1000000ULL));
	cJSON_AddNumberToObject(sys, "heap_free",
		(double)esp_get_free_heap_size());
	cJSON_AddNumberToObject(sys, "heap_min_free",
		(double)esp_get_minimum_free_heap_size());
	cJSON_AddNumberToObject(sys, "psram_free",
		(double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
	cJSON_AddBoolToObject(sys, "logger_active",   data_logger_is_active());
	cJSON_AddBoolToObject(sys, "replay_active",   signal_replay_is_active());

	/* CAN bus */
	cJSON *can = cJSON_AddObjectToObject(root, "can");
	twai_status_info_t ts;
	if (twai_get_status_info(&ts) == ESP_OK) {
		const char *state_str = "unknown";
		switch (ts.state) {
			case TWAI_STATE_STOPPED:      state_str = "stopped";      break;
			case TWAI_STATE_RUNNING:      state_str = "running";      break;
			case TWAI_STATE_BUS_OFF:      state_str = "bus_off";      break;
			case TWAI_STATE_RECOVERING:   state_str = "recovering";   break;
		}
		cJSON_AddStringToObject(can, "state", state_str);
		cJSON_AddNumberToObject(can, "rx_pending",  ts.msgs_to_rx);
		cJSON_AddNumberToObject(can, "tx_errors",   ts.tx_error_counter);
		cJSON_AddNumberToObject(can, "rx_errors",   ts.rx_error_counter);
		cJSON_AddNumberToObject(can, "bus_errors",  ts.bus_error_count);
		cJSON_AddNumberToObject(can, "rx_missed",   ts.rx_missed_count);
	} else {
		cJSON_AddStringToObject(can, "state", "unavailable");
	}

	/* WiFi */
	cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
	wifi_mgr_state_t ws = wifi_manager_get_state();
	const char *ws_str = "off";
	switch (ws) {
		case WIFI_MGR_STATE_OFF:         ws_str = "off";         break;
		case WIFI_MGR_STATE_IDLE:        ws_str = "idle";        break;
		case WIFI_MGR_STATE_SCANNING:    ws_str = "scanning";    break;
		case WIFI_MGR_STATE_CONNECTING:  ws_str = "connecting";  break;
		case WIFI_MGR_STATE_CONNECTED:   ws_str = "connected";   break;
		case WIFI_MGR_STATE_AP_ONLY:     ws_str = "ap_only";     break;
		case WIFI_MGR_STATE_FAILED:      ws_str = "failed";      break;
	}
	cJSON_AddStringToObject(wifi, "state", ws_str);
	cJSON_AddStringToObject(wifi, "ssid",   wifi_manager_get_connected_ssid() ? wifi_manager_get_connected_ssid() : "");
	cJSON_AddStringToObject(wifi, "sta_ip", wifi_manager_get_sta_ip() ? wifi_manager_get_sta_ip() : "");
	cJSON_AddBoolToObject  (wifi, "ap_enabled", wifi_manager_is_ap_enabled());
	cJSON_AddStringToObject(wifi, "ap_ssid", wifi_manager_get_ap_ssid() ? wifi_manager_get_ap_ssid() : "");
	cJSON_AddStringToObject(wifi, "ap_ip",   wifi_manager_get_ap_ip() ? wifi_manager_get_ap_ip() : "");

	/* SD card */
	cJSON *sd = cJSON_AddObjectToObject(root, "sd");
	bool sd_mounted = sd_manager_is_mounted();
	cJSON_AddBoolToObject(sd, "mounted", sd_mounted);
	if (sd_mounted) {
		size_t total_b = 0, used_b = 0, free_b = 0;
		if (sd_manager_get_info(&total_b, &used_b, &free_b) == ESP_OK) {
			cJSON_AddNumberToObject(sd, "total", (double)total_b);
			cJSON_AddNumberToObject(sd, "used",  (double)used_b);
			cJSON_AddNumberToObject(sd, "free",  (double)free_b);
		}
	}

	/* Signals summary */
	cJSON *sigs = cJSON_AddObjectToObject(root, "signals");
	uint16_t sig_total = signal_get_count();
	uint16_t sig_fresh = 0;
	for (uint16_t i = 0; i < sig_total; i++) {
		signal_t *s = signal_get_by_index(i);
		if (s && !s->is_stale) sig_fresh++;
	}
	cJSON_AddNumberToObject(sigs, "total", sig_total);
	cJSON_AddNumberToObject(sigs, "fresh", sig_fresh);
	cJSON_AddNumberToObject(sigs, "stale", (int)sig_total - (int)sig_fresh);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

static const httpd_uri_t device_info_uri = {
    .uri = "/api/device/info", .method = HTTP_GET,
    .handler = _device_info_handler, .user_ctx = NULL};

/* ── Brightness ──────────────────────────────────────────────────────────── */

static esp_err_t _brightness_get_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	char buf[48];
	snprintf(buf, sizeof(buf), "{\"brightness\":%d}", (int)current_brightness);
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

static esp_err_t _brightness_post_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	char buf[64];
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

	cJSON *j = cJSON_GetObjectItem(root, "brightness");
	if (!cJSON_IsNumber(j)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing brightness");
		return ESP_FAIL;
	}

	int val = (int)j->valuedouble;
	cJSON_Delete(root);

	if (val < 1) val = 1;
	if (val > 100) val = 100;
	set_display_brightness(val);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
	return ESP_OK;
}

static const httpd_uri_t brightness_get_uri = {
    .uri = "/api/brightness", .method = HTTP_GET,
    .handler = _brightness_get_handler, .user_ctx = NULL};
static const httpd_uri_t brightness_post_uri = {
    .uri = "/api/brightness", .method = HTTP_POST,
    .handler = _brightness_post_handler, .user_ctx = NULL};

/* ── CAN Config ──────────────────────────────────────────────────────────── */

static esp_err_t _can_config_get_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	uint8_t bitrate = 2; /* default 500 kbps */
	config_store_load_bitrate(&bitrate);

	char buf[32];
	snprintf(buf, sizeof(buf), "{\"bitrate\":%d}", (int)bitrate);
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

static esp_err_t _can_config_post_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	char buf[64];
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

	cJSON *j = cJSON_GetObjectItem(root, "bitrate");
	if (!cJSON_IsNumber(j)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing bitrate");
		return ESP_FAIL;
	}

	uint8_t bitrate = (uint8_t)j->valuedouble;
	cJSON_Delete(root);

	if (bitrate > 3) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid bitrate index (0-3)");
		return ESP_FAIL;
	}

	config_store_save_bitrate(bitrate);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
	return ESP_OK;
}

static const httpd_uri_t can_config_get_uri = {
    .uri = "/api/can/config", .method = HTTP_GET,
    .handler = _can_config_get_handler, .user_ctx = NULL};
static const httpd_uri_t can_config_post_uri = {
    .uri = "/api/can/config", .method = HTTP_POST,
    .handler = _can_config_post_handler, .user_ctx = NULL};

/* ── System Health ───────────────────────────────────────────────────────── */

static esp_err_t _system_health_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "uptime_s",
		(double)(esp_timer_get_time() / 1000000ULL));
	cJSON_AddNumberToObject(root, "heap_free",
		(double)esp_get_free_heap_size());
	cJSON_AddNumberToObject(root, "heap_min_free",
		(double)esp_get_minimum_free_heap_size());
	cJSON_AddNumberToObject(root, "psram_free",
		(double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

	/* WiFi RSSI (0 if not connected) */
	int rssi = 0;
	wifi_ap_record_t ap_info;
	if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
		rssi = ap_info.rssi;
	}
	cJSON_AddNumberToObject(root, "wifi_rssi", rssi);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

static const httpd_uri_t system_health_uri = {
    .uri = "/api/system/health", .method = HTTP_GET,
    .handler = _system_health_handler, .user_ctx = NULL};

/* ── System Reboot ───────────────────────────────────────────────────────── */

static void _deferred_reboot(void *arg) {
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(500));
	esp_restart();
}

static esp_err_t _system_reboot_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");

	/* Schedule reboot after response is sent */
	xTaskCreate((TaskFunction_t)_deferred_reboot, "reboot", 2048, NULL, 1, NULL);
	return ESP_OK;
}

static const httpd_uri_t system_reboot_uri = {
    .uri = "/api/system/reboot", .method = HTTP_POST,
    .handler = _system_reboot_handler, .user_ctx = NULL};

/* ── Dimmer Config ───────────────────────────────────────────────────────── */

static esp_err_t _dimmer_config_get_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "signal_name", dimmer_config.signal_name);
	cJSON_AddNumberToObject(root, "threshold", dimmer_config.threshold);
	cJSON_AddBoolToObject(root, "is_momentary", dimmer_config.is_momentary);
	cJSON_AddBoolToObject(root, "invert", dimmer_config.invert);
	cJSON_AddNumberToObject(root, "dim_brightness", dimmer_config.dim_brightness);
	cJSON_AddBoolToObject(root, "enabled", dimmer_config.enabled);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

static void _deferred_dimmer_subscribe(void *arg) {
	(void)arg;
	dimmer_subscribe();
}

static esp_err_t _dimmer_config_post_handler(httpd_req_t *req) {
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

	if (!rdm_lvgl_lock(100)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
		return ESP_FAIL;
	}

	cJSON *j;
	if ((j = cJSON_GetObjectItem(root, "signal_name")) && cJSON_IsString(j)) {
		strncpy(dimmer_config.signal_name, j->valuestring,
				sizeof(dimmer_config.signal_name) - 1);
		dimmer_config.signal_name[sizeof(dimmer_config.signal_name) - 1] = '\0';
	}
	if ((j = cJSON_GetObjectItem(root, "threshold")) && cJSON_IsNumber(j))
		dimmer_config.threshold = (float)j->valuedouble;
	if ((j = cJSON_GetObjectItem(root, "is_momentary")))
		dimmer_config.is_momentary = cJSON_IsTrue(j);
	if ((j = cJSON_GetObjectItem(root, "invert")))
		dimmer_config.invert = cJSON_IsTrue(j);
	if ((j = cJSON_GetObjectItem(root, "dim_brightness")) && cJSON_IsNumber(j))
		dimmer_config.dim_brightness = (uint8_t)j->valuedouble;
	if ((j = cJSON_GetObjectItem(root, "enabled")))
		dimmer_config.enabled = cJSON_IsTrue(j);

	rdm_lvgl_unlock();
	cJSON_Delete(root);

	save_dimmer_config_to_nvs();
	lv_async_call(_deferred_dimmer_subscribe, NULL);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
	return ESP_OK;
}

static const httpd_uri_t dimmer_config_get_uri = {
    .uri = "/api/dimmer/config", .method = HTTP_GET,
    .handler = _dimmer_config_get_handler, .user_ctx = NULL};
static const httpd_uri_t dimmer_config_post_uri = {
    .uri = "/api/dimmer/config", .method = HTTP_POST,
    .handler = _dimmer_config_post_handler, .user_ctx = NULL};

/* ── URI registration ────────────────────────────────────────────────────── */

void web_server_system_register(httpd_handle_t server) {
	REGISTER_URI(server, &device_info_uri);
	REGISTER_URI(server, &brightness_get_uri);
	REGISTER_URI(server, &brightness_post_uri);
	REGISTER_URI(server, &can_config_get_uri);
	REGISTER_URI(server, &can_config_post_uri);
	REGISTER_URI(server, &system_health_uri);
	REGISTER_URI(server, &system_reboot_uri);
	REGISTER_URI(server, &dimmer_config_get_uri);
	REGISTER_URI(server, &dimmer_config_post_uri);
}
