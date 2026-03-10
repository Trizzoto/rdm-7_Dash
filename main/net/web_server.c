#include "web_server.h"
#include "cJSON.h"
#include "display_capture.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "layout/layout_manager.h"
#include "lvgl.h"
#include "system/rdm_settings.h"
#include "ui/dashboard.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/preset_picker.h"
#include "ui/ui.h"
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>

/* Fallback for static-analyser builds that don't see layout_manager.h's define.
 */
#ifndef LAYOUT_MAX_FILE_BYTES
#define LAYOUT_MAX_FILE_BYTES 16384
#endif

/* Embedded web UI (provided by EMBED_TXTFILES in CMakeLists.txt) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

/* LVGL lock helpers (defined in main.c) */
extern bool example_lvgl_lock(int timeout_ms);
extern void example_lvgl_unlock(void);

/* ── Deferred screen reload (runs on LVGL task via lv_async_call) ────────── */

static void _deferred_screen_reload(void *arg) {
	(void)arg;
	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
	ui_Screen3_screen_init();
	lv_scr_load(ui_Screen3);
	if (old && old != ui_Screen3)
		lv_obj_del(old);
}

/* Pending preview JSON — guarded by atomic pointer swap. Only one preview
 * can be pending at a time; newer previews replace older ones. */
static char *s_pending_preview_json = NULL;

static void _set_pending_preview(char *json) {
	char *old = s_pending_preview_json;
	s_pending_preview_json = json;
	free(old); /* free previous if the LVGL task hasn't consumed it yet */
}

static void _deferred_preview_apply(void *arg) {
	(void)arg;
	char *json = s_pending_preview_json;
	s_pending_preview_json = NULL;
	if (!json) return;

	cJSON *root = cJSON_Parse(json);
	free(json);
	if (!root) return;

	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
	ui_Screen3_preview_layout(root);
	lv_scr_load(ui_Screen3);
	if (old && old != ui_Screen3)
		lv_obj_del(old);
	cJSON_Delete(root);
}

// HTTP handler for the main page (serves embedded web/index.html)
static esp_err_t index_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	size_t len = index_html_end - index_html_start;
	return httpd_resp_send(req, (const char *)index_html_start, len);
}

// HTTP handler for screenshot API
static esp_err_t screenshot_handler(httpd_req_t *req) {
	ESP_LOGI(TAG, "Screenshot requested");

	uint8_t *screenshot_buffer = NULL;
	size_t screenshot_size = 0;

	esp_err_t ret =
		display_capture_screenshot(&screenshot_buffer, &screenshot_size);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to capture screenshot: %s", esp_err_to_name(ret));
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Screenshot capture failed");
		return ESP_FAIL;
	}

	// Set headers for binary data
	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

	// Send the screenshot data
	esp_err_t send_ret =
		httpd_resp_send(req, (const char *)screenshot_buffer, screenshot_size);

	// Clean up
	display_capture_free_buffer(screenshot_buffer);

	if (send_ret == ESP_OK) {
		ESP_LOGI(TAG, "Screenshot sent successfully (%zu bytes)",
				 screenshot_size);
	} else {
		ESP_LOGE(TAG, "Failed to send screenshot");
	}

	return send_ret;
}

// URI handlers
static const httpd_uri_t index_uri = {
	.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};

static const httpd_uri_t screenshot_uri = {.uri = "/screenshot",
										   .method = HTTP_GET,
										   .handler = screenshot_handler,
										   .user_ctx = NULL};

// HTTP handler for exporting current layout JSON
static esp_err_t layout_current_handler(httpd_req_t *req) {
	// Get active layout name (for the "name" field)
	char layout_name[LAYOUT_MAX_NAME];
	if (rdm_settings_get_active_layout(layout_name, sizeof(layout_name)) !=
		ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to read active layout name");
		return ESP_FAIL;
	}

	// Must hold LVGL mutex — widgets live on the LVGL task's core
	if (!example_lvgl_lock(1000)) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"LVGL busy");
		return ESP_FAIL;
	}

	widget_t **widgets = dashboard_get_widgets();
	uint8_t count = dashboard_get_widget_count();

	cJSON *root = layout_manager_build_json(layout_name, widgets, count);
	example_lvgl_unlock();

	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to build layout JSON");
		return ESP_FAIL;
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to serialise layout JSON");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

	esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

