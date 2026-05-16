#include "layout_manager.h"
#include "default_layout.h"
#include "boot_assets.h"

/* Widget factory headers */
#include "widget_bar.h"
#include "widget_indicator.h"
#include "widget_meter.h"
#include "widget_panel.h"
#include "widget_registry.h"
#include "widget_rpm_bar.h"
#include "widget_text.h"
#include "widget_types.h"
#include "widget_image.h"
#include "widget_warning.h"
#include "widget_shape_panel.h"
#include "widget_arc.h"
#include "widget_toggle.h"
#include "widget_button.h"
#include "widget_shift_light.h"
#include "widget_line.h"
#include "widget_rules.h"

#include "signal.h"
#include "signal_internal.h"
#include "screen_config.h"
#include "obd2.h"

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "layout_mgr";

/* NVS namespace + key for the active layout name */
#define NS_LAYOUT "layout_mgr"
#define KEY_ACTIVE "active"
#define KEY_ACTIVE_SPLASH "active_spl"
#define SPLASH_PREFIX "_splash_"

/* Track whether LittleFS has been mounted this boot */
static bool s_lfs_mounted = false;

/* Mutex protecting file I/O and s_layout_version */
static SemaphoreHandle_t s_layout_mutex = NULL;

/* Monotonic version counter — incremented on every save or load */
static uint32_t s_layout_version = 0;

/* ECU context fields from layout JSON (optional, empty = "Custom") */
static char s_layout_ecu[32] = "";
static char s_layout_ecu_version[32] = "";

/* Layout-level night-mode CAN trigger (optional).
 * If a layout binds night mode to a CAN signal, these hold the binding
 * until the dashboard reads them and subscribes to the signal. Empty
 * signal name means "no CAN trigger — manual toggle only". */
static char  s_night_trigger_signal[32]   = "";
static float s_night_trigger_active_when  = 1.0f;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Build the full VFS path for a layout name.
 *   path_out must be at least (strlen(LFS_LAYOUT_DIR) + 1 + LAYOUT_MAX_NAME +
 * 6) bytes, i.e. ≈ 64 bytes.
 */
static void _make_path(const char *name, char *path_out, size_t path_len) {
	snprintf(path_out, path_len, "%s/%s.json", LFS_LAYOUT_DIR, name);
}

/**
 * Strip the ".json" suffix from a filename, writing the result into out_name.
 * Returns true if the filename had the suffix, false otherwise.
 */
static bool _strip_json(const char *filename, char *out_name, size_t out_len) {
	size_t flen = strlen(filename);
	if (flen <= 5)
		return false;
	if (strcmp(filename + flen - 5, ".json") != 0)
		return false;
	size_t copy = flen - 5;
	if (copy >= out_len)
		copy = out_len - 1;
	memcpy(out_name, filename, copy);
	out_name[copy] = '\0';
	return true;
}

/**
 * Map a JSON type string to widget_type_t.  Returns WIDGET_TYPE_COUNT if
 * the type string is not recognised.
 */
static widget_type_t _type_from_str(const char *s) {
	if (!s)
		return WIDGET_TYPE_COUNT;
	if (strcmp(s, "panel") == 0)
		return WIDGET_PANEL;
	if (strcmp(s, "rpm_bar") == 0)
		return WIDGET_RPM_BAR;
	if (strcmp(s, "bar") == 0)
		return WIDGET_BAR;
	if (strcmp(s, "indicator") == 0)
		return WIDGET_INDICATOR;
	if (strcmp(s, "warning") == 0)
		return WIDGET_WARNING;
	if (strcmp(s, "text") == 0)
		return WIDGET_TEXT;
	if (strcmp(s, "meter") == 0)
		return WIDGET_METER;
	if (strcmp(s, "image") == 0)
		return WIDGET_IMAGE;
	if (strcmp(s, "shape_panel") == 0)
		return WIDGET_SHAPE_PANEL;
	if (strcmp(s, "arc") == 0)
		return WIDGET_ARC;
	if (strcmp(s, "toggle") == 0)
		return WIDGET_TOGGLE;
	if (strcmp(s, "button") == 0)
		return WIDGET_BUTTON;
	if (strcmp(s, "shift_light") == 0)
		return WIDGET_SHIFT_LIGHT;
	if (strcmp(s, "line") == 0)
		return WIDGET_LINE;
	return WIDGET_TYPE_COUNT;
}

/**
 * Call the correct factory function for the given type + slot.
 * Slot is extracted from the widget JSON object's "config.slot" field
 * (defaults to 0 if absent).
 * Returns a newly-allocated widget_t or NULL on error.
 */
