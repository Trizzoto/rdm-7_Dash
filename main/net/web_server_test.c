/* web_server_test.c — agent/automation test + introspection endpoints.
 *
 * These exist so an external agent can drive and verify the device over WiFi
 * without a CAN bus or a human watching the screen. They are read-mostly or
 * inject-only; none change persisted config.
 *
 * Endpoints:
 *   POST /api/can/inject   feed a synthetic frame through the REAL decode path
 *   GET  /api/widgets      live widget-tree dump (type/pos/field values/rules)
 *   GET  /api/selftest     one-shot subsystem health report
 */
#include "web_server_internal.h"
#include "cJSON.h"
#include "can/can_manager.h"
#include "can/can_bus_test.h"
#include "widgets/signal.h"
#include "widgets/signal_sim.h"
#include "widgets/widget_registry.h"
#include "widgets/widget_types.h"
#include "widgets/widget_fields.h"
#include "widgets/widget_rules.h"
#include "data/channel_manager.h"
#include "widgets/font_manager.h"
#include "system/render_perf.h"
#include "system/crash_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "web_test";

/* esp_http_server's httpd_err_code_t has no 503, so send one by hand. */
static esp_err_t _send_503(httpd_req_t *req, const char *msg) {
	httpd_resp_set_status(req, "503 Service Unavailable");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");
	char body[96];
	snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
	httpd_resp_sendstr(req, body);
	return ESP_FAIL;
}

/* Thin convenience wrapper: every caller in this file wants CORS set, so
 * bundle that with the shared send helper rather than repeat it at each of
 * the 5 call sites. Takes ownership of `root`. */
static esp_err_t _send_json(httpd_req_t *req, cJSON *root) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

/* ── POST /api/can/inject ────────────────────────────────────────────────
 *
 * Body: { "id": 864, "extd": false,
 *         "data": "0A1B2C3D4E5F0011"  (hex)  OR  [10,27,44,...],
 *         "dlc": 8,            (optional; defaults to the data length)
 *         "count": 1 }         (optional; repeat the same frame N times)
 *
 * The frame is queued into the real RX path, so bit extraction / scaling /
 * endianness / signal dispatch / channel-zone evaluation all run exactly as
 * for a bus frame. Response reports how many were queued and whether the
 * simulator is swallowing them. */
