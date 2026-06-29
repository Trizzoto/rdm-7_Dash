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
#include "data/channel_math.h"
#include "data/channel_source_apply.h"  /* shared bind-preconfig path */
#include "data/canonical_channels.h"
#include "data/unit_convert.h"
#include "layout/ecu_presets.h"
#include "layout/layout_manager.h"
#include "can/obd2.h"
#include "signal.h"
#include "ui/settings/preset_picker.h"  /* preconfig_items[] master catalog */
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

extern const obd2_pid_def_t OBD2_PIDS[];
extern const int OBD2_PIDS_COUNT;
extern const ecu_preset_t ECU_PRESETS[];
extern const int ECU_PRESETS_COUNT;

static const char *TAG = "web_channels";

/* Forward decl — defined below; used by channel_to_full_json for the
 * not-registered OBD2 provenance fallback. */
static bool _obd2_name_known(const char *name);

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

	/* Source-unit options ("what the ECU is sending") — the full convertible
	 * family computed from the canonical BASE native so e.g. °C/°F/K all appear
	 * regardless of the channel's current native. The web "Received as" dropdown
	 * renders this; absent (or 1-entry) means the unit has no alternatives. */
	{
		const canonical_channel_def_t *cdef = canonical_channel_find(c->id);
		const char *base = (cdef && cdef->units_native[0]) ? cdef->units_native
		                                                   : c->units_native;
		const char *targets[8];
		size_t nt = base[0] ? unit_convert_targets(base, targets, 8) : 0;
		if (nt > 0) {
			cJSON *su = cJSON_AddArrayToObject(j, "source_units");
			if (su) {
				cJSON_AddItemToArray(su, cJSON_CreateString(base));
				for (size_t k = 0; k < nt; ++k)
					cJSON_AddItemToArray(su, cJSON_CreateString(targets[k]));
			}
		}
	}

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

	/* CAN decode (ADR 0005) — present only when the channel owns one
	 * (can_id != 0). The Channels-tab decode editor reads these to populate
	 * the CAN ID / bit / scale inputs and writes them back via
	 * /api/channels/update {fields:{decode:{...}}}. */
	if (c->can_id != 0) {
		cJSON *d = cJSON_AddObjectToObject(j, "decode");
		if (d) {
			cJSON_AddNumberToObject(d, "can_id", (double)c->can_id);
			cJSON_AddNumberToObject(d, "bit_start", c->bit_start);
			cJSON_AddNumberToObject(d, "bit_length", c->bit_length);
			cJSON_AddNumberToObject(d, "scale", c->decode_scale);
			cJSON_AddNumberToObject(d, "offset", c->decode_offset);
			cJSON_AddBoolToObject(d, "is_signed", c->is_signed);
			cJSON_AddNumberToObject(d, "endian", c->endian);
			cJSON_AddStringToObject(d, "unit", c->decode_unit);
		}
	}

	/* Math/derived source — config block so the web form can populate, and
	 * the source classification below short-circuits to "math". Operands
	 * are typed: string = channel id, number = constant. */
	if (c->math_enabled) {
		cJSON *m = cJSON_AddObjectToObject(j, "math");
		if (m) {
			if (c->math_a_is_const) cJSON_AddNumberToObject(m, "a", c->math_a_const);
			else                    cJSON_AddStringToObject(m, "a", c->math_a);
			if (c->math_b_is_const) cJSON_AddNumberToObject(m, "b", c->math_b_const);
			else                    cJSON_AddStringToObject(m, "b", c->math_b);
			cJSON_AddNumberToObject(m, "op", c->math_op);
		}
	}

	/* Provenance — AUTHORITATIVE OBD2/CAN/internal source taken from the bound
	 * signal's registry source field (signal_t.source), NOT a name heuristic.
	 * Lets the Channels tab visibly separate OBD2 from raw CAN and suppress the
	 * CAN bit-decode editor for non-CAN channels. A channel bound to a signal
	 * not yet in the registry but owning a CAN decode is inherently "can". */
	const char *src_str;
	if (c->math_enabled) {
		src_str = "math";
	} else if (c->signal_name[0]) {
		int16_t si = signal_find_by_name(c->signal_name);
		signal_t *s = (si >= 0) ? signal_get_by_index((uint16_t)si) : NULL;
		if (s) {
			switch ((signal_source_t)s->source) {
				case SIGNAL_SOURCE_OBD2:     src_str = "obd2";     break;
				case SIGNAL_SOURCE_INTERNAL: src_str = "internal"; break;
				case SIGNAL_SOURCE_CAN:
				default:                     src_str = "can";      break;
			}
		} else {
			/* Bound by name but not in the registry — e.g. an OBD2 PID whose
			 * polling is currently off. Owning a CAN decode means CAN; else a
			 * name match against the OBD2 PID table is a safe fallback (no
			 * registered namesake to collide with) so the OBD2 badge survives. */
			if (c->can_id != 0)                    src_str = "can";
			else if (_obd2_name_known(c->signal_name)) src_str = "obd2";
			else                                   src_str = "unknown";
		}
	} else {
		src_str = (c->can_id != 0) ? "can" : "unbound";
	}
	cJSON_AddStringToObject(j, "source", src_str);

	/* Live runtime state */
	cJSON_AddNumberToObject(j, "current_value", c->current_value);
	cJSON_AddBoolToObject(j, "is_stale", c->is_stale);
	cJSON_AddNumberToObject(j, "last_update_ms", c->last_update_ms);
	/* Zone the current value falls into (tracked on the channel, only changes
	 * on a transition). Lets a test agent assert e.g. "EGT is in high_warn"
	 * after injecting a value, instead of re-deriving the threshold logic. */
	cJSON_AddStringToObject(j, "zone", channel_zone_name(c->last_zone));

	/* Display-unit conversion (units_native -> units_display). The native fields
	 * above stay authoritative for storage and threshold comparison; these
	 * mirror them in the user's chosen display unit so the channels UI can show
	 * e.g. boost in psi while the firmware keeps everything in kPa. Only emitted
	 * when the units actually differ AND a conversion is known, so unitless or
	 * identity channels add no noise and the client can fall back to the native
	 * fields. */
	if (unit_convert_supported(c->units_native, c->units_display) &&
	    strcmp(c->units_native, c->units_display) != 0) {
		cJSON_AddNumberToObject(j, "display_value",
			unit_convert(c->current_value, c->units_native, c->units_display));
		cJSON_AddNumberToObject(j, "display_min",
			unit_convert(c->min, c->units_native, c->units_display));
		cJSON_AddNumberToObject(j, "display_max",
			unit_convert(c->max, c->units_native, c->units_display));
		if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW)
			cJSON_AddNumberToObject(j, "display_low_warn",
				unit_convert(c->low_warn, c->units_native, c->units_display));
		if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
			cJSON_AddNumberToObject(j, "display_high_warn",
				unit_convert(c->high_warn, c->units_native, c->units_display));
	}

	return j;
}

