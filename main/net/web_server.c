#include "web_server.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "layout/layout_manager.h"
#include "layout/ecu_presets.h"
#include "lvgl.h"
#include "system/rdm_settings.h"
#include "ui/dashboard.h"
#include "ui/screens/splash_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/preset_picker.h"
#include "ui/ui.h"
#include "storage/config_store.h"
#include <dirent.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include "web_server_internal.h"

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

/* Send a structured 413 Payload-Too-Large JSON error.
 *
 * Layout JSON > LAYOUT_MAX_FILE_BYTES is a real failure mode in the editor,
 * historically masked by an opaque 400. This helper returns a body the
 * editor can parse and surface inline:
 *     { "ok":false, "error":"layout_too_large", "max":32768, "actual":N }
 */
esp_err_t web_server_send_layout_too_large(httpd_req_t *req, size_t actual) {
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_status(req, "413 Payload Too Large");
	char body[128];
	snprintf(body, sizeof(body),
			 "{\"ok\":false,\"error\":\"layout_too_large\","
			 "\"max\":%u,\"actual\":%u}",
			 (unsigned)LAYOUT_MAX_FILE_BYTES, (unsigned)actual);
	httpd_resp_sendstr(req, body);
	return ESP_FAIL;
}

/* â”€â”€ Path-safety check for user-supplied names (no traversal) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

bool web_server_name_is_safe(const char *name) {
	if (!name || !name[0]) return false;
	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == '.' || *p < 0x20) return false;
	}
	return true;
}

/* Like web_server_name_is_safe but allows a single dot for file extension (e.g. ".csv") */
bool web_server_filename_is_safe(const char *name) {
	if (!name || !name[0]) return false;
	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p < 0x20) return false;
	}
	/* Reject ".." sequences */
	if (strstr(name, "..")) return false;
	return true;
}

/* â”€â”€ Debounced screen reload (runs on LVGL task via lv_async_call) â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

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

/* â”€â”€ Splash screen reload (debounced, runs on LVGL task) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

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

// URI handlers
static const httpd_uri_t index_uri = {
	.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};

// Lightweight version check â€” web editor polls this to avoid fetching the
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

	// Must hold LVGL mutex â€” widgets live on the LVGL task's core
	if (!rdm_lvgl_lock(1000)) {
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
	rdm_lvgl_unlock();

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

/* GET /api/layout/raw?name=<layout_name> â€” return raw layout JSON from file
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
	if (total_len <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid layout size");
		return ESP_FAIL;
	}
	if (total_len > LAYOUT_MAX_FILE_BYTES) {
		ESP_LOGW(TAG, "POST /api/layout/save: payload %d B exceeds %d B cap",
				 total_len, LAYOUT_MAX_FILE_BYTES);
		return web_server_send_layout_too_large(req, (size_t)total_len);
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
	if (!web_server_name_is_safe(layout_name)) {
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

	/* User edits to "default" are deliberately preserved across reboots
	 * (see CLAUDE.md). The old "cannot overwrite default" rejection broke
	 * the auto-save flow and conflicted with the documented behaviour.
	 * Factory reset remains the way to restore the compiled-in default. */

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


/* POST /api/layout/preview â€” apply layout JSON live without saving to file. */
static esp_err_t layout_preview_handler(httpd_req_t *req) {
	int total_len = req->content_len;
	if (total_len <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid layout size");
		return ESP_FAIL;
	}
	if (total_len > LAYOUT_MAX_FILE_BYTES) {
		ESP_LOGW(TAG, "POST /api/layout/preview: payload %d B exceeds %d B cap",
				 total_len, LAYOUT_MAX_FILE_BYTES);
		return web_server_send_layout_too_large(req, (size_t)total_len);
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

/* Round a float to remove single-precision artifacts (e.g. 0.100000005 â†’ 0.1) */
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

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *  ECU selection endpoints
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

/* GET /api/ecu/list - returns the array of ECU presets from ecu_presets.c */
static esp_err_t ecu_list_handler(httpd_req_t *req) {
	cJSON *root = cJSON_CreateArray();
	for (int i = 0; i < ECU_PRESETS_COUNT; i++) {
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "make",    ECU_PRESETS[i].make);
		cJSON_AddStringToObject(item, "version", ECU_PRESETS[i].version);
		cJSON_AddStringToObject(item, "display", ECU_PRESETS[i].display);
		cJSON_AddItemToArray(root, item);
	}
	char *s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc"); return ESP_FAIL; }
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t r = httpd_resp_send(req, s, strlen(s));
	free(s);
	return r;
}

/* GET /api/ecu/current - returns {"make":"...","version":"..."} or empty strings */
static esp_err_t ecu_current_handler(httpd_req_t *req) {
	char make[32] = {0}, ver[32] = {0};
	config_store_load_ecu(make, sizeof(make), ver, sizeof(ver));
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "make", make);
	cJSON_AddStringToObject(root, "version", ver);
	char *s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc"); return ESP_FAIL; }
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t r = httpd_resp_send(req, s, strlen(s));
	free(s);
	return r;
}

