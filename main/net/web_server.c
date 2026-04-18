#include "web_server.h"
#include "cJSON.h"
#include "display_capture.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "layout/layout_manager.h"
#include "lvgl.h"
#include "storage/sd_manager.h"
#include "system/device_id.h"
#include "system/rdm_settings.h"
#include "system/screen_config.h"
#include "ui/dashboard.h"
#include "ui/screens/splash_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/preset_picker.h"
#include "ui/ui.h"
#include "widgets/font_manager.h"
#include "widgets/signal.h"
#include "widgets/signal_sim.h"
#include "ui/settings/device_settings.h"
#include "storage/boot_assets.h"
#include "storage/config_store.h"
#include "storage/data_logger.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

/* Fallback for static-analyser builds that don't see layout_manager.h's define.
 */
#ifndef LAYOUT_MAX_FILE_BYTES
#define LAYOUT_MAX_FILE_BYTES 32768
#endif

/* Embedded web UI (provided by EMBED_TXTFILES in CMakeLists.txt) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

/* LVGL lock helpers (defined in main.c) */
extern bool example_lvgl_lock(int timeout_ms);
extern void example_lvgl_unlock(void);

/* ── Path-safety check for user-supplied names (no traversal) ──────────── */

static bool _name_is_safe(const char *name) {
	if (!name || !name[0]) return false;
	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == '.' || *p < 0x20) return false;
	}
	return true;
}

/* Like _name_is_safe but allows a single dot for file extension (e.g. ".csv") */
static bool _filename_is_safe(const char *name) {
	if (!name || !name[0]) return false;
	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p < 0x20) return false;
	}
	/* Reject ".." sequences */
	if (strstr(name, "..")) return false;
	return true;
}

/* ── Debounced screen reload (runs on LVGL task via lv_async_call) ───────── */

static char *s_pending_preview_json = NULL;
static lv_timer_t *s_reload_debounce_timer  = NULL;
static lv_timer_t *s_splash_debounce_timer  = NULL;

#define RELOAD_DEBOUNCE_MS 600

static void _do_screen_reload(lv_timer_t *t) {
	(void)t;
	s_reload_debounce_timer = NULL;
	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
	ui_Screen3_screen_init();
	lv_scr_load(ui_Screen3);
	if (old && old != ui_Screen3 && lv_obj_is_valid(old))
		lv_obj_del(old);
}

/* Schedule or reset the debounce timer (must run on LVGL task) */
static void _deferred_screen_reload(void *arg) {
	(void)arg;
	if (s_reload_debounce_timer) {
		lv_timer_reset(s_reload_debounce_timer);
	} else {
		s_reload_debounce_timer = lv_timer_create(_do_screen_reload,
												   RELOAD_DEBOUNCE_MS, NULL);
		lv_timer_set_repeat_count(s_reload_debounce_timer, 1);
	}
}

static void _deferred_preview_apply(void *arg) {
	char *json = (char *)arg;
	if (!json) return;

	/* Clear the pending pointer if this is the latest preview */
	if (s_pending_preview_json == json)
		s_pending_preview_json = NULL;

	cJSON *root = cJSON_Parse(json);
	free(json);
	if (!root) return;

	if (splash_screen_is_edit_mode()) {
		/* Apply preview to splash screen */
		splash_screen_apply_preview(root);
	} else {
		/* Apply preview to dashboard */
		lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
		ui_Screen3_preview_layout(root);
		lv_scr_load(ui_Screen3);
		if (old && old != ui_Screen3)
			lv_obj_del(old);
	}
	cJSON_Delete(root);
}

/* ── Splash screen reload (debounced, runs on LVGL task) ─────────────── */

static void _do_splash_reload(lv_timer_t *t) {
	(void)t;
	s_splash_debounce_timer = NULL;
	splash_screen_enter_edit_mode();
}

static void _deferred_splash_reload(void *arg) {
	(void)arg;
	if (s_splash_debounce_timer) {
		lv_timer_reset(s_splash_debounce_timer);
	} else {
		s_splash_debounce_timer = lv_timer_create(_do_splash_reload,
												   RELOAD_DEBOUNCE_MS, NULL);
		lv_timer_set_repeat_count(s_splash_debounce_timer, 1);
	}
}

/* ── Screen switch (runs on LVGL task) ───────────────────────────────── */

static void _deferred_screen_switch_splash(void *arg) {
	(void)arg;
	splash_screen_enter_edit_mode();
}

static void _deferred_screen_switch_dashboard(void *arg) {
	(void)arg;
	splash_screen_exit_edit_mode();
}

// HTTP handler for the main page (serves embedded web/index.html)
// TODO: Serve gzip-compressed HTML for ~4x size reduction (~433KB -> ~60KB).
// Implementation: change CMakeLists.txt EMBED_TXTFILES to EMBED_FILES with a
// pre-compressed .gz file, then set Content-Encoding: gzip here when the
// client's Accept-Encoding includes "gzip". This requires a build-time gzip
// step (e.g. a custom CMake command or pre-committed .gz file).
static esp_err_t index_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html; charset=UTF-8");
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

// Lightweight version check — web editor polls this to avoid fetching the
// full layout JSON on every sync cycle.
static esp_err_t layout_version_handler(httpd_req_t *req) {
	char buf[32];
	snprintf(buf, sizeof(buf), "{\"v\":%lu}",
			 (unsigned long)layout_manager_get_version());
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, buf);
	return ESP_OK;
}

static const httpd_uri_t layout_version_uri = {
	.uri = "/api/layout/version",
	.method = HTTP_GET,
	.handler = layout_version_handler,
	.user_ctx = NULL};

