/*
 * web_server_channels.c — HTTP endpoints for the channel registry.
 *
 * Endpoints:
 *   GET  /api/channels             — list active channels + live state
 *   GET  /api/channels/canonical   — read-only canonical registry
 *   POST /api/channels/activate    — activate a canonical channel by id
 *   POST /api/channels/update      — patch one or more channel fields
 *   POST /api/channels/delete      — remove a channel (custom only)
 *
 * All firmware mutation endpoints use POST (no PUT/DELETE registered
 * anywhere else in the project). Bodies are JSON.
 *
 * Threading:
 *   These handlers run on the HTTP worker thread. Channel manager API
 *   calls require the LVGL lock (the manager itself runs on the LVGL
 *   task). Each mutation handler acquires rdm_lvgl_lock before touching
 *   channels, drops it before sending the response. Lock window is short
 *   — bounded by N field writes, each O(1).
 */

#include "web_server_internal.h"
#include "data/channel_manager.h"
#include "data/channel_source_apply.h"  /* shared bind-preconfig path */
#include "data/canonical_channels.h"
#include "layout/ecu_presets.h"
#include "layout/layout_manager.h"
#include "can/obd2.h"
#include "signal.h"
#include "ui/settings/preset_picker.h"  /* preconfig_items[] master catalog */
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

extern const obd2_pid_def_t OBD2_PIDS[];
extern const int OBD2_PIDS_COUNT;
extern const ecu_preset_t ECU_PRESETS[];
extern const int ECU_PRESETS_COUNT;

static const char *TAG = "web_channels";

/* Derive a signal_name from a preconfig label using the same rule as
 * config_modal.c::label_to_signal_name — uppercase, replace runs of
 * non-alnum with single underscore, trim trailing underscores. So
 * "ENGINE RPM" → "ENGINE_RPM", "WHEEL SPD FL" → "WHEEL_SPD_FL". */
static void _derive_signal_name(const char *label, char *out, size_t sz) {
	if (!label || !out || sz == 0) { if (out && sz) out[0] = '\0'; return; }
	size_t j = 0;
	for (size_t i = 0; label[i] && j < sz - 1; i++) {
		char c = label[i];
		if (isalnum((unsigned char)c))
			out[j++] = (char)toupper((unsigned char)c);
		else if (j > 0 && out[j - 1] != '_')
			out[j++] = '_';
	}
	while (j > 0 && out[j - 1] == '_') j--;
	out[j] = '\0';
}

/* Locate a preconfig_item by its (ecu, version, label) triple. The
 * bind-source endpoint uses this to resolve a picker click back to the
 * decode params it needs to write into the layout. */
static const preconfig_item_t *_find_preconfig(const char *ecu,
                                                const char *version,
                                                const char *label) {
	if (!ecu || !version || !label) return NULL;
	for (int i = 0; i < preconfig_items_count; ++i) {
		const preconfig_item_t *it = &preconfig_items[i];
		if (!it->ecu || !it->version || !it->label) continue;
		if (strcmp(it->ecu, ecu) != 0) continue;
		if (strcmp(it->version, version) != 0) continue;
		if (strcmp(it->label, label) != 0) continue;
		return it;
	}
	return NULL;
}

/* Preconfig → channel apply logic now lives in
 * data/channel_source_apply.c so the wizard's source picker and
 * /api/channels/bind-source share a single code path. */


/* Serialize one channel as JSON. Includes both user-config fields AND
 * runtime state (current_value, is_stale, last_update_ms) so the Studio
 * UI can render live values without a second endpoint. */
static cJSON *channel_to_full_json(const channel_t *c) {
	cJSON *j = cJSON_CreateObject();
	if (!j) return NULL;

	cJSON_AddStringToObject(j, "id", c->id);
	cJSON_AddStringToObject(j, "label", c->label);
	cJSON_AddNumberToObject(j, "tier", c->tier);
	cJSON_AddNumberToObject(j, "group", c->group);
	cJSON_AddStringToObject(j, "group_name", channel_group_name(c->group));
	cJSON_AddNumberToObject(j, "cardinality", c->card);
	cJSON_AddBoolToObject(j, "is_canonical", c->is_canonical);

	cJSON_AddStringToObject(j, "signal", c->signal_name);
	cJSON_AddNumberToObject(j, "signal_index", c->signal_index);
	cJSON_AddStringToObject(j, "units_native", c->units_native);
	cJSON_AddStringToObject(j, "units_display", c->units_display);
	cJSON_AddNumberToObject(j, "decimals", c->decimals);

	cJSON_AddNumberToObject(j, "min", c->min);
	cJSON_AddNumberToObject(j, "max", c->max);

	/* Thresholds — only emit when set (sentinel values omitted) */
	if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW)
		cJSON_AddNumberToObject(j, "low_warn", c->low_warn);
	if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
		cJSON_AddNumberToObject(j, "high_warn", c->high_warn);

	/* Colors — emit only if not default-color sentinel */
	if (c->color_normal != CHANNEL_USE_DEFAULT_COLOR)
		cJSON_AddNumberToObject(j, "color_normal", (int)c->color_normal);
	if (c->color_low_warn != CHANNEL_USE_DEFAULT_COLOR)
		cJSON_AddNumberToObject(j, "color_low_warn", (int)c->color_low_warn);
	if (c->color_high_warn != CHANNEL_USE_DEFAULT_COLOR)
		cJSON_AddNumberToObject(j, "color_high_warn", (int)c->color_high_warn);

	/* Live runtime state */
	cJSON_AddNumberToObject(j, "current_value", c->current_value);
	cJSON_AddBoolToObject(j, "is_stale", c->is_stale);
	cJSON_AddNumberToObject(j, "last_update_ms", c->last_update_ms);

	return j;
}