static widget_t *_factory(widget_type_t type, cJSON *widget_json) {
	/* Try to read slot from config sub-object */
	uint8_t slot = 0;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(widget_json, "config");
	if (cfg) {
		cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(cfg, "slot");
		if (cJSON_IsNumber(slot_item)) {
			slot = (uint8_t)slot_item->valueint;
		}
	}

	widget_t *w = NULL;

	switch (type) {
	case WIDGET_PANEL:
		w = widget_panel_create_instance(slot);
		break;
	case WIDGET_RPM_BAR:
		w = widget_rpm_bar_create_instance();
		break;
	case WIDGET_BAR:
		w = widget_bar_create_instance(slot);
		break;
	case WIDGET_INDICATOR:
		w = widget_indicator_create_instance(slot);
		break;
	case WIDGET_WARNING:
		w = widget_warning_create_instance(slot);
		break;
	case WIDGET_TEXT:
		w = widget_text_create_instance(slot);
		break;
	case WIDGET_METER:
		w = widget_meter_create_instance(slot);
		break;
	case WIDGET_IMAGE:
		w = widget_image_create_instance(slot);
		break;
	case WIDGET_SHAPE_PANEL:
		w = widget_shape_panel_create_instance(slot);
		break;
	case WIDGET_ARC:
		w = widget_arc_create_instance(slot);
		break;
	case WIDGET_TOGGLE:
		w = widget_toggle_create_instance(slot);
		break;
	case WIDGET_BUTTON:
		w = widget_button_create_instance(slot);
		break;
	case WIDGET_SHIFT_LIGHT:
		w = widget_shift_light_create_instance(slot);
		break;
	case WIDGET_LINE:
		w = widget_line_create_instance(slot);
		break;
	default:
		return NULL;
	}

	if (!w)
		return NULL;

	/* Fail fast: if we cannot register this widget, destroy it immediately and
	 * return NULL so the caller skips this JSON entry.  This prevents
	 * untracked "zombie" widgets whose LVGL objects would never be updated or
	 * destroyed. */
	if (!widget_registry_add(w)) {
		ESP_LOGE(TAG,
				 "widget_registry full (%u entries) — dropping widget id=%s",
				 (unsigned)widget_registry_count(), w->id);
		w->destroy(w);
		return NULL;
	}

	return w;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_init
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_init(void) {
	/* Create file I/O mutex once (recursive: save() calls save_raw()) */
	if (!s_layout_mutex) {
		s_layout_mutex = xSemaphoreCreateRecursiveMutex();
		configASSERT(s_layout_mutex);
	}

	if (s_lfs_mounted) {
		ESP_LOGD(TAG, "LittleFS already mounted — skipping");
		return ESP_OK;
	}

	esp_vfs_littlefs_conf_t lfs_conf = {
		.base_path = LFS_BASE_PATH,
		.partition_label = "littlefs",
		.format_if_mount_failed = true,
		.dont_mount = false,
	};

	esp_err_t err = esp_vfs_littlefs_register(&lfs_conf);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s",
				 esp_err_to_name(err));
		return err;
	}
	s_lfs_mounted = true;
	ESP_LOGI(TAG, "LittleFS mounted at %s", LFS_BASE_PATH);

	/* Seed firmware-embedded default assets (RDM logo, etc.) if missing.
	 * This runs after every boot so factory reset is self-healing. */
	boot_assets_seed_defaults();

	/* Create /lfs/layouts directory if it does not exist */
	struct stat st;
	if (stat(LFS_LAYOUT_DIR, &st) != 0) {
		if (mkdir(LFS_LAYOUT_DIR, 0755) != 0) {
			ESP_LOGW(TAG, "mkdir(%s) failed — may already exist",
					 LFS_LAYOUT_DIR);
		} else {
			ESP_LOGI(TAG, "Created %s", LFS_LAYOUT_DIR);
		}
	}

	/* ── Seed default layout if missing ────────────────────────────────
	 * Only generate default.json when it does not exist (first boot or
	 * after factory reset).  User edits to "default" are preserved
	 * across reboots — factory reset is the way to revert. */
	char default_path[80];
	snprintf(default_path, sizeof(default_path), "%s/default.json",
			 LFS_LAYOUT_DIR);

	struct stat st_def;
	if (stat(default_path, &st_def) != 0) {
		ESP_LOGI(TAG, "default.json not found — generating");
		esp_err_t err2 = generate_default_layout();
		if (err2 != ESP_OK) {
			ESP_LOGW(TAG, "generate_default_layout failed: %s",
					 esp_err_to_name(err2));
		}
		/* Set active to "default" only when no layout has been chosen */
		char cur[LAYOUT_MAX_NAME] = {0};
		if (layout_manager_get_active(cur, sizeof(cur)) != ESP_OK
		    || cur[0] == '\0') {
			layout_manager_set_active("default");
		}
	}

	/* ── Migrate legacy _splash.json → _splash_Default.json ──────────── */
	{
		char old_path[80], new_path[80];
		snprintf(old_path, sizeof(old_path), "%s/_splash.json", LFS_LAYOUT_DIR);
		snprintf(new_path, sizeof(new_path), "%s/_splash_Default.json",
				 LFS_LAYOUT_DIR);
		struct stat st_old, st_new;
		if (stat(old_path, &st_old) == 0 && stat(new_path, &st_new) != 0) {
			if (rename(old_path, new_path) == 0) {
				ESP_LOGI(TAG, "Migrated _splash.json → _splash_Default.json");
			} else {
				ESP_LOGW(TAG, "Failed to migrate _splash.json");
			}
		}
	}

	/* ── Seed default splash if none exists ─────────────────────────── */
	{
		char splash_path[80];
		snprintf(splash_path, sizeof(splash_path),
				 "%s/_splash_Default.json", LFS_LAYOUT_DIR);
		struct stat st_spl;
		if (stat(splash_path, &st_spl) != 0) {
			esp_err_t serr = generate_default_splash();
			if (serr != ESP_OK) {
				ESP_LOGW(TAG, "generate_default_splash failed: %s",
						 esp_err_to_name(serr));
			}
		}
	}

	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Signal loading + resolution helpers
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Parse the "signals" array from a layout JSON root and register each signal.
 * Must be called AFTER signal_registry_reset() and BEFORE widget creation so
 * that from_json can resolve signal names to indices.
 */
