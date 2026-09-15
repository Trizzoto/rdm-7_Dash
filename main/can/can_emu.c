/*
 * can_emu.c — the clock, the bus and the file behind can_emu.h.
 *
 * Read can_emu.h first; it says what the rules are. This says how:
 *
 *  - The model (can_emu_core's cemu_model_t, ~60 KB) lives in PSRAM and is
 *    only ever touched on the LVGL task. A store from HTTP parses a private
 *    copy to validate, writes the file, and hands the text to the LVGL task,
 *    which parses again and swaps — carrying latch states across by id, so
 *    editing a device's voltages doesn't switch its cruise off.
 *  - One lv_timer at 10 ms walks every device. A frame is due every_ms after
 *    it last went; a press makes the frames that carry that control due now,
 *    so a button reaches the bus within one tick, not one period.
 *  - A new or re-enabled device listens for LISTEN_FIRST_MS before its first
 *    frame. A clash (hearing one of its IDs) stops it; it resumes on its own
 *    once that ID has been quiet for CLASH_QUIET_MS.
 *  - The old per-widget outputs share the same tick, composed per ID.
 */
#include "can_emu.h"
#include "can_emu_core.h"
#include "can_manager.h"

#include "widgets/signal.h"
#include "widgets/signal_sim.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "nvs.h"
#include "system/rdm_lv_async.h"
#include "ui/lvgl_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "can_emu";

#define CAN_EMU_TMP_PATH  "/lfs/can_emu.tmp"
#define TICK_MS           10
#define LISTEN_FIRST_MS   300
#define CLASH_QUIET_MS    3000
#define FAIL_BURST        25
#define BACKOFF_MS        3000
#define TEST_HOLD_MS      1500
#define MAX_TEST_HOLDS    4
#define LEGACY_OFF_BURST  3

/* main/CMakeLists.txt embeds a minified copy of can_emu_templates.json. */
extern const char can_emu_templates_json_start[] asm("_binary_can_emu_templates_min_json_start");

static cemu_model_t *s_m;               /* LVGL task only */
static uint32_t      s_generation;
static lv_timer_t   *s_timer;
static SemaphoreHandle_t s_file_lock;

/* Tables in PSRAM, not internal RAM: internal RAM belongs to WiFi, and ~8 KB
 * of UI statics once made esp_wifi_init fail at boot (see ADR-0075). */
static EXT_RAM_BSS_ATTR uint32_t s_filter_ids[CEMU_FILTER_MAX];
static int      s_filter_n;
static bool     s_filter_ext;

typedef struct {
	char     ref[CEMU_ID_LEN * 2 + CEMU_STATE_LEN + 4];
	uint32_t until_ms;
} test_hold_t;
static EXT_RAM_BSS_ATTR test_hold_t s_tests[MAX_TEST_HOLDS];

static EXT_RAM_BSS_ATTR cemu_legacy_t s_legacy[CEMU_MAX_LEGACY];
static EXT_RAM_BSS_ATTR uint32_t      s_legacy_due[CEMU_MAX_LEGACY];
static EXT_RAM_BSS_ATTR bool          s_legacy_now[CEMU_MAX_LEGACY];
static char          s_legacy_gone;     /* owner of a slot still owed OFF frames */

/* ── small helpers ─────────────────────────────────────────────────────── */

static cemu_model_t *_model_alloc(void) {
	cemu_model_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM);
	return m ? m : calloc(1, sizeof(*m));
}

static uint16_t _dash_baud_k(void) {
	switch (can_get_bitrate_index()) {
		case 0: return 125;
		case 1: return 250;
		case 2: return 500;
		case 3: return 1000;
		default: return 0;
	}
}

static void _copy(char *dst, size_t cap, const char *src) {
	if (!cap) return;
	strncpy(dst, src ? src : "", cap - 1);
	dst[cap - 1] = '\0';
}

static bool _channel_value(const char *name, float *v, void *user) {
	(void)user;
	/* A simulated value must never reach an ECU: while the simulator runs
	 * every channel field sends its "no data" value instead. */
	if (signal_sim_is_active()) return false;
	int16_t i = signal_find_by_name(name);
	signal_t *s = i >= 0 ? signal_get_by_index((uint16_t)i) : NULL;
	if (!s || s->is_stale) return false;
	*v = s->current_value;
	return true;
}

/* ── latches that survive power-off ────────────────────────────────────── */