/* Serializes only the heavy channel-list BUILD (cJSON tree + serialized string).
 * Several concurrent builds (Studio + mirror + a browser polling at once) race
 * for PSRAM and intermittently OOM into 500s — the stress test reproduced this.
 * We serialize the build but NOT the network send (a slow reader must not hold
 * the lock), and we WAIT (not try-lock) so a legitimate concurrent reader gets
 * served a moment later instead of a spurious 503. Created in
 * web_server_channels_register(). */
static SemaphoreHandle_t s_channels_list_mux = NULL;

/* GET /api/channels — return all active channels with config + live state. */
static esp_err_t channels_list_handler(httpd_req_t *req) {
	/* Serialize the build: wait up to 2 s for our turn; only shed with 503 if
	 * the device is genuinely saturated that long (the build itself is short). */
	if (s_channels_list_mux &&
	    xSemaphoreTake(s_channels_list_mux, pdMS_TO_TICKS(2000)) != pdTRUE)
		return web_server_send_busy(req);

	/* Build the whole JSON string while holding the lock... */
	char *json = NULL;
	cJSON *root = cJSON_CreateObject();
	if (root) {
		cJSON_AddNumberToObject(root, "count", channel_manager_count());
		cJSON *arr = cJSON_AddArrayToObject(root, "channels");
		for (size_t i = 0; i < channel_manager_count(); ++i) {
			channel_t *c = channel_manager_at(i);
			if (!c) continue;
			cJSON *jc = channel_to_full_json(c);
			if (jc) cJSON_AddItemToArray(arr, jc);
		}
		json = cJSON_PrintUnformatted(root);
		cJSON_Delete(root);
	}

	/* ...then release BEFORE the (potentially slow) send, so a slow reader can't
	 * block other channel reads for the whole send timeout. */
	if (s_channels_list_mux) xSemaphoreGive(s_channels_list_mux);

	if (!json) return web_server_send_busy(req);
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
 * Sends a 400 response and returns ESP_FAIL otherwise. (This was the
 * original loop the shared helper was promoted from — delegate now.) */
static esp_err_t recv_json_body(httpd_req_t *req, char *buf, size_t max) {
	return web_server_recv_body(req, buf, max);
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
		return web_server_send_busy(req);
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
	if (!strcmp(key, "units_native") && cJSON_IsString(val)) {
		return channel_manager_set_units_native(c, val->valuestring);
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

	/* CAN decode (ADR 0005) — `{ "decode": { can_id, bit_start, bit_length,
	 * scale, offset, is_signed, endian|is_little_endian, unit } }`. Writes the
	 * decode onto the channel (device-local, authoritative) and live-registers
	 * it so the bus decodes immediately. Missing sub-fields fall back to the
	 * channel's current values so a partial patch (e.g. just can_id) works. */
	if (!strcmp(key, "decode") && cJSON_IsObject(val)) {
		cJSON *it;
		uint32_t can_id    = c->can_id;
		uint8_t  bit_start = c->bit_start;
		uint8_t  bit_length= c->bit_length;
		float    scale     = c->decode_scale;
		float    offset    = c->decode_offset;
		bool     is_signed = c->is_signed;
		uint8_t  endian    = c->endian;
		char     unit[8];
		strncpy(unit, c->decode_unit, sizeof(unit) - 1);
		unit[sizeof(unit) - 1] = '\0';

		it = cJSON_GetObjectItemCaseSensitive(val, "can_id");
		if (cJSON_IsNumber(it)) can_id = (uint32_t)it->valuedouble;
		it = cJSON_GetObjectItemCaseSensitive(val, "bit_start");
		if (cJSON_IsNumber(it)) bit_start = (uint8_t)it->valueint;
		it = cJSON_GetObjectItemCaseSensitive(val, "bit_length");
		if (cJSON_IsNumber(it)) bit_length = (uint8_t)it->valueint;
		it = cJSON_GetObjectItemCaseSensitive(val, "scale");
		if (cJSON_IsNumber(it)) scale = (float)it->valuedouble;
		it = cJSON_GetObjectItemCaseSensitive(val, "offset");
		if (cJSON_IsNumber(it)) offset = (float)it->valuedouble;
		it = cJSON_GetObjectItemCaseSensitive(val, "is_signed");
		if (cJSON_IsBool(it)) is_signed = cJSON_IsTrue(it);
		cJSON *jen = cJSON_GetObjectItemCaseSensitive(val, "endian");
		cJSON *jle = cJSON_GetObjectItemCaseSensitive(val, "is_little_endian");
		if (cJSON_IsNumber(jen))      endian = (uint8_t)jen->valueint;
		else if (cJSON_IsBool(jle))   endian = cJSON_IsTrue(jle) ? 1 : 0;
		it = cJSON_GetObjectItemCaseSensitive(val, "unit");
		if (cJSON_IsString(it) && it->valuestring) {
			strncpy(unit, it->valuestring, sizeof(unit) - 1);
			unit[sizeof(unit) - 1] = '\0';
		}
		return channel_manager_set_decode(c, can_id, bit_start, bit_length,
		                                  scale, offset, is_signed, endian, unit,
		                                  /* persist_now */ true);
	}

	/* Math/derived source — `{ "math": { "a": <operand>, "b": <operand>,
	 * "op": 0..3 } }` configures, `{ "math": null }` clears (channel reverts
	 * to unbound). An operand is a channel-id STRING or a NUMBER constant
	 * (`{"a":"manifold_pressure","b":50,"op":1}` = MAP − 50). Validation
	 * (operands exist, no self-reference, ≥1 channel) lives in
	 * channel_math_set. */
	if (!strcmp(key, "math")) {
		if (cJSON_IsNull(val)) return channel_math_clear(c);
		if (cJSON_IsObject(val)) {
			cJSON *a  = cJSON_GetObjectItemCaseSensitive(val, "a");
			cJSON *b  = cJSON_GetObjectItemCaseSensitive(val, "b");
			cJSON *op = cJSON_GetObjectItemCaseSensitive(val, "op");
			channel_math_operand_t oa = {0}, ob = {0};
			if (cJSON_IsString(a))      oa.channel_id = a->valuestring;
			else if (cJSON_IsNumber(a)) { oa.is_const = true; oa.value = (float)a->valuedouble; }
			else return false;
			if (cJSON_IsString(b))      ob.channel_id = b->valuestring;
			else if (cJSON_IsNumber(b)) { ob.is_const = true; ob.value = (float)b->valuedouble; }
			else return false;
			uint8_t opv = cJSON_IsNumber(op) ? (uint8_t)op->valueint : 0;
			return channel_math_set(c, &oa, &ob, opv);
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
		return web_server_send_busy(req);
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
		return web_server_send_busy(req);
	}
	ok = channel_manager_delete(id_copy);
	rdm_lvgl_unlock();

	if (!ok) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Channel not found");
		return ESP_FAIL;
	}
	return send_channel_ok(req, NULL);
}

/* NOTE: the former name-heuristic classifiers (_is_internal_signal_name /
 * _is_ecu_preset_signal_name + the INTERNAL_SOURCES table) were removed — the
 * source picker's Custom/OBD2/ECU bucketing now keys off the authoritative
 * signal_t.source provenance instead of matching signal NAMES, so a raw-CAN
 * signal that shares an OBD2/ECU name no longer leaks into the wrong section. */

/* True if @p name is a standard OBD2 PID name. Used ONLY as a provenance
 * FALLBACK when the registry has NO signal of that name (e.g. a channel bound
 * to an OBD2 PID whose polling is currently off, so its signal isn't
 * registered). Safe precisely because the registry is silent: with no
 * registered signal of this name there is no CAN namesake to collide with, so
 * the name match cannot mis-attribute a CAN signal as OBD2. */
static bool _obd2_name_known(const char *name) {
	if (!name || !name[0]) return false;
	for (int i = 0; i < OBD2_PIDS_COUNT; ++i) {
		if (OBD2_PIDS[i].signal_name && strcmp(OBD2_PIDS[i].signal_name, name) == 0)
			return true;
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

/* A frozen copy of one registry signal's pickable state. The source-options
 * handler snapshots the whole registry into an array of these under a brief
 * LVGL lock, then builds the (~98 KB) response from the snapshot UNLOCKED so
 * the dashboard never freezes for the duration of the JSON build (H15). */
typedef struct {
	char    name[32];
	char    unit[8];
	uint8_t source;          /* signal_source_t */
	float   current_value;
	bool    is_stale;
} sig_snapshot_t;

/* Find a snapshot row by exact name, or NULL. */
static const sig_snapshot_t *_snap_find(const sig_snapshot_t *snap, uint16_t n,
                                        const char *name) {
	if (!snap || !name || !name[0]) return NULL;
	for (uint16_t i = 0; i < n; ++i)
		if (strcmp(snap[i].name, name) == 0) return &snap[i];
	return NULL;
}

/* Attribute live registry state to a picker row ONLY when the registered
 * signal's provenance matches the row's section (@p expected). Without this
 * gate a raw-CAN signal that happens to share an OBD2 PID name (RPM,
 * COOLANT_TEMP, VEHICLE_SPEED, …) would make the OBD2 row light up with the
 * CAN signal's live value — "CAN leaking through as OBD2". Provenance is the
 * authoritative signal_t.source, not a name match. Reads the pre-taken
 * snapshot — never the live registry — so it is safe to call unlocked. */
static void _add_live_signal_state(cJSON *out, const sig_snapshot_t *snap,
                                   uint16_t snap_n, const char *signal_name,
                                   signal_source_t expected) {
	if (!out || !signal_name || !signal_name[0]) return;
	const sig_snapshot_t *s = _snap_find(snap, snap_n, signal_name);
	bool match = s && ((signal_source_t)s->source == expected);
	cJSON_AddBoolToObject(out, "exists_in_layout", match);
	if (!match) return;
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

	/* Read the active layout (for the ECU make/version highlight) BEFORE taking
	 * the lock — it's LittleFS I/O with no live state, and this handler used to
	 * hold the LVGL mutex across the read (plus the whole ~98 KB build),
	 * freezing rendering ~1.9 s every time the source picker opened. Moving the
	 * filesystem read out shortens the lock hold; the live-signal reads in the
	 * build still need it. (Remaining: snapshot signals to move the build out
	 * too.) */
	cJSON *layout_root = _load_active_layout_root();
	const char *active_make = NULL, *active_version = NULL;
	if (layout_root) {
		cJSON *jmake = cJSON_GetObjectItemCaseSensitive(layout_root, "ecu");
		cJSON *jver  = cJSON_GetObjectItemCaseSensitive(layout_root, "ecu_version");
		if (cJSON_IsString(jmake)) active_make    = jmake->valuestring;
		if (cJSON_IsString(jver))  active_version = jver->valuestring;
	}

	/* Snapshot everything that needs the LVGL lock into local memory, then
	 * release the lock and build the response from the snapshot. The build
	 * iterates ~180 preconfig rows + 200 signal slots and emits ~98 KB of
	 * cJSON; doing that under the lock froze rendering ~1.9 s every time the
	 * picker opened (H15). The lock now covers only the O(N) copy below. */
	sig_snapshot_t *snap = malloc(sizeof(sig_snapshot_t) * MAX_SIGNALS);
	if (!snap) {
		if (layout_root) cJSON_Delete(layout_root);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	char current_signal[32] = {0};
	uint16_t snap_n = 0;

	if (!rdm_lvgl_lock(500)) {
		free(snap);
		if (layout_root) cJSON_Delete(layout_root);
		return web_server_send_busy(req);
	}

	channel_t *c = channel_manager_get(channel_id);
	if (c) {
		strncpy(current_signal, c->signal_name, sizeof(current_signal) - 1);
		current_signal[sizeof(current_signal) - 1] = '\0';
	}
	uint16_t sig_total = signal_get_count();
	for (uint16_t i = 0; i < sig_total && snap_n < MAX_SIGNALS; ++i) {
		signal_t *s = signal_get_by_index(i);
		if (!s || s->name[0] == '\0') continue;
		sig_snapshot_t *d = &snap[snap_n++];
		strncpy(d->name, s->name, sizeof(d->name) - 1);
		d->name[sizeof(d->name) - 1] = '\0';
		strncpy(d->unit, s->unit, sizeof(d->unit) - 1);
		d->unit[sizeof(d->unit) - 1] = '\0';
		d->source        = s->source;
		d->current_value = s->current_value;
		d->is_stale      = s->is_stale;
	}

	rdm_lvgl_unlock();

	/* Provenance of the currently-bound signal. A row is only "is_current" in
	 * the section whose source matches it — so an OBD2 row never highlights as
	 * the active source when the channel is really bound to a same-named CAN
	 * signal (and vice-versa). */
	signal_source_t current_src = SIGNAL_SOURCE_CAN;
	if (current_signal[0]) {
		const sig_snapshot_t *cs = _snap_find(snap, snap_n, current_signal);
		if (cs) current_src = (signal_source_t)cs->source;
		else if (_obd2_name_known(current_signal)) current_src = SIGNAL_SOURCE_OBD2;
	}

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "channel_id", channel_id);
	cJSON_AddStringToObject(root, "current_signal", current_signal);

	/* active_make / active_version were read from the layout above, before the
	 * lock. Build the unified makes[] response. Each entry has its versions[]
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
			strcmp(current_signal, sname) == 0 && current_src == SIGNAL_SOURCE_CAN);
		/* Live value only when a CAN-sourced signal of this name is registered
		 * (ECU preset signals are broadcast CAN). A same-named OBD2/internal
		 * signal must not light up the ECU row. */
		_add_live_signal_state(o, snap, snap_n, sname, SIGNAL_SOURCE_CAN);
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
			cJSON_AddBoolToObject  (o, "is_current",
				strcmp(current_signal, d->signal_name) == 0 && current_src == SIGNAL_SOURCE_OBD2);
			/* Live state only from an actually-OBD2 signal of this name — a
			 * raw-CAN signal sharing the PID's name (RPM, COOLANT_TEMP, …) must
			 * NOT make this OBD2 row appear live/bound ("CAN leaking as OBD2"). */
			_add_live_signal_state(o, snap, snap_n, d->signal_name, SIGNAL_SOURCE_OBD2);
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
		for (uint16_t i = 0; i < snap_n; ++i) {
			const sig_snapshot_t *s = &snap[i];
			/* Classify by PROVENANCE, not name. Only genuine CAN-sourced signals
			 * belong here (DBC imports, user CAN frames, ECU-preset signals) —
			 * OBD2 + internal signals live under their own makes. This is what
			 * keeps a user's raw-CAN "RPM"/"COOLANT_TEMP" in Custom instead of
			 * being swallowed by an OBD2/ECU name collision, and conversely keeps
			 * real OBD2 signals out of Custom regardless of name. */
			if ((signal_source_t)s->source != SIGNAL_SOURCE_CAN) continue;
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "kind",        "custom");
			cJSON_AddStringToObject(o, "signal_name", s->name);
			cJSON_AddStringToObject(o, "label",       s->name);
			cJSON_AddStringToObject(o, "unit",        s->unit);
			cJSON_AddNumberToObject(o, "live_value",  s->current_value);
			cJSON_AddBoolToObject  (o, "is_stale",    s->is_stale);
			cJSON_AddBoolToObject  (o, "is_current",
				strcmp(current_signal, s->name) == 0 && current_src == SIGNAL_SOURCE_CAN);
			cJSON_AddBoolToObject  (o, "exists_in_layout", true);
			cJSON_AddItemToArray(signals_arr, o);
		}
		cJSON_AddItemToArray(versions_arr, ver_obj);
		cJSON_AddItemToArray(makes_arr, make_obj);
	}

	free(snap);
	if (layout_root) cJSON_Delete(layout_root);

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
		return web_server_send_busy(req);
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
		/* Build the new polled-PID list, start polling it best-effort (so the
		 * bind works THIS session even if persistence fails), then surface a
		 * save failure to the client. Previously a failed save silently left
		 * the channel bound-but-never-polling and still returned HTTP 200. */
		esp_err_t save_err = ESP_OK;
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
			if (!dup) {
				if (count >= OBD2_MAX_ENABLED) {
					save_err = ESP_ERR_NO_MEM;          /* list full (max 48) */
				} else {
					pids[count++] = enc;
					save_err = ecu_preset_save_obd2_pids(layout, pids, count);
				}
			}
			if (count > 0) obd2_start(pids, count);     /* poll regardless of save */
		} else {
			save_err = ESP_FAIL;                        /* no active layout */
		}
		channel_manager_set_signal(c, signal_name);
		channel_manager_resolve_signals();
		if (save_err != ESP_OK) {
			rdm_lvgl_unlock();
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				save_err == ESP_ERR_NO_MEM
					? "OBD2 PID limit reached (max 48)"
					: "Bound for this session but failed to save — won't survive a reboot");
			return ESP_FAIL;
		}
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
			/* ADR 0005: copy the decode onto the channel (device-local,
			 * authoritative, survives reboot via channels.json). */
			channel_manager_set_decode(c, s->can_id, s->bit_start, s->bit_length,
			                           s->scale, s->offset, s->is_signed,
			                           s->endian, s->unit[0] ? s->unit : NULL,
			                           /* persist_now (set_signal already flushed) */ false);
			char layout[64];
			if (layout_manager_get_active(layout, sizeof(layout)) == ESP_OK) {
				/* ecu_preset_write_signal_to_layout persists to disk itself
				 * (calls layout_manager_save_raw) — no double-save here.
				 * Kept as a harmless local cache; the channel decode wins. */
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

/* ── GET /api/channels/export — download the entire channels.json ──────────
 * Channel config (CAN decode + bindings) is device-local per ADR-0005 and was
 * previously trapped on-device with no way to back it up or move it to another
 * unit. Returns the raw file as a downloadable attachment. */
static esp_err_t channels_export_handler(httpd_req_t *req) {
	char *buf = NULL;
	size_t len = 0;
	esp_err_t e = channel_manager_export_raw(&buf, &len);
	if (e != ESP_OK || !buf) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
			e == ESP_ERR_NOT_FOUND ? "No channels.json yet" : "Export failed");
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Content-Disposition",
	                   "attachment; filename=\"channels.json\"");
	esp_err_t r = httpd_resp_send(req, buf, len);
	free(buf);
	return r;
}