static void _load_signals(const cJSON *root) {
	const cJSON *signals_arr =
		cJSON_GetObjectItemCaseSensitive(root, "signals");
	if (!cJSON_IsArray(signals_arr))
		return;

	const cJSON *sj = NULL;
	cJSON_ArrayForEach(sj, signals_arr) {
		const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(sj, "name");
		const cJSON *can_id_item =
			cJSON_GetObjectItemCaseSensitive(sj, "can_id");
		const cJSON *start_item =
			cJSON_GetObjectItemCaseSensitive(sj, "bit_start");
		const cJSON *len_item =
			cJSON_GetObjectItemCaseSensitive(sj, "bit_length");

		if (!cJSON_IsString(name_item) || !cJSON_IsNumber(can_id_item) ||
			!cJSON_IsNumber(start_item) || !cJSON_IsNumber(len_item))
			continue;

		float scale = 1.0f, offset = 0.0f;
		bool is_signed = false;
		uint8_t endian = 1; /* default Intel (little-endian) */

		const cJSON *item;
		item = cJSON_GetObjectItemCaseSensitive(sj, "scale");
		if (cJSON_IsNumber(item))
			scale = (float)item->valuedouble;
		item = cJSON_GetObjectItemCaseSensitive(sj, "offset");
		if (cJSON_IsNumber(item))
			offset = (float)item->valuedouble;
		item = cJSON_GetObjectItemCaseSensitive(sj, "is_signed");
		if (cJSON_IsBool(item))
			is_signed = cJSON_IsTrue(item);
		item = cJSON_GetObjectItemCaseSensitive(sj, "endian");
		if (cJSON_IsNumber(item))
			endian = (uint8_t)item->valueint;

		const cJSON *unit_item = cJSON_GetObjectItemCaseSensitive(sj, "unit");
		const char *unit_str = (cJSON_IsString(unit_item) && unit_item->valuestring)
			? unit_item->valuestring : "";

		int16_t idx = signal_register(
			name_item->valuestring, (uint32_t)can_id_item->valueint,
			(uint8_t)start_item->valueint, (uint8_t)len_item->valueint, scale,
			offset, is_signed, endian, unit_str);

		if (idx >= 0) {
			ESP_LOGD(TAG, "Registered signal '%s' → index %d",
					 name_item->valuestring, (int)idx);
		} else {
			ESP_LOGW(TAG, "Failed to register signal '%s'",
					 name_item->valuestring);
		}

		/* Check for fuel_cal object on FUEL_SENDER_V signal */
		if (strcmp(name_item->valuestring, "FUEL_SENDER_V") == 0) {
			const cJSON *fc = cJSON_GetObjectItemCaseSensitive(sj, "fuel_cal");
			if (cJSON_IsObject(fc)) {
				float empty_v = 0.5f, full_v = 3.0f, full_val = 100.0f;
				bool en = false;
				const cJSON *fci;
				fci = cJSON_GetObjectItemCaseSensitive(fc, "empty_v");
				if (cJSON_IsNumber(fci)) empty_v = (float)fci->valuedouble;
				fci = cJSON_GetObjectItemCaseSensitive(fc, "full_v");
				if (cJSON_IsNumber(fci)) full_v = (float)fci->valuedouble;
				fci = cJSON_GetObjectItemCaseSensitive(fc, "full_value");
				if (cJSON_IsNumber(fci)) full_val = (float)fci->valuedouble;
				fci = cJSON_GetObjectItemCaseSensitive(fc, "enabled");
				if (cJSON_IsBool(fci)) en = cJSON_IsTrue(fci);
				signal_internal_set_fuel_cal(empty_v, full_v, full_val, en);
				ESP_LOGI(TAG, "Loaded fuel_cal from FUEL_SENDER_V signal");
			}
		}
	}

	ESP_LOGI(TAG, "_load_signals: registered %u signals",
			 (unsigned)signal_get_count());

	/* Parse the layout's `custom_pids` array (if any) into the runtime
	 * custom-PID registry. Must happen BEFORE obd2_start so polled_pids
	 * referring to custom (service, pid) tuples can be found by
	 * obd2_pid_find_svc. Reset first so the previous layout's custom set
	 * doesn't leak into this one. */
	obd2_custom_reset();
	const cJSON *cust_arr = cJSON_GetObjectItemCaseSensitive(root, "custom_pids");
	if (cJSON_IsArray(cust_arr)) {
		const cJSON *ci;
		cJSON_ArrayForEach(ci, cust_arr) {
			if (!cJSON_IsObject(ci)) continue;
			const cJSON *jn = cJSON_GetObjectItemCaseSensitive(ci, "signal_name");
			const cJSON *jp = cJSON_GetObjectItemCaseSensitive(ci, "pid");
			if (!cJSON_IsString(jn) || !cJSON_IsNumber(jp)) continue;

			uint8_t  svc       = 0x01;
			uint16_t pid       = (uint16_t)jp->valueint;
			uint8_t  offset_b  = 0;
			uint8_t  bytes     = 1;
			float    scale     = 1.0f;
			float    sig_off   = 0.0f;
			bool     is_signed = false;
			uint32_t req_id    = 0;
			obd2_tier_t tier   = OBD2_TIER_SLOW;
			const char *label    = jn->valuestring;
			const char *unit     = "";
			const char *category = NULL;

			const cJSON *t;
			t = cJSON_GetObjectItemCaseSensitive(ci, "service");
			if (cJSON_IsNumber(t)) svc = (uint8_t)t->valueint;
			t = cJSON_GetObjectItemCaseSensitive(ci, "data_offset");
			if (cJSON_IsNumber(t)) offset_b = (uint8_t)t->valueint;
			t = cJSON_GetObjectItemCaseSensitive(ci, "data_bytes");
			if (cJSON_IsNumber(t)) bytes = (uint8_t)t->valueint;
			t = cJSON_GetObjectItemCaseSensitive(ci, "scale");
			if (cJSON_IsNumber(t)) scale = (float)t->valuedouble;
			t = cJSON_GetObjectItemCaseSensitive(ci, "offset");
			if (cJSON_IsNumber(t)) sig_off = (float)t->valuedouble;
			t = cJSON_GetObjectItemCaseSensitive(ci, "is_signed");
			if (cJSON_IsBool(t)) is_signed = cJSON_IsTrue(t);
			t = cJSON_GetObjectItemCaseSensitive(ci, "request_id");
			if (cJSON_IsNumber(t)) req_id = (uint32_t)t->valueint;
			t = cJSON_GetObjectItemCaseSensitive(ci, "unit");
			if (cJSON_IsString(t)) unit = t->valuestring;
			t = cJSON_GetObjectItemCaseSensitive(ci, "label");
			if (cJSON_IsString(t)) label = t->valuestring;
			t = cJSON_GetObjectItemCaseSensitive(ci, "category");
			if (cJSON_IsString(t)) category = t->valuestring;
			t = cJSON_GetObjectItemCaseSensitive(ci, "tier");
			if (cJSON_IsString(t) && t->valuestring &&
			    strcmp(t->valuestring, "fast") == 0) {
				tier = OBD2_TIER_FAST;
			}

			obd2_custom_add(svc, pid, jn->valuestring, label, unit,
			                offset_b, bytes, scale, sig_off, is_signed,
			                tier, category, req_id);
		}
	}

	/* Parse the layout's polled_pids array (if any) and start OBD2 polling.
	 * Numbers are encoded (service<<8 | pid) tuples; values <= 0xFF are
	 * treated as Mode 01 for back-compat. Falls back to legacy `obd2_pids`
	 * key (always Mode 01) for layouts written by earlier firmware. */
	uint32_t pid_list[OBD2_MAX_ENABLED];
	uint8_t pid_count = 0;
	const cJSON *pids_arr = cJSON_GetObjectItemCaseSensitive(root, "polled_pids");
	if (!cJSON_IsArray(pids_arr)) {
		pids_arr = cJSON_GetObjectItemCaseSensitive(root, "obd2_pids");
	}
	if (cJSON_IsArray(pids_arr)) {
		const cJSON *pi;
		cJSON_ArrayForEach(pi, pids_arr) {
			if (!cJSON_IsNumber(pi)) continue;
			if (pid_count >= OBD2_MAX_ENABLED) break;
			double v = pi->valuedouble;
			if (v < 0 || v > 0xFFFFFFu) continue;
			pid_list[pid_count++] = (uint32_t)v;
		}
	}
	if (pid_count > 0) {
		obd2_start(pid_list, pid_count);
	} else {
		obd2_stop();
	}
}