static int _parse_hex_byte(const char *p) {
	int hi = -1, lo = -1;
	if (p[0] >= '0' && p[0] <= '9') hi = p[0] - '0';
	else if ((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'f') hi = (p[0] | 0x20) - 'a' + 10;
	if (p[1] >= '0' && p[1] <= '9') lo = p[1] - '0';
	else if ((p[1] | 0x20) >= 'a' && (p[1] | 0x20) <= 'f') lo = (p[1] | 0x20) - 'a' + 10;
	if (hi < 0 || lo < 0) return -1;
	return (hi << 4) | lo;
}

static esp_err_t _can_inject_handler(httpd_req_t *req) {
	char buf[512];
	int total = 0;
	while (total < (int)sizeof(buf) - 1) {
		int r = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total);
		if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
		if (r <= 0) break;
		total += r;
		if (total >= req->content_len) break;
	}
	if (total <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
		return ESP_FAIL;
	}
	buf[total] = '\0';

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

	/* Data: hex string or numeric array. */
	uint8_t data[8] = {0};
	int dlen = 0;
	cJSON *jdata = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (cJSON_IsString(jdata) && jdata->valuestring) {
		const char *s = jdata->valuestring;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
		while (dlen < 8 && s[0] && s[1]) {
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

	cJSON *jcount = cJSON_GetObjectItemCaseSensitive(root, "count");
	int count = cJSON_IsNumber(jcount) ? jcount->valueint : 1;
	if (count < 1) count = 1;
	if (count > 256) count = 256;
	cJSON_Delete(root);

	int queued = 0;
	esp_err_t last = ESP_OK;
	for (int i = 0; i < count; i++) {
		last = can_inject_rx_frame(id, extd, data, (uint8_t)dlc);
		if (last == ESP_OK) queued++;
		else break;
	}

	if (queued == 0 && last == ESP_ERR_INVALID_STATE)
		return _send_503(req, "CAN RX queue not ready");

	bool sim = signal_sim_is_active();
	if (sim)
		ESP_LOGW(TAG, "can/inject: %d frame(s) queued but sim is active — "
		              "they will be drained without dispatch", queued);

	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "ok", queued > 0);
	cJSON_AddNumberToObject(resp, "queued", queued);
	cJSON_AddNumberToObject(resp, "dlc", dlc);
	/* When true the frames are dropped (drained but not decoded) — disable the
	 * simulator first for the injection to take effect. */
	cJSON_AddBoolToObject(resp, "sim_active", sim);
	return _send_json(req, resp);
}

/* ── GET /api/widgets ─────────────────────────────────────────────────────
 *
 * Live dump of every widget the registry tracks: identity + geometry + the
 * CURRENT value of each schema field (via the widget's inspector_get) + active
 * conditional rules. Lets an agent verify a rule fired, a colour changed, or a
 * value updated without a screenshot. Built entirely under the LVGL lock since
 * inspector_get reads type_data (and returns string pointers into it). */
static esp_err_t _widgets_handler(httpd_req_t *req) {
	cJSON *root = cJSON_CreateObject();
	cJSON *arr  = cJSON_AddArrayToObject(root, "widgets");

	if (!rdm_lvgl_lock(1000)) {
		cJSON_Delete(root);
		return _send_503(req, "LVGL busy");
	}

	widget_t *list[WIDGET_REGISTRY_MAX];
	uint8_t n = 0;
	widget_registry_snapshot(list, WIDGET_REGISTRY_MAX, &n);

	for (uint8_t i = 0; i < n; i++) {
		widget_t *w = list[i];
		if (!w) continue;
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "id", w->id);
		cJSON_AddStringToObject(o, "type", widget_type_name(w->type));
		cJSON_AddNumberToObject(o, "slot", w->slot);
		cJSON_AddNumberToObject(o, "x", w->x);
		cJSON_AddNumberToObject(o, "y", w->y);
		cJSON_AddNumberToObject(o, "w", w->w);
		cJSON_AddNumberToObject(o, "h", w->h);
		/* Live hidden state — confirms overlay-bake absorbed a shape (baked
		 * shapes are HIDDEN; their pixels live in the meter face). */
		if (w->root && lv_obj_is_valid(w->root))
			cJSON_AddBoolToObject(o, "hidden",
			    lv_obj_has_flag(w->root, LV_OBJ_FLAG_HIDDEN));

		/* Field values via inspector_get, keyed by schema name. Same
		 * type→union mapping the on-device inspector uses. */
		const widget_fields_def_t *def = widget_fields_for_type(w->type);
		if (def && w->inspector_get) {
			cJSON *fields = cJSON_AddObjectToObject(o, "fields");
			for (uint16_t fi = 0; fi < def->field_count; fi++) {
				const widget_field_t *f = &def->fields[fi];
				widget_field_value_t v = {0};
				if (!w->inspector_get(w, f->name, &v)) continue;
				switch (f->type) {
				case WF_TYPE_COLOR:
					cJSON_AddNumberToObject(fields, f->name, v.color);
					break;
				case WF_TYPE_CHECKBOX:
					cJSON_AddBoolToObject(fields, f->name, v.b);
					break;
				case WF_TYPE_TEXT:
				case WF_TYPE_TEXTAREA:
				case WF_TYPE_FONT:
				case WF_TYPE_IMAGE_PICKER:
					if (v.str) cJSON_AddStringToObject(fields, f->name, v.str);
					break;
				default:  /* NUMBER/STEPPER/SLIDER/SELECT/CAN_ID */
					/* Fractional-default fields travel in .f — reading .i
					 * would reinterpret the float bits (and vice versa). */
					if (widget_field_is_fractional(f))
						cJSON_AddNumberToObject(fields, f->name, v.f);
					else
						cJSON_AddNumberToObject(fields, f->name, v.i);
					break;
				}
			}
		}

		/* Conditional rules + live active state. */
		if (w->rules && w->rule_count > 0) {
			cJSON *rules = cJSON_AddArrayToObject(o, "rules");
			cJSON_AddNumberToObject(o, "rule_mask", w->last_rule_mask);
			for (uint8_t ri = 0; ri < w->rule_count; ri++) {
				const widget_rule_t *r = &w->rules[ri];
				cJSON *rj = cJSON_CreateObject();
				cJSON_AddStringToObject(rj, "signal", r->signal_name);
				cJSON_AddNumberToObject(rj, "op", r->op);
				cJSON_AddNumberToObject(rj, "threshold", r->threshold);
				cJSON_AddBoolToObject(rj, "active", r->is_active);
				cJSON_AddItemToArray(rules, rj);
			}
		}

		cJSON_AddItemToArray(arr, o);
	}
	cJSON_AddNumberToObject(root, "count", n);

	rdm_lvgl_unlock();
	return _send_json(req, root);
}

