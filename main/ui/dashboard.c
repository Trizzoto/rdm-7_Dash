#include "ui/dashboard.h"
#include "layout/layout_manager.h"
#include "widgets/signal.h"
#include "widgets/widget_registry.h"

/* Existing widget create functions — used as fallback */
#include "widgets/widget_bar.h"
#include "widgets/widget_indicator.h"
#include "widgets/widget_panel.h"
#include "widgets/widget_rpm_bar.h"
#include "widgets/widget_warning.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "dashboard";

/* ── Internal widget registry snapshot ───────────────────────────────────── */

/* Maximum widgets the dashboard tracks (5 types × worst-case instances):
 *   panel×8, rpm_bar×1, bar×2, indicator×2, warning×8, text×N, meter×N */
#define DASHBOARD_MAX_WIDGETS 24

static widget_t *s_widgets[DASHBOARD_MAX_WIDGETS];
static uint8_t s_widget_count = 0;

/* ── Signal timeout timer ─────────────────────────────────────────────────── */

static lv_timer_t *s_signal_timeout_timer = NULL;

static void _signal_timeout_cb(lv_timer_t *timer) {
	(void)timer;
	uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
	signal_check_timeouts(now_ms);
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

widget_t **dashboard_get_widgets(void) { return s_widgets; }
uint8_t dashboard_get_widget_count(void) { return s_widget_count; }

/* ── Fallback: directly call the existing widget create functions ─────────
 *
 * Used when layout_manager_load() fails (e.g. corrupted file, FS error).
 * This guarantees the screen always renders correctly.
 * ────────────────────────────────────────────────────────────────────────── */
static void _fallback_create_all(lv_obj_t *parent) {
	ESP_LOGW(TAG, "Using fallback direct widget creation");
	widget_panel_create(parent);
	widget_rpm_bar_create(parent);
	widget_bar_create(parent);
	widget_indicator_create(parent);
	widget_warning_create(parent);
	/* s_widget_count stays 0 — no widget_t handles in this path */
}

/* ════════════════════════════════════════════════════════════════════════════
 *  dashboard_init
 * ════════════════════════════════════════════════════════════════════════════
 */
void dashboard_init(lv_obj_t *parent) {
	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	signal_registry_init();
	widget_registry_reset();
	widget_warning_reset();
	widget_indicator_reset();

	/* Create (or keep) signal timeout timer — checks every 500 ms */
	if (!s_signal_timeout_timer) {
		s_signal_timeout_timer =
			lv_timer_create(_signal_timeout_cb, 500, NULL);
	}

	esp_err_t err = layout_manager_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "layout_manager_init failed (%s) — using fallback",
				 esp_err_to_name(err));
		_fallback_create_all(parent);
		return;
	}

	char layout_name[LAYOUT_MAX_NAME];
	layout_manager_get_active(layout_name, sizeof(layout_name));
	ESP_LOGI(TAG, "Loading layout '%s'", layout_name);

	err = layout_manager_load(layout_name, parent);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "layout_manager_load('%s') failed (%s) — using fallback",
				 layout_name, esp_err_to_name(err));
		_fallback_create_all(parent);
		return;
	}

	widget_registry_snapshot(s_widgets, DASHBOARD_MAX_WIDGETS, &s_widget_count);
}

void dashboard_apply_layout_json(lv_obj_t *parent, cJSON *root) {
	if (!root || !parent)
		return;

	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));
	widget_registry_reset();
	widget_warning_reset();
	widget_indicator_reset();

	/* Ensure layout manager is init (FS mounted etc) before applying */
	layout_manager_init();

	esp_err_t err = layout_manager_apply_json(root, parent);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "dashboard_apply_layout_json failed (%s)",
				 esp_err_to_name(err));
		_fallback_create_all(parent);
	} else {
		widget_registry_snapshot(s_widgets, DASHBOARD_MAX_WIDGETS,
								 &s_widget_count);
	}
}

esp_err_t dashboard_persist_layout(void) {
	char layout_name[LAYOUT_MAX_NAME];
	esp_err_t err = layout_manager_get_active(layout_name, sizeof(layout_name));
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "dashboard_persist_layout: get_active failed (%s)",
				 esp_err_to_name(err));
		return err;
	}

	err = layout_manager_save(layout_name, s_widgets, s_widget_count);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "dashboard_persist_layout: save '%s' failed (%s)",
				 layout_name, esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, "Layout '%s' persisted (%d widgets)", layout_name,
				 s_widget_count);
	}
	return err;
}