// HTTP handler for exporting current layout JSON
static esp_err_t layout_current_handler(httpd_req_t *req) {
	/* Check if we're in splash edit mode */
	bool is_splash = splash_screen_is_edit_mode();

	char layout_name[LAYOUT_MAX_NAME];
	if (is_splash) {
		snprintf(layout_name, sizeof(layout_name), "_splash_%s",
		         splash_screen_get_active_name());
	} else if (rdm_settings_get_active_layout(layout_name,
	                                          sizeof(layout_name)) != ESP_OK) {
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

	widget_t **widgets;
	uint8_t count;
	if (is_splash) {
		widgets = splash_screen_get_widgets();
		count = splash_screen_get_widget_count();
	} else {
		widgets = dashboard_get_widgets();
		count = dashboard_get_widget_count();
	}

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
	char layout_name[LAYOUT_MAX_NAME];
	memcpy(layout_name, name_val, name_len);
	layout_name[name_len] = '\0';

	/* Reject path traversal: slash, backslash, or ".." */
	for (size_t i = 0; i < name_len; i++) {
		if (layout_name[i] == '/' || layout_name[i] == '\\') {
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
			return ESP_FAIL;
		}
	}
	if (strstr(layout_name, "..")) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

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

	/* Reject path traversal in layout names */
	if (!_name_is_safe(layout_name)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Invalid layout name");
		return ESP_FAIL;
	}

	/* Ensure schema_version is always set so layout_load won't reject it */
	cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
	if (!cJSON_IsNumber(sv) || sv->valueint < 1) {
		if (sv) cJSON_ReplaceItemInObjectCaseSensitive(root, "schema_version",
					cJSON_CreateNumber(LAYOUT_SCHEMA_VERSION));
		else cJSON_AddNumberToObject(root, "schema_version",
					LAYOUT_SCHEMA_VERSION);
	}

	bool is_splash = (strncmp(layout_name, "_splash_", 8) == 0);

	/* Protect the default layout from being overwritten via web editor */
	if (!is_splash && strcmp(layout_name, "default") == 0) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Cannot overwrite default layout");
		return ESP_FAIL;
	}

	// Persist raw JSON to LittleFS
	esp_err_t err = layout_manager_save_raw(layout_name, root);
	cJSON_Delete(root);
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to save layout to LittleFS");
		return ESP_FAIL;
	}

	if (apply_after_save) {
		if (is_splash) {
			/* Update active splash NVS + reload splash screen */
			const char *bare = layout_name + 8; /* skip "_splash_" */
			layout_manager_set_active_splash(bare);
			splash_screen_set_active_name(bare);
			lv_async_call(_deferred_splash_reload, NULL);
		} else {
			// Update active layout name in NVS
			if (rdm_settings_set_active_layout(layout_name) != ESP_OK) {
				httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
									"Failed to set active layout");
				return ESP_FAIL;
			}

			// Defer heavy screen rebuild to the LVGL task
			lv_async_call(_deferred_screen_reload, NULL);
		}
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
	 * Free any previous pending preview that hasn't been consumed yet. */
	char *json_copy = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_copy) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to serialize preview JSON");
		return ESP_FAIL;
	}
	/* Atomically swap the pending pointer.  The old pointer (if any) is NOT
	 * freed here because a previous lv_async_call may still reference it.
	 * Instead, the async callback (_deferred_preview_apply) frees its own
	 * argument after use, so each buffer is freed exactly once. */
	s_pending_preview_json = json_copy;
	lv_async_call(_deferred_preview_apply, json_copy);
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
		/* Hide system layouts (prefixed with _) from the layout list */
		if (names[i][0] == '_') continue;
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

