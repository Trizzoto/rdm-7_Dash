/* keypad_lights.c — the tape player. See keypad_lights.h for why it exists.
 *
 * Three things in here are deliberate and worth reading before changing:
 *
 *  1. It transmits on its own clock, not the HTTP thread's. A boot is a
 *     sequence of frames at millisecond offsets; playing it inside a request
 *     handler would hold a socket open for four seconds and put the timing at
 *     the mercy of the network.
 *
 *  2. It refuses to play on the wrong bitrate rather than changing it. The
 *     keypad wizard is allowed to hop the dash's rate because a human is
 *     watching a bench; a boot player runs at ignition in a moving car, and
 *     re-timing the bus there to light some LEDs is not a trade anybody would
 *     take. If the rate does not match the file, it says so and does nothing.
 *
 *  3. It is rate-capped the same as /api/can/send. Studio's own baker already
 *     keeps every boot under the dash's 24-frames-a-second limit, but the cap
 *     lives here too, because the file arrives over the network and "Studio
 *     would never send that" is not a guarantee, it is a hope.
 */
#include "can/keypad_lights.h"

#include "can/can_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        /* fsync, fileno */

static const char *TAG = "kp_lights";

#define KPL_PATH      "/lfs/keypad_lights.json"
#define KPL_TMP_PATH  "/lfs/keypad_lights.tmp"
#define KPL_MAX_BYTES (48 * 1024)
/* The keypad plays its own start-up show first (2014h) and ignores PDOs until
 * it is operational. Both are over well inside a second; this is the wait
 * before the first frame of ours, on top of whatever the boot sequence has
 * already spent getting CAN up. */
#define KPL_SETTLE_MS 900
#define KPL_TX_PER_SEC 24

typedef struct {
	uint16_t ms;
	uint32_t id;
	uint8_t  data[8];
	uint8_t  dlc;
} kpl_frame_t;

static kpl_frame_t *s_frames = NULL;
static uint16_t     s_count = 0;
static uint16_t     s_total_ms = 0;
static bool         s_loop = false;
static uint16_t     s_loop_from = 0;
static bool         s_enabled = true;
static bool         s_nmt = true;
static uint8_t      s_node = 0x15;
static uint16_t     s_baud = 125;
static char         s_keypad[24] = {0};
static char         s_reason[96] = {0};

static volatile bool s_playing = false;
static volatile bool s_stop = false;
static TaskHandle_t  s_task = NULL;

/* ── parsing ─────────────────────────────────────────────────────────────
 * The file is Studio's own baked output. Everything here is defensive: it
 * arrives over HTTP, and the only thing standing between a malformed one and
 * a boot loop is this function refusing it.
 */
static int _hex_nibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
	return -1;
}

static int _parse_frames(cJSON *root, kpl_frame_t **out, uint16_t *out_n,
                         uint16_t *out_ms) {
	cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "frames");
	if (!cJSON_IsArray(arr)) return -1;
	int n = cJSON_GetArraySize(arr);
	if (n <= 0) return -1;
	if (n > KEYPAD_LIGHTS_MAX_FRAMES) n = KEYPAD_LIGHTS_MAX_FRAMES;

	kpl_frame_t *f = calloc((size_t)n, sizeof(kpl_frame_t));
	if (!f) return -1;

	int i = 0, last_ms = 0;
	cJSON *el;
	cJSON_ArrayForEach(el, arr) {
		if (i >= n) break;
		cJSON *jms = cJSON_GetObjectItemCaseSensitive(el, "ms");
		cJSON *jid = cJSON_GetObjectItemCaseSensitive(el, "id");
		cJSON *jd  = cJSON_GetObjectItemCaseSensitive(el, "d");
		if (!cJSON_IsNumber(jms) || !cJSON_IsNumber(jid) || !cJSON_IsString(jd))
			continue;
		int ms = jms->valueint;
		if (ms < 0) ms = 0;
		if (ms > 65000) ms = 65000;
		/* Times must only go forwards. A file whose offsets go backwards
		 * would make the player sleep on a negative delta, i.e. not at all,
		 * and dump the rest of the tape onto the bus in one burst. */
		if (ms < last_ms) ms = last_ms;
		last_ms = ms;

		uint32_t id = (uint32_t)jid->valuedouble;
		if (id > 0x7FFu) continue;      /* keypad PDOs are 11-bit, always */

		kpl_frame_t *fr = &f[i];
		fr->ms = (uint16_t)ms;
		fr->id = id;
		const char *s = jd->valuestring;
		int b = 0;
		while (b < 8 && s[0] && s[1]) {
			int hi = _hex_nibble(s[0]), lo = _hex_nibble(s[1]);
			if (hi < 0 || lo < 0) break;
			fr->data[b++] = (uint8_t)((hi << 4) | lo);
			s += 2;
		}
		fr->dlc = 8;                    /* every keypad PDO here is 8 bytes */
		(void)b;
		i++;
	}
	if (i == 0) { free(f); return -1; }
	*out = f;
	*out_n = (uint16_t)i;
	*out_ms = f[i - 1].ms;
	return 0;
}

