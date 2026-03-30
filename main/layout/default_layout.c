#include "default_layout.h"
#include "cJSON.h"
#include "esp_log.h"
#include "layout_manager.h"
#include "screen_config.h"

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
	if (!wj) return;
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
	cJSON_AddNumberToObject(root, "screen_w", SCREEN_W);
	cJSON_AddNumberToObject(root, "screen_h", SCREEN_H);
	cJSON_AddStringToObject(root, "ecu", "MaxxECU");
	cJSON_AddStringToObject(root, "ecu_version", "1.3");

	cJSON *arr = cJSON_AddArrayToObject(root, "widgets");

	/* ── Divider line under RPM bar ─────────────────────────────────────── */
	{
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "bg_color", 10597);
		cJSON_AddNumberToObject(cfg, "bg_opa", 255);
		cJSON_AddNumberToObject(cfg, "border_color", 10597);
		cJSON_AddNumberToObject(cfg, "border_width", 0);
		cJSON_AddNumberToObject(cfg, "border_radius", 0);
		cJSON_AddNumberToObject(cfg, "shadow_width", 0);
		cJSON_AddNumberToObject(cfg, "shadow_color", 0);
		cJSON_AddNumberToObject(cfg, "shadow_opa", 128);
		cJSON_AddNumberToObject(cfg, "shadow_ofs_x", 0);
		cJSON_AddNumberToObject(cfg, "shadow_ofs_y", 0);
		_add_widget(arr, "shape_panel", "shape_panel_1", 0, -182, 800, 9, cfg);
	}

	/* ── RPM Bar ────────────────────────────────────────────────────────── */
	{
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "rpm_max", 7000);
		cJSON_AddStringToObject(cfg, "signal_name", "RPM");
		cJSON_AddNumberToObject(cfg, "redline", 6500);
		_add_widget(arr, "rpm_bar", "rpm_bar_0", 0, -215, 800, 55, cfg);
	}

	/* ── Panels (8 slots) ───────────────────────────────────────────────── */
	{
		static const char * const labels[8] = {
			"COOLANT PRESSURE", "FUEL PRESSURE", "COOLANT TEMP", "INTAKE AIR TEMP",
			"OIL TEMP", "MAP", "LAMBDA TARGET", "LAMBDA"
		};
		static const char * const signals[8] = {
			"COOLANT_PRESSURE", "FUEL_PRESSURE", "COOLANT_TEMP", "INTAKE_AIR_TEMP",
			"OIL_TEMP", "MAP", "LAMBDA_TARGET", "LAMBDA"
		};
		static const int decimals[8] = { 0, 0, 0, 0, 0, 0, 0, 2 };

		for (int i = 0; i < 8; i++) {
			char id[16];
			snprintf(id, sizeof(id), "panel_%d", i);
			cJSON *cfg = cJSON_CreateObject();
			cJSON_AddNumberToObject(cfg, "slot", i);
			cJSON_AddNumberToObject(cfg, "decimals", decimals[i]);
			cJSON_AddStringToObject(cfg, "label", labels[i]);
			cJSON_AddStringToObject(cfg, "signal_name", signals[i]);
			_add_widget(arr, "panel", id, s_panel_pos[i].x, s_panel_pos[i].y,
						155, 92, cfg);
		}
	}

	/* ── Bars (2 slots) ─────────────────────────────────────────────────── */
	{
		cJSON *cfg0 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg0, "slot", 0);
		cJSON_AddStringToObject(cfg0, "label", "COOLANT TEMP");
		cJSON_AddStringToObject(cfg0, "signal_name", "COOLANT_TEMP");
		cJSON_AddNumberToObject(cfg0, "bar_low", 0);
		cJSON_AddNumberToObject(cfg0, "bar_high", 100);
		cJSON_AddNumberToObject(cfg0, "bar_low_color", 31);
		cJSON_AddNumberToObject(cfg0, "bar_high_color", 63488);
		_add_widget(arr, "bar", "bar_0", -240, 209, 300, 30, cfg0);

		cJSON *cfg1 = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg1, "slot", 1);
		cJSON_AddStringToObject(cfg1, "label", "THROTTLE  ");
		cJSON_AddStringToObject(cfg1, "signal_name", "THROTTLE__");
		cJSON_AddNumberToObject(cfg1, "bar_low", 0);
		cJSON_AddNumberToObject(cfg1, "bar_high", 100);
		cJSON_AddNumberToObject(cfg1, "bar_low_color", 31);
		cJSON_AddNumberToObject(cfg1, "bar_high_color", 63488);
		_add_widget(arr, "bar", "bar_1", 240, 209, 300, 30, cfg1);
	}

	/* ── Indicators (2 slots) ───────────────────────────────────────────── */
	{
		cJSON *cfgL = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfgL, "slot", 0);
		cJSON_AddNumberToObject(cfgL, "opa_off", 180);
		_add_widget(arr, "indicator", "indicator_0", -95, -133, 35, 35, cfgL);

		cJSON *cfgR = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfgR, "slot", 1);
		cJSON_AddNumberToObject(cfgR, "opa_off", 180);
		_add_widget(arr, "indicator", "indicator_1", 95, -133, 35, 35, cfgR);
	}

	/* ── Warnings (8 slots) ─────────────────────────────────────────────── */
	for (int i = 0; i < 8; i++) {
		char id[16];
		snprintf(id, sizeof(id), "warning_%d", i);
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddNumberToObject(cfg, "slot", i);
		cJSON_AddNumberToObject(cfg, "inactive_opa", 180);
		if (i == 7) {
			cJSON_AddStringToObject(cfg, "label", "INTAKE AIR TEMP");
			cJSON_AddNumberToObject(cfg, "radius", 200);
		}
		_add_widget(arr, "warning", id, s_warn_pos[i].x, s_warn_pos[i].y,
					20, 20, cfg);
	}

	/* ── Vehicle speed (large center text) ──────────────────────────────── */
	{
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddStringToObject(cfg, "static_text", "");
		cJSON_AddNumberToObject(cfg, "decimals", 0);
		cJSON_AddNumberToObject(cfg, "rotation", 0);
		cJSON_AddStringToObject(cfg, "font", "fugaz_56");
		cJSON_AddNumberToObject(cfg, "text_color", 65535);
		cJSON_AddStringToObject(cfg, "signal_name", "VEHICLE_SPEED");
		_add_widget(arr, "text", "text_1", 0, 70, 180, 80, cfg);
	}

	/* ── RDM logo image ─────────────────────────────────────────────────── */
	{
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddStringToObject(cfg, "image_name", "RDM");
		cJSON_AddNumberToObject(cfg, "image_scale", 256);
		cJSON_AddNumberToObject(cfg, "opacity", 255);
		_add_widget(arr, "image", "image_1", 0, -60, 120, 62, cfg);
	}

	/* ── RPM text ───────────────────────────────────────────────────────── */
	{
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddStringToObject(cfg, "static_text", "");
		cJSON_AddNumberToObject(cfg, "decimals", 0);
		cJSON_AddNumberToObject(cfg, "rotation", 0);
		cJSON_AddStringToObject(cfg, "font", "fugaz_28");
		cJSON_AddNumberToObject(cfg, "text_color", 65535);
		cJSON_AddStringToObject(cfg, "signal_name", "RPM");
		_add_widget(arr, "text", "text_2", 0, -111, 180, 80, cfg);
	}

	/* ── FPS counter (debug) ────────────────────────────────────────────── */
	{
		cJSON *cfg = cJSON_CreateObject();
		cJSON_AddStringToObject(cfg, "static_text", "");
		cJSON_AddNumberToObject(cfg, "decimals", 0);
		cJSON_AddNumberToObject(cfg, "rotation", 0);
		cJSON_AddStringToObject(cfg, "font", "");
		cJSON_AddNumberToObject(cfg, "text_color", 65535);
		cJSON_AddStringToObject(cfg, "signal_name", "FPS");
		_add_widget(arr, "text", "text_3", -380, -160, 100, 30, cfg);
	}

	/* ── Signals ────────────────────────────────────────────────────────── */
	cJSON *sigs = cJSON_AddArrayToObject(root, "signals");
	static const struct { const char *name; int can_id; int bit_start; int bit_length;
		     double scale; double offset; int is_signed; int endian; } sig_defs[] = {
		{ "VEHICLE_SPEED", 1314, 48, 16, 0.1,   0, 0, 1 },
		{ "RPM",           1312,  0, 16, 1.0,   0, 1, 1 },
		{ "COOLANT_TEMP",  1328, 48, 16, 0.1,   0, 0, 1 },
		{ "FPS",              0,  0, 16, 1.0,   0, 0, 1 },
		{ "THROTTLE__",    1312, 16, 16, 0.1,   0, 0, 1 },
		{ "COOLANT_PRESSURE",1335,32, 16, 0.1,  0, 0, 1 },
		{ "FUEL_PRESSURE", 1335,  0, 16, 0.1,   0, 0, 1 },
		{ "INTAKE_AIR_TEMP",1328,32, 16, 0.1,   0, 0, 1 },
		{ "LAMBDA_TARGET", 1319, 48, 16, 0.001, 0, 0, 1 },
		{ "LAMBDA_A",      1313,  0, 16, 0.001, 0, 0, 1 },
		{ "LAMBDA",        1312, 48, 16, 0.001, 0, 0, 1 },
		{ "MAP",           1312, 32, 16, 0.1,   0, 0, 1 },
		{ "OIL_TEMP",      1334, 48, 16, 0.1,   0, 0, 1 },
	};
	for (int i = 0; i < (int)(sizeof(sig_defs)/sizeof(sig_defs[0])); i++) {
		cJSON *s = cJSON_CreateObject();
		cJSON_AddStringToObject(s, "name", sig_defs[i].name);
		cJSON_AddNumberToObject(s, "can_id", sig_defs[i].can_id);
		cJSON_AddNumberToObject(s, "bit_start", sig_defs[i].bit_start);
		cJSON_AddNumberToObject(s, "bit_length", sig_defs[i].bit_length);
		cJSON_AddNumberToObject(s, "scale", sig_defs[i].scale);
		cJSON_AddNumberToObject(s, "offset", sig_defs[i].offset);
		cJSON_AddBoolToObject(s, "is_signed", sig_defs[i].is_signed);
		cJSON_AddStringToObject(s, "unit", "");
		cJSON_AddNumberToObject(s, "endian", sig_defs[i].endian);
		cJSON_AddItemToArray(sigs, s);
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
	cJSON_AddNumberToObject(root, "screen_w", SCREEN_W);
	cJSON_AddNumberToObject(root, "screen_h", SCREEN_H);

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