/* GET /api/channels — return all active channels with config + live state. */
static esp_err_t channels_list_handler(httpd_req_t *req) {
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	cJSON_AddNumberToObject(root, "count", channel_manager_count());
	cJSON *arr = cJSON_AddArrayToObject(root, "channels");

	for (size_t i = 0; i < channel_manager_count(); ++i) {
		channel_t *c = channel_manager_at(i);
		if (!c) continue;
		cJSON *jc = channel_to_full_json(c);
		if (jc) cJSON_AddItemToArray(arr, jc);
	}

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t e = httpd_resp_sendstr(req, json);
	cJSON_free(json);
	return e;
}

/* GET /api/channels/canonical — return the firmware-baked canonical
 * registry (for the "Add channel" picker in Studio). Static data, so
 * this can be a long response (90 entries). */
static esp_err_t channels_canonical_handler(httpd_req_t *req) {
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	cJSON_AddNumberToObject(root, "count", CANONICAL_CHANNEL_COUNT);
	cJSON *arr = cJSON_AddArrayToObject(root, "channels");

	for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; ++i) {
		const canonical_channel_def_t *def = &CANONICAL_CHANNELS[i];
		cJSON *jc = cJSON_CreateObject();
		if (!jc) continue;
		cJSON_AddStringToObject(jc, "id", def->id);
		cJSON_AddStringToObject(jc, "label", def->label);
		cJSON_AddNumberToObject(jc, "group", def->group);
		cJSON_AddStringToObject(jc, "group_name", channel_group_name(def->group));
		cJSON_AddNumberToObject(jc, "tier", def->tier);
		cJSON_AddNumberToObject(jc, "cardinality", def->card);
		cJSON_AddStringToObject(jc, "units_native", def->units_native);
		cJSON_AddStringToObject(jc, "units_display_default", def->units_display_def);
		cJSON_AddNumberToObject(jc, "decimals", def->decimals);
		cJSON_AddNumberToObject(jc, "min_default", def->min_default);
		cJSON_AddNumberToObject(jc, "max_default", def->max_default);
		if (def->low_warn != CHANNEL_THRESHOLD_UNSET_LOW)
			cJSON_AddNumberToObject(jc, "low_warn", def->low_warn);
		if (def->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
			cJSON_AddNumberToObject(jc, "high_warn", def->high_warn);
		cJSON_AddNumberToObject(jc, "color_normal", (int)def->color_normal);
		if (def->notes) cJSON_AddStringToObject(jc, "notes", def->notes);
		/* Whether this channel is currently active in the user's config */
		cJSON_AddBoolToObject(jc, "active",
			channel_manager_get(def->id) != NULL);
		cJSON_AddItemToArray(arr, jc);
	}

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t e = httpd_resp_sendstr(req, json);
	cJSON_free(json);
	return e;
}

/* ── Mutation helpers ────────────────────────────────────────────── */

/* Read body up to max-1 bytes into buf. Returns ESP_OK on success.
 * Sends a 400 response and returns ESP_FAIL otherwise. */
static esp_err_t recv_json_body(httpd_req_t *req, char *buf, size_t max) {
	int total = 0;
	while (total < (int)max - 1) {
		int ret = httpd_req_recv(req, buf + total, max - 1 - total);
		if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
		if (ret <= 0) break;
		total += ret;
	}
	if (total <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
		return ESP_FAIL;
	}
	buf[total] = '\0';
	return ESP_OK;
}

/* Send `{"ok":true}` with the channel inlined under "channel". */
static esp_err_t send_channel_ok(httpd_req_t *req, channel_t *c) {
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	cJSON_AddBoolToObject(root, "ok", true);
	if (c) {
		cJSON *jc = channel_to_full_json(c);
		if (jc) cJSON_AddItemToObject(root, "channel", jc);
	}
	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t e = httpd_resp_sendstr(req, json);
	cJSON_free(json);
	return e;
}

/* POST /api/channels/activate — body `{ "id": "rpm" }`.
 * Idempotent — re-activating an active channel returns its current state. */
static esp_err_t channels_activate_handler(httpd_req_t *req) {
	char buf[256];
	if (recv_json_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}
	cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (!cJSON_IsString(jid) || jid->valuestring[0] == '\0') {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
		return ESP_FAIL;
	}

	channel_t *c = NULL;
	if (!rdm_lvgl_lock(500)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
		return ESP_FAIL;
	}
	c = channel_manager_activate(jid->valuestring);
	rdm_lvgl_unlock();
	cJSON_Delete(root);

	if (!c) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not a canonical channel id");
		return ESP_FAIL;
	}
	return send_channel_ok(req, c);
}

/* Apply one field from the request body. Returns true if anything changed.
 * Unknown field names are silently ignored (forward compat). */
