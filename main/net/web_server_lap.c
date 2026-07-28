/*
 * web_server_lap.c — HTTP surface for GPS lap timing (ADR-0008).
 *
 * The endpoints the web editor's Lap Timing modal (and, via ADR-0007 sync,
 * RDM Studio) is built on:
 *
 *   GET  /api/lap/status         puck presence, live fix, track summary, timing
 *   GET  /api/lap/track          the configured track, full detail
 *   POST /api/lap/track          replace the track ({"clear":true} to remove)
 *   POST /api/lap/capture        set a line from the CAR'S CURRENT POSITION
 *   POST /api/lap/session/reset  clear times + reference, keep the track
 *   POST /api/lap/gps            {"base_id": N} — move to another puck block
 *
 * The capture endpoint is the whole configuration story: park on (or better,
 * drive across) the start/finish line and tap the button — position AND
 * heading come from the live GPS stream, so there is no coordinate entry, no
 * map, no PC. Heading is course-over-ground and is garbage at a standstill,
 * so captures below a walking pace accept the position but flag the heading
 * for the UI to warn about.
 *
 * Threading: every handler that touches the lap engine takes the LVGL lock —
 * the engine runs on the LVGL task (see lap_engine.h).
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "lap/lap_engine.h"
#include "ui/lvgl_helpers.h"
#include "web_server_internal.h"

static const char *TAG = "web_lap";

/* Below this, course-over-ground is noise and a captured heading would make
 * the line directional in a random direction. */
#define CAPTURE_MIN_SPEED_KPH 5.0f

#define LAP_BODY_MAX 2048

/* ── JSON helpers ──────────────────────────────────────────────────────── */
static cJSON *line_to_json(const lap_line_t *l) {
	cJSON *j = cJSON_CreateObject();
	if (!j)
		return NULL;
	cJSON_AddNumberToObject(j, "lat", l->lat_deg);
	cJSON_AddNumberToObject(j, "lon", l->lon_deg);
	cJSON_AddNumberToObject(j, "heading", l->heading_deg);
	cJSON_AddNumberToObject(j, "half_width_m", l->half_width_m);
	return j;
}

static bool line_from_json(const cJSON *j, lap_line_t *out) {
	if (!cJSON_IsObject(j))
		return false;
	const cJSON *lat = cJSON_GetObjectItemCaseSensitive(j, "lat");
	const cJSON *lon = cJSON_GetObjectItemCaseSensitive(j, "lon");
	if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon))
		return false;
	if (lat->valuedouble < -90.0 || lat->valuedouble > 90.0 || lon->valuedouble < -180.0 ||
	    lon->valuedouble > 180.0)
		return false;
	memset(out, 0, sizeof(*out));
	out->lat_deg = lat->valuedouble;
	out->lon_deg = lon->valuedouble;
	const cJSON *h = cJSON_GetObjectItemCaseSensitive(j, "heading");
	out->heading_deg = cJSON_IsNumber(h) ? (float)h->valuedouble : 0.0f;
	const cJSON *w = cJSON_GetObjectItemCaseSensitive(j, "half_width_m");
	out->half_width_m = cJSON_IsNumber(w) ? (float)w->valuedouble : 15.0f;
	if (out->half_width_m < 2.0f)
		out->half_width_m = 2.0f;
	if (out->half_width_m > 100.0f)
		out->half_width_m = 100.0f;
	return true;
}

static cJSON *track_to_json(const lap_track_t *t) {
	cJSON *j = cJSON_CreateObject();
	if (!j)
		return NULL;
	cJSON_AddStringToObject(j, "name", t->name);
	cJSON_AddNumberToObject(j, "min_lap_time_s", t->min_lap_time_s);
	cJSON_AddItemToObject(j, "start_finish", line_to_json(&t->start_finish));
	cJSON *arr = cJSON_AddArrayToObject(j, "sectors");
	for (uint8_t i = 0; i < t->sector_count && arr; i++)
		cJSON_AddItemToArray(arr, line_to_json(&t->sectors[i]));
	/* Emitted only for a point-to-point course, so a circuit's payload is
	 * byte-identical to what it was before this existed. */
	if (t->point_to_point) {
		cJSON_AddBoolToObject(j, "point_to_point", true);
		cJSON_AddItemToObject(j, "finish", line_to_json(&t->finish));
	}
	return j;
}