/**
 * Serialise all registered signals into a "signals" JSON array on @p root.
 */
static void _save_signals(cJSON *root) {
	uint16_t count = signal_get_count();
	if (count == 0)
		return;

	cJSON *arr = cJSON_AddArrayToObject(root, "signals");
	if (!arr)
		return;

	for (uint16_t i = 0; i < count; i++) {
		signal_t *sig = signal_get_by_index(i);
		if (!sig)
			continue;
		cJSON *sj = cJSON_CreateObject();
		if (!sj)
			continue;
		cJSON_AddStringToObject(sj, "name", sig->name);
		cJSON_AddNumberToObject(sj, "can_id", sig->can_id);
		cJSON_AddNumberToObject(sj, "bit_start", sig->bit_start);
		cJSON_AddNumberToObject(sj, "bit_length", sig->bit_length);
		cJSON_AddNumberToObject(sj, "scale", sig->scale);
		cJSON_AddNumberToObject(sj, "offset", sig->offset);
		cJSON_AddBoolToObject(sj, "is_signed", sig->is_signed);
		cJSON_AddNumberToObject(sj, "endian", sig->endian);
		if (sig->unit[0] != '\0')
			cJSON_AddStringToObject(sj, "unit", sig->unit);

		/* Attach fuel calibration to FUEL_SENDER_V signal */
		if (strcmp(sig->name, "FUEL_SENDER_V") == 0) {
			fuel_cal_config_t fc;
			signal_internal_get_fuel_cal(&fc);
			cJSON *fc_obj = cJSON_AddObjectToObject(sj, "fuel_cal");
			if (fc_obj) {
				cJSON_AddNumberToObject(fc_obj, "empty_v", fc.empty_v);
				cJSON_AddNumberToObject(fc_obj, "full_v", fc.full_v);
				cJSON_AddNumberToObject(fc_obj, "full_value", fc.full_value);
				cJSON_AddBoolToObject(fc_obj, "enabled", fc.enabled);
			}
		}

		cJSON_AddItemToArray(arr, sj);
	}
}

/**
 * Shared helper: reset signals, load signal definitions, then iterate the
 * "widgets" array — calling factory → from_json → rules → create for each.
 * Callers are responsible for file I/O, JSON parsing, and cJSON cleanup.
 *
 * @param root     Parsed layout JSON root (must contain "widgets" array)
 * @param parent   LVGL parent object for widget creation
 * @param caller   Tag string for log messages (e.g. "layout_load", "apply_json")
 * @return ESP_OK on success, ESP_FAIL if "widgets" array is missing
 */