static void _latch_key(const cemu_device_t *d, const cemu_control_t *c, char key[16]) {
	uint32_t h = 2166136261u;                    /* FNV-1a of "device:control" */
	for (const char *p = d->id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
	h = (h ^ ':') * 16777619u;
	for (const char *p = c->id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
	snprintf(key, 16, "l%08lx", (unsigned long)h);
}

static void _latch_save(const cemu_device_t *d, const cemu_control_t *c) {
	if (!c->remember) return;
	nvs_handle_t h;
	if (nvs_open("canemu", NVS_READWRITE, &h) != ESP_OK) return;
	char key[16];
	_latch_key(d, c, key);
	nvs_set_u8(h, key, c->latched);
	nvs_commit(h);
	nvs_close(h);
}

static void _latch_load(cemu_model_t *m) {
	nvs_handle_t h;
	if (nvs_open("canemu", NVS_READONLY, &h) != ESP_OK) return;
	for (int d = 0; d < m->n_devices; d++)
		for (int c = 0; c < m->devices[d].n_controls; c++) {
			cemu_control_t *ct = &m->devices[d].controls[c];
			if (!ct->remember) continue;
			char key[16];
			uint8_t v;
			_latch_key(&m->devices[d], ct, key);
			if (nvs_get_u8(h, key, &v) == ESP_OK) ct->latched = v ? 1 : 0;
		}
	nvs_close(h);
}

/* ── the file ──────────────────────────────────────────────────────────── */

const char *can_emu_templates(void) { return can_emu_templates_json_start; }

char *can_emu_read_file(void) {
	char *buf = NULL;
	if (s_file_lock) xSemaphoreTake(s_file_lock, portMAX_DELAY);
	FILE *f = fopen(CAN_EMU_PATH, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0 && sz <= CAN_EMU_MAX_BYTES) {
			buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
			if (buf) {
				size_t rd = fread(buf, 1, (size_t)sz, f);
				buf[rd] = '\0';
			}
		}
		fclose(f);
	}
	if (s_file_lock) xSemaphoreGive(s_file_lock);
	if (!buf) {
		const char *empty = "{\"v\":1,\"devices\":[]}";
		buf = malloc(strlen(empty) + 1);
		if (buf) strcpy(buf, empty);
	}
	return buf;
}

static esp_err_t _write_file(const char *json) {
	if (!s_file_lock) s_file_lock = xSemaphoreCreateMutex();
	xSemaphoreTake(s_file_lock, portMAX_DELAY);
	esp_err_t err = ESP_FAIL;
	FILE *f = fopen(CAN_EMU_TMP_PATH, "wb");
	if (f) {
		size_t len = strlen(json);
		bool ok = fwrite(json, 1, len, f) == len;
		ok = ok && fflush(f) == 0;
		if (ok) fsync(fileno(f));
		fclose(f);
		if (ok) {
			remove(CAN_EMU_PATH);
			ok = rename(CAN_EMU_TMP_PATH, CAN_EMU_PATH) == 0;
		}
		if (ok) err = ESP_OK;
		else remove(CAN_EMU_TMP_PATH);
	}
	xSemaphoreGive(s_file_lock);
	return err;
}

/* An ID the dash itself must never send is a refusal at store time, not a
 * surprise later. The bitrate and clashes are live conditions, not errors. */
static bool _policy_ok(const cemu_model_t *m, char *err, size_t n) {
	for (int d = 0; d < m->n_devices; d++)
		for (int i = 0; i < m->devices[d].n_send; i++) {
			const cemu_send_t *s = &m->devices[d].send[i];
			const char *why = can_tx_refused_reason(s->can_id, s->extd);
			if (why) {
				snprintf(err, n, "%s, frame 0x%lX: %s", m->devices[d].name,
				         (unsigned long)s->can_id, why);
				return false;
			}
		}
	return true;
}

/* ── apply (LVGL task) ─────────────────────────────────────────────────── */

static void _register_signals(void) {
	if (!s_m) return;
	for (int d = 0; d < s_m->n_devices; d++)
		for (int l = 0; l < s_m->devices[d].n_listen; l++) {
			const cemu_listen_t *ls = &s_m->devices[d].listen[l];
			for (int c = 0; c < ls->n_channels; c++) {
				const cemu_channel_t *ch = &ls->channels[c];
				int16_t idx = signal_register_with_source(
					ch->name, ls->can_id, ch->bit_start, ch->bit_length,
					ch->scale, ch->offset, ch->is_signed, ch->endian,
					ch->unit, SIGNAL_SOURCE_CAN);
				if (idx >= 0) signal_set_mux(idx, 0, 0, 0);
			}
		}
}

static bool _refresh_filter_ids(void) {
	uint32_t ids[CEMU_FILTER_MAX];
	int n = 0;
	bool ext = false;
	for (int d = 0; s_m && d < s_m->n_devices; d++) {
		if (!s_m->devices[d].enabled) continue;
		for (int i = 0; i < s_m->devices[d].n_send && n < CEMU_FILTER_MAX; i++) {
			if (s_m->devices[d].send[i].extd) ext = true;
			ids[n++] = s_m->devices[d].send[i].can_id;
		}
	}
	bool changed = n != s_filter_n || ext != s_filter_ext ||
	               memcmp(ids, s_filter_ids, (size_t)n * sizeof(ids[0])) != 0;
	memcpy(s_filter_ids, ids, (size_t)n * sizeof(ids[0]));
	s_filter_n = n;
	s_filter_ext = ext;
	return changed;
}

static void _apply_text(const char *json) {
	cemu_model_t *next = _model_alloc();
	if (!next) { ESP_LOGE(TAG, "no memory for the device model"); return; }
	char err[160];
	if (!cemu_parse(json, next, err, sizeof(err))) {
		ESP_LOGW(TAG, "stored devices not loaded: %s", err);
		memset(next, 0, sizeof(*next));
	}
	uint32_t now = lv_tick_get();
	_latch_load(next);
	for (int d = 0; d < next->n_devices; d++) {
		cemu_device_t *nd = &next->devices[d];
		nd->start_ms = now;
		nd->status = nd->enabled ? CEMU_ST_WAITING : CEMU_ST_OFF;
		if (!s_m) continue;
		for (int o = 0; o < s_m->n_devices; o++) {
			const cemu_device_t *od = &s_m->devices[o];
			if (strcmp(od->id, nd->id)) continue;
			/* Same device edited: keep its latches, and don't make a device
			 * that was already on the bus listen again before resuming. */
			for (int c = 0; c < nd->n_controls; c++)
				for (int k = 0; k < od->n_controls; k++)
					if (!strcmp(nd->controls[c].id, od->controls[k].id) &&
					    nd->controls[c].kind == CEMU_KIND_LATCH &&
					    od->controls[k].kind == CEMU_KIND_LATCH)
						nd->controls[c].latched = od->controls[k].latched;
			if (od->enabled && nd->enabled && od->status == CEMU_ST_SENDING)
				nd->start_ms = now - LISTEN_FIRST_MS;
			nd->clash_id = od->clash_id;
			nd->clash_ms = od->clash_ms;
		}
	}
	cemu_model_t *old = s_m;
	s_m = next;
	s_generation++;
	free(old);
	memset(s_tests, 0, sizeof(s_tests));
	_register_signals();
	/* A filter rebuild restarts the TWAI driver (~150 ms). At boot the
	 * dashboard rebuilds it itself once the layout's signals are in, so only a
	 * change applied later (a save from the editor) does it here. */
	if (_refresh_filter_ids() && s_timer) reconfigure_can_filter();
	ESP_LOGI(TAG, "%u CAN device(s) loaded", (unsigned)s_m->n_devices);
}

static void _apply_async(void *arg) {
	char *json = arg;
	_apply_text(json);
	free(json);
}

esp_err_t can_emu_store(const char *json, char *err, size_t n) {
	if (err && n) err[0] = '\0';
	if (!json || strlen(json) > CAN_EMU_MAX_BYTES) {
		snprintf(err, n, "That's more than %d kB.", CAN_EMU_MAX_BYTES / 1024);
		return ESP_ERR_INVALID_SIZE;
	}
	cemu_model_t *check = _model_alloc();
	if (!check) { snprintf(err, n, "The dash is out of memory."); return ESP_ERR_NO_MEM; }
	bool ok = cemu_parse(json, check, err, n) && _policy_ok(check, err, n);
	free(check);
	if (!ok) return ESP_ERR_INVALID_ARG;
	if (_write_file(json) != ESP_OK) {
		snprintf(err, n, "Couldn't write the file to the dash's storage.");
		return ESP_FAIL;
	}
	char *copy = heap_caps_malloc(strlen(json) + 1, MALLOC_CAP_SPIRAM);
	if (!copy) copy = malloc(strlen(json) + 1);
	if (!copy) { snprintf(err, n, "The dash is out of memory."); return ESP_ERR_NO_MEM; }
	strcpy(copy, json);
	rdm_async_call(_apply_async, copy);
	return ESP_OK;
}

/* Edit the stored file as JSON and store it back (LVGL task callers). */
static esp_err_t _edit_file(void (*edit)(cJSON *devices, void *user), void *user,
                            char *err, size_t n) {
	char *text = can_emu_read_file();
	cJSON *root = text ? cJSON_Parse(text) : NULL;
	free(text);
	if (!root) root = cJSON_CreateObject();
	cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
	if (!cJSON_IsArray(devs)) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, "devices");
		devs = cJSON_AddArrayToObject(root, "devices");
	}
	if (!cJSON_GetObjectItemCaseSensitive(root, "v")) cJSON_AddNumberToObject(root, "v", 1);
	edit(devs, user);
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!out) return ESP_ERR_NO_MEM;
	char local[160];
	esp_err_t e = can_emu_store(out, err ? err : local, err ? n : sizeof(local));
	cJSON_free(out);
	return e;
}