/* ── GET /api/lap/status ───────────────────────────────────────────────── */
static esp_err_t lap_status_get_handler(httpd_req_t *req) {
	if (!rdm_lvgl_lock(500))
		return web_server_send_busy(req);

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		rdm_lvgl_unlock();
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
		return ESP_FAIL;
	}

	cJSON_AddBoolToObject(root, "gps_present", lap_engine_gps_present());
	cJSON_AddNumberToObject(root, "gps_base_id", lap_engine_get_base_id());

	double lat, lon;
	float heading, speed;
	if (lap_engine_last_position(&lat, &lon, &heading, &speed)) {
		cJSON *fix = cJSON_AddObjectToObject(root, "fix");
		if (fix) {
			cJSON_AddNumberToObject(fix, "lat", lat);
			cJSON_AddNumberToObject(fix, "lon", lon);
			cJSON_AddNumberToObject(fix, "heading", heading);
			cJSON_AddNumberToObject(fix, "speed_kph", speed);
		}
	}

	const lap_engine_t *e = lap_engine_state();
	cJSON_AddBoolToObject(root, "has_track", e->have_track);
	if (e->have_track) {
		cJSON_AddStringToObject(root, "track_name", e->track.name);
		/* Only for a point-to-point course, so a circuit's status payload is
		 * unchanged. Clients need it because the two read differently: on a
		 * sprint, `armed` false with a time banked means the run FINISHED,
		 * whereas on a circuit it means nothing has started yet. */
		if (e->track.point_to_point)
			cJSON_AddBoolToObject(root, "point_to_point", true);
	}

	cJSON *timing = cJSON_AddObjectToObject(root, "timing");
	if (timing) {
		cJSON_AddBoolToObject(timing, "armed", e->armed);
		cJSON_AddNumberToObject(timing, "lap_number", e->lap_number);
		cJSON_AddNumberToObject(timing, "lap_time_current", e->lap_time_current);
		cJSON_AddNumberToObject(timing, "lap_time_last", e->lap_time_last);
		cJSON_AddNumberToObject(timing, "lap_time_best", e->lap_time_best);
		cJSON_AddNumberToObject(timing, "lap_time_theoretical", e->lap_time_theoretical);
		cJSON_AddNumberToObject(timing, "sector_number", e->sector_number);
		if (e->lap_delta_valid)
			cJSON_AddNumberToObject(timing, "lap_delta", e->lap_delta);
		/* Diagnostics the desktop's device view can surface. */
		cJSON_AddNumberToObject(timing, "fixes_seen", e->fixes_seen);
		cJSON_AddNumberToObject(timing, "rejected_width", e->crossings_rejected_width);
		cJSON_AddNumberToObject(timing, "rejected_mintime", e->crossings_rejected_mintime);
	}
	rdm_lvgl_unlock();

	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

/* ── GET /api/lap/track ────────────────────────────────────────────────── */
static esp_err_t lap_track_get_handler(httpd_req_t *req) {
	if (!rdm_lvgl_lock(500))
		return web_server_send_busy(req);

	cJSON *root;
	const lap_track_t *t = lap_engine_get_track();
	if (t) {
		root = track_to_json(t);
	} else {
		root = cJSON_CreateObject();
		if (root)
			cJSON_AddBoolToObject(root, "empty", true);
	}
	rdm_lvgl_unlock();

	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
		return ESP_FAIL;
	}
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, root);
}

