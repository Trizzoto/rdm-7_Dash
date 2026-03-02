#include "layout_manager.h"
#include "default_layout.h"

/* Widget factory headers */
#include "widget_bar.h"
#include "widget_gear.h"
#include "widget_indicator.h"
#include "widget_panel.h"
#include "widget_rpm_bar.h"
#include "widget_speed.h"
#include "widget_types.h"
#include "widget_warning.h"

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

/* Maximum JSON file size we'll read into heap */
#define LAYOUT_MAX_FILE_BYTES 16384

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
	if (strcmp(s, "speed") == 0)
		return WIDGET_SPEED;
	if (strcmp(s, "gear") == 0)
		return WIDGET_GEAR;
	if (strcmp(s, "bar") == 0)
		return WIDGET_BAR;
	if (strcmp(s, "indicator") == 0)
		return WIDGET_INDICATOR;
	if (strcmp(s, "warning") == 0)
		return WIDGET_WARNING;
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

	switch (type) {
	case WIDGET_PANEL:
		return widget_panel_create_instance(slot);
	case WIDGET_RPM_BAR:
		return widget_rpm_bar_create_instance();
	case WIDGET_SPEED:
		return widget_speed_create_instance();
	case WIDGET_GEAR:
		return widget_gear_create_instance();
	case WIDGET_BAR:
		return widget_bar_create_instance(slot);
	case WIDGET_INDICATOR:
		return widget_indicator_create_instance(slot);
	case WIDGET_WARNING:
		return widget_warning_create_instance(slot);
	default:
		return NULL;
	}
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
	 * the version is older than DEFAULT_LAYOUT_SCHEMA_VERSION so that a
	 * firmware update automatically refreshes stale position data.      */
#define DEFAULT_LAYOUT_SCHEMA_VERSION 2
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
		if (ver < DEFAULT_LAYOUT_SCHEMA_VERSION) {
			need_regen = true;
			ESP_LOGI(TAG, "default.json schema v%d < v%d — regenerating", ver,
					 DEFAULT_LAYOUT_SCHEMA_VERSION);
		}
	}

	if (need_regen) {
		esp_err_t err2 = generate_default_layout();
		if (err2 != ESP_OK) {
			ESP_LOGW(TAG, "generate_default_layout failed: %s",
					 esp_err_to_name(err2));
		}
		layout_manager_set_active("default");
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
		w->from_json(w, (cJSON *)wj);

		/* Build LVGL objects on the parent screen */
		w->create(w, parent);

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
 *  layout_manager_save
 * ═══════════════════════════════════════════════════════════════════════════
 */
esp_err_t layout_manager_save(const char *name, widget_t **widgets,
							  uint8_t count) {
	if (!name || !widgets)
		return ESP_ERR_INVALID_ARG;

	cJSON *root = cJSON_CreateObject();
	if (!root)
		return ESP_ERR_NO_MEM;

	cJSON_AddNumberToObject(root, "schema_version", 1);
	cJSON_AddStringToObject(root, "name", name);

	cJSON *arr = cJSON_AddArrayToObject(root, "widgets");
	for (uint8_t i = 0; i < count; i++) {
		widget_t *w = widgets[i];
		if (!w || !w->to_json)
			continue;
		cJSON *wj = cJSON_CreateObject();
		w->to_json(w, wj);
		cJSON_AddItemToArray(arr, wj);
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_str)
		return ESP_ERR_NO_MEM;

	char path[80];
	_make_path(name, path, sizeof(path));

	FILE *f = fopen(path, "w");
	if (!f) {
		ESP_LOGE(TAG, "layout_save: cannot open %s for writing", path);
		free(json_str);
		return ESP_FAIL;
	}

	size_t len = strlen(json_str);
	size_t nw = fwrite(json_str, 1, len, f);
	fclose(f);
	free(json_str);

	if (nw != len) {
		ESP_LOGE(TAG, "layout_save: short write (%u/%u bytes) for %s",
				 (unsigned)nw, (unsigned)len, path);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "layout_save: saved '%s' to %s (%u bytes)", name, path,
			 (unsigned)len);
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