typedef struct { cJSON *dev; } add_ctx_t;
static void _edit_add(cJSON *devs, void *user) {
	add_ctx_t *a = user;
	cJSON_AddItemToArray(devs, a->dev);
	a->dev = NULL;
}

esp_err_t can_emu_add_template(const char *template_id, const char *variant,
                               char *err, size_t n) {
	/* A unique id: the template's own, or with _2, _3… */
	char *probe = cemu_template_device(can_emu_templates(), template_id, variant, NULL, err, n);
	if (!probe) return ESP_ERR_NOT_FOUND;
	cJSON *dev = cJSON_Parse(probe);
	cJSON_free(probe);
	if (!dev) return ESP_ERR_NO_MEM;
	const cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev, "id");
	char base[CEMU_ID_LEN] = "device";
	if (cJSON_IsString(jid)) _copy(base, sizeof(base), jid->valuestring);
	char id[CEMU_ID_LEN];
	_copy(id, sizeof(id), base);
	for (int k = 2; s_m && k < 10; k++) {
		bool taken = false;
		for (int d = 0; d < s_m->n_devices; d++)
			if (!strcmp(s_m->devices[d].id, id)) taken = true;
		if (!taken) break;
		char trimmed[CEMU_ID_LEN - 3];
		_copy(trimmed, sizeof(trimmed), base);
		snprintf(id, sizeof(id), "%s_%d", trimmed, k);
	}
	/* The same box twice would be two devices sending one ID. Say which one is
	 * already there, before the file-level check says it less helpfully. */
	cJSON *fr;
	cJSON_ArrayForEach(fr, cJSON_GetObjectItemCaseSensitive(dev, "send")) {
		const cJSON *cid = cJSON_GetObjectItemCaseSensitive(fr, "can_id");
		for (int d = 0; s_m && cJSON_IsNumber(cid) && d < s_m->n_devices; d++)
			for (int i = 0; i < s_m->devices[d].n_send; i++)
				if (s_m->devices[d].send[i].can_id == (uint32_t)cid->valuedouble) {
					snprintf(err, n, "%s already sends 0x%lX. Remove it, or add the other box.",
					         s_m->devices[d].name, (unsigned long)s_m->devices[d].send[i].can_id);
					cJSON_Delete(dev);
					return ESP_ERR_INVALID_STATE;
				}
	}
	cJSON_ReplaceItemInObjectCaseSensitive(dev, "id", cJSON_CreateString(id));
	cJSON_DeleteItemFromObjectCaseSensitive(dev, "enabled");
	cJSON_AddBoolToObject(dev, "enabled", true);
	if (variant && variant[0] && !cJSON_GetObjectItemCaseSensitive(dev, "variant"))
		cJSON_AddStringToObject(dev, "variant", variant);   /* the editor's "Box A" */
	add_ctx_t ctx = { dev };
	esp_err_t e = _edit_file(_edit_add, &ctx, err, n);
	if (ctx.dev) cJSON_Delete(ctx.dev);
	return e;
}