/* ── POST /api/channels/import — restore channels.json, then reboot ────────
 * Validates + atomically replaces the file (channel_manager_import_raw), then
 * reboots so the new config loads cleanly at boot. We deliberately do NOT hot-
 * swap the live registry — that would dangle widgets' channel subscriptions —
 * and a restore is a rare, deliberate op where a reboot is expected. */
static void _deferred_chimport_reboot(void *arg) {
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(800));
	esp_restart();
}

static esp_err_t channels_import_handler(httpd_req_t *req) {
	int total = req->content_len;
	if (total <= 0 || total > 64 * 1024) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
		return ESP_FAIL;
	}
	char *body = malloc((size_t)total + 1);
	if (!body) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
		return ESP_FAIL;
	}
	/* Read the full body (may arrive across multiple TCP segments). */
	int got = 0;
	while (got < total) {
		int r = httpd_req_recv(req, body + got, total - got);
		if (r <= 0) {
			free(body);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body recv failed");
			return ESP_FAIL;
		}
		got += r;
	}
	body[total] = '\0';

	esp_err_t e = channel_manager_import_raw(body, (size_t)total);
	free(body);
	if (e != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
			"Invalid channels.json (need channels[] + loadable schema_version)");
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_sendstr(req, "{\"status\":\"ok\",\"reboot\":true}");
	xTaskCreate((TaskFunction_t)_deferred_chimport_reboot, "chimp_rb",
	            2048, NULL, 1, NULL);
	return ESP_OK;
}

static const httpd_uri_t channels_export_uri = {
	.uri = "/api/channels/export", .method = HTTP_GET,
	.handler = channels_export_handler, .user_ctx = NULL
};

static const httpd_uri_t channels_import_uri = {
	.uri = "/api/channels/import", .method = HTTP_POST,
	.handler = channels_import_handler, .user_ctx = NULL
};

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
	if (!s_channels_list_mux) s_channels_list_mux = xSemaphoreCreateMutex();
	REGISTER_URI(server, &channels_list_uri);
	REGISTER_URI(server, &channels_canonical_uri);
	REGISTER_URI(server, &channels_activate_uri);
	REGISTER_URI(server, &channels_update_uri);
	REGISTER_URI(server, &channels_delete_uri);
	REGISTER_URI(server, &channels_source_options_uri);
	REGISTER_URI(server, &channels_bind_source_uri);
	REGISTER_URI(server, &channels_export_uri);
	REGISTER_URI(server, &channels_import_uri);
	ESP_LOGI(TAG, "channel endpoints: list, canonical, activate, update, delete, source-options, bind-source, export, import");
}