static const httpd_uri_t layout_current_uri = {.uri = "/api/layout/current",
											   .method = HTTP_GET,
											   .handler =
												   layout_current_handler,
											   .user_ctx = NULL};

/* GET /api/layout/raw?name=<layout_name> — return raw layout JSON from file
 * (for editing in web UI without switching active layout). */
static esp_err_t layout_raw_handler(httpd_req_t *req) {
	char query_buf[128];
	esp_err_t qerr =
		httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf));
	if (qerr != ESP_OK && qerr != ESP_ERR_HTTPD_RESULT_TRUNC) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
		return ESP_FAIL;
	}

	const char *name_val = strstr(query_buf, "name=");
	if (!name_val) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name=");
		return ESP_FAIL;
	}
	name_val += 5; /* skip "name=" */
	const char *end = strchr(name_val, '&');
	size_t name_len = end ? (size_t)(end - name_val) : strlen(name_val);
	if (name_len == 0 || name_len >= LAYOUT_MAX_NAME) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name length");
		return ESP_FAIL;
	}
	for (size_t i = 0; i < name_len; i++) {
		if (name_val[i] == '/' || name_val[i] == '\\') {
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
			return ESP_FAIL;
		}
	}

	char layout_name[LAYOUT_MAX_NAME];
	memcpy(layout_name, name_val, name_len);
	layout_name[name_len] = '\0';

	char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
	if (!buf) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Out of memory");
		return ESP_FAIL;
	}

	size_t out_len = 0;
	esp_err_t err = layout_manager_read_raw(layout_name, buf,
											LAYOUT_MAX_FILE_BYTES, &out_len);
	if (err != ESP_OK) {
		free(buf);
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Layout not found");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	esp_err_t send_ret = httpd_resp_send(req, buf, out_len);
	free(buf);
	return send_ret;
}

static const httpd_uri_t layout_raw_uri = {.uri = "/api/layout/raw",
										   .method = HTTP_GET,
										   .handler = layout_raw_handler,
										   .user_ctx = NULL};

