#include "layout_manager.h"
#include "default_layout.h"

/* Widget factory headers */
#include "widget_bar.h"
#include "widget_indicator.h"
#include "widget_meter.h"
#include "widget_panel.h"
#include "widget_registry.h"
#include "widget_rpm_bar.h"
#include "widget_text.h"
#include "widget_types.h"
#include "widget_warning.h"

#include "signal.h"

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "layout_mgr";

/* NVS namespace + key for the active layout name */
#define NS_LAYOUT "layout_mgr"
#define KEY_ACTIVE "active"

/* Track whether LittleFS has been mounted this boot */
static bool s_lfs_mounted = false;

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
	if (s_lfs_mounted) {
		ESP_LOGI(TAG, "LittleFS already mounted — skipping");
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

	/* ── Ensure default layout exists and is up-to-date ─────────────────
	 * Read default.json's schema_version.  Regenerate if missing OR if
	 * the version is older than LAYOUT_SCHEMA_VERSION so that a
	 * firmware update automatically refreshes stale position data.      */
	bool need_regen = false;
	char default_path[80];
	snprintf(default_path, sizeof(default_path), "%s/default.json",
			 LFS_LAYOUT_DIR);

	FILE *df = fopen(default_path, "r");
	if (!df) {
		need_regen = true;
		ESP_LOGI(TAG, "default.json not found — will generate");
	} else {
		char hdr[128];
		size_t nr = fread(hdr, 1, sizeof(hdr) - 1, df);
		fclose(df);
		hdr[nr] = '\0';
		cJSON *tmp = cJSON_Parse(hdr);
		int ver = 0;
		if (tmp) {
			cJSON *sv = cJSON_GetObjectItemCaseSensitive(tmp, "schema_version");
			if (cJSON_IsNumber(sv))
				ver = sv->valueint;
			cJSON_Delete(tmp);
		}
		if (ver < LAYOUT_SCHEMA_VERSION) {
			need_regen = true;
			ESP_LOGI(TAG, "default.json schema v%d < v%d — regenerating", ver,
					 LAYOUT_SCHEMA_VERSION);
		}
	}

	if (need_regen) {
		esp_err_t err2 = generate_default_layout();
		if (err2 != ESP_OK) {
			ESP_LOGW(TAG, "generate_default_layout failed: %s",
					 esp_err_to_name(err2));
		}
		/* Also regenerate the RPM meter test layout to ensure it's up to date
		 */
		generate_rpm_meter_test_layout();
		layout_manager_set_active("default");
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

		int16_t idx = signal_register(
			name_item->valuestring, (uint32_t)can_id_item->valueint,
			(uint8_t)start_item->valueint, (uint8_t)len_item->valueint, scale,
			offset, is_signed, endian);

		if (idx >= 0) {
			ESP_LOGD(TAG, "Registered signal '%s' → index %d",
					 name_item->valuestring, (int)idx);
		} else {
			ESP_LOGW(TAG, "Failed to register signal '%s'",
					 name_item->valuestring);
		}
	}

	ESP_LOGI(TAG, "_load_signals: registered %u signals",
			 (unsigned)signal_get_count());
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
		cJSON_AddItemToArray(arr, sj);
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_load
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_load(const char *name, lv_obj_t *parent) {
	if (!name || !parent)
		return ESP_ERR_INVALID_ARG;

	char path[80];
	_make_path(name, path, sizeof(path));

	FILE *f = fopen(path, "r");
	if (!f) {
		ESP_LOGE(TAG, "layout_load: cannot open %s", path);
		return ESP_ERR_NOT_FOUND;
	}

	/* Read entire file into a heap buffer */
	char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
	if (!buf) {
		fclose(f);
		return ESP_ERR_NO_MEM;
	}

	size_t nread = fread(buf, 1, LAYOUT_MAX_FILE_BYTES - 1, f);
	fclose(f);
	buf[nread] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);

	if (!root) {
		ESP_LOGE(TAG, "layout_load: JSON parse failed for %s", path);
		return ESP_FAIL;
	}

	/* ── Load signals BEFORE widgets so from_json can resolve names ── */
	signal_registry_reset();
	_load_signals(root);

	const cJSON *widgets_arr =
		cJSON_GetObjectItemCaseSensitive(root, "widgets");
	if (!cJSON_IsArray(widgets_arr)) {
		ESP_LOGE(TAG, "layout_load: no 'widgets' array in %s", path);
		cJSON_Delete(root);
		return ESP_FAIL;
	}

	const cJSON *wj = NULL;
	cJSON_ArrayForEach(wj, widgets_arr) {
		const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(wj, "type");
		widget_type_t wtype = _type_from_str(
			cJSON_IsString(type_item) ? type_item->valuestring : NULL);
		if (wtype == WIDGET_TYPE_COUNT) {
			ESP_LOGW(TAG, "layout_load: unknown widget type '%s', skipping",
					 cJSON_IsString(type_item) ? type_item->valuestring
											   : "(null)");
			continue;
		}

		widget_t *w = _factory(wtype, (cJSON *)wj);
		if (!w) {
			ESP_LOGW(TAG, "layout_load: factory returned NULL for type %d",
					 (int)wtype);
			continue;
		}

		/* Restore base fields (x, y, w, h, id) and type-specific config.
		 * Cast away const: cJSON iterator yields const ptr but our vtable
		 * takes mutable (cJSON API doesn't propagate const internally). */
		ESP_LOGD(TAG, "layout_load: Calling from_json for %s", w->id);
		w->from_json(w, (cJSON *)wj);

		/* Build LVGL objects on the parent screen */
		ESP_LOGD(TAG, "layout_load: Calling create for %s", w->id);
		w->create(w, parent);
		ESP_LOGD(TAG, "layout_load: Returned from create for %s", w->id);

		/* Position root object if valid */
		if (w->root && lv_obj_is_valid(w->root)) {
			lv_obj_set_x(w->root, w->x);
			lv_obj_set_y(w->root, w->y);
		}

		ESP_LOGD(TAG, "layout_load: loaded widget id=%s type=%d at (%d,%d)",
				 w->id, (int)wtype, (int)w->x, (int)w->y);

		/* Note: the widget_t is orphaned here until a registry is added in
		 * Phase 4.  For now the LVGL objects are correctly positioned on
		 * screen without a centralised registry. */
	}

	cJSON_Delete(root);
	ESP_LOGI(TAG, "layout_load: loaded '%s' from %s", name, path);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_apply_json
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_apply_json(cJSON *root, lv_obj_t *parent) {
	if (!root || !parent)
		return ESP_ERR_INVALID_ARG;

	/* ── Load signals BEFORE widgets so from_json can resolve names ── */
	signal_registry_reset();
	_load_signals(root);

	const cJSON *widgets_arr =
		cJSON_GetObjectItemCaseSensitive(root, "widgets");
	if (!cJSON_IsArray(widgets_arr)) {
		ESP_LOGE(TAG, "apply_json: no 'widgets' array");
		return ESP_FAIL;
	}

	const cJSON *wj = NULL;
	cJSON_ArrayForEach(wj, widgets_arr) {
		const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(wj, "type");
		widget_type_t wtype = _type_from_str(
			cJSON_IsString(type_item) ? type_item->valuestring : NULL);
		if (wtype == WIDGET_TYPE_COUNT) {
			ESP_LOGW(TAG, "apply_json: unknown widget type '%s', skipping",
					 cJSON_IsString(type_item) ? type_item->valuestring
											   : "(null)");
			continue;
		}

		widget_t *w = _factory(wtype, (cJSON *)wj);
		if (!w) {
			ESP_LOGW(TAG, "apply_json: factory returned NULL for type %d",
					 (int)wtype);
			continue;
		}

		w->from_json(w, (cJSON *)wj);
		w->create(w, parent);

		if (w->root && lv_obj_is_valid(w->root)) {
			lv_obj_set_x(w->root, w->x);
			lv_obj_set_y(w->root, w->y);
		}

		ESP_LOGD(TAG, "apply_json: loaded widget id=%s type=%d at (%d,%d)",
				 w->id, (int)wtype, (int)w->x, (int)w->y);
	}

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

	/* Serialise registered signals */
	_save_signals(root);

	cJSON *arr = cJSON_AddArrayToObject(root, "widgets");
	for (uint8_t i = 0; i < count; i++) {
		widget_t *w = widgets[i];
		if (!w || !w->to_json)
			continue;
		cJSON *wj = cJSON_CreateObject();
		if (!wj) {
			continue;
		}
		w->to_json(w, wj);
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

	char *json_str = cJSON_PrintUnformatted(root);
	if (!json_str)
		return ESP_ERR_NO_MEM;

	char path[80];
	_make_path(name, path, sizeof(path));

	FILE *f = fopen(path, "w");
	if (!f) {
		ESP_LOGE(TAG, "layout_save_raw: cannot open %s for writing", path);
		free(json_str);
		return ESP_FAIL;
	}

	size_t len = strlen(json_str);
	size_t nw = fwrite(json_str, 1, len, f);
	fclose(f);
	free(json_str);

	if (nw != len) {
		ESP_LOGE(TAG, "layout_save_raw: short write (%u/%u bytes) for %s",
				 (unsigned)nw, (unsigned)len, path);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "layout_save_raw: saved '%s' to %s (%u bytes)", name, path,
			 (unsigned)len);
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

	char path[80];
	_make_path(name, path, sizeof(path));

	FILE *f = fopen(path, "r");
	if (!f) {
		ESP_LOGE(TAG, "layout_read_raw: cannot open %s", path);
		return ESP_ERR_NOT_FOUND;
	}

	size_t nread = fread(buf, 1, buf_size - 1, f);
	fclose(f);
	buf[nread] = '\0';

	if (out_len)
		*out_len = nread;

	ESP_LOGD(TAG, "layout_read_raw: read '%s' (%u bytes)", name, (unsigned)nread);
	return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  layout_manager_delete
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_delete(const char *name) {
	if (!name)
		return ESP_ERR_INVALID_ARG;
	char path[80];
	_make_path(name, path, sizeof(path));
	if (remove(path) != 0) {
		ESP_LOGW(TAG, "layout_delete: remove(%s) failed", path);
		return ESP_ERR_NOT_FOUND;
	}
	ESP_LOGI(TAG, "layout_delete: deleted '%s'", name);
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
