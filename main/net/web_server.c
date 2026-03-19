#include "web_server.h"
#include "cJSON.h"
#include "display_capture.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "layout/layout_manager.h"
#include "lvgl.h"
#include "storage/sd_manager.h"
#include "system/rdm_settings.h"
#include "ui/dashboard.h"
#include "ui/screens/splash_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/preset_picker.h"
#include "ui/ui.h"
#include "widgets/font_manager.h"
#include "widgets/signal.h"
#include "widgets/signal_sim.h"
#include "ui/settings/device_settings.h"
#include "storage/config_store.h"
#include "esp_littlefs.h"
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
	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == '.') return false;
	}
	return true;
}

/* ── Deferred screen reload (runs on LVGL task via lv_async_call) ────────── */

static char *s_pending_preview_json = NULL;

static void _deferred_screen_reload(void *arg) {
	(void)arg;
	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
	ui_Screen3_screen_init();
	lv_scr_load(ui_Screen3);
	if (old && old != ui_Screen3 && lv_obj_is_valid(old))
		lv_obj_del(old);
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

/* ── Splash screen reload (runs on LVGL task) ────────────────────────── */

static void _deferred_splash_reload(void *arg) {
	(void)arg;
	splash_screen_enter_edit_mode();
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
	/* Check if we're in splash edit mode */
	bool is_splash = splash_screen_is_edit_mode();

	char layout_name[LAYOUT_MAX_NAME];
	if (is_splash) {
		strcpy(layout_name, "_splash");
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

	bool is_splash = (strcmp(layout_name, "_splash") == 0);

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
			/* Reload splash screen (don't change active dashboard layout) */
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
	/* If a previous preview is still pending, free it to avoid leak */
	char *old_preview = s_pending_preview_json;
	s_pending_preview_json = json_copy;
	if (old_preview) {
		free(old_preview);
	}
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  Image API endpoints
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define LFS_IMAGE_DIR "/lfs/images"
#define IMAGE_MAX_SIZE (1200 * 1024) /* 1200KB max — full 800x480 RDMIMG is ~1125KB */

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
	/* Increase stack size to handle LVGL snapshot + capture logic safely. */
	config.stack_size = 8192;
	config.max_uri_handlers = 30;
	config.max_resp_headers = 8;
	config.lru_purge_enable = true;
	config.recv_wait_timeout = 30; /* 30s for image uploads */
	config.send_wait_timeout = 30;

	ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

	esp_err_t ret = httpd_start(&server, &config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
		return ret;
	}

	// Register URI handlers
	REGISTER_URI(server, &index_uri);
	REGISTER_URI(server, &screenshot_uri);
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
	REGISTER_URI(server, &image_upload_uri);
	REGISTER_URI(server, &image_list_uri);
	REGISTER_URI(server, &image_delete_uri);
	REGISTER_URI(server, &image_data_uri);
	REGISTER_URI(server, &font_upload_uri);
	REGISTER_URI(server, &font_list_uri);
	REGISTER_URI(server, &font_delete_uri);
	REGISTER_URI(server, &storage_info_uri);
	REGISTER_URI(server, &sd_status_uri);
	REGISTER_URI(server, &sd_files_uri);
	REGISTER_URI(server, &sd_copy_uri);
	REGISTER_URI(server, &sd_delete_uri);
	REGISTER_URI(server, &screen_switch_uri);
	REGISTER_URI(server, &dimmer_config_get_uri);
	REGISTER_URI(server, &dimmer_config_post_uri);
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