// HTTP handler for importing/saving a new layout JSON
static esp_err_t layout_save_handler(httpd_req_t *req) {
	/* Check query for apply=0 (save without switching active or reloading) */
	bool apply_after_save = true;
	char query_buf[64];
	if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) ==
		ESP_OK) {
		const char *apply_val = strstr(query_buf, "apply=0");
		if (apply_val && (apply_val == query_buf || apply_val[-1] == '&') &&
			(apply_val[7] == '\0' || apply_val[7] == '&'))
			apply_after_save = false;
	}

	int total_len = req->content_len;
	if (total_len <= 0 || total_len > LAYOUT_MAX_FILE_BYTES) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid layout size");
		return ESP_FAIL;
	}

	char *buf = malloc(total_len + 1);
	if (!buf) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Out of memory");
		return ESP_FAIL;
	}

	int received = 0;
	while (received < total_len) {
		int r = httpd_req_recv(req, buf + received, total_len - received);
		if (r <= 0) {
			free(buf);
			if (r == HTTPD_SOCK_ERR_TIMEOUT) {
				httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT,
									"Request timeout");
			} else {
				httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
									"Failed to receive body");
			}
			return ESP_FAIL;
		}
		received += r;
	}
	buf[received] = '\0';

	/* Boot-loop prevention: reject syntactically invalid JSON before writing
	 * to LittleFS. Never call layout_manager_save_raw until parse succeeds. */
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) {
		ESP_LOGW(TAG,
				 "POST /api/layout/save: invalid JSON rejected (not written)");
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	cJSON *widgets_arr = cJSON_GetObjectItemCaseSensitive(root, "widgets");
	if (!cJSON_IsString(name_item) || !cJSON_IsArray(widgets_arr)) {
		ESP_LOGW(TAG, "POST /api/layout/save: missing or invalid "
					  "'name'/'widgets' (not written)");
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Missing or invalid 'name'/'widgets'");
		return ESP_FAIL;
	}

	/* Copy layout name out of the cJSON tree so it remains valid after
	 * cJSON_Delete(). name_item->valuestring points into root's memory. */
	char layout_name[LAYOUT_MAX_NAME];
	strncpy(layout_name, name_item->valuestring, sizeof(layout_name) - 1);
	layout_name[sizeof(layout_name) - 1] = '\0';

	// Persist raw JSON to LittleFS
	esp_err_t err = layout_manager_save_raw(layout_name, root);
	cJSON_Delete(root);
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to save layout to LittleFS");
		return ESP_FAIL;
	}

	if (apply_after_save) {
		// Update active layout name in NVS
		if (rdm_settings_set_active_layout(layout_name) != ESP_OK) {
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
								"Failed to set active layout");
			return ESP_FAIL;
		}

		// Defer heavy screen rebuild to the LVGL task to avoid
		// holding the mutex for 100-500ms and blocking other handlers.
		lv_async_call(_deferred_screen_reload, NULL);
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	const char *ok = "{\"status\":\"ok\"}";
	return httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t layout_save_uri = {.uri = "/api/layout/save",
											.method = HTTP_POST,
											.handler = layout_save_handler,
											.user_ctx = NULL};

/* POST /api/layout/preview — apply layout JSON live without saving to file. */
static esp_err_t layout_preview_handler(httpd_req_t *req) {
	int total_len = req->content_len;
	if (total_len <= 0 || total_len > LAYOUT_MAX_FILE_BYTES) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid layout size");
		return ESP_FAIL;
	}

	char *buf = malloc(total_len + 1);
	if (!buf) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Out of memory");
		return ESP_FAIL;
	}

	int received = 0;
	while (received < total_len) {
		int r = httpd_req_recv(req, buf + received, total_len - received);
		if (r <= 0) {
			free(buf);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
								"Recv failed");
			return ESP_FAIL;
		}
		received += r;
	}
	buf[received] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	/* Stash the JSON string for deferred apply on the LVGL task.
	 * The previous pending preview (if any) is freed here. */
	char *json_copy = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_copy) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to serialize preview JSON");
		return ESP_FAIL;
	}
	_set_pending_preview(json_copy);
	lv_async_call(_deferred_preview_apply, NULL);
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t layout_preview_uri = {.uri = "/api/layout/preview",
											   .method = HTTP_POST,
											   .handler =
												   layout_preview_handler,
											   .user_ctx = NULL};

static esp_err_t layout_list_handler(httpd_req_t *req) {
	char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
	int count = layout_manager_list(names, LAYOUT_MAX_COUNT);
	if (count < 0) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to list layouts");
		return ESP_FAIL;
	}

	char active_name[LAYOUT_MAX_NAME];
	if (layout_manager_get_active(active_name, sizeof(active_name)) != ESP_OK) {
		strcpy(active_name, "default");
	}

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "active", active_name);
	cJSON *arr = cJSON_AddArrayToObject(root, "layouts");
	for (int i = 0; i < count; i++) {
		cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to serialize JSON");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

static esp_err_t presets_list_handler(httpd_req_t *req) {
	cJSON *root = cJSON_CreateArray();
	for (size_t i = 0; i < preconfig_items_count; i++) {
		if (preconfig_items[i].ecu == NULL)
			continue;
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "ecu", preconfig_items[i].ecu);
		cJSON_AddStringToObject(item, "version", preconfig_items[i].version);
		cJSON_AddStringToObject(item, "label", preconfig_items[i].label);
		cJSON_AddStringToObject(item, "can_id", preconfig_items[i].can_id);
		cJSON_AddNumberToObject(item, "endianess",
								preconfig_items[i].endianess);
		cJSON_AddNumberToObject(item, "bit_start",
								preconfig_items[i].bit_start);
		cJSON_AddNumberToObject(item, "bit_length",
								preconfig_items[i].bit_length);
		cJSON_AddNumberToObject(item, "scale", preconfig_items[i].scale);
		cJSON_AddNumberToObject(item, "offset",
								preconfig_items[i].value_offset);
		cJSON_AddNumberToObject(item, "decimals", preconfig_items[i].decimals);
		cJSON_AddBoolToObject(item, "is_signed", preconfig_items[i].is_signed);
		cJSON_AddItemToArray(root, item);
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Alloc failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str, strlen(json_str));
	free(json_str);
	return res;
}

