/* web_server_touch.c — remote touch, indicator/warning/banner test, screen switch
 *
 * Endpoints:
 *   POST /api/touch             inject pointer event (x, y, state) or toggle enable
 *   GET  /api/touch             return current enabled state
 *   POST /api/indicator/test    force wire-mode indicator on/off by slot
 *   POST /api/warning/test      force alert (warning) widget on/off by slot
 *   POST /api/banner/test       force alert-banner widget visible/hidden by id
 *   POST /api/screen/switch     switch between dashboard and splash screen */
#include "web_server_internal.h"
#include "system/rdm_lv_async.h"
#include "cJSON.h"
#include "system/remote_touch.h"
#include "widgets/widget_indicator.h"
#include "widgets/widget_warning.h"
#include "widgets/widget_banner.h"
#include "widgets/widget_registry.h"
#include "widgets/widget_types.h"
#include "widgets/widget_fields.h"
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
	/* Drive the lamps directly by slot regardless of input_source so the
	 * "Test Active" preview works for CAN-mode (and unbound) indicators too —
	 * indicator_apply_analog_state() only touches Wire-mode indicators. */
	widget_indicator_apply_test_state(0, s_ind_test_left);
	widget_indicator_apply_test_state(1, s_ind_test_right);
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

	rdm_async_call(_ind_test_apply_cb, NULL);

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
	rdm_async_call(_warn_test_apply_cb, payload);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ── /api/banner/test ────────────────────────────────────────────────────── */

/* Banner has no slot concept (unlike warning) — multiple banners
 * coexist freely under the 32 KB layout budget — so the test override
 * is keyed by the widget's `id` string instead. The payload is bounded
 * (id ≤ 16 chars per widget_t), so the body is well under 96 bytes. */
typedef struct {
	char id[24];
	bool active;
} banner_test_req_t;

static void _banner_test_apply_cb(void *param) {
	banner_test_req_t *req = (banner_test_req_t *)param;
	if (req) {
		widget_banner_apply_test_state(req->id, req->active);
		free(req);
	}
}

static esp_err_t api_banner_test_post_handler(httpd_req_t *req) {
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
	cJSON *id_js     = cJSON_GetObjectItemCaseSensitive(root, "id");
	cJSON *active_js = cJSON_GetObjectItemCaseSensitive(root, "active");
	if (!cJSON_IsString(id_js) || !id_js->valuestring) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id must be a non-empty string");
		return ESP_FAIL;
	}
	bool active = cJSON_IsBool(active_js) ? cJSON_IsTrue(active_js) : false;

	banner_test_req_t *payload = calloc(1, sizeof(*payload));
	if (!payload) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
		return ESP_FAIL;
	}
	strncpy(payload->id, id_js->valuestring, sizeof(payload->id) - 1);
	payload->id[sizeof(payload->id) - 1] = '\0';
	payload->active = active;
	cJSON_Delete(root);

	rdm_async_call(_banner_test_apply_cb, payload);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ── /api/widget/transform + /api/widget/set ──────────────────────────────
 *
 * Editor fast-paths: mutate ONE live widget in place instead of pushing the
 * whole layout to /api/layout/preview (which tears down and rebuilds every
 * widget, reloading large background images — the "fuzz on every nudge").
 *
 *   POST /api/widget/transform  { id, x, y, w, h }   move/resize by id
 *   POST /api/widget/set        { id, field, value } set one schema field
 *
 * Both run synchronously under the LVGL lock (like GET /api/widgets) — the
 * work is cheap (set_pos / resize / inspector_set) and we need a synchronous
 * result so the editor knows whether the field was applied or it must fall
 * back to a full preview. find_by_id + the registry read also need the lock
 * (a concurrent layout rebuild swaps the registry out from under us). */