/* Round a float to remove single-precision artifacts (e.g. 0.100000005 → 0.1) */
static double _round_float(float v) {
	if (v == 0.0f) return 0.0;
	char buf[24];
	snprintf(buf, sizeof(buf), "%.6g", (double)v);
	return strtod(buf, NULL);
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
		cJSON_AddNumberToObject(item, "scale",
								_round_float(preconfig_items[i].scale));
		cJSON_AddNumberToObject(item, "offset",
								_round_float(preconfig_items[i].value_offset));
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

/* ── Custom signal presets (stored as JSON in /lfs/presets/) ──────────── */

#define LFS_PRESET_DIR "/lfs/presets"
#define CUSTOM_PRESET_MAX_BYTES 16384

static void _ensure_preset_dir(void) {
	struct stat st;
	if (stat(LFS_PRESET_DIR, &st) != 0)
		mkdir(LFS_PRESET_DIR, 0755);
}

static void _sanitize_preset_filename(const char *ecu, const char *version,
									  char *out, size_t out_len) {
	size_t pos = 0;
	for (const char *p = ecu; *p && pos < out_len - 6; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9'))
			out[pos++] = *p;
		else
			out[pos++] = '_';
	}
	if (pos < out_len - 6)
		out[pos++] = '_';
	for (const char *p = version; *p && pos < out_len - 6; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9'))
			out[pos++] = *p;
		else
			out[pos++] = '_';
	}
	out[pos] = '\0';
	strncat(out, ".json", out_len - pos - 1);
}

/* GET /api/presets/custom — list all custom presets as flat signal array */
static esp_err_t custom_presets_list_handler(httpd_req_t *req) {
	_ensure_preset_dir();

	cJSON *root = cJSON_CreateArray();
	DIR *d = opendir(LFS_PRESET_DIR);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t flen = strlen(de->d_name);
			if (flen <= 5 || strcmp(de->d_name + flen - 5, ".json") != 0)
				continue;

			char path[96];
			snprintf(path, sizeof(path), "%s/%s", LFS_PRESET_DIR, de->d_name);

			FILE *f = fopen(path, "r");
			if (!f) continue;

			fseek(f, 0, SEEK_END);
			long file_size = ftell(f);
			fseek(f, 0, SEEK_SET);

			if (file_size <= 0 || file_size > CUSTOM_PRESET_MAX_BYTES) {
				fclose(f);
				continue;
			}

			char *buf = malloc(file_size + 1);
			if (!buf) { fclose(f); continue; }
			size_t nr = fread(buf, 1, file_size, f);
			fclose(f);
			buf[nr] = '\0';

			cJSON *preset = cJSON_Parse(buf);
			free(buf);
			if (!preset) continue;

			cJSON *ecu_item = cJSON_GetObjectItemCaseSensitive(preset, "ecu");
			cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(preset, "version");
			cJSON *signals = cJSON_GetObjectItemCaseSensitive(preset, "signals");

			if (!cJSON_IsString(ecu_item) || !cJSON_IsString(ver_item) ||
				!cJSON_IsArray(signals)) {
				cJSON_Delete(preset);
				continue;
			}

			const char *ecu = ecu_item->valuestring;
			const char *ver = ver_item->valuestring;

			/* If no signals, emit a placeholder so the ECU still appears */
			if (cJSON_GetArraySize(signals) == 0) {
				cJSON *item = cJSON_CreateObject();
				cJSON_AddStringToObject(item, "ecu", ecu);
				cJSON_AddStringToObject(item, "version", ver);
				cJSON_AddStringToObject(item, "label", "");
				cJSON_AddBoolToObject(item, "_empty", 1);
				cJSON_AddItemToArray(root, item);
			}

			cJSON *sig;
			cJSON_ArrayForEach(sig, signals) {
				cJSON *item = cJSON_CreateObject();
				cJSON_AddStringToObject(item, "ecu", ecu);
				cJSON_AddStringToObject(item, "version", ver);

				cJSON *label = cJSON_GetObjectItemCaseSensitive(sig, "label");
				cJSON_AddStringToObject(item, "label",
					cJSON_IsString(label) ? label->valuestring : "");

				cJSON *can_id = cJSON_GetObjectItemCaseSensitive(sig, "can_id");
				cJSON_AddStringToObject(item, "can_id",
					cJSON_IsString(can_id) ? can_id->valuestring : "0");

				cJSON *endianess = cJSON_GetObjectItemCaseSensitive(sig, "endianess");
				cJSON_AddNumberToObject(item, "endianess",
					cJSON_IsNumber(endianess) ? endianess->valuedouble : 1);

				cJSON *bit_start = cJSON_GetObjectItemCaseSensitive(sig, "bit_start");
				cJSON_AddNumberToObject(item, "bit_start",
					cJSON_IsNumber(bit_start) ? bit_start->valuedouble : 0);

				cJSON *bit_length = cJSON_GetObjectItemCaseSensitive(sig, "bit_length");
				cJSON_AddNumberToObject(item, "bit_length",
					cJSON_IsNumber(bit_length) ? bit_length->valuedouble : 16);

				cJSON *scale = cJSON_GetObjectItemCaseSensitive(sig, "scale");
				cJSON_AddNumberToObject(item, "scale",
					_round_float(cJSON_IsNumber(scale) ? (float)scale->valuedouble : 1.0f));

				cJSON *offset = cJSON_GetObjectItemCaseSensitive(sig, "offset");
				cJSON_AddNumberToObject(item, "offset",
					_round_float(cJSON_IsNumber(offset) ? (float)offset->valuedouble : 0.0f));

				cJSON *decimals = cJSON_GetObjectItemCaseSensitive(sig, "decimals");
				cJSON_AddNumberToObject(item, "decimals",
					cJSON_IsNumber(decimals) ? decimals->valuedouble : 0);

				cJSON *is_signed = cJSON_GetObjectItemCaseSensitive(sig, "is_signed");
				cJSON_AddBoolToObject(item, "is_signed",
					cJSON_IsBool(is_signed) ? cJSON_IsTrue(is_signed) : false);

				cJSON_AddItemToArray(root, item);
			}

			cJSON_Delete(preset);
		}
		closedir(d);
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Alloc failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

static const httpd_uri_t custom_presets_list_uri = {
	.uri = "/api/presets/custom",
	.method = HTTP_GET,
	.handler = custom_presets_list_handler,
	.user_ctx = NULL};

/* POST /api/presets/custom/save — save a custom preset JSON file */
static esp_err_t custom_preset_save_handler(httpd_req_t *req) {
	int total_len = req->content_len;
	if (total_len <= 0 || total_len > CUSTOM_PRESET_MAX_BYTES) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid preset size");
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
			if (r == HTTPD_SOCK_ERR_TIMEOUT)
				httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request timeout");
			else
				httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
									"Failed to receive body");
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

	cJSON *ecu_item = cJSON_GetObjectItemCaseSensitive(root, "ecu");
	cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(root, "version");
	cJSON *signals = cJSON_GetObjectItemCaseSensitive(root, "signals");
	if (!cJSON_IsString(ecu_item) || !cJSON_IsString(ver_item) ||
		!cJSON_IsArray(signals)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Missing ecu, version, or signals");
		return ESP_FAIL;
	}

	char filename[80];
	_sanitize_preset_filename(ecu_item->valuestring, ver_item->valuestring,
							  filename, sizeof(filename));

	_ensure_preset_dir();

	char path[128];
	snprintf(path, sizeof(path), "%s/%s", LFS_PRESET_DIR, filename);

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Alloc failed");
		return ESP_FAIL;
	}

	FILE *f = fopen(path, "w");
	if (!f) {
		free(json_str);
		ESP_LOGE(TAG, "Failed to open preset file for writing: %s", path);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to write preset file");
		return ESP_FAIL;
	}
	fputs(json_str, f);
	fclose(f);
	free(json_str);

	ESP_LOGI(TAG, "Saved custom preset: %s", filename);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t custom_preset_save_uri = {
	.uri = "/api/presets/custom/save",
	.method = HTTP_POST,
	.handler = custom_preset_save_handler,
	.user_ctx = NULL};