/* ── GET /api/selftest ────────────────────────────────────────────────────
 *
 * One-shot subsystem health so a post-flash agent can replace a serial-monitor
 * session with a single GET. Registry/signal/font reads happen under the LVGL
 * lock (also doubles as the lock-latency probe). */
static esp_err_t _selftest_handler(httpd_req_t *req) {
	cJSON *root = cJSON_CreateObject();
	bool overall = true;

	/* URI registration tally (set during web_server_start). */
	cJSON *uri = cJSON_AddObjectToObject(root, "uri_registration");
	cJSON_AddNumberToObject(uri, "attempts", web_server_uri_register_attempts);
	cJSON_AddNumberToObject(uri, "failures", web_server_uri_register_failures);
	bool uri_ok = (web_server_uri_register_failures == 0);
	cJSON_AddBoolToObject(uri, "ok", uri_ok);
	overall &= uri_ok;

	/* LittleFS mount: a known dir must stat. */
	struct stat st;
	bool lfs_ok = (stat("/lfs/layouts", &st) == 0);
	cJSON *lfs = cJSON_AddObjectToObject(root, "littlefs");
	cJSON_AddBoolToObject(lfs, "ok", lfs_ok);
	overall &= lfs_ok;

	/* CAN driver state. */
	uint32_t can_state = 0, bus_err = 0, rx_err = 0, dummy = 0;
	can_get_diagnostics(&can_state, &dummy, &dummy, &dummy, &rx_err, &bus_err, &dummy);
	cJSON *can = cJSON_AddObjectToObject(root, "can");
	cJSON_AddNumberToObject(can, "state", can_state);
	cJSON_AddNumberToObject(can, "bus_errors", bus_err);
	cJSON_AddNumberToObject(can, "rx_errors", rx_err);
	cJSON_AddNumberToObject(can, "rx_frames", can_get_rx_frame_count());

	/* Heap. */
	cJSON *heap = cJSON_AddObjectToObject(root, "heap");
	cJSON_AddNumberToObject(heap, "free", esp_get_free_heap_size());
	cJSON_AddNumberToObject(heap, "min_free", esp_get_minimum_free_heap_size());
	cJSON_AddNumberToObject(heap, "psram_free",
	                        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
	/* Largest contiguous PSRAM block — the real ceiling for a single big
	 * allocation (e.g. a full-screen image buffer). Can be far below psram_free
	 * once the heap is fragmented by loaded image caches. */
	cJSON_AddNumberToObject(heap, "psram_largest_free",
	                        (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

	/* Registry / signals / fonts under the LVGL lock; the acquire time doubles
	 * as a responsiveness probe. */
	int64_t t0 = esp_timer_get_time();
	bool got = rdm_lvgl_lock(200);
	int64_t lock_us = esp_timer_get_time() - t0;
	cJSON *lvgl = cJSON_AddObjectToObject(root, "lvgl");
	cJSON_AddBoolToObject(lvgl, "lock_acquired", got);
	cJSON_AddNumberToObject(lvgl, "lock_ms", (double)lock_us / 1000.0);
	overall &= got;

	if (got) {
		size_t ch_count = channel_manager_count();
		int ch_decoded = 0;
		for (size_t i = 0; i < ch_count; i++) {
			channel_t *c = channel_manager_at(i);
			if (c && c->can_id != 0) ch_decoded++;
		}
		cJSON *ch = cJSON_AddObjectToObject(root, "channels");
		cJSON_AddNumberToObject(ch, "count", ch_count);
		cJSON_AddNumberToObject(ch, "decoded", ch_decoded);

		uint16_t sig_count = signal_get_count();
		int fresh = 0, stale = 0;
		for (uint16_t i = 0; i < sig_count; i++) {
			signal_t *s = signal_get_by_index(i);
			if (!s) continue;
			if (s->is_stale) stale++; else fresh++;
		}
		cJSON *sig = cJSON_AddObjectToObject(root, "signals");
		cJSON_AddNumberToObject(sig, "count", sig_count);
		cJSON_AddNumberToObject(sig, "fresh", fresh);
		cJSON_AddNumberToObject(sig, "stale", stale);

		cJSON *fonts = cJSON_AddObjectToObject(root, "fonts");
		cJSON_AddNumberToObject(fonts, "families", font_manager_family_count());

		rdm_lvgl_unlock();
	}

	/* OTA / rollback state — which slot is running and whether the bootloader
	 * is still waiting for us to confirm it (PENDING_VERIFY). After a healthy
	 * boot the LVGL task calls esp_ota_mark_app_valid_cancel_rollback(), so a
	 * stable device should report "valid" (or "undefined" for a USB-flashed
	 * image, which is never subject to rollback). */
	{
		cJSON *ota = cJSON_AddObjectToObject(root, "ota");
		const esp_partition_t *run = esp_ota_get_running_partition();
		esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
		const char *st_name = "unknown";
		if (run) {
			cJSON_AddStringToObject(ota, "running", run->label);
			if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
				switch (st) {
				case ESP_OTA_IMG_NEW:            st_name = "new"; break;
				case ESP_OTA_IMG_PENDING_VERIFY: st_name = "pending_verify"; break;
				case ESP_OTA_IMG_VALID:          st_name = "valid"; break;
				case ESP_OTA_IMG_INVALID:        st_name = "invalid"; break;
				case ESP_OTA_IMG_ABORTED:        st_name = "aborted"; break;
				default:                         st_name = "undefined"; break;
				}
			}
		}
		cJSON_AddStringToObject(ota, "state", st_name);
		cJSON_AddBoolToObject(ota, "rollback_pending",
		                      st == ESP_OTA_IMG_PENDING_VERIFY);
	}

	/* Previous-boot crash snapshot (from NVS). panic_count is the lifetime
	 * crash-reboot tally — polling it across an action reliably reveals a crash
	 * even when the serial console isn't being captured. */
	{
		const crash_log_record_t *cl = crash_log_get();
		cJSON *crash = cJSON_AddObjectToObject(root, "crash");
		if (cl) {
			cJSON_AddNumberToObject(crash, "panic_count", cl->panic_count);
			cJSON_AddBoolToObject(crash, "prev_was_crash", cl->was_crash);
			cJSON_AddNumberToObject(crash, "prev_reason", (double)cl->reason);
			cJSON_AddNumberToObject(crash, "prev_uptime_s", cl->last_uptime_s);
			cJSON_AddStringToObject(crash, "prev_fw", cl->last_fw);
		}
	}

	cJSON_AddBoolToObject(root, "ok", overall);
	return _send_json(req, root);
}

/* ── GET /api/perf ────────────────────────────────────────────────────────
 * Last ~1 s window of LVGL render telemetry (fps, invalidated area, render
 * and flush time) published by rdm7_lvgl_monitor_cb. The serial refr_diag
 * log is LOGD/compiled-out; this is the supported way to measure render
 * performance on-device. No LVGL lock needed (plain struct copy). */
static esp_err_t _perf_handler(httpd_req_t *req) {
	render_perf_t p;
	render_perf_get(&p);
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	cJSON_AddNumberToObject(root, "seq", p.seq);
	cJSON_AddNumberToObject(root, "fps", p.fps_x10 / 10.0);
	cJSON_AddNumberToObject(root, "frames", p.frames);
	cJSON_AddNumberToObject(root, "avg_px", p.avg_px);
	cJSON_AddNumberToObject(root, "avg_pct_screen", p.avg_pct);
	cJSON_AddNumberToObject(root, "max_pct_screen", p.max_pct);
	cJSON_AddNumberToObject(root, "avg_render_ms", p.avg_render_ms);
	cJSON_AddNumberToObject(root, "flush_per_frame", p.flush_per_frame_x10 / 10.0);
	cJSON_AddNumberToObject(root, "flush_us_per_frame", p.flush_us_per_frame);
	return _send_json(req, root);
}

/* ── GET /api/perf/history ────────────────────────────────────────────────
 * Boot-timeline ring: one entry per ~1 s render window since boot (up to
 * RENDER_PERF_HISTORY_LEN, oldest dropped). Captures the post-boot fps
 * curve — including the pre-WiFi seconds no HTTP poller can observe — and
 * per-window worst single-frame render time (max_ms) for spike hunting.
 * Streamed chunked: 180 entries would not fit a single send buffer. */
static esp_err_t _perf_history_handler(httpd_req_t *req) {
	static render_perf_hist_t hist[RENDER_PERF_HISTORY_LEN]; /* httpd task only */
	uint16_t n = render_perf_history_get(hist, RENDER_PERF_HISTORY_LEN);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_sendstr_chunk(req, "{\"entries\":[");
	char line[112];
	for (uint16_t i = 0; i < n; i++) {
		snprintf(line, sizeof(line),
		         "%s{\"t\":%u,\"fps\":%u.%u,\"ms\":%u,\"max_ms\":%u,"
		         "\"max_pct\":%u,\"px\":%u}",
		         i ? "," : "",
		         (unsigned)hist[i].up_ms,
		         (unsigned)(hist[i].fps_x10 / 10), (unsigned)(hist[i].fps_x10 % 10),
		         (unsigned)hist[i].avg_render_ms,
		         (unsigned)hist[i].max_render_ms,
		         (unsigned)hist[i].max_pct,
		         (unsigned)hist[i].avg_px);
		httpd_resp_sendstr_chunk(req, line);
	}
	httpd_resp_sendstr_chunk(req, "]}");
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;
}

/* ── GET /api/perf/bigframe ───────────────────────────────────────────────
 * Pre-join dirty-rect list of the most recent frame that invalidated >=90%
 * of the screen (RDM7 DIAG hook in lv_refr.c). Maps rect coords back to the
 * widgets whose invalidations chained into a full-screen redraw. */
extern int16_t  rdm7_bigframe_rects[][4];
extern uint16_t rdm7_bigframe_count;
extern uint32_t rdm7_bigframe_px;
extern volatile uint32_t rdm7_bigframe_seq;
extern uint32_t rdm7_fullinv_pcs[8];
extern volatile uint32_t rdm7_fullinv_seq;
extern uint32_t rdm7_invovf_pcs[8];
extern volatile uint32_t rdm7_invovf_seq;

static esp_err_t _perf_bigframe_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	char head[512];
	int hl = snprintf(head, sizeof(head),
	                  "{\"seq\":%u,\"px\":%u,\"fullinv_seq\":%u,\"fullinv_pcs\":[",
	                  (unsigned)rdm7_bigframe_seq, (unsigned)rdm7_bigframe_px,
	                  (unsigned)rdm7_fullinv_seq);
	for (int i = 0; i < 8; i++)
		hl += snprintf(head + hl, sizeof(head) - hl, "%s\"0x%08x\"",
		               i ? "," : "", (unsigned)rdm7_fullinv_pcs[i]);
	hl += snprintf(head + hl, sizeof(head) - hl,
	               "],\"invovf_seq\":%u,\"invovf_pcs\":[",
	               (unsigned)rdm7_invovf_seq);
	for (int i = 0; i < 8; i++)
		hl += snprintf(head + hl, sizeof(head) - hl, "%s\"0x%08x\"",
		               i ? "," : "", (unsigned)rdm7_invovf_pcs[i]);
	snprintf(head + hl, sizeof(head) - hl, "],\"rects\":[");
	httpd_resp_sendstr_chunk(req, head);
	char line[64];
	uint16_t n = rdm7_bigframe_count;
	for (uint16_t i = 0; i < n; i++) {
		snprintf(line, sizeof(line), "%s[%d,%d,%d,%d]", i ? "," : "",
		         (int)rdm7_bigframe_rects[i][0], (int)rdm7_bigframe_rects[i][1],
		         (int)rdm7_bigframe_rects[i][2], (int)rdm7_bigframe_rects[i][3]);
		httpd_resp_sendstr_chunk(req, line);
	}
	httpd_resp_sendstr_chunk(req, "]}");
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;
}

/* ── CAN bus scan: remote trigger + report ─────────────────────────────────
 *
 * The wizard/device-settings bitrate scan is touch-driven; these two
 * endpoints make the same scan drivable over WiFi so a failing slot can be
 * reproduced and diagnosed without the touchscreen (the serial console is
 * owned by the desktop protocol ~2 s into boot, so the scan's error logs
 * are invisible in the field). install_err carries the actual esp_err per
 * bitrate — the on-device UI only shows a generic "CAN driver busy". */

static esp_err_t _can_scan_start_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");
	bool started = can_bus_test_start();
	httpd_resp_sendstr(req, started ? "{\"started\":true}"
	                                : "{\"started\":false,\"error\":\"scan already running\"}");
	return ESP_OK;
}

