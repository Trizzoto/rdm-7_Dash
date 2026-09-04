/* web_server_ota.c — OTA update endpoints
 *
 * Endpoints:
 *   GET  /api/ota/status  cached state: current + latest version, progress,
 *                         release notes, file size. Safe to poll.
 *   POST /api/ota/check   kick off check_for_update() on a background task.
 *                         Returns 202 immediately.
 *   POST /api/ota/start   begin download + flash via start_ota_update_task().
 *   POST /api/ota/upload  stream a firmware .bin straight from the LAN into
 *                         the inactive OTA slot, then reboot. */
#include "web_server_internal.h"
#include "cJSON.h"
#include "net/ota_handler.h"
#include "system/device_id.h"
#include "system/render_perf.h"
#include "version.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server_ota";

static const char *_ota_status_string(ota_status_t s) {
	switch (s) {
		case OTA_IDLE:                 return "idle";
		case OTA_CHECKING:             return "checking";
		case OTA_NO_UPDATE_AVAILABLE:  return "no_update";
		case OTA_UPDATE_AVAILABLE:     return "available";
		case OTA_UPDATE_IN_PROGRESS:   return "installing";
		case OTA_UPDATE_COMPLETED:     return "completed";
		case OTA_UPDATE_FAILED:        return "failed";
		default:                       return "unknown";
	}
}

static esp_err_t _ota_status_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	ota_status_t st = get_ota_status();
	const char *latest = get_latest_version();
	const char *notes  = get_release_notes();
	float size_mb      = get_update_file_size_mb();
	int progress       = get_ota_progress();

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "status",          _ota_status_string(st));
	cJSON_AddStringToObject(root, "current_version", FIRMWARE_VERSION);
	cJSON_AddStringToObject(root, "latest_version",  latest ? latest : "");
	cJSON_AddStringToObject(root, "release_notes",   notes  ? notes  : "");
	cJSON_AddNumberToObject(root, "file_size_mb",    size_mb);
	cJSON_AddNumberToObject(root, "progress",        progress);
	cJSON_AddBoolToObject  (root, "update_available", st == OTA_UPDATE_AVAILABLE);

	return web_server_send_json(req, root);
}

/* Background-task wrapper: the HTTPD handler returns 202 immediately;
 * check_for_update runs here and mutates the shared OTA state. Stack
 * sized for HTTPS + cJSON parse of the GitHub release payload. */
static void _ota_check_task(void *arg) {
	(void)arg;
	check_for_update();
	vTaskDelete(NULL);
}

static esp_err_t _ota_check_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	/* Skip if a check is already in flight to avoid stacking tasks. */
	if (get_ota_status() == OTA_CHECKING) {
		httpd_resp_set_status(req, "202 Accepted");
		httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
		return ESP_OK;
	}

	/* Same 6 KB contiguous-internal-RAM constraint as the install task — see
	 * the OTA_STACK note in ota_handler.c. Name the real cause: "failed to
	 * start check task" sent us hunting the network for an hour. */
	BaseType_t ok = xTaskCreate(_ota_check_task, "ota_check", 6144,
	                            NULL, 5, NULL);
	if (ok != pdPASS) {
		char msg[224];
		snprintf(msg, sizeof(msg),
			"{\"error\":\"not enough contiguous internal RAM to start the "
			"update check (%u bytes free, 6144 needed) \xe2\x80\x94 reboot the "
			"dash and retry\",\"internal_largest_free\":%u}",
			(unsigned)ota_internal_largest_free(),
			(unsigned)ota_internal_largest_free());
		ESP_LOGE(TAG, "check task create failed — %u B contiguous internal, need 6144",
		         (unsigned)ota_internal_largest_free());
		httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_sendstr(req, msg);
		return ESP_OK;
	}
	httpd_resp_set_status(req, "202 Accepted");
	httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
	return ESP_OK;
}

