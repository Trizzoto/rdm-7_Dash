/* web_server_can.c — the dash as a WiFi↔CAN gateway for RDM Studio.
 *
 * Studio has no CAN dongle. Everything it does on a bus — the analyzer, and
 * now the keypad setup wizard — it does through a dash that is already wired
 * into the car. The analyzer only ever READS (/api/can/monitor), so it lived
 * happily in web_server_test.c alongside the other read-mostly endpoints.
 * Provisioning a third-party device does not: it has to put real frames on a
 * real vehicle bus, and it has to move the dash's own bitrate around to find
 * a device that is on the wrong one.
 *
 * That does not belong in web_server_test.c, whose contract in its own header
 * is "read-mostly or inject-only; none change persisted config". So the write
 * side lives here, together, with the guards that make it safe to leave
 * switched on in a car rather than a bench-only debug hook:
 *
 *   POST /api/can/send            transmit one raw frame onto the physical bus
 *   POST /api/can/monitor/reset   wipe the per-ID tracker
 *   POST /api/can/promiscuous     accept every ID regardless of the layout
 *
 * Why each guard exists is written at its own site. The short version: an ID
 * range the dash or its own device bus owns is refused outright, and the rate
 * is capped, because a runaway caller here is not a failed test — it is
 * traffic on the bus a car is running on.
 *
 * The bitrate lives on /api/can/config in web_server_system.c (it was already
 * there, and the on-device settings screen shares it).
 */
#include "web_server_internal.h"
#include "cJSON.h"
#include "can/can_manager.h"
#include "can/can_id_tracker.h"
#include "can/rdm_bus.h"
#include "can/rdm_bus_proto.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "web_can";

static esp_err_t _send_json(httpd_req_t *req, cJSON *root) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

/* esp_http_server's httpd_err_code_t has no 429/503, so send them by hand. */
static esp_err_t _send_status(httpd_req_t *req, const char *status,
                              const char *err, const char *detail) {
	httpd_resp_set_status(req, status);
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", false);
	cJSON_AddStringToObject(root, "error", err);
	if (detail) cJSON_AddStringToObject(root, "detail", detail);
	char *s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!s) { httpd_resp_sendstr(req, "{\"ok\":false}"); return ESP_FAIL; }
	httpd_resp_sendstr(req, s);
	cJSON_free(s);
	return ESP_FAIL;
}