/* Adopt a parsed document into the live tape. Takes ownership of nothing;
 * copies what it needs. */
static int _adopt(cJSON *root) {
	kpl_frame_t *f = NULL;
	uint16_t n = 0, ms = 0;
	if (_parse_frames(root, &f, &n, &ms) != 0) return -1;

	cJSON *j;
	bool loop = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "loop"));
	uint16_t loop_from = 0;
	j = cJSON_GetObjectItemCaseSensitive(root, "loop_from_ms");
	if (cJSON_IsNumber(j) && j->valueint >= 0) loop_from = (uint16_t)j->valueint;

	uint8_t node = 0x15;
	j = cJSON_GetObjectItemCaseSensitive(root, "node");
	if (cJSON_IsNumber(j)) node = (uint8_t)(j->valueint & 0x7F);

	uint16_t baud = 125;
	j = cJSON_GetObjectItemCaseSensitive(root, "baud");
	if (cJSON_IsNumber(j) && j->valueint > 0) baud = (uint16_t)j->valueint;

	bool nmt = true;
	j = cJSON_GetObjectItemCaseSensitive(root, "nmt");
	if (cJSON_IsBool(j)) nmt = cJSON_IsTrue(j);

	bool enabled = true;
	j = cJSON_GetObjectItemCaseSensitive(root, "at_boot");
	if (cJSON_IsBool(j)) enabled = cJSON_IsTrue(j);

	char keypad[24] = {0};
	j = cJSON_GetObjectItemCaseSensitive(root, "keypad");
	if (cJSON_IsString(j) && j->valuestring) {
		strncpy(keypad, j->valuestring, sizeof(keypad) - 1);
	}

	/* Swap in only once everything parsed. */
	kpl_frame_t *old = s_frames;
	s_frames = f;
	s_count = n;
	s_total_ms = ms;
	s_loop = loop;
	s_loop_from = (loop_from <= ms) ? loop_from : 0;
	s_node = node;
	s_baud = baud;
	s_nmt = nmt;
	s_enabled = enabled;
	memcpy(s_keypad, keypad, sizeof(s_keypad));
	s_reason[0] = '\0';
	free(old);
	return 0;
}

/* ── the file ────────────────────────────────────────────────────────────── */

static esp_err_t _write_file(const char *json, size_t len) {
	FILE *f = fopen(KPL_TMP_PATH, "w");
	if (!f) return ESP_FAIL;
	size_t w = fwrite(json, 1, len, f);
	/* fflush drains stdio's buffer; fsync commits to flash. Both before the
	 * rename, so the published file is durable. (fsync is ESP/POSIX only —
	 * the native test builds this same file on a host with no such call, and
	 * durability there is not a thing anybody is relying on.) */
	bool bad = (w != len) || (fflush(f) != 0);
#ifdef ESP_PLATFORM
	if (!bad && fsync(fileno(f)) != 0) bad = true;
#endif
	if (bad) {
		fclose(f);
		remove(KPL_TMP_PATH);
		return ESP_FAIL;
	}
	if (fclose(f) != 0) { remove(KPL_TMP_PATH); return ESP_FAIL; }
	/* rename() is atomic on LittleFS: a power cut mid-write leaves the old
	 * file or the new one, never half of either. Same idiom as channels.json. */
	if (rename(KPL_TMP_PATH, KPL_PATH) != 0) {
		remove(KPL_TMP_PATH);
		return ESP_FAIL;
	}
	return ESP_OK;
}