static bool apply_one_field(channel_t *c, const char *key, cJSON *val) {
	if (!key || !val) return false;

	/* Strings */
	if (!strcmp(key, "label") && cJSON_IsString(val)) {
		return channel_manager_set_label(c, val->valuestring);
	}
	if (!strcmp(key, "signal")) {
		/* Null OR empty string both mean unbind. The web UI sends null
		 * when the user clicks the red "Unbind" link (see _chEdit). */
		if (cJSON_IsNull(val)) return channel_manager_set_signal(c, "");
		if (cJSON_IsString(val)) return channel_manager_set_signal(c, val->valuestring);
		return false;
	}
	if (!strcmp(key, "units_display") && cJSON_IsString(val)) {
		return channel_manager_set_units_display(c, val->valuestring);
	}

	/* Numbers */
	if (!strcmp(key, "decimals") && cJSON_IsNumber(val)) {
		int d = (int)val->valuedouble;
		if (d < 0) d = 0;
		if (d > 3) d = 3;
		return channel_manager_set_decimals(c, (uint8_t)d);
	}
	if (!strcmp(key, "min") && cJSON_IsNumber(val)) {
		return channel_manager_set_range(c, (float)val->valuedouble, c->max);
	}
	if (!strcmp(key, "max") && cJSON_IsNumber(val)) {
		return channel_manager_set_range(c, c->min, (float)val->valuedouble);
	}

	/* Thresholds — null clears, number sets */
	int zone = -1;
	if      (!strcmp(key, "low_warn"))      zone = CHZONE_LOW_WARN;
	else if (!strcmp(key, "high_warn"))     zone = CHZONE_HIGH_WARN;
	if (zone >= 0) {
		if (cJSON_IsNull(val)) {
			return channel_manager_clear_threshold(c, (channel_zone_t)zone);
		}
		if (cJSON_IsNumber(val)) {
			return channel_manager_set_threshold(c, (channel_zone_t)zone,
			                                     (float)val->valuedouble);
		}
		return false;
	}

	/* Zone colors */
	const struct { const char *name; channel_zone_t z; } color_map[] = {
		{"color_low_warn",      CHZONE_LOW_WARN},
		{"color_normal",        CHZONE_NORMAL},
		{"color_high_warn",     CHZONE_HIGH_WARN},
	};
	for (size_t i = 0; i < sizeof(color_map)/sizeof(color_map[0]); ++i) {
		if (!strcmp(key, color_map[i].name)) {
			if (cJSON_IsNull(val)) {
				return channel_manager_set_zone_color(c, color_map[i].z,
				                                      CHANNEL_USE_DEFAULT_COLOR);
			}
			if (cJSON_IsNumber(val)) {
				return channel_manager_set_zone_color(c, color_map[i].z,
				                                      (uint32_t)val->valuedouble);
			}
		}
	}

	return false;
}

/* POST /api/channels/update — body:
 *   { "id": "rpm", "fields": { "label": "Engine RPM", "high_warn": 6800, ... } }
 *
 * Unknown fields are silently ignored. Null on a threshold/color clears it.
 * The channel is auto-activated if it's a canonical id not yet active. */
static esp_err_t channels_update_handler(httpd_req_t *req) {
	char buf[1024];
	if (recv_json_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}
	cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
	cJSON *jfields = cJSON_GetObjectItemCaseSensitive(root, "fields");
	if (!cJSON_IsString(jid) || !cJSON_IsObject(jfields)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id or fields");
		return ESP_FAIL;
	}

	if (!rdm_lvgl_lock(500)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
		return ESP_FAIL;
	}

	channel_t *c = channel_manager_get(jid->valuestring);
	if (!c && canonical_channel_exists(jid->valuestring)) {
		c = channel_manager_activate(jid->valuestring);
	}
	if (!c) {
		rdm_lvgl_unlock();
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown channel");
		return ESP_FAIL;
	}

	int applied = 0;
	cJSON *f = NULL;
	cJSON_ArrayForEach(f, jfields) {
		if (!f->string) continue;
		if (apply_one_field(c, f->string, f)) applied++;
	}
	rdm_lvgl_unlock();
	cJSON_Delete(root);

	ESP_LOGI(TAG, "update %s: %d field(s) applied", c->id, applied);
	return send_channel_ok(req, c);
}

/* POST /api/channels/delete — body `{ "id": "custom_xyz" }`.
 * Only custom channels can be deleted; canonical channels return 403
 * (the user can clear their signal binding to "disable" them). */
static esp_err_t channels_delete_handler(httpd_req_t *req) {
	char buf[256];
	if (recv_json_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}
	cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (!cJSON_IsString(jid)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
		return ESP_FAIL;
	}

	char id_copy[32];
	strncpy(id_copy, jid->valuestring, sizeof(id_copy) - 1);
	id_copy[sizeof(id_copy) - 1] = '\0';
	cJSON_Delete(root);

	if (!channel_id_is_custom(id_copy)) {
		httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
			"Canonical channels cannot be deleted; clear the signal instead");
		return ESP_FAIL;
	}

	bool ok = false;
	if (!rdm_lvgl_lock(500)) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
		return ESP_FAIL;
	}
	ok = channel_manager_delete(id_copy);
	rdm_lvgl_unlock();

	if (!ok) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Channel not found");
		return ESP_FAIL;
	}
	return send_channel_ok(req, NULL);
}