/* ── POST /api/lap/track ───────────────────────────────────────────────── */
static esp_err_t lap_track_post_handler(httpd_req_t *req) {
	/* Off the httpd stack: a full 8-sector track body is ~1.2 KB and the httpd
	 * task runs on only 5 KB, ahead of this file's deepest chain (JSON parse →
	 * set_track → 8× signal publish → LittleFS write). Heap-allocate, and free
	 * as soon as cJSON has taken its own copy. */
	char *buf = malloc(LAP_BODY_MAX);
	if (!buf) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
		return ESP_FAIL;
	}
	esp_err_t err = web_server_recv_body(req, buf, LAP_BODY_MAX);
	if (err != ESP_OK) {
		free(buf);
		return err;
	}

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
		return ESP_FAIL;
	}

	const cJSON *clear = cJSON_GetObjectItemCaseSensitive(root, "clear");
	if (cJSON_IsTrue(clear)) {
		cJSON_Delete(root);
		if (!rdm_lvgl_lock(500))
			return web_server_send_busy(req);
		lap_engine_set_track(NULL);
		rdm_lvgl_unlock();
		cJSON *resp = cJSON_CreateObject();
		if (resp)
			cJSON_AddBoolToObject(resp, "ok", true);
		httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
		return web_server_send_json(req, resp);
	}

	lap_track_t track;
	memset(&track, 0, sizeof(track));

	const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
	if (cJSON_IsString(name) && name->valuestring)
		snprintf(track.name, sizeof(track.name), "%s", name->valuestring);
	else
		snprintf(track.name, sizeof(track.name), "Track");

	const cJSON *mlt = cJSON_GetObjectItemCaseSensitive(root, "min_lap_time_s");
	track.min_lap_time_s = cJSON_IsNumber(mlt) ? (float)mlt->valuedouble : 0.0f;

	if (!line_from_json(cJSON_GetObjectItemCaseSensitive(root, "start_finish"),
	                    &track.start_finish)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "start_finish missing or invalid");
		return ESP_FAIL;
	}

	const cJSON *sectors = cJSON_GetObjectItemCaseSensitive(root, "sectors");
	if (cJSON_IsArray(sectors)) {
		const cJSON *el;
		cJSON_ArrayForEach(el, sectors) {
			if (track.sector_count >= LAP_MAX_SECTORS)
				break;
			if (line_from_json(el, &track.sectors[track.sector_count]))
				track.sector_count++;
		}
	}

	/* Point-to-point (sprint / hillclimb): the run ends on its own line.
	 *
	 * Both keys are required together. `point_to_point` without a usable
	 * `finish` would arm a run with nothing able to close it — the timer would
	 * look alive and never produce a time — so reject it loudly rather than
	 * store a track that cannot work. */
	const cJSON *p2p = cJSON_GetObjectItemCaseSensitive(root, "point_to_point");
	if (cJSON_IsTrue(p2p)) {
		const cJSON *fin = cJSON_GetObjectItemCaseSensitive(root, "finish");
		if (!line_from_json(fin, &track.finish)) {
			cJSON_Delete(root);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
			                    "point_to_point needs a valid finish line");
			return ESP_FAIL;
		}
		track.point_to_point = true;
	}
	cJSON_Delete(root);

	if (!rdm_lvgl_lock(500))
		return web_server_send_busy(req);
	err = lap_engine_set_track(&track);
	rdm_lvgl_unlock();

	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "track '%s' set via web (%u sector lines)", track.name, track.sector_count);
	cJSON *resp = cJSON_CreateObject();
	if (resp) {
		cJSON_AddBoolToObject(resp, "ok", true);
		/* Echo what was actually stored: invalid or excess sector lines are
		 * skipped, so a client that sent N can compare and warn if fewer landed
		 * rather than silently driving a track it never defined. */
		cJSON_AddNumberToObject(resp, "sector_count", track.sector_count);
	}
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, resp);
}

/* ── POST /api/lap/capture ─────────────────────────────────────────────── */
static esp_err_t lap_capture_post_handler(httpd_req_t *req) {
	char buf[256];
	esp_err_t err = web_server_recv_body(req, buf, sizeof(buf));
	if (err != ESP_OK)
		return err;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
		return ESP_FAIL;
	}
	const cJSON *what = cJSON_GetObjectItemCaseSensitive(root, "what");
	bool is_sf = cJSON_IsString(what) && strcmp(what->valuestring, "start_finish") == 0;
	bool is_sector = cJSON_IsString(what) && strcmp(what->valuestring, "sector") == 0;
	/* "finish" is the point-to-point finish line: drive to the top of the
	 * hill and capture it there, exactly as you do the start. */
	bool is_finish = cJSON_IsString(what) && strcmp(what->valuestring, "finish") == 0;
	const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "half_width_m");
	float half_width = cJSON_IsNumber(w) ? (float)w->valuedouble : 15.0f;
	cJSON_Delete(root);

	if (!is_sf && !is_sector && !is_finish) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "what must be start_finish, sector or finish");
		return ESP_FAIL;
	}
	if (half_width < 2.0f)
		half_width = 2.0f;
	if (half_width > 100.0f)
		half_width = 100.0f;

	if (!rdm_lvgl_lock(500))
		return web_server_send_busy(req);

	double lat, lon;
	float heading, speed;
	if (!lap_engine_last_position(&lat, &lon, &heading, &speed)) {
		rdm_lvgl_unlock();
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "no GPS fix — capture needs a live position");
		return ESP_FAIL;
	}

	/* Start from the current track so captures compose: SF first (which
	 * begins a fresh definition), then sectors appended in driving order. */
	lap_track_t track;
	const lap_track_t *cur = lap_engine_get_track();
	if (cur)
		track = *cur;
	else {
		memset(&track, 0, sizeof(track));
		snprintf(track.name, sizeof(track.name), "Captured");
	}

	lap_line_t line = {
	    .lat_deg = lat,
	    .lon_deg = lon,
	    .heading_deg = heading,
	    .half_width_m = half_width,
	};

	if (is_sf) {
		track.start_finish = line;
		/* A new start/finish begins a new track definition — stale sector
		 * lines from some other layout of the circuit would misfire. */
		track.sector_count = 0;
	} else if (is_finish) {
		if (!track.start_finish.half_width_m) {
			rdm_lvgl_unlock();
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
			                    "capture start_finish before the finish line");
			return ESP_FAIL;
		}
		track.finish = line;
		/* Capturing a finish line is what MAKES the course point-to-point. */
		track.point_to_point = true;
	} else {
		if (!track.start_finish.half_width_m) {
			rdm_lvgl_unlock();
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
			                    "capture start_finish before sectors");
			return ESP_FAIL;
		}
		if (track.sector_count >= LAP_MAX_SECTORS) {
			rdm_lvgl_unlock();
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sector limit reached");
			return ESP_FAIL;
		}
		track.sectors[track.sector_count++] = line;
	}

	err = lap_engine_set_track(&track);
	rdm_lvgl_unlock();

	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "captured %s at %.7f,%.7f heading %.1f (%.1f km/h)",
	         is_sf ? "start/finish" : (is_finish ? "finish" : "sector"),
	         lat, lon, (double)heading, (double)speed);

	cJSON *resp = cJSON_CreateObject();
	if (resp) {
		cJSON_AddBoolToObject(resp, "ok", true);
		cJSON_AddItemToObject(resp, "line", line_to_json(&line));
		cJSON_AddNumberToObject(resp, "sector_count", track.sector_count);
		/* Course-over-ground from a stationary car is noise; the UI should
		 * tell the user to re-capture while driving across the line. */
		cJSON_AddBoolToObject(resp, "heading_reliable", speed >= CAPTURE_MIN_SPEED_KPH);
	}
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, resp);
}