static int _parse_hex_byte(const char *p) {
	int hi = -1, lo = -1;
	if (p[0] >= '0' && p[0] <= '9') hi = p[0] - '0';
	else if ((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'f') hi = (p[0] | 0x20) - 'a' + 10;
	if (p[1] >= '0' && p[1] <= '9') lo = p[1] - '0';
	else if ((p[1] | 0x20) >= 'a' && (p[1] | 0x20) <= 'f') lo = (p[1] | 0x20) - 'a' + 10;
	if (hi < 0 || lo < 0) return -1;
	return (hi << 4) | lo;
}

/* ── What this endpoint refuses to transmit ──────────────────────────────
 *
 * Not a security boundary — anyone who can reach this HTTP server can
 * already reflash the dash. It is a footgun guard: the caller is a wizard
 * scripting an unfamiliar third-party device, and the two ways that goes
 * badly wrong are both addressable here.
 *
 *  1. The RDM device bus. The dash and the GPS puck talk to each other on
 *     the same physical wire, on a 16-ID block plus the discovery ID. A
 *     stray frame there is not noise — it is a well-formed message to our
 *     own protocol handler, which will act on it (adopt a track outline,
 *     answer an asset transfer). Refused so a mistyped ID in a keypad
 *     script cannot drive the dash's own state machine.
 *
 *  2. OBD2 request IDs. If the dash is polling a car's ECU, injecting more
 *     requests on 0x7DF / 0x7E0-0x7E7 interleaves with its own transaction
 *     and corrupts both. Nothing being provisioned over this endpoint has
 *     any business there; a keypad certainly does not.
 *
 * Returns a human-readable reason, or NULL when the frame is allowed. The
 * reason is sent to the caller verbatim — Studio shows it to the user, so
 * it says what and why, not "forbidden".
 */
static const char *_tx_refused_reason(uint32_t id, bool extd) {
	if (extd) {
		if (id > 0x1FFFFFFFu) return "Extended IDs are 29-bit — 0x1FFFFFFF is the highest.";
		/* The RDM device bus and OBD2 request ranges below are 11-bit
		 * standard IDs. A 29-bit frame cannot collide with them. */
		return NULL;
	}
	if (id > 0x7FFu)
		return "Standard IDs are 11-bit — set \"extd\": true for a 29-bit ID.";

	if (id == RDM_BUS_DISCOVERY_ID)
		return "That is the RDM device-bus discovery ID — the dash and the GPS "
		       "puck find each other on it.";

	uint16_t base = rdm_bus_get_base();
	if (id >= base && id < (uint32_t)base + 16u)
		return "That is inside the RDM device-bus block the dash and the GPS "
		       "puck talk on. Move the device off it, or move the RDM block.";

	if (id == 0x7DFu || (id >= 0x7E0u && id <= 0x7E7u))
		return "That is an OBD2 request ID. If the dash is polling the car, a "
		       "frame there lands in the middle of its own transaction.";

	return NULL;
}

/* ── Rate cap ────────────────────────────────────────────────────────────
 *
 * One frame per HTTP round-trip is already slow, so this never bites a
 * caller behaving itself: the keypad wizard sends roughly three frames a
 * second because it waits for each reply before sending the next (the
 * Blink keypad's SDO server silently drops overlapping requests, so it has
 * no choice). What it stops is a loop that has lost its wait — a retry
 * storm, a stuck poll — turning into sustained traffic on a bus a car is
 * running on. A rolling one-second window, not a token bucket, because the
 * useful question is "how much did this put on the wire in the last
 * second" and the answer should not depend on when the window started. */
#define TX_MAX_PER_SECOND  24

static int64_t s_tx_window_us = 0;
static int     s_tx_in_window = 0;

static bool _tx_rate_ok(void) {
	int64_t now = esp_timer_get_time();
	if (now - s_tx_window_us >= 1000000) { s_tx_window_us = now; s_tx_in_window = 0; }
	if (s_tx_in_window >= TX_MAX_PER_SECOND) return false;
	s_tx_in_window++;
	return true;
}

/* ── POST /api/can/send ──────────────────────────────────────────────────
 *
 * Transmit ONE frame onto the physical bus. The counterpart of
 * /api/can/inject in web_server_test.c, which never touches the wire — that
 * one feeds the local decode path so a layout can be tested without a car;
 * this one is how Studio talks to a device that is not the dash.
 *
 * Body: { "id": 1557, "extd": false,
 *         "data": "2F10200002000000"  (hex, spaces ignored)  OR  [byte,...],
 *         "dlc": 8 }    (optional; defaults to the data length)
 *
 * Response: { "ok": true, "id": …, "dlc": … } or ok:false with "error" and,
 * for a refused ID, a "detail" that says which range it hit and why.
 */
static esp_err_t _can_send_handler(httpd_req_t *req) {
	char buf[256];
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (!cJSON_IsNumber(jid)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing numeric 'id'");
		return ESP_FAIL;
	}
	uint32_t id = (uint32_t)jid->valuedouble;
	bool extd = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "extd"));

	uint8_t data[8] = {0};
	int dlen = 0;
	cJSON *jdata = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (cJSON_IsString(jdata) && jdata->valuestring) {
		const char *s = jdata->valuestring;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
		while (dlen < 8 && s[0]) {
			if (s[0] == ' ') { s++; continue; }
			if (!s[1]) break;
			int b = _parse_hex_byte(s);
			if (b < 0) break;
			data[dlen++] = (uint8_t)b;
			s += 2;
		}
	} else if (cJSON_IsArray(jdata)) {
		cJSON *el;
		cJSON_ArrayForEach(el, jdata) {
			if (dlen >= 8) break;
			if (cJSON_IsNumber(el)) data[dlen++] = (uint8_t)el->valueint;
		}
	}

	cJSON *jdlc = cJSON_GetObjectItemCaseSensitive(root, "dlc");
	int dlc = cJSON_IsNumber(jdlc) ? jdlc->valueint : dlen;
	if (dlc < 0) dlc = 0;
	if (dlc > 8) dlc = 8;
	cJSON_Delete(root);

	const char *refused = _tx_refused_reason(id, extd);
	if (refused)
		return _send_status(req, "403 Forbidden", "id refused", refused);

	/* The bus scan owns the peripheral while it runs — a transmit would fail
	 * with INVALID_STATE and look like a dead device. Say which it is. */
	if (can_is_suspended())
		return _send_status(req, "503 Service Unavailable", "CAN busy",
		                    "The dash's bus scan owns the CAN peripheral right "
		                    "now. Wait for it to finish and try again.");

	if (!_tx_rate_ok())
		return _send_status(req, "429 Too Many Requests", "rate limited",
		                    "More than 24 frames in one second. This endpoint "
		                    "puts real traffic on the car's bus, so it is "
		                    "capped — wait for each reply before sending the "
		                    "next frame.");

	esp_err_t err = can_transmit_frame_ext(id, extd, data, (uint8_t)dlc);
	if (err != ESP_OK)
		ESP_LOGW(TAG, "can/send 0x%lX failed: %s",
		         (unsigned long)id, esp_err_to_name(err));

	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
	if (err != ESP_OK) cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
	cJSON_AddNumberToObject(resp, "id", id);
	cJSON_AddNumberToObject(resp, "dlc", dlc);
	return _send_json(req, resp);
}