static esp_err_t _ota_start_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	ota_status_t st = get_ota_status();
	if (st != OTA_UPDATE_AVAILABLE) {
		httpd_resp_set_status(req, "409 Conflict");
		httpd_resp_sendstr(req, "{\"error\":\"no update available \xe2\x80\x94 run /api/ota/check first\"}");
		return ESP_OK;
	}

	/* Report what actually happened. This used to return "installing"
	 * unconditionally, so a spawn failure — the install never opening a
	 * socket — surfaced in the UI as a download stuck at 0%. */
	esp_err_t err = start_ota_update_task();
	if (err == ESP_ERR_NO_MEM) {
		char msg[224];
		snprintf(msg, sizeof(msg),
			"{\"error\":\"not enough contiguous internal RAM to start the "
			"update (%u bytes free, 6144 needed) \xe2\x80\x94 reboot the dash "
			"and retry, or push the build over the LAN with "
			"POST /api/ota/upload\",\"internal_largest_free\":%u}",
			(unsigned)ota_internal_largest_free(),
			(unsigned)ota_internal_largest_free());
		httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_sendstr(req, msg);
		return ESP_OK;
	}
	if (err != ESP_OK) {
		httpd_resp_set_status(req, "409 Conflict");
		httpd_resp_sendstr(req, "{\"error\":\"another OTA operation is already running\"}");
		return ESP_OK;
	}

	httpd_resp_set_status(req, "202 Accepted");
	httpd_resp_sendstr(req, "{\"status\":\"installing\"}");
	return ESP_OK;
}

/* ── Local firmware push ────────────────────────────────────────────────
 *
 * POST /api/ota/upload with the raw esp32-firmware.bin as the body.
 *
 * Why this exists: /api/ota/start can ONLY install what the GitHub release
 * feed offered, and the only other esp_ota_write path is the USB serial
 * `upload.*` RPC. A dash on WiFi with no cable attached therefore had no
 * way to receive a local build — you had to publish a public release (which
 * reaches every device that auto-checks) just to test one board.
 *
 * SECURITY: this is deliberately NOT authenticated, matching the rest of the
 * LAN API (layout save, channels import, factory reset are all open too).
 * The severity is higher here — this one is arbitrary code execution — so it
 * requires an `X-RDM-Device: <serial>` header that must match this board.
 * That is NOT real auth: the serial is readable from /api/device/info by
 * anyone on the LAN. It exists to stop a drive-by or a CSRF'd browser from
 * flashing the wrong dash by accident, and to force a deliberate target.
 * Real auth is tracked as the open "OTA-auth" item from the 2026-07-05
 * hardening sweep and MUST land before customer rollout.
 *
 * Rollback note: main.c calls esp_ota_mark_app_valid_cancel_rollback() once
 * the LVGL task is up, so the freshly-flashed image self-validates ~10 s
 * into boot. Do NOT power-cycle before the dash has rendered or the
 * bootloader rolls back to the previous slot. */
#define OTA_UPLOAD_CHUNK  4096
/* An ESP32 app image starts with the 0xE9 magic byte. Catches the common
 * mistake of POSTing the merged flash image or a layout JSON. */
#define ESP_IMAGE_MAGIC   0xE9
/* Consecutive recv timeouts tolerated before giving up. esp_http_server runs
 * ONE task for every socket, so a handler that blocks blocks the whole LAN
 * API — a client that announces a Content-Length and then goes silent must
 * not be able to pin it forever. At config.recv_wait_timeout = 30 s this is a
 * 90 s window of total silence, far longer than any real stall on a LAN, and
 * bounded. */
#define OTA_UPLOAD_MAX_STALLS 3