static esp_err_t _can_scan_status_handler(httpd_req_t *req) {
	const can_scan_report_t *r = can_bus_test_get_report();
	static const char *state_names[] = {
		"idle", "stopping", "testing", "restoring", "complete", "cancelled", "error"
	};
	static const char *br_names[] = { "125k", "250k", "500k", "1M" };

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	cJSON_AddBoolToObject(root, "running", can_bus_test_is_running());
	cJSON_AddStringToObject(root, "state",
		r->state <= CAN_SCAN_ERROR ? state_names[r->state] : "?");
	cJSON_AddNumberToObject(root, "current_bitrate_idx", r->current_bitrate_idx);
	cJSON_AddNumberToObject(root, "recommended_bitrate", r->recommended_bitrate);

	cJSON *arr = cJSON_AddArrayToObject(root, "results");
	for (int i = 0; i < 4; i++) {
		const can_scan_bitrate_result_t *b = &r->results[i];
		cJSON *e = cJSON_CreateObject();
		if (!e) break;
		cJSON_AddStringToObject(e, "bitrate", br_names[i]);
		cJSON_AddNumberToObject(e, "frames", (double)b->frames_received);
		cJSON_AddNumberToObject(e, "unique_ids", b->unique_id_count);
		cJSON_AddBoolToObject(e, "traffic", b->traffic_detected);
		cJSON_AddBoolToObject(e, "install_failed", b->bus_errors == 0xFFFFFFFFu);
		cJSON_AddStringToObject(e, "install_err", esp_err_to_name(b->install_err));
		if (b->bus_errors != 0xFFFFFFFFu)
			cJSON_AddNumberToObject(e, "bus_errors", (double)b->bus_errors);

		/* Step trace — which exact driver call failed, per attempt. */
		esp_err_t pre_stop = 0, pre_uninst = 0, inst[3], strt[3];
		can_bus_test_get_step_dbg((uint8_t)i, &pre_stop, &pre_uninst, inst, strt);
		cJSON *dbg = cJSON_AddObjectToObject(e, "steps");
		if (dbg) {
			const esp_err_t NR = 0x7F7F7F7F;  /* memset 0x7F marker */
			cJSON_AddStringToObject(dbg, "pre_stop",
				pre_stop == NR ? "-" : esp_err_to_name(pre_stop));
			cJSON_AddStringToObject(dbg, "pre_uninstall",
				pre_uninst == NR ? "-" : esp_err_to_name(pre_uninst));
			char key[12];
			for (int a = 0; a < 3; a++) {
				snprintf(key, sizeof(key), "install_%d", a + 1);
				cJSON_AddStringToObject(dbg, key,
					inst[a] == NR ? "-" : esp_err_to_name(inst[a]));
				snprintf(key, sizeof(key), "start_%d", a + 1);
				cJSON_AddStringToObject(dbg, key,
					strt[a] == NR ? "-" : esp_err_to_name(strt[a]));
			}
		}
		cJSON_AddItemToArray(arr, e);
	}
	return _send_json(req, root);
}