esp_err_t keypad_lights_init(void) {
	FILE *f = fopen(KPL_PATH, "rb");
	if (!f) return ESP_ERR_NOT_FOUND;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > KPL_MAX_BYTES) { fclose(f); return ESP_FAIL; }
	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) {
		ESP_LOGW(TAG, "stored boot does not parse — ignoring it");
		return ESP_FAIL;
	}
	int rc = _adopt(root);
	cJSON_Delete(root);
	if (rc != 0) {
		ESP_LOGW(TAG, "stored boot has no usable frames — ignoring it");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "boot loaded: %u frames, %u ms, node 0x%02X @ %u k, %s",
	         (unsigned)s_count, (unsigned)s_total_ms, s_node, s_baud,
	         s_loop ? "loops" : "holds");
	return ESP_OK;
}

esp_err_t keypad_lights_store(const char *json, size_t len) {
	if (!json || !len || len > KPL_MAX_BYTES) return ESP_ERR_INVALID_ARG;
	cJSON *root = cJSON_Parse(json);
	if (!root) return ESP_ERR_INVALID_ARG;
	/* Validate into memory FIRST. Only a document that would actually play
	 * gets written, so a bad upload cannot replace a working boot. */
	int rc = _adopt(root);
	cJSON_Delete(root);
	if (rc != 0) return ESP_ERR_INVALID_ARG;
	esp_err_t err = _write_file(json, len);
	if (err != ESP_OK) ESP_LOGE(TAG, "could not write %s", KPL_PATH);
	else ESP_LOGI(TAG, "stored boot: %u frames for %s", (unsigned)s_count,
	              s_keypad[0] ? s_keypad : "a keypad");
	return err;
}

esp_err_t keypad_lights_forget(void) {
	keypad_lights_stop();
	free(s_frames);
	s_frames = NULL;
	s_count = 0;
	s_total_ms = 0;
	s_keypad[0] = '\0';
	s_reason[0] = '\0';
	remove(KPL_PATH);
	return ESP_OK;
}

esp_err_t keypad_lights_set_enabled(bool enabled) {
	if (!s_frames) return ESP_ERR_NOT_FOUND;
	s_enabled = enabled;
	/* Rewrite the file with the flag in it, so it survives a reboot without a
	 * second store. Cheap: the tape is a few kB and this happens on a click. */
	FILE *f = fopen(KPL_PATH, "rb");
	if (!f) return ESP_OK;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > KPL_MAX_BYTES) { fclose(f); return ESP_OK; }
	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = '\0';
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) return ESP_OK;
	cJSON_DeleteItemFromObjectCaseSensitive(root, "at_boot");
	cJSON_AddBoolToObject(root, "at_boot", enabled);
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!out) return ESP_ERR_NO_MEM;
	esp_err_t err = _write_file(out, strlen(out));
	cJSON_free(out);
	return err;
}

/* ── playing ─────────────────────────────────────────────────────────────── */