static esp_err_t _ota_upload_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_type(req, "application/json");

	/* Don't race the release-download OTA for the same partition. */
	if (get_ota_status() == OTA_UPDATE_IN_PROGRESS) {
		httpd_resp_set_status(req, "409 Conflict");
		httpd_resp_sendstr(req, "{\"error\":\"an OTA download is already installing\"}");
		return ESP_OK;
	}

	/* Deliberate-target check — see the SECURITY note above. */
	char want[MAX_SERIAL_LENGTH] = {0};
	if (get_device_serial(want) != ESP_OK || want[0] == '\0') {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\":\"device serial unavailable\"}");
		return ESP_OK;
	}
	char got[MAX_SERIAL_LENGTH] = {0};
	if (httpd_req_get_hdr_value_str(req, "X-RDM-Device", got, sizeof(got)) != ESP_OK ||
	    strcmp(got, want) != 0) {
		httpd_resp_set_status(req, "403 Forbidden");
		httpd_resp_sendstr(req,
			"{\"error\":\"X-RDM-Device header must carry this board's serial "
			"(see /api/device/info)\"}");
		return ESP_OK;
	}

	int total = req->content_len;
	if (total <= 0) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\":\"empty body — POST the raw .bin\"}");
		return ESP_OK;
	}

	const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
	if (!part) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\":\"no OTA partition available\"}");
		return ESP_OK;
	}
	if ((size_t)total > part->size) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\":\"image larger than the OTA partition\"}");
		return ESP_OK;
	}

	uint8_t *buf = heap_caps_malloc(OTA_UPLOAD_CHUNK, MALLOC_CAP_SPIRAM);
	if (!buf) buf = malloc(OTA_UPLOAD_CHUNK);
	if (!buf) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\":\"OOM\"}");
		return ESP_OK;
	}

	esp_ota_handle_t handle = 0;
	esp_err_t err = esp_ota_begin(part, total, &handle);
	if (err != ESP_OK) {
		free(buf);
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\":\"esp_ota_begin failed\"}");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "OTA upload starting: %d bytes -> partition '%s'",
	         total, part->label);

	/* Panel health across the write. A flash write is the one thing that can
	 * take the PSRAM cache away from the RGB panel's bounce-buffer refill
	 * interrupt, which would freeze or tear the screen mid-update; XIP plus
	 * LCD_RGB_ISR_IRAM_SAFE are what prevent it. Both ISRs are in IRAM, so if
	 * the cache did go away the refill count would fall behind the VSYNC count.
	 * Logged rather than measured over HTTP because the upload owns the server
	 * for its whole duration — nothing can poll us while this runs. */
	uint32_t v0 = 0, b0 = 0;
	panel_frame_counters_get(&v0, &b0);

	int received = 0;
	bool checked_magic = false;
	int stalls = 0;
	while (received < total) {
		int want_n = total - received;
		if (want_n > OTA_UPLOAD_CHUNK) want_n = OTA_UPLOAD_CHUNK;
		int n = httpd_req_recv(req, (char *)buf, want_n);
		if (n == HTTPD_SOCK_ERR_TIMEOUT) {
			/* Transient — keep waiting, but only so long (see MAX_STALLS). */
			if (++stalls > OTA_UPLOAD_MAX_STALLS) {
				esp_ota_abort(handle);
				free(buf);
				ESP_LOGE(TAG, "OTA upload stalled at %d/%d bytes — giving up",
				         received, total);
				httpd_resp_set_status(req, "408 Request Timeout");
				httpd_resp_sendstr(req,
					"{\"error\":\"upload stalled — no data received\"}");
				return ESP_OK;
			}
			continue;
		}
		stalls = 0;   /* any progress resets the stall budget */
		if (n <= 0) {
			esp_ota_abort(handle);
			free(buf);
			ESP_LOGE(TAG, "OTA upload aborted at %d/%d bytes", received, total);
			httpd_resp_set_status(req, "400 Bad Request");
			httpd_resp_sendstr(req, "{\"error\":\"connection dropped mid-upload\"}");
			return ESP_OK;
		}
		/* Reject a non-ESP image before writing a single byte of it. */
		if (!checked_magic) {
			checked_magic = true;
			if (buf[0] != ESP_IMAGE_MAGIC) {
				esp_ota_abort(handle);
				free(buf);
				httpd_resp_set_status(req, "400 Bad Request");
				httpd_resp_sendstr(req,
					"{\"error\":\"not an ESP32 app image (bad magic) — "
					"send build/esp32-firmware.bin, not the merged flash image\"}");
				return ESP_OK;
			}
		}
		err = esp_ota_write(handle, buf, n);
		if (err != ESP_OK) {
			esp_ota_abort(handle);
			free(buf);
			ESP_LOGE(TAG, "esp_ota_write failed at %d bytes: %s",
			         received, esp_err_to_name(err));
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "{\"error\":\"flash write failed\"}");
			return ESP_OK;
		}
		received += n;
	}
	free(buf);

	{
		uint32_t v1 = 0, b1 = 0;
		panel_frame_counters_get(&v1, &b1);
		uint32_t dv = v1 - v0, db = b1 - b0;
		if (db == 0 && dv == 0) {
			/* Bounce mode off: nothing refills, so there is nothing to compare. */
		} else if (db == 0) {
			ESP_LOGE(TAG, "panel STALLED across the write: %u VSYNCs, 0 refills "
			              "— the display lost the PSRAM cache", (unsigned)dv);
		} else if (dv > db) {
			ESP_LOGW(TAG, "panel starved across the write: %u VSYNCs vs %u refills "
			              "(%u frames behind)", (unsigned)dv, (unsigned)db,
			         (unsigned)(dv - db));
		} else {
			ESP_LOGI(TAG, "panel healthy across the write: %u VSYNCs, %u refills",
			         (unsigned)dv, (unsigned)db);
		}
	}

	err = esp_ota_end(handle);   /* validates the image */
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req,
			err == ESP_ERR_OTA_VALIDATE_FAILED
				? "{\"error\":\"image failed validation\"}"
				: "{\"error\":\"esp_ota_end failed\"}");
		return ESP_OK;
	}
	err = esp_ota_set_boot_partition(part);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\":\"could not set boot partition\"}");
		return ESP_OK;
	}

	/* Arm the reboot BEFORE answering, so "rebooting" is a fact rather than an
	 * intention. The image is written, validated and set bootable by this
	 * point, so a failure here is recoverable by a power cycle — but ONLY if
	 * we say so. This previously spawned a 2 KB task and reported
	 * "rebooting":true unconditionally; on RDM-E806-90A2 (2304 B contiguous
	 * internal) the spawn failed, the dash carried on running the old image,
	 * and rdm_ota_push.py reported a successful flash. */
	bool rebooting = web_server_schedule_reboot(700);

	ESP_LOGW(TAG, "OTA upload complete (%d bytes) into '%s' — %s",
	         received, part->label,
	         rebooting ? "rebooting" : "REBOOT NOT SCHEDULED, power-cycle to apply");

	char resp[240];
	snprintf(resp, sizeof(resp),
	         "{\"ok\":true,\"bytes\":%d,\"partition\":\"%s\",\"rebooting\":%s%s}",
	         received, part->label, rebooting ? "true" : "false",
	         rebooting ? ""
	                   : ",\"warning\":\"image installed and set bootable, but the "
	                     "dash could not schedule its own reboot \\u2014 power-cycle "
	                     "it to finish the update\"");
	httpd_resp_sendstr(req, resp);
	return ESP_OK;
}

static const httpd_uri_t ota_status_uri = {
    .uri = "/api/ota/status", .method = HTTP_GET,
    .handler = _ota_status_handler, .user_ctx = NULL};
static const httpd_uri_t ota_upload_uri = {
    .uri = "/api/ota/upload", .method = HTTP_POST,
    .handler = _ota_upload_handler, .user_ctx = NULL};
static const httpd_uri_t ota_check_uri = {
    .uri = "/api/ota/check", .method = HTTP_POST,
    .handler = _ota_check_handler, .user_ctx = NULL};
static const httpd_uri_t ota_start_uri = {
    .uri = "/api/ota/start", .method = HTTP_POST,
    .handler = _ota_start_handler, .user_ctx = NULL};

void web_server_ota_register(httpd_handle_t server) {
	REGISTER_URI(server, &ota_status_uri);
	REGISTER_URI(server, &ota_check_uri);
	REGISTER_URI(server, &ota_start_uri);
	REGISTER_URI(server, &ota_upload_uri);
}
