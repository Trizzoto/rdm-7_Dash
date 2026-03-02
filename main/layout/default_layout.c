#include "default_layout.h"
#include "cJSON.h"
#include "esp_log.h"
#include "layout_manager.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "default_layout";

/*
 * Hardcoded Screen3 positions, taken directly from the existing widget_*.c
 * files (lv_obj_set_x / lv_obj_set_y calls).
 *
 * Coordinate system: LV_ALIGN_CENTER origin (0,0 = screen centre).
 * Screen resolution: 800 × 480.
 *
 * Panel positions are taken from widget_panel.c:  box_positions[8].
 * RPM bar: fixed top-of-screen, no LVGL position (uses LV_ALIGN_TOP_MID).
 * Speed: lv_obj_set_x(0), lv_obj_set_y(-127) from widget_rpm_bar.c.
 * Gear: x=0, y=180 (widget_gear.c default).
 * BAR1: x=-240, y=209 / BAR2: x=240, y=209 (widget_bar.c defaults).
 * Indicators: Left x=-95 y=-133; Right x=95 y=-133 (widget_indicator.c).
 * Warnings: positions taken from widget_warning.c warning_positions[8].
 */

/* Panel grid positions (x, y relative to LV_ALIGN_CENTER) */
static const struct {
	int16_t x;
	int16_t y;
} s_panel_pos[8] = {
	{-285, 100}, /* Panel 0  – top-left  */
	{-95, 100},	 /* Panel 1  – top-mid-L */
	{95, 100},	 /* Panel 2  – top-mid-R */
	{285, 100},	 /* Panel 3  – top-right */
	{-285, 190}, /* Panel 4  – bot-left  */
	{-95, 190},	 /* Panel 5  – bot-mid-L */
	{95, 190},	 /* Panel 6  – bot-mid-R */
	{285, 190},	 /* Panel 7  – bot-right */
};

/* Warning circle positions (from widget_warning.c) */
static const struct {
	int16_t x;
	int16_t y;
} s_warn_pos[8] = {
	{-280, -127}, {-200, -127}, {-120, -127}, {-40, -127},
	{40, -127},	  {120, -127},	{200, -127},  {280, -127},
};

/* ── Helper: add one widget object to the widgets JSON array ─────────────── */

static void _add_widget(cJSON *arr, const char *type_str, const char *id,
						int16_t x, int16_t y, uint16_t w, uint16_t h,
						cJSON *config) {
	cJSON *wj = cJSON_CreateObject();
	cJSON_AddStringToObject(wj, "type", type_str);
	cJSON_AddStringToObject(wj, "id", id);
	cJSON_AddNumberToObject(wj, "x", x);
	cJSON_AddNumberToObject(wj, "y", y);
	cJSON_AddNumberToObject(wj, "w", w);
	cJSON_AddNumberToObject(wj, "h", h);

	if (config) {
		cJSON_AddItemToObject(wj, "config", config);
	} else {
		cJSON_AddObjectToObject(wj, "config");
	}

	cJSON_AddItemToArray(arr, wj);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  generate_default_layout
 * ════════════════════════════════════════════════════════════════════════════
 */
esp_err_t generate_default_layout(void) {
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return ESP_ERR_NO_MEM;

	cJSON_AddNumberToObject(root, "schema_version", 1);
	cJSON_AddStringToObject(root, "name", "default");

	cJSON *arr = cJSON_AddArrayToObject(root, "widgets");

	/* ── RPM Bar (singleton) ────────────────────────────────────────────── */
	_add_widget(arr, "rpm_bar", "rpm_bar_0", 0, 0, 800, 55, NULL);

	/* ── Speed (singleton) ──────────────────────────────────────────────── */
	_add_widget(arr, "speed", "speed_0", 0, -127, 150, 60, NULL);

	/* ── Gear (singleton) ───────────────────────────────────────────────── */
	_add_widget(arr, "gear", "gear_0", 0, 180, 90, 90, NULL);

	/* ── Panels (8 slots) ───────────────────────────────────────────────── */
	for (int i = 0; i < 8; i++) {
		char id[16];
		snprintf(id, sizeof(id), "panel_%d", i);

		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "slot", i);

		_add_widget(arr, "panel", id, s_panel_pos[i].x, s_panel_pos[i].y, 150,
					60, cfg);
	}

	/* ── Bars (2 slots) ─────────────────────────────────────────────────── */
	{
		cJSON *cfg0 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg0, "slot", 0);
		_add_widget(arr, "bar", "bar_0", -240, 209, 300, 30, cfg0);

		cJSON *cfg1 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg1, "slot", 1);
		_add_widget(arr, "bar", "bar_1", 240, 209, 300, 30, cfg1);
	}

	/* ── Indicators (2 slots) ───────────────────────────────────────────── */
	{
		cJSON *cfgL = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfgL, "slot", 0);
		_add_widget(arr, "indicator", "indicator_0", -95, -133, 50, 50, cfgL);

		cJSON *cfgR = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfgR, "slot", 1);
		_add_widget(arr, "indicator", "indicator_1", 95, -133, 50, 50, cfgR);
	}

	/* ── Warnings (8 slots) ─────────────────────────────────────────────── */
	for (int i = 0; i < 8; i++) {
		char id[16];
		snprintf(id, sizeof(id), "warning_%d", i);

		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "slot", i);

		_add_widget(arr, "warning", id, s_warn_pos[i].x, s_warn_pos[i].y, 15,
					15, cfg);
	}

	/* ── Serialise & write ──────────────────────────────────────────────── */
	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_str) {
		ESP_LOGE(TAG, "cJSON_PrintUnformatted returned NULL");
		return ESP_ERR_NO_MEM;
	}

	const char *path = LFS_LAYOUT_DIR "/default.json";
	FILE *f = fopen(path, "w");
	if (!f) {
		ESP_LOGE(TAG, "Cannot open %s for writing", path);
		free(json_str);
		return ESP_FAIL;
	}

	size_t len = strlen(json_str);
	size_t nw = fwrite(json_str, 1, len, f);
	fclose(f);
	free(json_str);

	if (nw != len) {
		ESP_LOGE(TAG, "Short write: %u/%u bytes to %s", (unsigned)nw,
				 (unsigned)len, path);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Default layout written to %s (%u bytes)", path,
			 (unsigned)len);
	return ESP_OK;
}