static const httpd_uri_t presets_list_uri = {.uri = "/api/presets",
											 .method = HTTP_GET,
											 .handler = presets_list_handler,
											 .user_ctx = NULL};

static const httpd_uri_t layout_list_uri = {.uri = "/api/layout/list",
											.method = HTTP_GET,
											.handler = layout_list_handler,
											.user_ctx = NULL};

static esp_err_t layout_set_handler(httpd_req_t *req) {
	char buf[128];
	int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (received <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Failed to receive body");
		return ESP_FAIL;
	}
	buf[received] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	if (!cJSON_IsString(name_item)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name'");
		return ESP_FAIL;
	}

	char layout_name[LAYOUT_MAX_NAME];
	strncpy(layout_name, name_item->valuestring, sizeof(layout_name) - 1);
	layout_name[sizeof(layout_name) - 1] = '\0';
	cJSON_Delete(root);

	if (layout_manager_set_active(layout_name) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to set active layout");
		return ESP_FAIL;
	}

	lv_async_call(_deferred_screen_reload, NULL);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t layout_set_uri = {.uri = "/api/layout/set",
										   .method = HTTP_POST,
										   .handler = layout_set_handler,
										   .user_ctx = NULL};

esp_err_t web_server_start(void) {
	if (server != NULL) {
		ESP_LOGW(TAG, "Web server already running");
		return ESP_OK;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = WEB_SERVER_PORT;
	/* Increase stack size to handle LVGL snapshot + capture logic safely. */
	config.stack_size = 8192;
	config.max_uri_handlers = 14;
	config.max_resp_headers = 8;
	config.lru_purge_enable = true;

	ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

	esp_err_t ret = httpd_start(&server, &config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
		return ret;
	}

	// Register URI handlers
	httpd_register_uri_handler(server, &index_uri);
	httpd_register_uri_handler(server, &screenshot_uri);
	httpd_register_uri_handler(server, &layout_current_uri);
	httpd_register_uri_handler(server, &layout_raw_uri);
	httpd_register_uri_handler(server, &layout_save_uri);
	httpd_register_uri_handler(server, &layout_preview_uri);
	httpd_register_uri_handler(server, &layout_list_uri);
	httpd_register_uri_handler(server, &presets_list_uri);
	httpd_register_uri_handler(server, &layout_set_uri);
	httpd_register_uri_handler(server, &layout_preview_uri);

	ESP_LOGI(TAG, "Web server started successfully");
	return ESP_OK;
}

esp_err_t web_server_stop(void) {
	if (server == NULL) {
		ESP_LOGW(TAG, "Web server not running");
		return ESP_OK;
	}

	esp_err_t ret = httpd_stop(server);
	if (ret == ESP_OK) {
		server = NULL;
		ESP_LOGI(TAG, "Web server stopped");
	} else {
		ESP_LOGE(TAG, "Failed to stop web server: %s", esp_err_to_name(ret));
	}

	return ret;
}

bool web_server_is_running(void) { return (server != NULL); }