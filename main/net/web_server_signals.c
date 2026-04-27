/* web_server_signals.c — signal values, simulator, inject/clear, fuel cal
 *
 * Endpoints:
 *   GET  /api/signals/values     current value + stale flag for all signals
 *   POST /api/signal/simulate    enable/disable the signal simulator
 *   GET  /api/signal/simulate    current simulator state
 *   POST /api/signal/inject      inject test value(s) into named signal(s)
 *   POST /api/signal/clear       release a test lock (single or all)
 *   GET  /api/fuel/status        current fuel voltage + cal config
 *   POST /api/fuel/set-empty     record current voltage as empty reference
 *   POST /api/fuel/set-full      record current voltage as full reference */
#include "web_server_internal.h"
#include "cJSON.h"
#include "widgets/signal.h"
#include "widgets/signal_sim.h"
#include "widgets/signal_internal.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

/* ── Fuel sender calibration ─────────────────────────────────────────────── */

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

/* ── Signal values ───────────────────────────────────────────────────────── */

static esp_err_t _signal_values_handler(httpd_req_t *req) {
	/* No LVGL lock needed — signal_get_by_index() is a simple array lookup
	 * into a static registry, and the fields we read (name, current_value,
	 * is_stale) are word-sized or char arrays that are atomic-enough for a
	 * read-only snapshot. The old 500 ms lock timeout was causing the mobile
	 * editor (which polls this every 3 s) to hit 500 "LVGL busy" errors
	 * whenever the signal simulator was running and the LVGL task held the
	 * mutex for extended periods during frame dispatch. */
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

/* ── Signal simulator ────────────────────────────────────────────────────── */

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

/* ── Signal inject ───────────────────────────────────────────────────────── */

typedef struct {
	uint8_t count;
	struct { char name[32]; float value; } entries[16];
} signal_inject_batch_t;

static void _deferred_inject(void *arg) {
	signal_inject_batch_t *batch = (signal_inject_batch_t *)arg;
	for (uint8_t i = 0; i < batch->count; i++) {
		signal_inject_test_value(batch->entries[i].name, batch->entries[i].value);
		/* Lock the signal so live CAN traffic won't overwrite this test
		 * value — the user is driving a test from the web editor and
		 * wants the injected value pinned until they click × (which
		 * hits /api/signal/clear). Safe to call on every re-inject:
		 * signal_set_test_lock short-circuits when state is unchanged. */
		signal_set_test_lock(batch->entries[i].name, true);
	}
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

/* ── Signal clear ────────────────────────────────────────────────────────── */

typedef struct {
	uint8_t  mode;               /* 0 = single, 1 = all */
	char     name[32];
} signal_clear_req_t;

static void _deferred_clear(void *arg) {
	signal_clear_req_t *r = (signal_clear_req_t *)arg;
	if (r->mode == 1) signal_clear_all_test_locks();
	else              signal_set_test_lock(r->name, false);
	free(r);
}

static esp_err_t _signal_clear_handler(httpd_req_t *req) {
	char buf[128];
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

	signal_clear_req_t *r = calloc(1, sizeof(signal_clear_req_t));
	if (!r) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}

	cJSON *all = cJSON_GetObjectItemCaseSensitive(root, "all");
	cJSON *sg  = cJSON_GetObjectItemCaseSensitive(root, "signal");
	if (cJSON_IsTrue(all)) {
		r->mode = 1;
	} else if (cJSON_IsString(sg) && sg->valuestring) {
		r->mode = 0;
		strncpy(r->name, sg->valuestring, sizeof(r->name) - 1);
	} else {
		cJSON_Delete(root);
		free(r);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "Expected {signal:<name>} or {all:true}");
		return ESP_FAIL;
	}
	cJSON_Delete(root);

	lv_async_call(_deferred_clear, r);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

/* ── URI descriptors ─────────────────────────────────────────────────────── */

static const httpd_uri_t signal_values_uri = {
    .uri = "/api/signals/values", .method = HTTP_GET,
    .handler = _signal_values_handler, .user_ctx = NULL};
static const httpd_uri_t signal_simulate_post_uri = {
    .uri = "/api/signal/simulate", .method = HTTP_POST,
    .handler = _signal_simulate_post_handler, .user_ctx = NULL};
static const httpd_uri_t signal_simulate_get_uri = {
    .uri = "/api/signal/simulate", .method = HTTP_GET,
    .handler = _signal_simulate_get_handler, .user_ctx = NULL};
static const httpd_uri_t signal_inject_uri = {
    .uri = "/api/signal/inject", .method = HTTP_POST,
    .handler = _signal_inject_handler, .user_ctx = NULL};
static const httpd_uri_t signal_clear_uri = {
    .uri = "/api/signal/clear", .method = HTTP_POST,
    .handler = _signal_clear_handler, .user_ctx = NULL};
static const httpd_uri_t fuel_status_uri = {
    .uri = "/api/fuel/status", .method = HTTP_GET,
    .handler = _fuel_status_handler, .user_ctx = NULL};
static const httpd_uri_t fuel_set_empty_uri = {
    .uri = "/api/fuel/set-empty", .method = HTTP_POST,
    .handler = _fuel_set_empty_handler, .user_ctx = NULL};
static const httpd_uri_t fuel_set_full_uri = {
    .uri = "/api/fuel/set-full", .method = HTTP_POST,
    .handler = _fuel_set_full_handler, .user_ctx = NULL};

void web_server_signals_register(httpd_handle_t server) {
	REGISTER_URI(server, &signal_values_uri);
	REGISTER_URI(server, &signal_simulate_post_uri);
	REGISTER_URI(server, &signal_simulate_get_uri);
	REGISTER_URI(server, &signal_inject_uri);
	REGISTER_URI(server, &signal_clear_uri);
	REGISTER_URI(server, &fuel_status_uri);
	REGISTER_URI(server, &fuel_set_empty_uri);
	REGISTER_URI(server, &fuel_set_full_uri);
}