/* POST /api/ecu/set  body: {"make":"...","version":"..."} - empty strings clear */
static esp_err_t ecu_set_handler(httpd_req_t *req) {
	char buf[128];
	if (req->content_len >= sizeof(buf)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
		return ESP_FAIL;
	}
	int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
	buf[n] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }
	const cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "make");
	const cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "version");
	if (!cJSON_IsString(jm) || !cJSON_IsString(jv)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
		return ESP_FAIL;
	}
	const char *make = jm->valuestring;
	const char *ver  = jv->valuestring;

	if (make[0] && ver[0]) {
		const ecu_preset_t *p = ecu_preset_find(make, ver);
		if (!p) {
			cJSON_Delete(root);
			httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown ECU");
			return ESP_FAIL;
		}
		char active[LAYOUT_MAX_NAME] = {0};
		layout_manager_get_active(active, sizeof(active));
		if (active[0] == '\0') strcpy(active, "default");
		if (ecu_preset_apply_to_layout(active, p) != ESP_OK) {
			cJSON_Delete(root);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Apply failed");
			return ESP_FAIL;
		}
		config_store_save_ecu(make, ver);
		lv_async_call(_deferred_screen_reload, NULL);
	} else {
		config_store_save_ecu("", "");
	}
	cJSON_Delete(root);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", 11);
}

static const httpd_uri_t ecu_list_uri    = { .uri = "/api/ecu/list",    .method = HTTP_GET,  .handler = ecu_list_handler,    .user_ctx = NULL };
static const httpd_uri_t ecu_current_uri = { .uri = "/api/ecu/current", .method = HTTP_GET,  .handler = ecu_current_handler, .user_ctx = NULL };
static const httpd_uri_t ecu_set_uri     = { .uri = "/api/ecu/set",     .method = HTTP_POST, .handler = ecu_set_handler,     .user_ctx = NULL };

/* â”€â”€ Custom signal presets (stored as JSON in /lfs/presets/) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

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

/* GET /api/presets/custom â€” list all custom presets as flat signal array */
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

/* POST /api/presets/custom/save â€” save a custom preset JSON file */
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