/* POST /api/presets/custom/delete?ecu=<name>&version=<ver> — delete a custom preset */
static esp_err_t custom_preset_delete_handler(httpd_req_t *req) {
	char query[128] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
		return ESP_FAIL;
	}

	char ecu[64] = {0};
	char version[32] = {0};
	if (httpd_query_key_value(query, "ecu", ecu, sizeof(ecu)) != ESP_OK ||
		ecu[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'ecu' parameter");
		return ESP_FAIL;
	}
	if (httpd_query_key_value(query, "version", version, sizeof(version)) != ESP_OK ||
		version[0] == '\0') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'version' parameter");
		return ESP_FAIL;
	}

	char filename[80];
	_sanitize_preset_filename(ecu, version, filename, sizeof(filename));

	char path[128];
	snprintf(path, sizeof(path), "%s/%s", LFS_PRESET_DIR, filename);

	if (remove(path) != 0) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Preset not found");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Deleted custom preset: %s", filename);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t custom_preset_delete_uri = {
	.uri = "/api/presets/custom/delete",
	.method = HTTP_POST,
	.handler = custom_preset_delete_handler,
	.user_ctx = NULL};

static const httpd_uri_t layout_list_uri = {.uri = "/api/layout/list",
											.method = HTTP_GET,
											.handler = layout_list_handler,
											.user_ctx = NULL};

static esp_err_t layout_set_handler(httpd_req_t *req) {
	char buf[128];
	if (req->content_len >= sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Request body too large");
		return ESP_FAIL;
	}
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

	/* Reject path traversal in layout names */
	if (!_name_is_safe(layout_name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Invalid layout name");
		return ESP_FAIL;
	}

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

// HTTP handler for deleting a layout JSON file
static esp_err_t layout_delete_handler(httpd_req_t *req) {
	char buf[128];
	if (req->content_len >= sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Request body too large");
		return ESP_FAIL;
	}
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

	/* Reject path traversal in layout names */
	if (!_name_is_safe(layout_name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Invalid layout name");
		return ESP_FAIL;
	}

	/* Protect the default layout from deletion */
	if (strcmp(layout_name, "default") == 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Cannot delete default layout");
		return ESP_FAIL;
	}

	esp_err_t err = layout_manager_delete(layout_name);
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Layout not found");
		return ESP_FAIL;
	}

	/* If the deleted layout was the active one, switch back to default */
	char active[LAYOUT_MAX_NAME];
	layout_manager_get_active(active, sizeof(active));
	if (strcmp(active, layout_name) == 0) {
		layout_manager_set_active("default");
		lv_async_call(_deferred_screen_reload, NULL);
	}

	ESP_LOGI(TAG, "Deleted layout '%s'", layout_name);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t layout_delete_uri = {.uri = "/api/layout/delete",
											  .method = HTTP_POST,
											  .handler = layout_delete_handler,
											  .user_ctx = NULL};

/* POST /api/layout/rename — rename a layout file and update NVS if active
 * Body: { "old_name": "Foo", "new_name": "Bar" } */
static esp_err_t layout_rename_handler(httpd_req_t *req) {
	char buf[192];
	if (req->content_len >= sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
		return ESP_FAIL;
	}
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

	cJSON *old_item = cJSON_GetObjectItemCaseSensitive(root, "old_name");
	cJSON *new_item = cJSON_GetObjectItemCaseSensitive(root, "new_name");
	if (!cJSON_IsString(old_item) || !cJSON_IsString(new_item)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Missing old_name/new_name");
		return ESP_FAIL;
	}

	char old_name[LAYOUT_MAX_NAME], new_name[LAYOUT_MAX_NAME];
	strncpy(old_name, old_item->valuestring, sizeof(old_name) - 1);
	old_name[sizeof(old_name) - 1] = '\0';
	strncpy(new_name, new_item->valuestring, sizeof(new_name) - 1);
	new_name[sizeof(new_name) - 1] = '\0';
	cJSON_Delete(root);

	if (!_name_is_safe(old_name) || !_name_is_safe(new_name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}
	if (strcmp(old_name, "default") == 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Cannot rename default layout");
		return ESP_FAIL;
	}
	if (strcmp(old_name, new_name) == 0) {
		httpd_resp_set_type(req, "application/json");
		return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
	}

	/* Build paths */
	char old_path[80], new_path[80];
	snprintf(old_path, sizeof(old_path), "/lfs/layouts/%s.json", old_name);
	snprintf(new_path, sizeof(new_path), "/lfs/layouts/%s.json", new_name);

	/* Check destination doesn't already exist */
	struct stat st;
	if (stat(new_path, &st) == 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"A layout with that name already exists");
		return ESP_FAIL;
	}

	/* Read existing file, update internal "name" field, write to new path */
	FILE *f = fopen(old_path, "r");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Layout not found");
		return ESP_FAIL;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0 || fsize > LAYOUT_MAX_FILE_BYTES) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad file");
		return ESP_FAIL;
	}
	char *json_buf = malloc(fsize + 1);
	if (!json_buf) {
		fclose(f);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	fread(json_buf, 1, fsize, f);
	fclose(f);
	json_buf[fsize] = '\0';

	/* Update the "name" field inside the JSON */
	cJSON *layout = cJSON_Parse(json_buf);
	free(json_buf);
	if (layout) {
		cJSON *n = cJSON_GetObjectItemCaseSensitive(layout, "name");
		if (n) cJSON_SetValuestring(n, new_name);
		else cJSON_AddStringToObject(layout, "name", new_name);

		char *out = cJSON_PrintUnformatted(layout);
		cJSON_Delete(layout);
		if (out) {
			FILE *fw = fopen(new_path, "w");
			if (fw) {
				size_t len = strlen(out);
				size_t written = fwrite(out, 1, len, fw);
				int close_err = fclose(fw);
				if (written == len && close_err == 0) {
					remove(old_path);
				} else {
					ESP_LOGE(TAG, "Failed to write renamed layout (wrote %u/%u, close=%d)",
							 (unsigned)written, (unsigned)len, close_err);
					remove(new_path);
					free(out);
					httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
					return ESP_FAIL;
				}
			}
			free(out);
		}
	} else {
		/* Can't parse JSON — just rename the file */
		rename(old_path, new_path);
	}

	/* Update NVS if this was the active layout */
	char active[LAYOUT_MAX_NAME];
	layout_manager_get_active(active, sizeof(active));
	if (strcmp(active, old_name) == 0) {
		layout_manager_set_active(new_name);
	}

	/* Also remove backup file if it exists */
	char bak_path[96];
	snprintf(bak_path, sizeof(bak_path), "%s.bak", old_path);
	remove(bak_path);

	ESP_LOGI(TAG, "Renamed layout '%s' -> '%s'", old_name, new_name);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t layout_rename_uri = {.uri = "/api/layout/rename",
											  .method = HTTP_POST,
											  .handler = layout_rename_handler,
											  .user_ctx = NULL};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Splash API endpoints
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* GET /api/splash/list — list splash layouts + active splash name */
static esp_err_t splash_list_handler(httpd_req_t *req) {
	char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
	int count = layout_manager_list_splash(names, LAYOUT_MAX_COUNT);
	if (count < 0) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to list splashes");
		return ESP_FAIL;
	}

	char active[LAYOUT_MAX_NAME];
	layout_manager_get_active_splash(active, sizeof(active));

	bool fade_enabled = true;
	config_store_load_splash_fade(&fade_enabled);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "active", active);
	cJSON_AddBoolToObject(root, "fade_enabled", fade_enabled);
	cJSON *arr = cJSON_AddArrayToObject(root, "splashes");
	for (int i = 0; i < count; i++)
		cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));

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