static const httpd_uri_t can_scan_start_uri = {
	.uri = "/api/can/scan/start", .method = HTTP_POST,
	.handler = _can_scan_start_handler, .user_ctx = NULL};
static const httpd_uri_t can_scan_status_uri = {
	.uri = "/api/can/scan/status", .method = HTTP_GET,
	.handler = _can_scan_status_handler, .user_ctx = NULL};

static const httpd_uri_t perf_uri = {
	.uri = "/api/perf", .method = HTTP_GET,
	.handler = _perf_handler, .user_ctx = NULL};
static const httpd_uri_t perf_history_uri = {
	.uri = "/api/perf/history", .method = HTTP_GET,
	.handler = _perf_history_handler, .user_ctx = NULL};
static const httpd_uri_t perf_bigframe_uri = {
	.uri = "/api/perf/bigframe", .method = HTTP_GET,
	.handler = _perf_bigframe_handler, .user_ctx = NULL};

static const httpd_uri_t can_inject_uri = {
	.uri = "/api/can/inject", .method = HTTP_POST,
	.handler = _can_inject_handler, .user_ctx = NULL};
static const httpd_uri_t widgets_uri = {
	.uri = "/api/widgets", .method = HTTP_GET,
	.handler = _widgets_handler, .user_ctx = NULL};
static const httpd_uri_t selftest_uri = {
	.uri = "/api/selftest", .method = HTTP_GET,
	.handler = _selftest_handler, .user_ctx = NULL};

void web_server_test_register(httpd_handle_t server) {
	REGISTER_URI(server, &can_inject_uri);
	REGISTER_URI(server, &widgets_uri);
	REGISTER_URI(server, &selftest_uri);
	REGISTER_URI(server, &perf_uri);
	REGISTER_URI(server, &perf_history_uri);
	REGISTER_URI(server, &perf_bigframe_uri);
	REGISTER_URI(server, &can_scan_start_uri);
	REGISTER_URI(server, &can_scan_status_uri);
}