typedef struct { const char *id; int op; bool on; } dev_ctx_t;   /* op 0 = enable, 1 = remove */
static void _edit_dev(cJSON *devs, void *user) {
	dev_ctx_t *c = user;
	int i = 0;
	cJSON *e;
	cJSON_ArrayForEach(e, devs) {
		const cJSON *jid = cJSON_GetObjectItemCaseSensitive(e, "id");
		if (cJSON_IsString(jid) && !strcmp(jid->valuestring, c->id)) {
			if (c->op == 1) { cJSON_DeleteItemFromArray(devs, i); return; }
			cJSON_DeleteItemFromObjectCaseSensitive(e, "enabled");
			cJSON_AddBoolToObject(e, "enabled", c->on);
			return;
		}
		i++;
	}
}

esp_err_t can_emu_set_enabled(const char *device_id, bool enabled) {
	dev_ctx_t c = { device_id, 0, enabled };
	return _edit_file(_edit_dev, &c, NULL, 0);
}

esp_err_t can_emu_remove(const char *device_id) {
	dev_ctx_t c = { device_id, 1, false };
	return _edit_file(_edit_dev, &c, NULL, 0);
}

/* ── controls ──────────────────────────────────────────────────────────── */

static cemu_control_t *_resolve(const char *ref, cemu_device_t **dev, int *state) {
	int d, c, s;
	if (!s_m || !ref || !ref[0] || !cemu_resolve(s_m, ref, &d, &c, &s)) return NULL;
	if (dev) *dev = &s_m->devices[d];
	if (state) *state = s;
	return &s_m->devices[d].controls[c];
}

/* The frames that carry this control go out on the next tick, not the next
 * period — a button should reach the ECU within 10 ms. */
static void _kick(cemu_device_t *d, const cemu_control_t *c) {
	uint8_t ci = (uint8_t)(c - d->controls);
	uint32_t now = lv_tick_get();
	for (int i = 0; i < d->n_send; i++)
		for (int f = 0; f < d->send[i].n_fields; f++)
			if (d->send[i].fields[f].src == CEMU_SRC_CONTROL &&
			    d->send[i].fields[f].control == ci)
				d->send[i].due_ms = now;
}

