/* web_server_keypad.c — where a keypad's boot lands on the dash.
 *
 * Studio designs the boot and, until now, was also the only thing that could
 * play it: it streamed the frames over this same HTTP gateway, one frame per
 * round trip, for as long as the window was open. That makes a demo, not a
 * car. These two endpoints are the handover — Studio bakes the boot to timed
 * frames, posts the tape here, and the dash plays it at every power-up with
 * no laptop anywhere near it.
 *
 *   GET  /api/keypad/lights   what is stored, and whether it can play
 *   POST /api/keypad/lights   store a tape, or act on the stored one
 *
 * Two URIs rather than four: the actions (play / stop / forget / at_boot) ride
 * in the POST body, because handler slots are a shared budget and "one more
 * verb" is not worth one more slot. The body is either the tape (it has a
 * "frames" array) or an action (it has an "action" string).
 *
 * See main/can/keypad_lights.h for what the tape is and why the tail matters.
 */
#include "web_server_internal.h"
#include "cJSON.h"
#include "can/keypad_lights.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_keypad";

/* A baked boot is a few kB — 384 frames at ~40 bytes of JSON each is the
 * worst case Studio can produce, and this is comfortably over it. */
#define KP_LIGHTS_MAX_BYTES (48 * 1024)

static esp_err_t _send_json(httpd_req_t *req, cJSON *root) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

static cJSON *_status_json(void) {
	keypad_lights_status_t st;
	keypad_lights_get_status(&st);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", true);
	cJSON_AddBoolToObject(o, "present", st.present);
	cJSON_AddBoolToObject(o, "playing", st.playing);
	cJSON_AddBoolToObject(o, "at_boot", st.enabled);
	cJSON_AddBoolToObject(o, "loop", st.loop);
	cJSON_AddNumberToObject(o, "frames", st.frames);
	cJSON_AddNumberToObject(o, "ms", st.ms);
	cJSON_AddNumberToObject(o, "node", st.node);
	cJSON_AddNumberToObject(o, "baud", st.baud);
	if (st.keypad[0]) cJSON_AddStringToObject(o, "keypad", st.keypad);
	if (st.reason[0]) cJSON_AddStringToObject(o, "reason", st.reason);
	return o;
}

/* ── GET /api/keypad/lights ─────────────────────────────────────────────── */
static esp_err_t _get_handler(httpd_req_t *req) {
	return _send_json(req, _status_json());
}

/* ── POST /api/keypad/lights ────────────────────────────────────────────── */
static esp_err_t _post_handler(httpd_req_t *req) {
	int total = req->content_len;
	if (total <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
		return ESP_FAIL;
	}
	if (total > KP_LIGHTS_MAX_BYTES) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "That boot is too big — 48 kB is the cap.");
		return ESP_FAIL;
	}
	char *buf = malloc((size_t)total + 1);
	if (!buf) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
		return ESP_FAIL;
	}
	int got = 0;
	while (got < total) {
		int r = httpd_req_recv(req, buf + got, total - got);
		if (r <= 0) {
			free(buf);
			httpd_resp_send_err(req, r == HTTPD_SOCK_ERR_TIMEOUT
			                             ? HTTPD_408_REQ_TIMEOUT
			                             : HTTPD_500_INTERNAL_SERVER_ERROR,
			                    "Failed to receive body");
			return ESP_FAIL;
		}
		got += r;
	}
	buf[got] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		free(buf);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *jact = cJSON_GetObjectItemCaseSensitive(root, "action");
	const char *act = cJSON_IsString(jact) ? jact->valuestring : NULL;

	if (act) {
		bool ok = true;
		char why[96] = {0};
		if (!strcmp(act, "play")) {
			ok = (keypad_lights_play(why, sizeof(why)) == ESP_OK);
		} else if (!strcmp(act, "stop")) {
			keypad_lights_stop();
		} else if (!strcmp(act, "forget")) {
			keypad_lights_forget();
		} else if (!strcmp(act, "at_boot")) {
			cJSON *je = cJSON_GetObjectItemCaseSensitive(root, "enabled");
			ok = (keypad_lights_set_enabled(cJSON_IsTrue(je)) == ESP_OK);
			if (!ok) strncpy(why, "No boot is stored on this dash.", sizeof(why) - 1);
		} else {
			ok = false;
			strncpy(why, "Unknown action.", sizeof(why) - 1);
		}
		cJSON_Delete(root);
		free(buf);
		cJSON *resp = _status_json();
		cJSON_DeleteItemFromObjectCaseSensitive(resp, "ok");
		cJSON_AddBoolToObject(resp, "ok", ok);
		if (!ok && why[0]) cJSON_AddStringToObject(resp, "error", why);
		return _send_json(req, resp);
	}

	/* Not an action: it is the tape. keypad_lights_store validates into
	 * memory before it writes, so a bad upload leaves the working boot
	 * exactly where it was. */
	cJSON_Delete(root);
	esp_err_t err = keypad_lights_store(buf, (size_t)got);
	free(buf);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "rejected a boot upload (%s)", esp_err_to_name(err));
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "That is not a keypad boot this dash can play.");
		return ESP_FAIL;
	}
	return _send_json(req, _status_json());
}

static const httpd_uri_t kp_lights_get_uri = {
	.uri = "/api/keypad/lights", .method = HTTP_GET,
	.handler = _get_handler, .user_ctx = NULL};
static const httpd_uri_t kp_lights_post_uri = {
	.uri = "/api/keypad/lights", .method = HTTP_POST,
	.handler = _post_handler, .user_ctx = NULL};

void web_server_keypad_register(httpd_handle_t server) {
	REGISTER_URI(server, &kp_lights_get_uri);
	REGISTER_URI(server, &kp_lights_post_uri);
}
