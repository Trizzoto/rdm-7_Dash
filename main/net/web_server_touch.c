/* web_server_touch.c — remote touch, indicator/warning test, screen switch
 *
 * Endpoints:
 *   POST /api/touch             inject pointer event (x, y, state) or toggle enable
 *   GET  /api/touch             return current enabled state
 *   POST /api/indicator/test    force wire-mode indicator on/off by slot
 *   POST /api/warning/test      force alert (warning) widget on/off by slot
 *   POST /api/screen/switch     switch between dashboard and splash screen */
#include "web_server_internal.h"
#include "cJSON.h"
#include "system/remote_touch.h"
#include "widgets/widget_indicator.h"
#include "widgets/widget_warning.h"
#include "ui/screens/splash_screen.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_server_touch";

/* ── /api/touch ─────────────────────────────────────────────────────────── */

static esp_err_t api_touch_post_handler(httpd_req_t *req) {
	char body[257];
	int total = req->content_len;
	if (total <= 0 || total >= (int)sizeof(body)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
		return ESP_FAIL;
	}
	int got = 0;
	while (got < total) {
		int r = httpd_req_recv(req, body + got, total - got);
		if (r <= 0) {
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
			return ESP_FAIL;
		}
		got += r;
	}
	body[got] = '\0';

	cJSON *root = cJSON_Parse(body);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
		return ESP_FAIL;
	}

	cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
	if (cJSON_IsBool(en)) {
		remote_touch_set_enabled(cJSON_IsTrue(en));
	}

	cJSON *x_js = cJSON_GetObjectItemCaseSensitive(root, "x");
	cJSON *y_js = cJSON_GetObjectItemCaseSensitive(root, "y");
	cJSON *s_js = cJSON_GetObjectItemCaseSensitive(root, "state");
	if (cJSON_IsNumber(x_js) && cJSON_IsNumber(y_js) && cJSON_IsString(s_js)) {
		const char *s = s_js->valuestring;
		bool pressed = (strcmp(s, "down") == 0) || (strcmp(s, "move") == 0);
		if (strcmp(s, "down") == 0 || strcmp(s, "up") == 0) {
			ESP_LOGI(TAG, "touch %s @ (%d, %d) enabled=%d",
			         s, x_js->valueint, y_js->valueint,
			         remote_touch_is_enabled());
		} else {
			ESP_LOGD(TAG, "touch move @ (%d, %d)",
			         x_js->valueint, y_js->valueint);
		}
		remote_touch_set((int16_t)x_js->valueint, (int16_t)y_js->valueint, pressed);
	}

	cJSON_Delete(root);

	char out[48];
	snprintf(out, sizeof(out), "{\"enabled\":%s}",
	         remote_touch_is_enabled() ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_touch_get_handler(httpd_req_t *req) {
	char out[48];
	snprintf(out, sizeof(out), "{\"enabled\":%s}",
	         remote_touch_is_enabled() ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* ── /api/indicator/test ─────────────────────────────────────────────────── */

static bool s_ind_test_left  = false;
static bool s_ind_test_right = false;

static void _ind_test_apply_cb(void *param) {
	(void)param;
	indicator_apply_analog_state(s_ind_test_left, s_ind_test_right);
}

static esp_err_t api_indicator_test_post_handler(httpd_req_t *req) {
	char body[128];
	int total = req->content_len;
	if (total <= 0 || total >= (int)sizeof(body)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
		return ESP_FAIL;
	}
	int got = 0;
	while (got < total) {
		int r = httpd_req_recv(req, body + got, total - got);
		if (r <= 0) {
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
			return ESP_FAIL;
		}
		got += r;
	}
	body[got] = '\0';

	cJSON *root = cJSON_Parse(body);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
		return ESP_FAIL;
	}

	cJSON *slot_js   = cJSON_GetObjectItemCaseSensitive(root, "slot");
	cJSON *active_js = cJSON_GetObjectItemCaseSensitive(root, "active");
	int  slot   = cJSON_IsNumber(slot_js)   ? slot_js->valueint      : -1;
	bool active = cJSON_IsBool(active_js)   ? cJSON_IsTrue(active_js) : false;

	if (slot == 0) s_ind_test_left  = active;
	else if (slot == 1) s_ind_test_right = active;
	else {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot must be 0 or 1");
		return ESP_FAIL;
	}
	cJSON_Delete(root);

	ESP_LOGI(TAG, "indicator test slot=%d active=%d (L=%d R=%d)",
	         slot, active, s_ind_test_left, s_ind_test_right);

	lv_async_call(_ind_test_apply_cb, NULL);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ── /api/warning/test ───────────────────────────────────────────────────── */

typedef struct { uint8_t slot; bool active; } warn_test_req_t;

static void _warn_test_apply_cb(void *param) {
	warn_test_req_t *req = (warn_test_req_t *)param;
	if (req) {
		widget_warning_apply_test_state(req->slot, req->active);
		free(req);
	}
}

static esp_err_t api_warning_test_post_handler(httpd_req_t *req) {
	char body[96];
	int total = req->content_len;
	if (total <= 0 || total >= (int)sizeof(body)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
		return ESP_FAIL;
	}
	int got = 0;
	while (got < total) {
		int r = httpd_req_recv(req, body + got, total - got);
		if (r <= 0) {
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
			return ESP_FAIL;
		}
		got += r;
	}
	body[got] = '\0';

	cJSON *root = cJSON_Parse(body);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
		return ESP_FAIL;
	}
	cJSON *slot_js   = cJSON_GetObjectItemCaseSensitive(root, "slot");
	cJSON *active_js = cJSON_GetObjectItemCaseSensitive(root, "active");
	int slot   = cJSON_IsNumber(slot_js) ? slot_js->valueint : -1;
	bool active = cJSON_IsBool(active_js) ? cJSON_IsTrue(active_js) : false;
	cJSON_Delete(root);

	if (slot < 0 || slot > 7) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot must be 0..7");
		return ESP_FAIL;
	}

	warn_test_req_t *payload = calloc(1, sizeof(*payload));
	if (!payload) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
		return ESP_FAIL;
	}
	payload->slot = (uint8_t)slot;
	payload->active = active;
	lv_async_call(_warn_test_apply_cb, payload);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ── /api/screen/switch ──────────────────────────────────────────────────── */

static void _deferred_screen_switch_splash(void *arg) {
	(void)arg;
	splash_screen_enter_edit_mode();
}

static void _deferred_screen_switch_dashboard(void *arg) {
	(void)arg;
	splash_screen_exit_edit_mode();
}

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

/* ── URI descriptors ─────────────────────────────────────────────────────── */

static const httpd_uri_t api_touch_post_uri = {
    .uri = "/api/touch", .method = HTTP_POST,
    .handler = api_touch_post_handler, .user_ctx = NULL};
static const httpd_uri_t api_touch_get_uri = {
    .uri = "/api/touch", .method = HTTP_GET,
    .handler = api_touch_get_handler, .user_ctx = NULL};
static const httpd_uri_t api_indicator_test_post_uri = {
    .uri = "/api/indicator/test", .method = HTTP_POST,
    .handler = api_indicator_test_post_handler, .user_ctx = NULL};
static const httpd_uri_t api_warning_test_post_uri = {
    .uri = "/api/warning/test", .method = HTTP_POST,
    .handler = api_warning_test_post_handler, .user_ctx = NULL};
static const httpd_uri_t screen_switch_uri = {
	.uri = "/api/screen/switch", .method = HTTP_POST,
	.handler = screen_switch_handler, .user_ctx = NULL
};

void web_server_touch_register(httpd_handle_t server) {
	REGISTER_URI(server, &api_touch_post_uri);
	REGISTER_URI(server, &api_touch_get_uri);
	REGISTER_URI(server, &api_indicator_test_post_uri);
	REGISTER_URI(server, &api_warning_test_post_uri);
	REGISTER_URI(server, &screen_switch_uri);
}