bool can_emu_ref_exists(const char *ref) { return _resolve(ref, NULL, NULL) != NULL; }

bool can_emu_ref_is_latch(const char *ref) {
	const cemu_control_t *c = _resolve(ref, NULL, NULL);
	return c && c->kind == CEMU_KIND_LATCH;
}

bool can_emu_press(const char *ref) {
	cemu_device_t *d;
	int st;
	cemu_control_t *c = _resolve(ref, &d, &st);
	if (!c) return false;
	cemu_press(s_m, c, st);
	if (c->kind == CEMU_KIND_LATCH) _latch_save(d, c);
	_kick(d, c);
	return true;
}

void can_emu_release(const char *ref) {
	cemu_device_t *d;
	int st;
	cemu_control_t *c = _resolve(ref, &d, &st);
	if (!c || c->kind == CEMU_KIND_LATCH) return;
	cemu_release(c, st);
	_kick(d, c);
}

bool can_emu_set_latch(const char *ref, bool on) {
	cemu_device_t *d;
	cemu_control_t *c = _resolve(ref, &d, NULL);
	if (!c || c->kind != CEMU_KIND_LATCH) return false;
	if (c->latched != (on ? 1 : 0)) {
		cemu_set_latch(c, on);
		_latch_save(d, c);
		_kick(d, c);
	}
	return true;
}

bool can_emu_is_active(const char *ref) {
	int st;
	cemu_control_t *c = _resolve(ref, NULL, &st);
	if (!c) return false;
	uint8_t now = cemu_control_state(c);
	return st < 0 ? now != 0 : now == (uint8_t)st;
}

bool can_emu_test_hold(const char *ref, bool held) {
	cemu_device_t *d;
	int st;
	cemu_control_t *c = _resolve(ref, &d, &st);
	if (!c) return false;
	if (c->kind == CEMU_KIND_LATCH) {
		if (held) can_emu_press(ref);
		return true;
	}
	int free_i = -1;
	for (int i = 0; i < MAX_TEST_HOLDS; i++) {
		if (s_tests[i].ref[0] && !strcmp(s_tests[i].ref, ref)) {
			if (held) s_tests[i].until_ms = lv_tick_get() + TEST_HOLD_MS;
			else { s_tests[i].ref[0] = '\0'; can_emu_release(ref); }
			return true;
		}
		if (!s_tests[i].ref[0] && free_i < 0) free_i = i;
	}
	if (!held) return true;
	if (free_i < 0) return false;
	_copy(s_tests[free_i].ref, sizeof(s_tests[free_i].ref), ref);
	s_tests[free_i].until_ms = lv_tick_get() + TEST_HOLD_MS;
	return can_emu_press(ref);
}

void can_emu_release_all(void) {
	if (!s_m) return;
	cemu_release_all(s_m);
	memset(s_tests, 0, sizeof(s_tests));
	uint32_t now = lv_tick_get();
	for (int d = 0; d < s_m->n_devices; d++)
		for (int i = 0; i < s_m->devices[d].n_send; i++) s_m->devices[d].send[i].due_ms = now;
}

/* ── the tick ──────────────────────────────────────────────────────────── */

static void _set_status(cemu_device_t *d, uint8_t st, const char *reason) {
	if (d->status != st || (reason && strcmp(d->reason, reason))) {
		if (st == CEMU_ST_REFUSED && reason)
			ESP_LOGW(TAG, "%s stopped: %s", d->name, reason);
		else if (st == CEMU_ST_SENDING && d->status != CEMU_ST_SENDING && d->status != CEMU_ST_PAUSED)
			ESP_LOGI(TAG, "%s is on the bus", d->name);
	}
	d->status = st;
	_copy(d->reason, sizeof(d->reason), reason);
}