/* ── Internal source catalog ──────────────────────────────────────
 *
 * Signals the dash synthesizes itself — calculated channels (gear from
 * RPM+speed), GPIO/ADC inputs, dash telemetry. These appear in their
 * own picker section regardless of whether the signal is currently
 * registered in the registry (some only get registered after first
 * use). Picking one binds the channel to the listed signal name and
 * relies on the corresponding internal feeder to start emitting.
 *
 * Keep in lockstep with main/widgets/signal_internal.c + main/io —
 * any signal name a feeder emits via signal_inject_test_value()
 * belongs here so users can discover and pick it. */
typedef struct {
	const char *signal_name;
	const char *label;
	const char *unit;
	const char *category;       /* UI grouping: "RDM Internal" / "GPIO / ADC" / "Dash telemetry" */
	const char *setup_hint;     /* optional: where to configure if not yet emitting */
} internal_source_t;

static const internal_source_t INTERNAL_SOURCES[] = {
	/* RDM-computed */
	{ "CALCULATED_GEAR", "Calculated Gear", "",      "RDM Internal",   "Gear Setup modal" },
	{ "ODOMETER",        "Odometer",        "km",    "RDM Internal",   "Auto-accumulates from speed" },

	/* GPIO / ADC */
	{ "FUEL_SENDER_V",   "Fuel Sender (analog)", "V","GPIO / ADC",     "Fuel Sender Setup modal" },
	{ "INDICATOR_LEFT",  "Left Indicator (wired)", "","GPIO / ADC",    "Wire Input mode (Device Settings)" },
	{ "INDICATOR_RIGHT", "Right Indicator (wired)", "","GPIO / ADC",   "Wire Input mode (Device Settings)" },

	/* Dash telemetry */
	{ "FPS",             "Render FPS",      "fps",   "Dash telemetry", NULL },
	{ "CPU_PERCENT",     "CPU Load",        "%",     "Dash telemetry", NULL },
	{ "FREE_HEAP_KB",    "Free Heap",       "KB",    "Dash telemetry", NULL },
	{ "FREE_PSRAM_KB",   "Free PSRAM",      "KB",    "Dash telemetry", NULL },
	{ "UPTIME_S",        "Uptime",          "s",     "Dash telemetry", NULL },
	{ "CHIP_TEMP",       "ESP32 Chip Temp", "°C",    "Dash telemetry", NULL },
	{ "WIFI_RSSI",       "WiFi RSSI",       "dBm",   "Dash telemetry", NULL },
};
static const size_t INTERNAL_SOURCES_COUNT = sizeof(INTERNAL_SOURCES) / sizeof(INTERNAL_SOURCES[0]);

static bool _is_internal_signal_name(const char *name) {
	if (!name) return false;
	for (size_t i = 0; i < INTERNAL_SOURCES_COUNT; ++i) {
		if (strcmp(INTERNAL_SOURCES[i].signal_name, name) == 0) return true;
	}
	return false;
}

/* True if the signal name matches any ECU preset slot name (RPM, MAP,
 * THROTTLE, ...). Used to keep the Custom section free of signals that
 * belong to the ECU layer — even when they're for slots other than the
 * current channel's slot. */
static bool _is_ecu_preset_signal_name(const char *name) {
	if (!name) return false;
	for (size_t i = 0; i < ECU_SIG__COUNT; ++i) {
		const char *s = ecu_signal_slot_name((ecu_signal_slot_t)i);
		if (s && s[0] && strcmp(s, name) == 0) return true;
	}
	return false;
}

static bool _is_obd2_pid_signal_name(const char *name) {
	if (!name) return false;
	for (int i = 0; i < OBD2_PIDS_COUNT; ++i) {
		const obd2_pid_def_t *d = &OBD2_PIDS[i];
		if (d->signal_name && strcmp(d->signal_name, name) == 0) return true;
	}
	return false;
}

/* ── Source picker: ECU preset row + OBD2 PID + custom signals ──────
 *
 * Backs the channel detail's "Source" picker. Returns curated options
 * grouped by origin so the UI never has to expose "signals" as a
 * user-facing concept.
 *
 * Sections returned:
 *   ecu     — active layout's loaded ECU preset, if its row for this
 *             canonical channel's slot has a valid CAN id (rate-shaped
 *             by the slot's preset signal_name, e.g. "rpm" / "RPM" /
 *             "Engine_RPM" depending on the preset).
 *   obd2    — the J1979 mapping for this channel if one exists in
 *             canonical_channels.h's OBD2 table. Includes whether the
 *             PID is currently in the polled_pids list.
 *   custom  — every other registered signal (DBC imports, internal,
 *             user-defined CAN frames) — the catch-all.
 *
 * Live values come straight from the signal registry, so the user sees
 * which option is producing data right now. */

static void _add_live_signal_state(cJSON *out, const char *signal_name) {
	if (!out || !signal_name || !signal_name[0]) return;
	int16_t idx = signal_find_by_name(signal_name);
	if (idx < 0) {
		cJSON_AddBoolToObject(out, "exists_in_layout", false);
		return;
	}
	cJSON_AddBoolToObject(out, "exists_in_layout", true);
	signal_t *s = signal_get_by_index((uint16_t)idx);
	if (!s) return;
	cJSON_AddNumberToObject(out, "live_value", s->current_value);
	cJSON_AddBoolToObject(out, "is_stale", s->is_stale);
}