static esp_err_t _instantiate_widgets(cJSON *root, lv_obj_t *parent,
									  const char *caller) {
	/* ── Load signals BEFORE widgets so from_json can resolve names ── */
	signal_registry_reset();
	_load_signals(root);
	/* Restore persisted peak/min values for any signal name that made it
	 * into the new layout. Stale records (signal no longer present) are
	 * silently dropped on the next autosave. */
	signal_peaks_load();

	const cJSON *widgets_arr =
		cJSON_GetObjectItemCaseSensitive(root, "widgets");
	if (!cJSON_IsArray(widgets_arr)) {
		ESP_LOGE(TAG, "%s: no 'widgets' array", caller);
		return ESP_FAIL;
	}

	const cJSON *wj = NULL;
	uint16_t loaded_idx = 0;
	cJSON_ArrayForEach(wj, widgets_arr) {
		const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(wj, "type");
		widget_type_t wtype = _type_from_str(
			cJSON_IsString(type_item) ? type_item->valuestring : NULL);
		if (wtype == WIDGET_TYPE_COUNT) {
			ESP_LOGW(TAG, "%s: unknown widget type '%s', skipping", caller,
					 cJSON_IsString(type_item) ? type_item->valuestring
											   : "(null)");
			continue;
		}

		widget_t *w = _factory(wtype, (cJSON *)wj);
		if (!w) {
			ESP_LOGW(TAG, "%s: factory returned NULL for type %d", caller,
					 (int)wtype);
			continue;
		}

		/* Restore base fields (x, y, w, h, id) and type-specific config.
		 * Cast away const: cJSON iterator yields const ptr but our vtable
		 * takes mutable (cJSON API doesn't propagate const internally). */
		ESP_LOGD(TAG, "%s: Calling from_json for %s", caller, w->id);
		w->from_json(w, (cJSON *)wj);

		/* Parse conditional rules from config */
		cJSON *cfg_obj = cJSON_GetObjectItemCaseSensitive((cJSON *)wj, "config");
		if (cfg_obj)
			widget_rules_from_json(w, cfg_obj);

		/* Build LVGL objects on the parent screen.
		 * DIAGNOSTIC: log before+after so if create hangs (infinite loop in
		 * LVGL walking a corrupt parent chain etc.), the serial monitor
		 * tells us exactly which widget is the culprit. These can be
		 * downgraded to ESP_LOGD once the layout-load path is proven. */
		ESP_LOGI(TAG, "%s: create START  id=%s type=%d (slot=%u) @(%d,%d) %dx%d",
		         caller, w->id, (int)wtype, w->slot, w->x, w->y, w->w, w->h);
		w->create(w, parent);
		ESP_LOGI(TAG, "%s: create END    id=%s", caller, w->id);

		/* Yield so the priority-0 IDLE task on CPU 1 can run and feed its
		 * watchdog. We need vTaskDelay(1) (one actual tick = 2 ms at
		 * CONFIG_FREERTOS_HZ=500) — NOT pdMS_TO_TICKS(1), which rounds to
		 * zero ticks at 500 Hz and produces vTaskDelay(0). That variant
		 * only yields to same-or-higher priority tasks, so IDLE (priority 0)
		 * never gets scheduled and the TWDT fires after ~5 s of continuous
		 * widget instantiation (tripped during first-boot layout load).
		 *
		 * Yield EVERY widget so a single slow-to-create widget (e.g., image
		 * widget loading from LittleFS + decoding a 1+ MB RGB888 backdrop)
		 * can't consume the 5-second budget before the next yield lands.
		 * Cost: ~2 ms per widget × 32 widgets = ~64 ms added to boot —
		 * imperceptible. */
		vTaskDelay(1);
		(void)loaded_idx;  /* retained for future diagnostics */
		loaded_idx++;

		/* Subscribe rule signals after create (needs w->root) */
		widget_rules_subscribe(w);

		/* Position root object if valid */
		if (w->root && lv_obj_is_valid(w->root)) {
			lv_obj_set_x(w->root, w->x);
			lv_obj_set_y(w->root, w->y);
		}

		ESP_LOGD(TAG, "%s: loaded widget id=%s type=%d at (%d,%d)", caller,
				 w->id, (int)wtype, (int)w->x, (int)w->y);
	}

	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_load
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_load(const char *name, lv_obj_t *parent) {
	if (!name || !parent)
		return ESP_ERR_INVALID_ARG;

	xSemaphoreTakeRecursive(s_layout_mutex, portMAX_DELAY);

	char path[80];
	_make_path(name, path, sizeof(path));

	/* POSIX open() — see layout_manager_read_raw for rationale. */
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		/* Try recovering from backup if primary file is missing */
		char bak_path[96];
		snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
		if (rename(bak_path, path) == 0) {
			ESP_LOGW(TAG, "layout_load: recovered %s from .bak", path);
			fd = open(path, O_RDONLY);
		}
		if (fd < 0) {
			ESP_LOGE(TAG, "layout_load: cannot open %s (errno=%d)", path, errno);
			xSemaphoreGiveRecursive(s_layout_mutex);
			return ESP_ERR_NOT_FOUND;
		}
	}

	/* Read entire file into a heap buffer */
	char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
	if (!buf) {
		close(fd);
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_ERR_NO_MEM;
	}

	size_t nread = 0;
	while (nread < LAYOUT_MAX_FILE_BYTES - 1) {
		ssize_t n = read(fd, buf + nread, LAYOUT_MAX_FILE_BYTES - 1 - nread);
		if (n < 0) {
			ESP_LOGE(TAG, "layout_load: read failed (errno=%d)", errno);
			close(fd);
			free(buf);
			xSemaphoreGiveRecursive(s_layout_mutex);
			return ESP_FAIL;
		}
		if (n == 0) break;
		nread += (size_t)n;
	}
	close(fd);
	buf[nread] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);

	if (!root) {
		ESP_LOGE(TAG, "layout_load: JSON parse failed for %s", path);
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_FAIL;
	}

	/* ── Validate schema version ── */
	cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
	int schema_ver = cJSON_IsNumber(sv) ? sv->valueint : 0;
	if (schema_ver < 1) {
		ESP_LOGE(TAG, "layout_load: schema v%d invalid (expected >= 1) in %s",
				 schema_ver, path);
		cJSON_Delete(root);
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_FAIL;
	}
	if (schema_ver > LAYOUT_SCHEMA_VERSION) {
		ESP_LOGW(TAG, "layout_load: schema v%d newer than firmware v%d — loading anyway",
				 schema_ver, LAYOUT_SCHEMA_VERSION);
	}

	/* ── Extract optional ECU context fields ── */
	const cJSON *ecu_item = cJSON_GetObjectItemCaseSensitive(root, "ecu");
	if (cJSON_IsString(ecu_item) && ecu_item->valuestring[0]) {
		strncpy(s_layout_ecu, ecu_item->valuestring, sizeof(s_layout_ecu) - 1);
		s_layout_ecu[sizeof(s_layout_ecu) - 1] = '\0';
	} else {
		s_layout_ecu[0] = '\0';
	}
	const cJSON *ecu_ver_item =
		cJSON_GetObjectItemCaseSensitive(root, "ecu_version");
	if (cJSON_IsString(ecu_ver_item) && ecu_ver_item->valuestring[0]) {
		strncpy(s_layout_ecu_version, ecu_ver_item->valuestring,
				sizeof(s_layout_ecu_version) - 1);
		s_layout_ecu_version[sizeof(s_layout_ecu_version) - 1] = '\0';
	} else {
		s_layout_ecu_version[0] = '\0';
	}

	/* ── Optional night-mode CAN trigger ── */
	s_night_trigger_signal[0]  = '\0';
	s_night_trigger_active_when = 1.0f;
	const cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "night_mode");
	if (cJSON_IsObject(nm)) {
		const cJSON *sig = cJSON_GetObjectItemCaseSensitive(nm, "signal_name");
		if (cJSON_IsString(sig) && sig->valuestring[0]) {
			strncpy(s_night_trigger_signal, sig->valuestring,
			        sizeof(s_night_trigger_signal) - 1);
			s_night_trigger_signal[sizeof(s_night_trigger_signal) - 1] = '\0';
		}
		const cJSON *aw = cJSON_GetObjectItemCaseSensitive(nm, "active_when");
		if (cJSON_IsNumber(aw)) {
			s_night_trigger_active_when = (float)aw->valuedouble;
		}
	}

	/* ── Instantiate signals + widgets ── */
	esp_err_t ret = _instantiate_widgets(root, parent, "layout_load");
	cJSON_Delete(root);
	if (ret != ESP_OK) {
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ret;
	}

	s_layout_version++;
	ESP_LOGI(TAG, "layout_load: loaded '%s' from %s (version %lu)", name, path,
			 (unsigned long)s_layout_version);
	xSemaphoreGiveRecursive(s_layout_mutex);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_get_night_trigger
 * ═══════════════════════════════════════════════════════════════════════════
 */