static const httpd_uri_t splash_list_uri = {
	.uri = "/api/splash/list", .method = HTTP_GET,
	.handler = splash_list_handler, .user_ctx = NULL
};

/* POST /api/splash/set — set active splash by name */
static esp_err_t splash_set_handler(httpd_req_t *req) {
	char buf[128];
	if (req->content_len >= (int)sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
		return ESP_FAIL;
	}
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
	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	if (!cJSON_IsString(name_item)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name'");
		return ESP_FAIL;
	}

	char name[LAYOUT_MAX_NAME];
	strncpy(name, name_item->valuestring, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	cJSON_Delete(root);

	if (!_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	layout_manager_set_active_splash(name);
	splash_screen_set_active_name(name);

	/* If in splash edit mode, reload to the newly selected splash */
	if (splash_screen_is_edit_mode())
		lv_async_call(_deferred_splash_reload, NULL);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t splash_set_uri = {
	.uri = "/api/splash/set", .method = HTTP_POST,
	.handler = splash_set_handler, .user_ctx = NULL
};

/* POST /api/splash/delete — delete a splash layout file */
static esp_err_t splash_delete_handler(httpd_req_t *req) {
	char buf[128];
	if (req->content_len >= (int)sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
		return ESP_FAIL;
	}
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
	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	if (!cJSON_IsString(name_item)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name'");
		return ESP_FAIL;
	}

	char name[LAYOUT_MAX_NAME];
	strncpy(name, name_item->valuestring, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	cJSON_Delete(root);

	if (!_name_is_safe(name)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	/* Build full layout name e.g. "_splash_Racing" */
	char full_name[LAYOUT_MAX_NAME];
	snprintf(full_name, sizeof(full_name), "_splash_%s", name);
	esp_err_t err = layout_manager_delete(full_name);
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Splash not found");
		return ESP_FAIL;
	}

	/* If deleted splash was active, reset to Default */
	char active[LAYOUT_MAX_NAME];
	layout_manager_get_active_splash(active, sizeof(active));
	if (strcmp(active, name) == 0) {
		layout_manager_set_active_splash("Default");
		splash_screen_set_active_name("Default");
		if (splash_screen_is_edit_mode())
			lv_async_call(_deferred_splash_reload, NULL);
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t splash_delete_uri = {
	.uri = "/api/splash/delete", .method = HTTP_POST,
	.handler = splash_delete_handler, .user_ctx = NULL
};

/* POST /api/splash/fade — set splash fade enabled/disabled */
static esp_err_t splash_fade_handler(httpd_req_t *req) {
	char buf[64];
	if (req->content_len >= (int)sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
		return ESP_FAIL;
	}
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

	cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
	if (!cJSON_IsBool(enabled)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'enabled' bool");
		return ESP_FAIL;
	}

	bool val = cJSON_IsTrue(enabled);
	cJSON_Delete(root);

	config_store_save_splash_fade(val);
	ESP_LOGI(TAG, "Splash fade set to %s", val ? "enabled" : "disabled");

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t splash_fade_uri = {
	.uri = "/api/splash/fade", .method = HTTP_POST,
	.handler = splash_fade_handler, .user_ctx = NULL
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Image API endpoints
 * ═══════════════════════════════════════════════════════════════════════════
 */

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

	if (!_name_is_safe(name)) {
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

	if (!_name_is_safe(name)) {
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

	if (!_name_is_safe(name)) {
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  Font management endpoints
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define LFS_FONT_DIR  "/lfs/fonts"
#define FONT_MAX_FILE_SIZE (512 * 1024)

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

	if (!_name_is_safe(name)) {
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

	if (!_name_is_safe(name)) {
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

	if (!_name_is_safe(name)) {
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  Fuel sender calibration endpoints
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "widgets/signal_internal.h"

static esp_err_t _fuel_status_handler(httpd_req_t *req) {
	fuel_cal_config_t fc;
	signal_internal_get_fuel_cal(&fc);
	float voltage = signal_internal_get_fuel_voltage();

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "voltage", voltage);
	cJSON *cal = cJSON_AddObjectToObject(root, "cal");
	cJSON_AddNumberToObject(cal, "empty_v", fc.empty_v);
	cJSON_AddNumberToObject(cal, "full_v", fc.full_v);
	cJSON_AddNumberToObject(cal, "full_value", fc.full_value);
	cJSON_AddBoolToObject(cal, "enabled", fc.enabled);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

static esp_err_t _fuel_set_empty_handler(httpd_req_t *req) {
	float v = signal_internal_get_fuel_voltage();
	fuel_cal_config_t fc;
	signal_internal_get_fuel_cal(&fc);
	signal_internal_set_fuel_cal(v, fc.full_v, fc.full_value, fc.enabled);

	char resp[64];
	snprintf(resp, sizeof(resp), "{\"voltage\":%.4f}", v);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, resp);
	return ESP_OK;
}

static esp_err_t _fuel_set_full_handler(httpd_req_t *req) {
	float v = signal_internal_get_fuel_voltage();
	fuel_cal_config_t fc;
	signal_internal_get_fuel_cal(&fc);
	signal_internal_set_fuel_cal(fc.empty_v, v, fc.full_value, fc.enabled);

	char resp[64];
	snprintf(resp, sizeof(resp), "{\"voltage\":%.4f}", v);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, resp);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Signal values endpoint — returns current value for all registered signals
 * ═══════════════════════════════════════════════════════════════════════════
 */

static esp_err_t _signal_values_handler(httpd_req_t *req) {
	if (!example_lvgl_lock(500)) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
		return ESP_FAIL;
	}

	uint16_t count = signal_get_count();
	cJSON *root = cJSON_CreateObject();
	cJSON *arr  = cJSON_AddArrayToObject(root, "signals");

	for (uint16_t i = 0; i < count; i++) {
		signal_t *sig = signal_get_by_index(i);
		if (!sig || sig->name[0] == '\0') continue;
		cJSON *obj = cJSON_CreateObject();
		cJSON_AddStringToObject(obj, "name", sig->name);
		cJSON_AddNumberToObject(obj, "value", sig->current_value);
		cJSON_AddBoolToObject(obj, "stale", sig->is_stale);
		cJSON_AddNumberToObject(obj, "can_id", sig->can_id);
		cJSON_AddItemToArray(arr, obj);
	}

	example_lvgl_unlock();

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Signal simulator endpoints
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void _deferred_sim_toggle(void *arg) {
	bool enable = (bool)(uintptr_t)arg;
	if (enable) signal_sim_start();
	else signal_sim_stop();
}

static esp_err_t _signal_simulate_post_handler(httpd_req_t *req) {
	char buf[64];
	int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (ret <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
		return ESP_FAIL;
	}
	buf[ret] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
	bool en = cJSON_IsTrue(enabled);
	cJSON_Delete(root);

	lv_async_call(_deferred_sim_toggle, (void *)(uintptr_t)en);

	httpd_resp_set_type(req, "application/json");
	const char *resp = en ? "{\"status\":\"ok\",\"enabled\":true}" : "{\"status\":\"ok\",\"enabled\":false}";
	httpd_resp_sendstr(req, resp);
	return ESP_OK;
}

static esp_err_t _signal_simulate_get_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "application/json");
	const char *resp = signal_sim_is_active()
		? "{\"enabled\":true}"
		: "{\"enabled\":false}";
	httpd_resp_sendstr(req, resp);
	return ESP_OK;
}

/* POST /api/signal/inject — inject test value into one or more signals
 * Body: { "signal": "RPM", "value": 3500 }
 * Or batch: { "values": [{ "signal": "RPM", "value": 3500 }, ...] } */

typedef struct {
	uint8_t count;
	struct { char name[32]; float value; } entries[16];
} signal_inject_batch_t;

static void _deferred_inject(void *arg) {
	signal_inject_batch_t *batch = (signal_inject_batch_t *)arg;
	for (uint8_t i = 0; i < batch->count; i++)
		signal_inject_test_value(batch->entries[i].name, batch->entries[i].value);
	free(batch);
}

static esp_err_t _signal_inject_handler(httpd_req_t *req) {
	char buf[512];
	int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (ret <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
		return ESP_FAIL;
	}
	buf[ret] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	signal_inject_batch_t *batch = calloc(1, sizeof(signal_inject_batch_t));
	if (!batch) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}

	/* Single: { signal, value } */
	cJSON *sig = cJSON_GetObjectItemCaseSensitive(root, "signal");
	cJSON *val = cJSON_GetObjectItemCaseSensitive(root, "value");
	if (cJSON_IsString(sig) && cJSON_IsNumber(val)) {
		strncpy(batch->entries[0].name, sig->valuestring, 31);
		batch->entries[0].value = (float)val->valuedouble;
		batch->count = 1;
	}

	/* Batch: { values: [...] } */
	cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
	if (cJSON_IsArray(values)) {
		cJSON *item;
		cJSON_ArrayForEach(item, values) {
			if (batch->count >= 16) break;
			cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "signal");
			cJSON *v = cJSON_GetObjectItemCaseSensitive(item, "value");
			if (cJSON_IsString(s) && cJSON_IsNumber(v)) {
				strncpy(batch->entries[batch->count].name, s->valuestring, 31);
				batch->entries[batch->count].value = (float)v->valuedouble;
				batch->count++;
			}
		}
	}

	cJSON_Delete(root);

	if (batch->count > 0)
		lv_async_call(_deferred_inject, batch);
	else
		free(batch);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

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
	.handler = storage_info_handler, .user_ctx = NULL
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  SD Card API endpoints
 * ═══════════════════════════════════════════════════════════════════════════
 */

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
	.handler = sd_status_handler, .user_ctx = NULL
};

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
	.handler = sd_files_handler, .user_ctx = NULL
};

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
	.handler = sd_copy_handler, .user_ctx = NULL
};

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
	.handler = sd_delete_handler, .user_ctx = NULL
};

/* ── Screen switch endpoint ──────────────────────────────────────────── */

static esp_err_t screen_switch_handler(httpd_req_t *req) {
	char query_buf[64];
	esp_err_t qerr =
		httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf));
	if (qerr != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
		return ESP_FAIL;
	}

	const char *screen_val = strstr(query_buf, "screen=");
	if (!screen_val) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing screen=");
		return ESP_FAIL;
	}
	screen_val += 7;

	if (strncmp(screen_val, "splash", 6) == 0) {
		if (!splash_screen_is_edit_mode())
			lv_async_call(_deferred_screen_switch_splash, NULL);
	} else if (strncmp(screen_val, "dashboard", 9) == 0) {
		if (splash_screen_is_edit_mode())
			lv_async_call(_deferred_screen_switch_dashboard, NULL);
	} else {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Invalid screen (use splash or dashboard)");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t screen_switch_uri = {
	.uri = "/api/screen/switch", .method = HTTP_POST,
	.handler = screen_switch_handler, .user_ctx = NULL
};

/* ── Dimmer config API ──────────────────────────────────────────────── */

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

	if (!example_lvgl_lock(100)) {
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

	example_lvgl_unlock();
	cJSON_Delete(root);

	save_dimmer_config_to_nvs();
	lv_async_call(_deferred_dimmer_subscribe, NULL);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
	return ESP_OK;
}

static const httpd_uri_t dimmer_config_get_uri = {
	.uri = "/api/dimmer/config", .method = HTTP_GET,
	.handler = _dimmer_config_get_handler, .user_ctx = NULL
};
static const httpd_uri_t dimmer_config_post_uri = {
	.uri = "/api/dimmer/config", .method = HTTP_POST,
	.handler = _dimmer_config_post_handler, .user_ctx = NULL
};

/* ── Data Logger API ──────────────────────────────────────────────────── */

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
	if (!_filename_is_safe(name)) {
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

	if (!_filename_is_safe(name)) {
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

/* ── Device Info API ───────────────────────────────────────────────────── */

static esp_err_t _device_info_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	cJSON *root = cJSON_CreateObject();

	char serial[MAX_SERIAL_LENGTH];
	get_device_serial(serial);
	cJSON_AddStringToObject(root, "serial", serial);

	const esp_app_desc_t *desc = esp_app_get_description();
	cJSON_AddStringToObject(root, "version", desc->version);
	cJSON_AddNumberToObject(root, "schema", LAYOUT_SCHEMA_VERSION);
	cJSON_AddStringToObject(root, "project", desc->project_name);

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

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

static const httpd_uri_t device_info_uri = {
	.uri = "/api/device/info", .method = HTTP_GET,
	.handler = _device_info_handler, .user_ctx = NULL
};

/* ── Brightness API ───────────────────────────────────────────────────── */

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
	.handler = _brightness_get_handler, .user_ctx = NULL
};
static const httpd_uri_t brightness_post_uri = {
	.uri = "/api/brightness", .method = HTTP_POST,
	.handler = _brightness_post_handler, .user_ctx = NULL
};

/* ── CAN Config API ───────────────────────────────────────────────────── */

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
	.handler = _can_config_get_handler, .user_ctx = NULL
};
static const httpd_uri_t can_config_post_uri = {
	.uri = "/api/can/config", .method = HTTP_POST,
	.handler = _can_config_post_handler, .user_ctx = NULL
};

/* ── System Health API ────────────────────────────────────────────────── */

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
	.handler = _system_health_handler, .user_ctx = NULL
};

/* ── System Reboot API ────────────────────────────────────────────────── */

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
	.handler = _system_reboot_handler, .user_ctx = NULL
};

/* ── WiFi Config API ──────────────────────────────────────────────────── */

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
	.handler = _wifi_config_get_handler, .user_ctx = NULL
};
static const httpd_uri_t wifi_config_post_uri = {
	.uri = "/api/wifi/config", .method = HTTP_POST,
	.handler = _wifi_config_post_handler, .user_ctx = NULL
};

/* ═════════════════════════════════════════════════════════════════════════════
 *  CORS Preflight Handler — responds to OPTIONS requests from cross-origin
 *  desktop apps (Tauri) so that POST requests with Content-Type: application/json
 *  pass the browser's preflight check.
 * ═════════════════════════════════════════════════════════════════════════════ */
static esp_err_t cors_preflight_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
	httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
	httpd_resp_set_status(req, "204 No Content");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/* Helper macro to log on URI registration failure */
#define REGISTER_URI(svr, uri_ptr) do { \
	if (httpd_register_uri_handler(svr, uri_ptr) != ESP_OK) \
		ESP_LOGW(TAG, "Failed to register URI: %s", (uri_ptr)->uri); \
} while(0)