static void _kpl_task(void *arg) {
	(void)arg;
	int64_t t0 = esp_timer_get_time();
	int64_t win_us = t0;
	int in_win = 0;

	if (s_nmt) {
		/* PDOs are ignored until the node is operational, and a keypad that
		 * has just been powered may have missed an earlier start. */
		uint8_t nmt[8] = {0x01, s_node, 0, 0, 0, 0, 0, 0};
		can_transmit_frame(0x000, nmt, 2);
	}
	vTaskDelay(pdMS_TO_TICKS(KPL_SETTLE_MS));
	t0 = esp_timer_get_time();

	uint16_t i = 0;
	while (!s_stop) {
		if (i >= s_count) {
			if (!s_loop) break;
			/* One turn of the resting animation, again. The tape's own clock
			 * restarts at loop_from_ms so the join is seamless. */
			i = 0;
			while (i < s_count && s_frames[i].ms < s_loop_from) i++;
			if (i >= s_count) break;
			t0 = esp_timer_get_time() - (int64_t)s_frames[i].ms * 1000;
		}
		int64_t due = t0 + (int64_t)s_frames[i].ms * 1000;
		int64_t now = esp_timer_get_time();
		if (due > now) {
			int64_t wait_ms = (due - now) / 1000;
			if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
			if (s_stop) break;
		}
		now = esp_timer_get_time();
		if (now - win_us >= 1000000) { win_us = now; in_win = 0; }
		if (in_win >= KPL_TX_PER_SEC) {
			/* Over the cap: wait out the window rather than dropping the
			 * frame. A dropped ring frame is a keypad stuck on the wrong
			 * colour; a late one is a boot a few milliseconds slower. */
			vTaskDelay(pdMS_TO_TICKS(50));
			continue;
		}
		can_transmit_frame(s_frames[i].id, s_frames[i].data, s_frames[i].dlc);
		in_win++;
		i++;
	}

	ESP_LOGI(TAG, "boot %s", s_stop ? "stopped" : "played");
	s_playing = false;
	s_stop = false;
	s_task = NULL;
	vTaskDelete(NULL);
}

/* 0=125k 1=250k 2=500k 3=1M — the dash's own index, mapped to kbit so the
 * file can say what it was made for in the unit a human types. */
static uint16_t _dash_baud_k(void) {
	switch (can_get_bitrate_index()) {
		case 0: return 125;
		case 1: return 250;
		case 2: return 500;
		case 3: return 1000;
		default: return 0;
	}
}

esp_err_t keypad_lights_play(char *why, size_t why_len) {
	const char *no = NULL;
	if (!s_frames || !s_count) no = "No boot is stored on this dash.";
	else if (can_is_suspended())
		no = "The dash's bus scan owns the CAN peripheral right now.";
	else if (_dash_baud_k() != s_baud)
		no = "The dash's bus is not on the bitrate this boot was made for.";
	if (no) {
		if (why && why_len) { strncpy(why, no, why_len - 1); why[why_len - 1] = '\0'; }
		strncpy(s_reason, no, sizeof(s_reason) - 1);
		return ESP_ERR_INVALID_STATE;
	}
	if (s_playing) {
		keypad_lights_stop();
		/* let the task notice and exit before starting another */
		for (int i = 0; i < 40 && s_playing; i++) vTaskDelay(pdMS_TO_TICKS(10));
	}
	s_reason[0] = '\0';
	s_stop = false;
	s_playing = true;
	if (xTaskCreate(_kpl_task, "kp_lights", 3072, NULL, 4, &s_task) != pdPASS) {
		s_playing = false;
		if (why && why_len) strncpy(why, "Out of memory.", why_len - 1);
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

void keypad_lights_stop(void) {
	if (s_playing) s_stop = true;
}

void keypad_lights_boot_play(void) {
	if (!s_frames || !s_count) return;
	if (!s_enabled) {
		strncpy(s_reason, "Set not to play at power-up.", sizeof(s_reason) - 1);
		return;
	}
	char why[96] = {0};
	if (keypad_lights_play(why, sizeof(why)) != ESP_OK)
		ESP_LOGW(TAG, "not playing at boot: %s", why);
}

void keypad_lights_get_status(keypad_lights_status_t *out) {
	if (!out) return;
	memset(out, 0, sizeof(*out));
	out->present = (s_frames != NULL && s_count > 0);
	out->playing = s_playing;
	out->enabled = s_enabled;
	out->loop = s_loop;
	out->frames = s_count;
	out->ms = s_total_ms;
	out->node = s_node;
	out->baud = s_baud;
	memcpy(out->keypad, s_keypad, sizeof(out->keypad));
	if (out->present && _dash_baud_k() != s_baud && !s_reason[0])
		snprintf(s_reason, sizeof(s_reason),
		         "This dash's bus is on %u k, the boot was made for %u k.",
		         (unsigned)_dash_baud_k(), (unsigned)s_baud);
	memcpy(out->reason, s_reason, sizeof(out->reason));
}