/* POST /api/presets/custom/delete?ecu=<name>&version=<ver> â€” delete a custom preset */
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
	if (!web_server_name_is_safe(layout_name)) {
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
	if (!web_server_name_is_safe(layout_name)) {
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

/* POST /api/layout/rename â€” rename a layout file and update NVS if active
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

	if (!web_server_name_is_safe(old_name) || !web_server_name_is_safe(new_name)) {
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
		/* Can't parse JSON â€” just rename the file */
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

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *  Splash API endpoints
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 */

/* GET /api/splash/list â€” list splash layouts + active splash name */
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

/* POST /api/splash/set â€” set active splash by name */
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

	if (!web_server_name_is_safe(name)) {
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

/* POST /api/splash/delete â€” delete a splash layout file */
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

	if (!web_server_name_is_safe(name)) {
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

/* POST /api/splash/fade â€” set splash fade enabled/disabled */
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

/* image/font/storage/SD endpoints moved to web_server_assets.c */


/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *  CORS Preflight Handler â€” responds to OPTIONS requests from cross-origin
 *  desktop apps (Tauri) so that POST requests with Content-Type: application/json
 *  pass the browser's preflight check.
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */
static esp_err_t cors_preflight_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
	httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
	httpd_resp_set_status(req, "204 No Content");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/* URI registration tally â€” REGISTER_URI macro is in web_server_internal.h.
 * These globals are declared extern there so domain register() functions
 * that include the header can increment the same counters. */
int web_server_uri_register_attempts = 0;
int web_server_uri_register_failures = 0;

esp_err_t web_server_start(void) {
	if (server != NULL) {
		ESP_LOGW(TAG, "Web server already running");
		return ESP_OK;
	}

	web_server_uri_register_attempts = 0;
	web_server_uri_register_failures = 0;

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = WEB_SERVER_PORT;
	config.stack_size = 5120;
	/* Pin httpd to core 0. LVGL runs on core 1; if httpd also lands on
	 * core 1 and then jpeg_enc_process() runs for 10+ seconds (happens
	 * under load: CONTROL mode + stream + widget rebuild + layout save
	 * in parallel saturates PSRAM bandwidth), it preempts the LVGL task
	 * and starves IDLE1 â†’ task-watchdog trip â†’ eventual reboot. With
	 * httpd on core 0 the heavy encode work stays out of LVGL's way,
	 * and IDLE0 is less loaded than IDLE1. Observed in serial log:
	 * back-to-back WDT hits during /api/screenshot encode stalls. */
	config.core_id    = 0;
	/* 128 slots: ~86 actual REGISTER_URI calls today, leaving ~40 slots of
	 * headroom for the next round of endpoint adds before we have to think
	 * about this again. ESP-IDF silently drops handlers registered past
	 * max_uri_handlers â€” when we ran with 80, the last ~6 POST/OPTIONS
	 * handlers fell through to the wildcard CORS preflight and returned 405
	 * (e.g. `/api/signal/simulate` POST). Each slot is ~32 bytes of static
	 * RAM, so 128 costs ~1 KB. The REGISTER_URI macro tallies failures and
	 * logs at the end of web_server_start; a non-zero tally in the boot log
	 * means we hit the cap and need to bump it again. */
	config.max_uri_handlers = 128;
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

	/* Captive portal probe URIs â€” register before any wildcard handler
	 * so the specific paths aren't shadowed. */
	web_server_captive_register(server);

	// Register URI handlers
	REGISTER_URI(server, &index_uri);
	web_server_capture_register(server);
	web_server_touch_register(server);
	web_server_gear_register(server);
	REGISTER_URI(server, &layout_version_uri);
	REGISTER_URI(server, &layout_current_uri);
	REGISTER_URI(server, &layout_raw_uri);
	REGISTER_URI(server, &layout_save_uri);
	REGISTER_URI(server, &layout_preview_uri);
	REGISTER_URI(server, &layout_list_uri);
	REGISTER_URI(server, &presets_list_uri);
	REGISTER_URI(server, &ecu_list_uri);
	REGISTER_URI(server, &ecu_current_uri);
	REGISTER_URI(server, &ecu_set_uri);
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
	web_server_assets_register(server);
	web_server_system_register(server);
	web_server_ota_register(server);
	web_server_wifi_register(server);
	web_server_logger_register(server);
	web_server_signals_register(server);

	/* Final registration tally. If any registration failed (almost always
	 * because max_uri_handlers is too low), shout loudly so the developer
	 * who just added an endpoint notices in `idf.py monitor` instead of
	 * chasing a phantom 405 in DevTools later. */
	if (web_server_uri_register_failures > 0) {
		ESP_LOGE(TAG,
				 "URI registration: %d/%d FAILED â€” bump max_uri_handlers "
				 "(currently %d) in web_server_start. Failed endpoints will "
				 "return 405 via the OPTIONS wildcard.",
				 web_server_uri_register_failures, web_server_uri_register_attempts,
				 (int)config.max_uri_handlers);
	} else {
		ESP_LOGI(TAG, "URI registration: %d handlers registered (cap %d)",
				 web_server_uri_register_attempts, (int)config.max_uri_handlers);
	}

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