/* Read the active layout's "ecu"/"ecu_version" fields. Returns NULL
 * if the active layout has no ECU set. The caller must cJSON_Delete()
 * the returned object (it carries the parsed layout JSON the caller
 * may also want to inspect). */
static cJSON *_load_active_layout_root(void) {
	char name[64];
	if (layout_manager_get_active(name, sizeof(name)) != ESP_OK) return NULL;
	char *buf = malloc(8192);
	if (!buf) return NULL;
	size_t n = 0;
	esp_err_t e = layout_manager_read_raw(name, buf, 8192, &n);
	if (e != ESP_OK || n == 0) { free(buf); return NULL; }
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	return root;
}

/* True if the (service, pid) tuple is in the active layout's polled_pids. */
static bool _is_obd2_pid_polled(uint8_t service, uint16_t pid) {
	char name[64];
	if (layout_manager_get_active(name, sizeof(name)) != ESP_OK) return false;
	uint32_t pids[OBD2_MAX_ENABLED];
	uint8_t count = 0;
	if (ecu_preset_read_obd2_pids(name, pids, OBD2_MAX_ENABLED, &count) != ESP_OK)
		return false;
	uint32_t target = obd2_encode_pid(service, pid);
	for (uint8_t i = 0; i < count; ++i) {
		if (pids[i] == target) return true;
	}
	return false;
}

/* Make → versions tracking for non-broadcast presets that we want to
 * expose as virtual "ECUs" in the picker (OBD2, RDM-7 Internal, Custom).
 * They live alongside the real ECU_PRESETS entries so the front-end
 * picker can treat them uniformly. */
static const char *_VIRTUAL_OBD2_MAKE     = "OBD2";
static const char *_VIRTUAL_CUSTOM_MAKE   = "Custom";