static esp_err_t _read_small_body(httpd_req_t *req, char *buf, size_t cap) {
	int total = req->content_len;
	if (total <= 0 || total >= (int)cap) return ESP_FAIL;
	int got = 0;
	while (got < total) {
		int r = httpd_req_recv(req, buf + got, total - got);
		if (r <= 0) return ESP_FAIL;
		got += r;
	}
	buf[got] = '\0';
	return ESP_OK;
}

/* POST /api/widget/transform — { id, x, y, w, h } (device coords; any of
 * x/y/w/h may be omitted to leave that dimension unchanged). */
static esp_err_t api_widget_transform_post_handler(httpd_req_t *req) {
	char body[160];
	if (_read_small_body(req, body, sizeof(body)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
		return ESP_FAIL;
	}
	cJSON *root = cJSON_Parse(body);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
		return ESP_FAIL;
	}
	cJSON *id_js = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (!cJSON_IsString(id_js) || !id_js->valuestring) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id must be a string");
		return ESP_FAIL;
	}
	cJSON *x_js = cJSON_GetObjectItemCaseSensitive(root, "x");
	cJSON *y_js = cJSON_GetObjectItemCaseSensitive(root, "y");
	cJSON *w_js = cJSON_GetObjectItemCaseSensitive(root, "w");
	cJSON *h_js = cJSON_GetObjectItemCaseSensitive(root, "h");

	bool found = false;
	/* 2 s acquire: a live drag fires these rapidly and the LVGL render task
	 * can hold the lock through a heavy frame (image load / full rebuild).
	 * On timeout return 503 + Retry-After so the editor retries that edit
	 * rather than silently dropping it (see _doWidgetTransform). */
	if (!rdm_lvgl_lock(2000)) {
		cJSON_Delete(root);
		return web_server_send_busy(req);
	}
	widget_t *wd = widget_registry_find_by_id(id_js->valuestring);
	if (wd && wd->root && lv_obj_is_valid(wd->root)) {
		found = true;
		int nx = cJSON_IsNumber(x_js) ? x_js->valueint : wd->x;
		int ny = cJSON_IsNumber(y_js) ? y_js->valueint : wd->y;
		int nw = cJSON_IsNumber(w_js) ? w_js->valueint : wd->w;
		int nh = cJSON_IsNumber(h_js) ? h_js->valueint : wd->h;
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;
		/* Mirror the canonical on-device move/resize (see edit_mode.c):
		 * resize() owns w->w/h + internal relayout when present, else set
		 * size by hand; position is always applied via set_pos on the root. */
		wd->x = (int16_t)nx;
		wd->y = (int16_t)ny;
		if (wd->resize) {
			wd->resize(wd, (uint16_t)nw, (uint16_t)nh);
		} else {
			wd->w = (uint16_t)nw;
			wd->h = (uint16_t)nh;
			lv_obj_set_size(wd->root, nw, nh);
		}
		lv_obj_set_align(wd->root, LV_ALIGN_CENTER);
		lv_obj_set_pos(wd->root, nx, ny);
	}
	rdm_lvgl_unlock();
	cJSON_Delete(root);

	char resp[48];
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"found\":%s}",
			 found ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/widget/set — { id, field, value }. Routes one schema field
 * through the widget's inspector_set (writes type_data AND applies live).
 * "handled" is false when the widget/field isn't live-settable so the editor
 * can fall back to a full /api/layout/preview rebuild. value is mapped to the
 * inspector union by the field's schema type, exactly as the on-device
 * inspector does (colour = 0xRRGGBB; numeric/select = int32; bool; string). */