bool layout_manager_get_night_trigger(char *out_signal, size_t signal_buf_size,
                                      float *out_active_when) {
	if (s_night_trigger_signal[0] == '\0') {
		if (out_signal && signal_buf_size > 0) out_signal[0] = '\0';
		if (out_active_when) *out_active_when = 1.0f;
		return false;
	}
	if (out_signal && signal_buf_size > 0) {
		strncpy(out_signal, s_night_trigger_signal, signal_buf_size - 1);
		out_signal[signal_buf_size - 1] = '\0';
	}
	if (out_active_when) *out_active_when = s_night_trigger_active_when;
	return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_apply_json
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_apply_json(cJSON *root, lv_obj_t *parent) {
	if (!root || !parent)
		return ESP_ERR_INVALID_ARG;

	esp_err_t ret = _instantiate_widgets(root, parent, "apply_json");
	if (ret != ESP_OK)
		return ret;

	ESP_LOGI(TAG, "apply_json: applied layout from cJSON");
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_build_json / layout_manager_save / layout_manager_save_raw
 * ═══════════════════════════════════════════════════════════════════════════
 */

cJSON *layout_manager_build_json(const char *name, widget_t **widgets,
								 uint8_t count) {
	if (!name || !widgets)
		return NULL;

	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	cJSON_AddNumberToObject(root, "schema_version", LAYOUT_SCHEMA_VERSION);
	cJSON_AddStringToObject(root, "name", name);
	cJSON_AddNumberToObject(root, "screen_w", SCREEN_W);
	cJSON_AddNumberToObject(root, "screen_h", SCREEN_H);

	/* Conditionally add ECU context if set */
	if (s_layout_ecu[0])
		cJSON_AddStringToObject(root, "ecu", s_layout_ecu);
	if (s_layout_ecu_version[0])
		cJSON_AddStringToObject(root, "ecu_version", s_layout_ecu_version);

	/* Conditionally add night-mode CAN trigger if set */
	if (s_night_trigger_signal[0]) {
		cJSON *nm = cJSON_AddObjectToObject(root, "night_mode");
		if (nm) {
			cJSON_AddStringToObject(nm, "signal_name", s_night_trigger_signal);
			cJSON_AddNumberToObject(nm, "active_when", s_night_trigger_active_when);
		}
	}

	/* Serialise registered signals */
	_save_signals(root);

	/* Preserve polled-PID list on internal saves (e.g. serial save, data
	 * logger snapshots). Each value is the encoded (service<<8 | pid)
	 * tuple. The web editor save_raw path already carries polled_pids
	 * forward from the editor's JSON tree. */
	{
		uint32_t pids[OBD2_MAX_ENABLED];
		uint8_t pc = obd2_get_enabled(pids, OBD2_MAX_ENABLED);
		if (pc > 0) {
			cJSON *pa = cJSON_AddArrayToObject(root, "polled_pids");
			for (uint8_t i = 0; i < pc; i++) {
				cJSON_AddItemToArray(pa, cJSON_CreateNumber(pids[i]));
			}
		}
	}

	/* Preserve the runtime custom-PID registry on internal saves. The
	 * web editor save_raw path already carries custom_pids forward from
	 * the editor's JSON tree, but build_json (used by serial save / data
	 * logger snapshots) builds the JSON from scratch — without this
	 * block, custom PIDs would silently disappear after such a save. */
	{
		uint8_t cc = obd2_custom_count();
		if (cc > 0) {
			cJSON *ca = cJSON_AddArrayToObject(root, "custom_pids");
			for (uint8_t i = 0; i < cc; i++) {
				const obd2_pid_def_t *d = obd2_custom_at(i);
				if (!d || !d->sub_fields || d->sub_field_count == 0) continue;
				const obd2_subfield_t *sf = &d->sub_fields[0];
				cJSON *cj = cJSON_CreateObject();
				if (!cj) continue;
				cJSON_AddStringToObject(cj, "signal_name", sf->signal_name);
				if (d->human_name && d->human_name[0])
					cJSON_AddStringToObject(cj, "label", d->human_name);
				cJSON_AddNumberToObject(cj, "service",
				                        d->service ? d->service : 0x01);
				cJSON_AddNumberToObject(cj, "pid", d->pid);
				cJSON_AddNumberToObject(cj, "data_offset", sf->byte_offset);
				cJSON_AddNumberToObject(cj, "data_bytes", sf->bytes);
				cJSON_AddNumberToObject(cj, "scale", sf->scale);
				cJSON_AddNumberToObject(cj, "offset", sf->offset);
				cJSON_AddBoolToObject(cj, "is_signed", sf->is_signed);
				if (sf->unit && sf->unit[0])
					cJSON_AddStringToObject(cj, "unit", sf->unit);
				if (d->category && d->category[0])
					cJSON_AddStringToObject(cj, "category", d->category);
				cJSON_AddStringToObject(cj, "tier",
				                       (d->tier == OBD2_TIER_FAST) ? "fast" : "slow");
				if (d->request_id)
					cJSON_AddNumberToObject(cj, "request_id", d->request_id);
				cJSON_AddItemToArray(ca, cj);
			}
		}
	}

	cJSON *arr = cJSON_AddArrayToObject(root, "widgets");
	if (!arr) { cJSON_Delete(root); return NULL; }
	for (uint8_t i = 0; i < count; i++) {
		widget_t *w = widgets[i];
		if (!w || !w->to_json)
			continue;
		cJSON *wj = cJSON_CreateObject();
		if (!wj) {
			continue;
		}
		w->to_json(w, wj);
		/* Serialize conditional rules into the widget's config object */
		cJSON *cfg_ser = cJSON_GetObjectItemCaseSensitive(wj, "config");
		if (cfg_ser)
			widget_rules_to_json(w, cfg_ser);
		cJSON_AddItemToArray(arr, wj);
	}

	return root;
}

esp_err_t layout_manager_save(const char *name, widget_t **widgets,
							  uint8_t count) {
	if (!name || !widgets)
		return ESP_ERR_INVALID_ARG;

	cJSON *root = layout_manager_build_json(name, widgets, count);
	if (!root)
		return ESP_ERR_NO_MEM;

	/* Delegate to raw save helper. */
	esp_err_t err = layout_manager_save_raw(name, root);
	cJSON_Delete(root);
	return err;
}

esp_err_t layout_manager_save_raw(const char *name, const cJSON *root) {
	if (!name || !root)
		return ESP_ERR_INVALID_ARG;

	xSemaphoreTakeRecursive(s_layout_mutex, portMAX_DELAY);

	char *json_str = cJSON_PrintUnformatted(root);
	if (!json_str) {
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_ERR_NO_MEM;
	}

	char path[80];
	_make_path(name, path, sizeof(path));

	/* Create backup of existing file before overwriting */
	char bak_path[96];
	snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
	rename(path, bak_path);

	/* POSIX open()/write()/close() — see layout_manager_read_raw for rationale.
	 * fsync() replaces fflush() to ensure data reaches flash before close. */
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		ESP_LOGE(TAG, "layout_save_raw: cannot open %s for writing (errno=%d)",
				 path, errno);
		rename(bak_path, path);
		free(json_str);
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_FAIL;
	}

	size_t len = strlen(json_str);
	size_t total_written = 0;
	while (total_written < len) {
		ssize_t n = write(fd, json_str + total_written, len - total_written);
		if (n <= 0) {
			ESP_LOGE(TAG, "layout_save_raw: write failed at %u/%u (errno=%d)",
					 (unsigned)total_written, (unsigned)len, errno);
			close(fd);
			free(json_str);
			remove(path);
			rename(bak_path, path);
			xSemaphoreGiveRecursive(s_layout_mutex);
			return ESP_FAIL;
		}
		total_written += (size_t)n;
	}
	free(json_str);

	/* close() commits LittleFS writes to flash — no separate fsync needed
	 * with POSIX write() (which bypasses the stdio userspace buffer that
	 * the previous fflush() was draining). */
	if (close(fd) != 0) {
		ESP_LOGE(TAG, "layout_save_raw: close failed for %s (errno=%d)",
				 path, errno);
		remove(path);
		rename(bak_path, path);
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_FAIL;
	}

	s_layout_version++;
	ESP_LOGI(TAG, "layout_save_raw: saved '%s' to %s (%u bytes, version %lu)",
			 name, path, (unsigned)len, (unsigned long)s_layout_version);
	xSemaphoreGiveRecursive(s_layout_mutex);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_read_raw
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_read_raw(const char *name, char *buf,
								  size_t buf_size, size_t *out_len) {
	if (!name || !buf || buf_size == 0)
		return ESP_ERR_INVALID_ARG;

	xSemaphoreTakeRecursive(s_layout_mutex, portMAX_DELAY);

	char path[80];
	_make_path(name, path, sizeof(path));

	/* POSIX open()/read()/close() instead of fopen()/fread()/fclose() — the
	 * stdio path mallocs a per-FILE newlib lock from internal SRAM on every
	 * fopen(), and newlib calls abort() if that alloc fails. The Device
	 * Settings open path used to hit this in production. POSIX file
	 * descriptors don't allocate per-handle locks, so this path is heap-safe
	 * even when internal SRAM is fragmented. */
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		ESP_LOGE(TAG, "layout_read_raw: cannot open %s (errno=%d)", path, errno);
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_ERR_NOT_FOUND;
	}

	/* Drain the file. read() may return short reads on some VFS backends;
	 * loop until EOF or buffer full. */
	size_t total = 0;
	while (total < buf_size - 1) {
		ssize_t n = read(fd, buf + total, buf_size - 1 - total);
		if (n < 0) {
			ESP_LOGE(TAG, "layout_read_raw: read failed (errno=%d)", errno);
			close(fd);
			xSemaphoreGiveRecursive(s_layout_mutex);
			return ESP_FAIL;
		}
		if (n == 0) break;  /* EOF */
		total += (size_t)n;
	}
	close(fd);
	buf[total] = '\0';

	if (out_len)
		*out_len = total;

	ESP_LOGD(TAG, "layout_read_raw: read '%s' (%u bytes)", name, (unsigned)total);
	xSemaphoreGiveRecursive(s_layout_mutex);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_delete
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_delete(const char *name) {
	if (!name)
		return ESP_ERR_INVALID_ARG;

	xSemaphoreTakeRecursive(s_layout_mutex, portMAX_DELAY);

	char path[80];
	_make_path(name, path, sizeof(path));
	if (remove(path) != 0) {
		ESP_LOGW(TAG, "layout_delete: remove(%s) failed", path);
		xSemaphoreGiveRecursive(s_layout_mutex);
		return ESP_ERR_NOT_FOUND;
	}
	/* Also remove backup file if it exists */
	char bak_path[96];
	snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
	remove(bak_path);
	ESP_LOGI(TAG, "layout_delete: deleted '%s'", name);
	xSemaphoreGiveRecursive(s_layout_mutex);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_list
 * ═══════════════════════════════════════════════════════════════════════════
 */
int layout_manager_list(char names[][LAYOUT_MAX_NAME], int max_count) {
	DIR *d = opendir(LFS_LAYOUT_DIR);
	if (!d) {
		ESP_LOGW(TAG, "layout_list: opendir(%s) failed", LFS_LAYOUT_DIR);
		return -1;
	}

	int count = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && count < max_count) {
		/* ESP-IDF LittleFS dirent does not expose d_type; filter by
		 * .json suffix only.  Subdirectories won't match, so this is safe. */
		char stripped[LAYOUT_MAX_NAME];
		if (_strip_json(de->d_name, stripped, sizeof(stripped))) {
			if (stripped[0] == '_')
				continue;
			strncpy(names[count], stripped, LAYOUT_MAX_NAME - 1);
			names[count][LAYOUT_MAX_NAME - 1] = '\0';
			count++;
		}
	}
	closedir(d);
	return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_any_exist
 * ═══════════════════════════════════════════════════════════════════════════
 */
bool layout_manager_any_exist(void) {
	char names[1][LAYOUT_MAX_NAME];
	int n = layout_manager_list(names, 1);
	return (n > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_set_active / layout_manager_get_active
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_set_active(const char *name) {
	if (!name)
		return ESP_ERR_INVALID_ARG;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NS_LAYOUT, NVS_READWRITE, &h);
	if (err != ESP_OK)
		return err;
	err = nvs_set_str(h, KEY_ACTIVE, name);
	if (err == ESP_OK)
		err = nvs_commit(h);
	nvs_close(h);
	ESP_LOGI(TAG, "set_active: '%s'", name);
	return err;
}

esp_err_t layout_manager_get_active(char *name_out, size_t len) {
	if (!name_out || len == 0)
		return ESP_ERR_INVALID_ARG;

	nvs_handle_t h;
	esp_err_t err = nvs_open(NS_LAYOUT, NVS_READONLY, &h);
	if (err != ESP_OK) {
		/* NVS namespace not yet created — use default */
		strncpy(name_out, "default", len - 1);
		name_out[len - 1] = '\0';
		return ESP_OK;
	}
	size_t sz = len;
	err = nvs_get_str(h, KEY_ACTIVE, name_out, &sz);
	nvs_close(h);

	if (err != ESP_OK) {
		strncpy(name_out, "default", len - 1);
		name_out[len - 1] = '\0';
		return ESP_OK;
	}
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Splash layout list / active splash NVS
 * ═══════════════════════════════════════════════════════════════════════════
 */
int layout_manager_list_splash(char names[][LAYOUT_MAX_NAME], int max_count) {
	DIR *d = opendir(LFS_LAYOUT_DIR);
	if (!d) {
		ESP_LOGW(TAG, "list_splash: opendir(%s) failed", LFS_LAYOUT_DIR);
		return -1;
	}

	int count = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && count < max_count) {
		/* Match _splash_*.json */
		if (strncmp(de->d_name, SPLASH_PREFIX, strlen(SPLASH_PREFIX)) != 0)
			continue;
		char stripped[LAYOUT_MAX_NAME];
		if (!_strip_json(de->d_name, stripped, sizeof(stripped)))
			continue;
		/* stripped is e.g. "_splash_Default" — skip the prefix */
		const char *bare = stripped + strlen(SPLASH_PREFIX);
		if (bare[0] == '\0')
			continue;
		strncpy(names[count], bare, LAYOUT_MAX_NAME - 1);
		names[count][LAYOUT_MAX_NAME - 1] = '\0';
		count++;
	}
	closedir(d);
	return count;
}

esp_err_t layout_manager_set_active_splash(const char *name) {
	if (!name)
		return ESP_ERR_INVALID_ARG;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NS_LAYOUT, NVS_READWRITE, &h);
	if (err != ESP_OK)
		return err;
	err = nvs_set_str(h, KEY_ACTIVE_SPLASH, name);
	if (err == ESP_OK)
		err = nvs_commit(h);
	nvs_close(h);
	ESP_LOGI(TAG, "set_active_splash: '%s'", name);
	return err;
}

esp_err_t layout_manager_get_active_splash(char *name_out, size_t len) {
	if (!name_out || len == 0)
		return ESP_ERR_INVALID_ARG;

	nvs_handle_t h;
	esp_err_t err = nvs_open(NS_LAYOUT, NVS_READONLY, &h);
	if (err != ESP_OK) {
		strncpy(name_out, "Default", len - 1);
		name_out[len - 1] = '\0';
		return ESP_OK;
	}
	size_t sz = len;
	err = nvs_get_str(h, KEY_ACTIVE_SPLASH, name_out, &sz);
	nvs_close(h);

	if (err != ESP_OK) {
		strncpy(name_out, "Default", len - 1);
		name_out[len - 1] = '\0';
		return ESP_OK;
	}
	return ESP_OK;
}

uint32_t layout_manager_get_version(void) { return s_layout_version; }
