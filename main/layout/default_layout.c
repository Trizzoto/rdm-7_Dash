#include "default_layout.h"
#include "cJSON.h"
#include "esp_log.h"
#include "layout_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "default_layout";

/*
 * Exact hardcoded positions taken DIRECTLY from the widget_*.c create
 * functions.  All coordinates are relative to LV_ALIGN_CENTER
 * (screen centre).  Screen resolution: 800 × 480.
 *
 * widget_panel.c   box_positions[8][2]
 * widget_bar.c     ui_Bar_1 at (-240, 209);  ui_Bar_2 at (240, 209)
 * widget_indicator.c  Left (-95,-133); Right (95,-133)
 * widget_warning.c warning_positions[] below
 */

/* Panel box positions — must match box_positions[8][2] in widget_panel.c */
static const struct {
	int16_t x;
	int16_t y;
} s_panel_pos[8] = {
	{-312, -26}, /* Panel 0 */
	{-146, -26}, /* Panel 1 */
	{-312, 82},	 /* Panel 2 */
	{-146, 82},	 /* Panel 3 */
	{146, -26},	 /* Panel 4 */
	{312, -26},	 /* Panel 5 */
	{146, 82},	 /* Panel 6 */
	{312, 82},	 /* Panel 7 */
};

/* Warning circle positions — must match warning_positions[] in widget_warning.c
 */
static const struct {
	int16_t x;
	int16_t y;
} s_warn_pos[8] = {
	{-352, -148}, /* Warning 0 */
	{-292, -148}, /* Warning 1 */
	{-232, -148}, /* Warning 2 */
	{-172, -148}, /* Warning 3 */
	{172, -148},  /* Warning 4 */
	{232, -148},  /* Warning 5 */
	{292, -148},  /* Warning 6 */
	{352, -148},  /* Warning 7 */
};

/* ── Helper: add one widget object to the widgets JSON array ────────────────
 */

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

	cJSON_AddNumberToObject(root, "schema_version",
							LAYOUT_SCHEMA_VERSION);
	cJSON_AddStringToObject(root, "name", "default");

	cJSON *arr = cJSON_AddArrayToObject(root, "widgets");

	/* ── RPM Bar (singleton) ────────────────────────────────────────────── */
	/* Center-origin: y=-213 places the 55px bar at the top edge */
	_add_widget(arr, "rpm_bar", "rpm_bar_0", 0, -213, 800, 55, NULL);

	/* ── Panels (8 slots) ───────────────────────────────────────────────── */
	for (int i = 0; i < 8; i++) {
		char id[16];
		snprintf(id, sizeof(id), "panel_%d", i);

		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "slot", i);

		_add_widget(arr, "panel", id, s_panel_pos[i].x, s_panel_pos[i].y, 155,
					92, cfg);
	}

	/* ── Bars (2 slots) ─────────────────────────────────────────────────── */
	{
		cJSON *cfg0 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg0, "slot", 0);
		/* widget_bar_create: ui_Bar_1 at (-240, 209) */
		_add_widget(arr, "bar", "bar_0", -240, 209, 300, 30, cfg0);

		cJSON *cfg1 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg1, "slot", 1);
		/* widget_bar_create: ui_Bar_2 at (240, 209) */
		_add_widget(arr, "bar", "bar_1", 240, 209, 300, 30, cfg1);
	}

	/* ── Indicators (2 slots) ───────────────────────────────────────────── */
	{
		cJSON *cfgL = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfgL, "slot", 0);
		/* widget_indicator_create: Left at (-95, -133) */
		_add_widget(arr, "indicator", "indicator_0", -95, -133, 50, 50, cfgL);

		cJSON *cfgR = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfgR, "slot", 1);
		/* widget_indicator_create: Right at (95, -133) */
		_add_widget(arr, "indicator", "indicator_1", 95, -133, 50, 50, cfgR);
	}

	/* ── Warnings (8 slots) ─────────────────────────────────────────────── */
	for (int i = 0; i < 8; i++) {
		char id[16];
		snprintf(id, sizeof(id), "warning_%d", i);

		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "slot", i);

		_add_widget(arr, "warning", id, s_warn_pos[i].x, s_warn_pos[i].y, 25,
					25, cfg);
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

	ESP_LOGI(TAG, "Default layout v%d written to %s (%u bytes)",
			 LAYOUT_SCHEMA_VERSION, path, (unsigned)len);
	return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  generate_rpm_meter_test_layout
 * ════════════════════════════════════════════════════════════════════════════
 */
esp_err_t generate_rpm_meter_test_layout(void) {
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return ESP_ERR_NO_MEM;

	cJSON_AddNumberToObject(root, "schema_version",
							LAYOUT_SCHEMA_VERSION);
	cJSON_AddStringToObject(root, "name", "rpm_meter_test");

	cJSON *arr = cJSON_AddArrayToObject(root, "widgets");

	/* ── Panels (8 slots) ───────────────────────────────────────────────── */
	static const struct {
		int16_t x;
		int16_t y;
	} p_pos[8] = {
		{-312, 200}, {-146, 200}, {-312, 308}, {-146, 308},
		{146, 200},	 {312, 200},  {146, 308},  {312, 308},
	};
	for (int i = 0; i < 8; i++) {
		char id[16];
		snprintf(id, sizeof(id), "panel_%d", i);
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "slot", i);
		_add_widget(arr, "panel", id, p_pos[i].x, p_pos[i].y, 155, 92, cfg);
	}

	/* ── Bars (2 slots) ─────────────────────────────────────────────────── */
	{
		cJSON *cfg0 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg0, "slot", 0);
		_add_widget(arr, "bar", "bar_0", -240, 420, 300, 30, cfg0);

		cJSON *cfg1 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg1, "slot", 1);
		_add_widget(arr, "bar", "bar_1", 240, 420, 300, 30, cfg1);
	}

	/* ── Meter singleton (Analog RPM) ───────────────────────────────────── */
	{
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "slot", 8);
		cJSON_AddNumberToObject(cfg, "min", 0);
		cJSON_AddNumberToObject(cfg, "max", 8000);
		cJSON_AddNumberToObject(cfg, "start_angle", 135);
		cJSON_AddNumberToObject(cfg, "end_angle", 45);
		_add_widget(arr, "meter", "meter_rpm_0", 0, 80, 300, 300, cfg);
	}

	/* ── Serialise & write ──────────────────────────────────────────────── */
	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_str)
		return ESP_ERR_NO_MEM;

	const char *path = LFS_LAYOUT_DIR "/rpm_meter_test.json";
	FILE *f = fopen(path, "w");
	if (!f) {
		free(json_str);
		return ESP_FAIL;
	}

	size_t len = strlen(json_str);
	size_t nw = fwrite(json_str, 1, len, f);
	fclose(f);
	free(json_str);

	if (nw != len)
		return ESP_FAIL;

	ESP_LOGI(TAG, "RPM Meter Test layout v%d written to %s (%u bytes)",
			 LAYOUT_SCHEMA_VERSION, path, (unsigned)len);
	return ESP_OK;
}