static esp_err_t channels_source_options_handler(httpd_req_t *req) {
	/* Parse ?id=channel_id query */
	char query[128];
	char channel_id[32] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
		httpd_query_key_value(query, "id", channel_id, sizeof(channel_id));
	}
	if (!channel_id[0]) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
		return ESP_FAIL;
	}

	if (!rdm_lvgl_lock(500)) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
		return ESP_FAIL;
	}

	channel_t *c = channel_manager_get(channel_id);
	const char *current_signal = c ? c->signal_name : "";

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "channel_id", channel_id);
	cJSON_AddStringToObject(root, "current_signal", current_signal);

	/* What ECU make+version is the layout currently configured for? */
	cJSON *layout_root = _load_active_layout_root();
	const char *active_make = NULL, *active_version = NULL;
	if (layout_root) {
		cJSON *jmake = cJSON_GetObjectItemCaseSensitive(layout_root, "ecu");
		cJSON *jver  = cJSON_GetObjectItemCaseSensitive(layout_root, "ecu_version");
		if (cJSON_IsString(jmake)) active_make    = jmake->valuestring;
		if (cJSON_IsString(jver))  active_version = jver->valuestring;
	}

	/* Build the unified makes[] response. Each entry has its versions[]
	 * array, each version has its signals[] array of pickable rows.
	 * Three-level drilldown: Make → Version → Signal.
	 *
	 * Master source for the ECU section: preconfig_items[] in
	 * preset_picker.c (~180 entries). This is the lookup table the
	 * device firmware's preset picker uses, with full per-CAN-id signal
	 * detail (RPM, MAP, wheel speeds, brake state, accel pedal, etc.).
	 * We iterate it once and group by (ecu, version). */
	cJSON *makes_arr = cJSON_AddArrayToObject(root, "makes");

	for (int i = 0; i < preconfig_items_count; ++i) {
		const preconfig_item_t *it = &preconfig_items[i];
		if (!it->ecu || !it->version || !it->label) continue;
		/* OBD2-marker entries (obd2_pid != 0) live under the OBD2
		 * virtual make below, built dynamically from OBD2_PIDS. */
		if (it->obd2_pid != 0) continue;

		/* Find or append this make in makes_arr. */
		cJSON *make_obj = NULL;
		cJSON *iter;
		cJSON_ArrayForEach(iter, makes_arr) {
			cJSON *mk = cJSON_GetObjectItemCaseSensitive(iter, "make");
			if (cJSON_IsString(mk) && strcmp(mk->valuestring, it->ecu) == 0) {
				make_obj = iter; break;
			}
		}
		if (!make_obj) {
			make_obj = cJSON_CreateObject();
			cJSON_AddStringToObject(make_obj, "make", it->ecu);
			cJSON_AddBoolToObject(make_obj, "is_active", false);
			cJSON_AddArrayToObject(make_obj, "versions");
			cJSON_AddItemToArray(makes_arr, make_obj);
		}

		bool is_active_version = (active_make && active_version
			&& strcmp(it->ecu, active_make) == 0
			&& strcmp(it->version, active_version) == 0);
		if (is_active_version) {
			cJSON_DeleteItemFromObject(make_obj, "is_active");
			cJSON_AddBoolToObject(make_obj, "is_active", true);
		}

		cJSON *versions_arr = cJSON_GetObjectItemCaseSensitive(make_obj, "versions");

		/* Find or append this version inside the make. */
		cJSON *ver_obj = NULL;
		cJSON *viter;
		cJSON_ArrayForEach(viter, versions_arr) {
			cJSON *vn = cJSON_GetObjectItemCaseSensitive(viter, "version");
			if (cJSON_IsString(vn) && strcmp(vn->valuestring, it->version) == 0) {
				ver_obj = viter; break;
			}
		}
		if (!ver_obj) {
			ver_obj = cJSON_CreateObject();
			cJSON_AddStringToObject(ver_obj, "version", it->version);
			char display[64];
			snprintf(display, sizeof(display), "%s %s", it->ecu, it->version);
			cJSON_AddStringToObject(ver_obj, "display", display);
			cJSON_AddBoolToObject  (ver_obj, "is_active", is_active_version);
			cJSON_AddArrayToObject (ver_obj, "signals");
			cJSON_AddItemToArray(versions_arr, ver_obj);
		}

		cJSON *signals_arr = cJSON_GetObjectItemCaseSensitive(ver_obj, "signals");

		/* Build the signal row. signal_name is derived from the label
		 * (matches device firmware's preset apply path). */
		char sname[32];
		_derive_signal_name(it->label, sname, sizeof(sname));
		uint32_t can_id = (uint32_t)strtol(it->can_id, NULL, 16);

		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "kind",        "ecu");
		cJSON_AddStringToObject(o, "signal_name", sname);
		cJSON_AddStringToObject(o, "label",       it->label);
		cJSON_AddNumberToObject(o, "can_id",      can_id);
		cJSON_AddNumberToObject(o, "bit_start",   it->bit_start);
		cJSON_AddNumberToObject(o, "bit_length",  it->bit_length);
		cJSON_AddNumberToObject(o, "scale",       it->scale);
		cJSON_AddNumberToObject(o, "offset",      it->value_offset);
		cJSON_AddBoolToObject  (o, "is_signed",   it->is_signed);
		cJSON_AddNumberToObject(o, "endian",      it->endianess);
		cJSON_AddNumberToObject(o, "decimals",    it->decimals);
		cJSON_AddBoolToObject  (o, "is_current",
			strcmp(current_signal, sname) == 0);
		/* Live value when the signal is currently in the registry —
		 * works regardless of which preset's row planted it, so users
		 * see live readings on alternates too if the names happen to
		 * collide (e.g. multiple presets all using "RPM"). */
		_add_live_signal_state(o, sname);
		cJSON_AddItemToArray(signals_arr, o);
	}

	/* ── Virtual: OBD2 ────────────────────────────────────────────── */
	{
		cJSON *make_obj = cJSON_CreateObject();
		cJSON_AddStringToObject(make_obj, "make", _VIRTUAL_OBD2_MAKE);
		cJSON_AddBoolToObject  (make_obj, "is_active", false);
		cJSON *versions_arr = cJSON_AddArrayToObject(make_obj, "versions");
		cJSON *ver_obj = cJSON_CreateObject();
		cJSON_AddStringToObject(ver_obj, "version", "Standard");
		cJSON_AddStringToObject(ver_obj, "display", "OBD2 Standard (any 2008+ car)");
		cJSON_AddBoolToObject  (ver_obj, "is_active", false);
		cJSON *signals_arr = cJSON_AddArrayToObject(ver_obj, "signals");
		for (int i = 0; i < OBD2_PIDS_COUNT; ++i) {
			const obd2_pid_def_t *d = &OBD2_PIDS[i];
			if (!d->signal_name) continue;
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "kind",        "obd2");
			cJSON_AddStringToObject(o, "signal_name", d->signal_name);
			cJSON_AddStringToObject(o, "label",       d->human_name ? d->human_name : d->signal_name);
			cJSON_AddStringToObject(o, "unit",        d->unit ? d->unit : "");
			uint8_t svc = obd2_def_service(d);
			cJSON_AddNumberToObject(o, "service",     svc);
			cJSON_AddNumberToObject(o, "pid",         d->pid);
			cJSON_AddBoolToObject  (o, "polled",      _is_obd2_pid_polled(svc, d->pid));
			cJSON_AddBoolToObject  (o, "is_current",  strcmp(current_signal, d->signal_name) == 0);
			_add_live_signal_state(o, d->signal_name);
			cJSON_AddItemToArray(signals_arr, o);
		}
		cJSON_AddItemToArray(versions_arr, ver_obj);
		cJSON_AddItemToArray(makes_arr, make_obj);
	}

	/* ── Virtual: Custom (DBC + user-defined signals not classified) ─ */
	{
		cJSON *make_obj = cJSON_CreateObject();
		cJSON_AddStringToObject(make_obj, "make", _VIRTUAL_CUSTOM_MAKE);
		cJSON_AddBoolToObject  (make_obj, "is_active", false);
		cJSON *versions_arr = cJSON_AddArrayToObject(make_obj, "versions");
		cJSON *ver_obj = cJSON_CreateObject();
		cJSON_AddStringToObject(ver_obj, "version", "User-defined");
		cJSON_AddStringToObject(ver_obj, "display", "Custom CAN + DBC imports");
		cJSON_AddBoolToObject  (ver_obj, "is_active", false);
		cJSON *signals_arr = cJSON_AddArrayToObject(ver_obj, "signals");
		uint16_t sig_n = signal_get_count();
		for (uint16_t i = 0; i < sig_n; ++i) {
			signal_t *s = signal_get_by_index(i);
			if (!s) continue;
			if (_is_ecu_preset_signal_name(s->name)) continue;
			if (_is_obd2_pid_signal_name(s->name))   continue;
			if (_is_internal_signal_name(s->name))   continue;
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "kind",        "custom");
			cJSON_AddStringToObject(o, "signal_name", s->name);
			cJSON_AddStringToObject(o, "label",       s->name);
			cJSON_AddStringToObject(o, "unit",        s->unit);
			cJSON_AddNumberToObject(o, "live_value",  s->current_value);
			cJSON_AddBoolToObject  (o, "is_stale",    s->is_stale);
			cJSON_AddBoolToObject  (o, "is_current",  strcmp(current_signal, s->name) == 0);
			cJSON_AddBoolToObject  (o, "exists_in_layout", true);
			cJSON_AddItemToArray(signals_arr, o);
		}
		cJSON_AddItemToArray(versions_arr, ver_obj);
		cJSON_AddItemToArray(makes_arr, make_obj);
	}

	if (layout_root) cJSON_Delete(layout_root);
	rdm_lvgl_unlock();

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t e = httpd_resp_sendstr(req, json);
	cJSON_free(json);
	return e;
}