static void _tick_device(cemu_device_t *d, uint32_t now, bool suspended) {
	char why[112];
	if (!d->enabled) { _set_status(d, CEMU_ST_OFF, NULL); return; }
	if (suspended) { _set_status(d, CEMU_ST_PAUSED, "The dash's bus scan has the CAN bus."); return; }
	uint16_t dash = _dash_baud_k();
	if (d->bitrate_k && dash && dash != d->bitrate_k) {
		snprintf(why, sizeof(why), "Needs the bus at %u kbit/s; this dash is set to %u.",
		         (unsigned)d->bitrate_k, (unsigned)dash);
		_set_status(d, CEMU_ST_REFUSED, why);
		return;
	}
	if (d->clash_ms) {
		if ((uint32_t)(now - d->clash_ms) < CLASH_QUIET_MS) {
			snprintf(why, sizeof(why), "Something else is already sending 0x%lX. Is the real "
			         "device fitted? Use another box or ID.", (unsigned long)d->clash_id);
			_set_status(d, CEMU_ST_REFUSED, why);
			return;
		}
		d->clash_ms = 0;
		d->start_ms = now;                         /* listen again before resuming */
	}
	if ((uint32_t)(now - d->start_ms) < LISTEN_FIRST_MS) {
		_set_status(d, CEMU_ST_WAITING, "Listening before it starts.");
		return;
	}
	if (d->backoff_until_ms) {
		if ((uint32_t)(d->backoff_until_ms - now) <= BACKOFF_MS) {
			_set_status(d, CEMU_ST_PAUSED, "Nothing on the bus is answering. Trying again shortly.");
			return;
		}
		d->backoff_until_ms = 0;
		d->consec_fail = 0;
	}
	_set_status(d, CEMU_ST_SENDING, NULL);

	for (int i = 0; i < d->n_send; i++) {
		cemu_send_t *s = &d->send[i];
		if ((int32_t)(now - s->due_ms) < 0 && (uint32_t)(s->due_ms - now) <= s->every_ms)
			continue;
		uint8_t frame[8];
		uint8_t dlc = cemu_compose(d, s, _channel_value, NULL, frame);
		esp_err_t e = can_try_transmit_frame_ext(s->can_id, s->extd, frame, dlc, true);
		memcpy(s->last, frame, 8);
		/* Next slot from the schedule, not from now — unless we fell a whole
		 * period behind (a stall), in which case start again from now. */
		s->due_ms += s->every_ms;
		if ((uint32_t)(now - s->due_ms) < 0x80000000u && (uint32_t)(now - s->due_ms) >= s->every_ms)
			s->due_ms = now + s->every_ms;
		if (e == ESP_OK) {
			s->sent++;
			d->consec_fail = 0;
			d->fail_logged = false;
		} else {
			s->failed++;
			if (++d->consec_fail >= FAIL_BURST) {
				d->backoff_until_ms = now + BACKOFF_MS;
				/* Once per streak: a bench dash with nothing to ACK would
				 * otherwise say this every three seconds forever. */
				if (!d->fail_logged)
					ESP_LOGW(TAG, "%s: %u sends in a row failed - pausing %d ms at a time until something answers",
					         d->name, (unsigned)d->consec_fail, BACKOFF_MS);
				d->fail_logged = true;
				break;
			}
		}
	}
}

static void _tick_legacy(uint32_t now, bool suspended) {
	uint32_t done[CEMU_MAX_LEGACY];
	int n_done = 0;
	for (int i = 0; i < CEMU_MAX_LEGACY; i++) {
		cemu_legacy_t *l = &s_legacy[i];
		if (!l->owner) continue;
		bool due = s_legacy_now[i] || l->off_burst ||
		           (l->on && l->rate_hz && (int32_t)(now - s_legacy_due[i]) >= 0);
		if (!due) continue;
		s_legacy_now[i] = false;
		if (l->on && l->rate_hz) s_legacy_due[i] = now + 1000u / l->rate_hz;
		if (l->off_burst) l->off_burst--;
		bool sent = false;
		for (int k = 0; k < n_done; k++) if (done[k] == l->can_id) sent = true;
		if (!sent && !suspended) {
			uint8_t frame[8];
			cemu_legacy_compose(s_legacy, CEMU_MAX_LEGACY, l->can_id, frame);
			/* Not single shot: a button's OFF frame is the one that must land. */
			can_try_transmit_frame_ext(l->can_id, false, frame, 8, false);
			done[n_done++] = l->can_id;
		}
		if (l->owner == &s_legacy_gone && !l->off_burst) memset(l, 0, sizeof(*l));
	}
}

static void _tick(lv_timer_t *t) {
	(void)t;
	uint32_t now = lv_tick_get();
	bool suspended = can_is_suspended();
	for (int i = 0; i < MAX_TEST_HOLDS; i++)
		if (s_tests[i].ref[0] && (int32_t)(now - s_tests[i].until_ms) >= 0) {
			char ref[sizeof(s_tests[i].ref)];
			_copy(ref, sizeof(ref), s_tests[i].ref);
			s_tests[i].ref[0] = '\0';
			can_emu_release(ref);
		}
	for (int d = 0; s_m && d < s_m->n_devices; d++)
		_tick_device(&s_m->devices[d], now, suspended);
	_tick_legacy(now, suspended);
}

void can_emu_on_can_frame(uint32_t id, bool extd) {
	if (!s_m) return;
	for (int d = 0; d < s_m->n_devices; d++) {
		cemu_device_t *dv = &s_m->devices[d];
		if (!dv->enabled) continue;
		for (int i = 0; i < dv->n_send; i++)
			if (dv->send[i].can_id == id && dv->send[i].extd == extd) {
				dv->clash_id = id;
				dv->clash_ms = lv_tick_get();
				if (!dv->clash_ms) dv->clash_ms = 1;
				return;
			}
	}
}

int can_emu_filter_ids(uint32_t *ids, int max, bool *has_ext) {
	int n = s_filter_n < max ? s_filter_n : max;
	memcpy(ids, s_filter_ids, (size_t)n * sizeof(ids[0]));
	if (has_ext) *has_ext = s_filter_ext;
	return n;
}