/* ── POST /api/lap/session/reset ───────────────────────────────────────── */
static esp_err_t lap_session_reset_handler(httpd_req_t *req) {
	if (!rdm_lvgl_lock(500))
		return web_server_send_busy(req);
	lap_engine_reset_session();
	rdm_lvgl_unlock();

	cJSON *resp = cJSON_CreateObject();
	if (resp)
		cJSON_AddBoolToObject(resp, "ok", true);
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, resp);
}

/* ── POST /api/lap/gps ─────────────────────────────────────────────────── */
static esp_err_t lap_gps_post_handler(httpd_req_t *req) {
	char buf[128];
	esp_err_t err = web_server_recv_body(req, buf, sizeof(buf));
	if (err != ESP_OK)
		return err;

	cJSON *root = cJSON_Parse(buf);
	const cJSON *base = root ? cJSON_GetObjectItemCaseSensitive(root, "base_id") : NULL;
	if (!cJSON_IsNumber(base)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "base_id required");
		return ESP_FAIL;
	}
	uint16_t base_id = (uint16_t)base->valueint;
	cJSON_Delete(root);

	/* Same rule the puck enforces: 16-aligned inside the GPS class range.
	 * See rdm-gps-node docs/PROTOCOL.md §2. */
	if (base_id < 0x400 || base_id > 0x430 || (base_id % 0x10) != 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
		                    "base_id must be 0x400/0x410/0x420/0x430");
		return ESP_FAIL;
	}

	if (!rdm_lvgl_lock(500))
		return web_server_send_busy(req);
	lap_engine_set_base_id(base_id);
	rdm_lvgl_unlock();

	cJSON *resp = cJSON_CreateObject();
	if (resp) {
		cJSON_AddBoolToObject(resp, "ok", true);
		cJSON_AddNumberToObject(resp, "base_id", base_id);
	}
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return web_server_send_json(req, resp);
}

/* ── Registration ──────────────────────────────────────────────────────── */
static const httpd_uri_t lap_status_uri = {
    .uri = "/api/lap/status", .method = HTTP_GET, .handler = lap_status_get_handler};
static const httpd_uri_t lap_track_get_uri = {
    .uri = "/api/lap/track", .method = HTTP_GET, .handler = lap_track_get_handler};
static const httpd_uri_t lap_track_post_uri = {
    .uri = "/api/lap/track", .method = HTTP_POST, .handler = lap_track_post_handler};
static const httpd_uri_t lap_capture_uri = {
    .uri = "/api/lap/capture", .method = HTTP_POST, .handler = lap_capture_post_handler};
static const httpd_uri_t lap_session_reset_uri = {
    .uri = "/api/lap/session/reset", .method = HTTP_POST, .handler = lap_session_reset_handler};
static const httpd_uri_t lap_gps_post_uri = {
    .uri = "/api/lap/gps", .method = HTTP_POST, .handler = lap_gps_post_handler};

void web_server_lap_register(httpd_handle_t server) {
	REGISTER_URI(server, &lap_status_uri);
	REGISTER_URI(server, &lap_track_get_uri);
	REGISTER_URI(server, &lap_track_post_uri);
	REGISTER_URI(server, &lap_capture_uri);
	REGISTER_URI(server, &lap_session_reset_uri);
	REGISTER_URI(server, &lap_gps_post_uri);
}