/* POST /api/channels/bind-source
 *
 * Body shapes (discriminated by `source_type`):
 *   { channel_id, source_type:"ecu_preset", make, version }
 *      → rewrite the layout's signals[] entry for the channel's
 *        canonical signal name using that preset's slot row, then
 *        bind the channel to that name.
 *   { channel_id, source_type:"obd2",       obd2_service, obd2_pid, signal_name }
 *      → append the PID to polled_pids and restart OBD2 polling (so
 *        the signal gets registered), then bind.
 *   { channel_id, source_type:"custom",     signal_name }
 *      → just bind. Signal must already exist in the registry.
 *
 * Backwards-compat: a body without source_type but with signal_name +
 * optional obd2_service/obd2_pid still works — treated as obd2/custom
 * automatically based on whether the PID fields are present.
 */
static esp_err_t channels_bind_source_handler(httpd_req_t *req) {
	char buf[512];
	if (recv_json_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}
	cJSON *jid    = cJSON_GetObjectItemCaseSensitive(root, "channel_id");
	cJSON *jtype  = cJSON_GetObjectItemCaseSensitive(root, "source_type");
	cJSON *jmake  = cJSON_GetObjectItemCaseSensitive(root, "make");
	cJSON *jver   = cJSON_GetObjectItemCaseSensitive(root, "version");
	cJSON *jlabel = cJSON_GetObjectItemCaseSensitive(root, "label");
	cJSON *jsig   = cJSON_GetObjectItemCaseSensitive(root, "signal_name");
	cJSON *jsvc   = cJSON_GetObjectItemCaseSensitive(root, "obd2_service");
	cJSON *jpid   = cJSON_GetObjectItemCaseSensitive(root, "obd2_pid");
	if (!cJSON_IsString(jid)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing channel_id");
		return ESP_FAIL;
	}

	char channel_id[32];
	strncpy(channel_id, jid->valuestring, sizeof(channel_id) - 1);
	channel_id[sizeof(channel_id) - 1] = '\0';

	const char *source_type = cJSON_IsString(jtype) ? jtype->valuestring : "";
	const bool is_ecu_preset = (strcmp(source_type, "ecu_preset") == 0);
	const bool is_obd2 = (strcmp(source_type, "obd2") == 0)
		|| (!is_ecu_preset && cJSON_IsNumber(jsvc) && cJSON_IsNumber(jpid));

	char make[32]   = {0};
	char version[32] = {0};
	char preconfig_label[40] = {0};
	char signal_name[32] = {0};
	uint8_t  obd_svc = 0;
	uint16_t obd_pid = 0;

	if (is_ecu_preset) {
		if (!cJSON_IsString(jmake) || !cJSON_IsString(jver) || !cJSON_IsString(jlabel)) {
			cJSON_Delete(root);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ecu_preset needs make+version+label");
			return ESP_FAIL;
		}
		strncpy(make,    jmake->valuestring,  sizeof(make)    - 1);
		strncpy(version, jver->valuestring,   sizeof(version) - 1);
		strncpy(preconfig_label, jlabel->valuestring, sizeof(preconfig_label) - 1);
	} else {
		if (!cJSON_IsString(jsig)) {
			cJSON_Delete(root);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing signal_name");
			return ESP_FAIL;
		}
		strncpy(signal_name, jsig->valuestring, sizeof(signal_name) - 1);
		if (is_obd2) {
			obd_svc = (uint8_t)jsvc->valuedouble;
			obd_pid = (uint16_t)jpid->valuedouble;
		}
	}
	cJSON_Delete(root);

	if (!rdm_lvgl_lock(1200)) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL busy");
		return ESP_FAIL;
	}

	/* Activate the channel up-front so we always have a target. */
	channel_t *c = channel_manager_get(channel_id);
	if (!c && canonical_channel_exists(channel_id)) {
		c = channel_manager_activate(channel_id);
	}
	if (!c) {
		rdm_lvgl_unlock();
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown channel");
		return ESP_FAIL;
	}

	if (is_ecu_preset) {
		/* Look up the preconfig_item by (make, version, label) — the
		 * triple uniquely identifies a row in the master catalog. Then
		 * funnel the bind through the shared channel_apply_preconfig
		 * helper so the wizard's source picker and this endpoint stay
		 * in lockstep on what "apply a preconfig" means. */
		const preconfig_item_t *item = _find_preconfig(make, version, preconfig_label);
		if (!item) {
			rdm_lvgl_unlock();
			httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Preset row not found");
			return ESP_FAIL;
		}
		esp_err_t e = channel_apply_preconfig(c, item);
		if (e != ESP_OK) {
			rdm_lvgl_unlock();
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Apply preset failed");
			return ESP_FAIL;
		}
	} else if (is_obd2) {
		char layout[64];
		if (layout_manager_get_active(layout, sizeof(layout)) == ESP_OK) {
			uint32_t pids[OBD2_MAX_ENABLED];
			uint8_t count = 0;
			ecu_preset_read_obd2_pids(layout, pids, OBD2_MAX_ENABLED, &count);
			uint32_t enc = obd2_encode_pid(obd_svc, obd_pid);
			bool dup = false;
			for (uint8_t i = 0; i < count; ++i) {
				if (pids[i] == enc) { dup = true; break; }
			}
			if (!dup && count < OBD2_MAX_ENABLED) {
				pids[count++] = enc;
				if (ecu_preset_save_obd2_pids(layout, pids, count) == ESP_OK) {
					obd2_start(pids, count);
				}
			}
		}
		channel_manager_set_signal(c, signal_name);
		channel_manager_resolve_signals();
	} else {
		channel_manager_set_signal(c, signal_name);
		channel_manager_resolve_signals();

		/* Persist the CAN decode into the layout's signals[]. Without this
		 * the custom/DBC bind only stores the signal NAME (in channels.json)
		 * — the decode params (can_id/bits/scale/offset/...) live nowhere in
		 * the layout, so at boot _load_signals can't re-register the signal
		 * and channel_manager_resolve_signals() misses → the channel stays
		 * preserved-but-dead. The ECU path already does this via
		 * ecu_preset_write_signal_to_layout(); route custom binds through the
		 * same writer so they survive a reboot. Internal/OBD2 signals
		 * (can_id==0 or source!=CAN) have nothing to decode and are skipped —
		 * OBD2 re-registers itself at boot from polled_pids. */
		int16_t si = signal_find_by_name(signal_name);
		signal_t *s = (si >= 0) ? signal_get_by_index((uint16_t)si) : NULL;
		if (s && s->source == SIGNAL_SOURCE_CAN && s->can_id != 0) {
			char layout[64];
			if (layout_manager_get_active(layout, sizeof(layout)) == ESP_OK) {
				/* ecu_preset_write_signal_to_layout persists to disk itself
				 * (calls layout_manager_save_raw) — no double-save here. */
				ecu_preset_write_signal_to_layout(
					layout, s->name, s->can_id, s->bit_start, s->bit_length,
					s->scale, s->offset, s->is_signed, s->endian,
					s->unit[0] ? s->unit : NULL, -1);
			}
		}
	}

	rdm_lvgl_unlock();
	return send_channel_ok(req, c);
}