void can_emu_init(void) {
	if (!s_file_lock) s_file_lock = xSemaphoreCreateMutex();
	if (!s_m) {
		char *text = can_emu_read_file();
		_apply_text(text ? text : "{}");
		free(text);
	} else {
		_register_signals();
	}
	if (!s_timer) s_timer = lv_timer_create(_tick, TICK_MS, NULL);
}

/* ── status ────────────────────────────────────────────────────────────── */

static const char *_status_word(uint8_t st) {
	switch (st) {
		case CEMU_ST_SENDING: return "sending";
		case CEMU_ST_WAITING: return "waiting";
		case CEMU_ST_PAUSED:  return "paused";
		case CEMU_ST_REFUSED: return "refused";
		default:              return "off";
	}
}

const struct cemu_model_s *can_emu_model(void) { return s_m; }
uint32_t can_emu_generation(void) { return s_generation; }

void can_emu_summary(can_emu_summary_t *out) {
	memset(out, 0, sizeof(*out));
	for (int d = 0; s_m && d < s_m->n_devices; d++) {
		const cemu_device_t *dv = &s_m->devices[d];
		out->devices++;
		if (dv->enabled) out->enabled++;
		if (dv->status == CEMU_ST_SENDING) out->sending++;
		if (dv->status == CEMU_ST_REFUSED) {
			if (!out->problems) _copy(out->first_problem, sizeof(out->first_problem), dv->reason);
			out->problems++;
		}
	}
}

cJSON *can_emu_status_json(void) {
	cJSON *arr = cJSON_CreateArray();
	for (int d = 0; s_m && d < s_m->n_devices; d++) {
		const cemu_device_t *dv = &s_m->devices[d];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "id", dv->id);
		cJSON_AddStringToObject(o, "name", dv->name);
		cJSON_AddStringToObject(o, "status", _status_word(dv->status));
		if (dv->reason[0]) cJSON_AddStringToObject(o, "reason", dv->reason);
		cJSON *fr = cJSON_AddArrayToObject(o, "frames");
		for (int i = 0; i < dv->n_send; i++) {
			const cemu_send_t *s = &dv->send[i];
			cJSON *f = cJSON_CreateObject();
			/* What it says right now — even while stopped or backing off. */
			char hex[3 * 8 + 1] = "";
			uint8_t now[8];
			uint8_t dlc = cemu_peek(dv, s, _channel_value, NULL, now);
			for (int b = 0; b < dlc; b++)
				snprintf(hex + b * 2, sizeof(hex) - (size_t)b * 2, "%02X", now[b]);
			cJSON_AddNumberToObject(f, "can_id", s->can_id);
			cJSON_AddNumberToObject(f, "sent", s->sent);
			cJSON_AddNumberToObject(f, "failed", s->failed);
			cJSON_AddStringToObject(f, "bytes", hex);
			cJSON_AddItemToArray(fr, f);
		}
		cJSON *cs = cJSON_AddObjectToObject(o, "controls");
		for (int c = 0; c < dv->n_controls; c++)
			cJSON_AddStringToObject(cs, dv->controls[c].id,
			                        dv->controls[c].states[cemu_control_state(&dv->controls[c])]);
		cJSON *ch = cJSON_AddObjectToObject(o, "channels");
		for (int l = 0; l < dv->n_listen; l++)
			for (int c = 0; c < dv->listen[l].n_channels; c++) {
				const char *name = dv->listen[l].channels[c].name;
				int16_t idx = signal_find_by_name(name);
				signal_t *sg = idx >= 0 ? signal_get_by_index((uint16_t)idx) : NULL;
				if (sg && !sg->is_stale && sg->last_update_ms)
					cJSON_AddNumberToObject(ch, name, sg->current_value);
				else
					cJSON_AddNullToObject(ch, name);
			}
		cJSON_AddItemToArray(arr, o);
	}
	return arr;
}

/* ── requests (HTTP and USB serial share these) ────────────────────────── */

