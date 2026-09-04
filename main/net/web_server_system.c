/* web_server_system.c — device info, brightness, CAN config, system health,
 *                        system reboot, dimmer config
 *
 * Endpoints:
 *   GET  /api/device/info      full device/hw/system/signal snapshot
 *   GET  /api/brightness       current display brightness
 *   POST /api/brightness       set display brightness
 *   GET  /api/can/config       CAN bitrate index (saved + live), bus state
 *   POST /api/can/config       set CAN bitrate index — applied LIVE, optionally
 *                              without persisting (see the handler)
 *   GET  /api/system/health    uptime, heap, psram, WiFi RSSI
 *   POST /api/system/reboot    deferred esp_restart()
 *   GET  /api/dimmer/config    brightness-dimmer signal config
 *   POST /api/dimmer/config    update dimmer config + re-subscribe */
#include "web_server_internal.h"
#include "system/rdm_lv_async.h"
#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/twai.h"
#include "can/can_manager.h"
#include "net/wifi_manager.h"
#include "storage/bootloader_selfupdate.h"
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

	return web_server_send_json(req, root);
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
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

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

/* GET reports the SAVED rate and the LIVE one separately, because they are
 * allowed to differ: the OBD2 bus scan probes other rates transiently, and
 * so does Studio's keypad wizard, which walks the bus looking for a device
 * whose rate nobody knows. A caller that reads one number cannot tell "the
 * dash is set to 500k" from "the dash is listening at 500k right now", and
 * those are different questions when something is mid-probe. */
static esp_err_t _can_config_get_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	uint8_t bitrate = 2; /* default 500 kbps */
	config_store_load_bitrate(&bitrate);

	char buf[96];
	snprintf(buf, sizeof(buf),
	         "{\"bitrate\":%d,\"live\":%d,\"promiscuous\":%s,\"suspended\":%s}",
	         (int)bitrate, (int)can_get_bitrate_index(),
	         can_is_promiscuous() ? "true" : "false",
	         can_is_suspended()   ? "true" : "false");
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

/* Body: { "bitrate": 0-3, "persist": true }
 *
 * This used to save to NVS and stop there, so a bitrate change did nothing at
 * all until somebody rebooted the dash — and the endpoint answered "ok" the
 * whole time. That cost real hours on the bench: the rate was set, the reply
 * said it worked, and the bus stayed silent, which looks exactly like a dead
 * device on the other end. can_change_bitrate() had been sitting in
 * can_manager.c the entire time; the on-device settings screen and the OBD2
 * scan both call it. Now this does too.
 *
 * "persist": false applies the rate WITHOUT writing it to NVS — for a caller
 * that is walking rates to find something and will put the dash back where it
 * found it. Defaults to true so every existing caller keeps its old meaning.
 *
 * The response reports the rate actually in effect afterwards, which is not
 * always the one asked for: while the bus scan owns the peripheral the change
 * is deferred to can_resume(), and saying "ok" to that would be the same lie
 * this endpoint used to tell. */
static esp_err_t _can_config_post_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	char buf[64];
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

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
	cJSON *jp = cJSON_GetObjectItem(root, "persist");
	bool persist = cJSON_IsBool(jp) ? cJSON_IsTrue(jp) : true;
	cJSON_Delete(root);

	if (bitrate > 3) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid bitrate index (0-3)");
		return ESP_FAIL;
	}

	/* Persist BEFORE applying: can_resume() reloads the saved rate from NVS
	 * when a deferred change lands, so a persisted rate survives the defer.
	 * (can_persist_bitrate's own docs call out this ordering.) */
	if (persist) config_store_save_bitrate(bitrate);
	can_change_bitrate(bitrate);

	bool deferred = can_is_suspended();
	char out[128];
	snprintf(out, sizeof(out),
	         "{\"status\":\"ok\",\"bitrate\":%d,\"live\":%d,\"persisted\":%s,"
	         "\"deferred\":%s}",
	         (int)bitrate, (int)can_get_bitrate_index(),
	         persist ? "true" : "false", deferred ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, out);
	return ESP_OK;
}


/* ── Bootloader self-update ──────────────────────────────────────────────
 *
 * The CAN boot-window park lives in the 2nd-stage bootloader (ADR-0047) and
 * OTA writes app partitions only, so without this a dash already in a car
 * needs a USB reflash to get it. Deliberately two endpoints: a GET that says
 * whether an update is even pending, and a POST that has to be asked for.
 * Nothing calls the POST automatically — see bootloader_selfupdate.h for why
 * that matters. */