static const httpd_uri_t channels_source_options_uri = {
	.uri = "/api/channels/source-options",
	.method = HTTP_GET,
	.handler = channels_source_options_handler,
	.user_ctx = NULL
};

static const httpd_uri_t channels_bind_source_uri = {
	.uri = "/api/channels/bind-source",
	.method = HTTP_POST,
	.handler = channels_bind_source_handler,
	.user_ctx = NULL
};

static const httpd_uri_t channels_list_uri = {
	.uri = "/api/channels",
	.method = HTTP_GET,
	.handler = channels_list_handler,
	.user_ctx = NULL
};

static const httpd_uri_t channels_canonical_uri = {
	.uri = "/api/channels/canonical",
	.method = HTTP_GET,
	.handler = channels_canonical_handler,
	.user_ctx = NULL
};

static const httpd_uri_t channels_activate_uri = {
	.uri = "/api/channels/activate",
	.method = HTTP_POST,
	.handler = channels_activate_handler,
	.user_ctx = NULL
};

static const httpd_uri_t channels_update_uri = {
	.uri = "/api/channels/update",
	.method = HTTP_POST,
	.handler = channels_update_handler,
	.user_ctx = NULL
};

static const httpd_uri_t channels_delete_uri = {
	.uri = "/api/channels/delete",
	.method = HTTP_POST,
	.handler = channels_delete_handler,
	.user_ctx = NULL
};

void web_server_channels_register(httpd_handle_t server) {
	REGISTER_URI(server, &channels_list_uri);
	REGISTER_URI(server, &channels_canonical_uri);
	REGISTER_URI(server, &channels_activate_uri);
	REGISTER_URI(server, &channels_update_uri);
	REGISTER_URI(server, &channels_delete_uri);
	REGISTER_URI(server, &channels_source_options_uri);
	REGISTER_URI(server, &channels_bind_source_uri);
	ESP_LOGI(TAG, "channel endpoints: list, canonical, activate, update, delete, source-options, bind-source");
}