bool can_emu_request(const cJSON *req, const char *text, char *err, size_t n) {
	if (err && n) err[0] = '\0';
	if (!cJSON_IsObject(req)) { snprintf(err, n, "That isn't valid JSON."); return false; }
	const cJSON *ja = cJSON_GetObjectItemCaseSensitive(req, "action");
	if (!cJSON_IsString(ja)) {
		/* A whole file: validated here, applied on the LVGL task. */
		char *owned = NULL;
		if (!text) owned = cJSON_PrintUnformatted(req);
		bool ok = can_emu_store(text ? text : owned, err, n) == ESP_OK;
		if (owned) cJSON_free(owned);
		return ok;
	}
	const char *act = ja->valuestring;
	const cJSON *j = cJSON_GetObjectItemCaseSensitive(req, "control");
	const char *ref = cJSON_IsString(j) ? j->valuestring : "";
	j = cJSON_GetObjectItemCaseSensitive(req, "device");
	const char *dev = cJSON_IsString(j) ? j->valuestring : NULL;
	if (!rdm_lvgl_lock(2000)) { snprintf(err, n, "The dash is busy. Try again."); return false; }
	bool ok = true;
	if (!strcmp(act, "test")) {
		ok = can_emu_test_hold(ref, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "held")));
		if (!ok) snprintf(err, n, "There's no control \"%s\".", ref);
	} else if (!strcmp(act, "set")) {
		ok = can_emu_set_latch(ref, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "on")));
		if (!ok) snprintf(err, n, "\"%s\" isn't an on/off control.", ref);
	} else if (!strcmp(act, "add")) {
		const cJSON *t = cJSON_GetObjectItemCaseSensitive(req, "template");
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, "variant");
		ok = can_emu_add_template(cJSON_IsString(t) ? t->valuestring : NULL,
		                          cJSON_IsString(v) ? v->valuestring : NULL, err, n) == ESP_OK;
	} else if (!strcmp(act, "enable")) {
		ok = dev && can_emu_set_enabled(dev, cJSON_IsTrue(
		         cJSON_GetObjectItemCaseSensitive(req, "enabled"))) == ESP_OK;
		if (!ok) snprintf(err, n, "Couldn't change that device.");
	} else if (!strcmp(act, "remove")) {
		ok = dev && can_emu_remove(dev) == ESP_OK;
		if (!ok) snprintf(err, n, "Couldn't remove that device.");
	} else if (!strcmp(act, "release_all")) {
		can_emu_release_all();
	} else {
		ok = false;
		snprintf(err, n, "Unknown action \"%s\".", act);
	}
	rdm_lvgl_unlock();
	return ok;
}

cJSON *can_emu_report(bool ok, const char *err, bool templates) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "ok", ok);
	if (err && err[0]) cJSON_AddStringToObject(resp, "error", err);
	cJSON_AddNumberToObject(resp, "bitrate_k", _dash_baud_k());
	char *file = can_emu_read_file();
	cJSON *fj = file ? cJSON_Parse(file) : NULL;
	free(file);
	cJSON_AddItemToObject(resp, "file", fj ? fj : cJSON_CreateObject());
	if (rdm_lvgl_lock(1200)) {
		cJSON_AddItemToObject(resp, "status", can_emu_status_json());
		rdm_lvgl_unlock();
	}
	if (templates) {
		cJSON *tj = cJSON_Parse(can_emu_templates());
		if (tj) cJSON_AddItemToObject(resp, "templates", tj);
	}
	return resp;
}

/* ── old per-widget outputs ────────────────────────────────────────────── */

void can_emu_legacy_set(void *owner, uint32_t can_id, uint8_t bit_start,
                        uint8_t bit_length, uint8_t endian, uint8_t rate_hz, bool on) {
	if (!owner || !can_id) return;
	int slot = -1, spare = -1;
	for (int i = 0; i < CEMU_MAX_LEGACY; i++) {
		if (s_legacy[i].owner == owner) { slot = i; break; }
		if (!s_legacy[i].owner && spare < 0) spare = i;
	}
	if (slot < 0) {
		if (spare < 0) { ESP_LOGW(TAG, "more than %d button outputs", CEMU_MAX_LEGACY); return; }
		slot = spare;
	}
	cemu_legacy_t *l = &s_legacy[slot];
	bool was_on = l->owner == owner && l->on;
	l->owner = owner;
	l->can_id = can_id & 0x7FFu;
	l->bit_start = bit_start;
	l->bit_length = bit_length ? bit_length : 1;
	l->endian = endian;
	l->rate_hz = rate_hz > 50 ? 50 : rate_hz;
	l->on = on;
	if (on && !was_on) { s_legacy_now[slot] = true; l->off_burst = 0; s_legacy_due[slot] = lv_tick_get(); }
	if (!on && was_on) l->off_burst = LEGACY_OFF_BURST;
	if (s_timer == NULL) s_timer = lv_timer_create(_tick, TICK_MS, NULL);
}

void can_emu_legacy_forget(void *owner, bool send_off) {
	for (int i = 0; i < CEMU_MAX_LEGACY; i++) {
		cemu_legacy_t *l = &s_legacy[i];
		if (l->owner != owner) continue;
		if (send_off && (l->on || l->off_burst)) {
			l->on = false;
			if (!l->off_burst) l->off_burst = LEGACY_OFF_BURST;
			l->owner = &s_legacy_gone;
		} else if (!send_off && l->on) {
			/* A remembered latch going away for a reload: leave the bits as
			 * they are on the bus — the recreated widget re-asserts them. */
			memset(l, 0, sizeof(*l));
		} else {
			memset(l, 0, sizeof(*l));
		}
	}
}