esp_err_t web_server_start(void) {
	if (server != NULL) {
		ESP_LOGW(TAG, "Web server already running");
		return ESP_OK;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = WEB_SERVER_PORT;
	config.stack_size = 5120;
	config.max_uri_handlers = 64;
	config.max_resp_headers = 8;
	config.lru_purge_enable = true;
	config.recv_wait_timeout = 30; /* 30s for image uploads */
	config.send_wait_timeout = 30;
	config.uri_match_fn = httpd_uri_match_wildcard;

	ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

	esp_err_t ret = httpd_start(&server, &config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
		return ret;
	}

	/* Register CORS preflight handler for all API OPTIONS requests */
	static const httpd_uri_t cors_options_uri = {
		.uri = "/api/*",
		.method = HTTP_OPTIONS,
		.handler = cors_preflight_handler,
		.user_ctx = NULL,
	};
	REGISTER_URI(server, &cors_options_uri);

	// Register URI handlers
	REGISTER_URI(server, &index_uri);
	REGISTER_URI(server, &screenshot_uri);
	REGISTER_URI(server, &layout_version_uri);
	REGISTER_URI(server, &layout_current_uri);
	REGISTER_URI(server, &layout_raw_uri);
	REGISTER_URI(server, &layout_save_uri);
	REGISTER_URI(server, &layout_preview_uri);
	REGISTER_URI(server, &layout_list_uri);
	REGISTER_URI(server, &presets_list_uri);
	REGISTER_URI(server, &custom_presets_list_uri);
	REGISTER_URI(server, &custom_preset_save_uri);
	REGISTER_URI(server, &custom_preset_delete_uri);
	REGISTER_URI(server, &layout_set_uri);
	REGISTER_URI(server, &layout_delete_uri);
	REGISTER_URI(server, &layout_rename_uri);
	REGISTER_URI(server, &splash_list_uri);
	REGISTER_URI(server, &splash_set_uri);
	REGISTER_URI(server, &splash_delete_uri);
	REGISTER_URI(server, &splash_fade_uri);
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
	REGISTER_URI(server, &screen_switch_uri);
	REGISTER_URI(server, &device_info_uri);
	REGISTER_URI(server, &brightness_get_uri);
	REGISTER_URI(server, &brightness_post_uri);
	REGISTER_URI(server, &can_config_get_uri);
	REGISTER_URI(server, &can_config_post_uri);
	REGISTER_URI(server, &system_health_uri);
	REGISTER_URI(server, &system_reboot_uri);
	REGISTER_URI(server, &wifi_config_get_uri);
	REGISTER_URI(server, &wifi_config_post_uri);
	REGISTER_URI(server, &dimmer_config_get_uri);
	REGISTER_URI(server, &dimmer_config_post_uri);
	REGISTER_URI(server, &log_start_uri);
	REGISTER_URI(server, &log_stop_uri);
	REGISTER_URI(server, &log_status_uri);
	REGISTER_URI(server, &log_list_uri);
	REGISTER_URI(server, &log_download_uri);
	REGISTER_URI(server, &log_delete_uri);
	REGISTER_URI(server, &log_config_get_uri);
	REGISTER_URI(server, &log_config_post_uri);
	static const httpd_uri_t signal_values_uri = {
		.uri = "/api/signals/values",
		.method = HTTP_GET,
		.handler = _signal_values_handler
	};
	REGISTER_URI(server, &signal_values_uri);

	static const httpd_uri_t signal_simulate_post_uri = {
		.uri = "/api/signal/simulate",
		.method = HTTP_POST,
		.handler = _signal_simulate_post_handler
	};
	REGISTER_URI(server, &signal_simulate_post_uri);

	static const httpd_uri_t signal_simulate_get_uri = {
		.uri = "/api/signal/simulate",
		.method = HTTP_GET,
		.handler = _signal_simulate_get_handler
	};
	REGISTER_URI(server, &signal_simulate_get_uri);

	static const httpd_uri_t signal_inject_uri = {
		.uri = "/api/signal/inject",
		.method = HTTP_POST,
		.handler = _signal_inject_handler
	};
	REGISTER_URI(server, &signal_inject_uri);

	static const httpd_uri_t fuel_status_uri = {
		.uri = "/api/fuel/status",
		.method = HTTP_GET,
		.handler = _fuel_status_handler
	};
	REGISTER_URI(server, &fuel_status_uri);

	static const httpd_uri_t fuel_set_empty_uri = {
		.uri = "/api/fuel/set-empty",
		.method = HTTP_POST,
		.handler = _fuel_set_empty_handler
	};
	REGISTER_URI(server, &fuel_set_empty_uri);

	static const httpd_uri_t fuel_set_full_uri = {
		.uri = "/api/fuel/set-full",
		.method = HTTP_POST,
		.handler = _fuel_set_full_handler
	};
	REGISTER_URI(server, &fuel_set_full_uri);

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