static esp_err_t api_widget_set_post_handler(httpd_req_t *req) {
	char body[512];
	if (_read_small_body(req, body, sizeof(body)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
		return ESP_FAIL;
	}
	cJSON *root = cJSON_Parse(body);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
		return ESP_FAIL;
	}
	cJSON *id_js    = cJSON_GetObjectItemCaseSensitive(root, "id");
	cJSON *field_js = cJSON_GetObjectItemCaseSensitive(root, "field");
	cJSON *value_js = cJSON_GetObjectItemCaseSensitive(root, "value");
	if (!cJSON_IsString(id_js) || !id_js->valuestring ||
		!cJSON_IsString(field_js) || !field_js->valuestring || !value_js) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"need id, field, value");
		return ESP_FAIL;
	}

	bool found = false, handled = false;
	/* 2 s acquire + 503 on timeout — same rationale as transform above. */
	if (!rdm_lvgl_lock(2000)) {
		cJSON_Delete(root);
		return web_server_send_busy(req);
	}
	widget_t *wd = widget_registry_find_by_id(id_js->valuestring);
	if (wd && wd->inspector_set && wd->root && lv_obj_is_valid(wd->root)) {
		found = true;
		const widget_fields_def_t *def = widget_fields_for_type(wd->type);
		const widget_field_t *f = def ? widget_fields_find(def, field_js->valuestring) : NULL;
		if (f) {
			widget_field_value_t v = {0};
			bool typed_ok = true;
			switch (f->type) {
			case WF_TYPE_COLOR:
				if (cJSON_IsNumber(value_js)) v.color = ((uint32_t)value_js->valueint) & 0xFFFFFF;
				else typed_ok = false;
				break;
			case WF_TYPE_CHECKBOX:
				if (cJSON_IsBool(value_js))        v.b = cJSON_IsTrue(value_js);
				else if (cJSON_IsNumber(value_js)) v.b = (value_js->valueint != 0);
				else typed_ok = false;
				break;
			case WF_TYPE_TEXT:
			case WF_TYPE_TEXTAREA:
			case WF_TYPE_FONT:
			case WF_TYPE_IMAGE_PICKER:
				if (cJSON_IsString(value_js)) v.str = value_js->valuestring;
				else typed_ok = false;
				break;
			default: /* NUMBER / STEPPER / STEPPER_AUTO / SLIDER / SELECT / CAN_ID */
				if (cJSON_IsNumber(value_js)) v.i = value_js->valueint;
				else typed_ok = false;
				break;
			}
			if (typed_ok) handled = wd->inspector_set(wd, field_js->valuestring, &v);
		}
	}
	rdm_lvgl_unlock();
	cJSON_Delete(root);

	char resp[64];
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"found\":%s,\"handled\":%s}",
			 found ? "true" : "false", handled ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
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
			rdm_async_call(_deferred_screen_switch_splash, NULL);
	} else if (strncmp(screen_val, "dashboard", 9) == 0) {
		if (splash_screen_is_edit_mode())
			rdm_async_call(_deferred_screen_switch_dashboard, NULL);
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
static const httpd_uri_t api_banner_test_post_uri = {
    .uri = "/api/banner/test", .method = HTTP_POST,
    .handler = api_banner_test_post_handler, .user_ctx = NULL};
static const httpd_uri_t api_widget_transform_post_uri = {
    .uri = "/api/widget/transform", .method = HTTP_POST,
    .handler = api_widget_transform_post_handler, .user_ctx = NULL};
static const httpd_uri_t api_widget_set_post_uri = {
    .uri = "/api/widget/set", .method = HTTP_POST,
    .handler = api_widget_set_post_handler, .user_ctx = NULL};
static const httpd_uri_t screen_switch_uri = {
	.uri = "/api/screen/switch", .method = HTTP_POST,
	.handler = screen_switch_handler, .user_ctx = NULL
};

void web_server_touch_register(httpd_handle_t server) {
	REGISTER_URI(server, &api_touch_post_uri);
	REGISTER_URI(server, &api_touch_get_uri);
	REGISTER_URI(server, &api_indicator_test_post_uri);
	REGISTER_URI(server, &api_warning_test_post_uri);
	REGISTER_URI(server, &api_banner_test_post_uri);
	REGISTER_URI(server, &api_widget_transform_post_uri);
	REGISTER_URI(server, &api_widget_set_post_uri);
	REGISTER_URI(server, &screen_switch_uri);
}