/* ── POST /api/can/monitor/reset ─────────────────────────────────────────
 *
 * Wipe the per-ID tracker /api/can/monitor reads.
 *
 * The wizard needs this, not just the analyzer's Clear button (which only
 * empties the browser's own copy). When it moves the dash from 125k to 250k
 * looking for a device, every ID captured at the old bitrate is still in the
 * table, with real data and a real count — indistinguishable from something
 * answering at the NEW rate until enough time passes for the age to give it
 * away. Clearing between probes turns "is this entry stale?" into "there is
 * no entry", which is a question with an answer.
 */
static esp_err_t _can_monitor_reset_handler(httpd_req_t *req) {
	can_id_tracker_reset();
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "ok", true);
	cJSON_AddNumberToObject(resp, "ids", can_id_tracker_count());
	return _send_json(req, resp);
}

/* ── POST /api/can/promiscuous ───────────────────────────────────────────
 *
 * Body: { "enable": true }
 *
 * The hardware acceptance filter is built from whatever signals the loaded
 * layout binds, so which IDs reach the tracker depends on the dashboard the
 * user happens to have open. That is right for normal running and useless
 * for finding a device: a keypad's SDO reply on 0x595 or its J1939 broadcast
 * on 0x18EFFF21 is passed or dropped by accident. The first-run wizard's ECU
 * probe already had this problem and solved it with ACCEPT_ALL; this is the
 * same switch, reachable over HTTP so Studio can do the same.
 *
 * Always paired — whoever turns it on turns it off. The response reports the
 * state actually in effect rather than the one requested, because a bus scan
 * defers the change.
 */
static esp_err_t _can_promiscuous_handler(httpd_req_t *req) {
	char buf[64];
	if (web_server_recv_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}
	cJSON *jen = cJSON_GetObjectItemCaseSensitive(root, "enable");
	if (!cJSON_IsBool(jen)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing boolean 'enable'");
		return ESP_FAIL;
	}
	bool enable = cJSON_IsTrue(jen);
	cJSON_Delete(root);

	if (can_is_suspended())
		return _send_status(req, "503 Service Unavailable", "CAN busy",
		                    "The dash's bus scan owns the CAN peripheral right "
		                    "now. Wait for it to finish and try again.");

	can_set_promiscuous_mode(enable);

	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "ok", true);
	cJSON_AddBoolToObject(resp, "promiscuous", can_is_promiscuous());
	return _send_json(req, resp);
}

static const httpd_uri_t can_send_uri = {
	.uri = "/api/can/send", .method = HTTP_POST,
	.handler = _can_send_handler, .user_ctx = NULL};
static const httpd_uri_t can_monitor_reset_uri = {
	.uri = "/api/can/monitor/reset", .method = HTTP_POST,
	.handler = _can_monitor_reset_handler, .user_ctx = NULL};
static const httpd_uri_t can_promiscuous_uri = {
	.uri = "/api/can/promiscuous", .method = HTTP_POST,
	.handler = _can_promiscuous_handler, .user_ctx = NULL};

void web_server_can_register(httpd_handle_t server) {
	REGISTER_URI(server, &can_send_uri);
	REGISTER_URI(server, &can_monitor_reset_uri);
	REGISTER_URI(server, &can_promiscuous_uri);
}