/* GET /api/bootloader/status */
static esp_err_t bootloader_status_handler(httpd_req_t *req) {
	bl_selfupdate_state_t st = bootloader_selfupdate_check();
	cJSON *root = cJSON_CreateObject();
	const char *s = (st == BL_SELFUPDATE_UP_TO_DATE) ? "up_to_date"
	              : (st == BL_SELFUPDATE_DIFFERENT)  ? "update_available"
	                                                 : "unreadable";
	cJSON_AddStringToObject(root, "state", s);
	cJSON_AddBoolToObject  (root, "update_available", st == BL_SELFUPDATE_DIFFERENT);
	cJSON_AddNumberToObject(root, "image_bytes", bootloader_selfupdate_image_size());
	/* Which features flash has vs which this app carries. 0 = no stamp, i.e. a
	 * bootloader older than the stamp, so we cannot tell whether it parks the
	 * CAN line and offer the write rather than assume. bytes_identical is
	 * diagnostic only — see bootloader_selfupdate.h for why it does not drive
	 * the banner. */
	bl_selfupdate_info_t info;
	if (bootloader_selfupdate_info(&info)) {
		cJSON_AddNumberToObject(root, "installed_feature", info.installed);
		cJSON_AddNumberToObject(root, "carried_feature",   info.carried);
		cJSON_AddBoolToObject  (root, "bytes_identical",   info.bytes_identical);
	}
	/* Said plainly because the consequence is not recoverable over the air. */
	cJSON_AddStringToObject(root, "warning",
	    "Writing the bootloader briefly leaves the dash with none. If power is "
	    "lost during the write the dash will not boot and must be recovered "
	    "over USB. Do this with the engine running or on a stable supply.");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

/* POST /api/bootloader/update  body: {"confirm":"write-bootloader"} */
static esp_err_t bootloader_update_handler(httpd_req_t *req) {
	char buf[128];
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;
	cJSON *root = cJSON_Parse(buf);
	if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }
	const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "confirm");
	bool confirmed = cJSON_IsString(jc) &&
	                 strcmp(jc->valuestring, "write-bootloader") == 0;
	cJSON_Delete(root);
	/* A typed confirmation, so this cannot be reached by a stray POST from a
	 * script that was iterating endpoints. */
	if (!confirmed) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "Send {\"confirm\":\"write-bootloader\"}");
		return ESP_FAIL;
	}

	const char *detail = "";
	bool committed = false;
	esp_err_t err = bootloader_selfupdate_apply(&detail, &committed);

	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject  (resp, "ok", err == ESP_OK);
	cJSON_AddStringToObject(resp, "detail", detail ? detail : "");
	cJSON_AddNumberToObject(resp, "err", err);
	/* The distinction the caller actually needs on failure: whether the old
	 * bootloader is still intact, or whether this dash now needs a cable. */
	cJSON_AddBoolToObject  (resp, "flash_modified", committed);
	cJSON_AddBoolToObject  (resp, "reboot_required", err == ESP_OK && committed);
	if (err != ESP_OK && committed) {
		cJSON_AddStringToObject(resp, "recovery",
		    "The bootloader region was modified and did not verify. Do NOT "
		    "power cycle. Recover over USB with the Full Recovery Flash.");
	}
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, resp);
}

static const httpd_uri_t bootloader_status_uri = {
	.uri = "/api/bootloader/status", .method = HTTP_GET,
	.handler = bootloader_status_handler, .user_ctx = NULL };
static const httpd_uri_t bootloader_update_uri = {
	.uri = "/api/bootloader/update", .method = HTTP_POST,
	.handler = bootloader_update_handler, .user_ctx = NULL };

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

	return web_server_send_json(req, root);
}

static const httpd_uri_t system_health_uri = {
    .uri = "/api/system/health", .method = HTTP_GET,
    .handler = _system_health_handler, .user_ctx = NULL};

/* ── System Reboot ───────────────────────────────────────────────────────── */

static esp_err_t _system_reboot_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	/* Arm the reboot BEFORE replying, so the reply can tell the truth. This
	 * used to xTaskCreate a 2 KB task and answer "rebooting" without checking
	 * — on a starved dash the spawn failed and the dash just kept running. */
	if (!web_server_schedule_reboot(500)) {
		httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_sendstr(req,
			"{\"error\":\"could not schedule a reboot — power-cycle the dash\"}");
		return ESP_OK;
	}
	httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
	return ESP_OK;
}

static const httpd_uri_t system_reboot_uri = {
    .uri = "/api/system/reboot", .method = HTTP_POST,
    .handler = _system_reboot_handler, .user_ctx = NULL};

/* ── Dimmer Config ───────────────────────────────────────────────────────── */

static esp_err_t _dimmer_config_get_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "signal_name", dimmer_config.signal_name);
	cJSON_AddNumberToObject(root, "threshold", dimmer_config.threshold);
	cJSON_AddBoolToObject(root, "is_momentary", dimmer_config.is_momentary);
	cJSON_AddBoolToObject(root, "invert", dimmer_config.invert);
	cJSON_AddNumberToObject(root, "dim_brightness", dimmer_config.dim_brightness);
	cJSON_AddBoolToObject(root, "enabled", dimmer_config.enabled);

	return web_server_send_json(req, root);
}

static void _deferred_dimmer_subscribe(void *arg) {
	(void)arg;
	dimmer_subscribe();
}

static esp_err_t _dimmer_config_post_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	char buf[256];
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

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
	rdm_async_call(_deferred_dimmer_subscribe, NULL);

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
	REGISTER_URI(server, &bootloader_status_uri);
	REGISTER_URI(server, &bootloader_update_uri);
	REGISTER_URI(server, &dimmer_config_get_uri);
	REGISTER_URI(server, &dimmer_config_post_uri);
}
